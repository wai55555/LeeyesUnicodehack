#pragma once

#include "target_resolver.h"

#include <Windows.h>
#include <atomic>
#include <cstdio>

namespace LeeyesInternal
{

static const DWORD kTraceCapacity = 4096;
static const DWORD kTraceTextCapacity = 64;

struct TraceRecord
{
	ULONGLONG tick = 0;
	DWORD threadId = 0;
	DWORD kind = 0;
	DWORD caller = 0;
	DWORD eax = 0;
	DWORD edx = 0;
	DWORD ecx = 0;
	DWORD stack0 = 0;
	DWORD stack1 = 0;
	DWORD stack2 = 0;
	DWORD stack3 = 0;
	WORD text0Length = 0;
	WORD text1Length = 0;
	char text0[kTraceTextCapacity] = {};
	char text1[kTraceTextCapacity] = {};
};

class TraceBuffer
{
public:
	TraceBuffer();
	bool Start(const ResolvedTargets& targets, bool hooksInstalled);
	void RequestStop();
	void Append(const TraceRecord& record);

private:
	static DWORD WINAPI FlushThreadEntry(LPVOID parameter);
	DWORD FlushThread();
	void FlushAvailable(FILE* log);
	void WriteHex(FILE* log, const char* label, const char* data, WORD length);

	std::atomic<DWORD> next_;
	std::atomic<DWORD> dropped_;
	std::atomic<LONG> committed_[kTraceCapacity];
	TraceRecord records_[kTraceCapacity];
	HANDLE wakeEvent_;
	HANDLE flushThread_;
	std::atomic<LONG> stop_;
	DWORD flushed_;
	char logPath_[MAX_PATH * 2];
};

TraceBuffer& GetTraceBuffer();

}
