#include "FriendsService.h"
#include "Cemu/nex/FriendsSession.h"
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <rapidjson/document.h>
#include <charconv>
#include <set>

std::string FriendsSnapshotJson()
{
	const auto view = FriendsSession::Instance().Snapshot();
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	auto text = [&](const char* key, const std::string& value) {
		writer.Key(key);
		writer.String(value.data(), static_cast<rapidjson::SizeType>(value.size()));
	};
	writer.StartObject();
	writer.Key("generation");
	writer.Uint64(view.generation);
	writer.Key("revision");
	writer.Uint64(view.revision);
	writer.Key("connectionId");
	writer.Uint64(view.connectionId);
	writer.Key("activeId");
	writer.Uint(view.activeId);
	text("service", view.service);
	text("status", view.status);
	writer.Key("refreshing");
	writer.Bool(view.refreshing);
	writer.Key("preferencesUnset");
	writer.Bool(view.preferencesUnset);
	writer.Key("operation");
	writer.StartObject();
	writer.Key("id");
	writer.Uint64(view.operation.id);
	text("state", view.operation.state);
	text("notice", view.operation.notice);
	writer.Key("busy");
	writer.Bool(view.operation.busy);
	writer.EndObject();
	writer.Key("friends");
	writer.StartArray();
	for (const auto& friendData : view.friends)
	{
		writer.StartObject();
		writer.Key("pid");
		writer.Uint(friendData.pid);
		text("accountId", friendData.accountId);
		text("displayName", friendData.displayName);
		writer.Key("online");
		writer.Bool(view.status == "connected" && friendData.online);
		text("titleId", friendData.online && friendData.titleId ? fmt::format("{:016x}", friendData.titleId) : "");
		writer.EndObject();
	}
	writer.EndArray();
	writer.Key("requests");
	writer.StartArray();
	for (const auto& request : view.requests)
	{
		writer.StartObject();
		writer.Key("pid");
		writer.Uint(request.pid);
		text("accountId", request.accountId);
		text("displayName", request.displayName);
		text("message", request.message);
		writer.Key("incoming");
		writer.Bool(request.incoming);
		text("messageId", std::to_string(request.messageId));
		writer.Key("manageable");
		writer.Bool(request.manageable);
		writer.Key("expired");
		writer.Bool(request.expired);
		writer.EndObject();
	}
	writer.EndArray();
	writer.EndObject();
	return buffer.GetString();
}

std::string FriendsManagementCommand(uint64_t requestId, std::string_view payload)
{
	std::string notice = "invalid-command";
	rapidjson::Document document;
	if (payload.size() <= 1024 && !payload.empty())
		document.Parse<rapidjson::kParseValidateEncodingFlag>(payload.data(), payload.size());
	if (!document.HasParseError() && document.IsObject() && document.MemberCount() == 6)
	{
		const std::set<std::string> expected{"op", "generation", "connectionId", "pid", "messageId", "pnid"};
		std::set<std::string> actual;
		for (auto member = document.MemberBegin(); member != document.MemberEnd(); ++member)
			actual.emplace(member->name.GetString(), member->name.GetStringLength());
		if (actual == expected && document["op"].IsString() && document["generation"].IsUint64() && document["connectionId"].IsUint64() && document["pid"].IsUint() && document["messageId"].IsString() && document["pnid"].IsString())
		{
			FriendsSession::Command command;
			command.id = requestId;
			command.generation = document["generation"].GetUint64();
			command.connectionId = document["connectionId"].GetUint64();
			command.pid = document["pid"].GetUint();
			command.accountId.assign(document["pnid"].GetString(), document["pnid"].GetStringLength());
			const std::string_view messageId(document["messageId"].GetString(), document["messageId"].GetStringLength());
			const auto parsed = std::from_chars(messageId.data(), messageId.data() + messageId.size(), command.messageId);
			const std::string_view operation(document["op"].GetString(), document["op"].GetStringLength());
			bool valid = true;
			if (operation == "send")
				command.operation = FriendsSession::Operation::Send;
			else if (operation == "remove")
				command.operation = FriendsSession::Operation::Remove;
			else if (operation == "accept")
				command.operation = FriendsSession::Operation::Accept;
			else if (operation == "reject")
				command.operation = FriendsSession::Operation::Reject;
			else if (operation == "cancel")
				command.operation = FriendsSession::Operation::Cancel;
			else
				valid = false;
			if (valid && command.generation <= 9007199254740991ULL && command.connectionId <= 9007199254740991ULL && messageId.size() <= 20 && parsed.ec == std::errc{} && parsed.ptr == messageId.data() + messageId.size() && messageId == std::to_string(command.messageId))
				notice = FriendsSession::Instance().Submit(std::move(command));
		}
	}
	// noticeは固定値のみ。任意の入力や実PNIDを診断応答へ反射しない。
	return "{\"requestId\":" + std::to_string(requestId) + ",\"notice\":\"" + notice + "\"}";
}
