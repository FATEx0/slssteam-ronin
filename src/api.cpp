#include "api.hpp"

#include "sdk/CSteamEngine.hpp"
#include "sdk/CUser.hpp"
#include "sdk/IClientAppManager.hpp"

#include "config.hpp"
#include "filewatcher.hpp"
#include "utils.hpp"

#include <cerrno>
#include <mutex>


namespace SLSAPI
{
	const char* path = "/tmp/SLSsteam.API";

	std::fstream fstream;
	CFileWatcher* watcher;

	std::mutex executionMutex;
	std::vector<InstallCommand_t> installs;
	std::vector<AppId_t> uninstalls;
}

bool SLSAPI::isEnabled()
{
	return g_config.api.get() && fstream.is_open();
}

void SLSAPI::onFileChange()
{
	//Hot reload support :)
	if (!isEnabled())
	{
		return;
	}

	//Shitty way to reopen the stream. We have to do this, otherwise the fstream gets invalidated when running echo >
	fstream.close();
	fstream.open(path, std::fstream::in);

	char cmd[128];
	fstream.getline(cmd, sizeof(cmd));

	g_pLog->debug("API Running %s\n", cmd);

	const auto split = Utils::strsplit(cmd, "|");
	if (strcmp(split[0].c_str(), "install") == 0 && split.size() > 2)
	{
		try
		{
			const AppId_t appId = std::strtoul(split[1].c_str(), nullptr, 10);
			const uint32_t library = std::strtoul(split[2].c_str(), nullptr, 10);

			const std::lock_guard guard(executionMutex);
			installs.emplace_back(InstallCommand_t { appId, library } );
		}
		catch(...)
		{
			g_pLog->info("API Failed to parse %s or %s!\n", split[1].c_str(), split[2].c_str());
		}
	}
	else if (strcmp(split[0].c_str(), "uninstall") == 0 && split.size() > 1)
	{
		try
		{
			const AppId_t appId = std::strtoul(split[1].c_str(), nullptr, 10);

			const std::lock_guard guard(executionMutex);
			uninstalls.emplace_back(appId);
		}
		catch(...)
		{
			g_pLog->info("API Failed to parse %s!\n", split[1].c_str());
		}
	}
}

void SLSAPI::init()
{
	fstream = std::fstream(path, std::fstream::in | std::fstream::out | std::fstream::trunc); //Open for reading, writing and also delete contents

	if (!fstream.is_open())
	{
		g_pLog->warn("Failed to create %s (%s)!\n API will be unavailable", path, strerror(errno));
		return;
	}

	watcher = new CFileWatcher(onFileChange);
	const int fd = watcher->addFile(path);
	if (fd == -1)
	{
		g_pLog->warn("Failed to watch %s!\n API will be unavailable", path);
		return;
	}

	watcher->start();
	g_pLog->debug("SLSsteam API initialized!\n");
}

void SLSAPI::runIPCFrame()
{
	if (!installs.size() && !uninstalls.size()) //No need to lock mutex when no commands are queued
	{
		return;
	}

	const auto usr = g_pSteamEngine->getUser();
	if (!usr)
	{
		return;
	}

	const auto appManager = usr->getAppManager();

	const std::lock_guard guard(executionMutex);

	while(installs.size())
	{
		const auto app = installs.begin();
		appManager->installApp(app->appId, app->libraryIndex);
		installs.erase(app);

		g_pLog->debug("Installed %u to %u\n", app->appId, app->libraryIndex);
	}

	while(uninstalls.size())
	{
		const auto app = uninstalls.begin();
		appManager->uninstallApp(*app);
		uninstalls.erase(app);

		g_pLog->debug("Uninstalled %u\n", *app);
	}
}
