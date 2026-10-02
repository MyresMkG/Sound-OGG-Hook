#include "image.h"

#include <cstring>

#include "log.h"

namespace oggsound {
namespace {

// MinGW's IMAGE_NT_HEADERS has no SectionHeader member, so the first section
// header is computed the same way IMAGE_FIRST_SECTION does.
const IMAGE_SECTION_HEADER* FirstSection(const IMAGE_NT_HEADERS* nt) {
  return reinterpret_cast<const IMAGE_SECTION_HEADER*>(
      reinterpret_cast<const uint8_t*>(&nt->OptionalHeader) + nt->FileHeader.SizeOfOptionalHeader);
}

}  // namespace

bool Image::InitFromModule(HMODULE module) {
  base_ = reinterpret_cast<const uint8_t*>(module);
  if (base_ == nullptr) return false;
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base_);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  return InitFromHeader(base_, SIZE_MAX);
}

bool Image::InitFromFile(const std::wstring& path) {
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    Log("image: cannot open %ls (error %lu)", path.c_str(), ::GetLastError());
    return false;
  }
  LARGE_INTEGER size{};
  ::GetFileSizeEx(file, &size);
  if (size.QuadPart <= 0 || size.QuadPart > (1LL << 31)) {
    ::CloseHandle(file);
    Log("image: %ls has an implausible size", path.c_str());
    return false;
  }
  std::vector<uint8_t> file_bytes((size_t)size.QuadPart);
  DWORD got = 0;
  const BOOL ok = ::ReadFile(file, file_bytes.data(), (DWORD)file_bytes.size(), &got, nullptr);
  ::CloseHandle(file);
  if (!ok || got != file_bytes.size()) {
    Log("image: short read on %ls", path.c_str());
    return false;
  }

  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file_bytes.data());
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    Log("image: %ls is not a PE image", path.c_str());
    return false;
  }
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(file_bytes.data() + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    Log("image: %ls has no NT signature", path.c_str());
    return false;
  }

  // Lay the sections out by RVA so that every other lookup behaves like a live
  // module; only the raw bytes are copied, nothing else is needed for reading.
  const IMAGE_SECTION_HEADER* headers = FirstSection(nt);
  uint32_t span = 0;
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    const auto& s = headers[i];
    const uint32_t section_end =
        s.VirtualAddress + (std::max)((uint32_t)s.Misc.VirtualSize, (uint32_t)s.SizeOfRawData);
    span = (std::max)(span, section_end);
  }
  owned_.assign(span + 0x1000, 0);
  // The headers live at RVA 0 and are needed by InitFromHeader below.
  const size_t headers_size = (std::min<size_t>)(nt->OptionalHeader.SizeOfHeaders,
                                                file_bytes.size());
  std::memcpy(owned_.data(), file_bytes.data(), headers_size);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    const auto& s = headers[i];
    const uint32_t copy = (std::min)(s.SizeOfRawData, s.Misc.VirtualSize ? s.Misc.VirtualSize
                                                                        : s.SizeOfRawData);
    if (s.PointerToRawData + copy <= file_bytes.size()) {
      std::memcpy(owned_.data() + s.VirtualAddress, file_bytes.data() + s.PointerToRawData, copy);
    }
  }
  base_ = owned_.data();
  return InitFromHeader(owned_.data(), owned_.size());
}

bool Image::InitFromHeader(const uint8_t* data, size_t size) {
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(data);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(data + dos->e_lfanew);
  if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
    Log("image: not a PE32+ image");
    return false;
  }
  if (size != SIZE_MAX && (size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS) > size) {
    Log("image: header runs past the buffer");
    return false;
  }
  image_base_ = nt->OptionalHeader.ImageBase;
  image_size_ = nt->OptionalHeader.SizeOfImage;

  const IMAGE_SECTION_HEADER* headers = FirstSection(nt);
  sections_.clear();
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    const auto& s = headers[i];
    Section out{};
    std::memcpy(out.name, s.Name, 8);
    out.name[8] = 0;
    out.va = s.VirtualAddress;
    out.vsize = s.Misc.VirtualSize;
    out.raw = s.PointerToRawData;
    out.rawsize = s.SizeOfRawData;
    out.characteristics = s.Characteristics;
    sections_.push_back(out);
  }

  functions_.clear();
  if (const Section* s = SectionByName(".pdata")) {
    const uint32_t count = s->vsize / 12;
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* p = At(s->va + i * 12, 12);
      if (p == nullptr) break;
      uint32_t b = 0, e = 0, u = 0;
      std::memcpy(&b, p, 4);
      std::memcpy(&e, p + 4, 4);
      std::memcpy(&u, p + 8, 4);
      if (b != 0 && e > b) functions_.push_back({b, e, u});
    }
    // The table is emitted sorted already; keep it that way defensively.
    for (size_t i = 1; i < functions_.size(); ++i) {
      for (size_t j = i; j > 0 && functions_[j][0] < functions_[j - 1][0]; --j) {
        std::swap(functions_[j], functions_[j - 1]);
      }
    }
  }
  Log("image: base 0x%llx, %zu sections, %zu .pdata functions", (unsigned long long)image_base_,
      sections_.size(), functions_.size());
  return true;
}

const Section* Image::SectionContaining(uint32_t rva) const {
  for (const Section& s : sections_) {
    const uint32_t span = (std::max)(s.vsize, s.rawsize);
    if (rva >= s.va && rva < s.va + span) return &s;
  }
  return nullptr;
}

const Section* Image::SectionByName(const char* name) const {
  for (const Section& s : sections_) {
    if (std::strcmp(s.name, name) == 0) return &s;
  }
  return nullptr;
}

const uint8_t* Image::At(uint32_t rva, size_t size) const {
  if (base_ == nullptr) return nullptr;
  if (rva >= image_size_ || size > image_size_ || rva + size > image_size_) return nullptr;
  return base_ + rva;
}

bool Image::IsExecutable(uint32_t rva) const {
  const Section* s = SectionContaining(rva);
  return s != nullptr && (s->characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
}

bool Image::IsWritable(uint32_t rva) const {
  const Section* s = SectionContaining(rva);
  return s != nullptr && (s->characteristics & IMAGE_SCN_MEM_WRITE) != 0;
}

bool Image::FunctionBounds(uint32_t rva, uint32_t* lo, uint32_t* hi) const {
  size_t a = 0, b = functions_.size();
  while (a < b) {
    const size_t mid = (a + b) / 2;
    if (rva < functions_[mid][0]) {
      b = mid;
    } else if (rva >= functions_[mid][1]) {
      a = mid + 1;
    } else {
      *lo = functions_[mid][0];
      *hi = functions_[mid][1];
      return true;
    }
  }
  return false;
}

uint32_t Image::FindBytes(const void* needle, size_t len, bool skip_exec) const {
  if (base_ == nullptr || len == 0 || len > image_size_) return 0;
  for (const Section& s : sections_) {
    if (skip_exec && (s.characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) continue;
    if (s.characteristics & IMAGE_SCN_MEM_DISCARDABLE) continue;
    const uint32_t span = (std::max)(s.vsize, s.rawsize);
    if (span < len) continue;
    const uint8_t* start = At(s.va, 1);
    if (start == nullptr) continue;
    const uint8_t* end = start + span - len;
    for (const uint8_t* p = start; p <= end; ++p) {
      if (p[0] == ((const uint8_t*)needle)[0] && std::memcmp(p, needle, len) == 0) {
        return s.va + (uint32_t)(p - start);
      }
    }
  }
  return 0;
}

}  // namespace oggsound
