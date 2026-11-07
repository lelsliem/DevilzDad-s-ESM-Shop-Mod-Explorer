#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// Dear ImGui's Win32 backend needs the GDI declarations even though the
// CommonLibSF translation units do not.

#include "SFSE/SFSE.h"

// After SFSE.h: CommonLib rejects a Windows API include that precedes it
// (REX/W32/BASE.h raises C1189). Needed only by NarrowUtf8 below.
#include <Windows.h>

// wingdi.h defines the bare name `ERROR` as 0, which turns every REX::ERROR
// call into "illegal token on right side of '::'" (C2589). The plugin logs
// through REX and never uses the Win32 ERROR constant, so drop the macro.
#ifdef ERROR
#undef ERROR
#endif

#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/msvc_sink.h"

using namespace std::literals;

// Windows wide -> UTF-8, in one place. Four separate call sites used to
// narrow with std::string(w.begin(), w.end()), which truncates every
// non-ASCII wchar_t to its low byte (C4244) and turns a Unicode MO2 profile
// name or skipmodels entry into mojibake. PCH-only logging/text use, so a
// failed conversion degrades to '?' instead of throwing mid-frame.
inline std::string NarrowUtf8(std::wstring_view a_text)
{
	if (a_text.empty()) {
		return {};
	}
	const int need = ::WideCharToMultiByte(CP_UTF8, 0, a_text.data(),
		static_cast<int>(a_text.size()), nullptr, 0, nullptr, nullptr);
	if (need <= 0) {
		return std::string(a_text.size(), '?');
	}
	std::string out(static_cast<std::size_t>(need), '\0');
	::WideCharToMultiByte(CP_UTF8, 0, a_text.data(), static_cast<int>(a_text.size()),
		out.data(), need, nullptr, nullptr);
	return out;
}
