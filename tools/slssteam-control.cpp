// SPDX-License-Identifier: AGPL-3.0-only
//
// Ronin control-plane companion for module-owned views. This is deliberately
// a separate native process: Tsuki supplies only the generic framed Ronin
// transport, while all SLS-specific config knowledge stays in this package.
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/un.h>
#include <unistd.h>

#include <openssl/sha.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
std::vector<std::string> g_pendingEvents;
int g_lastFeatureReady = -1;

struct Json
{
	enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
	bool boolean = false;
	std::string text;
	std::vector<Json> array;
	std::map<std::string, Json> object;

	const Json* get(const std::string& key) const
	{
		const auto it = object.find(key);
		return it == object.end() ? nullptr : &it->second;
	}
};

class Parser
{
	const std::string& source;
	size_t pos = 0;

	void ws() { while (pos < source.size() && std::isspace(static_cast<unsigned char>(source[pos]))) ++pos; }
	char take()
	{
		if (pos >= source.size()) throw std::runtime_error("unexpected end of JSON");
		return source[pos++];
	}
	std::string string()
	{
		if (take() != '"') throw std::runtime_error("expected string");
		std::string out;
		while (pos < source.size())
		{
			const char c = take();
			if (c == '"') return out;
			if (c != '\\') { out += c; continue; }
			const char esc = take();
			switch (esc)
			{
				case '"': case '\\': case '/': out += esc; break;
				case 'b': out += '\b'; break;
				case 'f': out += '\f'; break;
				case 'n': out += '\n'; break;
				case 'r': out += '\r'; break;
				case 't': out += '\t'; break;
				default: throw std::runtime_error("unsupported JSON escape");
			}
		}
		throw std::runtime_error("unterminated string");
	}
	Json value()
	{
		ws();
		Json out;
		if (pos >= source.size()) throw std::runtime_error("missing JSON value");
		if (source[pos] == '"') { out.kind = Json::String; out.text = string(); return out; }
		if (source[pos] == '{')
		{
			++pos; out.kind = Json::Object; ws();
			if (pos < source.size() && source[pos] == '}') { ++pos; return out; }
			for (;;)
			{
				ws(); const auto key = string(); ws();
				if (take() != ':') throw std::runtime_error("expected colon");
				out.object[key] = value(); ws();
				const char c = take();
				if (c == '}') return out;
				if (c != ',') throw std::runtime_error("expected comma");
			}
		}
		if (source[pos] == '[')
		{
			++pos; out.kind = Json::Array; ws();
			if (pos < source.size() && source[pos] == ']') { ++pos; return out; }
			for (;;)
			{
				out.array.push_back(value()); ws();
				const char c = take();
				if (c == ']') return out;
				if (c != ',') throw std::runtime_error("expected comma");
			}
		}
		if (source.compare(pos, 4, "true") == 0) { pos += 4; out.kind = Json::Bool; out.boolean = true; return out; }
		if (source.compare(pos, 5, "false") == 0) { pos += 5; out.kind = Json::Bool; return out; }
		if (source.compare(pos, 4, "null") == 0) { pos += 4; return out; }
		const size_t begin = pos;
		if (source[pos] == '-') ++pos;
		while (pos < source.size() && std::isdigit(static_cast<unsigned char>(source[pos]))) ++pos;
		if (begin == pos) throw std::runtime_error("invalid JSON value");
		out.kind = Json::Number; out.text = source.substr(begin, pos - begin); return out;
	}
public:
	explicit Parser(const std::string& input) : source(input) {}
	Json parse()
	{
		Json out = value(); ws();
		if (pos != source.size()) throw std::runtime_error("trailing JSON");
		return out;
	}
};

std::string quote(const std::string& value)
{
	std::string out = "\"";
	for (const unsigned char c : value)
	{
		if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
		else if (c == '\n') out += "\\n";
		else if (c == '\r') out += "\\r";
		else if (c == '\t') out += "\\t";
		else if (c >= 0x20) out += static_cast<char>(c);
	}
	return out + '"';
}

bool decimal(const std::string& value, uint64_t max)
{
	if (value.empty()) return false;
	uint64_t result = 0;
	for (const char c : value)
	{
		if (c < '0' || c > '9') return false;
		const unsigned digit = static_cast<unsigned>(c - '0');
		if (digit > max) return false;
		if (result > (max - digit) / 10) return false;
		result = result * 10 + digit;
	}
	return true;
}

std::string scalar(const Json* value)
{
	if (!value || (value->kind != Json::String && value->kind != Json::Number))
		throw std::runtime_error("expected decimal string");
	return value->text;
}

struct AppPin
{
	bool locked = true;
	uint32_t build = 0;
	std::map<uint32_t, uint64_t> depots;
};
using Pins = std::map<uint32_t, AppPin>;
struct PendingManifestPack
{
	std::string path;
	Pins before;
	std::string event;
};
std::map<std::string, PendingManifestPack> g_pendingManifestPacks;

struct ManagedGame
{
	uint32_t appid = 0;
	std::string title;
	uint32_t installedBuild = 0;
	std::map<uint32_t, uint64_t> manifests;
};

struct ManifestTransition
{
	uint64_t oldGid = 0;
	uint64_t newGid = 0;
};

struct BuildObservation
{
	uint32_t build = 0;
	std::map<uint32_t, ManifestTransition> transitions;
};

using BuildSnapshots = std::vector<std::pair<uint32_t, std::map<uint32_t, uint64_t>>>;

struct TimedSnapshot
{
	uint32_t build = 0;
	std::string published;
	std::map<uint32_t, uint64_t> depots;
};

struct AppHistory
{
	struct Depot
	{
		uint32_t id = 0;
		uint32_t owner = 0;
		std::string configuration;
	};
	struct Branch
	{
		std::string name;
		uint32_t build = 0;
		std::string built;
		std::string updated;
	};
	uint32_t appid = 0;
	std::string relation;
	std::vector<Depot> declaredDepots;
	std::vector<Branch> branches;
	std::vector<TimedSnapshot> builds;
};

struct SharedDepotHistory
{
	struct Manifest
	{
		uint64_t gid = 0;
		std::string seen;
	};
	uint32_t depot = 0;
	uint32_t owner = 0;
	std::string category;
	std::vector<Manifest> manifests;
};

std::string trim(std::string value)
{
	const auto first = value.find_first_not_of(" \t\r");
	if (first == std::string::npos) return {};
	const auto last = value.find_last_not_of(" \t\r");
	return value.substr(first, last - first + 1);
}

bool keyValue(const std::string& line, unsigned indent, std::string& key, std::string& value)
{
	if (line.size() < indent || line.compare(0, indent, std::string(indent, ' ')) != 0) return false;
	if (line.size() > indent && line[indent] == ' ') return false;
	const auto colon = line.find(':', indent);
	if (colon == std::string::npos) return false;
	key = trim(line.substr(indent, colon - indent));
	value = trim(line.substr(colon + 1));
	if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
		value = value.substr(1, value.size() - 2);
	return true;
}

Pins parsePins(const std::vector<std::string>& lines, size_t begin, size_t end)
{
	Pins pins;
	uint32_t app = 0;
	bool inDepots = false;
	for (size_t i = begin; i < end; ++i)
	{
		std::string key, value;
		if (keyValue(lines[i], 2, key, value) && value.empty() && decimal(key, UINT32_MAX))
		{
			app = static_cast<uint32_t>(std::stoul(key));
			pins[app] = {}; inDepots = false; continue;
		}
		if (!app) continue;
		if (keyValue(lines[i], 4, key, value))
		{
			inDepots = key == "depots";
			if (key == "locked") pins[app].locked = value == "true" || value == "yes";
			else if ((key == "build" || key == "build_id") && decimal(value, UINT32_MAX))
				pins[app].build = static_cast<uint32_t>(std::stoul(value));
			continue;
		}
		if (inDepots && keyValue(lines[i], 6, key, value)
		    && decimal(key, UINT32_MAX) && decimal(value, UINT64_MAX))
			pins[app].depots[static_cast<uint32_t>(std::stoul(key))] = std::stoull(value);
	}
	return pins;
}

std::vector<std::string> readLines(const std::string& path)
{
	std::ifstream input(path);
	if (!input) throw std::runtime_error("cannot read " + path);
	std::vector<std::string> lines;
	for (std::string line; std::getline(input, line);) lines.push_back(line);
	return lines;
}

std::pair<size_t, size_t> pinsRange(const std::vector<std::string>& lines)
{
	for (size_t i = 0; i < lines.size(); ++i)
	{
		if (trim(lines[i]) != "ManifestPins:") continue;
		size_t end = i + 1;
		while (end < lines.size())
		{
			const auto t = trim(lines[end]);
			if (!t.empty() && t.front() != '#' && !std::isspace(static_cast<unsigned char>(lines[end][0]))) break;
			++end;
		}
		return {i, end};
	}
	return {lines.size(), lines.size()};
}

Pins loadPins(const std::string& path)
{
	const auto lines = readLines(path);
	const auto [begin, end] = pinsRange(lines);
	return begin == lines.size() ? Pins{} : parsePins(lines, begin + 1, end);
}

std::string steamRoot()
{
	const char* home = std::getenv("HOME");
	if (!home || !*home) return {};
	for (const char* suffix : {"/.local/share/Steam", "/.steam/steam",
	                           "/.steam/debian-installation"})
	{
		const std::string candidate = std::string(home) + suffix;
		if (std::filesystem::is_directory(candidate + "/config/stplug-in"))
			return candidate;
	}
	return {};
}

void readAppManifest(const std::string& root, ManagedGame& game)
{
	std::ifstream input(root + "/steamapps/appmanifest_"
	                    + std::to_string(game.appid) + ".acf");
	if (!input) return;
	std::regex pair(R"re(^\s*"([^"]+)"\s*"([^"]*)")re");
	std::string pendingDepot;
	bool inInstalled = false;
	unsigned depth = 0;
	for (std::string line; std::getline(input, line);)
	{
		const std::string clean = trim(line);
		if (clean == "\"InstalledDepots\"") { inInstalled = true; depth = 0; continue; }
		if (inInstalled && clean == "{") { ++depth; continue; }
		if (inInstalled && clean == "}")
		{
			if (depth > 0) --depth;
			if (depth == 0) inInstalled = false;
			pendingDepot.clear();
			continue;
		}
		std::smatch match;
		if (!std::regex_search(line, match, pair)) continue;
		const std::string key = match[1], value = match[2];
		if (key == "name") game.title = value;
		else if (key == "buildid" && decimal(value, UINT32_MAX))
			game.installedBuild = static_cast<uint32_t>(std::stoul(value));
		else if (inInstalled && depth == 1 && decimal(key, UINT32_MAX))
			pendingDepot = key;
		else if (inInstalled && key == "manifest" && decimal(pendingDepot, UINT32_MAX)
		         && decimal(value, UINT64_MAX))
			game.manifests[static_cast<uint32_t>(std::stoul(pendingDepot))] =
			    std::stoull(value);
	}
}

std::map<uint32_t, ManagedGame> discoverGames()
{
	std::map<uint32_t, ManagedGame> games;
	const std::string root = steamRoot();
	if (root.empty()) return games;
	const std::filesystem::path plugin = root + "/config/stplug-in";
	const std::regex manifestCall(
	    R"re(setManifestid\s*\(\s*(\d+)\s*,\s*["'](\d+)["'])re");
	std::error_code error;
	for (const auto& entry : std::filesystem::directory_iterator(plugin, error))
	{
		if (!entry.is_regular_file() || entry.path().extension() != ".lua") continue;
		const std::string stem = entry.path().stem().string();
		if (!decimal(stem, UINT32_MAX) || stem == "0") continue;
		const uint32_t appid = static_cast<uint32_t>(std::stoul(stem));
		ManagedGame& game = games[appid];
		game.appid = appid;
		std::ifstream source(entry.path());
		for (std::string line; std::getline(source, line);)
		{
			std::smatch match;
			if (!std::regex_search(line, match, manifestCall)) continue;
			const std::string depot = match[1], gid = match[2];
			if (decimal(depot, UINT32_MAX) && decimal(gid, UINT64_MAX))
				game.manifests[static_cast<uint32_t>(std::stoul(depot))] =
				    std::stoull(gid);
		}
		readAppManifest(root, game);
	}

	const char* home = std::getenv("HOME");
	std::ifstream additional(std::string(home ? home : "")
	                         + "/.config/SLSsteam/luaappids.yaml");
	const std::regex appLine(R"re(^\s*-\s*(\d+)\s*$)re");
	for (std::string line; std::getline(additional, line);)
	{
		std::smatch match;
		if (!std::regex_match(line, match, appLine)
		    || !decimal(match[1], UINT32_MAX) || match[1] == "0") continue;
		const uint32_t appid = static_cast<uint32_t>(std::stoul(match[1]));
		games[appid].appid = appid;
		readAppManifest(root, games[appid]);
	}
	return games;
}

void savePins(const std::string& path, const Pins& pins)
{
	struct stat original{};
	if (::stat(path.c_str(), &original) != 0)
		throw std::runtime_error("cannot stat " + path);
	auto lines = readLines(path);
	const auto [begin, end] = pinsRange(lines);
	std::vector<std::string> block;
	if (!pins.empty())
	{
		block.push_back("ManifestPins:");
		for (const auto& [appid, app] : pins)
		{
			block.push_back("  " + std::to_string(appid) + ":");
			block.push_back(std::string("    locked: ") + (app.locked ? "true" : "false"));
			block.push_back("    build_id: " + std::to_string(app.build));
			block.push_back("    depots:");
			for (const auto& [depot, gid] : app.depots)
				block.push_back("      " + std::to_string(depot) + ": \"" + std::to_string(gid) + "\"");
		}
	}
	lines.erase(lines.begin() + static_cast<long>(begin), lines.begin() + static_cast<long>(end));
	lines.insert(lines.begin() + static_cast<long>(begin), block.begin(), block.end());
	const std::string temporary = path + ".slssteam-control.tmp";
	{
		std::ofstream output(temporary, std::ios::trunc);
		if (!output) throw std::runtime_error("cannot create temporary config");
		for (const auto& line : lines) output << line << '\n';
		output.flush();
		if (!output) throw std::runtime_error("failed writing temporary config");
	}
	if (::chmod(temporary.c_str(), original.st_mode & 07777) != 0)
	{
		::unlink(temporary.c_str());
		throw std::runtime_error("cannot preserve config permissions");
	}
	if (::rename(temporary.c_str(), path.c_str()) != 0)
	{
		::unlink(temporary.c_str());
		throw std::runtime_error(std::string("atomic config replacement failed: ") + std::strerror(errno));
	}
}

std::string pinsJson(const Pins& pins)
{
	std::string out = "{\"apps\":[";
	bool firstApp = true;
	for (const auto& [appid, app] : pins)
	{
		if (!firstApp) out += ',';
		firstApp = false;
		out += "{\"appid\":\"" + std::to_string(appid) + "\",\"locked\":"
		    + (app.locked ? "true" : "false") + ",\"build_id\":\""
		    + std::to_string(app.build) + "\",\"depots\":{";
		bool firstDepot = true;
		for (const auto& [depot, gid] : app.depots)
		{
			if (!firstDepot) out += ',';
			firstDepot = false;
			out += quote(std::to_string(depot)) + ':' + quote(std::to_string(gid));
		}
		out += "}}";
	}
	return out + "]}";
}

std::string catalogJson(const Pins& pins)
{
	std::string out = pinsJson(pins);
	out.pop_back();
	out += ",\"games\":[";
	bool firstGame = true;
	for (const auto& [appid, game] : discoverGames())
	{
		if (!firstGame) out += ',';
		firstGame = false;
		out += "{\"appid\":" + quote(std::to_string(appid))
		    + ",\"title\":" + quote(game.title.empty()
		        ? ("App " + std::to_string(appid)) : game.title)
		    + ",\"installed_build_id\":" + quote(std::to_string(game.installedBuild))
		    + ",\"manifests\":{";
		bool firstManifest = true;
		for (const auto& [depot, gid] : game.manifests)
		{
			if (!firstManifest) out += ',';
			firstManifest = false;
			out += quote(std::to_string(depot)) + ':' + quote(std::to_string(gid));
		}
		out += "}}";
	}
	return out + "]}";
}

std::string configPath()
{
	if (const char* managed = std::getenv("TSUKI_RONIN_SETTINGS_PATH");
	    managed && *managed) return managed;
	const char* home = std::getenv("HOME");
	if (!home || !*home) throw std::runtime_error("HOME is unset");
	return std::string(home) + "/.config/SLSsteam/config.yaml";
}

std::string historyPath(uint32_t appid)
{
	if (const char* managed = std::getenv("TSUKI_RONIN_DATA_DIR");
	    managed && *managed)
		return std::string(managed) + "/manifest-history/" + std::to_string(appid) + ".json";
	const char* home = std::getenv("HOME");
	if (!home || !*home) throw std::runtime_error("HOME is unset");
	return std::string(home) + "/.config/SLSsteam/ronin/manifest-history/"
	    + std::to_string(appid) + ".json";
}

uint32_t decimal32(const Json* value, const char* label)
{
	const std::string text = scalar(value);
	if (!decimal(text, UINT32_MAX) || text == "0")
		throw std::runtime_error(std::string("invalid ") + label);
	return static_cast<uint32_t>(std::stoul(text));
}

uint64_t decimal64(const Json* value, const char* label)
{
	const std::string text = scalar(value);
	if (!decimal(text, UINT64_MAX))
		throw std::runtime_error(std::string("invalid ") + label);
	return std::stoull(text);
}

std::map<uint32_t, uint64_t> depotMap(const Json* value, const char* label,
                                     bool requireNonempty = true)
{
	if (!value || value->kind != Json::Object)
		throw std::runtime_error(std::string(label) + " must be an object");
	std::map<uint32_t, uint64_t> out;
	for (const auto& [depotText, gid] : value->object)
	{
		if (!decimal(depotText, UINT32_MAX) || depotText == "0")
			throw std::runtime_error("invalid depot id");
		const uint32_t depot = static_cast<uint32_t>(std::stoul(depotText));
		const uint64_t manifest = decimal64(&gid, "manifest gid");
		if (!manifest) throw std::runtime_error("manifest gid cannot be zero");
		if (!out.emplace(depot, manifest).second)
			throw std::runtime_error("duplicate depot id");
	}
	if (requireNonempty && out.empty()) throw std::runtime_error("anchor depots cannot be empty");
	return out;
}

BuildSnapshots reconstructHistory(const Json& payload, uint32_t& appid)
{
	appid = decimal32(payload.get("appid"), "appid");
	const Json* anchor = payload.get("anchor");
	if (!anchor || anchor->kind != Json::Object)
		throw std::runtime_error("anchor must be an object");
	const uint32_t anchorBuild = decimal32(anchor->get("build_id"), "anchor build id");
	const auto anchorDepots = depotMap(anchor->get("depots"), "anchor depots");
	const Json* builds = payload.get("builds");
	if (!builds || builds->kind != Json::Array || builds->array.empty())
		throw std::runtime_error("builds must be a non-empty array");

	// A local added-game snapshot can contain DLC or auxiliary depots whose
	// history belongs to another app and is absent from this observation.
	// Carrying their current GIDs backwards would fabricate historical data.
	// Reconstruct only depots for which the provider supplied at least one
	// transition; omitted depots remain unpinned and follow Steam normally.
	std::map<uint32_t, uint64_t> current;
	for (const Json& observed : builds->array)
	{
		if (observed.kind != Json::Object) continue;
		const Json* transitions = observed.get("transitions");
		if (!transitions || transitions->kind != Json::Object) continue;
		for (const auto& [depotText, ignored] : transitions->object)
		{
			(void)ignored;
			if (!decimal(depotText, UINT32_MAX) || depotText == "0") continue;
			const uint32_t depot = static_cast<uint32_t>(std::stoul(depotText));
			const auto anchorDepot = anchorDepots.find(depot);
			if (anchorDepot != anchorDepots.end()) current[depot] = anchorDepot->second;
		}
	}
	if (current.empty())
		throw std::runtime_error("observation covers no depot in the local anchor");

	BuildSnapshots snapshots;
	uint32_t previousBuild = 0;
	for (size_t index = 0; index < builds->array.size(); ++index)
	{
		const Json& observed = builds->array[index];
		if (observed.kind != Json::Object)
			throw std::runtime_error("build observation must be an object");
		const uint32_t build = decimal32(observed.get("build_id"), "build id");
		if (index == 0 && build != anchorBuild)
			throw std::runtime_error("first observed build does not match anchor");
		if (previousBuild && build == previousBuild)
			throw std::runtime_error("duplicate adjacent build id");
		if (std::any_of(snapshots.begin(), snapshots.end(),
		    [build](const auto& item) { return item.first == build; }))
			throw std::runtime_error("duplicate build id");
		snapshots.emplace_back(build, current);
		previousBuild = build;

		const Json* transitions = observed.get("transitions");
		if (!transitions || transitions->kind != Json::Object)
			throw std::runtime_error("transitions must be an object");
		for (const auto& [depotText, transition] : transitions->object)
		{
			if (!decimal(depotText, UINT32_MAX) || depotText == "0"
			    || transition.kind != Json::Object)
				throw std::runtime_error("invalid depot transition");
			const uint32_t depot = static_cast<uint32_t>(std::stoul(depotText));
			const uint64_t oldGid = decimal64(transition.get("old"), "old manifest gid");
			const uint64_t newGid = decimal64(transition.get("new"), "new manifest gid");
			const auto live = current.find(depot);
			if (newGid == 0)
			{
				if (live != current.end())
					throw std::runtime_error("removed-depot transition does not join current snapshot");
			}
			else if (live == current.end() || live->second != newGid)
			{
				throw std::runtime_error("manifest transition does not join current snapshot");
			}
			if (oldGid == 0) current.erase(depot);
			else current[depot] = oldGid;
		}
	}
	return snapshots;
}

std::string historyJson(uint32_t appid, const BuildSnapshots& snapshots)
{
	std::string out = "{\"schema_version\":\"1.0\",\"source\":\"steamdb-browser-observation\","
	    "\"appid\":" + quote(std::to_string(appid)) + ",\"builds\":[";
	bool firstBuild = true;
	for (const auto& [build, depots] : snapshots)
	{
		if (!firstBuild) out += ',';
		firstBuild = false;
		out += "{\"build_id\":" + quote(std::to_string(build)) + ",\"depots\":{";
		bool firstDepot = true;
		for (const auto& [depot, gid] : depots)
		{
			if (!firstDepot) out += ',';
			firstDepot = false;
			out += quote(std::to_string(depot)) + ':' + quote(std::to_string(gid));
		}
		out += "}}";
	}
	return out + "]}";
}

std::string depotsJson(const std::map<uint32_t, uint64_t>& depots)
{
	std::string out = "{";
	bool first = true;
	for (const auto& [depot, gid] : depots)
	{
		if (!first) out += ',';
		first = false;
		out += quote(std::to_string(depot)) + ':' + quote(std::to_string(gid));
	}
	return out + '}';
}

std::string requiredString(const Json* value, const char* label)
{
	if (!value || value->kind != Json::String || value->text.empty())
		throw std::runtime_error(std::string("invalid ") + label);
	return value->text;
}

std::string optionalString(const Json* value, const char* label)
{
	if (!value || value->kind == Json::Null) return {};
	if (value->kind != Json::String)
		throw std::runtime_error(std::string("invalid ") + label);
	return value->text;
}

std::vector<AppHistory> reconstructAppHistories(const Json& payload, uint32_t& rootAppid)
{
	rootAppid = decimal32(payload.get("appid"), "appid");
	const Json* anchor = payload.get("anchor");
	if (!anchor || anchor->kind != Json::Object)
		throw std::runtime_error("anchor must be an object");
	const uint32_t anchorBuild = decimal32(anchor->get("build_id"), "anchor build id");
	const auto anchorDepots = depotMap(anchor->get("depots"), "anchor depots");
	const Json* apps = payload.get("apps");
	if (!apps || apps->kind != Json::Array || apps->array.empty())
		throw std::runtime_error("apps must be a non-empty array");

	std::vector<AppHistory> result;
	bool sawRoot = false;
	for (const Json& observedApp : apps->array)
	{
		if (observedApp.kind != Json::Object)
			throw std::runtime_error("app history must be an object");
		AppHistory history;
		history.appid = decimal32(observedApp.get("appid"), "history appid");
		history.relation = requiredString(observedApp.get("relation"), "app relation");
		if (history.relation != "root" && history.relation != "dlc")
			throw std::runtime_error("unsupported app relation");
		if (std::any_of(result.begin(), result.end(), [&](const AppHistory& item) {
		    return item.appid == history.appid;
		})) throw std::runtime_error("duplicate app history");
		const bool root = history.appid == rootAppid;
		if (root != (history.relation == "root"))
			throw std::runtime_error("root app relation mismatch");
		sawRoot = sawRoot || root;
		const Json* downloadable = observedApp.get("downloadable");
		if (!downloadable || downloadable->kind != Json::Bool)
			throw std::runtime_error("downloadable must be boolean");
		if (!downloadable->boolean)
		{
			if (root) throw std::runtime_error("root app is not downloadable");
			continue;
		}
		const Json* declarations = observedApp.get("depots");
		if (!declarations || declarations->kind != Json::Array
		    || declarations->array.empty())
			throw std::runtime_error("downloadable app has no depot declarations");
		for (const Json& declaration : declarations->array)
		{
			if (declaration.kind != Json::Object)
				throw std::runtime_error("invalid depot declaration");
			AppHistory::Depot depot;
			depot.id = decimal32(declaration.get("depot_id"), "declared depot id");
			depot.owner = decimal32(declaration.get("owner_appid"), "depot owner appid");
			depot.configuration = requiredString(declaration.get("configuration"),
			                                     "depot configuration");
			if (std::any_of(history.declaredDepots.begin(), history.declaredDepots.end(),
			    [&](const AppHistory::Depot& item) { return item.id == depot.id; }))
				throw std::runtime_error("duplicate depot declaration");
			history.declaredDepots.push_back(std::move(depot));
		}
		const Json* branchList = observedApp.get("branches");
		if (!branchList || branchList->kind != Json::Array)
			throw std::runtime_error("branches must be an array");
		for (const Json& observedBranch : branchList->array)
		{
			if (observedBranch.kind != Json::Object)
				throw std::runtime_error("invalid branch declaration");
			AppHistory::Branch branch;
			branch.name = requiredString(observedBranch.get("name"), "branch name");
			branch.build = decimal32(observedBranch.get("build_id"), "branch build id");
			branch.built = optionalString(observedBranch.get("built_at"), "branch built time");
			branch.updated = optionalString(observedBranch.get("updated_at"), "branch updated time");
			if (std::any_of(history.branches.begin(), history.branches.end(),
			    [&](const AppHistory::Branch& item) { return item.name == branch.name; }))
				throw std::runtime_error("duplicate branch name");
			history.branches.push_back(std::move(branch));
		}

		const Json* builds = observedApp.get("builds");
		if (!builds || builds->kind != Json::Array || builds->array.empty())
			throw std::runtime_error("app builds must be a non-empty array");
		std::map<uint32_t, uint64_t> current;
		for (const AppHistory::Depot& depot : history.declaredDepots)
		{
			if (depot.owner != history.appid) continue;
			const auto anchorDepot = anchorDepots.find(depot.id);
			if (anchorDepot != anchorDepots.end())
				current[depot.id] = anchorDepot->second;
		}
		if (current.empty())
		{
			// An unowned DLC listed by SteamDB is intentionally irrelevant to
			// this local added-game package.
			if (!root) continue;
			throw std::runtime_error("root history covers no local anchor depot");
		}

		std::string previousTime;
		for (size_t index = 0; index < builds->array.size(); ++index)
		{
			const Json& build = builds->array[index];
			const uint32_t buildid = decimal32(build.get("build_id"), "build id");
			const std::string published = requiredString(build.get("published_at"),
			                                             "build timestamp");
			if (!previousTime.empty() && published >= previousTime)
				throw std::runtime_error("build timestamps are not newest-to-oldest");
			if (index == 0 && root && buildid != anchorBuild)
				throw std::runtime_error("first root build does not match local anchor");
			if (std::any_of(history.builds.begin(), history.builds.end(),
			    [buildid](const TimedSnapshot& item) { return item.build == buildid; }))
				throw std::runtime_error("duplicate build id");
			history.builds.push_back({buildid, published, current});
			previousTime = published;

			const Json* transitions = build.get("transitions");
			if (!transitions || transitions->kind != Json::Object)
				throw std::runtime_error("transitions must be an object");
			for (const auto& [depotText, transition] : transitions->object)
			{
				if (!decimal(depotText, UINT32_MAX) || depotText == "0"
				    || transition.kind != Json::Object)
					throw std::runtime_error("invalid depot transition");
				const uint32_t depot = static_cast<uint32_t>(std::stoul(depotText));
				const uint64_t oldGid = decimal64(transition.get("old"), "old manifest gid");
				const uint64_t newGid = decimal64(transition.get("new"), "new manifest gid");
				const auto live = current.find(depot);
				if (newGid == 0)
				{
					if (live != current.end())
						throw std::runtime_error("removed-depot transition does not join current snapshot");
				}
				else if (live == current.end() || live->second != newGid)
					throw std::runtime_error("manifest transition does not join current snapshot");
				if (oldGid == 0) current.erase(depot);
				else current[depot] = oldGid;
			}
		}
		result.push_back(std::move(history));
	}
	if (!sawRoot) throw std::runtime_error("root app history is missing");
	return result;
}

std::vector<SharedDepotHistory> reconstructSharedDepotHistories(
    const Json& payload, uint32_t rootAppid, const std::vector<AppHistory>& apps)
{
	const Json* observed = payload.get("shared_depot_histories");
	if (!observed) return {};
	if (observed->kind != Json::Array)
		throw std::runtime_error("shared depot histories must be an array");
	const Json* policy = payload.get("availability_policy");
	if (!policy || policy->kind != Json::Object ||
	    requiredString(policy->get("dlc_release_between_base_builds"),
	                   "DLC availability policy") != "preceding_base_build")
		throw std::runtime_error("unsupported DLC availability policy");
	const auto root = std::find_if(apps.begin(), apps.end(), [rootAppid](const AppHistory& app) {
		return app.appid == rootAppid;
	});
	if (root == apps.end()) throw std::runtime_error("root app history is missing");

	std::vector<SharedDepotHistory> result;
	for (const Json& item : observed->array)
	{
		if (item.kind != Json::Object)
			throw std::runtime_error("shared depot history must be an object");
		SharedDepotHistory history;
		history.depot = decimal32(item.get("depot_id"), "shared depot id");
		history.owner = decimal32(item.get("owner_appid"), "shared depot owner");
		history.category = requiredString(item.get("category"), "shared depot category");
		if (history.category != "shared" && history.category != "redistributable")
			throw std::runtime_error("unsupported shared depot category");
		if (std::any_of(result.begin(), result.end(), [&](const SharedDepotHistory& other) {
			return other.depot == history.depot;
		})) throw std::runtime_error("duplicate shared depot history");
		const auto declaration = std::find_if(root->declaredDepots.begin(),
		    root->declaredDepots.end(), [&](const AppHistory::Depot& depot) {
			return depot.id == history.depot && depot.owner == history.owner;
		});
		if (declaration == root->declaredDepots.end())
			throw std::runtime_error("shared depot history is not declared by root app");
		const Json* manifests = item.get("manifests");
		if (!manifests || manifests->kind != Json::Array || manifests->array.empty())
			throw std::runtime_error("shared depot manifests must be a non-empty array");
		std::string previousTime;
		for (const Json& manifest : manifests->array)
		{
			if (manifest.kind != Json::Object)
				throw std::runtime_error("shared depot manifest must be an object");
			SharedDepotHistory::Manifest parsed;
			parsed.gid = decimal64(manifest.get("manifest_id"), "shared manifest gid");
			if (!parsed.gid) throw std::runtime_error("shared manifest gid cannot be zero");
			parsed.seen = requiredString(manifest.get("seen_at"), "shared manifest timestamp");
			if (!previousTime.empty() && parsed.seen >= previousTime)
				throw std::runtime_error("shared manifest timestamps are not newest-to-oldest");
			if (std::any_of(history.manifests.begin(), history.manifests.end(),
			    [&](const auto& other) { return other.gid == parsed.gid; }))
				throw std::runtime_error("duplicate shared manifest gid");
			previousTime = parsed.seen;
			history.manifests.push_back(std::move(parsed));
		}

		// stplug-in Lua files are the only source of decryption keys for
		// Lua-added game and DLC depots. They are not a complete Steam depot
		// topology and normally omit shared/redistributable depots entirely.
		// Consequently these histories cannot be required to join the
		// Lua-derived local anchor. They are instead bounded by two independent
		// observations: SteamDB must declare the shared depot on the root app,
		// and its timestamped history must cover every root-build interval.
		// Under preceding_base_build, a shared release between base patches is
		// assigned to the older endpoint, exactly like a DLC release.
		for (size_t index = 0; index < root->builds.size(); ++index)
		{
			const std::string upper = index > 0 ? root->builds[index - 1].published : "";
			const bool covered = std::any_of(history.manifests.begin(),
			    history.manifests.end(), [&](const SharedDepotHistory::Manifest& manifest) {
				    return upper.empty() || manifest.seen < upper;
			    });
			if (!covered)
				throw std::runtime_error(
				    "shared depot history does not cover every root build interval");
		}
		result.push_back(std::move(history));
	}
	return result;
}

std::string appHistoriesJson(uint32_t rootAppid, const std::vector<AppHistory>& apps,
                             const std::vector<SharedDepotHistory>& shared)
{
	std::string out = "{\"schema_version\":\"2.1\",\"source\":\"steamdb-browser-observation\","
	    "\"appid\":" + quote(std::to_string(rootAppid)) + ",\"apps\":[";
	bool firstApp = true;
	for (const AppHistory& app : apps)
	{
		if (!firstApp) out += ',';
		firstApp = false;
		out += "{\"appid\":" + quote(std::to_string(app.appid))
		    + ",\"relation\":" + quote(app.relation) + ",\"downloadable\":true,\"depots\":[";
		bool firstDepotDeclaration = true;
		for (const AppHistory::Depot& depot : app.declaredDepots)
		{
			if (!firstDepotDeclaration) out += ',';
			firstDepotDeclaration = false;
			out += "{\"depot_id\":" + quote(std::to_string(depot.id))
			    + ",\"owner_appid\":" + quote(std::to_string(depot.owner))
			    + ",\"configuration\":" + quote(depot.configuration) + '}';
		}
		out += "],\"branches\":[";
		bool firstBranch = true;
		for (const AppHistory::Branch& branch : app.branches)
		{
			if (!firstBranch) out += ',';
			firstBranch = false;
			out += "{\"name\":" + quote(branch.name)
			    + ",\"build_id\":" + quote(std::to_string(branch.build))
			    + ",\"built_at\":" + quote(branch.built)
			    + ",\"updated_at\":" + quote(branch.updated) + '}';
		}
		out += "],\"builds\":[";
		bool firstBuild = true;
		for (const TimedSnapshot& build : app.builds)
		{
			if (!firstBuild) out += ',';
			firstBuild = false;
			out += "{\"build_id\":" + quote(std::to_string(build.build))
			    + ",\"published_at\":" + quote(build.published)
			    + ",\"depots\":" + depotsJson(build.depots) + '}';
		}
		out += "]}";
	}
	out += "],\"shared_depot_histories\":[";
	bool firstShared = true;
	for (const SharedDepotHistory& history : shared)
	{
		if (!firstShared) out += ',';
		firstShared = false;
		out += "{\"depot_id\":" + quote(std::to_string(history.depot))
		    + ",\"owner_appid\":" + quote(std::to_string(history.owner))
		    + ",\"category\":" + quote(history.category) + ",\"manifests\":[";
		bool firstManifest = true;
		for (const auto& manifest : history.manifests)
		{
			if (!firstManifest) out += ',';
			firstManifest = false;
			out += "{\"manifest_id\":" + quote(std::to_string(manifest.gid))
			    + ",\"seen_at\":" + quote(manifest.seen) + '}';
		}
		out += "]}";
	}
	return out + "],\"availability_policy\":{\"dlc_release_between_base_builds\":"
	    "\"preceding_base_build\"}}";
}

void saveHistory(uint32_t appid, const std::string& encoded)
{
	const std::filesystem::path path = historyPath(appid);
	std::error_code error;
	std::filesystem::create_directories(path.parent_path(), error);
	if (error) throw std::runtime_error("cannot create manifest history directory");
	const auto temporary = path.string() + ".tmp";
	{
		std::ofstream output(temporary, std::ios::trunc);
		if (!output) throw std::runtime_error("cannot create manifest history cache");
		output << encoded << '\n';
		output.flush();
		if (!output) throw std::runtime_error("cannot write manifest history cache");
	}
	if (::chmod(temporary.c_str(), 0600) != 0
	    || ::rename(temporary.c_str(), path.c_str()) != 0)
	{
		::unlink(temporary.c_str());
		throw std::runtime_error("cannot install manifest history cache");
	}
}

std::string readFile(const std::string& path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input) throw std::runtime_error("cannot read " + path);
	return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string sha256(const std::string& value)
{
	unsigned char digest[SHA256_DIGEST_LENGTH];
	SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest);
	static constexpr char hex[] = "0123456789abcdef";
	std::string out;
	out.reserve(SHA256_DIGEST_LENGTH * 2);
	for (const unsigned char byte : digest)
	{
		out += hex[byte >> 4];
		out += hex[byte & 15];
	}
	return out;
}

uint64_t documentRevision(const std::string& value)
{
	// Thirteen hex digits fit inside JSON's exactly representable integer range.
	return std::stoull(sha256(value).substr(0, 13), nullptr, 16);
}

std::string yamlString(const std::string& value)
{
	std::string out = "\"";
	for (const char c : value)
	{
		if (c == '"' || c == '\\') out += '\\';
		out += c;
	}
	return out + '"';
}

std::string yamlScalarJson(const std::string& raw, Json::Kind kind)
{
	const std::string value = trim(raw);
	if (kind == Json::Bool)
	{
		if (value == "yes" || value == "true") return "true";
		if (value == "no" || value == "false") return "false";
		throw std::runtime_error("invalid boolean in settings document");
	}
	if (kind == Json::Number)
	{
		if (!decimal(value, INT32_MAX)) throw std::runtime_error("invalid integer in settings document");
		return value;
	}
	if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
	{
		std::string decoded;
		for (size_t i = 1; i + 1 < value.size(); ++i)
		{
			if (value[i] == '\\' && i + 2 < value.size()) ++i;
			decoded += value[i];
		}
		return quote(decoded);
	}
	return quote(value);
}

struct Setting
{
	const char* key;
	Json::Kind kind;
	const char* defaultJson;
};

const std::vector<Setting>& settings()
{
	static const std::vector<Setting> values = {
		{"PlayNotOwnedGames", Json::Bool, "false"},
		{"DisableFamilyShareLock", Json::Bool, "true"},
		{"DisableParentalRestrictions", Json::Bool, "false"},
		{"Notifications", Json::Bool, "true"},
		{"NotifyInit", Json::Bool, "true"},
		{"DisableCloud", Json::Bool, "true"},
		{"Achievements", Json::Bool, "true"},
		{"DisableUpdates", Json::Bool, "true"},
		{"FakeEmail", Json::String, "\"\""},
		{"FakeWalletBalance", Json::Number, "0"},
		{"AutoFilterList", Json::Bool, "true"},
		{"UseWhitelist", Json::Bool, "false"},
		{"WarnHashMissmatch", Json::Bool, "false"},
		{"API", Json::Bool, "true"},
		{"ExtendedLogging", Json::Bool, "false"},
		{"LogLevel", Json::Number, "2"},
		{"MaxSchemaTries", Json::Number, "10"},
		{"DumpClientInterfaces", Json::Bool, "false"},
		{"AchievementOwnerId", Json::String, "\"76561198028121353\""},
	};
	return values;
}

std::map<std::string, std::string> topLevelScalars(const std::vector<std::string>& lines)
{
	std::map<std::string, std::string> result;
	const std::regex scalarLine(R"(^([A-Za-z][A-Za-z0-9]*):[ \t]*(.*)$)");
	for (const std::string& line : lines)
	{
		std::smatch match;
		if (std::regex_match(line, match, scalarLine)) result[match[1]] = match[2];
	}
	return result;
}

std::string settingsValuesJson(const std::vector<std::string>& lines)
{
	const auto scalars = topLevelScalars(lines);
	std::string out = "{";
	bool first = true;
	for (const Setting& setting : settings())
	{
		if (!first) out += ',';
		first = false;
		const auto found = scalars.find(setting.key);
		out += quote(setting.key) + ':'
		    + (found == scalars.end() || trim(found->second).empty()
		       ? setting.defaultJson : yamlScalarJson(found->second, setting.kind));
	}
	out += ",\"AchievementOwners\":{";
	bool firstOwner = true;
	const std::regex ownerLine(R"(^[ \t]{2}([0-9]+):[ \t]*[\"']?([0-9]+)[\"']?[ \t]*$)");
	bool inOwners = false;
	for (const std::string& line : lines)
	{
		if (line == "AchievementOwners:") { inOwners = true; continue; }
		if (inOwners && !line.empty() && line[0] != ' ' && line[0] != '\t') break;
		if (!inOwners) continue;
		std::smatch match;
		if (!std::regex_match(line, match, ownerLine)
		    || !decimal(match[1], UINT32_MAX) || !decimal(match[2], UINT64_MAX))
			continue;
		if (!firstOwner) out += ',';
		firstOwner = false;
		out += quote(match[1]) + ':' + quote(match[2]);
	}
	return out + "}}";
}

std::string settingsState(const std::string& content)
{
	std::istringstream input(content);
	std::vector<std::string> lines;
	for (std::string line; std::getline(input, line);) lines.push_back(line);
	return "{\"revision\":" + std::to_string(documentRevision(content))
	    + ",\"values\":" + settingsValuesJson(lines) + '}';
}

std::string settingYaml(const Json& value)
{
	switch (value.kind)
	{
		case Json::Bool: return value.boolean ? "yes" : "no";
		case Json::Number: return value.text;
		case Json::String: return yamlString(value.text);
		default: throw std::runtime_error("setting value is not scalar");
	}
}

void atomicWrite(const std::string& path, const std::vector<std::string>& lines)
{
	struct stat original{};
	if (::stat(path.c_str(), &original) != 0) throw std::runtime_error("cannot stat " + path);
	const std::string temporary = path + ".slssteam-control.tmp";
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output) throw std::runtime_error("cannot create temporary settings document");
		for (const auto& line : lines) output << line << '\n';
		output.flush();
		if (!output) throw std::runtime_error("failed writing settings document");
	}
	if (::chmod(temporary.c_str(), original.st_mode & 07777) != 0
	    || ::rename(temporary.c_str(), path.c_str()) != 0)
	{
		::unlink(temporary.c_str());
		throw std::runtime_error("atomic settings replacement failed");
	}
}

std::string updateSettings(const Json& payload)
{
	const Json* revision = payload.get("base_revision");
	const Json* values = payload.get("values");
	if (!revision || revision->kind != Json::Number || !values || values->kind != Json::Object)
		throw std::runtime_error("settings.set requires base_revision and values");
	const std::string path = configPath();
	const std::string current = readFile(path);
	if (!decimal(revision->text, UINT64_MAX)
	    || std::stoull(revision->text) != documentRevision(current))
		throw std::runtime_error("settings document revision conflict");

	std::istringstream input(current);
	std::vector<std::string> lines;
	for (std::string line; std::getline(input, line);) lines.push_back(line);
	const auto known = settings();
	for (const auto& [key, value] : values->object)
	{
		if (key == "AchievementOwners")
		{
			if (value.kind != Json::Object || value.object.size() > 100000)
				throw std::runtime_error("AchievementOwners must be a bounded object");
			size_t begin = lines.size(), end = lines.size();
			for (size_t i = 0; i < lines.size(); ++i)
				if (lines[i] == "AchievementOwners:")
				{
					begin = i;
					end = i + 1;
					while (end < lines.size()
					       && (lines[end].empty() || lines[end][0] == ' ' || lines[end][0] == '\t'))
						++end;
					break;
				}
			std::vector<std::string> block{"AchievementOwners:"};
			for (const auto& [appid, owner] : value.object)
			{
				if (!decimal(appid, UINT32_MAX) || owner.kind != Json::String
				    || !decimal(owner.text, UINT64_MAX))
					throw std::runtime_error("invalid AchievementOwners entry");
				block.push_back("  " + appid + ": \"" + owner.text + "\"");
			}
			if (begin == lines.size()) lines.insert(lines.end(), block.begin(), block.end());
			else
			{
				lines.erase(lines.begin() + static_cast<long>(begin), lines.begin() + static_cast<long>(end));
				lines.insert(lines.begin() + static_cast<long>(begin), block.begin(), block.end());
			}
			continue;
		}
		const auto spec = std::find_if(known.begin(), known.end(),
		    [&](const Setting& item) { return key == item.key; });
		if (spec == known.end() || value.kind != spec->kind)
			throw std::runtime_error("unknown or invalid setting: " + key);
		if (value.kind == Json::String && value.text.size() > 4096)
			throw std::runtime_error("setting string exceeds 4096 bytes");
		if (value.kind == Json::String && key == "AchievementOwnerId"
		    && !decimal(value.text, UINT64_MAX))
			throw std::runtime_error("invalid AchievementOwnerId");
		if (value.kind == Json::Number)
		{
			uint64_t maximum = INT32_MAX;
			if (key == "LogLevel") maximum = 6;
			else if (key == "MaxSchemaTries") maximum = 1000;
			if (!decimal(value.text, maximum))
				throw std::runtime_error("setting integer is outside its declared range");
		}
		bool replaced = false;
		const std::string prefix = key + ":";
		for (std::string& line : lines)
			if (line.rfind(prefix, 0) == 0)
			{
				line = prefix + " " + settingYaml(value);
				replaced = true;
				break;
			}
		if (!replaced) lines.push_back(prefix + " " + settingYaml(value));
	}
	atomicWrite(path, lines);
	return settingsState(readFile(path));
}

std::string isoTime()
{
	const auto now = std::chrono::system_clock::now();
	const std::time_t time = std::chrono::system_clock::to_time_t(now);
	std::tm utc{};
	gmtime_r(&time, &utc);
	char buffer[32];
	std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
	return buffer;
}

std::string canonicalJson(const Json& value)
{
	switch (value.kind)
	{
		case Json::Null: return "null";
		case Json::Bool: return value.boolean ? "true" : "false";
		case Json::Number: return value.text;
		case Json::String: return quote(value.text);
		case Json::Array:
		{
			std::string out = "[";
			for (size_t i = 0; i < value.array.size(); ++i)
			{
				if (i) out += ',';
				out += canonicalJson(value.array[i]);
			}
			return out + ']';
		}
		case Json::Object:
		{
			std::string out = "{";
			bool first = true;
			for (const auto& [key, child] : value.object)
			{
				if (!first) out += ',';
				first = false;
				out += quote(key) + ':' + canonicalJson(child);
			}
			return out + '}';
		}
	}
	throw std::runtime_error("invalid JSON kind");
}

std::string manifestPackStatus(uint32_t appid)
{
	const Pins pins = loadPins(configPath());
	const auto it = pins.find(appid);
	if (it == pins.end())
		return "{\"app_id\":" + quote(std::to_string(appid))
		    + ",\"installed\":false,\"depots\":{}}";
	return "{\"app_id\":" + quote(std::to_string(appid))
	    + ",\"installed\":true,\"build_id\":" + quote(std::to_string(it->second.build))
	    + ",\"depots\":" + depotsJson(it->second.depots) + '}';
}

std::string healthEvidence();

std::string featureStatus(const Json& payload)
{
	const Json evidence = Parser(healthEvidence()).parse();
	const bool ready = scalar(evidence.get("status")) == "ready";
	const Json* requested = payload.get("feature");
	const std::string wanted = requested ? scalar(requested) : "";
	const std::vector<std::string> features = {
	    "added-games", "manifest-pinning", "achievement-schemas",
	    "compatibility-tools", "steamstub-ticket"};
	if (g_lastFeatureReady != static_cast<int>(ready))
	{
		g_lastFeatureReady = static_cast<int>(ready);
		for (const std::string& feature : features)
			g_pendingEvents.push_back(
			    "{\"v\":1,\"t\":\"evt\",\"method\":\"feature.changed\",\"payload\":{\"id\":"
			    + quote(feature) + ",\"status\":"
			    + quote(ready ? "available" : "unavailable")
			    + ",\"observed_at\":" + quote(isoTime())
			    + (ready ? "" : ",\"code\":\"no-live-evidence\",\"detail\":\"Steam hooks have no current readiness evidence\"")
			    + "}}" );
	}
	std::string out = "{\"features\":[";
	bool first = true;
	for (const std::string& feature : features)
	{
		if (!wanted.empty() && wanted != feature) continue;
		if (!first) out += ',';
		first = false;
		out += "{\"id\":" + quote(feature) + ",\"status\":"
		    + quote(ready ? "available" : "unavailable")
		    + ",\"observed_at\":" + quote(isoTime());
		if (!ready) out += ",\"code\":\"no-live-evidence\",\"detail\":\"Steam hooks have no current readiness evidence\"";
		out += '}';
	}
	return out + "]}";
}

std::string dispatch(const std::string& method, const Json& payload);

std::string healthEvidence()
{
	const char* configured = std::getenv("TSUKI_RONIN_RUNTIME_DIR");
	const std::filesystem::path directory =
	    configured && *configured ? configured : "/tmp";
	for (const auto& item : std::filesystem::directory_iterator(directory))
	{
		const std::string name = item.path().filename();
		const std::string prefix = ".slssteam-ronin.ready.";
		if (name.rfind(prefix, 0) != 0 || !decimal(name.substr(prefix.size()), INT32_MAX))
			continue;
		const int fd = ::open(item.path().c_str(), O_RDONLY | O_CLOEXEC);
		if (fd < 0) continue;
		const bool heldByProducer = ::flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
		if (!heldByProducer) { ::flock(fd, LOCK_UN); ::close(fd); continue; }
		const std::string marker = readFile(item.path());
		::close(fd);
		Json record;
		try { record = Parser(marker).parse(); } catch (...) { continue; }
		const Json* hash = record.get("steamclient_sha256");
		if (!hash || hash->kind != Json::String
		    || !std::regex_match(hash->text, std::regex("[0-9a-f]{64}"))) continue;
		const std::string pid = name.substr(prefix.size());
		std::ifstream stat("/proc/" + pid + "/stat");
		std::string statLine;
		if (!std::getline(stat, statLine)) continue;
		const auto close = statLine.rfind(')');
		std::istringstream fields(close == std::string::npos ? "" : statLine.substr(close + 2));
		std::string field, start;
		for (unsigned index = 3; index <= 22 && fields >> field; ++index)
			if (index == 22) start = field;
		if (start.empty()) continue;
		return "{\"module_id\":\"slsteam\",\"component\":\"steam-hooks\","
		    "\"producer\":\"slssteam-control\",\"carrier\":\"companion-export\","
		    "\"observed_at\":" + quote(isoTime()) + ",\"process_instance\":"
		    + quote(pid + ":" + start) + ",\"compatibility_subject\":\"steam-build\","
		    "\"compatibility_value\":\"sha256:" + hash->text + "\",\"status\":\"ready\","
		    "\"digest\":\"sha256:" + sha256(marker) + "\"}";
	}
	const std::string zeros(64, '0');
	return "{\"module_id\":\"slsteam\",\"component\":\"steam-hooks\","
	    "\"producer\":\"slssteam-control\",\"carrier\":\"companion-export\","
	    "\"observed_at\":" + quote(isoTime()) + ",\"process_instance\":\"none\","
	    "\"compatibility_subject\":\"steam-build\",\"compatibility_value\":\"sha256:"
	    + zeros + "\",\"status\":\"failed\",\"code\":\"no-live-evidence\","
	    "\"detail\":\"No live SLSsteam hook readiness producer was found\","
	    "\"digest\":\"sha256:" + zeros + "\"}";
}

std::string dispatch(const std::string& method, const Json& payload)
{
	if (method == "feature.status") return featureStatus(payload);
	if (method == "manifest-pack.status")
		return manifestPackStatus(decimal32(payload.get("app_id"), "app id"));
	if (method == "manifest-pack.inspect")
	{
		const uint32_t appid = decimal32(payload.get("app_id"), "app id");
		const std::string action = requiredString(payload.get("action"), "action");
		std::string plan;
		if (action == "install")
		{
			const uint32_t build = decimal32(payload.get("build_id"), "build id");
			const Json request = Parser("{\"appid\":" + quote(std::to_string(appid))
			    + ",\"build_id\":" + quote(std::to_string(build)) + '}').parse();
			const Json resolved = Parser(dispatch("pins.history.resolve", request)).parse();
			const Json* depots = resolved.get("depots");
			if (!depots || depots->kind != Json::Object)
				throw std::runtime_error("resolved manifest pack has no depots");
			plan = "{\"action\":\"install\",\"app_id\":"
			    + quote(std::to_string(appid)) + ",\"build_id\":"
			    + quote(std::to_string(build)) + ",\"depots\":"
			    + canonicalJson(*depots) + '}';
		}
		else if (action == "remove")
		{
			const Json status = Parser(manifestPackStatus(appid)).parse();
			const Json* depots = status.get("depots");
			plan = "{\"action\":\"remove\",\"app_id\":"
			    + quote(std::to_string(appid)) + ",\"depots\":"
			    + (depots ? canonicalJson(*depots) : "{}") + '}';
		}
		else throw std::runtime_error("unsupported manifest-pack action");
		const std::string digest = sha256(plan);
		return "{\"plan\":" + plan + ",\"plan_digest\":" + quote(digest)
		    + ",\"summary\":" + quote(
		        action + " manifest pack for Steam app " + std::to_string(appid))
		    + ",\"effects\":[" + quote("Steam app " + std::to_string(appid)) + "]}";
	}
	if (method == "manifest-pack.install" || method == "manifest-pack.remove")
	{
		const uint32_t appid = decimal32(payload.get("app_id"), "app id");
		const Json* plan = payload.get("plan");
		const Json* authority = payload.get("authority");
		if (!plan || plan->kind != Json::Object || !authority || authority->kind != Json::Object)
			throw std::runtime_error("host operation authority is required");
		for (const char* field : {"operation_id", "confirmation_handle", "grant_id", "resource_handle"})
			(void)requiredString(authority->get(field), field);
		const std::string operationId = requiredString(
		    authority->get("operation_id"), "operation id");
		const Json* phaseValue = authority->get("phase");
		const std::string phase = phaseValue ? requiredString(phaseValue, "authority phase") : "commit";
		if (phase == "rollback")
		{
			const auto pending = g_pendingManifestPacks.find(operationId);
			if (pending == g_pendingManifestPacks.end())
				throw std::runtime_error("manifest-pack rollback state is unavailable");
			savePins(pending->second.path, pending->second.before);
			g_pendingManifestPacks.erase(pending);
			return "{\"verified\":true}";
		}
		if (phase == "finalize")
		{
			const auto pending = g_pendingManifestPacks.find(operationId);
			if (pending == g_pendingManifestPacks.end())
				throw std::runtime_error("manifest-pack finalize state is unavailable");
			if (!pending->second.event.empty())
				g_pendingEvents.push_back(pending->second.event);
			g_pendingManifestPacks.erase(pending);
			return "{\"verified\":true}";
		}
		if (phase != "commit") throw std::runtime_error("invalid manifest-pack authority phase");
		const std::string digest = requiredString(payload.get("plan_digest"), "plan digest");
		if (requiredString(authority->get("plan_digest"), "authority plan digest") != digest
		    || sha256(canonicalJson(*plan)) != digest)
			throw std::runtime_error("confirmed manifest-pack plan changed");
		const std::string action = requiredString(plan->get("action"), "plan action");
		const std::string expected = method == "manifest-pack.install" ? "install" : "remove";
		if (action != expected || decimal32(plan->get("app_id"), "plan app id") != appid)
			throw std::runtime_error("manifest-pack plan does not match operation");

		const auto path = configPath();
		Pins pins = loadPins(path);
		const Pins before = pins;
		if (g_pendingManifestPacks.contains(operationId))
			throw std::runtime_error("manifest-pack operation id is already active");
		g_pendingManifestPacks.emplace(operationId, PendingManifestPack{path, before, {}});
		uint32_t build = 0;
		try
		{
			if (action == "install")
			{
				build = decimal32(plan->get("build_id"), "plan build id");
				AppPin pin; pin.locked = true; pin.build = build;
				pin.depots = depotMap(plan->get("depots"), "plan depots", false);
				if (pin.depots.empty()) throw std::runtime_error("manifest pack has no depots");
				pins[appid] = std::move(pin);
			}
			else pins.erase(appid);
			savePins(path, pins);
			const Pins verified = loadPins(path);
			const bool present = verified.contains(appid);
			if ((action == "install") != present
			    || (present && (verified.at(appid).build != build
			      || verified.at(appid).depots != pins.at(appid).depots)))
				throw std::runtime_error("manifest-pack verification failed");
		}
		catch (...)
		{
			try { savePins(path, before); } catch (...) {}
			g_pendingManifestPacks.erase(operationId);
			throw;
		}
		const std::map<uint32_t, uint64_t> depots =
		    action == "install" ? pins.at(appid).depots : std::map<uint32_t, uint64_t>{};
		g_pendingManifestPacks.at(operationId).event =
		    "{\"v\":1,\"t\":\"evt\",\"method\":\"manifest-pack.changed\",\"payload\":{\"action\":"
		    + quote(action) + ",\"app_id\":" + quote(std::to_string(appid))
		    + (build ? ",\"build_id\":" + quote(std::to_string(build)) : "")
		    + ",\"observed_at\":" + quote(isoTime()) + "}}";
		return "{\"verified\":true,\"action\":" + quote(action)
		    + ",\"app_id\":" + quote(std::to_string(appid))
		    + (build ? ",\"build_id\":" + quote(std::to_string(build)) : "")
		    + ",\"depots\":" + depotsJson(depots)
		    + ",\"rollback_available\":true}";
	}
	if (method == "settings.get") return settingsState(readFile(configPath()));
	if (method == "settings.set") return updateSettings(payload);
	if (method == "health.evidence.get") return healthEvidence();
	if (method == "pins.history.import")
	{
		uint32_t appid = 0;
		std::string encoded;
		if (payload.get("apps"))
		{
			const auto apps = reconstructAppHistories(payload, appid);
			const auto shared = reconstructSharedDepotHistories(payload, appid, apps);
			encoded = appHistoriesJson(appid, apps, shared);
		}
		else
			encoded = historyJson(appid, reconstructHistory(payload, appid));
		saveHistory(appid, encoded);
		return encoded;
	}
	if (method == "pins.history.list")
	{
		const uint32_t appid = decimal32(payload.get("appid"), "appid");
		std::ifstream input(historyPath(appid));
		if (!input) throw std::runtime_error("no cached manifest history for app");
		return std::string(std::istreambuf_iterator<char>(input),
		                   std::istreambuf_iterator<char>());
	}
	if (method == "pins.history.resolve")
	{
		const uint32_t appid = decimal32(payload.get("appid"), "appid");
		const uint32_t wanted = decimal32(payload.get("build_id"), "build id");
		std::ifstream input(historyPath(appid));
		if (!input) throw std::runtime_error("no cached manifest history for app");
		const Json cache = Parser(std::string(std::istreambuf_iterator<char>(input),
		                                     std::istreambuf_iterator<char>())).parse();
		if (const Json* apps = cache.get("apps"))
		{
			if (apps->kind != Json::Array)
				throw std::runtime_error("invalid app history cache");
			std::string targetTime;
			std::string upperTime;
			std::map<uint32_t, uint64_t> resolved;
			for (const Json& app : apps->array)
			{
				if (scalar(app.get("appid")) != std::to_string(appid)) continue;
				const Json* builds = app.get("builds");
				if (!builds || builds->kind != Json::Array)
					throw std::runtime_error("invalid root build history");
				for (size_t index = 0; index < builds->array.size(); ++index)
				{
					const Json& build = builds->array[index];
					if (decimal32(build.get("build_id"), "cached build id") != wanted) continue;
					targetTime = requiredString(build.get("published_at"), "cached build timestamp");
					if (index > 0) upperTime = requiredString(
					    builds->array[index - 1].get("published_at"), "cached build timestamp");
					resolved = depotMap(build.get("depots"), "cached depots", false);
					break;
				}
			}
			if (targetTime.empty())
				throw std::runtime_error("build is not present in cached root history");
			for (const Json& app : apps->array)
			{
				if (scalar(app.get("appid")) == std::to_string(appid)) continue;
				const Json* builds = app.get("builds");
				if (!builds || builds->kind != Json::Array)
					throw std::runtime_error("invalid related build history");
				for (const Json& build : builds->array)
				{
					const std::string published = requiredString(
					    build.get("published_at"), "cached build timestamp");
					if (!upperTime.empty() && published >= upperTime) continue;
					for (const auto& [depot, gid] :
					     depotMap(build.get("depots"), "cached depots", false))
					{
						if (!resolved.emplace(depot, gid).second)
							throw std::runtime_error("depot appears in multiple app histories");
					}
					break;
				}
			}
			// Keep shared/runtime history as observation data, but never turn it
			// into a game pin. The owner app controls one global installation;
			// pinning it for a dependent game conflicts with that owner's
			// public update and every other game using the same runtime.
			return "{\"appid\":" + quote(std::to_string(appid))
			    + ",\"build_id\":" + quote(std::to_string(wanted))
			    + ",\"published_at\":" + quote(targetTime)
			    + ",\"depots\":" + depotsJson(resolved) + '}';
		}
		const Json* builds = cache.get("builds");
		if (!builds || builds->kind != Json::Array)
			throw std::runtime_error("invalid manifest history cache");
		for (const Json& build : builds->array)
		{
			if (decimal32(build.get("build_id"), "cached build id") != wanted) continue;
			return "{\"appid\":" + quote(std::to_string(appid))
			    + ",\"build_id\":" + quote(std::to_string(wanted))
			    + ",\"depots\":" + depotsJson(
			        depotMap(build.get("depots"), "cached depots")) + '}';
		}
		throw std::runtime_error("build is not present in cached manifest history");
	}
	const auto path = configPath();
	Pins pins = loadPins(path);
	if (method == "pins.list") return catalogJson(pins);
	if (method == "pins.clear")
	{
		const std::string appText = scalar(payload.get("appid"));
		if (!decimal(appText, UINT32_MAX)) throw std::runtime_error("invalid appid");
		pins.erase(static_cast<uint32_t>(std::stoul(appText)));
		savePins(path, pins);
		return catalogJson(pins);
	}
	if (method == "pins.set")
	{
		const std::string appText = scalar(payload.get("appid"));
		const std::string buildText = scalar(payload.get("build_id"));
		if (!decimal(appText, UINT32_MAX) || !decimal(buildText, UINT32_MAX))
			throw std::runtime_error("invalid appid or build id");
		AppPin pin;
		if (const Json* locked = payload.get("locked"))
		{
			if (locked->kind != Json::Bool) throw std::runtime_error("locked must be boolean");
			pin.locked = locked->boolean;
		}
		const Json* depots = payload.get("depots");
		if (!depots || depots->kind != Json::Object) throw std::runtime_error("depots must be an object");
		for (const auto& [depotText, gidValue] : depots->object)
		{
			const std::string gidText = scalar(&gidValue);
			if (!decimal(depotText, UINT32_MAX) || !decimal(gidText, UINT64_MAX))
				throw std::runtime_error("invalid depot id or manifest gid");
			pin.depots[static_cast<uint32_t>(std::stoul(depotText))] = std::stoull(gidText);
		}
		if (pin.depots.empty()) throw std::runtime_error("at least one depot pin is required");
		pin.build = static_cast<uint32_t>(std::stoul(buildText));
		pins[static_cast<uint32_t>(std::stoul(appText))] = std::move(pin);
		savePins(path, pins);
		return catalogJson(pins);
	}
	throw std::runtime_error("unsupported export: " + method);
}

bool readAll(int fd, void* data, size_t size)
{
	auto* out = static_cast<unsigned char*>(data);
	while (size)
	{
		const ssize_t count = ::read(fd, out, size);
		if (count == 0) return false;
		if (count < 0) { if (errno == EINTR) continue; return false; }
		out += count; size -= static_cast<size_t>(count);
	}
	return true;
}

bool writeAll(int fd, const void* data, size_t size)
{
	const auto* in = static_cast<const unsigned char*>(data);
	while (size)
	{
		const ssize_t count = ::write(fd, in, size);
		if (count < 0) { if (errno == EINTR) continue; return false; }
		in += count; size -= static_cast<size_t>(count);
	}
	return true;
}

void sendFrame(int fd, const std::string& payload)
{
	const uint32_t length = static_cast<uint32_t>(payload.size());
	unsigned char prefix[4] = {
		static_cast<unsigned char>(length), static_cast<unsigned char>(length >> 8),
		static_cast<unsigned char>(length >> 16), static_cast<unsigned char>(length >> 24)
	};
	if (!writeAll(fd, prefix, sizeof(prefix)) || !writeAll(fd, payload.data(), payload.size()))
		throw std::runtime_error("socket write failed");
}

std::string response(const Json& message, bool ok, const std::string& payload)
{
	const Json* id = message.get("id");
	const std::string idText = id ? (id->kind == Json::String ? quote(id->text) : id->text) : "null";
	if (ok) return "{\"v\":1,\"t\":\"res\",\"id\":" + idText + ",\"payload\":" + payload + "}";
	return "{\"v\":1,\"t\":\"err\",\"id\":" + idText
	    + ",\"code\":\"E_BAD_MESSAGE\",\"error\":{\"message\":" + quote(payload) + "}}";
}
}

int main()
{
	try
	{
		const char* socketPath = std::getenv("TSUKI_RONIN_SOCKET");
		if (!socketPath || !*socketPath) throw std::runtime_error("TSUKI_RONIN_SOCKET is unset");
		const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0) throw std::runtime_error("socket creation failed");
		sockaddr_un address{}; address.sun_family = AF_UNIX;
		if (std::strlen(socketPath) >= sizeof(address.sun_path)) throw std::runtime_error("socket path too long");
		std::strcpy(address.sun_path, socketPath);
		for (unsigned attempt = 0; ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0; ++attempt)
		{
			if (attempt >= 200) throw std::runtime_error("could not connect to Tsuki");
			::usleep(10000);
		}
		for (;;)
		{
			unsigned char prefix[4];
			if (!readAll(fd, prefix, sizeof(prefix))) break;
			const uint32_t length = static_cast<uint32_t>(prefix[0])
			    | (static_cast<uint32_t>(prefix[1]) << 8)
			    | (static_cast<uint32_t>(prefix[2]) << 16)
			    | (static_cast<uint32_t>(prefix[3]) << 24);
			if (length > 1024 * 1024) throw std::runtime_error("oversized frame");
			std::string raw(length, '\0');
			if (!readAll(fd, raw.data(), raw.size())) break;
			Json message = Parser(raw).parse();
			const Json* type = message.get("t");
			const Json* method = message.get("method");
			if (!type || type->kind != Json::String)
			{
				sendFrame(fd, response(message, false, "missing message type"));
				continue;
			}
			if (type->text == "ping")
			{
				const Json* id = message.get("id");
				const std::string idText = id
				    ? (id->kind == Json::String ? quote(id->text) : id->text)
				    : "null";
				sendFrame(fd, "{\"v\":1,\"t\":\"pong\",\"id\":" + idText + "}");
				continue;
			}
			if (!method || method->kind != Json::String)
			{
				sendFrame(fd, response(message, false, "missing method"));
				continue;
			}
			if (type->text == "ctl")
			{
				sendFrame(fd, response(message, true, "{}"));
				if (method->text == "shutdown") break;
			}
			else if (type->text == "req")
			{
				try
				{
					const Json empty;
					const Json* payload = message.get("payload");
					sendFrame(fd, response(message, true,
					    dispatch(method->text, payload ? *payload : empty)));
					for (const std::string& event : g_pendingEvents) sendFrame(fd, event);
					g_pendingEvents.clear();
				}
				catch (const std::exception& error)
				{
					sendFrame(fd, response(message, false, error.what()));
				}
			}
		}
		::close(fd);
		return 0;
	}
	catch (const std::exception& error)
	{
		const std::string message = std::string(error.what()) + "\n";
		(void)writeAll(STDERR_FILENO, message.data(), message.size());
		return 1;
	}
}
