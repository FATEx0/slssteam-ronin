#include "update.hpp"

#include "config.hpp"
#include "curl.hpp"
#include "globals.hpp"
#include "log.hpp"
#include "utils.hpp"
#include "version.hpp"


#include <filesystem>
#include <fstream>
#include <map>
#include <map>
#include <string>

std::map<uint64_t, std::unordered_set<std::string>> Updater::clientHashMap = std::map<uint64_t, std::unordered_set<std::string>>();

constexpr static const char* urls[] =
{
	"https://raw.githubusercontent.com/AceSLS/SLSsteam/refs/heads/main/res/updates.yaml",
	"https://cdn.jsdelivr.net/gh/AceSLS/SLSsteam/res/updates.yaml"
};

bool Updater::init()
{
	std::string data;
	int res;

	bool downloadSuccess = false;

	for(const auto url : urls)
	{
		data.clear();
		res = Curl::getString(url, data);

		if (res == 0 && !data.empty())
		{
			g_pLog->info(
			    "Steam compatibility hash list downloaded from %s (%zu bytes)\n",
			    url, data.size());
			downloadSuccess = true;
			break;
		}

		if (res == 0)
			g_pLog->info(
			    "Steam compatibility hash list source %s returned an empty response\n",
			    url);
		else
			g_pLog->info(
			    "Steam compatibility hash list download from %s failed "
			    "(curl exit code %d)\n",
			    url, res);
	}

	if(!downloadSuccess)
	{
		data = loadFromCache();
		if(data.size() < 1)
		{
			g_pLog->warn(
			    "Steam compatibility hash list is unavailable and no cached "
			    "copy exists\n");
			return false;
		}

		g_pLog->info(
		    "Steam compatibility hash list downloads unavailable; using cached "
		    "copy (%zu bytes)\n",
		    data.size());
	}

	g_pLog->debug("updates.yaml:\n%s\n", data.c_str());

	try
	{
		YAML::Node node = YAML::Load(data);
		for (const auto& sub : node["SafeModeHashes"])
		{
			const uint64_t version = sub.first.as<uint64_t>();
			clientHashMap[version] = std::unordered_set<std::string>();

			g_pLog->debug("Parsing version %llu\n", version);

			for(const auto& hash : sub.second)
			{
				const auto str = hash.as<std::string>();
				clientHashMap[version].emplace(str);

				g_pLog->debug("Added %s to SLSsteam version %llu\n", str.c_str(), version);
			}
		}
	}
	catch(...)
	{
		g_pLog->warn("Steam compatibility hash list could not be parsed\n");
		return false;
	}

	saveToCache(data);
	return true;
}

std::string Updater::getCacheFilePath()
{
	const auto path = g_config.getDir().append("/.updates.yaml");
	return path;
}

void Updater::saveToCache(const std::string yaml)
{
	const auto path = Updater::getCacheFilePath();

	std::ofstream stream = std::ofstream(path.c_str());
	stream << yaml;
	stream.close();

	g_pLog->debug("Cached res/updates.yaml!\n");
}

std::string Updater::loadFromCache()
{
	const auto path = Updater::getCacheFilePath();
	if (!std::filesystem::exists(path))
	{
		return std::string();
	}

	g_pLog->debug("Loading updates.ymal from disk!\n");

	std::ifstream fstream = std::ifstream(path.c_str());
	std::stringstream buf;
	buf << fstream.rdbuf();

	fstream.close();
	return buf.str();
}

bool Updater::verifySafeModeHash()
{
	const auto path = std::filesystem::path(g_modSteamClient.path);

	try
	{
		const std::string sha256 = Utils::getFileSHA256(path.c_str());
		g_pLog->info("steamclient.so hash is %s\n", sha256.c_str());

		if (!clientHashMap.contains(VERSION))
		{
			return false;
		}

		const auto& safeHashes = clientHashMap[VERSION];
		if (safeHashes.contains(sha256))
		{
			return true;
		}

		return false;
	}
	catch(std::runtime_error& err)
	{
		g_pLog->debug("Unable to read steamclient.so hash!\n");
		return false;
	}

	return true;
}
