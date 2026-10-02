// Host-side self test for everything that does not need the game running:
// WAV synthesis, content fingerprints, the memory RWops, the cache, and a real
// Ogg decode with a throughput report.
//
//   selftest.exe [some.ogg]
//
// Without an argument it looks for a .ogg in the installed game's music folder.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "audio.h"
#include "cache.h"
#include "config.h"
#include "log.h"
#include "rwops.h"
#include "sound_asset.h"
#include "stream.h"

using namespace oggsound;

namespace {

int g_failures = 0;

#define CHECK(cond, ...)             \
  do {                               \
    if (!(cond)) {                   \
      wprintf(L"FAIL: ");            \
      wprintf(__VA_ARGS__);          \
      wprintf(L"\n");                \
      ++g_failures;                  \
    }                                \
  } while (0)

RwopsLayout ModernLayout() {
  RwopsLayout layout{};
  layout.size_off = 0x00;
  layout.seek_off = 0x08;
  layout.read_off = 0x10;
  layout.write_off = 0x18;
  layout.close_off = 0x20;
  layout.type_off = 0x28;
  layout.hidden_off = 0x30;
  return layout;
}

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>* out) {
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  ::GetFileSizeEx(file, &size);
  out->resize((size_t)size.QuadPart);
  DWORD got = 0;
  const BOOL ok = ::ReadFile(file, out->data(), (DWORD)out->size(), &got, nullptr);
  ::CloseHandle(file);
  return ok != 0 && got == out->size();
}

std::wstring FindGameOgg() {
  std::vector<std::wstring> music_dirs;
  wchar_t steam_path[MAX_PATH] = {0};
  DWORD size = sizeof(steam_path);
  if (::RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ,
                     nullptr, steam_path, &size) == ERROR_SUCCESS) {
    music_dirs.push_back(std::wstring(steam_path) + L"\\steamapps\\common\\Stellaris\\music");
  }
  music_dirs.push_back(L"D:\\SteamLibrary\\steamapps\\common\\Stellaris\\music");

  for (const std::wstring& dir : music_dirs) {
    const std::wstring pattern = dir + L"\\*.ogg";
    WIN32_FIND_DATAW data{};
    HANDLE find = ::FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) continue;
    const std::wstring found = dir + L"\\" + data.cFileName;
    ::FindClose(find);
    return found;
  }
  return std::wstring();
}

void TestWav() {
  Pcm pcm;
  pcm.channels = 2;
  pcm.rate = 44100;
  pcm.frames = 4;
  pcm.bytes = {1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 7, 0, 8, 0};
  std::vector<uint8_t> wav;
  BuildWav(pcm, &wav);
  CHECK(wav.size() == 44 + pcm.bytes.size(), L"wav size %zu", wav.size());
  CHECK(memcmp(wav.data(), "RIFF", 4) == 0, L"RIFF tag");
  CHECK(memcmp(wav.data() + 8, "WAVE", 4) == 0, L"WAVE tag");
  CHECK(memcmp(wav.data() + 12, "fmt ", 4) == 0, L"fmt tag");
  CHECK(memcmp(wav.data() + 36, "data", 4) == 0, L"data tag");
  const uint32_t riff_size = *(const uint32_t*)(wav.data() + 4);
  const uint32_t data_size = *(const uint32_t*)(wav.data() + 40);
  CHECK(riff_size == 36 + pcm.bytes.size(), L"riff size %u", riff_size);
  CHECK(data_size == pcm.bytes.size(), L"data size %u", data_size);
  CHECK(*(const uint16_t*)(wav.data() + 20) == 1, L"PCM format tag");
  CHECK(*(const uint16_t*)(wav.data() + 22) == 2, L"channels");
  CHECK(*(const uint32_t*)(wav.data() + 24) == 44100, L"rate");
  CHECK(*(const uint16_t*)(wav.data() + 34) == 16, L"bits");
  CHECK(memcmp(wav.data() + 44, pcm.bytes.data(), pcm.bytes.size()) == 0, L"payload");
}

void TestFingerprint() {
  std::vector<uint8_t> data(200000);
  for (size_t i = 0; i < data.size(); ++i) data[i] = (uint8_t)(i * 31 + 7);
  const uint64_t a = Fingerprint(data.data(), kFingerprintPrefix, (int64_t)data.size());
  const uint64_t b = Fingerprint(data.data(), kFingerprintPrefix, (int64_t)data.size());
  const uint64_t c = Fingerprint(data.data(), kFingerprintPrefix, (int64_t)data.size() - 1);
  std::vector<uint8_t> other = data;
  other[100] ^= 0xFF;
  const uint64_t d = Fingerprint(other.data(), kFingerprintPrefix, (int64_t)other.size());
  CHECK(a == b, L"fingerprint is stable");
  CHECK(a != c, L"fingerprint depends on the size");
  CHECK(a != d, L"fingerprint depends on the content");
}

void TestMemoryRwops() {
  std::vector<uint8_t> data(1000);
  for (size_t i = 0; i < data.size(); ++i) data[i] = (uint8_t)i;

  const RwopsLayout layout = ModernLayout();
  MemoryRwops mem;
  mem.Init(layout, data.data(), data.size());

  CHECK(RwSeek(layout, mem.ops(), 0, 2) == (int64_t)data.size(), L"seek end");
  CHECK(RwSeek(layout, mem.ops(), 0, 0) == 0, L"seek set");

  uint8_t buffer[64] = {0};
  CHECK(RwRead(layout, mem.ops(), buffer, 1, 16) == 16, L"read 16 objects");
  CHECK(memcmp(buffer, data.data(), 16) == 0, L"read content");
  CHECK(RwSeek(layout, mem.ops(), 5, 1) == 21, L"seek cur");
  CHECK(RwRead(layout, mem.ops(), buffer, 4, 8) == 8, L"read 8 x 4 bytes");
  CHECK(memcmp(buffer, data.data() + 21, 32) == 0, L"read content 2");
  CHECK(RwSeek(layout, mem.ops(), data.size() - 4, 0) == (int64_t)data.size() - 4, L"seek tail");
  CHECK(RwRead(layout, mem.ops(), buffer, 1, 16) == 4, L"short read at the end");
  CHECK(RwRead(layout, mem.ops(), buffer, 1, 16) == 0, L"read past the end");
  CHECK(RwSeek(layout, mem.ops(), -1, 0) == -1, L"negative seek rejected");
  CHECK(RwSeek(layout, mem.ops(), 1, 2) == -1, L"seek past the end rejected");
}

void TestCache() {
  Cache cache;
  cache.Configure(64 * 1024);

  Pcm one;
  one.bytes.assign(1024, 0x11);
  one.channels = 1;
  one.rate = 44100;
  one.frames = 512;
  CHECK(cache.Put(1, std::move(one)), L"cache put");
  CHECK(cache.Contains(1), L"cache contains");

  Pcm out;
  CHECK(cache.Take(1, &out), L"cache take");
  CHECK(out.bytes.size() == 1024, L"cache take size");
  CHECK(!cache.Contains(1), L"entry is handed over, not shared");
  CHECK(!cache.Take(1, &out), L"second take misses");

  // Budget: 64 kB fits exactly 64 one-kB entries; the 65th evicts the oldest.
  for (int i = 0; i < 80; ++i) {
    Pcm pcm;
    pcm.bytes.assign(1024, (uint8_t)i);
    pcm.channels = 1;
    pcm.rate = 44100;
    pcm.frames = 512;
    cache.Put(0x1000 + i, std::move(pcm));
  }
  CHECK(cache.bytes() <= 64 * 1024, L"cache respects the budget (%zu)", cache.bytes());
  CHECK(cache.stats().evicted > 0, L"evictions happened");
  CHECK(cache.Contains(0x1000 + 79), L"newest entry survives");
  CHECK(!cache.Contains(0x1000), L"oldest entry was evicted");
}

// The prefetcher computes the fingerprint from the file on disk; the hook
// computes it through the game's RWops. They have to agree - this is the logic
// that decides between "already decoded" and "decode now".
void TestStreamMatching(const std::vector<uint8_t>& raw, const Pcm& pcm,
                        const RwopsLayout& layout) {
  const size_t prefix_bytes =
      raw.size() < kFingerprintPrefix ? raw.size() : (size_t)kFingerprintPrefix;
  const uint64_t from_disk = Fingerprint(raw.data(), prefix_bytes, (int64_t)raw.size());

  MemoryRwops mem;
  mem.Init(layout, raw.data(), raw.size());

  uint8_t magic[4] = {0};
  CHECK(PeekMagic(layout, mem.ops(), magic), L"peek magic");
  CHECK(LooksLikeOgg(magic, 4), L"peek sees the OggS magic");
  CHECK(RwRead(layout, mem.ops(), magic, 1, 4) == 4 && memcmp(magic, raw.data(), 4) == 0,
        L"peek rewound the stream");

  uint8_t prefix[kFingerprintPrefix];
  size_t prefix_len = 0;
  int64_t total = 0;
  FingerprintStream(layout, mem.ops(), prefix, &prefix_len, &total);
  CHECK(total == (int64_t)raw.size(), L"stream reports its size (%lld vs %zu)", (long long)total,
        raw.size());
  const uint64_t from_stream = Fingerprint(prefix, prefix_len, total);
  CHECK(from_disk == from_stream, L"disk and stream fingerprints agree");

  std::vector<uint8_t> whole;
  CHECK(ReadStream(layout, mem.ops(), total, &whole), L"read whole stream");
  CHECK(whole.size() == raw.size() && memcmp(whole.data(), raw.data(), raw.size()) == 0,
        L"whole stream matches");
  CHECK(RwRead(layout, mem.ops(), magic, 1, 4) == 4, L"read stream rewound");

  // What the runtime does: the prefetcher stores under the disk fingerprint,
  // the hook looks up with the stream fingerprint.
  Cache cache;
  cache.Configure(64u << 20);
  Pcm copy = pcm;
  CHECK(cache.Put(from_disk, std::move(copy)), L"prefetch insert");
  Pcm hit;
  CHECK(cache.Take(from_stream, &hit), L"hook lookup hits");
  CHECK(hit.bytes.size() == pcm.bytes.size() && hit.rate == pcm.rate, L"hit payload matches");
}

// Parses the shipped sound_ogg_hook.ini: every key in it is documented as
// "equals the built-in default", so loading it must land exactly on the
// defaults - which also catches a typo in a key name in the shipped file.
void TestConfig(const std::wstring& dir) {
  Config config;
  LoadConfig(dir, &config);
  // ASCII only: the console code page cannot spell the module path on all
  // systems, and the assertion below is what actually matters.
  wprintf(L"config          : shipped ini parses to -> %s\n", config.summary().c_str());
  CHECK(config.cache_mb == 256, L"cache_mb parsed as %d", config.cache_mb);
  CHECK(config.threads == 0, L"threads parsed as %d", config.threads);
  CHECK(config.prefetch, L"prefetch parsed as off");
  CHECK(config.prefetch_max_file_mb == 24, L"prefetch_max_file_mb parsed as %d",
        config.prefetch_max_file_mb);
  CHECK(config.scan_roots == 1, L"scan_roots parsed as %d", config.scan_roots);
  CHECK(config.verbose, L"verbose parsed as off");
  CHECK(!config.force_scan, L"force_scan parsed as on");
  CHECK(config.override_stub_rva == 0, L"loadwav_stub_rva parsed as 0x%x",
        config.override_stub_rva);
}

void TestDecode(const std::wstring& path) {
  std::vector<uint8_t> raw;
  if (!ReadFileBytes(path, &raw)) {
    wprintf(L"FAIL: cannot read %ls\n", path.c_str());
    ++g_failures;
    return;
  }
  Pcm pcm;
  std::string error;
  double ms = 0;
  if (!DecodeOgg(raw.data(), raw.size(), &pcm, &error, &ms)) {
    wprintf(L"FAIL: decode of %ls failed: %s\n", path.c_str(), error.c_str());
    ++g_failures;
    return;
  }
  const double seconds = (double)pcm.frames / (double)pcm.rate;
  const double mb = (double)pcm.bytes.size() / 1048576.0;
  wprintf(L"decode          : %ls\n", path.c_str());
  wprintf(L"                  %d Hz, %d ch, %d frames (%.2f s), %.1f MB PCM\n", pcm.rate,
          pcm.channels, pcm.frames, seconds, mb);
  wprintf(L"                  %.1f ms wall, %.1fx realtime, %.1f MB/s of PCM\n", ms,
          ms > 0 ? seconds * 1000.0 / ms : 0.0, ms > 0 ? mb * 1000.0 / ms : 0.0);
  CHECK(pcm.frames > 0, L"decoded frames");
  CHECK(pcm.channels == 1 || pcm.channels == 2, L"channels %d", pcm.channels);
  CHECK(pcm.bytes.size() == (size_t)pcm.frames * pcm.channels * 2, L"buffer size matches");

  // The prefetcher's budget check asks the headers how big the decode will be;
  // on this file that estimate must be exactly right.
  const uint64_t estimated = EstimateDecodedBytes(raw.data(), raw.size());
  CHECK(estimated == pcm.bytes.size(),
        L"header-only estimate matches the decode (%llu vs %zu)", (unsigned long long)estimated,
        pcm.bytes.size());

  // The buffer we would hand to SDL must survive the round trip through a
  // memory RWops, which is exactly what the hook does at runtime.
  std::vector<uint8_t> wav;
  BuildWav(pcm, &wav);
  const RwopsLayout layout = ModernLayout();
  MemoryRwops mem;
  mem.Init(layout, wav.data(), wav.size());
  uint8_t header[44] = {0};
  CHECK(RwRead(layout, mem.ops(), header, 1, 44) == 44, L"wav header read back");
  CHECK(memcmp(header, "RIFF", 4) == 0 && memcmp(header + 8, "WAVE", 4) == 0,
        L"wav round trip tags");
  CHECK(*(const uint32_t*)(header + 40) == pcm.bytes.size(), L"wav round trip data size");

  TestStreamMatching(raw, pcm, layout);
}

// The prefetcher picks its work from the `file = "..."` entries of the sound
// assets. Two things have to hold: a `music = { ... }` block must never be
// queued (the music player does not go through the hooked loader) and the entries
// that are kept must resolve to the paths the game itself would use.
void TestAssetParsing() {
  const std::string asset =
      "# a comment that mentions file = \"comment.ogg\"\n"
      "sound = {\n"
      "    name = \"one\"\n"
      "    file = \"vo/one.ogg\"\n"
      "    falloff = {\n"
      "        file = \"vo/nested.ogg\"\n"
      "    }\n"
      "    file = \"two.ogg\"   # trailing comment\n"
      "}\n"
      "music = {\n"
      "    name = \"track\"\n"
      "    file = \"track.ogg\"\n"
      "}\n"
      "sound = { file = \"sound/three.ogg\" }\n";
  const std::vector<std::string> entries = AssetFileEntries(asset);
  CHECK(entries.size() == 4, L"asset entries %zu (music and comments skipped)", entries.size());
  if (entries.size() == 4) {
    CHECK(entries[0] == "vo/one.ogg", L"first entry is %s", entries[0].c_str());
    CHECK(entries[1] == "vo/nested.ogg", L"nested entry is %s", entries[1].c_str());
    CHECK(entries[2] == "two.ogg", L"entry behind a nested block is %s", entries[2].c_str());
    CHECK(entries[3] == "sound/three.ogg", L"second block entry is %s", entries[3].c_str());
  }

  // "sound/vo/x.ogg" is rooted at the game folder, "a.ogg" at sound\.
  const std::vector<std::wstring> rooted =
      AssetEntryCandidates(L"D:\\mod", L"D:\\mod\\sound\\sub", L"/sound/vo/x.ogg");
  CHECK(rooted.size() == 3, L"rooted candidates %zu", rooted.size());
  if (rooted.size() == 3) {
    CHECK(rooted[0] == L"D:\\mod\\sound\\vo\\x.ogg", L"rooted candidate is %ls",
          rooted[0].c_str());
  }
  const std::vector<std::wstring> plain =
      AssetEntryCandidates(L"D:\\mod", L"D:\\mod\\sound", L"a.ogg");
  CHECK(plain.size() == 2, L"plain candidates %zu (the duplicate is dropped)", plain.size());
  if (plain.size() == 2) {
    CHECK(plain[0] == L"D:\\mod\\a.ogg", L"plain root candidate is %ls", plain[0].c_str());
    CHECK(plain[1] == L"D:\\mod\\sound\\a.ogg", L"plain sound candidate is %ls",
          plain[1].c_str());
  }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  LogInit(nullptr);
  std::wstring ogg = (argc > 1 && argv[1][0] != 0) ? argv[1] : FindGameOgg();
  wprintf(L"sound_ogg_hook selftest\n");

  TestWav();
  TestFingerprint();
  TestMemoryRwops();
  TestCache();
  TestAssetParsing();
  if (argc > 2 && argv[2][0] != 0) {
    TestConfig(argv[2]);
  }
  if (ogg.empty()) {
    wprintf(L"FAIL: no .ogg given and none found in the installed game's music folder\n");
    ++g_failures;
  } else {
    TestDecode(ogg);
  }

  wprintf(g_failures == 0 ? L"all checks passed\n" : L"%d check(s) failed\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
