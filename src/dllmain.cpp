// DLL entry point.
//
// Installing the hook under the loader lock can deadlock against the game's own
// startup, so everything happens on a worker thread. If anything fails the game
// simply keeps running without the hook, which is also what the log says.
#include <windows.h>

#include <exception>

#include "hook.h"
#include "log.h"

namespace {

DWORD WINAPI Worker(void* self) {
  // Nothing that allocates may escape into the game: an exception crossing the
  // game's own frames would take the process down with it.
  try {
    oggsound::LogInit(static_cast<HMODULE>(self));
    if (!oggsound::InitSoundHook(static_cast<HMODULE>(self))) {
      oggsound::Log("sound_ogg_hook: not installed; see the messages above");
      return 1;
    }
  } catch (const std::exception& error) {
    oggsound::Log("sound_ogg_hook: not installed (%s)", error.what());
    return 1;
  } catch (...) {
    oggsound::Log("sound_ogg_hook: not installed (unknown exception)");
    return 1;
  }
  oggsound::Log("sound_ogg_hook: initialisation complete (probe mode installs no hook)");
  return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    wchar_t host[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, host, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return TRUE;
    const wchar_t* leaf = wcsrchr(host, L'\\');
    leaf = leaf ? leaf + 1 : host;
    if (_wcsicmp(leaf, L"hoi4.exe") != 0) return TRUE;
    DisableThreadLibraryCalls(instance);
    HANDLE thread = CreateThread(nullptr, 0, Worker, instance, 0, nullptr);
    if (thread != nullptr) CloseHandle(thread);
  }
  return TRUE;
}
