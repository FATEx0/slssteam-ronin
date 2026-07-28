#include "achievements.hpp"

#include "../sdk/CAPIJob.hpp"
#include "../sdk/CClientUnifiedServiceTransport.hpp"
#include "../sdk/CProtoBufMsgBase.hpp"
#include "../sdk/CSteamEngine.hpp"
#include "../sdk/CUser.hpp"
#include "../sdk/steam.hpp"

#include "../curl.hpp"
#include "../config.hpp"
#include "../log.hpp"

#include <cstdint>
#include <cstring>
#include <regex>
#include <sstream>
#include <string>


std::unordered_map<AppId_t, std::unordered_set<uint64_t>> Achievements::ownerBlacklist;


std::string Achievements::getReviewUrl(const AppId_t appId)
{
	std::stringstream url;
	url << "https://store.steampowered.com/appreviews/" << appId
		<< "?json=1&filter=recent&language=all&purchase_type=all&num_per_page="
		<< g_config.maxSchemaTries.get();

	return url.str();
}

std::unordered_set<uint64_t> Achievements::getReviewersForGame(const AppId_t appId)
{
	auto list = std::unordered_set<uint64_t>();
	if (!g_config.maxSchemaTries.get())
	{
		return list;
	}

	auto url = getReviewUrl(appId);
	std::string reviews;

	if(Curl::getString(url.c_str(), reviews))
	{
		g_pLog->debug("Failed to get reviewer list for %u!\n", appId);
		return list;
	}

	if (!ownerBlacklist.contains(appId))
	{
		ownerBlacklist[appId] = std::unordered_set<uint64_t>();
	}

	//g_pLog->debug("Downloaded reviewers %s\n", reviews.c_str());

	std::regex steamIdFieldsRe("\"steamid\":\"[0-9]+\"");

	auto begin = std::sregex_iterator(reviews.begin(), reviews.end(), steamIdFieldsRe);
	auto end = std::sregex_iterator();

	for(auto i = begin; i != end; ++i)
	{
		std::smatch steamIdMatch = *i;
		std::string steamIdFieldStr = steamIdMatch.str();

		//g_pLog->debug("SteamId match %s\n", match.str().c_str());

		std::regex idRe("[0-9]+");
		if (!std::regex_search(steamIdFieldStr, steamIdMatch, idRe))
		{
			continue;
		}

		g_pLog->debug("Extracted SteamId %s\n", steamIdMatch.str().c_str());

		uint64_t steamId = std::stoull(steamIdMatch.str().c_str());
		if (ownerBlacklist[appId].contains(steamId))
		{
			g_pLog->debug("Skipping %llu for %u because it has failed before\n", steamId, appId);
			continue;
		}

		list.emplace(steamId);
	}

	return list;
}

uint32_t Achievements::sendAndRecvGetPlayerStats
(
	CClientUnifiedServiceTransport* serviceTransport,
	const char* serviceName,
	CPlayer_GetUserStats_Request* send,
	CPlayer_GetUserStats_Response* recv
)
{
	if (strcmp(serviceName, GET_PLAYER_STATS_SERVICE_NAME) != 0)
	{
		return k_EResultNoResult;
	}

	//Don't do anything for legit apps. An unresolved user (null) can't
	//confirm ownership either way -- fall through to the reviewer path
	//rather than crash, same default used elsewhere in this codebase.
	CUser* user = getLocalUser();
	if (user != nullptr && user->isSubscribed(send->appid()))
	{
		return k_EResultNoResult;
	}

	const auto reviewers = getReviewersForGame(send->appid());

	send->clear_crc_stats();

	for(const auto& id : reviewers)
	{
		send->set_steamid(id);
		g_pLog->debug("CPlayer_GetUserStats_Request->set_steamid(%llu)\n", id);

		const auto res = serviceTransport->sendAndRecvMsg(GET_PLAYER_STATS_SERVICE_NAME, send, recv);

		if (res != k_EResultOK)
		{
			if (res == k_EResultFailure)
			{
				//Only blacklist confirmed failures, exclude NO_CONNECTION and whatever else could happen
				ownerBlacklist[send->appid()].emplace(id);
			}

			continue;
		}

		//Clear stats so steam merges them for us
		recv->clear_crc_stats();
		recv->clear_stats();

		g_pLog->debug("Using schema from %llu for %u\n", id, send->appid());
		return k_EResultOK;
	}

	g_pLog->debug("No schemas for %u found! Falling back to offline cache\n", send->appid());
	return k_EResultNoConnection;
}

uint32_t Achievements::sendAndRecvGetUserStats(CAPIJob* job, CProtoBufMsgBase* send, const uint32_t timeOut, CProtoBufMsgBase* recv, const EMsg targetType)
{
	if (targetType != k_EMsgClientGetUserStatsResponse)
	{
		return 0;
	}

	const auto sendBdy = send->getBody<CMsgClientGetUserStats>();

	CUser* user = getLocalUser();
	if (user != nullptr && user->isSubscribed(sendBdy->game_id()))
	{
		return 0;
	}

	const auto recvBdy = recv->getBody<CMsgClientGetUserStatsResponse>();
	const auto reviewers = getReviewersForGame(sendBdy->game_id());

	sendBdy->clear_crc_stats();

	for(const auto& id : reviewers)
	{
		sendBdy->set_steam_id_for_user(id);
		g_pLog->debug("CMsgClientGetUserStats->set_steam_id_for_user(%llu)\n", id);

		const uint32_t ret = job->sendAndRecv(send, timeOut, recv, targetType);
		if (!ret)
		{
			continue;
		}

		if (recvBdy->eresult() != k_EResultOK)
		{
			if (recvBdy->eresult() == k_EResultFailure)
			{
				//Only blacklist confirmed failures, exclude NO_CONNECTION and whatever else could happen
				ownerBlacklist[recvBdy->game_id()].emplace(id);
			}

			continue;
		}
		
		recvBdy->clear_achievement_blocks();
		recvBdy->clear_crc_stats();
		recvBdy->clear_stats();

		g_pLog->debug("Using schema from %llu for %u\n", id, recvBdy->game_id());

		return ret;
	}

	g_pLog->debug("No schemas for %u found! Falling back to offline cache\n", sendBdy->game_id());
	recvBdy->set_eresult(k_EResultNoConnection);
	return 1;
}
