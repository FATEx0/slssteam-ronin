#include "CUser.hpp"

#include "CUser.hpp"

#include "../hooks.hpp"
#include "../patterns.hpp"


bool CUser::checkAppOwnership(const AppId_t appId, AppOwnershipInfo_t* pInfo)
{
	return Hooks::CUser_CheckAppOwnership.tramp.fn(this, appId, pInfo);
}

bool CUser::isSubscribed(const AppId_t appId)
{
	AppOwnershipInfo_t info {};
	if (!checkAppOwnership(appId, &info))
	{
		return false;
	}

	return info.ownsLicense && !info.licenseExpired;
}

void CUser::postCallback(const ECallbackType type, void* pCallback, const uint32_t callbackSize)
{
	const static auto fn = reinterpret_cast<void(*)(void*, ECallbackType, void*, uint32_t, uint32_t)>(Patterns::CUser::PostCallback.address);
	fn(this, type, pCallback, callbackSize, 0);
}

void CUser::updateAppOwnershipTicket(const AppId_t appId, void* pTicket, const uint32_t len)
{
	const static auto fn = reinterpret_cast<void(*)(void*, uint32_t, void*, uint32_t)>(Patterns::CUser::UpdateAppOwnershipTicket.address);
	fn(this, appId, pTicket, len);

	//Dunno if this achieves anything, but the client does it so we do too
	AppOwnershipTicketReceived_t cb;
	cb.result = k_EResultOK;
	cb.appId = appId;
	postCallback(ECallbackType::AppOwnershipTicketReceived_t, &cb, sizeof(cb));
}

bool CUser::notifyLicensesUpdated()
{
	const auto address = Patterns::CUser::NotifyLicensesUpdated.address;
	if (address == LM_ADDRESS_BAD)
		return false;

	reinterpret_cast<void(*)(void*)>(address)(this);
	return true;
}
