#pragma once

#include "target_resolver.h"

#if !defined(_M_IX86)
#error "Leeyes internal Delphi bridge requires Win32 x86"
#endif

namespace LeeyesInternal
{

bool InstallPassThroughHooks(const ResolvedTargets& targets);
void RemovePassThroughHooks();
bool InstallFileMoveBatchHooks(const ResolvedTargets& targets);
void RemoveFileMoveBatchHooks();

}
