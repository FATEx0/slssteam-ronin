#include "../src/config.hpp"
#include "../src/globals.hpp"
#include "../src/log.hpp"

#include "../src/feats/appinfo_provision.hpp"
#include "../src/feats/appinfo_vdf.hpp"
#include "../src/feats/depotkey.hpp"
#include "../src/feats/manifestid.hpp"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

int main()
{
	setenv("SLSSTEAM_PRELAUNCH", "1", 1);
	g_pLog = std::unique_ptr<CLog>(CLog::createDefaultLog());
	if (!g_pLog || !g_config.init())
		return 3;

	const char* home = std::getenv("HOME");
	if (!home)
		return 4;
	static const char* steamRoots[] = {
		"/.steam/steam",
		"/.steam/debian-installation",
		"/.local/share/Steam",
	};
	std::string appinfoPath;
	for (const char* suffix : steamRoots)
	{
		const auto candidate =
		    std::string(home) + suffix + "/appcache/appinfo.vdf";
		if (std::filesystem::exists(candidate))
		{
			appinfoPath = candidate;
			break;
		}
	}
	if (appinfoPath.empty())
		return 5;

	DepotKey::importLuaScripts();
	ManifestId::importLuaScripts();
	AppInfoProvision::provisionAllAddedApps(appinfoPath);
	AppInfoVdf::injectAllCached(appinfoPath);
	return 0;
}
