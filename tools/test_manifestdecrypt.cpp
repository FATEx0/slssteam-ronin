// Compatibility oracle for ManifestDecrypt. The encrypted/decrypted fixture
// pair and depot key come from SteamRE/SteamKit's DepotManifestFacts (LGPL-2.1).
// Matching every byte outside the signature section pins down AES handling,
// filename sorting/hashing, protobuf serialization, metadata and CRC.
//
// Build from the repository root:
//   g++ -std=c++20 -Iinclude tools/test_manifestdecrypt.cpp \
//       src/feats/manifestdecrypt.cpp -lcrypto -o /tmp/test_manifestdecrypt
//   /tmp/test_manifestdecrypt \
//       tools/fixtures/steamkit/depot_440_encrypted.manifest \
//       tools/fixtures/steamkit/depot_440_decrypted.manifest

#include "../src/feats/manifestdecrypt.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

static std::string readAll(const char* path)
{
	std::ifstream in(path, std::ios::binary);
	return {
	    std::istreambuf_iterator<char>(in),
	    std::istreambuf_iterator<char>()};
}

static size_t signatureOffset(const std::string& bytes)
{
	const std::string magic{"\x17\xb8\x81\x1b", 4};
	return bytes.find(magic);
}

static uint32_t readU32(const std::string& bytes, size_t pos)
{
	return static_cast<uint8_t>(bytes[pos])
	    | (static_cast<uint32_t>(static_cast<uint8_t>(bytes[pos + 1])) << 8)
	    | (static_cast<uint32_t>(static_cast<uint8_t>(bytes[pos + 2])) << 16)
	    | (static_cast<uint32_t>(static_cast<uint8_t>(bytes[pos + 3])) << 24);
}

int main(int argc, char** argv)
{
	if (argc != 3) return 2;
	const std::string encrypted = readAll(argv[1]);
	const std::string expected = readAll(argv[2]);
	const unsigned char keyBytes[32] = {
	    0x44, 0xCE, 0x5C, 0x52, 0x97, 0xA4, 0x15, 0xA1,
	    0xA6, 0xF6, 0x9C, 0x85, 0x60, 0x37, 0xA5, 0xA2,
	    0xFD, 0xD8, 0x2C, 0xD4, 0x74, 0xFA, 0x65, 0x9E,
	    0xDF, 0xB4, 0xD5, 0x9B, 0x2A, 0xBC, 0x55, 0xFC,
	};
	const std::string key{
	    reinterpret_cast<const char*>(keyBytes), sizeof(keyBytes)};
	std::string actual;
	std::string error;
	if (!ManifestDecrypt::normalizeForDepotcache(
	        encrypted, key, actual, error))
	{
		std::cerr << error << "\n";
		return 1;
	}
	const size_t actualSig = signatureOffset(actual);
	const size_t expectedSig = signatureOffset(expected);
	if (actualSig == std::string::npos || expectedSig == std::string::npos)
		return 1;
	if (actual.substr(0, actualSig) != expected.substr(0, expectedSig))
	{
		std::cerr << "payload/metadata mismatch\n";
		return 1;
	}
	const size_t actualTail = actualSig + 8 + readU32(actual, actualSig + 4);
	const size_t expectedTail =
	    expectedSig + 8 + readU32(expected, expectedSig + 4);
	if (actual.substr(actualTail) != expected.substr(expectedTail))
	{
		std::cerr << "post-signature mismatch\n";
		return 1;
	}
	std::cout << "SteamKit encrypted/decrypted fixture match\n";
}
