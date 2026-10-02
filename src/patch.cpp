#include "patch.h"

#include <cstring>

#include "log.h"

namespace oggsound {
namespace {

// The pages a write of |size| bytes at |va| touches, plus their previous
// protection so it can be put back exactly as it was. A 16-byte write at the
// very end of a page spans two of them, so the region is rounded up.
struct WritableRegion {
  void* base = nullptr;
  size_t size = 0;
  DWORD old_protect = 0;
};

bool MakeWritable(void* va, size_t size, WritableRegion* out) {
  SYSTEM_INFO si{};
  ::GetSystemInfo(&si);
  const uintptr_t page = (uintptr_t)si.dwPageSize;
  const uintptr_t start = (uintptr_t)va & ~(page - 1);
  const uintptr_t end = ((uintptr_t)va + size + page - 1) & ~(page - 1);
  out->base = (void*)start;
  out->size = (size_t)(end - start);
  return ::VirtualProtect(out->base, out->size, PAGE_EXECUTE_READWRITE, &out->old_protect) != 0;
}

void RestoreProtection(const WritableRegion& region) {
  if (region.base == nullptr) return;
  DWORD ignored = 0;
  ::VirtualProtect(region.base, region.size, region.old_protect, &ignored);
}

}  // namespace

bool InstallAbsoluteJump(const Image& img, uint32_t rva, const void* destination, Patch* out) {
  const uint8_t* code = img.At(rva, 16);
  if (code == nullptr) {
    Log("patch: rva 0x%x is not readable", rva);
    return false;
  }
  if (code[0] != 0x48 || code[1] != 0xFF || code[2] != 0x25) {
    Log("patch: rva 0x%x is not a `jmp qword ptr [rip+..]` trampoline", rva);
    return false;
  }
  for (int i = 7; i < 14; ++i) {
    if (code[i] != 0xCC) {
      Log("patch: rva 0x%x has no int3 padding after the jump (byte %d = 0x%02x)", rva, i,
          code[i]);
      return false;
    }
  }
  if ((rva & 7) != 0) {
    Log("patch: rva 0x%x is not 8-byte aligned; refusing a non-atomic patch", rva);
    return false;
  }

  out->rva = rva;
  out->installed = false;
  std::memcpy(out->original, code, 16);

  uint8_t patch_bytes[16] = {0};
  patch_bytes[0] = 0xFF;
  patch_bytes[1] = 0x25;
  std::memcpy(patch_bytes + 6, &destination, 8);

  void* target = (void*)(img.image_base() + rva);
  WritableRegion region;
  if (!MakeWritable(target, 16, &region)) {
    Log("patch: VirtualProtect failed at 0x%x (error %lu)", rva, ::GetLastError());
    return false;
  }

  uint8_t* bytes = (uint8_t*)target;
  std::memcpy(bytes + 8, patch_bytes + 8, 6);  // padding: safe to touch first

  uint64_t head = 0;
  std::memcpy(&head, patch_bytes, 8);
  ::InterlockedExchange64((volatile LONG64*)bytes, (LONG64)head);

  ::FlushInstructionCache(::GetCurrentProcess(), bytes, 16);
  RestoreProtection(region);

  out->installed = true;
  Log("patch: hooked rva 0x%x -> %p (absolute jump, slot left alone)", rva, destination);
  return true;
}

}  // namespace oggsound
