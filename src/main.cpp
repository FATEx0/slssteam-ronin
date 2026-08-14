#include "api.hpp"
#include "config.hpp"
#include "decompiler.hpp"
#include "globals.hpp"
#include "hooks.hpp"
#include "log.hpp"
#include "patterns.hpp"
#include "ronin_env.hpp"
#include "update.hpp"
#include "utils.hpp"
#include "vftableinfo.hpp"

#include "feats/appinfo_provision.hpp"
#include "feats/appinfo_vdf.hpp"
#include "feats/cefport.hpp"
#include "feats/depotkey.hpp"
#include "feats/manifestid.hpp"
#include "feats/packagepatch.hpp"

#include "libmem/libmem.h"

#include <chrono>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <link.h>
#include <memory>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>


static bool cleanEnvVar(const char* varName, const char* endsWith)
{
	char* var = getenv(varName);
	if (var == NULL)
		return false;

	const auto splits = Utils::strsplit(var, ":");
	auto newEnv = std::string();

	for(unsigned int i = 0; i < splits.size(); i++)
	{
		const auto split = splits.at(i);
		if (split.ends_with(endsWith))
		{
			g_pLog->debug("Removed %s from $%s\n", endsWith, varName);
			continue;
		}

		if(newEnv.size() > 0)
		{
			newEnv.append(":");
		}
		newEnv.append(split);
	}

	if(newEnv.size())
	{
		setenv(varName, newEnv.c_str(), true);
	}
	else
	{
		unsetenv(varName);
	}
	//g_pLog->debug("Set %s to %s\n", varName, newEnv.c_str());

	return true;
}

//Looking at /proc/self/maps it seems like this isn't needed for processes that aren't steam
//__attribute__((noreturn))
static void unload()
{
	//Hooks::remove();

	//This is absolutely unnessecary for applications loading SLSsteam where it cancels from setup()
	//Would be nice to run have for failed load() attempts though 
	//lm_module_t mod;
	//if (LM_FindModule("SLSsteam.so", &mod))
	//{
	//	//TODO: Investigate crash ?
	//	//Possibly: Might be because we're unmapping what ever thread we're running in
	//	//munmap(reinterpret_cast<void*>(mod.base), mod.size);
	//}
	//exit(0);
}

//TODO: Remove when unload() works properly since it should not be needed anymore after that
static bool setupSuccess = false;
static uint16_t g_cefSessionPort = 0;
static bool g_cefKeepDefaultPort = false;
static int g_readinessFd = -1;
static std::string g_readinessPath;

static bool publishReadiness()
{
	const std::string steamclientHash = Utils::getFileSHA256(g_modSteamClient.path);
	if (steamclientHash.size() != 64)
		return false;
	const char* configured = getenv(("TSUKI_RONIN_RUNTIME_DIR_" + std::string(kRoninEnvId)).c_str());
	const std::string directory =
	    configured && *configured ? configured : "/tmp";
	g_readinessPath = directory + "/.slssteam-ronin.ready." + std::to_string(getpid());
	g_readinessFd = open(g_readinessPath.c_str(), O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
	if (g_readinessFd < 0 || flock(g_readinessFd, LOCK_EX | LOCK_NB) != 0)
	{
		if (g_readinessFd >= 0) close(g_readinessFd);
		g_readinessFd = -1;
		return false;
	}
	const std::string record =
	    "{\"schema_version\":\"1\",\"steamclient_sha256\":\""
	    + steamclientHash + "\"}\n";
	if (write(g_readinessFd, record.data(), record.size())
	    != static_cast<ssize_t>(record.size())
	    || fsync(g_readinessFd) != 0)
	{
		close(g_readinessFd);
		g_readinessFd = -1;
		unlink(g_readinessPath.c_str());
		g_readinessPath.clear();
		return false;
	}
	return true;
}

__attribute__((destructor))
static void removeReadiness()
{
	if (g_readinessFd >= 0) close(g_readinessFd);
	if (!g_readinessPath.empty()) unlink(g_readinessPath.c_str());
}

static void setup()
{
	lm_process_t proc {};
	if (!LM_GetProcess(&proc))
	{
		unload();
		return;
	}

	//Do not do anything in other processes
	if (strcmp(proc.name, "steam") != 0)
	{
		unload();
		return;
	}

	g_pLog = std::unique_ptr<CLog>(CLog::createDefaultLog());
	if (!g_pLog)
	{
		unload();
		return;
	}

	g_pLog->debug("SLSsteam loading in %s\n", proc.name);

	//Any release
	cleanEnvVar("LD_AUDIT", "SLSsteam.so");
	cleanEnvVar("LD_AUDIT", "library-inject.so");

	//Arch release
	cleanEnvVar("LD_AUDIT", "libSLSsteam.so");
	cleanEnvVar("LD_AUDIT", "libSLS-library-inject.so");
	//TODO: Investigate weird logging. Not like it's necessary anymore
	//cleanEnvVar("LD_PRELOAD");

	if(!g_config.init())
	{
		unload();
		return;
	}

	// Pick one port for the complete Steam session, but publish it only when
	// this process tree actually launches steamwebhelper. At login two Steam
	// clients may race; publishing here would allow the losing process to
	// overwrite Tsuki's contract with a port that never becomes live.
	if (CefPort::deckyPresent())
	{
		// Decky's injector is fixed to 8080. Multiple CDP clients can share the
		// endpoint, so preserve 8080 and remove any stale ephemeral contract.
		g_cefKeepDefaultPort = true;
		CefPort::removeContract(CefPort::contractPath());
	}
	else
	{
		g_cefSessionPort =
		    CefPort::resolveSessionPortNoPersist(CefPort::contractPath());
	}

	// App-info provisioning runs in the package-contained sls-prelaunch
	// executable before Tsuki spawns Steam. It must not run from this
	// rtld-audit callback: doing the same allocation/network/cache work here
	// corrupts the loader process's allocator on Steam build 1784778118.

	// Upstream-merge note: preserve the null check in this block. Upstream
	// historically constructed std::string(getenv("LD_LIBRARY_PATH"))
	// directly; getenv returns nullptr when the variable is unset, and passing
	// that to std::string is undefined behavior (typically a startup crash).
	// If upstream later changes this block, keep either its equivalent safe
	// handling or this guarded form while retaining the required library paths.
	//
	// Since we cannot statically link everything and distributions resolve
	// these dependencies differently, append the common system library paths.
	const char* currentLdPath = getenv("LD_LIBRARY_PATH");
	auto ldLibPath = currentLdPath ? std::string(currentLdPath) : std::string();
	if (!ldLibPath.empty())
		ldLibPath.append(":");
	ldLibPath.append("/usr/lib:/usr/lib32");
	setenv("LD_LIBRARY_PATH", ldLibPath.c_str(), true);

	Updater::init();

	setupSuccess = true;
}

static void load()
{
	if (!setupSuccess)
	{
		return;
	}

	// la_objopen reaches this function for both steamclient.so and steamui.so.
	// The detours below must be installed exactly once per process. A local
	// static alone is insufficient because glibc can instantiate an LD_AUDIT
	// object in multiple link-map namespaces, giving each copy separate
	// statics while they still patch the same steamclient text.
	static bool loadDone = false;
	if (loadDone)
		return;

	if (!g_modSteamClient.base || !g_modSteamUI.base)
	{
		return;
	}

	// Claim the process-wide pass only after both target modules exist, so an
	// early la_objopen call can retry. The kernel-held advisory lock is shared
	// by all auditor namespaces and disappears automatically with the process.
	{
		char lockPath[64];
		std::snprintf(lockPath, sizeof(lockPath),
		              "/tmp/.slssteam-ronin.load.%d", getpid());
		const int lockFd = open(lockPath, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
		if (lockFd >= 0)
		{
			if (flock(lockFd, LOCK_EX | LOCK_NB) != 0)
			{
				g_pLog->info(
				    "load: another auditor namespace already installed hooks\n");
				close(lockFd);
				return;
			}
			// Keep the winning descriptor open for process lifetime so the
			// lock remains held. O_CLOEXEC prevents inheritance.
		}
		// Failure to create a /tmp guard must not disable the module. The
		// namespace-local guard still prevents ordinary duplicate calls.
	}
	loadDone = true;

	const auto path = std::filesystem::path(g_modSteamClient.path);
	const auto dir = path.parent_path();

	g_pLog->info
	(
		"steamclient.so loaded from %s/%s at %p to %p\n",
		dir.filename().c_str(),
		path.filename().c_str(),
		g_modSteamClient.base,
		g_modSteamClient.end
	);
	g_pLog->info
	(
		"steamui.so loaded at %p to %p\n",
		g_modSteamUI.base,
		g_modSteamUI.end
	);

	if (!Updater::verifySafeModeHash())
	{
		if (g_config.safeMode.get())
		{
			g_pLog->warn("Unknown steamclient.so hash! Aborting...");
			unload();
			return;
		}
		else if (g_config.warnHashMissmatch.get())
		{
			g_pLog->warn("steamclient.so hash missmatch! Please update :)");
		}
	}

	if(!VFTIndexes::init())
	{
		g_pLog->warn("Failed to parse VFTables! Aborting...");
		return;
	}

	if (!Patterns::init())
	{
		g_pLog->warn("Failed to find all patterns! Aborting...");
		return;
	}

	if (!Hooks::setup())
	{
		unload();
		return;
	}

	SLSAPI::init();
	Decompiler::cleanUp();

	DepotKey::onStartup();
	ManifestId::importLuaScripts();

	{
		const auto added = g_config.addedAppIds.get();
		std::vector<uint32_t> ids(added.begin(), added.end());
		const auto dlcIds = AppInfoProvision::collectDlcAppIdsForAddedApps();
		PackagePatch::setExtraAppIds(dlcIds);
		ids.insert(ids.end(), dlcIds.begin(), dlcIds.end());
		if (!ids.empty())
			PackagePatch::injectIntoPackage0(ids);
	}

	if (!publishReadiness())
	{
		g_pLog->warn("Failed to publish Ronin hook readiness evidence");
		unload();
		return;
	}

	if (g_config.notifyInit.get())
	{
		const auto now = std::chrono::time_point { std::chrono::system_clock::now() };
		const auto ymd = std::chrono::year_month_day { std::chrono::floor<std::chrono::days>(now) };

		//Funsy easter egg :)
		if (static_cast<unsigned int>(ymd.month()) == 2 && static_cast<unsigned int>(ymd.day()) == 22)
		{
			g_pLog->notify("Happy birthday SLSsteam!");
		}
		else
		{
			g_pLog->notify("Loaded successfully");
		}
	}
}

namespace
{
using ExecvFn = int (*)(const char*, char* const[]);
using ExecveFn = int (*)(const char*, char* const[], char* const[]);
using SpawnFn = int (*)(pid_t*, const char*, const posix_spawn_file_actions_t*,
                        const posix_spawnattr_t*, char* const[], char* const[]);

ExecvFn g_realExecv = nullptr;
ExecvFn g_realExecvp = nullptr;
ExecveFn g_realExecve = nullptr;
ExecveFn g_realExecvpe = nullptr;
SpawnFn g_realSpawn = nullptr;
SpawnFn g_realSpawnp = nullptr;

// Return a rewritten argv copy only when Steam is launching a command that
// contains the CEF remote-debugging switch. The process is about to exec, so
// the successful-path allocation intentionally lives until exec replaces the
// image. On exec failure libc returns to us; freeRewrittenArgv then releases
// only the strings this function duplicated.
struct RewrittenArgv
{
	char** argv = nullptr;
	std::vector<bool> owned;
};

RewrittenArgv rewriteCefArgv(char* const argv[])
{
	if (!argv || g_cefKeepDefaultPort)
		return {};

	int count = 0;
	bool containsSwitch = false;
	for (; argv[count]; ++count)
	{
		const char* match = std::strstr(argv[count], CefPort::kSwitchPrefix);
		if (match
		    && std::isdigit(static_cast<unsigned char>(
		        match[std::strlen(CefPort::kSwitchPrefix)])))
			containsSwitch = true;
	}
	if (!containsSwitch)
		return {};

	const uint16_t port = g_cefSessionPort != 0
	    ? g_cefSessionPort
	    : CefPort::resolveSessionPortNoPersist(CefPort::contractPath());
	if (port == 0)
		return {};

	static std::atomic<bool> published{false};
	bool expected = false;
	if (published.compare_exchange_strong(expected, true)
	    && !CefPort::writePortFile(CefPort::contractPath(), port))
	{
		published.store(false);
	}
	else if (!expected)
	{
		if (g_pLog)
			g_pLog->info("CEF: published debug port %u to %s\n",
			             port, CefPort::contractPath().c_str());
	}

	RewrittenArgv result;
	result.argv = static_cast<char**>(
	    std::calloc(static_cast<std::size_t>(count + 1), sizeof(char*)));
	if (!result.argv)
		return {};
	result.owned.resize(static_cast<std::size_t>(count), false);

	for (int i = 0; i < count; ++i)
	{
		auto [rewritten, changed] =
		    CefPort::rewritePortArg(argv[i], port);
		if (changed)
		{
			result.argv[i] = ::strdup(rewritten.c_str());
			if (!result.argv[i])
			{
				for (int j = 0; j < i; ++j)
					if (result.owned[static_cast<std::size_t>(j)])
						std::free(result.argv[j]);
				std::free(result.argv);
				return {};
			}
			result.owned[static_cast<std::size_t>(i)] = true;
		}
		else
		{
			result.argv[i] = argv[i];
		}
	}

	if (g_pLog)
		g_pLog->info("CEF: rewrote remote-debugging port to %u\n", port);
	return result;
}

void freeRewrittenArgv(RewrittenArgv& rewritten)
{
	if (!rewritten.argv)
		return;
	for (std::size_t i = 0; i < rewritten.owned.size(); ++i)
		if (rewritten.owned[i])
			std::free(rewritten.argv[i]);
	std::free(rewritten.argv);
	rewritten.argv = nullptr;
}

int cefExecv(const char* path, char* const argv[])
{
	auto rewritten = rewriteCefArgv(argv);
	const int result = g_realExecv(path, rewritten.argv ? rewritten.argv : argv);
	freeRewrittenArgv(rewritten);
	return result;
}

int cefExecvp(const char* file, char* const argv[])
{
	auto rewritten = rewriteCefArgv(argv);
	const int result = g_realExecvp(file, rewritten.argv ? rewritten.argv : argv);
	freeRewrittenArgv(rewritten);
	return result;
}

int cefExecve(const char* path, char* const argv[], char* const envp[])
{
	auto rewritten = rewriteCefArgv(argv);
	const int result =
	    g_realExecve(path, rewritten.argv ? rewritten.argv : argv, envp);
	freeRewrittenArgv(rewritten);
	return result;
}

int cefExecvpe(const char* file, char* const argv[], char* const envp[])
{
	auto rewritten = rewriteCefArgv(argv);
	const int result =
	    g_realExecvpe(file, rewritten.argv ? rewritten.argv : argv, envp);
	freeRewrittenArgv(rewritten);
	return result;
}

int cefSpawn(pid_t* pid, const char* path,
             const posix_spawn_file_actions_t* actions,
             const posix_spawnattr_t* attributes,
             char* const argv[], char* const envp[])
{
	auto rewritten = rewriteCefArgv(argv);
	const int result = g_realSpawn(
	    pid, path, actions, attributes,
	    rewritten.argv ? rewritten.argv : argv, envp);
	freeRewrittenArgv(rewritten);
	return result;
}

int cefSpawnp(pid_t* pid, const char* file,
              const posix_spawn_file_actions_t* actions,
              const posix_spawnattr_t* attributes,
              char* const argv[], char* const envp[])
{
	auto rewritten = rewriteCefArgv(argv);
	const int result = g_realSpawnp(
	    pid, file, actions, attributes,
	    rewritten.argv ? rewritten.argv : argv, envp);
	freeRewrittenArgv(rewritten);
	return result;
}
}

extern "C" uintptr_t la_symbind32(
    Elf32_Sym* symbol, __attribute__((unused)) unsigned int index,
    __attribute__((unused)) uintptr_t* referenceCookie,
    __attribute__((unused)) uintptr_t* definitionCookie,
    __attribute__((unused)) unsigned int* flags, const char* name)
{
	const auto original = static_cast<uintptr_t>(symbol->st_value);
	if (!name)
		return original;

	if (std::strcmp(name, "execv") == 0)
	{
		if (!g_realExecv) g_realExecv = reinterpret_cast<ExecvFn>(original);
		return reinterpret_cast<uintptr_t>(&cefExecv);
	}
	if (std::strcmp(name, "execvp") == 0)
	{
		if (!g_realExecvp) g_realExecvp = reinterpret_cast<ExecvFn>(original);
		return reinterpret_cast<uintptr_t>(&cefExecvp);
	}
	if (std::strcmp(name, "execve") == 0)
	{
		if (!g_realExecve) g_realExecve = reinterpret_cast<ExecveFn>(original);
		return reinterpret_cast<uintptr_t>(&cefExecve);
	}
	if (std::strcmp(name, "execvpe") == 0)
	{
		if (!g_realExecvpe) g_realExecvpe = reinterpret_cast<ExecveFn>(original);
		return reinterpret_cast<uintptr_t>(&cefExecvpe);
	}
	if (std::strcmp(name, "posix_spawn") == 0)
	{
		if (!g_realSpawn) g_realSpawn = reinterpret_cast<SpawnFn>(original);
		return reinterpret_cast<uintptr_t>(&cefSpawn);
	}
	if (std::strcmp(name, "posix_spawnp") == 0)
	{
		if (!g_realSpawnp) g_realSpawnp = reinterpret_cast<SpawnFn>(original);
		return reinterpret_cast<uintptr_t>(&cefSpawnp);
	}
	return original;
}

extern "C" unsigned int la_version(unsigned int)
{
	return LAV_CURRENT;
}

extern "C" unsigned int la_objopen(struct link_map *map, __attribute__((unused)) Lmid_t lmid, __attribute__((unused)) uintptr_t *cookie)
{
	if (!map || !map->l_name)
		return LA_FLG_BINDFROM | LA_FLG_BINDTO;
	if (!setupSuccess)
		setup();
	if (std::string(map->l_name).ends_with("/steamclient.so"))
	{
		//Analyse modules before any relocations get applied
		LM_FindModule("steamclient.so", &g_modSteamClient);
		Decompiler::parseModule(g_modSteamClient);
		//This is wasteful, but we have to analyse right away otherwise the offset get turned into
		//addresses messing up the analysis.
		//We could workaround it by only loading after a late module has been loaded
		for(auto& vft : Decompiler::vftables)
		{
			vft.second.analyze();
		}

		load();
	}
	if (std::string(map->l_name).ends_with("/steamui.so"))
	{
		//Analyse modules before any relocations get applied
		LM_FindModule("steamui.so", &g_modSteamUI);
		Decompiler::parseModule(g_modSteamUI);
		//This is wasteful, but we have to analyse right away otherwise the offset get turned into
		//addresses messing up the analysis.
		//We could workaround it by only loading after a late module has been loaded
		for(auto& vft : Decompiler::vftables)
		{
			vft.second.analyze();
		}
		load();
	}
	return LA_FLG_BINDFROM | LA_FLG_BINDTO;
}

extern "C" void la_preinit(__attribute__((unused)) uintptr_t *cookie)
{
	setup();
}
