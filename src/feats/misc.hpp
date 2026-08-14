#pragma once


class CNetPacket;

namespace Misc
{
	bool shouldFakeOffline();
	void recvMsg(CNetPacket* pkt);
}
