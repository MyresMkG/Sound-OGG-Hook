// Locating the one hook target we need: the SDL_LoadWAV_RW dynapi trampoline
// that the sound loader calls.
//
// Nothing here is copied between game builds. The chain is rebuilt from the
// binary every time:
//
//   1. a log string that only pdx_audiosound_sdl.cpp prints
//      -> the rip-relative instruction that references it
//      -> the enclosing function (bounds from the exception directory)
//        = AudioInternalSoundLoad
//   2. inside that function, the call whose 5th argument is stored to
//      [rsp+0x20] and which is followed by `test rax, rax` - that is the
//      SDL_LoadWAV_RW call, and its target is the dynapi trampoline stub
//   3. the stub is `jmp qword ptr [rip+disp32]`; the slot it jumps through is
//      where SDL's own bootstrap later installs the real implementation.
//
// Step 2 is the only heuristic one, so it is checked three ways: the argument
// pattern, the trailing `test rax, rax`, and the shape of the target
// (`48 FF 25` plus a readable, writable slot).
#pragma once

#include <cstdint>

#include "image.h"

namespace oggsound {

struct Resolution {
  uint32_t loader_rva = 0;    // AudioInternalSoundLoad
  uint32_t loader_end = 0;
  uint32_t call_rva = 0;      // `call <stub>` inside it
  uint32_t stub_rva = 0;      // the dynapi trampoline that call reaches
  uint32_t slot_rva = 0;      // .data slot the trampoline jumps through
  uint64_t slot_value = 0;    // what the slot held at resolve time
  uint64_t slot_target_rva = 0;  // slot_value as an RVA (0 when outside the image)
  const char* anchor = nullptr;

  bool ok() const { return stub_rva != 0 && slot_rva != 0; }
};

// Reads the stub at |stub_rva| and reports where its jump goes.
bool StubSlot(const Image& img, uint32_t stub_rva, uint32_t* slot_rva, uint64_t* slot_value);

// Runs the whole chain. On failure the log explains which step gave up.
bool ResolveLoadWav(const Image& img, Resolution* out);

// The same target, found without using any log string: scan every function for
// a call that passes a 5th argument on the stack to a dynapi trampoline, resolve
// each trampoline's real implementation through the installer, and keep the one
// that recognises the RIFF/WAVE chunk identifiers. Exactly one candidate must
// survive, otherwise nothing is hooked. This is the fallback for builds whose
// log text was edited or replaced.
bool ResolveByScan(const Image& img, Resolution* out);

// Best-effort confirmation that |fn_rva| is really SDL's WAV parser: its code
// (or a direct callee's, up to |depth| levels) compares against the RIFF/WAVE
// chunk identifiers. Used by probe mode and by the host-side verifier.
bool LooksLikeWaveLoader(const Image& img, uint32_t fn_rva, int depth = 1);

// True when the function at |fn_rva| jumps through |slot_rva|, i.e. it is SDL's
// own bootstrap trampoline rather than a real implementation.
bool FunctionJumpsThroughSlot(const Image& img, uint32_t fn_rva, uint32_t slot_rva);

// The dynapi installer writes the real implementation into a slot with
// `lea rcx, [rip+impl]` + `mov [rip+slot], rcx`; this finds |impl| by looking
// for the store to |slot_rva| and taking the lea just before it. Returns 0 when
// the installer is not present (e.g. a build that only ships the override
// lookup).
uint32_t FindInstalledImplementation(const Image& img, uint32_t slot_rva);

}  // namespace oggsound
