#include "../stdafx.h"
#include "tree_layout_guard.h"

#include <cstring>

extern "C" void StartupDiagnosticLog(const char* stage, const char* detail);

namespace LeeyesInternal
{
namespace
{

const BYTE kActionSignature[] = {
	0x55,0x8B,0xEC,0x83,0xC4,0xC8,0x53,0x56,
	0x33,0xDB,0x89,0x5D,0xC8,0x89,0x5D,0xD0,
	0x89,0x5D,0xCC,0x89,0x5D,0xD4,0x89,0x5D,0xD8,
	0x89,0x5D,0xDC,0x89,0x4D,0xE0
};
const BYTE kFolderActionSignature[] = {
	0x55,0x8B,0xEC,0x83,0xC4,0xD0,0x53,0x56,
	0x33,0xDB,0x89,0x5D,0xD4,0x89,0x5D,0xD0,
	0x89,0x5D,0xDC,0x89,0x5D,0xD8,0x89,0x5D,0xE0,
	0x89,0x4D,0xF4,0x8B,0xDA,0x89,0x45,0xF8
};
const BYTE kSearchSignature[] = {
	0x55,0x8B,0xEC,0x6A,0x00,0x6A,0x00,0x6A,0x00,
	0x53,0x56,0x57,0x8B,0xF9,0x89,0x55,0xFC,
	0x8B,0xD8,0x8B,0x45,0xFC,0xE8
};
const BYTE kParentSignature[] = {
	0x55,0x8B,0xEC,0x51,0xB9,0x05,0x00,0x00,0x00,
	0x6A,0x00,0x6A,0x00,0x49,0x75,0xF9,0x87,0x4D,
	0xFC,0x53,0x56,0x57,0x8B,0xD9,0x89,0x55,0xFC
};
const BYTE kNodeDataSignature[] = {
	0x53,0x8B,0xD8,0x80,0x7B,0x24,0x00,0x75,0x63,
	0x3B,0x53,0x0C,0x74,0x5E,0x89,0x53,0x0C
};
const BYTE kInnerTreeSearchPrefix[] = {
	0x55,0x8B,0xEC,0x51,0x53,0x89,0x45,0xFC,
	0x8B,0x45,0xFC,0xE8
};
const BYTE kInnerTreeSearchSuffix[] = { 0x33,0xC0,0x55,0x68 };
const BYTE kPathGetterPrefix[] = {
	0x55,0x8B,0xEC,0x33,0xC9,0x51,0x51,0x51,0x51,
	0x53,0x56,0x57,0x8B,0xF2,0x8B,0xD8,0x33,0xC0,
	0x55,0x68
};
const BYTE kPathGetterSuffix[] = {
	0x64,0xFF,0x30,0x64,0x89,0x20,0x83,0x7B,0x38,0x00
};
const BYTE kDirectoryRescanSignature[] = {
	0x55,0x8B,0xEC,0x83,0xC4,0xE8,0x53,0x8B,0xDA,
	0x89,0x45,0xFC,0x33,0xC0,0x89,0x45,0xF0,
	0x33,0xC0,0x89,0x45,0xEC
};
const BYTE kAnsiStringAllocateSignature[] = { 0x85,0xC0,0x7E,0x24,0x50 };
const BYTE kAnsiStringClearSignature[] = { 0x8B,0x10,0x85,0xD2,0x74,0x1C };
const BYTE kShellOwnerOpcode[] = { 0xA1 };
const BYTE kShellOwnerSuffix[] = {
	0x8B,0x00,0x8B,0x40,0x30,0x50,0x8D,0x55,0xC8,0x8B,
	0x45,0xF8,0x8B,0x40,0x0C,0x8B,0x08,0xFF,0x51,0x0C
};
const BYTE kShellEnumSignature[] = {
	0x8B,0x00,0xFF,0x50,0x10,0x85,0xC0,0x74,0x0D,
	0x33,0xC0,0x5A,0x59,0x59,0x64,0x89,0x10,0xE9
};

bool IsExecutableRange(const BYTE* address, size_t length)
{
	if( !address || !length ) return false;
	MEMORY_BASIC_INFORMATION region = {};
	if( ::VirtualQuery(address, &region, sizeof(region)) == 0
		|| region.State != MEM_COMMIT
		|| (region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ
			| PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0 ) return false;
	const BYTE* end = reinterpret_cast<const BYTE*>(region.BaseAddress) + region.RegionSize;
	return address <= end && length <= static_cast<size_t>(end - address);
}

BYTE* FindUnique(const BYTE* image, DWORD imageSize, const BYTE* pattern, size_t patternSize,
	size_t suffixOffset = 0, const BYTE* suffix = nullptr, size_t suffixSize = 0)
{
	if( !image || !pattern || !patternSize || (suffixSize && !suffix)
		|| imageSize < sizeof(IMAGE_NT_HEADERS32) ) return nullptr;
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
	if( dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0
		|| static_cast<DWORD>(dos->e_lfanew) > imageSize - sizeof(IMAGE_NT_HEADERS32) ) return nullptr;
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
	if( nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ) return nullptr;
	const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
	BYTE* found = nullptr;
	for( WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section )
	{
		if( !(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)
			|| section->VirtualAddress >= imageSize ) continue;
		const size_t span = min(static_cast<size_t>(section->Misc.VirtualSize),
			static_cast<size_t>(imageSize - section->VirtualAddress));
		const size_t required = max(patternSize, suffixSize ? suffixOffset + suffixSize : size_t(0));
		if( span < required ) continue;
		BYTE* begin = const_cast<BYTE*>(image + section->VirtualAddress);
		if( !IsExecutableRange(begin, span) ) continue;
		for( size_t offset = 0; offset <= span - required; ++offset )
		{
			if( memcmp(begin + offset, pattern, patternSize) != 0 ) continue;
			if( suffixSize && memcmp(begin + offset + suffixOffset, suffix, suffixSize) != 0 ) continue;
			if( found ) return nullptr;
			found = begin + offset;
		}
	}
	return found;
}

bool RelativeCallTargets(const BYTE* site, const BYTE* target)
{
	if( !site || !target || site[0] != 0xE8 ) return false;
	LONG displacement = 0;
	memcpy(&displacement, site + 1, sizeof(displacement));
	return site + 5 + displacement == target;
}

bool LayoutReject(const char* reason, const char* detail = nullptr)
{
	char message[256] = {};
	if( detail ) sprintf_s(message, _countof(message), "%s %s", reason, detail);
	else sprintf_s(message, _countof(message), "%s", reason);
	StartupDiagnosticLog("tree_layout_reject", message);
	return false;
}

bool VerifyLayout()
{
	BYTE* const image = reinterpret_cast<BYTE*>(::GetModuleHandleW(nullptr));
	if( !image ) return LayoutReject("module_missing");
	if( reinterpret_cast<DWORD>(image) != 0x00400000 )
	{
		char detail[64] = {};
		sprintf_s(detail, _countof(detail), "base=%08lX", static_cast<unsigned long>(reinterpret_cast<DWORD>(image)));
		return LayoutReject("image_base", detail);
	}
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
	if( dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ) return LayoutReject("dos_header");
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
	if( nt->Signature != IMAGE_NT_SIGNATURE ) return LayoutReject("nt_signature");
	if( nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ) return LayoutReject("machine");
	if( nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ) return LayoutReject("optional_header");
	if( nt->OptionalHeader.ImageBase != 0x00400000 ) return LayoutReject("optional_image_base");
	if( nt->OptionalHeader.SizeOfImage < 0x000F8000 )
	{
		char detail[64] = {};
		sprintf_s(detail, _countof(detail), "size=%08lX", static_cast<unsigned long>(nt->OptionalHeader.SizeOfImage));
		return LayoutReject("image_size", detail);
	}
	const DWORD imageSize = nt->OptionalHeader.SizeOfImage;
	BYTE* const ansiStringAllocate = image + 0x000050CC;
	BYTE* const ansiStringClear = image + 0x00005008;
	if( !IsExecutableRange(ansiStringAllocate, sizeof(kAnsiStringAllocateSignature))
		|| memcmp(ansiStringAllocate, kAnsiStringAllocateSignature, sizeof(kAnsiStringAllocateSignature)) != 0 )
		return LayoutReject("ansi_allocate_signature");
	if( !IsExecutableRange(ansiStringClear, sizeof(kAnsiStringClearSignature))
		|| memcmp(ansiStringClear, kAnsiStringClearSignature, sizeof(kAnsiStringClearSignature)) != 0 )
		return LayoutReject("ansi_clear_signature");
	BYTE* const action = FindUnique(image, imageSize, kActionSignature, sizeof(kActionSignature));
	BYTE* const folderAction = FindUnique(image, imageSize, kFolderActionSignature, sizeof(kFolderActionSignature));
	BYTE* const search = FindUnique(image, imageSize, kSearchSignature, sizeof(kSearchSignature));
	BYTE* const parent = FindUnique(image, imageSize, kParentSignature, sizeof(kParentSignature));
	BYTE* const nodeData = FindUnique(image, imageSize, kNodeDataSignature, sizeof(kNodeDataSignature));
	BYTE* const innerTreeSearch = FindUnique(image, imageSize, kInnerTreeSearchPrefix,
		sizeof(kInnerTreeSearchPrefix), 16, kInnerTreeSearchSuffix, sizeof(kInnerTreeSearchSuffix));
	BYTE* const pathGetter = FindUnique(image, imageSize, kPathGetterPrefix, sizeof(kPathGetterPrefix),
		24, kPathGetterSuffix, sizeof(kPathGetterSuffix));
	BYTE* const directoryRescan = FindUnique(image, imageSize, kDirectoryRescanSignature,
		sizeof(kDirectoryRescanSignature));
	BYTE* const shellOwner = FindUnique(image, imageSize, kShellOwnerOpcode, sizeof(kShellOwnerOpcode),
		5, kShellOwnerSuffix, sizeof(kShellOwnerSuffix));
	BYTE* const shellEnum = FindUnique(image, imageSize, kShellEnumSignature, sizeof(kShellEnumSignature));
	if( !action ) return LayoutReject("signature_action");
	if( !folderAction ) return LayoutReject("signature_folder_action");
	if( !search ) return LayoutReject("signature_search");
	if( !parent ) return LayoutReject("signature_parent");
	if( !nodeData ) return LayoutReject("signature_node_data");
	if( !innerTreeSearch ) return LayoutReject("signature_inner_tree_search");
	if( !pathGetter ) return LayoutReject("signature_path_getter");
	if( !directoryRescan ) return LayoutReject("signature_directory_rescan");
	if( !shellOwner ) return LayoutReject("signature_shell_owner");
	if( !shellEnum ) return LayoutReject("signature_shell_enum");
	if( action != image + 0x000F6784 ) return LayoutReject("rva_action");
	// kSearchSignature is the action 1 branch helper at 0x004F3E70;
	// kParentSignature is the shared parent path search at 0x004CDD20.
	if( search != image + 0x000F3E70 ) return LayoutReject("rva_search");
	if( parent != image + 0x000CDD20 ) return LayoutReject("rva_parent");
	if( folderAction != image + 0x000CF3A8 ) return LayoutReject("rva_folder_action");
	if( innerTreeSearch != image + 0x000F65CC ) return LayoutReject("rva_inner_tree_search");
	if( pathGetter != image + 0x000BD470 ) return LayoutReject("rva_path_getter");
	if( directoryRescan != image + 0x000F7BE0 ) return LayoutReject("rva_directory_rescan");
	const BYTE addResultBranch[] = { 0x89,0x45,0xE4,0x83,0x7D,0xE4,0x00,0x74,0x12 };
	const BYTE deleteResultBranch[] = { 0x89,0x45,0xE4,0x83,0x7D,0xE4,0x00,0x0F,0x84,0xFB,0x00,0x00,0x00 };
	const BYTE ownerEventCall[] = { 0xFF,0x93,0xD0,0x03,0x00,0x00 };
	const BYTE addOwnerEventTail[] = { 0xE9,0x18,0x03,0x00,0x00 };
	// Delphi SEH finally epilogue after the delete owner event call. The add
	// branch jumps here instead of running it inline (0xE9 18 03 00 00).
	const BYTE deleteOwnerEventTail[] = { 0x33,0xC0,0x5A,0x59,0x59,0x64,0x89,0x10 };
	if( !IsExecutableRange(folderAction, 0x4BF) ) return LayoutReject("folder_action_range");
	if( !RelativeCallTargets(folderAction + 0x78, parent) ) return LayoutReject("folder_add_call_site");
	if( memcmp(folderAction + 0x7D, addResultBranch, sizeof(addResultBranch)) != 0 )
		return LayoutReject("folder_add_branch");
	if( !RelativeCallTargets(folderAction + 0x36C, parent) ) return LayoutReject("folder_delete_call_site");
	if( memcmp(folderAction + 0x371, deleteResultBranch, sizeof(deleteResultBranch)) != 0 )
		return LayoutReject("folder_delete_branch");
	if( memcmp(folderAction + 0x194, ownerEventCall, sizeof(ownerEventCall)) != 0 )
		return LayoutReject("folder_add_event_call");
	if( memcmp(folderAction + 0x19A, addOwnerEventTail, sizeof(addOwnerEventTail)) != 0 )
		return LayoutReject("folder_add_event_tail");
	if( memcmp(folderAction + 0x4B1, ownerEventCall, sizeof(ownerEventCall)) != 0 )
		return LayoutReject("folder_delete_event_call");
	if( memcmp(folderAction + 0x4B7, deleteOwnerEventTail, sizeof(deleteOwnerEventTail)) != 0 )
		return LayoutReject("folder_delete_event_tail");
	if( !RelativeCallTargets(search + 0x3D, parent) ) return LayoutReject("search_parent_call");
	if( !RelativeCallTargets(action + 0x2AE, image + 0x000F65CC) ) return LayoutReject("action_inner_search_call");
	return true;
}


}

bool VerifyLeeyesTreeLayout()
{
	static const bool supported = VerifyLayout();
	return supported;
}

bool GetLeeyesTreeProviderHookSites(BYTE*& ownerSite, BYTE*& enumSite)
{
	ownerSite = nullptr;
	enumSite = nullptr;
	if( !VerifyLeeyesTreeLayout() ) return false;
	BYTE* const image = reinterpret_cast<BYTE*>(::GetModuleHandleW(nullptr));
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
		image + reinterpret_cast<const IMAGE_DOS_HEADER*>(image)->e_lfanew);
	const DWORD imageSize = nt->OptionalHeader.SizeOfImage;
	BYTE* owner = FindUnique(image, imageSize, kShellOwnerOpcode, sizeof(kShellOwnerOpcode),
		5, kShellOwnerSuffix, sizeof(kShellOwnerSuffix));
	BYTE* enumeration = FindUnique(image, imageSize, kShellEnumSignature, sizeof(kShellEnumSignature));
	if( !owner || !enumeration ) return false;
	ownerSite = owner;
	enumSite = enumeration;
	return true;
}

bool GetLeeyesFolderSearchReturnSites(DWORD& addReturnSite, DWORD& deleteReturnSite)
{
	addReturnSite = deleteReturnSite = 0;
	if( !VerifyLeeyesTreeLayout() ) return false;
	addReturnSite = 0x004CF425;
	deleteReturnSite = 0x004CF719;
	return true;
}

bool GetLeeyesFolderOwnerEventCallSites(DWORD& addCallSite, DWORD& deleteCallSite)
{
	addCallSite = deleteCallSite = 0;
	if( !VerifyLeeyesTreeLayout() ) return false;
	addCallSite = 0x004CF3A8 + 0x194;
	deleteCallSite = 0x004CF3A8 + 0x4B1;
	return true;
}

}
