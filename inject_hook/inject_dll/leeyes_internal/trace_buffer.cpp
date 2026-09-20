#include "trace_buffer.h"

#include <cstdio>
#include <cstring>

namespace LeeyesInternal
{
namespace
{

TraceBuffer gTraceBuffer;

void AppendPath(char* path, size_t capacity, const char* suffix)
{
	if( !path || capacity == 0 || !suffix ) return;
	size_t length = strlen(path);
	if( length >= capacity ) return;
	strncpy_s(path + length, capacity - length, suffix, _TRUNCATE);
}

}

TraceBuffer::TraceBuffer()
	: next_(0), dropped_(0), wakeEvent_(nullptr), flushThread_(nullptr), stop_(0), flushed_(0)
{
	logPath_[0] = '\0';
	for( DWORD index = 0; index < kTraceCapacity; ++index ) committed_[index].store(0);
}

bool TraceBuffer::Start(const ResolvedTargets& targets, bool hooksInstalled)
{
	if( wakeEvent_ ) return true;
	char modulePath[MAX_PATH * 2] = {};
	if( !targets.module || !::GetModuleFileNameA(targets.module, modulePath, _countof(modulePath)) )
		return false;
	strncpy_s(logPath_, _countof(logPath_), modulePath, _TRUNCATE);
	char* dot = strrchr(logPath_, '.');
	if( dot ) *dot = '\0';
	AppendPath(logPath_, _countof(logPath_), "_internal_trace.txt");
	wakeEvent_ = ::CreateEventA(nullptr, FALSE, FALSE, nullptr);
	if( !wakeEvent_ ) return false;
	stop_.store(0, std::memory_order_release);
	flushThread_ = ::CreateThread(nullptr, 0, &TraceBuffer::FlushThreadEntry, this, 0, nullptr);
	if( !flushThread_ )
	{
		::CloseHandle(wakeEvent_);
		wakeEvent_ = nullptr;
		return false;
	}
	::SetEvent(wakeEvent_);
	(void)hooksInstalled;
	return true;
}

void TraceBuffer::RequestStop()
{
	if( !wakeEvent_ ) return;
	stop_.store(1, std::memory_order_release);
	::SetEvent(wakeEvent_);
}

void TraceBuffer::Append(const TraceRecord& record)
{
	DWORD index = next_.fetch_add(1, std::memory_order_relaxed);
	if( index >= kTraceCapacity )
	{
		dropped_.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	records_[index] = record;
	committed_[index].store(static_cast<LONG>(index + 1), std::memory_order_release);
	if( wakeEvent_ ) ::SetEvent(wakeEvent_);
}

DWORD WINAPI TraceBuffer::FlushThreadEntry(LPVOID parameter)
{
	return reinterpret_cast<TraceBuffer*>(parameter)->FlushThread();
}

void TraceBuffer::WriteHex(FILE* log, const char* label, const char* data, WORD length)
{
	if( !log || !label ) return;
	fprintf(log, "%s=", label);
	for( WORD index = 0; index < length; ++index ) fprintf(log, "%02X", (unsigned char)data[index]);
	fputc('\n', log);
}

void TraceBuffer::FlushAvailable(FILE* log)
{
	if( !log ) return;
	for( ; flushed_ < kTraceCapacity; ++flushed_ )
	{
		if( committed_[flushed_].load(std::memory_order_acquire)
			!= static_cast<LONG>(flushed_ + 1) ) break;
		const TraceRecord& record = records_[flushed_];
		fprintf(log,
			"trace index=%lu tick=%llu tid=%lu kind=%lu caller=0x%08lX eax=0x%08lX edx=0x%08lX ecx=0x%08lX "
			"stack0=0x%08lX stack1=0x%08lX stack2=0x%08lX stack3=0x%08lX\n",
			(unsigned long)flushed_, (unsigned long long)record.tick,
			(unsigned long)record.threadId, (unsigned long)record.kind,
			(unsigned long)record.caller, (unsigned long)record.eax,
			(unsigned long)record.edx, (unsigned long)record.ecx,
			(unsigned long)record.stack0, (unsigned long)record.stack1,
			(unsigned long)record.stack2, (unsigned long)record.stack3);
		WriteHex(log, "text0", record.text0, record.text0Length);
		WriteHex(log, "text1", record.text1, record.text1Length);
	}
	if( flushed_ >= kTraceCapacity )
		fprintf(log, "trace_overflow=%lu\n", (unsigned long)dropped_.load(std::memory_order_relaxed));
	fflush(log);
}

DWORD TraceBuffer::FlushThread()
{
	FILE* log = nullptr;
	if( fopen_s(&log, logPath_, "w") != 0 || !log ) return 0;
	fprintf(log, "internal_hook_trace version=1 capacity=%lu\n", (unsigned long)kTraceCapacity);
	for(;;)
	{
		::WaitForSingleObject(wakeEvent_, 250);
		FlushAvailable(log);
		if( stop_.load(std::memory_order_acquire) != 0 ) break;
	}
	FlushAvailable(log);
	fprintf(log, "trace_end next=%lu dropped=%lu\n",
		(unsigned long)next_.load(std::memory_order_relaxed),
		(unsigned long)dropped_.load(std::memory_order_relaxed));
	fclose(log);
	return 0;
}

TraceBuffer& GetTraceBuffer()
{
	return gTraceBuffer;
}

}
