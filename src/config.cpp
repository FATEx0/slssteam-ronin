#include "config.hpp"
#include "config_discovery.hpp"
#include "confload.hpp"
#include "config_default.hpp"

#include "sdk/IClientApps.hpp"

#include "filewatcher.hpp"
#include "log.hpp"
#include "utils.hpp"

#include "yaml-cpp/yaml.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace
{
bool readFileText(const std::string& path, std::string& out);

std::filesystem::path findSteamRoot()
{
	const char* home = std::getenv("HOME");
	if (!home) return {};
	const std::filesystem::path candidates[] = {
		std::filesystem::path(home) / ".steam/steam",
		std::filesystem::path(home) / ".steam/debian-installation",
		std::filesystem::path(home) / ".local/share/Steam",
	};
	for (const auto& candidate : candidates)
	{
		std::error_code error;
		if (std::filesystem::exists(candidate / "steam.sh", error))
			return candidate;
	}
	return {};
}

std::unordered_set<AppId_t> discoverStPluginApps(
    const std::filesystem::path& steamRoot)
{
	std::unordered_set<AppId_t> result;
	if (steamRoot.empty()) return result;
	std::error_code error;
	const auto directory = steamRoot / "config/stplug-in";
	std::filesystem::directory_iterator entries(
	    directory, std::filesystem::directory_options::skip_permission_denied, error);
	if (error) return result;
	for (const auto& entry : entries)
	{
		if (!entry.is_regular_file(error) || error) { error.clear(); continue; }
		const auto appId = ConfigDiscovery::appIdFromScriptName(
		    entry.path().filename().string());
		if (ConfigDiscovery::keepDiscoveredMainApp(appId, false))
			result.emplace(appId);
	}
	return result;
}

std::unordered_set<AppId_t> loadLuaAppIds(const std::filesystem::path& path)
{
	std::unordered_set<AppId_t> result;
	std::string raw;
	if (!readFileText(path.string(), raw)) return result;
	YAML::Node node;
	std::string repaired;
	if (ConfLoad::parseWithRepair(raw, node, repaired) == ConfLoad::Outcome::Failed)
		return result;
	const auto values = node["AdditionalApps"];
	if (!values || !values.IsSequence()) return result;
	for (const auto& value : values)
	{
		const AppId_t appId = value.as<AppId_t>(0);
		if (appId != 0) result.emplace(appId);
	}
	return result;
}

bool readFileText(const std::string& path, std::string& out)
{
	std::ifstream file(path, std::ios::binary);
	if (!file.is_open()) return false;
	std::ostringstream contents;
	contents << file.rdbuf();
	out = contents.str();
	return true;
}

bool writeFileTextAtomic(const std::string& path, const std::string& text)
{
	const std::string temporary = path + ".ronin-heal.tmp";
	{
		std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
		if (!file.is_open()) return false;
		file << text;
		file.flush();
		if (!file.good()) return false;
	}
	std::error_code error;
	std::filesystem::rename(temporary, path, error);
	if (!error) return true;
	std::filesystem::remove(temporary, error);
	return false;
}
}


std::string CConfig::getDir() const
{
	char pathBuf[255];
	const char* configDir = getenv("XDG_CONFIG_HOME"); //Most users should have this set iirc
	if (configDir != NULL)
	{
		sprintf(pathBuf, "%s/SLSsteam", configDir);
	}
	else
	{
		const char* home = getenv("HOME");
		sprintf(pathBuf, "%s/.config/SLSsteam", home);
	}

	return std::string(pathBuf);
}

std::string CConfig::getPath() const
{
	return getDir().append("/config.yaml");
}

bool CConfig::createFile() const
{
	const std::string path = getPath();
	if (!std::filesystem::exists(path))
	{
		const std::string dir = getDir();
		if (!std::filesystem::exists(dir))
		{
			if (!std::filesystem::create_directory(dir))
			{
				g_pLog->notify("Unable to create config directory at %s!\n", dir.c_str());
				return false;
			}

			g_pLog->debug("Created config directory at %s\n", dir.c_str());
		}

		FILE* file = fopen(path.c_str(), "w");
		if (!file)
		{
			g_pLog->notify("Unable to create config at %s!\n", path.c_str());
			return false;
		}

		fputs(defaultConfig, file);
		fflush(file);
		fclose(file);
	}

	return true;
}

static void onFileChange()
{
	g_config.loadSettings();
	g_pLog->notify("Config reloaded!");
}

bool CConfig::init()
{
	if(createFile())
	{
		watcher = new CFileWatcher(onFileChange);
		watcher->addFile(getPath().c_str());
		// Watching the config directory also catches creation and atomic
		// replacement of luaappids.yaml.
		watcher->addFile(getDir().c_str());
		const auto steamRoot = findSteamRoot();
		const auto stplug = steamRoot / "config/stplug-in";
		std::error_code error;
		if (!steamRoot.empty() && std::filesystem::is_directory(stplug, error))
			watcher->addFile(stplug.c_str());
		watcher->start();
	}

	loadSettings(true);
	return true;
}

CConfig::~CConfig()
{
	if (watcher)
	{
		delete watcher;
	}
}


void CConfig::setError(ELoadError err)
{
	if (__loadErrors.get() > err)
	{
		return;
	}

	__loadErrors = err;
}

bool CConfig::loadSettings(bool firstLoad)
{
	YAML::Node node;
	std::string raw;
	if (!readFileText(getPath(), raw))
	{
		g_pLog->notifyLong("Can not read config.yaml!\nUsing defaults");
		node = YAML::Node();
	}
	else
	{
		std::string repaired;
		const auto outcome = ConfLoad::parseWithRepair(raw, node, repaired);
		if (outcome == ConfLoad::Outcome::Failed)
		{
			g_pLog->notifyLong("Error parsing config.yaml!\nUsing defaults");
			node = YAML::Node();
		}
		else if (outcome == ConfLoad::Outcome::Repaired)
		{
			if (writeFileTextAtomic(getPath(), repaired))
				g_pLog->notify("Config indentation was repaired on disk\n");
			else
				g_pLog->notify("Config indentation was repaired in memory; disk update failed\n");
		}
	}

	__loadErrors = ELoadError::None;
	
	disableFamilyLock = getSetting<bool>(node, "DisableFamilyShareLock", true);
	disableParentalRestrictions =
	    getSetting<bool>(node, "DisableParentalRestrictions", false);
	useWhiteList = getSetting<bool>(node, "UseWhitelist", false);
	maxSchemaTries = getSetting<uint32_t>(node, "MaxSchemaTries", 10);
	safeMode = getSetting<bool>(node, "SafeMode", false);
	notifications = getSetting<bool>(node, "Notifications", true);
	warnHashMissmatch = getSetting<bool>(node, "WarnHashMissmatch", false);
	notifyInit = getSetting<bool>(node, "NotifyInit", true);
	api = getSetting<bool>(node, "API", true);
	fakeEmail = getSetting<std::string>(node, "FakeEmail", "");
	fakeWalletBalance = getSetting<int32_t>(node, "FakeWalletBalance", 0);
	disableCloud = getSetting<bool>(node, "DisableCloud", true);
	disableUpdates = getSetting<bool>(node, "DisableUpdates", true);
	achievements = getSetting<bool>(node, "Achievements", true);
	achievementOwnerId = getSetting<uint64_t>(
	    node, "AchievementOwnerId", 76561198028121353ULL);
	dumpInterfaceMaps = getSetting<bool>(node, "DumpClientInterfaces", false);
	extendedLogging = getSetting<bool>(node, "ExtendedLogging", false);
	logLevel = getSetting<unsigned int>(node, "LogLevel", 2);

	//TODO: Create smart logging function to log them automatically via getSetting
	g_pLog->info("DisableFamilyShareLock: %i\n", disableFamilyLock.get());
	g_pLog->info("DisableParentalRestrictions: %i\n",
	             disableParentalRestrictions.get());
	g_pLog->info("UseWhitelist: %i\n", useWhiteList.get());
	g_pLog->info("MaxSchemaTries: %u\n", maxSchemaTries.get());
	g_pLog->info("SafeMode: %i\n", safeMode.get());
	g_pLog->info("Notifications: %i\n", notifications.get());
	g_pLog->info("WarnHashMissmatch: %i\n", warnHashMissmatch.get());
	g_pLog->info("NotifyInit: %i\n", notifyInit.get());
	g_pLog->info("API: %i\n", api.get());
	g_pLog->info("FakeEmail: %s\n", fakeEmail.get().c_str());
	g_pLog->info("FakeWalletBalance: %i\n", fakeWalletBalance.get());
	g_pLog->info("DisableCloud: %i\n", disableCloud.get());
	g_pLog->info("DisableUpdates: %i\n", disableUpdates.get());
	g_pLog->info("Achievements: %i\n", achievements.get());
	g_pLog->info("DumpClientInterfaces: %i\n", dumpInterfaceMaps.get());
	g_pLog->info("ExtendedLogging: %i\n", extendedLogging.get());
	g_pLog->info("LogLevel: %i\n", logLevel.get());

	const std::lock_guard appsChanged(appsChangedMutex);
	const auto prevAppIds = addedAppIds.get();
	const auto steamRoot = findSteamRoot();
	const auto stplugApps = discoverStPluginApps(steamRoot);
	const auto luaApps = loadLuaAppIds(getDir() + "/luaappids.yaml");
	const auto legacyApps = getList<AppId_t>(node, "AdditionalApps");
	ConfigDiscovery::InstalledApps installed;
	if (!steamRoot.empty())
		installed = ConfigDiscovery::scanInstalledApps(
		    ConfigDiscovery::steamAppsRootsFor(steamRoot));
	const auto classified = ConfigDiscovery::classifyAppIds(
	    stplugApps, luaApps, legacyApps, installed.all, installed.accela);
	const auto& _addedAppIds = classified.active;
	managedAppIds = classified.managed;
	g_pLog->info("Added-app sources: stplug-in=%zu luaappids=%zu "
	             "legacy=%zu managed=%zu compatibility=%zu active=%zu\n",
	             stplugApps.size(), luaApps.size(), legacyApps.size(),
	             classified.managed.size(), classified.compatibility.size(),
	             classified.active.size());

	if (!firstLoad)
	{
		for(const auto& appId : prevAppIds)
		{
			if (_addedAppIds.contains(appId))
			{
				continue;
			}

			removedApps.emplace(appId);
			g_pLog->debug("AppId %u removed from AdditionalApps\n", appId);
		}
		for(const auto& appId : _addedAppIds)
		{
			if (prevAppIds.contains(appId))
			{
				continue;
			}

			newApps.emplace(appId);
			g_pLog->debug("AppId %u added to AdditionalApps\n", appId);
		}
	}

	addedAppIds = _addedAppIds;

	appIds = getList<AppId_t>(node, "AppIds");
	fakeOffline = getList<AppId_t>(node, "FakeOffline");
	depotBlacklist = getList<AppId_t>(node, "DepotBlacklist");

	fakeAppIds = getMap<AppId_t, AppId_t>(node, "FakeAppIds");
	manifestIds = getMap<AppId_t, uint64_t>(node, "ManifestIds");
	appTokens = getMap<AppId_t, uint64_t>(node, "AppTokens");
	achievementOwners = getMap<AppId_t, uint64_t>(node, "AchievementOwners");
	gameTitles = getMap<AppId_t, std::string>(node, "GameTitles");
	subscriptionTimestamps = getMap<AppId_t, uint32_t>(node, "SubscriptionTimestamps");

	//Do not warn for these (yet?)
	const auto idleStatusNode = node["IdleStatus"];
	if (idleStatusNode)
	{
		try
		{
			const auto appId = idleStatusNode["AppId"].as<AppId_t>();
			const auto title = idleStatusNode["Title"].as<std::string>();

			idleStatus = FakeGame_t
			{
				appId,
				title
			};

			g_pLog->info("Idle status %s with AppId %u\n", title.c_str(), appId);
		}
		catch(...)
		{
			//g_pLog->warn("Failed to parse IdleStatus!");A
			setError(ELoadError::ParsingException);
		}
	}

	const auto dlcDataNode = node["DlcData"];
	if(dlcDataNode)
	{
		auto _dlcData = dlcData.empty();

		for(auto& app : dlcDataNode)
		{
			try
			{
				const AppId_t parentId = app.first.as<AppId_t>();

				CDlcData data;
				data.parentId = parentId;
				g_pLog->info("Adding DlcData for %u\n", parentId);

				for(auto& dlc : app.second)
				{
					const AppId_t dlcId = dlc.first.as<AppId_t>();
					//There's more efficient types to store strings, but they mostly do not work
					const std::string dlcName = dlc.second.as<std::string>();

					data.dlcIds[dlcId] = dlcName;
					g_pLog->info("DlcId %u -> %s\n", dlcId, dlcName.c_str());
				}

				_dlcData[parentId] = data;
			}
			catch(...)
			{
				//g_pLog->notify("Failed to parse DlcData!");
				setError(ELoadError::ParsingException);
				break;
			}
		}

		dlcData = _dlcData;
	}
	else
	{
		//g_pLog->notify("Missing DlcData entry in config!");
		setError(ELoadError::MissingKey);
	}

	const auto denuvoGamesNode = node["DenuvoGames"];
	if (denuvoGamesNode)
	{
		auto _denuvoGames = denuvoGames.empty();

		for (auto& steamIdNode : denuvoGamesNode)
		{
			try
			{
				const uint32_t steamId = steamIdNode.first.as<uint32_t>();
				_denuvoGames[steamId] = std::unordered_set<AppId_t>();

				for (auto& appIdNode : steamIdNode.second)
				{
					const AppId_t appId = appIdNode.as<AppId_t>();
					_denuvoGames[steamId].emplace(appId);

					//Again, not loggin SteamId because of privacy
					g_pLog->info("Added DenuvoGame %u\n", appId);
				}
			}
			catch (...)
			{
				//g_pLog->notify("Failed to parse DenuvoGames!");
				setError(ELoadError::ParsingException);
			}
		}

		denuvoGames.set(_denuvoGames);
	}
	else
	{
		//g_pLog->notify("Missing DenuvoGames entry in config!");
		setError(ELoadError::MissingKey);
	}

	// Structured manifest pins:
	//
	// ManifestPins:
	//   <app id>:
	//     locked: true
	//     build_id: <optional uint32>
	//     depots:
	//       <depot id>: "<uint64 gid>"
	//
	// Treat the complete block as optional and every malformed entry as local:
	// a bad app/depot/gid is skipped without discarding valid siblings or
	// throwing out of Steam's startup path. Pins for apps no longer managed by
	// Ronin are dropped so stale configuration cannot affect ordinary apps.
	{
		ManifestPins::PinMap pins;
		const auto pinsNode = node["ManifestPins"];
		if (pinsNode && pinsNode.IsMap())
		{
			for (const auto& appNode : pinsNode)
			{
				const auto& idNode = appNode.first;
				const auto& body = appNode.second;
				if (!idNode.IsScalar() || !body.IsMap())
					continue;

				const AppId_t appId = idNode.as<AppId_t>(0);
				if (appId == 0)
					continue;

				ManifestPins::AppPins app;
				app.locked = body["locked"].as<bool>(false);
				app.buildId = body["build_id"].as<uint32_t>(0);

				const auto depotsNode = body["depots"];
				if (depotsNode && depotsNode.IsMap())
				{
					for (const auto& depotNode : depotsNode)
					{
						if (!depotNode.first.IsScalar()
						    || !depotNode.second.IsScalar())
							continue;

						const AppId_t depotId =
						    depotNode.first.as<AppId_t>(0);
						uint64_t gid = 0;
						if (depotId != 0
						    && ManifestPins::parseDecimalGid(
						        depotNode.second.as<std::string>(""), gid)
						    && gid != 0)
							app.depots[depotId] = gid;
					}
				}

				pins[appId] = std::move(app);
			}
		}

		ManifestPins::purgeOrphans(pins, addedAppIds.get());
		manifestPinsByApp = pins;
		manifestPins = ManifestPins::flattenDepots(pins);
		lockedApps = ManifestPins::lockedAppSet(pins);
	}

	switch(__loadErrors.get())
	{
		case ELoadError::MissingKey:
			g_pLog->notify("Issues during config loading encountered! Missing key(s)");
			break;
		case ELoadError::ParsingException:
			g_pLog->notify("Issues during config loading encountered! Parsing error(s)");
			break;

		default:
			break;
	}

	return true;
}

bool CConfig::isAddedAppId(const AppId_t appId)
{
	return addedAppIds.get().contains(appId);
}

uint64_t CConfig::getManifestPin(AppId_t depotId)
{
	return ManifestPins::getPin(manifestPins.get(), depotId);
}

bool CConfig::isAppLocked(AppId_t appId)
{
	return ManifestPins::isLocked(lockedApps.get(), appId);
}

bool CConfig::shouldExcludeAppId(const AppId_t appId, const bool ignoreAdditionalApps)
{
	bool exclude = false;
	//Proper way would be with getAppType, but that seems broken so we need to do this instead
	constexpr AppId_t ONE_BILLION = 1E9; //Implicit cast from double to unsigned int, hopefully this does not break anything
	if (appId >= ONE_BILLION) //Higher and equal to 10^9 gets used by Steam Internally
	{
		exclude = true;
	}
	else
	{
		const bool whitelist = useWhiteList.get();
		const bool found = appIds.get().contains(appId);
		exclude = (!isAddedAppId(appId) || ignoreAdditionalApps) && ((whitelist && !found) || (!whitelist && found));

		if (!ignoreAdditionalApps)
		{
			//Might be worth to check for APPTYPE_DLC, but knowing Valve & individual gamedevs
			//surely not every DLC will be tagged as such
			char chParent[16] { };
			const int len = g_pClientApps ? g_pClientApps->getAppData(appId, "parent", chParent, sizeof(chParent)) : 0;
			//Do not blindly trust len, nor the str included. Some devs just like to mess with Valve or something (for example appId 221300)
			if (len > 0 && Utils::isNumber(chParent))
			{
				//g_pLog->debug("AppId %i, parent %s (%i)\n", appId, chParent, len);
				AppId_t parentId = std::stoul(chParent);

				if (whitelist && !shouldExcludeAppId(parentId, true))
				{
					//g_pLog->debug("Override exclude %i with false, because parent %u isn't excluded\n", exclude, parentId);
					exclude = false;
				}
				else if(!whitelist && shouldExcludeAppId(parentId, true))
				{
					//g_pLog->debug("Override exclude %i with true, because parent %u is excluded\n", exclude, parentId);
					exclude = true;
				}
			}
		}
	}

	g_pLog->once("shouldExcludeAppId(%u) -> %i\n", appId, exclude);
	return exclude;
}

uint32_t CConfig::getDenuvoGameOwner(const AppId_t appId)
{
	for(const auto& tpl : denuvoGames.get())
	{
		if (tpl.second.contains(appId))
		{
			//g_pLog->once("%u is DenuvoGame\n", appId);
			return tpl.first;
		}
	}

	return 0;
}

CConfig g_config = CConfig();
