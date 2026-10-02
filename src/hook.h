// The hook itself.
//
// One trampoline in the game's SDL dynapi layer is redirected to DetourLoadWav.
// The detour looks at the first four bytes of the stream it was given:
//
//   * anything that is not Ogg  -> the real SDL_LoadWAV_RW is called with the
//     stream untouched, so WAV loading (and its error reporting) is exactly
//     what it was before this DLL existed;
//   * Ogg that the prefetcher already decoded -> the samples are turned into an
//     in-memory RIFF/WAVE and handed to the real SDL_LoadWAV_RW, which then
//     allocates the PCM buffer with SDL's own allocator and fills |spec| the
//     same way it would for a .wav file. Everything downstream - the
//     SDL_BuildAudioCVT to 44.1 kHz/S16/stereo, SDL_malloc, SDL_ConvertAudio,
//     SDL_FreeWAV - is the game's untouched code path;
//   * Ogg that was not prefetched -> decoded here, on the calling thread.
#pragma once

#include <windows.h>

namespace oggsound {

// Runs on a worker thread, never under the loader lock. Returns false when the
// hook could not be installed, in which case the game runs unmodified.
bool InitSoundHook(HMODULE self);

}  // namespace oggsound
