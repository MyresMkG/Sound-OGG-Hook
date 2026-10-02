#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace oggsound {
namespace {

CRITICAL_SECTION g_lock;
bool g_lock_ready = false;
HANDLE g_file = INVALID_HANDLE_VALUE;
std::wstring g_dir;
std::wstring g_path;
DWORD g_process_start = 0;

void WriteAll(const char* text, int len) {
  if (g_file != INVALID_HANDLE_VALUE) {
    DWORD written = 0;
    ::WriteFile(g_file, text, (DWORD)len, &written, nullptr);
  }
  ::OutputDebugStringA(text);
}

}  // namespace

void LogInit(HMODULE self) {
  g_process_start = ::GetTickCount();

  wchar_t path[MAX_PATH] = {0};
  const DWORD n = ::GetModuleFileNameW(self, path, MAX_PATH);
  if (n != 0 && n < MAX_PATH) {
    std::wstring file(path, n);
    const size_t slash = file.find_last_of(L"\\/");
    g_dir = (slash == std::wstring::npos) ? std::wstring(L".") : file.substr(0, slash);
    g_path = g_dir + L"\\sound_ogg_hook.log";
  } else {
    g_dir = L".";
    g_path = L".\\sound_ogg_hook.log";
  }

  g_file = ::CreateFileW(g_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (g_file == INVALID_HANDLE_VALUE) {
    g_path.clear();
  }

  ::InitializeCriticalSection(&g_lock);
  g_lock_ready = true;

  Log("sound_ogg_hook: process start, module dir = %ls", g_dir.c_str());
  Log("log file: %ls", g_path.empty() ? L"(unavailable; OutputDebugString only)" : g_path.c_str());
}

void Log(const char* fmt, ...) {
  char body[2048];
  va_list args;
  va_start(args, fmt);
  _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, args);
  va_end(args);

  char line[2304];
  const DWORD elapsed = ::GetTickCount() - g_process_start;

  std::time_t now = std::time(nullptr);
  std::tm tm_buf{};
  localtime_s(&tm_buf, &now);

  _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02d:%02d:%02d %6lu.%03lu] %s\r\n",
              tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, elapsed / 1000, elapsed % 1000, body);

  if (g_lock_ready) ::EnterCriticalSection(&g_lock);
  WriteAll(line, (int)::strlen(line));
  if (g_lock_ready) ::LeaveCriticalSection(&g_lock);
}

const std::wstring& ModuleDir() { return g_dir; }
const std::wstring& LogPath() { return g_path; }

}  // namespace oggsound
