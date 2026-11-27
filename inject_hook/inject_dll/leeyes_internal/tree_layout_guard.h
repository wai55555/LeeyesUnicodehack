#pragma once

#include <Windows.h>

namespace LeeyesInternal
{

// The tree helpers use reverse-engineered Leeyes 2.6.1 node and owner layouts.
// Refuse to read those layouts unless the relevant executable signatures match.
bool VerifyLeeyesTreeLayout();
bool GetLeeyesTreeProviderHookSites(BYTE*& ownerSite, BYTE*& enumSite);
bool GetLeeyesFolderSearchReturnSites(DWORD& addReturnSite, DWORD& deleteReturnSite);
bool GetLeeyesFolderOwnerEventCallSites(DWORD& addCallSite, DWORD& deleteCallSite);

}
