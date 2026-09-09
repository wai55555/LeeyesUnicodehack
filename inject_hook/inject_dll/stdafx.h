// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//

#pragma once

#include "targetver.h"

#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
// Windows Header Files:
#include <windows.h>



#include <Shlobj.h>
#include <Shellapi.h>
#include <Shlwapi.h>
#pragma comment(lib,"Shlwapi.lib")
#pragma comment(lib,"Shell32.lib")

#include <map>
#include <list>
#include <vector>
#include <string>
#include <memory>