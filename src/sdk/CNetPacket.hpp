#pragma once

#include "CProtoBufMsgBase.hpp"
#include "steam.hpp"

#include "../log.hpp"

#include <cstdint>
#include <string>


//Helper class to make calculations more legible
class CNetPacketBody
{
public:

	ENetPacket type;
	uint32_t headerSize;
	//Header[headerSize]
	//Body[CNetPacket->size - headerSize - sizeof(CNetPacketBody)]
};

//Biggest message I have observed was around 600kb. We just set
//a limit so we don't accidentally fill the whole buffer with 1 message
constexpr static unsigned int MAX_PACKET_SIZE = 1024 * 1024 * 1; //1MB
//Theoretical max
constexpr static unsigned int MAX_PACKETS = 8;

//TODO: Move into anonymous namespace or something, so these don't clutter the global namespace
extern uint8_t g_packetsArray[MAX_PACKET_SIZE * MAX_PACKETS];
extern uintptr_t g_packetsArrayOffset;

extern std::mutex g_packetSerializeMutex;

class CNetPacket
{
public:
	uint8_t __pad0x0[0x4];			//0x0
	CNetPacketBody* body;			//0x4
	uint32_t size;					//0x8
	int32_t refs;					//0xC
	CNetPacketBody* originalBody;	//0x10
	
	constexpr bool isValid() const
	{
		return body && size >= 8 && body->type != INVALID_NETPACKET_TYPE;
	}
	
	constexpr ENetPacket getType() const
	{
		if (!body)
		{
			return 0;
		}

		return body->type;
	}

	std::string getProtoBufTypeName() const;

	constexpr bool isProtoBuf() const
	{
		return getType() & PROTOBUF_TYPE_MASK;
	}

	constexpr EMsg getProtoBufType() const
	{
		return static_cast<EMsg>(getType() & ~PROTOBUF_TYPE_MASK);
	}

	void serializeHeader(const CMsgProtoBufHeader& header);
	CMsgProtoBufHeader deserializeHeader() const;

	template<typename T>
	constexpr void serialize(const T& msg, const CMsgProtoBufHeader* header)
	{
		constexpr uintptr_t headerOffset = sizeof(CNetPacketBody);
		const uintptr_t headerSize = header ? header->ByteSizeLong() : body->headerSize;

		const uintptr_t msgOffset = headerSize + headerOffset;
		const uintptr_t newSize = msg.ByteSizeLong() + msgOffset;

		if (newSize >= MAX_PACKET_SIZE)
		{
			g_pLog->debug("Failed to serialize %p! Buffer to small (needed %u, has %u)\n", getType(), newSize, MAX_PACKET_SIZE);
			return;
		}

		const uintptr_t remainingSize = sizeof(g_packetsArray) - g_packetsArrayOffset;
		if (newSize >= remainingSize)
		{
			g_pLog->debug("New packet size doesn't fit in end of buffer, (needed %u, has %u). Starting anew\n", newSize, remainingSize);
			g_packetsArrayOffset = 0;
		}

		const std::lock_guard lock(g_packetSerializeMutex);
		uint8_t* mem = &g_packetsArray[g_packetsArrayOffset];

		if (header)
		{
			if (!header->SerializeToArray(mem + headerOffset, headerSize))
			{
				g_pLog->debug("Failed to serialize header!\n");
				return;
			}

			*reinterpret_cast<uint32_t*>(mem) = body->type;
			*reinterpret_cast<uint32_t*>(mem + sizeof(body->type)) = headerSize;
		}
		else
		{
			memcpy(mem, body, msgOffset);
		}

		if (!msg.SerializeToArray(mem + msgOffset, msg.ByteSizeLong()))
		{
			g_pLog->debug("Failed to serialize %p!\n", getType());
			return;
		}

		body = reinterpret_cast<CNetPacketBody*>(mem);
		size = newSize;
		//If I understand correctly Steam cleans up for us, that's why we crash when we free the oldBody ourself
		//However the body we allocate doesn't get freed, so we just reuse a buffer for it

		g_pLog->debug("Serialized %p into PACKETS_ARRAY at %u with size %u\n", getType(), g_packetsArrayOffset, newSize);

		g_packetsArrayOffset += size;
	}

	template<typename T>
	constexpr void serialize(const T& msg)
	{
		serialize(msg, nullptr);
	}
	
	template<typename T>
	constexpr T deserializeBody() const
	{
		const uintptr_t msgOffset = body->headerSize + sizeof(CNetPacketBody);
		auto msg = T();

		msg.ParseFromArray(reinterpret_cast<uint8_t*>(body) + msgOffset, size - msgOffset);

		return msg;
	}

	constexpr void clearBody()
	{
		//Hide body and call original function so steam uses it's own free
		//We can also free the body ourself if we don't call the original function
		size = body->headerSize + sizeof(CNetPacketBody);
	}
}; //0x14
