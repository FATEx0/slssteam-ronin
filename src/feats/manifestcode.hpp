
#pragma once

#include "../sdk/CNetPacket.hpp"
#include "../sdk/CWebSocketFrame.hpp"

#include <cstdint>


namespace ManifestCode
{
	bool hkBBuildAndAsyncSendFrame(void* pConnection,
	                               EWebSocketOpCode eOpCode,
	                               uint8_t* pubData,
	                               uint32_t cubData);

	void processRecv(CNetPacket* pPacket);
}
