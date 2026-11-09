#include "AccountService.h"

#include "Cafe/Account/Account.h"
#include "config/ActiveSettings.h"
#include "util/helpers/ZipArchive.h"
#include <Windows.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <pugixml.hpp>
#include <charconv>
#include <fstream>
#include <optional>
#include <set>
#include <atomic>
#include <curl/curl.h>
#include <openssl/x509.h>
#include "Cemu/napi/napi.h"
#include "Cemu/ncrypto/ncrypto.h"
#include "Cafe/IOSU/legacy/iosu_crypto.h"

namespace
{
	constexpr size_t s_accountLimit = 64 * 1024, s_fileLimit = 4 * 1024 * 1024, s_totalLimit = 64 * 1024 * 1024;
	using Writer = rapidjson::Writer<rapidjson::StringBuffer>;
	void RequireAccount(bool condition, const char* reason = "invalid-account")
	{
		if (!condition) throw std::runtime_error(reason);
	}
	void CheckPath(fs::path path)
	{
		RequireAccount(path.is_absolute(), "unsafe-path");
		for (; !path.empty(); path = path.parent_path())
		{
			const auto attributes = GetFileAttributesW(path.c_str());
			if (attributes == INVALID_FILE_ATTRIBUTES)
				RequireAccount(GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND, "unsafe-path");
			else RequireAccount(!(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "unsafe-path");
			if (path == path.root_path()) break;
		}
	}
	std::string Read(const fs::path& path, size_t limit = s_fileLimit)
	{
		CheckPath(path);
		RequireAccount(fs::is_regular_file(path) && fs::file_size(path) <= limit, "invalid-file");
		std::ifstream stream(path, std::ios::binary);
		RequireAccount(stream.good(), "read-failed");
		std::string result;
		char buffer[4096];
		while (stream.read(buffer, sizeof(buffer)) || stream.gcount())
		{
			RequireAccount(result.size() + static_cast<size_t>(stream.gcount()) <= limit, "invalid-file");
			result.append(buffer, static_cast<size_t>(stream.gcount()));
		}
		RequireAccount(!stream.bad(), "read-failed");
		return result;
	}
	void WriteNew(const fs::path& path, std::string_view bytes)
	{
		CheckPath(path);
		const HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		RequireAccount(handle != INVALID_HANDLE_VALUE, "write-failed");
		DWORD written = 0;
		const bool ok = WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size() && FlushFileBuffers(handle);
		const bool closed = CloseHandle(handle) != FALSE;
		if (!ok || !closed) { DeleteFileW(path.c_str()); RequireAccount(false, "write-failed"); }
	}
	std::wstring Wide(std::string_view value)
	{
		RequireAccount(value.find('\0') == std::string_view::npos && value.size() <= 4096, "invalid-text");
		if (value.empty()) return {};
		const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
		RequireAccount(length > 0, "invalid-text");
		std::wstring result(length, L'\0');
		RequireAccount(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length) == length, "invalid-text");
		return result;
	}
	std::wstring MiiName(std::string_view value)
	{
		auto name = Wide(value);
		RequireAccount(!name.empty() && name.size() <= 10 && name.front() != L' ' && name.back() != L' ', "invalid-name");
		for (size_t i = 0; i < name.size(); ++i)
		{
			const auto c = name[i];
			RequireAccount(c >= 0x20 && c != 0x7f && c != 0xfffe && c != 0xffff, "invalid-name");
			if (c >= 0xd800 && c <= 0xdbff)
				RequireAccount(++i < name.size() && name[i] >= 0xdc00 && name[i] <= 0xdfff, "invalid-name");
			else RequireAccount(c < 0xdc00 || c > 0xdfff, "invalid-name");
		}
		return name;
	}
	uint32 Hex(std::string_view value)
	{
		uint32 id = 0;
		const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), id, 16);
		RequireAccount(error == std::errc{} && end == value.data() + value.size(), "invalid-id");
		return id;
	}
	std::map<std::string, std::string> Fields(std::string_view bytes)
	{
		RequireAccount(!bytes.empty() && bytes.size() <= s_accountLimit && bytes.find('\0') == std::string_view::npos, "invalid-account");
		std::map<std::string, std::string> fields;
		size_t start = 0;
		bool header = false;
		while (start < bytes.size())
		{
			const auto end = bytes.find('\n', start);
			auto line = bytes.substr(start, end == std::string_view::npos ? bytes.size() - start : end - start);
			if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
			if (!header) { RequireAccount(line == "AccountInstance_20120705", "invalid-account"); header = true; }
			else if (!line.empty())
			{
				const auto split = line.find('=');
				RequireAccount(split != std::string_view::npos && split > 0 && split <= 80 && line.size() <= 4096, "invalid-account");
				const auto key = line.substr(0, split), value = line.substr(split + 1);
				RequireAccount(std::all_of(key.begin(), key.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; }), "invalid-account");
				RequireAccount(std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 32 && c != 127; }), "invalid-account");
				RequireAccount(fields.emplace(key, value).second, "duplicate-field");
			}
			if (end == std::string_view::npos) break;
			start = end + 1;
		}
		const std::map<std::string, size_t> required{{"PersistentId", 8}, {"TransferableIdBase", 0}, {"Uuid", 32}, {"MiiData", 192}, {"MiiName", 44}};
		for (const auto& [key, length] : required)
		{
			const auto it = fields.find(key);
			RequireAccount(it != fields.end() && !it->second.empty() && (length ? it->second.size() == length : it->second.size() <= 16), "incomplete-account");
		}
		const std::set<std::string> numeric{"PersistentId", "TransferableIdBase", "Uuid", "MiiData", "MiiName", "BirthYear", "BirthMonth", "BirthDay", "Gender", "Country", "SimpleAddressId", "TimeZone", "UtcOffset", "PrincipalId", "IsPasswordCacheEnabled", "AccountPasswordCache"};
		for (const auto& [key, value] : fields)
			if (numeric.contains(key)) RequireAccount(!value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isxdigit(c); }), "invalid-account");
		if (fields.contains("AccountPasswordCache")) RequireAccount(fields.at("AccountPasswordCache").size() == 64, "invalid-account");
		for (const auto& key : {"BirthYear", "BirthMonth", "BirthDay", "Gender", "Country", "SimpleAddressId", "PrincipalId", "IsPasswordCacheEnabled"})
			if (fields.contains(key)) { const auto value = Hex(fields.at(key)); RequireAccount(value <= (std::string_view(key) == "BirthYear" ? 65535u : std::string_view(key) == "BirthMonth" || std::string_view(key) == "BirthDay" || std::string_view(key) == "Gender" || std::string_view(key) == "IsPasswordCacheEnabled" ? 255u : UINT32_MAX)); }
		return fields;
	}
	Account ParseAccount(std::string_view bytes, uint32 expected)
	{
		const auto fields = Fields(bytes);
		RequireAccount(Hex(fields.at("PersistentId")) == expected && expected >= Account::kMinPersistendId, "invalid-id");
		RequireAccount(fields.at("MiiName").substr(40) == "0000", "invalid-name");
		bool ended = false;
		for (size_t i = 0; i < 11; ++i)
		{
			const auto c = Hex(std::string_view(fields.at("MiiName")).substr(i * 4, 4));
			if (c == 0) { ended = true; continue; }
			RequireAccount(!ended && c >= 32 && c != 127 && c != 0xfffe && c != 0xffff, "invalid-name");
			if (c >= 0xd800 && c <= 0xdbff)
			{
				RequireAccount(++i < 10, "invalid-name");
				const auto low = Hex(std::string_view(fields.at("MiiName")).substr(i * 4, 4));
				RequireAccount(low >= 0xdc00 && low <= 0xdfff, "invalid-name");
			}
			else RequireAccount(c < 0xdc00 || c > 0xdfff, "invalid-name");
		}
		// ACT が固定長バッファへコピーする文字列は、取込み前に終端分も含めて制限する。
		for (const auto& [key, maximum] : std::map<std::string, size_t>{{"AccountId", 63}, {"TimeZoneId", 64}, {"EmailAddress", 254}})
			if (fields.contains(key)) RequireAccount(fields.at(key).size() <= maximum, "invalid-account");
		const auto account = Account::FromStorage(bytes);
		MiiName(boost::nowide::narrow(account.GetMiiName()));
		Wide(account.GetAccountId());
		Wide(account.GetEmail());
		return account;
	}
	std::string String(const rapidjson::Value& value)
	{
		RequireAccount(value.IsString(), "invalid-request");
		std::string result(value.GetString(), value.GetStringLength());
		Wide(result);
		return result;
	}
	std::string LowerPath(std::string_view name)
	{
		const auto wide = Wide(name);
		std::wstring lowered(wide.size(), L'\0');
		RequireAccount(LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, wide.data(), static_cast<int>(wide.size()), lowered.data(), static_cast<int>(lowered.size()), nullptr, nullptr, 0) > 0, "unsafe-zip-path");
		return boost::nowide::narrow(lowered);
	}
	std::string ZipPath(std::string_view name)
	{
		RequireAccount(!name.empty() && name.size() < 1024 && name.front() != '/' && name.find('\\') == std::string_view::npos && name.find(':') == std::string_view::npos, "unsafe-zip-path");
		Wide(name);
		if (name.back() == '/') name.remove_suffix(1);
		size_t start = 0, depth = 0;
		while (start < name.size())
		{
			const auto end = name.find('/', start);
			const auto component = name.substr(start, end == std::string_view::npos ? name.size() - start : end - start);
			RequireAccount(++depth <= 32 && !component.empty() && component != "." && component != ".." && component.back() != '.' && component.back() != ' ', "unsafe-zip-path");
			RequireAccount(component.find_first_of("<>\"|?*") == std::string_view::npos && std::all_of(component.begin(), component.end(), [](unsigned char c) { return c >= 32 && c != 127; }), "unsafe-zip-path");
			auto base = LowerPath(component.substr(0, component.find('.')));
			RequireAccount(base != "con" && base != "prn" && base != "aux" && base != "nul" && !(base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '1' && base[3] <= '9'), "unsafe-zip-path");
			if (end == std::string_view::npos) break;
			start = end + 1;
		}
		RequireAccount(!name.empty(), "unsafe-zip-path");
		return LowerPath(name);
	}
	void Text(Writer& writer, const char* key, std::string_view value)
	{
		writer.Key(key); writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
	}
} // namespace

struct AccountService::Impl
{
	struct Candidate
	{
		uint32 id = 0, sourceId = 0, internalId = 0;
		std::string name, state = "invalid", reason;
		std::map<std::string, std::string> files;
	};
	struct Change
	{
		fs::path path;
		std::string bytes;
		std::optional<std::string> before;
	};
	fs::path portable = ActiveSettings::GetConfigPath();
	fs::path act = ActiveSettings::GetMlcPath("usr/save/system/act");
	fs::path settings = portable / "settings.xml";
	fs::path preferences = portable / "launcher-account-preferences.json";
	struct OnlineFile
	{
		std::string relative, bytes, state;
		std::optional<std::string> before;
	};
	std::vector<OnlineFile> onlineFiles;
	std::map<uint32, std::string> networks;
	std::atomic<int> connection{0}; // 未試験・試験中・ログイン成功・失敗
	std::vector<Candidate> candidates;
	uint64 revision = 1, preview = 0, sequence = 0;
	std::string notice = "ready", previewNotice;
	bool restartRequired = false, onlineComplete = false;
	bool healthy = true, hasOtp = false, hasSeeprom = false;

	Impl()
	{
		CheckPath(act); CheckPath(settings); CheckPath(preferences);
		if (fs::exists(portable / ".launcher-account-transaction")) { healthy = false; notice = "recovery-required"; return; }
		bool migrate = false;
		if (fs::exists(preferences))
		{
			try
			{
				const auto bytes = Read(preferences, 4096); rapidjson::Document json;
				json.Parse<rapidjson::kParseValidateEncodingFlag>(bytes.data(), bytes.size());
				RequireAccount(!json.HasParseError() && json.IsObject() && json.MemberCount() == 2 && json.HasMember("version") && json["version"].IsUint());
				if (json["version"].GetUint() == 1)
				{
					RequireAccount(json.HasMember("network")); const auto choice = String(json["network"]); Service(choice);
					networks[ActiveSettings::GetPersistentId()] = choice; migrate = true;
				}
				else
				{
					RequireAccount(json["version"].GetUint() == 2 && json.HasMember("accounts") && json["accounts"].IsObject() && json["accounts"].MemberCount() <= 12);
					for (auto it = json["accounts"].MemberBegin(); it != json["accounts"].MemberEnd(); ++it)
					{
						const auto key = String(it->name), value = String(it->value); Service(value);
						RequireAccount(key.size() == 8 && Hex(key) >= Account::kMinPersistendId && networks.emplace(Hex(key), value).second);
					}
				}
			}
			catch (...) { networks.clear(); notice = "preferences-invalid"; }
		}
		if (migrate) Commit({{preferences, Preferences(networks)}});
		ApplyNetwork();
	}

	static NetworkService Service(std::string_view value)
	{
		if (value == "Offline") return NetworkService::Offline;
		if (value == "Pretendo") return NetworkService::Pretendo;
		RequireAccount(value == "Custom", "invalid-network"); return NetworkService::Custom;
	}
	std::string Network() const
	{
		const auto it = networks.find(ActiveSettings::GetPersistentId()); return it == networks.end() ? "Offline" : it->second;
	}
	std::string Preferences(const std::map<uint32, std::string>& values) const
	{
		rapidjson::StringBuffer b; Writer w(b); w.StartObject(); w.Key("version"); w.Uint(2); w.Key("accounts"); w.StartObject();
		for (const auto& [id, value] : values) Text(w, fmt::format("{:08x}", id).c_str(), value);
		w.EndObject(); w.EndObject(); return b.GetString();
	}
	void ValidateCustom(std::string_view bytes) const
	{
		RequireAccount(!bytes.empty() && bytes.size() <= 64 * 1024 && bytes.find('\0') == std::string_view::npos, "custom-invalid");
		pugi::xml_document doc; RequireAccount(doc.load_buffer(bytes.data(), bytes.size(), pugi::parse_full), "custom-invalid");
		for (auto node : doc.children()) RequireAccount(node.type() == pugi::node_declaration || node.type() == pugi::node_element, "custom-invalid");
		const auto root = doc.child("content"); RequireAccount(root && !root.next_sibling("content"), "custom-invalid");
		std::set<std::string> seen;
		for (auto node : root.children()) RequireAccount(seen.insert(node.name()).second && (std::string_view(node.name()) == "networkname" || std::string_view(node.name()) == "disablesslverification" || std::string_view(node.name()) == "urls"), "custom-invalid");
		RequireAccount(seen.size() == 3 && std::string_view(root.child_value("disablesslverification")) == "0" && !std::string_view(root.child_value("networkname")).empty(), "custom-invalid");
		seen.clear(); const std::set<std::string> names{"act","ecs","nus","ias","ccsu","ccs","idbe","boss","tagaya","olv"};
		for (auto node : root.child("urls").children())
		{
			RequireAccount(names.contains(node.name()) && seen.insert(node.name()).second && !node.first_child().next_sibling(), "custom-invalid");
			const std::string url = node.child_value(); RequireAccount(url.size() <= 2048 && url.starts_with("https://") && std::all_of(url.begin(), url.end(), [](unsigned char c) { return c > 32 && c < 127; }), "custom-invalid");
			auto parsed = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>(curl_url(), curl_url_cleanup);
			RequireAccount(parsed && curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(), CURLU_NON_SUPPORT_SCHEME) == CURLUE_OK, "custom-invalid");
			for (auto part : {CURLUPART_USER, CURLUPART_PASSWORD, CURLUPART_QUERY, CURLUPART_FRAGMENT}) { char* out = nullptr; const auto result = curl_url_get(parsed.get(), part, &out, 0); curl_free(out); RequireAccount(result != CURLUE_OK, "custom-invalid"); }
		}
		RequireAccount(seen == names, "custom-invalid");
	}
	std::string Readiness() const
	{
		if (restartRequired) return "restart-required";
		try
		{
			const auto& account = Find(ActiveSettings::GetPersistentId());
			if (!account.IsValidOnlineAccount()) return "account-credentials-missing";
			if (!account.ValidateOnlineFiles().IsValid()) return "online-files-missing";
			if (!ActiveSettings::HasRequiredOnlineFiles() || !NCrypto::OTP_IsPresent() || !NCrypto::SEEPROM_IsPresent() || !iosuCrypto_hasValidCertificates()) return "crypto-not-ready";
			if (Network() == "Custom") { const auto path = portable / "network_services.xml"; if (!fs::exists(path)) return "custom-missing"; ValidateCustom(Read(path, 64 * 1024)); }
			return "ready";
		}
		catch (...) { return "online-files-invalid"; }
	}
	void ApplyNetwork()
	{
		GetNetworkConfig().requireVerifiedTLS = true;
		AccountService::EnforceOffline();
		if (Network() != "Offline" && Readiness() == "ready") GetConfig().SetAccountSelectedService(ActiveSettings::GetPersistentId(), Service(Network()));
	}
	std::set<uint32> Occupied() const
	{
		std::set<uint32> used;
		for (const auto& a : Account::GetAccounts()) used.insert(a.GetPersistentId());
		if (fs::exists(act)) for (const auto& e : fs::directory_iterator(act)) { CheckPath(e.path()); const auto name = e.path().filename().string(); if (name.size() == 8) { try { used.insert(Hex(name)); } catch (...) {} } }
		const auto saves = ActiveSettings::GetMlcPath("usr/save"); CheckPath(saves);
		size_t count = 0;
		if (fs::exists(saves)) for (const auto& high : fs::directory_iterator(saves))
		{
			CheckPath(high.path()); if (!high.is_directory() || high.path().filename().wstring().size() != 8) continue;
			for (const auto& low : fs::directory_iterator(high.path()))
			{
				CheckPath(low.path()); if (!low.is_directory() || low.path().filename().wstring().size() != 8) continue;
				const auto users = low.path() / "user"; CheckPath(users); if (!fs::exists(users)) continue;
				for (const auto& entry : fs::directory_iterator(users)) { RequireAccount(++count <= 10000, "save-scan-limit"); CheckPath(entry.path()); const auto name = entry.path().filename().string(); if (name.size() == 8) { try { used.insert(Hex(name)); } catch (...) {} } }
			}
		}
		return used;
	}
	static uint32 Allocate(std::set<uint32>& used)
	{
		uint32 id = Account::kMinPersistendId; while (used.contains(id)) { RequireAccount(id != UINT32_MAX, "id-unavailable"); ++id; }
		used.insert(id); return id;
	}
	static std::string Remap(std::string_view bytes, uint32 id)
	{
		const auto pos = bytes.find("\nPersistentId="); RequireAccount(pos != std::string_view::npos, "invalid-id");
		const auto start = pos + std::string_view("\nPersistentId=").size(); const auto end = bytes.find_first_of("\r\n", start);
		return std::string(bytes.substr(0, start)) + fmt::format("{:08x}", id) + (end == std::string_view::npos ? std::string{} : std::string(bytes.substr(end)));
	}

	fs::path File(uint32 id) const
	{
		RequireAccount(id >= Account::kMinPersistendId, "invalid-id");
		const auto result = act / fmt::format("{:08x}", id) / "account.dat";
		CheckPath(result);
		return result;
	}
	const Account& Find(uint32 id) const
	{
		const auto& list = Account::GetAccounts();
		const auto it = std::find_if(list.begin(), list.end(), [id](const auto& a) { return a.GetPersistentId() == id; });
		RequireAccount(it != list.end(), "account-not-found");
		return *it;
	}
	std::string Selection(uint32 id, const std::map<uint32, std::string>* services = nullptr)
	{
		pugi::xml_document xml;
		RequireAccount(xml.load_string(Read(settings).c_str()), "settings-invalid");
		auto config = xml.child("content");
		RequireAccount(config, "settings-invalid");
		auto account = config.child("Account");
		if (!account) account = config.append_child("Account");
		auto node = account.child("PersistentId");
		if (!node) node = account.append_child("PersistentId");
		node.text().set(id);
		if (services)
		{
			auto entries = config.child("AccountService"); if (!entries) entries = config.append_child("AccountService"); entries.remove_children();
			for (const auto& [pid, value] : *services) { auto e = entries.append_child("SelectedService"); e.append_attribute("PersistentId").set_value(pid); e.append_attribute("Service").set_value(static_cast<int>(Service(value))); }
		}
		std::ostringstream out; xml.save(out);
		return out.str();
	}
	std::string Snapshot(bool canEdit, bool details = false)
	{
		rapidjson::StringBuffer buffer;
		Writer writer(buffer);
		writer.StartObject();
		writer.Key("revision"); writer.Uint64(revision);
		Text(writer, "notice", notice);
		writer.Key("canEdit"); writer.Bool(canEdit && healthy);
		writer.Key("activeId"); writer.Uint(ActiveSettings::GetPersistentId());
		Text(writer, "network", Network());
		const auto effective = ActiveSettings::GetNetworkService();
		Text(writer, "effectiveNetwork", effective == NetworkService::Pretendo ? "Pretendo" : effective == NetworkService::Custom ? "Custom" : "Offline");
		Text(writer, "onlineStatus", Readiness());
		Text(writer, "connection", connection == 1 ? "testing" : connection == 2 ? "login-success" : connection == 3 ? "failed" : "not-tested");
		writer.Key("restartRequired"); writer.Bool(restartRequired);
		writer.Key("accounts"); writer.StartArray();
		for (const auto& account : Account::GetAccounts())
		{
			writer.StartObject();
			writer.Key("id"); writer.Uint(account.GetPersistentId());
			Text(writer, "name", boost::nowide::narrow(account.GetMiiName()));
			writer.EndObject();
		}
		writer.EndArray();
		if (details)
		{
			const auto& account = Find(ActiveSettings::GetPersistentId());
			writer.Key("details"); writer.StartObject();
			Text(writer, "accountId", account.GetAccountId());
			Text(writer, "birthday", account.GetBirthYear() ? fmt::format("{:04}-{:02}-{:02}", account.GetBirthYear(), account.GetBirthMonth(), account.GetBirthDay()) : "");
			writer.Key("gender"); writer.Uint(account.GetGender());
			writer.Key("country"); writer.Uint(account.GetCountry());
			Text(writer, "email", account.GetEmail());
			const auto online = account.ValidateOnlineFiles();
			Text(writer, "onlineRequirements", online.IsValid() ? "available" : "missing");
			writer.EndObject();
		}
		writer.Key("previewId"); writer.Uint64(preview);
		Text(writer, "previewNotice", previewNotice);
		writer.Key("onlineComplete"); writer.Bool(onlineComplete);
		writer.Key("onlineFiles"); writer.StartArray();
		for (const auto& file : onlineFiles) { writer.StartObject(); Text(writer, "path", file.relative); Text(writer, "state", file.state); writer.EndObject(); }
		writer.EndArray();
		writer.Key("hasOtp"); writer.Bool(hasOtp);
		writer.Key("hasSeeprom"); writer.Bool(hasSeeprom);
		writer.Key("candidates"); writer.StartArray();
		for (const auto& candidate : candidates)
		{
			writer.StartObject();
			writer.Key("id"); writer.Uint(candidate.id);
			writer.Key("sourceId"); writer.Uint(candidate.sourceId);
			writer.Key("internalId"); writer.Uint(candidate.internalId);
			Text(writer, "name", candidate.name);
			Text(writer, "state", candidate.state);
			Text(writer, "reason", candidate.reason);
			writer.EndObject();
		}
		writer.EndArray();
		writer.EndObject();
		return buffer.GetString();
	}

	// 各ファイルの atomic replace と、全変更前 bytes による復旧を組み合わせる。
	void Commit(std::vector<Change> changes, std::optional<uint32> archive = std::nullopt, bool refresh = false)
	{
		RequireAccount(healthy, "recovery-required");
		CheckPath(act); CheckPath(portable);
		if (refresh)
		{
			uint32 head = Account::kMinPersistendId;
			const auto manager = act / "persisid.dat";
			if (fs::exists(manager))
			{
				std::istringstream lines(Read(manager, s_accountLimit));
				std::string line;
				while (std::getline(lines, line))
				{
					if (!line.empty() && line.back() == '\r') line.pop_back();
					if (line.starts_with("PersistentIdHead=")) head = std::max(head, Hex(std::string_view(line).substr(17)));
				}
			}
			for (const auto& account : Account::GetAccounts()) head = std::max(head, account.GetPersistentId());
			for (const auto& change : changes)
				if (change.path.filename() == "account.dat") head = std::max(head, Hex(change.path.parent_path().filename().string()));
			changes.push_back({act / "persisid.dat", fmt::format("PersistentIdManager_20120607\r\nPersistentIdHead={:x}\r\n\r\n", head)});
		}
		for (auto& change : changes)
		{
			CheckPath(change.path);
			if (fs::exists(change.path)) change.before = Read(change.path);
		}
		const auto backups = portable / ".launcher-account-backups";
		CheckPath(backups); fs::create_directories(backups);
		const auto folder = backups / fmt::format("{}-{}-{}", GetCurrentProcessId(), GetTickCount64(), ++sequence);
		RequireAccount(fs::create_directory(folder), "backup-failed");
		const auto marker = portable / ".launcher-account-transaction";
		std::vector<fs::path> createdDirectories;
		size_t applied = 0;
		bool moved = false, marked = false;
		const auto archived = folder / "archived-account";
		try
		{
			rapidjson::StringBuffer manifestBuffer;
			Writer manifest(manifestBuffer); manifest.StartArray();
			for (size_t i = 0; i < changes.size(); ++i)
			{
				manifest.StartObject(); Text(manifest, "target", _pathToUtf8(changes[i].path.lexically_relative(portable)));
				manifest.Key("existed"); manifest.Bool(changes[i].before.has_value()); manifest.EndObject();
				if (changes[i].before) WriteNew(folder / fmt::format("before-{}", i), *changes[i].before);
				WriteNew(folder / fmt::format("after-{}", i), changes[i].bytes);
			}
			manifest.EndArray(); WriteNew(folder / "manifest.json", manifestBuffer.GetString());
			WriteNew(marker, _pathToUtf8(folder.filename())); marked = true;
			if (archive)
			{
				const auto directory = File(*archive).parent_path();
				for (const auto& entry : fs::recursive_directory_iterator(directory)) { CheckPath(entry.path()); RequireAccount(fs::is_regular_file(entry) || fs::is_directory(entry), "unsafe-path"); }
				RequireAccount(MoveFileExW(directory.c_str(), archived.c_str(), MOVEFILE_WRITE_THROUGH), "archive-failed");
				moved = true;
			}
			for (size_t i = 0; i < changes.size(); ++i)
			{
				auto& change = changes[i];
				CheckPath(change.path);
				RequireAccount(change.before ? fs::exists(change.path) && Read(change.path) == *change.before : !fs::exists(change.path), "concurrent-change");
				std::vector<fs::path> missing;
				for (auto parent = change.path.parent_path(); !fs::exists(parent); parent = parent.parent_path()) { RequireAccount(parent != parent.root_path(), "unsafe-path"); CheckPath(parent); missing.push_back(parent); }
				for (auto it = missing.rbegin(); it != missing.rend(); ++it) { RequireAccount(fs::create_directory(*it), "write-failed"); createdDirectories.push_back(*it); }
				const auto staged = folder / fmt::format("after-{}", i);
				RequireAccount(MoveFileExW(staged.c_str(), change.path.c_str(), (change.before ? MOVEFILE_REPLACE_EXISTING : 0) | MOVEFILE_WRITE_THROUGH), "save-failed");
				++applied;
				RequireAccount(Read(change.path) == change.bytes, "verify-failed");
				if (change.path.filename() == "account.dat") ParseAccount(change.bytes, Hex(change.path.parent_path().filename().string()));
			}
			if (refresh) RequireAccount(!Account::RefreshAccounts(false).empty(), "verify-failed");
			RequireAccount(DeleteFileW(marker.c_str()), "save-failed"); marked = false;
			++revision; notice = "saved";
		}
		catch (...)
		{
			bool restored = true;
			while (applied)
			{
				const size_t i = --applied;
				try
				{
					auto& change = changes[i];
					CheckPath(change.path);
					RequireAccount(Read(change.path) == change.bytes, "concurrent-change");
					if (change.before)
					{
						const auto rollback = folder / fmt::format("rollback-{}", i);
						WriteNew(rollback, *change.before);
						RequireAccount(MoveFileExW(rollback.c_str(), change.path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) && Read(change.path) == *change.before, "rollback-failed");
					}
					else RequireAccount(DeleteFileW(change.path.c_str()), "rollback-failed");
				}
				catch (...) { restored = false; }
			}
			for (auto it = createdDirectories.rbegin(); it != createdDirectories.rend(); ++it) { std::error_code error; fs::remove(*it, error); if (error) restored = false; }
			if (moved && !MoveFileExW(archived.c_str(), File(*archive).parent_path().c_str(), MOVEFILE_WRITE_THROUGH)) restored = false;
			if (restored && marked && !DeleteFileW(marker.c_str())) restored = false;
			if (refresh && restored) { try { RequireAccount(!Account::RefreshAccounts(false).empty()); } catch (...) { restored = false; } }
			healthy = restored;
			notice = restored ? "save-failed" : "recovery-required";
			throw;
		}
	}

	void ClearPreview()
	{
		candidates.clear(); onlineFiles.clear(); onlineComplete = false; hasOtp = hasSeeprom = false; previewNotice.clear(); ++preview;
	}
	void Preview(const fs::path& selected)
	{
		ClearPreview();
		CheckPath(selected);
		RequireAccount(LowerPath(_pathToUtf8(selected.extension())) == ".zip" && fs::is_regular_file(selected) && fs::file_size(selected) <= s_totalLimit, "invalid-zip");
		ZipArchive zip(selected, true);
		RequireAccount(zip.IsValid() && zip.GetNumEntries() > 0 && zip.GetNumEntries() <= 4096, "invalid-zip");
		std::set<std::string> paths;
		std::map<std::string, Candidate> found;
		std::map<std::string, std::string> online;
		std::set<std::string> certificates;
		for (const auto* name : iosuCrypt_getCertificateNames()) certificates.insert(LowerPath("sys/title/0005001b/10054000/content/" + boost::nowide::narrow(name)));
		for (const auto* name : iosuCrypt_getCertificateKeys()) certificates.insert(LowerPath("sys/title/0005001b/10054000/content/" + boost::nowide::narrow(name)));
		size_t total = 0;
		for (size_t i = 0; i < zip.GetNumEntries(); ++i)
		{
			ZipArchive::EntryInfo info;
			RequireAccount(zip.GetEntryInfo(i, info), "invalid-zip");
			const auto path = ZipPath(info.path);
			RequireAccount(paths.insert(path).second, "duplicate-zip-path");
			const bool directory = info.path.back() == '/';
			const uint32 type = (info.attributes >> 16) & 0170000;
			RequireAccount((type == 0 || type == (directory ? 0040000u : 0100000u)) && !(info.attributes & FILE_ATTRIBUTE_REPARSE_POINT) && info.encryption == 0, "unsafe-zip-entry");
			RequireAccount(info.size <= s_fileLimit && info.size <= s_totalLimit - total && (info.size == 0 || (info.compressedSize > 0 && info.size / info.compressedSize <= 1000)), "zip-size-limit");
			total += static_cast<size_t>(info.size);
			if (directory) { RequireAccount(info.size == 0, "invalid-zip"); continue; }
			std::vector<uint8> data;
			RequireAccount(zip.ExtractIntoMemoryBounded(i, data, s_fileLimit), "zip-integrity-failed");
			const auto slash = path.find_last_of('/');
			const auto filename = path.substr(slash == std::string::npos ? 0 : slash + 1);
			hasOtp |= filename == "otp.bin"; hasSeeprom |= filename == "seeprom.bin";
			const auto match = ("/" + path).find("/usr/save/system/act/");
			if (match == std::string::npos)
			{
				std::string relative;
				if (filename == "otp.bin" || filename == "seeprom.bin") relative = filename;
				else if (const auto mlc = ("/" + path).find("/mlc01/"); mlc != std::string::npos)
				{
					const auto sub = ("/" + path).substr(mlc + 7);
					const std::set<std::string> additional{"sys/title/0005001b/10054000/content/ccerts/wiiu_wagonu_crypto_symkey.aes","sys/title/0005001b/10054000/content/ccerts/wiiu_wagonu_hmac_key.aes","sys/title/0005001b/10056000/content/fflreshigh.dat","sys/title/0005001b/10056000/content/fflreshighlg.dat","sys/title/0005001b/10056000/content/fflresmiddle.dat","sys/title/0005001b/10056000/content/fflresmiddlelg.dat","usr/save/system/nim/ec/ec_account_info.exi"};
					if (certificates.contains(sub) || additional.contains(sub)) relative = "mlc01/" + sub;
				}
				if (!relative.empty()) RequireAccount(online.emplace(relative, std::string(reinterpret_cast<const char*>(data.data()), data.size())).second, "duplicate-zip-path");
				continue;
			}
			const auto accountPath = ("/" + path).substr(match + std::string_view("/usr/save/system/act/").size());
			if (accountPath == "persisid.dat") continue;
			RequireAccount(accountPath.size() > 9 && accountPath[8] == '/' && accountPath.find('/', 9) == std::string::npos, "unexpected-account-file");
			const uint32 id = Hex(std::string_view(accountPath).substr(0, 8));
			RequireAccount(id >= Account::kMinPersistendId, "invalid-id");
			RequireAccount(filename == "account.dat" || (filename.size() == 12 && filename.starts_with("miiimg0") && filename[7] >= '0' && filename[7] <= '8' && filename.substr(8) == ".dat"), "unexpected-account-file");
			auto& candidate = found[path.substr(0, path.find_last_of('/'))]; candidate.sourceId = id;
			RequireAccount(candidate.files.emplace(filename, std::string(reinterpret_cast<const char*>(data.data()), data.size())).second, "duplicate-account");
		}
		RequireAccount(!found.empty() && found.size() <= 12, "no-accounts");
		auto used = Occupied();
		for (auto& [key, candidate] : found)
		{
			try
			{
				RequireAccount(candidate.files.contains("account.dat"), "incomplete-account");
				const auto& bytes = candidate.files.at("account.dat");
				candidate.internalId = Hex(Fields(bytes).at("PersistentId"));
				const auto account = ParseAccount(bytes, candidate.internalId);
				candidate.name = boost::nowide::narrow(account.GetMiiName());
				candidate.id = Allocate(used);
				candidate.files["account.dat"] = Remap(bytes, candidate.id);
				ParseAccount(candidate.files["account.dat"], candidate.id);
				candidate.state = "new";
			}
			catch (const std::exception& error)
			{
				candidate.state = "invalid";
				candidate.reason = std::string_view(error.what()) == "persistent-id-mismatch" ? "persistent-id-mismatch" : "incomplete-or-invalid-account";
				candidate.files.clear();
			}
			candidates.push_back(std::move(candidate));
		}
		for (auto& [relative, bytes] : online)
		{
			OnlineFile file{relative, std::move(bytes), "new", std::nullopt};
			bool valid = !file.bytes.empty();
			if (relative == "otp.bin") valid = file.bytes.size() == 1024;
			else if (relative == "seeprom.bin") valid = file.bytes.size() == 512;
			else if (relative.ends_with(".der")) { const auto* ptr = reinterpret_cast<const unsigned char*>(file.bytes.data()); auto* cert = d2i_X509(nullptr, &ptr, static_cast<long>(file.bytes.size())); valid = cert && ptr == reinterpret_cast<const unsigned char*>(file.bytes.data() + file.bytes.size()); X509_free(cert); }
			else if (relative.ends_with(".aes")) valid = file.bytes.size() % 16 == 0;
			const auto target = portable / fs::u8path(relative); CheckPath(target);
			if (fs::exists(target)) { file.before = Read(target); file.state = *file.before == file.bytes ? "identical" : "different"; }
			if (!valid) file.state = "invalid";
			onlineFiles.push_back(std::move(file));
		}
		onlineComplete = online.contains("otp.bin") && online.contains("seeprom.bin") && std::all_of(onlineFiles.begin(), onlineFiles.end(), [](const auto& f) { return f.state != "invalid"; });
		for (const auto& cert : certificates) onlineComplete &= online.contains("mlc01/" + cert);
		previewNotice = "preview-ready";
	}
};

AccountService::AccountService() : m_impl(std::make_unique<Impl>()) {}
AccountService::~AccountService() = default;
bool AccountService::IsHealthy() const { return m_impl->healthy; }
void AccountService::EnforceOffline()
{
	GetNetworkConfig().requireVerifiedTLS = true;
	GetConfig().account.legacy_online_enabled = false;
	for (auto& [id, service] : GetConfig().account.service_select) service = NetworkService::Offline;
	GetConfig().SetAccountSelectedService(ActiveSettings::GetPersistentId(), NetworkService::Offline);
}

std::string AccountService::PreviewZip(const fs::path& selectedFile, bool canEdit)
{
	try { RequireAccount(canEdit && IsHealthy(), "not-ready"); m_impl->Preview(selectedFile); m_impl->notice = "preview-ready"; }
	catch (const std::exception& error)
	{
		const std::set<std::string> safe{"not-ready", "invalid-zip", "unsafe-zip-path", "duplicate-zip-path", "unsafe-zip-entry", "zip-size-limit", "zip-integrity-failed", "unexpected-account-file", "invalid-id", "duplicate-account", "no-accounts"};
		m_impl->candidates.clear(); m_impl->hasOtp = m_impl->hasSeeprom = false;
		m_impl->previewNotice = safe.contains(error.what()) ? error.what() : "invalid-zip"; m_impl->notice = "preview-failed";
	}
	return m_impl->Snapshot(canEdit);
}

uint64_t AccountService::Revision() const { return m_impl->revision; }

std::string AccountService::Command(std::string_view payload, bool canEdit)
{
	auto& state = *m_impl;
	bool details = false;
	try
	{
		RequireAccount(!payload.empty() && payload.size() <= 4096 && payload.find('\0') == std::string_view::npos, "invalid-request");
		rapidjson::Document json;
		json.Parse<rapidjson::kParseValidateEncodingFlag>(payload.data(), payload.size());
		RequireAccount(!json.HasParseError() && json.IsObject() && json.HasMember("op"), "invalid-request");
		std::set<std::string> members;
		for (auto it = json.MemberBegin(); it != json.MemberEnd(); ++it) RequireAccount(members.insert(String(it->name)).second, "invalid-request");
		const auto op = String(json["op"]);
		if (op == "list" || op == "details")
		{
			RequireAccount(json.MemberCount() == 1, "invalid-request");
			details = op == "details";
		}
		else
		{
			if (state.healthy) state.notice = "ready";
			RequireAccount(canEdit && state.healthy, "not-ready");
			RequireAccount(json.HasMember("revision") && json["revision"].IsUint64() && json["revision"].GetUint64() == state.revision, "stale-revision");
			CheckPath(state.act);
			const auto originalId = ActiveSettings::GetPersistentId();
			if (op == "select")
			{
				RequireAccount(json.MemberCount() == 3 && json.HasMember("id") && json["id"].IsUint(), "invalid-request");
				const uint32 id = json["id"].GetUint(); state.Find(id);
				ParseAccount(Read(state.File(id)), id);
				state.Commit({{state.settings, state.Selection(id)}});
				GetConfig().account.m_persistent_id = id; state.connection = 0;
			}
			else if (op == "clear-preview")
			{
				RequireAccount(json.MemberCount() == 2, "invalid-request");
				state.ClearPreview();
			}
			else if (op == "edit")
			{
				RequireAccount(json.MemberCount() == 5 && json.HasMember("id") && json["id"].IsUint() && json.HasMember("field") && json.HasMember("value"), "invalid-request");
				const uint32 id = json["id"].GetUint(); state.Find(id);
				const auto path = state.File(id);
				const auto original = Read(path);
				auto account = ParseAccount(original, id);
				const auto field = String(json["field"]), value = String(json["value"]);
				std::map<std::string, std::string> replacements;
				if (field == "name")
				{
					account.SetMiiName(MiiName(value));
					std::wstring name(account.GetMiiName()); name.resize(11, 0);
					for (auto c : name) replacements["MiiName"] += fmt::format("{:04x}", static_cast<uint16>(c));
				}
				else if (field == "birthday")
				{
					uint32 year = 0, month = 0, day = 0;
					if (!value.empty())
					{
						RequireAccount(value.size() == 10, "invalid-birthday");
						for (size_t i = 0; i < value.size(); ++i)
							RequireAccount(i == 4 || i == 7 ? value[i] == '-' : value[i] >= '0' && value[i] <= '9', "invalid-birthday");
						year = std::stoul(value.substr(0, 4)); month = std::stoul(value.substr(5, 2)); day = std::stoul(value.substr(8, 2));
						const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
						const uint32 days[]{31, leap ? 29u : 28u, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
						RequireAccount(year >= 1900 && year <= 9999 && month >= 1 && month <= 12 && day >= 1 && day <= days[month - 1], "invalid-birthday");
					}
					account.SetBirthYear(static_cast<uint16>(year)); account.SetBirthMonth(static_cast<uint8>(month)); account.SetBirthDay(static_cast<uint8>(day));
					replacements = {{"BirthYear", fmt::format("{:x}", year)}, {"BirthMonth", fmt::format("{:x}", month)}, {"BirthDay", fmt::format("{:x}", day)}};
				}
				else if (field == "gender" || field == "country")
				{
					uint32 number = 0;
					const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
					RequireAccount(error == std::errc{} && end == value.data() + value.size() && number <= (field == "gender" ? 1u : 255u), "invalid-value");
					if (field == "gender") account.SetGender(static_cast<uint8>(number)); else account.SetCountry(number);
					replacements[field == "gender" ? "Gender" : "Country"] = fmt::format("{:x}", number);
				}
				else if (field == "email")
				{
					RequireAccount(value.size() <= 254 && std::all_of(value.begin(), value.end(), [](unsigned char c) { return c > 32 && c < 127; }), "invalid-email");
					if (!value.empty()) { const auto at = value.find('@'); RequireAccount(at > 0 && at != std::string::npos && at + 1 < value.size() && value.find('@', at + 1) == std::string::npos, "invalid-email"); }
					account.SetEmail(value); replacements["EmailAddress"] = value;
				}
				else RequireAccount(false, "invalid-field");
				// 変更対象の行だけを置換し、MiiData・識別子・認証情報と未知フィールドを保持する。
				std::string bytes;
				std::istringstream lines(original);
				std::string line;
				const auto newline = original.find("\r\n") != std::string::npos ? "\r\n" : "\n";
				while (std::getline(lines, line))
				{
					if (!line.empty() && line.back() == '\r') line.pop_back();
					const auto split = line.find('=');
					const auto it = replacements.find(line.substr(0, split));
					if (it != replacements.end()) { line = it->first + "=" + it->second; replacements.erase(it); }
					bytes += line + newline;
				}
				for (const auto& [key, replacement] : replacements) bytes += key + "=" + replacement + newline;
				ParseAccount(bytes, id);
				state.Commit({{path, bytes}}, std::nullopt, true);
			}
			else if (op == "delete")
			{
				RequireAccount(json.MemberCount() == 3 && json.HasMember("id") && json["id"].IsUint(), "invalid-request");
				const uint32 id = json["id"].GetUint(); state.Find(id);
				const auto& list = Account::GetAccounts();
				RequireAccount(list.size() > 1, "last-account");
				ParseAccount(Read(state.File(id)), id);
				const auto nextId = originalId == id ? std::find_if(list.begin(), list.end(), [id](const auto& a) { return a.GetPersistentId() != id; })->GetPersistentId() : originalId;
				auto values = state.networks; values.erase(id);
				state.Commit({{state.settings, state.Selection(nextId, &values)}, {state.preferences, state.Preferences(values)}}, id, true);
				state.networks = std::move(values); state.connection = 0;
				GetConfig().account.m_persistent_id = nextId;
			}
			else if (op == "network")
			{
				RequireAccount(json.MemberCount() == 3 && json.HasMember("value"), "invalid-request");
				const auto value = String(json["value"]); Impl::Service(value);
				auto values = state.networks; values[originalId] = value;
				state.Commit({{state.preferences, state.Preferences(values)}, {state.settings, state.Selection(originalId, &values)}});
				state.networks = std::move(values); state.connection = 0;
			}
			else if (op == "test")
			{
				RequireAccount(json.MemberCount() == 2 && state.Network() != "Offline" && state.Readiness() == "ready", "network-not-ready");
				state.ApplyNetwork(); state.connection = 1;
			}
			else if (op == "import")
			{
				RequireAccount(json.MemberCount() == 6 && json.HasMember("online") && json["online"].IsBool() && json.HasMember("replaceOnline") && json["replaceOnline"].IsBool() && json.HasMember("preview") && json["preview"].IsUint64() && json["preview"].GetUint64() == state.preview && json.HasMember("ids") && json["ids"].IsArray(), "stale-preview");
				RequireAccount(json["ids"].Size() > 0 && Account::GetAccounts().size() + json["ids"].Size() <= 12, "account-limit");
				std::set<uint32> selected;
				const auto occupied = state.Occupied();
				std::vector<Impl::Change> changes;
				for (const auto& idValue : json["ids"].GetArray())
				{
					RequireAccount(idValue.IsUint() && selected.insert(idValue.GetUint()).second, "invalid-id");
					const uint32 id = idValue.GetUint();
					const auto it = std::find_if(state.candidates.begin(), state.candidates.end(), [id](const auto& c) { return c.id == id; });
					RequireAccount(it != state.candidates.end() && it->state == "new" && !occupied.contains(id) && !fs::exists(state.File(id).parent_path()), "persistent-id-exists");
					ParseAccount(it->files.at("account.dat"), id);
					for (const auto& [filename, bytes] : it->files) changes.push_back({state.File(id).parent_path() / filename, bytes});
				}
				bool onlineChanged = false;
				if (json["online"].GetBool())
				{
					RequireAccount(state.onlineComplete, "online-files-incomplete");
					for (const auto& file : state.onlineFiles)
					{
						const auto target = state.portable / fs::u8path(file.relative); CheckPath(target);
						RequireAccount(file.before ? fs::exists(target) && Read(target) == *file.before : !fs::exists(target), "stale-preview");
						RequireAccount(file.state != "different" || json["replaceOnline"].GetBool(), "replacement-confirmation-required");
						if (file.state != "identical") { changes.push_back({target, file.bytes}); onlineChanged = true; }
					}
				}
				state.Commit(std::move(changes), std::nullopt, true);
				state.restartRequired |= onlineChanged;
				state.candidates.clear(); ++state.preview; state.previewNotice = "imported";
			}
			else RequireAccount(false, "invalid-request");
			state.ApplyNetwork();
		}
	}
	catch (const std::exception& error)
	{
		// ファイルパスや account の値を例外から UI・ログへ流さない。
		const std::set<std::string> safe{"not-ready", "stale-revision", "stale-preview", "invalid-request", "invalid-id", "invalid-name", "invalid-birthday", "invalid-value", "invalid-email", "invalid-field", "invalid-network", "account-not-found", "account-limit", "last-account", "id-unavailable", "persistent-id-exists", "recovery-required", "online-files-incomplete", "replacement-confirmation-required", "network-not-ready", "custom-invalid"};
		if (state.notice != "recovery-required" && state.notice != "save-failed") state.notice = safe.contains(error.what()) ? error.what() : "operation-failed";
	}
	return state.Snapshot(canEdit, details);
}

bool AccountService::IsTesting() const { return m_impl->connection == 1; }
std::string AccountService::NetworkName() const { return m_impl->Network(); }
void AccountService::PrepareLaunch() { m_impl->ApplyNetwork(); }
void AccountService::RunConnectionTest()
{
	if (!IsTesting()) return;
	try
	{
		NAPI::AuthInfo auth;
		if (!NAPI::NAPI_MakeAuthInfoFromCurrentAccount(auth)) { m_impl->connection = 3; return; }
		auth.serviceOverwrite = ActiveSettings::GetNetworkService();
		const auto result = NAPI::ACT_TestAccountLogin(auth, 0x00050000101dbe00ULL, 560);
		m_impl->connection = result.isValid() ? 2 : 3;
	}
	catch (...) { m_impl->connection = 3; }
}
std::string AccountService::ImportCustom(const fs::path& selectedFile, bool canEdit, bool replaceExisting)
{
	try
	{
		RequireAccount(canEdit && IsHealthy(), "not-ready"); CheckPath(selectedFile);
		RequireAccount(LowerPath(_pathToUtf8(selectedFile.extension())) == ".xml", "custom-invalid");
		const auto bytes = Read(selectedFile, 64 * 1024); m_impl->ValidateCustom(bytes);
		const auto path = m_impl->portable / "network_services.xml";
		RequireAccount(!fs::exists(path) || Read(path) == bytes || replaceExisting, "replacement-confirmation-required");
		if (!fs::exists(path) || Read(path) != bytes) { m_impl->Commit({{path, bytes}}); m_impl->restartRequired = true; }
		m_impl->ApplyNetwork();
	}
	catch (...) { m_impl->notice = "custom-invalid"; }
	return m_impl->Snapshot(canEdit);
}
