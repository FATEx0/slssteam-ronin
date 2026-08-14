#pragma once

#include "../sdk/steam.hpp"

#include <cstdint>
#include <mutex>
#include <unordered_set>


class CNetPacket;
class CMsgClientGamesPlayed;
class CMsgClientPICSProductInfoRequest;
class CMsgClientPICSProductInfoResponse;
class CPlayer_GetLastPlayedTimes_Response;

template<typename T> class CUtlVector;

struct AppOwnershipInfo_t;
struct DepotInfo_t;

namespace Apps
{
	extern bool applistRequested;
	extern std::unordered_set<AppId_t> privateApps;

	extern std::mutex pendingLicenseChangesMutex;
	extern std::unordered_set<AppId_t> pendingLicenseChanges;

	bool unlockApp(const AppId_t appId, AppOwnershipInfo_t* info, const CSteamId& ownerId);
	bool unlockApp(const AppId_t appId, AppOwnershipInfo_t* info);

	void buildDepotDependency(const AppId_t appId, CUtlVector<DepotInfo_t>* depots, CUtlVector<DepotInfo_t>* sharedDepots);
	bool checkAppOwnership(const AppId_t appId, AppOwnershipInfo_t* info);
	void getSubscribedApps(AppId_t* appList, const uint32_t size, uint32_t& count);
	void parseProductInfoFromResponse(CMsgClientPICSProductInfoResponse* msg);
	void runIPCFrame();

	void postAppLicensesChanged(const std::unordered_set<AppId_t>& apps);

	bool shouldDisableCloud(const AppId_t appId);
	bool shouldDisableCDKey(const AppId_t appId);
	bool shouldDisableUpdates(const AppId_t appId);

	void sendAndRecvLastPlayedTimes(const char* name, CPlayer_GetLastPlayedTimes_Response* recv);
	void sendGamesPlayed(CNetPacket* pkt);
	void sendPICSInfoRequest(CNetPacket* pkt);
	void sendMsg(CNetPacket* pkt);

	void setConfigStoreString(const char* key, const char* value);
};
