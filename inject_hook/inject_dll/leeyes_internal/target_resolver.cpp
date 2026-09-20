#include "target_resolver.h"

#include <stdio.h>
#include <cstring>

namespace LeeyesInternal
{
namespace
{

struct TargetSpec
{
	TargetKind kind;
	const BYTE* signature;
	const char* mask;
	size_t signatureLength;
};

const BYTE kRefreshSignature[] =
	{ 0x53, 0x56, 0x83, 0xC4, 0xE4, 0x8B, 0xD8, 0x80, 0xBB, 0x78, 0x0E, 0x00, 0x00, 0x00 };
const BYTE kChangingSignature[] =
	{ 0x55, 0x8B, 0xEC, 0x53, 0x8B, 0xD8, 0x8B, 0x45, 0x0C, 0xE8,
	  0x00, 0x00, 0x00, 0x00, 0x8B, 0x45, 0x08, 0xE8,
	  0x00, 0x00, 0x00, 0x00, 0x33, 0xC0 };
const BYTE kChangeSignature[] =
	{ 0x55, 0x8B, 0xEC, 0x83, 0xC4, 0xF8, 0x53, 0x56, 0x57,
	  0x33, 0xDB, 0x89, 0x5D, 0xF8, 0x89, 0x4D, 0xFC, 0x8B, 0xF0,
	  0x8B, 0x45, 0x0C, 0xE8, 0x00, 0x00, 0x00, 0x00,
	  0x8B, 0x45, 0x08, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x33, 0xC0 };
const BYTE kFolderChangeSignature[] =
	{ 0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57, 0x8B, 0xF0, 0x8B, 0x45, 0x0C,
	  0xE8, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x45, 0x08, 0xE8,
	  0x00, 0x00, 0x00, 0x00, 0x33, 0xC0 };
const BYTE kNotifyTimerSignature[] =
	{ 0x53, 0x8B, 0xD8, 0x33, 0xD2, 0x8B, 0x83, 0x74, 0x09, 0x00, 0x00 };
const BYTE kLayoutRefreshSignature[] =
	{ 0x55, 0x8B, 0xEC, 0x51, 0x53, 0x56, 0x57, 0x8B, 0xD8,
	  0x80, 0xBB, 0x78, 0x0E, 0x00, 0x00, 0x00 };
const BYTE kLayoutUpdateSignature[] =
	{ 0x53, 0x56, 0x57, 0x55, 0x83, 0xC4, 0xF4, 0x88, 0x14, 0x24,
	  0x8B, 0xD8, 0x8B, 0x83, 0x50, 0x05, 0x00, 0x00 };
const BYTE kFileChangeDispatchSignature[] =
	{ 0x55, 0x8B, 0xEC, 0x51, 0xB9, 0x09, 0x00, 0x00, 0x00,
	  0x6A, 0x00, 0x6A, 0x00, 0x49, 0x75, 0xF9, 0x87, 0x4D, 0xFC,
	  0x53, 0x56, 0x89, 0x4D, 0xF8, 0x8B, 0xDA, 0x89, 0x45, 0xFC };
const BYTE kFullResyncSignature[] =
	{ 0x55, 0x8B, 0xEC, 0x83, 0xC4, 0xF8, 0x53, 0x8B, 0xDA,
	  0x89, 0x45, 0xFC, 0x33, 0xD2, 0x8B, 0x45, 0xFC, 0xE8,
	  0x00, 0x00, 0x00, 0x00, 0x8B, 0x45, 0xFC, 0xE8,
	  0x00, 0x00, 0x00, 0x00, 0x83, 0xCA, 0xFF };
const BYTE kChangeQueueDrainSignature[] =
	{ 0x55, 0x8B, 0xEC, 0x6A, 0x00, 0x6A, 0x00, 0x6A, 0x00,
	  0x53, 0x56, 0x57, 0x89, 0x45, 0xFC, 0x33, 0xC0, 0x55, 0x68,
	  0x00, 0x00, 0x00, 0x00, 0x64, 0xFF, 0x30, 0x64, 0x89, 0x20,
	  0x8B, 0x45, 0xFC, 0x8B, 0x40, 0x10, 0x33, 0xD2 };

const TargetSpec kSpecs[] =
{
	{ TargetKind::Refresh, kRefreshSignature, "xxxxxxxxxxxxxx", sizeof(kRefreshSignature) },
	{ TargetKind::Changing, kChangingSignature, "xxxxxxxxxx????xxxx????xx", sizeof(kChangingSignature) },
	{ TargetKind::Change, kChangeSignature, "xxxxxxxxxxxxxxxxxxxxxxx????xxxx????xx", sizeof(kChangeSignature) },
	{ TargetKind::FolderChange, kFolderChangeSignature, "xxxxxxxxxxxx????xxxx????xx", sizeof(kFolderChangeSignature) },
	{ TargetKind::NotifyTimer, kNotifyTimerSignature, "xxxxxxxxxxx", sizeof(kNotifyTimerSignature) },
	{ TargetKind::LayoutRefresh, kLayoutRefreshSignature, "xxxxxxxxxxxxxxx", sizeof(kLayoutRefreshSignature) },
	{ TargetKind::LayoutUpdate, kLayoutUpdateSignature, "xxxxxxxxxxxxxxxxxx", sizeof(kLayoutUpdateSignature) },
	{ TargetKind::FileChangeDispatch, kFileChangeDispatchSignature,
	  "xxxxxxxxxxxxxxxxxxxxxxxxxxxxx", sizeof(kFileChangeDispatchSignature) },
	{ TargetKind::FullResync, kFullResyncSignature,
	  "xxxxxxxxxxxxxxxxxx????xxxx????xxx", sizeof(kFullResyncSignature) },
	{ TargetKind::ChangeQueueDrain, kChangeQueueDrainSignature,
	  "xxxxxxxxxxxxxxxxxxx????xxxxxxxxxxxxxx", sizeof(kChangeQueueDrainSignature) }
};

bool IsExecutableRange(const BYTE* address, size_t length)
{
	if( !address || length == 0 ) return false;
	MEMORY_BASIC_INFORMATION information = {};
	if( ::VirtualQuery(address, &information, sizeof(information)) == 0 ) return false;
	if( information.State != MEM_COMMIT
		|| (information.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ
			| PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0 ) return false;
	const BYTE* regionEnd = reinterpret_cast<const BYTE*>(information.BaseAddress) + information.RegionSize;
	return address <= regionEnd && length <= static_cast<size_t>(regionEnd - address);
}

bool Matches(const BYTE* address, const TargetSpec& spec)
{
	for( size_t index = 0; index < spec.signatureLength; ++index )
		if( spec.mask[index] == 'x' && address[index] != spec.signature[index] ) return false;
	return true;
}

const IMAGE_SECTION_HEADER* FindTextSection(const BYTE* image, const IMAGE_NT_HEADERS32* nt)
{
	const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
	for( WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section )
	{
		if( (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 ) return section;
	}
	(void)image;
	return nullptr;
}

void SetFailure(ResolvedTargets& targets, const char* message)
{
	strncpy_s(targets.failure, sizeof(targets.failure), message, _TRUNCATE);
}

void* FindUnique(const BYTE* image, const IMAGE_SECTION_HEADER& section,
	const TargetSpec& spec, unsigned int& count)
{
	count = 0;
	const BYTE* begin = image + section.VirtualAddress;
	DWORD span = section.Misc.VirtualSize != 0 ? section.Misc.VirtualSize : section.SizeOfRawData;
	// The PE executable section is the search boundary. Validate that boundary
	// once, rather than calling VirtualQuery for every candidate byte below.
	// The previous per-candidate validation turned signature resolution into
	// millions of unnecessary system calls during Leeyes startup.
	if( span < spec.signatureLength || !IsExecutableRange(begin, span) ) return nullptr;
	const BYTE* end = begin + span - spec.signatureLength;
	const BYTE* found = nullptr;
	for( const BYTE* candidate = begin; candidate <= end; ++candidate )
	{
		if( !Matches(candidate, spec) ) continue;
		++count;
		found = candidate;
		if( count > 1 ) return nullptr;
	}
	return const_cast<BYTE*>(found);
}

void* ResolveOne(const BYTE* image, const IMAGE_SECTION_HEADER& section,
	const TargetSpec& spec, bool& unique)
{
	unsigned int count = 0;
	void* result = FindUnique(image, section, spec, count);
	if( count == 1 )
	{
		unique = true;
		return result;
	}
	unique = false;
	return nullptr;
}

bool IsRequired(TargetKind kind, bool requireTraceTargets, bool requireBatchTargets)
{
	if( requireTraceTargets && (kind == TargetKind::Refresh
		|| kind == TargetKind::Changing || kind == TargetKind::Change) ) return true;
	if( requireBatchTargets && (kind == TargetKind::FileChangeDispatch
		|| kind == TargetKind::FullResync || kind == TargetKind::ChangeQueueDrain) ) return true;
	return false;
}

bool ContainsCallTo(const BYTE* begin, size_t span, const BYTE* target)
{
	if( !begin || !target || span < 5 ) return false;
	for( size_t index = 0; index + 5 <= span; ++index )
	{
		if( begin[index] != 0xE8 ) continue;
		INT32 displacement = 0;
		memcpy(&displacement, begin + index + 1, sizeof(displacement));
		const BYTE* destination = begin + index + 5 + displacement;
		if( destination == target ) return true;
	}
	return false;
}

void AssignTarget(ResolvedTargets& targets, TargetKind kind, void* address)
{
	switch( kind )
	{
	case TargetKind::Refresh: targets.refresh = address; break;
	case TargetKind::Changing: targets.changing = address; break;
	case TargetKind::Change: targets.change = address; break;
	case TargetKind::FolderChange: targets.folderChange = address; break;
	case TargetKind::NotifyTimer: targets.notifyTimer = address; break;
	case TargetKind::LayoutRefresh: targets.layoutRefresh = address; break;
	case TargetKind::LayoutUpdate: targets.layoutUpdate = address; break;
	case TargetKind::FileChangeDispatch: targets.fileChangeDispatch = address; break;
	case TargetKind::FullResync: targets.fullResync = address; break;
	case TargetKind::ChangeQueueDrain: targets.changeQueueDrain = address; break;
	}
}

}

const char* TargetKindName(TargetKind kind)
{
	switch( kind )
	{
	case TargetKind::Refresh: return "lvFileRefresh";
	case TargetKind::Changing: return "lvFileChangingNotify";
	case TargetKind::Change: return "lvFileChangeNotify";
	case TargetKind::FolderChange: return "FolderChangeNotify";
	case TargetKind::NotifyTimer: return "NotifyTimerTimer";
	case TargetKind::LayoutRefresh: return "list_layout_refresh";
	case TargetKind::LayoutUpdate: return "list_layout_update";
	case TargetKind::FileChangeDispatch: return "file_change_dispatch";
	case TargetKind::FullResync: return "full_resync";
	case TargetKind::ChangeQueueDrain: return "change_queue_drain";
	}
	return "unknown";
}

bool ResolveTargets(ResolvedTargets& targets, bool requireTraceTargets,
	bool requireBatchTargets)
{
	targets = ResolvedTargets();
	targets.module = ::GetModuleHandleW(nullptr);
	if( !targets.module )
	{
		SetFailure(targets, "main module unavailable");
		return false;
	}
	const BYTE* image = reinterpret_cast<const BYTE*>(targets.module);
	const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
	if( dos->e_magic != IMAGE_DOS_SIGNATURE )
	{
		SetFailure(targets, "DOS header mismatch");
		return false;
	}
	const IMAGE_NT_HEADERS32* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
	if( nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386
		|| nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC )
	{
		SetFailure(targets, "not a PE32 x86 image");
		return false;
	}
	targets.imageBase = nt->OptionalHeader.ImageBase;
	targets.sizeOfImage = nt->OptionalHeader.SizeOfImage;
	const IMAGE_SECTION_HEADER* text = FindTextSection(image, nt);
	if( !text )
	{
		SetFailure(targets, "executable section unavailable");
		return false;
	}

	for( const TargetSpec& spec : kSpecs )
	{
		bool unique = false;
		void* address = ResolveOne(image, *text, spec, unique);
		if( !unique || !address )
		{
			// An unrelated or diagnostic-only signature mismatch must not disable
			// batching. Conversely, each enabled feature requires all of its own
			// structurally verified entry points.
			if( !IsRequired(spec.kind, requireTraceTargets, requireBatchTargets) )
			{
				AssignTarget(targets, spec.kind, nullptr);
				continue;
			}
			char message[128] = {};
			sprintf_s(message, "anchor not unique: %s", TargetKindName(spec.kind));
			SetFailure(targets, message);
			return false;
		}
		AssignTarget(targets, spec.kind, address);
	}

	if( !requireTraceTargets ) return true;
	const BYTE* change = reinterpret_cast<const BYTE*>(targets.change);
	const BYTE* refresh = reinterpret_cast<const BYTE*>(targets.refresh);
	const BYTE* textEnd = image + text->VirtualAddress + text->Misc.VirtualSize;
	if( !text->Misc.VirtualSize || change < image + text->VirtualAddress || change >= textEnd
		|| refresh < image + text->VirtualAddress || refresh >= textEnd
		|| !ContainsCallTo(change, static_cast<size_t>(textEnd - change), refresh) )
	{
		SetFailure(targets, "change-to-refresh relation missing");
		return false;
	}
	targets.changeCallsRefresh = true;
	return true;
}

}
