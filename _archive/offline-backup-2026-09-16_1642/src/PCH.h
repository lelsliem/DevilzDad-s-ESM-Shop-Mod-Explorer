#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// Dear ImGui's Win32 backend needs the GDI declarations even though the
// CommonLibSF translation units do not.

#include "SFSE/SFSE.h"

#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/msvc_sink.h"

using namespace std::literals;
