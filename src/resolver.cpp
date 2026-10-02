#include "resolver.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "log.h"

namespace oggsound {
namespace {

// Strings that live in the sound loader. The first one is unique to it; the
// others are fallbacks for builds where that text was edited.
const char* kAnchors[] = {
    "Failed to load audio \"%s\". SDL Error: \"%s\"\n",
    "For best performance and quality sound files should be in %.1fkHz (%s)",
    "SDL failed allocation in pdx_audio. SDL Error: \"%s\"\n Tried to allocate %d times %d",
    "No valid sound files for sound '%s'",
};

inline uint32_t ReadU32(const uint8_t* p) {
  uint32_t v = 0;
  std::memcpy(&v, p, 4);
  return v;
}

inline int32_t ReadI32(const uint8_t* p) {
  int32_t v = 0;
  std::memcpy(&v, p, 4);
  return v;
}

inline uint64_t ReadU64(const uint8_t* p) {
  uint64_t v = 0;
  std::memcpy(&v, p, 8);
  return v;
}

// Opcodes (after any prefixes) that can carry a rip-relative memory operand.
bool HasRipModRm(uint8_t opcode) {
  switch (opcode) {
    case 0x01: case 0x03: case 0x09: case 0x0B: case 0x11: case 0x13:
    case 0x19: case 0x1B: case 0x21: case 0x23: case 0x29: case 0x2B:
    case 0x31: case 0x33: case 0x39: case 0x3B: case 0x84: case 0x85:
    case 0x86: case 0x87: case 0x88: case 0x89: case 0x8A: case 0x8B:
    case 0x8D: case 0x8F: case 0xC6: case 0xC7: case 0xFF:
      return true;
    default:
      return false;
  }
}

bool HasRipModRmTwoByte(uint8_t second) {
  switch (second) {
    case 0x10: case 0x11: case 0x28: case 0x29: case 0x2E: case 0x2F:
    case 0x54: case 0x55: case 0x57: case 0x58: case 0x59: case 0x5C:
    case 0x5D: case 0x5E: case 0x5F: case 0x6E: case 0x6F: case 0x7E:
    case 0x7F: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16:
    case 0x17: case 0xB6: case 0xB7: case 0xBE: case 0xBF: case 0xD6:
    case 0xEF:
      return true;
    default:
      return false;
  }
}

// Every rip-relative reference to |target_va| inside .text.
std::vector<uint32_t> RefsTo(const Image& img, uint64_t target_va) {
  std::vector<uint32_t> hits;
  const Section* text = img.SectionByName(".text");
  if (text == nullptr) return hits;

  const uint8_t* data = img.At(text->va, 1);
  if (data == nullptr) return hits;
  const uint32_t start = text->va;
  uint32_t end = text->va + text->vsize;
  if (end > img.image_size()) end = img.image_size();

  for (uint32_t rva = start; rva + 16 < end;) {
    const uint32_t i = rva - start;
    const uint8_t b0 = data[i];
    // Cheap filter: only prefix bytes can start one of the encodings we want.
    if ((b0 < 0x40 || b0 > 0x4F) && b0 != 0x0F && b0 != 0x66 && b0 != 0xF2 && b0 != 0xF3) {
      ++rva;
      continue;
    }
    uint32_t p = 0;
    for (int guard = 0; guard < 4; ++guard) {
      const uint8_t b = data[i + p];
      if (b == 0x66 || b == 0xF2 || b == 0xF3 || b == 0x2E || b == 0x3E || b == 0x26 ||
          b == 0x36 || b == 0x64 || b == 0x65) {
        ++p;
        continue;
      }
      if (b >= 0x40 && b <= 0x4F) {
        ++p;
        continue;
      }
      break;
    }
    const uint8_t op = data[i + p];
    int modrm_at = -1;
    if (HasRipModRm(op)) {
      modrm_at = (int)p + 1;
    } else if (op == 0x0F && HasRipModRmTwoByte(data[i + p + 1])) {
      modrm_at = (int)p + 2;
    }
    if (modrm_at < 0) {
      ++rva;
      continue;
    }
    const uint8_t modrm = data[i + modrm_at];
    if ((modrm & 0xC7) != 0x05) {  // mod=00 rm=101 -> RIP + disp32
      ++rva;
      continue;
    }
    const uint32_t insn_len = (uint32_t)modrm_at + 5;
    const int32_t disp = ReadI32(data + i + modrm_at + 1);
    const uint64_t target = img.image_base() + rva + insn_len + (int64_t)disp;
    if (target == target_va) hits.push_back(rva);
    rva += insn_len;
  }
  return hits;
}

// `mov qword ptr [rsp + 0x20], reg` - the 5th argument of a Win64 call.
bool IsStoreRsp20(const uint8_t* b) {
  return (b[0] == 0x48 || b[0] == 0x4C) && b[1] == 0x89 && (b[2] & 0xC7) == 0x44 &&
         b[3] == 0x24 && b[4] == 0x20;
}

// `test rax, rax`, which the loader runs on the result of the load call. A
// couple of unrelated instructions are tolerated in between.
bool TesRaxSoonAfter(const Image& img, uint32_t rva, uint32_t limit) {
  for (uint32_t i = 0; i < limit; ++i) {
    const uint8_t* p = img.At(rva + i, 3);
    if (p == nullptr) return false;
    if (p[0] == 0x48 && p[1] == 0x85 && p[2] == 0xC0) return true;
  }
  return false;
}

}  // namespace

bool StubSlot(const Image& img, uint32_t stub_rva, uint32_t* slot_rva, uint64_t* slot_value) {
  const uint8_t* p = img.At(stub_rva, 7);
  if (p == nullptr) return false;
  if (p[0] != 0x48 || p[1] != 0xFF || p[2] != 0x25) return false;  // jmp qword ptr [rip+disp32]
  const int32_t disp = ReadI32(p + 3);
  const uint32_t slot = stub_rva + 7 + (int64_t)disp;
  const uint8_t* slot_bytes = img.At(slot, 8);
  if (slot_bytes == nullptr) return false;
  if (!img.IsWritable(slot)) return false;
  if (slot_rva != nullptr) *slot_rva = slot;
  if (slot_value != nullptr) *slot_value = ReadU64(slot_bytes);
  return true;
}

bool ResolveLoadWav(const Image& img, Resolution* out) {
  *out = Resolution{};

  for (const char* anchor : kAnchors) {
    const uint32_t str_rva = img.FindBytes(anchor, std::strlen(anchor) + 1, /*skip_exec=*/true);
    if (str_rva == 0) {
      Log("resolve: anchor not present: %.60s", anchor);
      continue;
    }
    const std::vector<uint32_t> refs = RefsTo(img, img.image_base() + str_rva);
    if (refs.empty()) {
      Log("resolve: no code reference to the anchor at rva 0x%x", str_rva);
      continue;
    }
    for (uint32_t ref : refs) {
      uint32_t lo = 0, hi = 0;
      if (!img.FunctionBounds(ref, &lo, &hi)) continue;
      if (hi - lo < 256) continue;  // the loader is ~1.3 kB; a stub would not be

      // Step 2: the call whose 5th argument went to [rsp+0x20] a couple of
      // instructions earlier, whose target is a dynapi stub, and whose result is
      // immediately tested. The window is in bytes because this walk is a byte
      // scan, not a decoder: the argument setup is ~22 bytes in both builds.
      uint32_t last_store = 0xFFFFFFFFu;
      uint32_t found_call = 0, found_stub = 0;
      for (uint32_t rva = lo; rva + 6 < hi; ++rva) {
        const uint8_t* b = img.At(rva, 6);
        if (b == nullptr) break;
        if (IsStoreRsp20(b)) {
          last_store = rva;
          rva += 4;  // skip the rest of the matched pattern
          continue;
        }
        if (b[0] != 0xE8 || last_store == 0xFFFFFFFFu || rva - last_store > 64) continue;
        const int32_t rel = ReadI32(b + 1);
        const uint32_t target = rva + 5 + (int64_t)rel;
        const uint8_t* t = img.At(target, 8);
        if (t != nullptr && t[0] == 0x48 && t[1] == 0xFF && t[2] == 0x25 &&
            TesRaxSoonAfter(img, rva + 5, 12)) {
          found_call = rva;
          found_stub = target;
          break;
        }
      }

      if (found_stub == 0) {
        Log("resolve: function 0x%x..0x%x (anchor %.40s) has no SDL_LoadWAV_RW call", lo, hi,
            anchor);
        continue;
      }

      uint32_t slot = 0;
      uint64_t slot_value = 0;
      if (!StubSlot(img, found_stub, &slot, &slot_value)) {
        Log("resolve: call at 0x%x targets 0x%x, which is not a dynapi stub", found_call,
            found_stub);
        continue;
      }

      out->loader_rva = lo;
      out->loader_end = hi;
      out->call_rva = found_call;
      out->stub_rva = found_stub;
      out->slot_rva = slot;
      out->slot_value = slot_value;
      out->slot_target_rva = (slot_value > img.image_base() &&
                              slot_value < img.image_base() + img.image_size())
                                 ? slot_value - img.image_base()
                                 : 0;
      out->anchor = anchor;

      Log("resolve: anchor \"%.50s\"", anchor);
      Log("resolve:   string rva 0x%x, referenced from rva 0x%x", str_rva, ref);
      Log("resolve:   sound loader 0x%x..0x%x (size %u)", lo, hi, hi - lo);
      Log("resolve:   SDL_LoadWAV_RW call at 0x%x -> stub 0x%x -> slot 0x%x", found_call,
          found_stub, slot);
      Log("resolve:   slot holds 0x%llx (rva 0x%llx) %s", (unsigned long long)slot_value,
          (unsigned long long)out->slot_target_rva,
          out->slot_target_rva != 0 ? "(image code: SDL is initialised)"
                                    : "(outside the image: SDL not initialised yet)");
      return true;
    }
    Log("resolve: anchor \"%.50s\" is referenced, but no candidate function matched", anchor);
  }
  return false;
}

namespace {

bool FunctionContainsRiffIds(const Image& img, uint32_t lo, uint32_t hi) {
  bool riff = false;
  bool wave = false;
  for (uint32_t rva = lo; rva + 4 <= hi; ++rva) {
    const uint8_t* p = img.At(rva, 4);
    if (p == nullptr) break;
    if (!riff && p[0] == 0x52 && p[1] == 0x49 && p[2] == 0x46 && p[3] == 0x46) riff = true;
    if (!wave && p[0] == 0x57 && p[1] == 0x41 && p[2] == 0x56 && p[3] == 0x45) wave = true;
    if (riff && wave) return true;
  }
  return false;
}

}  // namespace

bool LooksLikeWaveLoader(const Image& img, uint32_t fn_rva, int depth) {
  uint32_t lo = 0, hi = 0;
  if (!img.FunctionBounds(fn_rva, &lo, &hi)) return false;
  if (FunctionContainsRiffIds(img, lo, hi)) return true;
  if (depth <= 0) return false;

  // Follow direct calls and jumps...
  for (uint32_t rva = lo; rva + 5 <= hi; ++rva) {
    const uint8_t* p = img.At(rva, 5);
    if (p == nullptr) break;
    if (p[0] != 0xE8 && p[0] != 0xE9) continue;
    const uint32_t target = rva + 5 + (int64_t)ReadI32(p + 1);
    if (target == 0 || target == fn_rva) continue;
    if (!img.IsExecutable(target)) continue;
    if (LooksLikeWaveLoader(img, target, depth - 1)) return true;
  }
  // ...and the function placed right behind it: SDL's SDL_LoadWAV_RW validates
  // its arguments and then falls through into the loader proper, which is a
  // separate .pdata entry.
  return LooksLikeWaveLoader(img, hi, depth - 1);
}

bool FunctionJumpsThroughSlot(const Image& img, uint32_t fn_rva, uint32_t slot_rva) {
  uint32_t lo = 0, hi = 0;
  if (!img.FunctionBounds(fn_rva, &lo, &hi)) return false;
  for (uint32_t rva = lo; rva + 7 <= hi; ++rva) {
    const uint8_t* p = img.At(rva, 7);
    if (p == nullptr) break;
    if (p[0] != 0x48 || p[1] != 0xFF || p[2] != 0x25) continue;
    if (rva + 7 + (int64_t)ReadI32(p + 3) == slot_rva) return true;
  }
  return false;
}

uint32_t FindInstalledImplementation(const Image& img, uint32_t slot_rva) {
  const Section* text = img.SectionByName(".text");
  if (text == nullptr) return 0;
  uint32_t end = text->va + text->vsize;
  if (end > img.image_size()) end = img.image_size();

  for (uint32_t rva = text->va; rva + 7 <= end; ++rva) {
    const uint8_t* p = img.At(rva, 7);
    if (p == nullptr) break;
    if (p[0] != 0x48 || p[1] != 0x89) continue;
    if ((p[2] & 0xC7) != 0x05) continue;  // mov qword ptr [rip+disp32], reg
    if (rva + 7 + (int64_t)ReadI32(p + 3) != slot_rva) continue;
    // The value comes from the closest `lea reg, [rip+disp32]` before the store.
    for (uint32_t back = 0; back < 32 && rva >= back + 7; ++back) {
      const uint32_t at = rva - back - 7;
      const uint8_t* q = img.At(at, 7);
      if (q == nullptr) break;
      if (q[0] == 0x48 && q[1] == 0x8D && (q[2] & 0xC7) == 0x05) {
        const uint32_t source = at + 7 + (int64_t)ReadI32(q + 3);
        if (img.IsExecutable(source)) return source;
      }
    }
  }
  return 0;
}

namespace {

struct ScanCandidate {
  uint32_t loader_rva = 0;
  uint32_t loader_end = 0;
  uint32_t call_rva = 0;
  uint32_t stub_rva = 0;
  uint32_t implementation = 0;
};

// The address a stub effectively reaches: its slot's value, or - while SDL has
// not installed the real one yet - what the installer is going to write there.
uint32_t EffectiveImplementation(const Image& img, uint32_t stub_rva) {
  uint32_t slot = 0;
  uint64_t value = 0;
  if (!StubSlot(img, stub_rva, &slot, &value)) return 0;
  if (value < img.image_base() || value >= img.image_base() + img.image_size()) return 0;
  const uint32_t target = (uint32_t)(value - img.image_base());
  if (FunctionJumpsThroughSlot(img, target, slot)) {
    return FindInstalledImplementation(img, slot);
  }
  return target;
}

}  // namespace

bool ResolveByScan(const Image& img, Resolution* out) {
  std::vector<ScanCandidate> candidates;

  for (const std::array<uint32_t, 3>& fn : img.Functions()) {
    const uint32_t lo = fn[0];
    const uint32_t hi = fn[1];
    if (hi - lo < 48) continue;
    const uint8_t* code = img.At(lo, hi - lo);
    if (code == nullptr) continue;
    const uint32_t size = hi - lo;

    // Tight byte scan over the raw pointer; the expensive lookups only happen
    // where the 5th-argument store actually occurs.
    for (uint32_t off = 0; off + 6 < size; ++off) {
      if (!IsStoreRsp20(code + off)) continue;
      const uint32_t rva = lo + off;
      const uint32_t limit = (std::min)(hi - 6, rva + 64);
      for (uint32_t at = rva + 5; at <= limit; ++at) {
        const uint8_t* call = img.At(at, 6);
        if (call == nullptr || call[0] != 0xE8) continue;
        const uint32_t target = at + 5 + (int64_t)ReadI32(call + 1);
        const uint8_t* t = img.At(target, 8);
        if (t == nullptr || t[0] != 0x48 || t[1] != 0xFF || t[2] != 0x25) continue;
        if (!TesRaxSoonAfter(img, at + 5, 12)) continue;
        ScanCandidate candidate;
        candidate.loader_rva = lo;
        candidate.loader_end = hi;
        candidate.call_rva = at;
        candidate.stub_rva = target;
        candidates.push_back(candidate);
        break;
      }
    }
  }

  // Two stores in front of the same call would produce the same candidate twice.
  std::sort(candidates.begin(), candidates.end(),
            [](const ScanCandidate& a, const ScanCandidate& b) {
              return a.call_rva != b.call_rva ? a.call_rva < b.call_rva : a.stub_rva < b.stub_rva;
            });
  candidates.erase(std::unique(candidates.begin(), candidates.end(),
                               [](const ScanCandidate& a, const ScanCandidate& b) {
                                 return a.call_rva == b.call_rva && a.stub_rva == b.stub_rva;
                               }),
                   candidates.end());

  Log("scan: %zu candidate call(s) with a 5th stack argument to a dynapi trampoline", candidates.size());
  uint32_t winner = 0;
  int passing = 0;
  for (ScanCandidate& candidate : candidates) {
    uint32_t slot = 0;
    uint64_t value = 0;
    if (!StubSlot(img, candidate.stub_rva, &slot, &value)) continue;
    candidate.implementation = EffectiveImplementation(img, candidate.stub_rva);
    const bool wave_like =
        candidate.implementation != 0 && LooksLikeWaveLoader(img, candidate.implementation, 2);
    Log("scan:   func 0x%x..0x%x call 0x%x -> stub 0x%x -> impl 0x%x %s", candidate.loader_rva,
        candidate.loader_end, candidate.call_rva, candidate.stub_rva, candidate.implementation,
        wave_like ? "(RIFF/WAVE: match)" : "(RIFF/WAVE: no)");
    if (wave_like) {
      ++passing;
      winner = candidate.stub_rva;
    }
  }

  if (passing != 1) {
    Log("scan: %d candidate(s) recognise RIFF/WAVE; refusing to guess", passing);
    return false;
  }

  for (const ScanCandidate& candidate : candidates) {
    if (candidate.stub_rva != winner) continue;
    *out = Resolution{};
    out->loader_rva = candidate.loader_rva;
    out->loader_end = candidate.loader_end;
    out->call_rva = candidate.call_rva;
    out->stub_rva = candidate.stub_rva;
    uint32_t slot = 0;
    uint64_t value = 0;
    StubSlot(img, candidate.stub_rva, &slot, &value);
    out->slot_rva = slot;
    out->slot_value = value;
    out->slot_target_rva = (value > img.image_base() && value < img.image_base() + img.image_size())
                               ? value - img.image_base()
                               : 0;
    out->anchor = "(byte-pattern scan)";
    Log("scan: unique match - sound loader 0x%x..0x%x, call 0x%x, stub 0x%x, slot 0x%x",
        out->loader_rva, out->loader_end, out->call_rva, out->stub_rva, out->slot_rva);
    return true;
  }
  return false;
}

}  // namespace oggsound
