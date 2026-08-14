#pragma once

#include "log.hpp"

#include "libmem/libmem.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>


namespace MemHlp
{
	enum class SigFollowMode
	{
		None,
		Relative,
		PrologueUpwards
	};

	///Summary:
	///Write assembly code to address and increase address by bytes written
	template<typename ...Args>
	bool assembleCodeAt(lm_address_t& address, const char* fmt, Args... args)
	{
		if (address == LM_ADDRESS_BAD)
		{
			g_pLog->debug("Can't write to LM_ADDRESS_BAD!\n");
			return false;
		}

		const size_t size = snprintf(nullptr, 0, fmt, args...) + 1;
		char* code = reinterpret_cast<char*>(malloc(size));
		snprintf(code, size, fmt, args...);

		static lm_inst_t inst;
		//TODO: Potentially replace with LM_AssembleEx and only allocate memory as needed
		bool success = false;

		if (!LM_Assemble(code, &inst))
		{
			g_pLog->debug("Failed to assemble %s!\n", code);
		}
		else if (!LM_WriteMemory(address, inst.bytes, inst.size))
		{
			g_pLog->debug("Failed to write %s to %p!\n", code, address);
		}
		else
		{
			g_pLog->debug("Wrote %s to %p with %i bytes\n", code, address, inst.size);
			address += inst.size;
			success = true;
		}

		free(code);
		return success;
	}

	std::vector<int16_t> patternToBytes(const char* pattern);
	lm_address_t patternScan(const char* pattern, const lm_module_t module);

	lm_address_t searchSignature(const char* name, const char* signature, const lm_module_t module, const SigFollowMode mode, const void* extraData, const size_t extraDataSize);
	lm_address_t searchSignature(const char* name, const char* signature, const lm_module_t module, const SigFollowMode mode);
	lm_address_t searchSignature(const char* name, const char* signature, const lm_module_t module);

	lm_address_t getJmpTarget(const lm_address_t address);
	lm_address_t findPrologue(const lm_address_t address, const int16_t* prologueBytes, const lm_size_t prologueSize);

	//TODO: Create hooking wrapper that calls this automatically
	bool fixPICThunkCall(const char* name, const lm_address_t fn, const lm_address_t tramp);

	std::string hexdump(const void* address, const size_t size);

	const char* getTypeName(const void* pClass);
	
	template<typename tFN, typename ...Args>
	constexpr auto callVFunc(const unsigned int index, void* thisPtr, Args... args)
	{
		const auto fn = reinterpret_cast<tFN>(*(*reinterpret_cast<lm_address_t***>(thisPtr) + index));
		return fn(thisPtr, args...);
	}
}
