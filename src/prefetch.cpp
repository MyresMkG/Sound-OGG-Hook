#include "prefetch.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "audio.h"
#include "log.h"
#include "sound_asset.h"

namespace oggsound {
namespace {

const wchar_t kWorkshopAppId[] = L"394360";

struct Context {
  const Config* config = nullptr;
  Cache* cache = nullptr;

  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::wstring> queue;
  bool scan_done = false;
  std::unordered_set<std::wstring> queued;

  std::atomic<uint64_t> ogg_on_disk{0};
  std::atomic<uint64_t> ogg_referenced{0};
  std::atomic<uint64_t> decoded{0};
  std::atomic<uint64_t> skipped_cached{0};
  std::atomic<uint64_t> skipped_budget{0};
  std::atomic<uint64_t> skipped_big{0};
  std::atomic<uint64_t> failed{0};
  std::atomic<uint64_t> decoded_bytes{0};
  std::atomic<uint64_t> decode_ms{0};
  std::atomic<int> workers_left{0};
};

std::wstring ToLower(std::wstring text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](wchar_t c) { return (wchar_t)towlower(c); });
  return text;
}

bool IsDirectory(const std::wstring& path) {
  const DWORD attributes = ::GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool HasExtension(const std::wstring& path, const wchar_t* ext) {
  const size_t n = wcslen(ext);
  if (path.size() < n) return false;
  return ToLower(path.substr(path.size() - n)) == ext;
}

std::wstring ParentDir(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// Asset files are UTF-8 in practice but not guaranteed to be, and the name has
// to survive the trip to the filesystem either way.
std::wstring WidenPath(const std::string& text) {
  if (text.empty()) return std::wstring();
  UINT code_page = CP_UTF8;
  DWORD flags = MB_ERR_INVALID_CHARS;
  int length = ::MultiByteToWideChar(code_page, flags, text.c_str(), (int)text.size(), nullptr, 0);
  if (length <= 0) {
    code_page = CP_ACP;
    flags = 0;
    length = ::MultiByteToWideChar(code_page, flags, text.c_str(), (int)text.size(), nullptr, 0);
  }
  if (length <= 0) return std::wstring();
  std::wstring out((size_t)length, L'\0');
  ::MultiByteToWideChar(code_page, flags, text.c_str(), (int)text.size(), &out[0], length);
  return out;
}

std::wstring ExeDir() {
  wchar_t buffer[1024] = {0};
  const DWORD n = ::GetModuleFileNameW(nullptr, buffer, ARRAYSIZE(buffer));
  if (n == 0 || n >= ARRAYSIZE(buffer)) return std::wstring();
  return ParentDir(std::wstring(buffer, n));
}

std::wstring UserDataDir() {
  wchar_t buffer[MAX_PATH] = {0};
  if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, buffer))) {
    return std::wstring();
  }
  return std::wstring(buffer) + L"\\Paradox Interactive\\Hearts of Iron IV";
}

std::vector<std::wstring> ListSubdirs(const std::wstring& dir) {
  std::vector<std::wstring> out;
  const std::wstring pattern = JoinPath(dir, L"*");
  WIN32_FIND_DATAW data{};
  HANDLE find = ::FindFirstFileW(pattern.c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return out;
  do {
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
    if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;
    out.push_back(JoinPath(dir, data.cFileName));
  } while (::FindNextFileW(find, &data));
  ::FindClose(find);
  return out;
}

void AddRoot(std::vector<std::wstring>* roots, const std::wstring& path) {
  if (path.empty() || !IsDirectory(path)) return;
  const std::wstring key = ToLower(path);
  for (const std::wstring& existing : *roots) {
    if (ToLower(existing) == key) return;
  }
  roots->push_back(path);
}

void AddSubdirs(std::vector<std::wstring>* roots, const std::wstring& parent) {
  for (const std::wstring& child : ListSubdirs(parent)) AddRoot(roots, child);
}

std::wstring SteamWorkshopDir() {
  const std::wstring exe_dir = ExeDir();
  const std::wstring lowered = ToLower(exe_dir);
  const size_t at = lowered.find(L"\\steamapps\\");
  if (at != std::wstring::npos) {
    return exe_dir.substr(0, at) + L"\\steamapps\\workshop\\content\\" + kWorkshopAppId;
  }
  wchar_t steam_path[MAX_PATH] = {0};
  DWORD size = sizeof(steam_path);
  if (::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ,
                     nullptr, steam_path, &size) == ERROR_SUCCESS) {
    return std::wstring(steam_path) + L"\\steamapps\\workshop\\content\\" + kWorkshopAppId;
  }
  return std::wstring();
}

bool ReadWholeFile(const std::wstring& path, size_t max_bytes, std::vector<uint8_t>* out);

void AddDescriptorRoots(std::vector<std::wstring>* roots, const std::wstring& user_dir) {
  const std::wstring mod_dir = JoinPath(user_dir, L"mod");
  WIN32_FIND_DATAW data{};
  HANDLE find = ::FindFirstFileW(JoinPath(mod_dir, L"*.mod").c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return;
  do {
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
    std::vector<uint8_t> raw;
    if (!ReadWholeFile(JoinPath(mod_dir, data.cFileName), 1u << 20, &raw)) continue;
    for (const std::string& entry : ModDescriptorPaths(std::string(raw.begin(), raw.end()))) {
      std::wstring path = WidenPath(entry);
      std::replace(path.begin(), path.end(), L'/', L'\\');
      if (path.empty()) continue;
      const bool absolute = (path.size() > 2 && path[1] == L':') || path[0] == L'\\';
      if (!absolute) path = JoinPath(user_dir, path.c_str());
      AddRoot(roots, path);
    }
  } while (::FindNextFileW(find, &data));
  ::FindClose(find);
}

std::vector<std::wstring> CollectRoots(const Config& config) {
  std::vector<std::wstring> roots;
  const std::wstring exe_dir = ExeDir();
  AddRoot(&roots, exe_dir);
  AddSubdirs(&roots, JoinPath(exe_dir, L"dlc"));
  AddSubdirs(&roots, JoinPath(exe_dir, L"integrated_dlc"));

  const std::wstring user_dir = UserDataDir();
  AddRoot(&roots, user_dir);
  AddSubdirs(&roots, JoinPath(user_dir, L"mod"));
  AddDescriptorRoots(&roots, user_dir);

  if (config.scan_roots != 0) {
    const std::wstring workshop = SteamWorkshopDir();
    if (!workshop.empty()) AddSubdirs(&roots, workshop);
  }
  return roots;
}

bool QueryFileSize(const std::wstring& path, uint64_t* size_out) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == 0) return false;
  if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return false;
  *size_out = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
  return true;
}

bool ReadWholeFile(const std::wstring& path, size_t max_bytes, std::vector<uint8_t>* out) {
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  if (!::GetFileSizeEx(file, &size) || size.QuadPart <= 0 || (uint64_t)size.QuadPart > max_bytes) {
    ::CloseHandle(file);
    return false;
  }
  out->resize((size_t)size.QuadPart);
  size_t done = 0;
  while (done < out->size()) {
    DWORD got = 0;
    const DWORD want = (DWORD)(std::min<size_t>(out->size() - done, 1u << 20));
    if (!::ReadFile(file, out->data() + done, want, &got, nullptr) || got == 0) break;
    done += got;
  }
  ::CloseHandle(file);
  out->resize(done);
  return !out->empty();
}

// Returns true when this call newly queued the path.
bool Enqueue(Context* ctx, const std::wstring& path) {
  const std::wstring key = ToLower(path);
  std::lock_guard<std::mutex> guard(ctx->mu);
  if (!ctx->queued.insert(key).second) return false;
  ctx->queue.push_back(path);
  ctx->cv.notify_one();
  return true;
}

// The `file = "..."` entries of an asset, resolved to the .ogg files they name.
// Entries inside `music = { ... }` are skipped (see AssetFileEntries): those
// samples are played by the music player, never through the sound loader this
// DLL hooks, so decoding them ahead of time would only fill the cache with
// something that can never be handed out.
void ScanAsset(Context* ctx, const std::wstring& asset_path, const std::wstring& root) {
  std::vector<uint8_t> raw;
  if (!ReadWholeFile(asset_path, 4u << 20, &raw)) return;
  const std::string text((const char*)raw.data(), raw.size());
  const std::wstring asset_dir = ParentDir(asset_path);

  for (const std::string& entry : AssetFileEntries(text)) {
    const std::wstring relative = WidenPath(entry);
    if (relative.empty() || !HasExtension(relative, L".ogg")) continue;
    for (const std::wstring& candidate : AssetEntryCandidates(root, asset_dir, relative)) {
      if (IsDirectory(candidate)) continue;
      if (::GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
      if (Enqueue(ctx, candidate)) ctx->ogg_referenced.fetch_add(1, std::memory_order_relaxed);
      break;  // the first candidate that exists is the file the engine would open
    }
  }
}

void ScanTree(Context* ctx, const std::wstring& root, const std::wstring& dir, int depth) {
  if (depth > 12) return;
  const std::wstring pattern = JoinPath(dir, L"*");
  WIN32_FIND_DATAW data{};
  HANDLE find = ::FindFirstFileW(pattern.c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return;
  do {
    if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
    const std::wstring path = JoinPath(dir, data.cFileName);
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      ScanTree(ctx, root, path, depth + 1);
      continue;
    }
    if (HasExtension(path, L".ogg")) {
      // Only the files an asset points at are queued; a loose ogg that nothing
      // references would never be asked for, so it is counted and left alone.
      ctx->ogg_on_disk.fetch_add(1, std::memory_order_relaxed);
    } else if (HasExtension(path, L".asset")) {
      ScanAsset(ctx, path, root);
    }
  } while (::FindNextFileW(find, &data));
  ::FindClose(find);
}

void ProcessOne(Context* ctx, const std::wstring& path) {
  if (!ctx->cache->HasRoom(64u * 1024u)) {
    ctx->skipped_budget.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const size_t max_bytes = (size_t)ctx->config->prefetch_max_file_mb * 1024u * 1024u;
  uint64_t file_size = 0;
  if (!QueryFileSize(path, &file_size)) {
    ctx->failed.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (file_size == 0 || file_size > max_bytes) {
    ctx->skipped_big.fetch_add(1, std::memory_order_relaxed);
    if (ctx->config->verbose) {
      Log("prefetch: skipping %ls (%.1f MB exceeds prefetch_max_file_mb = %d); it will be decoded "
          "on demand if the game ever loads it",
          path.c_str(), (double)file_size / 1048576.0, ctx->config->prefetch_max_file_mb);
    }
    return;
  }

  std::vector<uint8_t> raw;
  if (!ReadWholeFile(path, max_bytes, &raw)) {
    ctx->failed.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!LooksLikeOgg(raw.data(), raw.size())) return;  // mislabelled, let the game handle it

  const uint64_t fingerprint =
      Fingerprint(raw.data(), std::min(raw.size(), kFingerprintPrefix), (int64_t)raw.size());
  if (ctx->cache->Contains(fingerprint)) {
    ctx->skipped_cached.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // Ask the Vorbis headers how big the decoded buffer will be. Without this a
  // single long file would be decoded (tens to hundreds of MB, plus the decode
  // time) and only then rejected by the budget.
  const uint64_t estimated = EstimateDecodedBytes(raw.data(), raw.size());
  if (estimated != 0 && !ctx->cache->HasRoom((size_t)estimated)) {
    ctx->skipped_budget.fetch_add(1, std::memory_order_relaxed);
    if (ctx->config->verbose) {
      Log("prefetch: skipping %ls (%.1f MB decoded would not fit the remaining cache, %.1f/%.1f MB "
          "used)",
          path.c_str(), (double)estimated / 1048576.0, (double)ctx->cache->bytes() / 1048576.0,
          (double)ctx->cache->budget() / 1048576.0);
    }
    return;
  }

  Pcm pcm;
  std::string error;
  double ms = 0;
  if (!DecodeOgg(raw.data(), raw.size(), &pcm, &error, &ms)) {
    ctx->failed.fetch_add(1, std::memory_order_relaxed);
    if (ctx->config->verbose) Log("prefetch: decode failed for %ls: %s", path.c_str(), error.c_str());
    return;
  }
  const size_t decoded_bytes = pcm.bytes.size();
  const int channels = pcm.channels;
  const int rate = pcm.rate;
  const double seconds = (double)pcm.frames / (double)rate;
  if (!ctx->cache->Put(fingerprint, std::move(pcm))) {
    ctx->skipped_budget.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  ctx->decoded.fetch_add(1, std::memory_order_relaxed);
  ctx->decoded_bytes.fetch_add(decoded_bytes, std::memory_order_relaxed);
  ctx->decode_ms.fetch_add((uint64_t)ms, std::memory_order_relaxed);
  if (ctx->config->verbose) {
    Log("prefetch: %ls -> %d Hz, %d ch, %.1f s, %.1f ms decode, %.2f MB decoded", path.c_str(),
        rate, channels, seconds, ms, (double)decoded_bytes / 1048576.0);
  }
}

// A worker thread that lets an exception escape would take the whole process
// down with it, so every file is decoded inside a guard.
void WorkerMain(Context* ctx) {
  for (;;) {
    std::wstring path;
    {
      std::unique_lock<std::mutex> lock(ctx->mu);
      ctx->cv.wait(lock, [ctx] { return ctx->scan_done || !ctx->queue.empty(); });
      if (ctx->queue.empty()) {
        if (ctx->scan_done) break;
        continue;
      }
      path = std::move(ctx->queue.front());
      ctx->queue.pop_front();
    }
    try {
      ProcessOne(ctx, path);
    } catch (const std::exception& error) {
      ctx->failed.fetch_add(1, std::memory_order_relaxed);
      Log("prefetch: %ls failed with %s; it stays available for on-demand decoding", path.c_str(),
          error.what());
    } catch (...) {
      ctx->failed.fetch_add(1, std::memory_order_relaxed);
      Log("prefetch: %ls failed with an unknown exception", path.c_str());
    }
  }
  ctx->workers_left.fetch_sub(1, std::memory_order_relaxed);
}

void ScannerMain(Config config, Cache* cache, Context* ctx) {
  try {
    const std::vector<std::wstring> roots = CollectRoots(config);
    Log("prefetch: %zu root(s) to scan", roots.size());
    for (const std::wstring& root : roots) {
      const std::wstring sound_dir = JoinPath(root, L"sound");
      if (IsDirectory(sound_dir)) {
        if (config.verbose) Log("prefetch: scanning %ls", sound_dir.c_str());
        ScanTree(ctx, root, sound_dir, 0);
      }
    }
  } catch (const std::exception& error) {
    Log("prefetch: scanning stopped early (%s)", error.what());
  } catch (...) {
    Log("prefetch: scanning stopped early (unknown exception)");
  }

  {
    std::lock_guard<std::mutex> lock(ctx->mu);
    ctx->scan_done = true;
    Log("prefetch: scan finished, %llu .ogg on disk, %llu referenced by sound assets, %zu queued",
        (unsigned long long)ctx->ogg_on_disk.load(),
        (unsigned long long)ctx->ogg_referenced.load(), ctx->queued.size());
  }
  ctx->cv.notify_all();

  // Wait for the workers, then report.
  while (ctx->workers_left.load(std::memory_order_relaxed) > 0) {
    ::Sleep(20);
  }

  const CacheStats stats = cache->stats();
  Log("prefetch: %llu decoded (%.1f MB, %.0f ms total), %llu already cached, %llu skipped over "
      "budget, %llu over prefetch_max_file_mb, %llu failed; cache now %.1f/%.1f MB, %llu evictions",
      (unsigned long long)ctx->decoded.load(), (double)ctx->decoded_bytes.load() / 1048576.0,
      (double)ctx->decode_ms.load(), (unsigned long long)ctx->skipped_cached.load(),
      (unsigned long long)ctx->skipped_budget.load(), (unsigned long long)ctx->skipped_big.load(),
      (unsigned long long)ctx->failed.load(),
      (double)cache->bytes() / 1048576.0, (double)cache->budget() / 1048576.0,
      (unsigned long long)stats.evicted);
  delete ctx;
}

}  // namespace

void StartPrefetch(const Config& config, Cache* cache) {
  if (!config.prefetch || cache == nullptr) {
    Log("prefetch: disabled by configuration");
    return;
  }
  int workers = config.threads;
  if (workers <= 0) {
    const int cores = (int)std::thread::hardware_concurrency();
    workers = std::max(1, std::min(4, cores / 2));
  }
  Log("prefetch: starting %d worker(s)", workers);

  auto* ctx = new Context();
  ctx->config = &config;
  ctx->cache = cache;
  try {
    for (int i = 0; i < workers; ++i) {
      ctx->workers_left.fetch_add(1, std::memory_order_relaxed);
      try {
        std::thread(WorkerMain, ctx).detach();
      } catch (...) {
        ctx->workers_left.fetch_sub(1, std::memory_order_relaxed);
        throw;
      }
    }
    std::thread(ScannerMain, config, cache, ctx).detach();
  } catch (...) {
    {
      std::lock_guard<std::mutex> lock(ctx->mu);
      ctx->scan_done = true;
    }
    ctx->cv.notify_all();
    while (ctx->workers_left.load(std::memory_order_relaxed) != 0) ::Sleep(1);
    delete ctx;
    throw;
  }
}

}  // namespace oggsound
