// Reading through the RWops the game hands us, without disturbing it.
//
// Everything here is written so that a caller who decides not to handle the
// stream can fall back to the real SDL loader with the position rewound to 0,
// which is where SDL expects a freshly opened file to be.
#pragma once

#include <cstdint>
#include <vector>

#include "audio.h"
#include "rwops.h"

namespace oggsound {

// Reads four bytes from the start and rewinds.
bool PeekMagic(const RwopsLayout& layout, void* src, uint8_t magic[4]);

// Size plus the leading bytes, rewinded afterwards. |total_size| is -1 when the
// stream cannot report its size.
void FingerprintStream(const RwopsLayout& layout, void* src, uint8_t* prefix, size_t* prefix_len,
                       int64_t* total_size);

// Whole stream from position 0, rewinded afterwards.
bool ReadStream(const RwopsLayout& layout, void* src, int64_t known_size,
                std::vector<uint8_t>* out);

}  // namespace oggsound
