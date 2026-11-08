#include "FriendsService.h"
#include "Cemu/nex/FriendsSession.h"
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

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
		writer.EndObject();
	}
	writer.EndArray();
	writer.EndObject();
	return buffer.GetString();
}
