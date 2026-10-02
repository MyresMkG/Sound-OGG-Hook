// Logging: a plain text log next to the DLL plus OutputDebugString, so the hook
// can be diagnosed without a debugger attached.
#pragma once

#include <windows.h>

#include <string>

namespace oggsound {

// Creates/truncates the log next to the DLL and records the module path.
void LogInit(HMODULE self);

// printf-style line, prefixed with the wall clock and seconds since process start.
void Log(const char* fmt, ...);

// Directory the DLL lives in (empty before LogInit).
const std::wstring& ModuleDir();

// Path of the log file (empty before LogInit).
const std::wstring& LogPath();

}  // namespace oggsound
