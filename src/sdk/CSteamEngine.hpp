#pragma once

#include "steam.hpp"

#include <cstdint>


class CUser;
class IClientUtils;

class CSteamEngine
{
public:
	CUser* getUser(const uint32_t index = 0);
	IClientUtils* getUtils();
	void setAppIdForCurrentPipe(const AppId_t appId);
};

extern CSteamEngine* g_pSteamEngine;

// Null-safe wrapper around g_pSteamEngine->getUser(0). g_pSteamEngine itself
// can be null before the Init hook resolves it, and getUser() can return
// null while the user vector is still empty during early bootstrap --
// calling ->isSubscribed()/etc directly on either without checking is a
// crash, not a graceful failure. Every caller that used to do
// g_pSteamEngine->getUser(0)->foo() unguarded should go through this instead.
CUser* getLocalUser();
