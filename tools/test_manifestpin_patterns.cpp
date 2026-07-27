// Regression test for the two cooperating manifest-pin patterns.

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

static std::vector<size_t> find(
    const std::string& bytes, const std::vector<Byte>& pattern)
{
	std::vector<size_t> matches;
	for (size_t pos = 0; pos + pattern.size() <= bytes.size(); ++pos)
	{
		bool equal = true;
		for (size_t i = 0; i < pattern.size(); ++i)
			if (!pattern[i].wildcard
			    && static_cast<uint8_t>(bytes[pos + i]) != pattern[i].value)
			{
				equal = false;
				break;
			}
		if (equal) matches.push_back(pos);
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

	const auto builder = find(bytes, parse(
	    "E8 ? ? ? ? 05 ? ? ? ? 55 89 E5 57 56 53 81 EC 8C 04 00 00 "
	    "8B 55 10 8B 7D 0C 89 85 A0 FB FF FF 8B 45 08"));
	const auto target = find(bytes, parse(
	    "E8 ? ? ? ? 8B 46 08 8D 9F ? ? ? ? 83 C4 10 89 5D ? "
	    "8B 10 8B 52 4C 39 DA 0F 85"));
	if (builder.size() != 1 || target.size() != 1)
	{
		std::cerr << "expected one builder and target call, got "
		          << builder.size() << " and " << target.size() << "\n";
		return 1;
	}

	int32_t relative = 0;
	std::memcpy(&relative, bytes.data() + target[0] + 1, sizeof(relative));
	const size_t destination =
	    static_cast<size_t>(static_cast<int64_t>(target[0] + 5) + relative);
	if (destination != builder[0])
	{
		std::cerr << "target call does not resolve to builder\n";
		return 1;
	}
	std::cout << "manifest-pin target call uniquely resolves to builder\n";
}
