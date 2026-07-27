// SPDX-License-Identifier: AGPL-3.0-only

#include "manifestdecrypt.hpp"

#include "base64/base64.hpp"

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace
{
	constexpr uint32_t kPayloadMagic = 0x71F617D0u;
	constexpr uint32_t kMetadataMagic = 0x1F4812BEu;
	constexpr uint32_t kEndMagic = 0x32C415ABu;

	struct Field
	{
		uint32_t number = 0;
		uint8_t wire = 0;
		uint64_t varint = 0;
		std::string bytes;
	};

	uint32_t readU32(const std::string& s, size_t pos)
	{
		return static_cast<uint8_t>(s[pos])
		    | (static_cast<uint32_t>(static_cast<uint8_t>(s[pos + 1])) << 8)
		    | (static_cast<uint32_t>(static_cast<uint8_t>(s[pos + 2])) << 16)
		    | (static_cast<uint32_t>(static_cast<uint8_t>(s[pos + 3])) << 24);
	}

	void putU32(std::string& out, uint32_t value)
	{
		for (unsigned i = 0; i < 4; ++i)
			out.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
	}

	bool readVarint(const std::string& s, size_t& pos, size_t end,
	                uint64_t& value)
	{
		value = 0;
		for (unsigned shift = 0; shift < 64 && pos < end; shift += 7)
		{
			const uint8_t byte = static_cast<uint8_t>(s[pos++]);
			value |= static_cast<uint64_t>(byte & 0x7f) << shift;
			if (!(byte & 0x80)) return true;
		}
		return false;
	}

	void putVarint(std::string& out, uint64_t value)
	{
		while (value >= 0x80)
		{
			out.push_back(static_cast<char>((value & 0x7f) | 0x80));
			value >>= 7;
		}
		out.push_back(static_cast<char>(value));
	}

	bool parseFields(const std::string& bytes, std::vector<Field>& fields)
	{
		size_t pos = 0;
		while (pos < bytes.size())
		{
			uint64_t key = 0;
			if (!readVarint(bytes, pos, bytes.size(), key) || !(key >> 3))
				return false;
			Field field;
			field.number = static_cast<uint32_t>(key >> 3);
			field.wire = static_cast<uint8_t>(key & 7);
			if (field.wire == 0)
			{
				if (!readVarint(bytes, pos, bytes.size(), field.varint))
					return false;
			}
			else if (field.wire == 1 || field.wire == 5)
			{
				const size_t len = field.wire == 1 ? 8 : 4;
				if (pos + len > bytes.size()) return false;
				field.bytes.assign(bytes, pos, len);
				pos += len;
			}
			else if (field.wire == 2)
			{
				uint64_t len = 0;
				if (!readVarint(bytes, pos, bytes.size(), len)
				    || len > bytes.size() - pos)
					return false;
				field.bytes.assign(bytes, pos, static_cast<size_t>(len));
				pos += static_cast<size_t>(len);
			}
			else
			{
				return false;
			}
			fields.push_back(std::move(field));
		}
		return true;
	}

	std::string encodeFields(const std::vector<Field>& fields)
	{
		std::string out;
		for (const auto& field : fields)
		{
			putVarint(out, (static_cast<uint64_t>(field.number) << 3)
			                    | field.wire);
			if (field.wire == 0)
			{
				putVarint(out, field.varint);
			}
			else if (field.wire == 2)
			{
				putVarint(out, field.bytes.size());
				out += field.bytes;
			}
			else
			{
				out += field.bytes;
			}
		}
		return out;
	}

	Field* findField(std::vector<Field>& fields, uint32_t number, uint8_t wire)
	{
		for (auto& field : fields)
			if (field.number == number && field.wire == wire) return &field;
		return nullptr;
	}

	void setVarint(std::vector<Field>& fields, uint32_t number, uint64_t value)
	{
		if (auto* field = findField(fields, number, 0))
		{
			field->varint = value;
			return;
		}
		fields.push_back({number, 0, value, {}});
	}

	void setBytes(std::vector<Field>& fields, uint32_t number,
	              const std::string& value)
	{
		if (auto* field = findField(fields, number, 2))
		{
			field->bytes = value;
			return;
		}
		fields.push_back({number, 2, 0, value});
	}

	bool aesDecryptName(const std::string& encoded, const std::string& key,
	                    std::string& clear)
	{
		std::string compact;
		compact.reserve(encoded.size());
		for (unsigned char c : encoded)
			if (!std::isspace(c)) compact.push_back(static_cast<char>(c));
		std::string encrypted;
		try
		{
			encrypted = std::string(base64::from_base64(compact));
		}
		catch (...)
		{
			return false;
		}
		if (key.size() != 32 || encrypted.size() < 32
		    || encrypted.size() % 16 != 0)
			return false;

		unsigned char iv[16]{};
		int produced = 0;
		int final = 0;
		EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
		if (!ctx) return false;
		bool ok =
		    EVP_DecryptInit_ex(ctx, EVP_aes_256_ecb(), nullptr,
		                       reinterpret_cast<const unsigned char*>(key.data()),
		                       nullptr) == 1
		    && EVP_CIPHER_CTX_set_padding(ctx, 0) == 1
		    && EVP_DecryptUpdate(
		           ctx, iv, &produced,
		           reinterpret_cast<const unsigned char*>(encrypted.data()), 16) == 1
		    && produced == 16
		    && EVP_DecryptFinal_ex(ctx, iv + produced, &final) == 1
		    && final == 0;
		EVP_CIPHER_CTX_free(ctx);
		if (!ok) return false;

		std::string decoded(encrypted.size(), '\0');
		ctx = EVP_CIPHER_CTX_new();
		if (!ctx) return false;
		produced = 0;
		final = 0;
		ok =
		    EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr,
		                       reinterpret_cast<const unsigned char*>(key.data()),
		                       iv) == 1
		    && EVP_DecryptUpdate(
		           ctx, reinterpret_cast<unsigned char*>(decoded.data()), &produced,
		           reinterpret_cast<const unsigned char*>(encrypted.data() + 16),
		           static_cast<int>(encrypted.size() - 16)) == 1
		    && EVP_DecryptFinal_ex(
		           ctx, reinterpret_cast<unsigned char*>(decoded.data()) + produced,
		           &final) == 1;
		EVP_CIPHER_CTX_free(ctx);
		if (!ok) return false;
		decoded.resize(static_cast<size_t>(produced + final));
		if (!decoded.empty() && decoded.back() == '\0') decoded.pop_back();
		for (char& c : decoded) if (c == '/') c = '\\';
		clear = std::move(decoded);
		return true;
	}

	std::string filenameSha(const std::string& name)
	{
		std::string lower = name;
		for (char& c : lower)
		{
			const unsigned char u = static_cast<unsigned char>(c);
			if (u >= 'A' && u <= 'Z') c = static_cast<char>(u + ('a' - 'A'));
		}
		unsigned char digest[SHA_DIGEST_LENGTH]{};
		SHA1(reinterpret_cast<const unsigned char*>(lower.data()),
		     lower.size(), digest);
		return std::string(
		    reinterpret_cast<const char*>(digest), sizeof(digest));
	}

	uint32_t crc32(const std::string& bytes)
	{
		uint32_t crc = 0xffffffffu;
		for (uint8_t byte : bytes)
		{
			crc ^= byte;
			for (unsigned bit = 0; bit < 8; ++bit)
				crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
		}
		return ~crc;
	}
}

bool ManifestDecrypt::normalizeForDepotcache(
    const std::string& input, const std::string& key, std::string& output,
    std::string& error)
{
	output.clear();
	error.clear();
	if (input.size() < 12)
	{
		error = "manifest is too short";
		return false;
	}

	struct Section { uint32_t magic; std::string bytes; };
	std::vector<Section> sections;
	size_t pos = 0;
	while (pos + 4 <= input.size())
	{
		const uint32_t magic = readU32(input, pos);
		pos += 4;
		if (magic == kEndMagic)
		{
			sections.push_back({magic, {}});
			if (pos != input.size())
			{
				error = "bytes after end marker";
				return false;
			}
			break;
		}
		if (pos + 4 > input.size())
		{
			error = "truncated section length";
			return false;
		}
		const uint32_t len = readU32(input, pos);
		pos += 4;
		if (len > input.size() - pos)
		{
			error = "truncated section";
			return false;
		}
		sections.push_back({magic, input.substr(pos, len)});
		pos += len;
	}
	if (sections.empty() || sections.back().magic != kEndMagic)
	{
		error = "missing end marker";
		return false;
	}

	auto payloadIt = std::find_if(
	    sections.begin(), sections.end(),
	    [](const Section& s) { return s.magic == kPayloadMagic; });
	auto metadataIt = std::find_if(
	    sections.begin(), sections.end(),
	    [](const Section& s) { return s.magic == kMetadataMagic; });
	if (payloadIt == sections.end() || metadataIt == sections.end())
	{
		error = "required section missing";
		return false;
	}

	std::vector<Field> metadata;
	if (!parseFields(metadataIt->bytes, metadata))
	{
		error = "invalid metadata protobuf";
		return false;
	}
	const auto* encryptedFlag = findField(metadata, 4, 0);
	if (!encryptedFlag || encryptedFlag->varint == 0)
	{
		output = input;
		return true;
	}
	if (key.size() != 32)
	{
		error = "missing 32-byte depot key";
		return false;
	}

	std::vector<Field> payload;
	if (!parseFields(payloadIt->bytes, payload))
	{
		error = "invalid payload protobuf";
		return false;
	}

	struct Mapping { std::string name; Field field; };
	std::vector<Mapping> mappings;
	for (auto& field : payload)
	{
		if (field.number != 1 || field.wire != 2) continue;
		std::vector<Field> file;
		if (!parseFields(field.bytes, file))
		{
			error = "invalid file mapping";
			return false;
		}
		auto* name = findField(file, 1, 2);
		if (!name)
		{
			error = "file mapping has no filename";
			return false;
		}
		std::string clearName;
		if (!aesDecryptName(name->bytes, key, clearName))
		{
			error = "filename decryption failed";
			return false;
		}
		name->bytes = clearName;
		setBytes(file, 4, filenameSha(clearName));
		if (auto* link = findField(file, 7, 2); link && !link->bytes.empty())
		{
			std::string clearLink;
			if (!aesDecryptName(link->bytes, key, clearLink))
			{
				error = "link target decryption failed";
				return false;
			}
			link->bytes = std::move(clearLink);
		}
		field.bytes = encodeFields(file);
		mappings.push_back({std::move(clearName), field});
	}
	if (mappings.empty())
	{
		error = "encrypted manifest has no file mappings";
		return false;
	}

	std::sort(mappings.begin(), mappings.end(),
	          [](const Mapping& a, const Mapping& b)
	          {
		          auto lower = [](unsigned char c) {
			          return static_cast<unsigned char>(std::tolower(c));
		          };
		          return std::lexicographical_compare(
		              a.name.begin(), a.name.end(), b.name.begin(), b.name.end(),
		              [&](char x, char y) { return lower(x) < lower(y); });
	          });
	size_t mappingIndex = 0;
	for (auto& field : payload)
	{
		if (field.number == 1 && field.wire == 2)
			field = std::move(mappings[mappingIndex++].field);
	}
	payloadIt->bytes = encodeFields(payload);

	setVarint(metadata, 4, 0);
	std::string crcInput;
	putU32(crcInput, static_cast<uint32_t>(payloadIt->bytes.size()));
	crcInput += payloadIt->bytes;
	setVarint(metadata, 9, crc32(crcInput));
	metadataIt->bytes = encodeFields(metadata);

	for (const auto& section : sections)
	{
		putU32(output, section.magic);
		if (section.magic == kEndMagic) continue;
		putU32(output, static_cast<uint32_t>(section.bytes.size()));
		output += section.bytes;
	}
	return true;
}
