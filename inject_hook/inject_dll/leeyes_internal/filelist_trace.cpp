#include "../stdafx.h"
#include "filelist_trace.h"

#include <cstring>

namespace LeeyesInternal
{
namespace
{

WORD CopyDelphiAnsiString(DWORD pointer, char* destination, WORD capacity)
{
	if( !pointer || !destination || capacity < 2 ) return 0;
	WORD length = 0;
	__try
	{
		DWORD declaredLength = *reinterpret_cast<const DWORD*>(pointer - sizeof(DWORD));
		if( declaredLength == 0 || declaredLength >= capacity ) return 0;
		length = static_cast<WORD>(declaredLength);
		for( WORD index = 0; index < length; ++index )
			destination[index] = *reinterpret_cast<const char*>(pointer + index);
		destination[length] = '\0';
	}
	__except( EXCEPTION_EXECUTE_HANDLER )
	{
		length = 0;
		destination[0] = '\0';
	}
	return length;
}

DWORD ReadStack(const SavedRegisters* registers, DWORD offset)
{
	if( !registers || !registers->originalEsp ) return 0;
	DWORD value = 0;
	__try
	{
		value = *reinterpret_cast<const DWORD*>(registers->originalEsp + offset);
	}
	__except( EXCEPTION_EXECUTE_HANDLER )
	{
		value = 0;
	}
	return value;
}

}

extern "C" void __cdecl LeeyesInternalTraceEntry(DWORD kind, const SavedRegisters* registers)
{
	if( !registers ) return;
	TraceRecord record;
	record.tick = ::GetTickCount64();
	record.threadId = ::GetCurrentThreadId();
	record.kind = kind;
	record.eax = registers->eax;
	record.edx = registers->edx;
	record.ecx = registers->ecx;
	// pushad's saved originalEsp points at the flags word pushed by the
	// bridge.  The original call's return address is therefore +4, followed
	// by the first stack argument at +8.  Keeping this distinction explicit
	// is important: treating +4 as an argument would turn the return address
	// into a false filename pointer and hide the actual action fields.
	record.stack0 = ReadStack(registers, 8);
	record.stack1 = ReadStack(registers, 12);
	record.stack2 = ReadStack(registers, 16);
	record.stack3 = ReadStack(registers, 20);
	__try
	{
		record.caller = registers->originalEsp
			? *reinterpret_cast<const DWORD*>(registers->originalEsp + 4) : 0;
	}
	__except( EXCEPTION_EXECUTE_HANDLER )
	{
		record.caller = 0;
	}
	// These are bounded, in-call snapshots. No pointer is retained after the
	// Delphi method returns, and non-string ABI values are rejected by the
	// length check in CopyDelphiAnsiString.
	record.text0Length = CopyDelphiAnsiString(record.stack0, record.text0, kTraceTextCapacity);
	record.text1Length = CopyDelphiAnsiString(record.stack1, record.text1, kTraceTextCapacity);
	GetTraceBuffer().Append(record);
}

}
