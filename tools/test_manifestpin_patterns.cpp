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

static size_t callDestination(const std::string& bytes, size_t call)
{
	int32_t relative = 0;
	std::memcpy(&relative, bytes.data() + call + 1, sizeof(relative));
	return static_cast<size_t>(static_cast<int64_t>(call + 5) + relative);
}

static bool hasPushLiteralBefore(
    const std::string& bytes, size_t call, uint8_t literal)
{
	const size_t begin = call > 16 ? call - 16 : 0;
	for (size_t i = begin; i + 1 < call; ++i)
		if (static_cast<uint8_t>(bytes[i]) == 0x6a
		    && static_cast<uint8_t>(bytes[i + 1]) == literal)
			return true;
	return false;
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

	if (callDestination(bytes, target[0]) != builder[0])
	{
		std::cerr << "target call does not resolve to builder\n";
		return 1;
	}

	size_t flag0 = 0;
	size_t flag1 = 0;
	for (size_t call = 0; call + 5 <= bytes.size(); ++call)
	{
		if (static_cast<uint8_t>(bytes[call]) != 0xe8
		    || callDestination(bytes, call) != builder[0])
			continue;
		flag0 += hasPushLiteralBefore(bytes, call, 0);
		flag1 += hasPushLiteralBefore(bytes, call, 1);
	}
	if (flag0 != 4 || flag1 != 4)
	{
		std::cerr << "expected four flag=0 and four flag=1 builder calls; found "
		          << flag0 << " and " << flag1 << '\n';
		return 1;
	}

	std::cout << "manifest-pin target and flag-pair assumptions are valid\n";
	return 0;
}
