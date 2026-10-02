#include "rwops.h"

#include <cstring>

#include "log.h"

namespace oggsound {
namespace {

const uint32_t kMemoryMagic = 0x4F67674Du;  // "OggM"

bool PointsIntoCode(const Image& img, uint64_t value) {
  if (value < img.image_base() || value >= img.image_base() + img.image_size()) return false;
  return img.IsExecutable((uint32_t)(value - img.image_base()));
}

template <typename T>
T Load(void* base, uint32_t off) {
  T value{};
  std::memcpy(&value, (uint8_t*)base + off, sizeof(T));
  return value;
}

template <typename T>
void Store(void* base, uint32_t off, T value) {
  std::memcpy((uint8_t*)base + off, &value, sizeof(T));
}

template <typename Fn>
uint64_t FnBits(Fn fn) {
  uint64_t bits = 0;
  static_assert(sizeof(Fn) == sizeof(bits), "function pointers are 8 bytes here");
  std::memcpy(&bits, &fn, sizeof(bits));
  return bits;
}

// A layout fits when its three callbacks hold code and the field at its `type`
// offset holds a small tag. The tag is what tells the two SDL generations apart:
// read with the other generation's layout, one struct's `type` lands on the
// other's `close` pointer, and a code pointer is never a small number.
bool LooksLikeLayout(const Image& img, void* ops, const RwopsLayout& layout) {
  return PointsIntoCode(img, Load<uint64_t>(ops, layout.seek_off)) &&
         PointsIntoCode(img, Load<uint64_t>(ops, layout.read_off)) &&
         PointsIntoCode(img, Load<uint64_t>(ops, layout.close_off)) &&
         Load<uint32_t>(ops, layout.type_off) < 256;
}

}  // namespace

bool DetectLayout(const Image& img, void* ops, RwopsLayout* out) {
  if (ops == nullptr) return false;

  // SDL >= 2.0.20: size, seek, read, write, close, type, hidden.
  RwopsLayout modern{};
  modern.size_off = 0x00;
  modern.seek_off = 0x08;
  modern.read_off = 0x10;
  modern.write_off = 0x18;
  modern.close_off = 0x20;
  modern.type_off = 0x28;
  modern.hidden_off = 0x30;
  modern.legacy_seek = false;

  // SDL < 2.0.20: seek, read, write, close, type, hidden.
  RwopsLayout legacy = modern;
  legacy.size_off = 0xFFFFFFFFu;
  legacy.seek_off = 0x00;
  legacy.read_off = 0x08;
  legacy.write_off = 0x10;
  legacy.close_off = 0x18;
  legacy.type_off = 0x20;
  legacy.hidden_off = 0x28;
  legacy.legacy_seek = true;

  // At most one of the two can fit - the tests above contradict each other on
  // the same bytes - so no ordering has to be guessed at.
  if (LooksLikeLayout(img, ops, modern)) {
    *out = modern;
    return true;
  }
  if (LooksLikeLayout(img, ops, legacy)) {
    Log("rwops: legacy SDL_RWops layout detected (pre-2.0.20)");
    *out = legacy;
    return true;
  }
  return false;
}

int64_t RwSeek(const RwopsLayout& layout, void* ops, int64_t offset, int whence) {
  using SeekFn = int64_t (*)(void*, int64_t, int);
  const SeekFn fn = (SeekFn)Load<uint64_t>(ops, layout.seek_off);
  if (fn == nullptr) return -1;
  const int64_t result = fn(ops, offset, whence);
  return layout.legacy_seek ? (int64_t)(int32_t)result : result;
}

size_t RwRead(const RwopsLayout& layout, void* ops, void* dst, size_t size, size_t maxnum) {
  using ReadFn = size_t (*)(void*, void*, size_t, size_t);
  const ReadFn fn = (ReadFn)Load<uint64_t>(ops, layout.read_off);
  if (fn == nullptr) return 0;
  return fn(ops, dst, size, maxnum);
}

int RwClose(const RwopsLayout& layout, void* ops) {
  using CloseFn = int (*)(void*);
  const CloseFn fn = (CloseFn)Load<uint64_t>(ops, layout.close_off);
  if (fn == nullptr) return 0;
  return fn(ops);
}

void MemoryRwops::Init(const RwopsLayout& layout, const uint8_t* data, size_t size) {
  std::memset(storage_, 0, sizeof(storage_));
  magic_ = kMemoryMagic;
  data_ = data;
  size_ = size;
  pos_ = 0;

  if (layout.size_off != 0xFFFFFFFFu) {
    Store<uint64_t>(storage_, layout.size_off, FnBits(&MemoryRwops::SizeCb));
  }
  Store<uint64_t>(storage_, layout.seek_off, FnBits(&MemoryRwops::SeekCb));
  Store<uint64_t>(storage_, layout.read_off, FnBits(&MemoryRwops::ReadCb));
  Store<uint64_t>(storage_, layout.write_off, (uint64_t)0);
  Store<uint64_t>(storage_, layout.close_off, FnBits(&MemoryRwops::CloseCb));
  Store<uint32_t>(storage_, layout.type_off, 5 /* SDL_RWOPS_MEMORY_RO */);
  Store<uint64_t>(storage_, layout.hidden_off, (uint64_t)(uintptr_t)this);
}

MemoryRwops* MemoryRwops::Self(void* ops) {
  // SDL only ever passes back the pointer it was handed, and storage_ is the
  // first member, so the RWops address is the object address.
  if (ops == nullptr) return nullptr;
  auto* self = reinterpret_cast<MemoryRwops*>(ops);
  if (self->magic_ != kMemoryMagic) return nullptr;
  return self;
}

int64_t MemoryRwops::SizeCb(void* ops) {
  MemoryRwops* self = Self(ops);
  return self != nullptr ? (int64_t)self->size_ : -1;
}

int64_t MemoryRwops::SeekCb(void* ops, int64_t offset, int whence) {
  MemoryRwops* self = Self(ops);
  if (self == nullptr) return -1;
  int64_t target = 0;
  switch (whence) {
    case 0: target = offset; break;                         // SEEK_SET
    case 1: target = (int64_t)self->pos_ + offset; break;   // SEEK_CUR
    case 2: target = (int64_t)self->size_ + offset; break;  // SEEK_END
    default: return -1;
  }
  if (target < 0 || (size_t)target > self->size_) return -1;
  self->pos_ = (size_t)target;
  return target;
}

size_t MemoryRwops::ReadCb(void* ops, void* ptr, size_t size, size_t maxnum) {
  MemoryRwops* self = Self(ops);
  if (self == nullptr || size == 0) return 0;
  const size_t available = self->size_ - self->pos_;
  size_t objects = maxnum;
  if (objects > available / size) objects = available / size;
  if (objects != 0) {
    std::memcpy(ptr, self->data_ + self->pos_, objects * size);
    self->pos_ += objects * size;
  }
  return objects;
}

int MemoryRwops::CloseCb(void* ops) {
  (void)ops;
  return 0;
}

}  // namespace oggsound
