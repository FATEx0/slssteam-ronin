#pragma once

#include "../sdk/protobufs/enums_clientserver.pb.h"

#include "../sdk/steam.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>


class CAPIJob;
class CClientUnifiedServiceTransport;
class CPlayer_GetUserStats_Request;
class CPlayer_GetUserStats_Response;
class CProtoBufMsgBase;

namespace Achievements
{
	constexpr const char* GET_PLAYER_STATS_SERVICE_NAME = "Player.GetUserStats#1";

	extern std::unordered_map<AppId_t, std::unordered_set<uint64_t>> ownerBlacklist;

	inline uint64_t resolveOwnerSteamId(
	    AppId_t appId,
	    const std::unordered_map<AppId_t, uint64_t>& perApp,
	    uint64_t defaultOwner)
	{
		const auto it = perApp.find(appId);
		return it != perApp.end() && it->second != 0 ? it->second : defaultOwner;
	}

	std::string getReviewUrl(const AppId_t appId);
	std::unordered_set<uint64_t> getReviewersForGame(const AppId_t appId);

	//CPlayer_GetUserStats
	uint32_t sendAndRecvGetPlayerStats
	(
		CClientUnifiedServiceTransport* serviceTransport,
		const char* serviceName,
		CPlayer_GetUserStats_Request* send,
		CPlayer_GetUserStats_Response* recv
	);
	//GetUserStats
	uint32_t sendAndRecvGetUserStats(CAPIJob* job, CProtoBufMsgBase* send, const uint32_t timeOut, CProtoBufMsgBase* recv, const EMsg targetType);
}
