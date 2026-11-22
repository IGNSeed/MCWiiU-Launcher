#include "../UpdateIdentity.h"
#include "../VersionedInstallation.h"
#include <glaze/glaze.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <zip.h>
#include <algorithm>
#include <iostream>
#include <memory>
#include <set>

using namespace LauncherUpdate;
namespace
{
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Bio = std::unique_ptr<BIO, decltype(&BIO_free)>;

std::string Hex(const unsigned char* bytes, size_t count)
{
	std::string result;
	for (size_t i = 0; i < count; ++i) { result += "0123456789abcdef"[bytes[i] >> 4]; result += "0123456789abcdef"[bytes[i] & 15]; }
	return result;
}
std::string Public(EVP_PKEY* key)
{
	std::array<unsigned char, 32> raw{};
	size_t size = raw.size();
	Require(EVP_PKEY_is_a(key, "ED25519") && EVP_PKEY_get_raw_public_key(key, raw.data(), &size) == 1 && size == raw.size(), "not-an-ed25519-key");
	return Hex(raw.data(), raw.size());
}
std::string ReadPublic(const fs::path& path)
{
	const auto key = ReadFileBytes(path, 64);
	Require(key.size() == 64 && key.find_first_not_of("0123456789abcdef") == key.npos, "public-key-must-be-64-lowercase-hex-bytes-no-newline");
	return key;
}
std::string Fingerprint(std::string_view key)
{
	std::string raw;
	for (size_t i = 0; i < key.size(); i += 2)
	{
		const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
		raw += static_cast<char>((digit(key[i]) << 4) | digit(key[i + 1]));
	}
	return Sha256(raw);
}
Key ReadPrivate(const fs::path& path)
{
	auto bytes = ReadFileBytes(path, 16384);
	Bio input(BIO_new_mem_buf(bytes.data(), static_cast<int>(bytes.size())), BIO_free);
	Key key(PEM_read_bio_PrivateKey(input.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
	OPENSSL_cleanse(bytes.data(), bytes.size());
	Require(key && EVP_PKEY_is_a(key.get(), "ED25519"), "private-key-unavailable-or-wrong-password");
	return key;
}
std::string Sign(EVP_PKEY* key, std::string_view bytes)
{
	std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
	std::array<unsigned char, 64> signature{};
	size_t size = signature.size();
	Require(context && EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key) == 1 &&
		EVP_DigestSign(context.get(), signature.data(), &size, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()) == 1 && size == signature.size(), "ed25519-signing-failed");
	return Hex(signature.data(), size);
}
void Generate(const fs::path& directory, bool test)
{
	Require(!fs::exists(directory), "key-directory-already-exists");
	DWORD mode = 0;
	Require(test || GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode), "production-key-generation-requires-interactive-console-and-encryption-passphrase");
	CreatePrivateDirectory(directory);
	Key key(EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519"), EVP_PKEY_free);
	Require(bool(key), "key-generation-failed");
	Bio output(BIO_new(BIO_s_mem()), BIO_free);
	// 本番鍵はOpenSSLの対話入力で暗号化する。パスフレーズを引数やログへ渡さない。
	Require(PEM_write_bio_PrivateKey(output.get(), key.get(), test ? nullptr : EVP_aes_256_cbc(), nullptr, 0, nullptr, nullptr) == 1, "private-key-encryption-failed");
	char* bytes = nullptr;
	const auto size = BIO_get_mem_data(output.get(), &bytes);
	Require(size > 0, "private-key-encoding-failed");
	WriteNew(directory / L"private.pem", {bytes, static_cast<size_t>(size)});
	OPENSSL_cleanse(bytes, size);
	const auto publicKey = Public(key.get());
	WriteNew(directory / L"public-key.txt", publicKey);
	WriteNew(directory / L"key-kind.txt", test ? "ISOLATED-TEST-ONLY" : "ENCRYPTED-PRODUCTION-KEY");
	std::cout << "Ed25519 public-key SHA256 " << Fingerprint(publicKey) << "\n";
}
void CheckFoundationFiles(const FoundationMetadata& foundation, const fs::path& binaries)
{
	for (const auto& entry : foundation.files)
	{
		auto file = OpenLocked(binaries / Wide(entry.path)); LARGE_INTEGER size{};
		Require(GetFileSizeEx(file.handle, &size) && size.QuadPart >= 0 && static_cast<uint64_t>(size.QuadPart) == entry.size && FileSha256(file.handle) == entry.sha256 && ExecutableVersion(binaries / Wide(entry.path)) == foundation.bootstrapper_version, "foundation-file-integrity-or-version-failed");
	}
}
void CheckZip(const fs::path& package, const Manifest& manifest)
{
	VerifyPackage(package, manifest);
	zip_error_t error; zip_error_init(&error);
	auto* source = zip_source_win32w_create(package.c_str(), 0, -1, &error);
	auto* raw = source ? zip_open_from_source(source, ZIP_RDONLY | ZIP_CHECKCONS, &error) : nullptr;
	if (!raw && source) zip_source_free(source);
	zip_error_fini(&error);
	Require(raw, "invalid-package-zip");
	std::unique_ptr<zip_t, decltype(&zip_discard)> archive(raw, zip_discard);
	Require(zip_get_num_entries(raw, ZIP_FL_UNCHANGED) == static_cast<zip_int64_t>(manifest.files.size()), "zip-inventory-count-mismatch");
	std::set<std::string> names;
	for (zip_uint64_t i = 0; i < manifest.files.size(); ++i)
	{
		zip_stat_t info{}; zip_stat_init(&info);
		Require(zip_stat_index(raw, i, ZIP_FL_ENC_STRICT, &info) == 0 && info.name, "invalid-zip-entry");
		const std::string name(info.name); ValidatePackageName(name);
		auto folded = name; std::transform(folded.begin(), folded.end(), folded.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
		Require(names.insert(folded).second, "duplicate-zip-entry");
		const auto entry = std::find_if(manifest.files.begin(), manifest.files.end(), [&](const auto& e) { return e.path == name; });
		Require(entry != manifest.files.end() && entry->size == info.size && info.encryption_method == ZIP_EM_NONE && (info.comp_method == ZIP_CM_STORE || info.comp_method == ZIP_CM_DEFLATE), "zip-inventory-mismatch");
		zip_uint8_t host = 0; zip_uint32_t attributes = 0;
		Require(zip_file_get_external_attributes(raw, i, 0, &host, &attributes) == 0 && (((attributes >> 16) & 0170000) == 0 || ((attributes >> 16) & 0170000) == 0100000) && !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE)), "linked-or-special-zip-entry");
		std::unique_ptr<zip_file_t, decltype(&zip_fclose)> input(zip_fopen_index(raw, i, 0), zip_fclose);
		Require(bool(input), "cannot-read-zip-entry");
		std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> hash(EVP_MD_CTX_new(), EVP_MD_CTX_free);
		Require(hash && EVP_DigestInit_ex(hash.get(), EVP_sha256(), nullptr) == 1, "hash-failed");
		std::array<char, 65536> buffer{}; uint64_t total = 0;
		for (;;) {
			const auto read = zip_fread(input.get(), buffer.data(), buffer.size());
			Require(read >= 0 && total + read <= entry->size, "invalid-zip-payload");
			if (!read) break;
			total += read; Require(EVP_DigestUpdate(hash.get(), buffer.data(), read) == 1, "hash-failed");
		}
		std::array<unsigned char, 32> digest{}; unsigned length = 0;
		Require(total == entry->size && EVP_DigestFinal_ex(hash.get(), digest.data(), &length) == 1 && length == digest.size() && Hex(digest.data(), length) == entry->sha256, "zip-file-hash-mismatch");
	}
}
}
int wmain(int count, wchar_t** arguments)
{
	try
	{
		Require(count >= 2, "missing-signing-operation");
		const std::wstring_view action(arguments[1]);
		if (action == L"identity" && count == 2)
		{
			std::cout << "{\"product_id\":\"" << ProductId << "\",\"product_name\":\"" << ProductName << "\",\"app_id\":\"" << AppId << "\",\"foundation_version\":\"" << BootstrapperVersion << "\",\"public_key\":\"" << TrustedPublicKey << "\"}\n";
			return 0;
		}
		if ((action == L"generate" || action == L"generate-test") && count == 3) Generate(arguments[2], action == L"generate-test");
		else if (action == L"public" && count == 4) { auto key = ReadPrivate(arguments[2]); WriteNew(arguments[3], Public(key.get())); }
		else if (action == L"fingerprint" && count == 3) std::cout << Fingerprint(ReadPublic(arguments[2])) << "\n";
		else if (action == L"sign" && count == 7)
		{
			const std::wstring_view kind(arguments[2]); const auto publicKey = ReadPublic(arguments[4]);
			auto key = ReadPrivate(arguments[3]); Require(Public(key.get()) == publicKey, "private-public-key-mismatch");
			const auto bytes = ReadFileBytes(arguments[5], 256 * 1024), signature = Sign(key.get(), bytes);
			Require(kind == L"manifest" || kind == L"foundation", "unsupported-signing-kind");
			if (kind == L"manifest") VerifyManifest(bytes, signature, publicKey, ProductId);
			else VerifyFoundationMetadata(bytes, signature, publicKey);
			const fs::path destination(arguments[6]); Require(!fs::exists(destination), "signature-destination-exists"); CreatePrivateDirectory(destination);
			WriteNew(destination / (kind == L"manifest" ? L"manifest.json" : L"foundation.json"), bytes);
			WriteNew(destination / (kind == L"manifest" ? L"manifest.sig" : L"foundation.sig"), signature);
			VerifySignature(bytes, signature, publicKey);
		}
		else if ((action == L"verify-manifest" || action == L"verify-foundation") && count == 5)
		{
			const auto key = ReadPublic(arguments[2]), bytes = ReadFileBytes(arguments[3], 256 * 1024), signature = ReadFileBytes(arguments[4], 128);
			if (action == L"verify-manifest") VerifyManifest(bytes, signature, key, ProductId); else VerifyFoundationMetadata(bytes, signature, key);
		}
		else if (action == L"verify-bundle" && count == 5)
		{
			const auto key = ReadPublic(arguments[2]); const fs::path bundle(arguments[3]), binaries(arguments[4]);
			const auto manifest = VerifyManifest(ReadFileBytes(bundle / L"manifest.json", 256 * 1024), ReadFileBytes(bundle / L"manifest.sig", 128), key, ProductId);
			const auto foundation = VerifyFoundationMetadata(ReadFileBytes(bundle / L"foundation.json", 256 * 1024), ReadFileBytes(bundle / L"foundation.sig", 128), key);
			Require(manifest.package_format == "zip" && Version::Parse(manifest.required_bootstrapper_version) <= Version::Parse(foundation.bootstrapper_version) && manifest.required_updater_protocol_version <= foundation.updater_protocol_version, "incompatible-initial-bundle");
			CheckFoundationFiles(foundation, binaries); CheckZip(bundle / Wide(manifest.package_filename), manifest);
			for (const auto& entry : manifest.files) {
				auto file = OpenLocked(bundle / L"payload" / Wide(entry.path)); LARGE_INTEGER size{};
				Require(GetFileSizeEx(file.handle, &size) && size.QuadPart >= 0 && static_cast<uint64_t>(size.QuadPart) == entry.size && FileSha256(file.handle) == entry.sha256, "payload-integrity-failed");
			}
			Require(ExecutableVersion(bundle / L"payload" / Wide(LauncherExecutableName(manifest))) == manifest.version, "payload-executable-version-mismatch");
		}
		else throw std::runtime_error("invalid-signing-operation-or-argument-count");
		std::cout << "PASS\n"; return 0;
	} catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << "\n"; return 1; }
}
