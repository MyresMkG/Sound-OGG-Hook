#include "config.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "log.h"

namespace oggsound {
namespace {

std::string Trim(const std::string& text) {
  size_t a = 0;
  size_t b = text.size();
  while (a < b && (text[a] == ' ' || text[a] == '\t' || text[a] == '\r' || text[a] == '\n')) ++a;
  while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\r' ||
                   text[b - 1] == '\n'))
    --b;
  return text.substr(a, b - a);
}

// Values are clamped to what the code can act on: a negative size would turn
// into an enormous unsigned limit, and an unbounded thread count would be taken
// literally. The clamped value is what the log then reports.
void ApplyKey(const std::string& key, const std::string& value, Config* out) {
  const int number = std::atoi(value.c_str());
  if (key == "cache_mb") {
    out->cache_mb = number < 0 ? 0 : number;
  } else if (key == "threads") {
    out->threads = number < 0 ? 0 : (number > 64 ? 64 : number);
  } else if (key == "prefetch") {
    out->prefetch = number != 0;
  } else if (key == "prefetch_max_file_mb") {
    out->prefetch_max_file_mb = number < 0 ? 0 : number;
  } else if (key == "scan_roots") {
    out->scan_roots = number;
  } else if (key == "verbose") {
    out->verbose = number != 0;
  } else if (key == "force_scan") {
    out->force_scan = number != 0;
  } else if (key == "loadwav_stub_rva") {
    out->override_stub_rva = (uint32_t)std::strtoul(value.c_str(), nullptr, 0);
  } else {
    Log("config: unknown key \"%s\" ignored", key.c_str());
  }
}

}  // namespace

std::string Config::summary() const {
  char text[320];
  _snprintf_s(text, sizeof(text), _TRUNCATE,
              "cache %d MB, threads %d, prefetch %s, max file %d MB, roots %d, verbose %d, "
              "force_scan %d",
              cache_mb, threads, prefetch ? "on" : "off", prefetch_max_file_mb, scan_roots,
              verbose ? 1 : 0, force_scan ? 1 : 0);
  return std::string(text);
}

void LoadConfig(const std::wstring& dll_dir, Config* out) {
  const std::wstring path = dll_dir + L"\\sound_ogg_hook.ini";
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    Log("config: %ls not present, using defaults (%s)", path.c_str(), out->summary().c_str());
    return;
  }
  LARGE_INTEGER size{};
  ::GetFileSizeEx(file, &size);
  std::vector<char> bytes((size_t)(size.QuadPart > 0 ? size.QuadPart : 0) + 1, 0);
  DWORD got = 0;
  ::ReadFile(file, bytes.data(), (DWORD)(bytes.size() - 1), &got, nullptr);
  ::CloseHandle(file);
  bytes[got] = 0;

  std::string text(bytes.data(), got);
  if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
      (unsigned char)text[2] == 0xBF) {
    text.erase(0, 3);  // UTF-8 BOM
  }

  size_t pos = 0;
  int applied = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    std::string line = Trim(text.substr(pos, end - pos));
    pos = end + 1;
    if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = Trim(line.substr(0, eq));
    const std::string value = Trim(line.substr(eq + 1));
    if (key.empty()) continue;
    ApplyKey(key, value, out);
    ++applied;
  }
  Log("config: %ls loaded (%d keys): %s", path.c_str(), applied, out->summary().c_str());
}

bool ProbeOnlyRequested(const std::wstring& dll_dir) {
  const wchar_t* flags[] = {L"\\sound_ogg_hook_probe_only.txt",
                            L"\\diplo_action_hook_probe_only.txt"};
  for (const wchar_t* flag : flags) {
    const std::wstring path = dll_dir + flag;
    if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
      Log("probe-only flag present: %ls", path.c_str());
      return true;
    }
  }
  return false;
}

}  // namespace oggsound
