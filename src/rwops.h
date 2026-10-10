// The SDL_RWops the game hands to SDL_LoadWAV_RW, and a memory-backed one we
// hand back to the real loader.
//
// HOI4 uses the layout with a `size` callback at the front. We recognise the live
// RWops from its callback pointers plus its type tag. That check also
// doubles as a guard - if the layout is ever different again, we simply do not
// touch the stream and the call falls through to SDL unchanged.
#pragma once

#include <cstdint>

#include "image.h"

namespace oggsound {

struct RwopsLayout {
  uint32_t size_off = 0;
  uint32_t seek_off = 0;
  uint32_t read_off = 0;
  uint32_t write_off = 0;
  uint32_t close_off = 0;
  uint32_t type_off = 0;
  uint32_t hidden_off = 0;
  bool legacy_seek = false;  // seek returned int, not Sint64
};

// Recognises the layout of |ops|; returns false when neither known layout fits.
bool DetectLayout(const Image& img, void* ops, RwopsLayout* out);

int64_t RwSeek(const RwopsLayout& layout, void* ops, int64_t offset, int whence);
size_t RwRead(const RwopsLayout& layout, void* ops, void* dst, size_t size, size_t maxnum);
int RwClose(const RwopsLayout& layout, void* ops);

// SDL_RWops over a buffer we own. Used to feed a synthesised WAV to the real
// SDL_LoadWAV_RW, so that SDL allocates the PCM buffer exactly as it would for
// a file and the game keeps freeing it with SDL_FreeWAV.
class MemoryRwops {
 public:
  static const uint32_t kStorageSize = 0x48;

  void Init(const RwopsLayout& layout, const uint8_t* data, size_t size);
  void* ops() { return storage_; }

 private:
  // Win64 has a single calling convention, so SDL's SDLCALL is the default one.
  static int64_t SizeCb(void* ops);
  static int64_t SeekCb(void* ops, int64_t offset, int whence);
  static size_t ReadCb(void* ops, void* ptr, size_t size, size_t maxnum);
  static int CloseCb(void* ops);

  static MemoryRwops* Self(void* ops);

  uint8_t storage_[kStorageSize];
  uint32_t magic_ = 0;
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  size_t pos_ = 0;
};

}  // namespace oggsound
