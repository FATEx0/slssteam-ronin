#pragma once

#include "../sdk/steam.hpp"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace FirstSeen
{

using TimestampMap = std::unordered_map<AppId_t, uint32_t>;

inline bool reconcile(TimestampMap& remembered,
                      const std::unordered_set<AppId_t>& active,
                      uint32_t now)
{
	bool changed = false;
	if (now == 0) return false;

	for (const AppId_t appId : active)
	{
		if (appId == 0 || remembered.contains(appId)) continue;
		remembered.emplace(appId, now);
		changed = true;
	}
	return changed;
}

inline TimestampMap effective(
    const TimestampMap& remembered,
    const TimestampMap& explicitTimestamps,
    const std::unordered_set<AppId_t>& active)
{
	TimestampMap result = explicitTimestamps;
	for (const AppId_t appId : active)
	{
		if (result.contains(appId)) continue;
		const auto found = remembered.find(appId);
		if (found != remembered.end() && found->second != 0)
			result.emplace(appId, found->second);
	}
	return result;
}

} // namespace FirstSeen
