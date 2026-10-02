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

DWORD WINAPI Worker(void*) {
  // Nothing that allocates may escape into the game: an exception crossing the
  // game's own frames would take the process down with it.
  try {
    if (!oggsound::InitSoundHook(nullptr)) {
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
  oggsound::Log("sound_ogg_hook: installed");
  return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
    oggsound::LogInit(instance);
    HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (thread != nullptr) CloseHandle(thread);
  }
  return TRUE;
}
