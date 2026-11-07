#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "SFSE/Impl/PCH.h"

// After the CommonLib header: REX/W32/BASE.h rejects a Windows API include
// that precedes it (C1189). NOMINMAX above keeps `std::min`/`std::max` usable
// in CommonLib's own headers, which is how the shop project includes it too.
#include <Windows.h>

// wingdi.h defines a bare `ERROR` as 0, which turns every REX::ERROR call into
// "illegal token on right side of '::'" (C2589). This plugin logs through REX
// and never uses the Win32 ERROR constant.
#ifdef ERROR
#undef ERROR
#endif
