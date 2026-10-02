// Optional INI next to the DLL, plus the probe flag convention shared with
// stellaris_mod_injector (`--probe` drops a file next to each DLL).
#pragma once

#include <cstdint>
#include <string>

namespace oggsound {

struct Config {
  int cache_mb = 256;            // decoded PCM the prefetcher may keep, in MB
  int threads = 0;               // prefetch workers; 0 = auto (cores/2, 1..4)
  bool prefetch = true;          // 0 disables the background decoding
  int prefetch_max_file_mb = 24; // skip prefetching ogg files bigger than this
  int scan_roots = 1;            // 0 = only game dir + user mod dirs, no workshop
  bool verbose = true;           // per-file logging
  bool force_scan = false;       // skip the anchor path entirely (debug/emergency)
  uint32_t override_stub_rva = 0;  // emergency override, e.g. 0x1d99dd0

  std::string summary() const;
};

// Reads <dll_dir>\sound_ogg_hook.ini when present.
void LoadConfig(const std::wstring& dll_dir, Config* out);

// True when a probe-only flag sits next to the DLL (ours, or the injector's).
bool ProbeOnlyRequested(const std::wstring& dll_dir);

}  // namespace oggsound
