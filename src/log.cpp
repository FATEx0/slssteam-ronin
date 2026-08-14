#include "log.hpp"

#include "config.hpp"
#include "ronin_env.hpp"

#include <cstdlib>
#include <memory>
#include <string>


CLog::CLog(const char* path) : path(path)
{
	ofstream = std::ofstream(path);
	if (!ofstream.is_open())
	{
		throw std::runtime_error("Unable to open logfile!");
	}
}

CLog::~CLog()
{
	if (ofstream.is_open())
	{
		ofstream.close();
	}
}

//Dirty workaround for not being able to access g_config from __log
LogLevel CLog::getMinLevel()
{
	return static_cast<LogLevel>(g_config.logLevel.get());
}

bool CLog::shouldNotify()
{
	return g_config.notifications.get();
}

void CLog::notifyUser(UserMsg msg, const std::string& detail)
{
	const UiMessage ui = messageFor(msg, Lang::English);
	const std::string body = substituteDetail(ui.body, detail);
	info("notifyUser: %s\n", body.c_str());
	notifyLong("%s", body.c_str());
}

CLog* CLog::createDefaultLog()
{
	const char* managed = getenv(("TSUKI_RONIN_LOG_FILE_" + std::string(kRoninEnvId)).c_str());
	if (managed && *managed)
	{
		return new CLog(managed);
	}
	const char* home = getenv("HOME");
	if (home)
	{
		std::ostringstream ss;
		ss << home << "/.SLSsteam.log";

		return new CLog(ss.str().c_str());
	}

	return nullptr;
}

std::unique_ptr<CLog> g_pLog;
