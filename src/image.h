// Read-only view of a mapped PE64 image.
//
// The same class serves two callers:
//   * the DLL, looking at the live game module (addresses are the real ones), and
//   * the host-side verifier, looking at a stellaris.exe on disk (the file is
//     laid out by RVA into a scratch buffer, so every lookup below works the
//     same way; image_base is still the one from the optional header).
#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace oggsound {

struct Section {
  char name[9];
  uint32_t va;
  uint32_t vsize;
  uint32_t raw;
  uint32_t rawsize;
  uint32_t characteristics;
};

class Image {
 public:
  bool InitFromModule(HMODULE module);
  bool InitFromFile(const std::wstring& path);

  bool valid() const { return base_ != nullptr; }
  uint64_t image_base() const { return image_base_; }
  uint32_t image_size() const { return image_size_; }

  const std::vector<Section>& sections() const { return sections_; }
  const Section* SectionContaining(uint32_t rva) const;
  const Section* SectionByName(const char* name) const;

  const uint8_t* At(uint32_t rva, size_t size = 1) const;
  bool IsExecutable(uint32_t rva) const;
  bool IsWritable(uint32_t rva) const;

  // Exception directory: (begin_rva, end_rva, unwind_rva) sorted by begin.
  const std::vector<std::array<uint32_t, 3>>& Functions() const { return functions_; }
  bool FunctionBounds(uint32_t rva, uint32_t* lo, uint32_t* hi) const;

  // First occurrence of |needle| in either the whole image (skip_exec=false) or
  // in every section that is not executable. Returns 0 when not found.
  uint32_t FindBytes(const void* needle, size_t len, bool skip_exec) const;

  std::string CString(uint32_t rva, size_t limit = 256) const;

 private:
  bool InitFromHeader(const uint8_t* data, size_t size);

  const uint8_t* base_ = nullptr;   // RVA-addressable image memory
  uint64_t image_base_ = 0;
  uint32_t image_size_ = 0;
  std::vector<Section> sections_;
  std::vector<std::array<uint32_t, 3>> functions_;
  std::vector<uint8_t> owned_;      // file-backed copy
};

}  // namespace oggsound
