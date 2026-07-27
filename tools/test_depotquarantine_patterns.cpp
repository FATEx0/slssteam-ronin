// Verify that the optional DLC-quarantine callbacks remain uniquely
// identifiable in the current 32-bit steamclient.

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

struct Byte
{
	uint8_t value;
	bool wildcard;
};

static std::vector<Byte> parse(const char* pattern)
{
	std::vector<Byte> out;
	for (std::string token; *pattern;)
	{
		while (*pattern == ' ') ++pattern;
		if (!*pattern) break;
		const char* end = std::strchr(pattern, ' ');
		token.assign(pattern, end ? end : pattern + std::strlen(pattern));
		out.push_back({
		    static_cast<uint8_t>(
		        token == "?" ? 0 : std::stoul(token, nullptr, 16)),
		    token == "?"});
		pattern = end ? end : pattern + std::strlen(pattern);
	}
	return out;
}

static std::size_t count(
    const std::string& bytes, const std::vector<Byte>& pattern)
{
	std::size_t matches = 0;
	for (std::size_t pos = 0; pos + pattern.size() <= bytes.size(); ++pos)
	{
		bool equal = true;
		for (std::size_t i = 0; i < pattern.size(); ++i)
		{
			if (!pattern[i].wildcard
			    && static_cast<uint8_t>(bytes[pos + i]) != pattern[i].value)
			{
				equal = false;
				break;
			}
		}
		if (equal) ++matches;
	}
	return matches;
}

int main(int argc, char** argv)
{
	if (argc != 2) return 2;
	std::ifstream input(argv[1], std::ios::binary);
	const std::string bytes{
	    std::istreambuf_iterator<char>(input),
	    std::istreambuf_iterator<char>()};

	const auto stack = count(bytes, parse(
	    "55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 81 EC 7C 04 "
	    "00 00 8B 45 0C 8B 7D 08 89 85 90 FB FF FF 8B 45 10 89 "
	    "85 8C FB FF FF"));
	const auto reg = count(bytes, parse(
	    "55 89 E5 57 E8 ? ? ? ? 81 C7 ? ? ? ? 56 89 C6 53 81 EC "
	    "7C 04 00 00 8B 45 0C 89 95 90 FB FF FF 89 8D 8C FB FF "
	    "FF 89 85 94 FB FF FF"));

	if (stack > 1 || reg > 1 || stack + reg == 0)
	{
		std::cerr << "expected at least one unique chunk callback, got cdecl="
		          << stack << " regparm3=" << reg << "\n";
		return 1;
	}
	std::cout << "depot-quarantine callbacks resolve uniquely: cdecl="
	          << stack << " regparm3=" << reg << "\n";
}
