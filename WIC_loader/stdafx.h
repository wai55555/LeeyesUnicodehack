#pragma once

#define _WIN32_WINNT _WIN32_WINNT_WIN7
#include <windows.h>

#include <xmmintrin.h>
#include <emmintrin.h>
#include <pmmintrin.h>
#include <tmmintrin.h>
#include <smmintrin.h>

#include <cmath>
#include <string>
#include <Shlwapi.h>
#pragma comment(lib,"Shlwapi.lib")

#include <memory>

#include "comptr.h"