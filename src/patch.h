// Replacing one dynapi trampoline with a jump to our detour.
//
// A trampoline is `48 FF 25 <disp32>` (7 bytes) followed by 0xCC padding up to
// the next 16-byte boundary, so a 14-byte absolute jump fits inside the stub's
// own slot and nothing else has to be relocated. The original instruction is
// not needed in a trampoline of our own: our detour reaches the real SDL
// implementation by reading the slot the stub used to jump through, which also
// keeps working when SDL's bootstrap replaces the slot's content later.
//
// The write is split so that a thread which is executing the stub at that
// moment never sees a half-written instruction: the six trailing address bytes
// go in first (they are padding, the old instruction does not touch them), then
// the leading eight bytes go in with a single interlocked store, which is why
// the stub must be 8-byte aligned. The patch is never taken back out: the DLL
// lives as long as the process does.
#pragma once

#include <cstdint>

#include "image.h"

namespace oggsound {

struct Patch {
  uint32_t rva = 0;
  uint8_t original[16] = {0};
  bool installed = false;
};

// |is_live| must only be set for the process' own mapped module.
bool InstallAbsoluteJump(const Image& img, uint32_t rva, const void* destination, Patch* out);

}  // namespace oggsound
