#pragma once

#include "trace_buffer.h"

namespace LeeyesInternal
{

struct SavedRegisters
{
	DWORD edi;
	DWORD esi;
	DWORD ebp;
	DWORD originalEsp;
	DWORD ebx;
	DWORD edx;
	DWORD ecx;
	DWORD eax;
};

extern "C" void __cdecl LeeyesInternalTraceEntry(DWORD kind, const SavedRegisters* registers);

}
