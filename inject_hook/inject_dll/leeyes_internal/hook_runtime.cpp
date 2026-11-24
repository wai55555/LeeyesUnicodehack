#include "../stdafx.h"
#include "hook_runtime.h"
#include "delphi_bridge_x86.h"
#include "file_move_batch.h"
#include "target_resolver.h"
#include "trace_buffer.h"

#include <atomic>
#include <stdio.h>

extern HMODULE gInjectedModule;

namespace LeeyesInternal
{
namespace
{

std::atomic<LONG> gInitializationState(0);

bool GetTraceLogPath(char* path, size_t capacity)
{
	if( !path || capacity == 0 || !gInjectedModule ) return false;
	if( !::GetModuleFileNameA(gInjectedModule, path, static_cast<DWORD>(capacity)) ) return false;
	char* dot = strrchr(path, '.');
	if( !dot ) return false;
	strcpy_s(dot, capacity - static_cast<size_t>(dot - path), "_internal_trace.txt");
	return true;
}

void WriteInitializationStatus(const char* status, const char* detail)
{
	char path[MAX_PATH * 2] = {};
	if( !GetTraceLogPath(path, _countof(path)) ) return;
	FILE* log = nullptr;
	if( fopen_s(&log, path, "w") != 0 || !log ) return;
	fprintf(log, "internal_hook_status=%s detail=%s\n",
		status ? status : "unknown", detail ? detail : "");
	fclose(log);
}

int ReadInternalSetting(const char* name, int defaultValue)
{
	if( !gInjectedModule || !name ) return defaultValue;
	char modulePath[MAX_PATH * 2] = {};
	if( !::GetModuleFileNameA(gInjectedModule, modulePath, _countof(modulePath)) ) return defaultValue;
	char* dot = strrchr(modulePath, '.');
	if( !dot ) return defaultValue;
	strcpy_s(dot, MAX_PATH * 2 - (dot - modulePath), ".ini");
	return ::GetPrivateProfileIntA("InternalHook", name, defaultValue, modulePath);
}

bool ReadTraceSetting()
{
	return ReadInternalSetting("Trace", 0) != 0;
}

bool ReadLayoutTraceSetting()
{
	return ReadInternalSetting("LayoutTrace", 0) != 0;
}

}

extern "C" void StartupDiagnosticLog(const char* stage, const char* detail);

extern "C" __declspec(dllexport) void WINAPI InitializeLeeyesInternalHooks()
{
	LONG expected = 0;
	if( ::InterlockedCompareExchange(reinterpret_cast<LONG*>(&gInitializationState), 1, expected) != expected )
		return;
	StartupDiagnosticLog("dll_attach_complete", "API hooks installed");
	StartupDiagnosticLog("internal_init_begin", "resolver pending");
	const bool traceEnabled = ReadTraceSetting();
	const bool batchEnabled = ReadInternalSetting("FileMoveBatch", 1) != 0;
	#if UNICODEHACK_PATH_DEBUG
	const bool treeTimingEnabled = ReadInternalSetting("TreeTiming", 0) != 0;
	#else
	const bool treeTimingEnabled = false;
	#endif
	if( !traceEnabled && !batchEnabled && !treeTimingEnabled )
	{
		StartupDiagnosticLog("internal_init_complete", "trace=0 batch=0 tree_timing=0");
		gInitializationState.store(2, std::memory_order_release);
		return;
	}

	#if UNICODEHACK_PATH_DEBUG
	if( treeTimingEnabled )
	{
		const bool treeInstalled = InstallTreeTimingHook();
		StartupDiagnosticLog(treeInstalled ? "tree_timing_installed" : "tree_timing_failed", "diagnostic only");
	}
	#endif
	if( !traceEnabled && !batchEnabled )
	{
		StartupDiagnosticLog("internal_init_complete", "trace=0 batch=0 tree_timing=1");
		gInitializationState.store(2, std::memory_order_release);
		return;
	}

	ResolvedTargets targets;
	if( !ResolveTargets(targets, traceEnabled, batchEnabled) )
	{
		StartupDiagnosticLog("resolver_failed", targets.failure);
		if( traceEnabled ) WriteInitializationStatus("resolver_failed", targets.failure);
		gInitializationState.store(3, std::memory_order_release);
		return;
	}
	bool batchInstalled = false;
	if( batchEnabled )
	{
		DWORD threshold = static_cast<DWORD>(ReadInternalSetting("FileMoveBatchThreshold", 32));
		bool diagnosticLog = ReadInternalSetting("FileMoveBatchLog", 0) != 0;
		ConfigureFileMoveBatch(threshold, diagnosticLog);
		if( InstallFileMoveBatchHooks(targets) )
		{
			batchInstalled = true;
			StartupDiagnosticLog("batch_hooks_installed", "file change hooks active");
		}
		else
		{
			StartupDiagnosticLog("batch_hook_failed", "hooks removed");
			RemoveFileMoveBatchHooks();
			ResetFileMoveBatchState();
			if( !traceEnabled )
			{
				gInitializationState.store(4, std::memory_order_release);
				return;
			}
		}
	}
	if( !traceEnabled )
	{
		StartupDiagnosticLog("internal_init_complete", batchInstalled ? "batch=1" : "batch=0");
		gInitializationState.store(batchInstalled ? 7 : 4, std::memory_order_release);
		return;
	}
	// Layout functions are a separate diagnostic probe. They are disabled by
	// default because they run on hot startup/list paths and must never become
	// an implicit part of the pass-through compatibility boundary.
	if( !ReadLayoutTraceSetting() )
	{
		targets.layoutRefresh = nullptr;
		targets.layoutUpdate = nullptr;
	}
	if( !InstallPassThroughHooks(targets) )
	{
		StartupDiagnosticLog("trace_hook_failed", "pass-through hooks removed");
		WriteInitializationStatus("hook_install_failed", "pass-through hook creation failed");
		RemovePassThroughHooks();
		gInitializationState.store(batchInstalled ? 7 : 4, std::memory_order_release);
		return;
	}
	if( !GetTraceBuffer().Start(targets, true) )
	{
		StartupDiagnosticLog("trace_start_failed", "trace buffer stopped");
		WriteInitializationStatus("trace_start_failed", "fixed trace buffer worker could not start");
		RemovePassThroughHooks();
		gInitializationState.store(batchInstalled ? 7 : 5, std::memory_order_release);
		return;
	}
	WriteInitializationStatus("hooks_started", "pass-through trace active");
	StartupDiagnosticLog("internal_init_complete", "trace=1");
	gInitializationState.store(6, std::memory_order_release);
}

void RequestTraceStop()
{
	GetTraceBuffer().RequestStop();
}

bool StopTraceWorkerAndJoin()
{
	return GetTraceBuffer().StopAndJoin();
}

}
