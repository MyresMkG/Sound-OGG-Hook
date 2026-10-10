#include "hook.h"

#include <atomic>
#include <cstring>
#include <exception>
#include <mutex>
#include <vector>

#include "audio.h"
#include "cache.h"
#include "config.h"
#include "image.h"
#include "log.h"
#include "patch.h"
#include "prefetch.h"
#include "resolver.h"
#include "rwops.h"
#include "stream.h"

namespace oggsound {
namespace {

// Only the layout matters; the callback types are ours to define because we
// never call SDL functions by name, we call through the slot the trampoline
// used.
struct SDL_AudioSpecStub {
  int freq;
  uint16_t format;
  uint8_t channels;
  uint8_t silence;
  uint16_t samples;
  uint16_t padding;
  uint32_t size;
  void* callback;
  void* userdata;
};
static_assert(sizeof(SDL_AudioSpecStub) == 32, "SDL_AudioSpec layout changed");

Image g_image;
Patch g_patch;
Config g_config;
Cache g_cache;

std::atomic<bool> g_ready{false};
// RVA of the slot the hooked trampoline jumps through. It is published *before*
// the patch is written: from that moment the detour can run, and it reaches the
// real SDL implementation through this slot. 0 means "not armed yet".
std::atomic<uint32_t> g_slot_rva{0};
std::atomic<int> g_layout_state{0};  // 0 unknown, 1 known-good
RwopsLayout g_layout;
std::mutex g_layout_mutex;
std::atomic<int> g_layout_failures{0};

std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_ogg_calls{0};
std::atomic<uint64_t> g_passthrough{0};
std::atomic<uint64_t> g_cache_hits{0};
std::atomic<uint64_t> g_sync_decodes{0};
std::atomic<uint64_t> g_sync_ms{0};
std::atomic<uint64_t> g_failures{0};

using LoadWavFn = SDL_AudioSpecStub* (*)(void* src, int freesrc, SDL_AudioSpecStub* spec,
                                         uint8_t** audio_buf, uint32_t* audio_len);

uint64_t SlotTarget() {
  const uint32_t slot_rva = g_slot_rva.load(std::memory_order_acquire);
  if (slot_rva == 0) return 0;
  const volatile uint64_t* slot =
      (const volatile uint64_t*)(uintptr_t)(g_image.image_base() + slot_rva);
  return *slot;
}

SDL_AudioSpecStub* CallOriginal(void* src, int freesrc, SDL_AudioSpecStub* spec, uint8_t** buf,
                                uint32_t* len) {
  const uint64_t target = SlotTarget();
  if (target == 0) return nullptr;
  return ((LoadWavFn)(uintptr_t)target)(src, freesrc, spec, buf, len);
}

bool EnsureLayout(void* src) {
  if (g_layout_state.load(std::memory_order_acquire) == 1) return true;
  RwopsLayout layout;
  if (!DetectLayout(g_image, src, &layout)) {
    if (g_layout_failures.fetch_add(1) == 0) {
      Log("hook: the RWops layout is not one we know; this call is passed through untouched");
    }
    return false;
  }
  // The layout is written before the state that advertises it: a second thread
  // can arrive here while this one is still filling the struct in.
  std::lock_guard<std::mutex> guard(g_layout_mutex);
  if (g_layout_state.load(std::memory_order_relaxed) != 1) {
    g_layout = layout;
    g_layout_state.store(1, std::memory_order_release);
    Log("hook: SDL_RWops layout: size@0x%x seek@0x%x read@0x%x close@0x%x hidden@0x%x%s",
        layout.size_off, layout.seek_off, layout.read_off, layout.close_off, layout.hidden_off,
        layout.legacy_seek ? " (legacy, int seek)" : "");
  }
  return true;
}

void LogSummaryIfDue() {
  const uint64_t ogg = g_ogg_calls.load();
  if (ogg == 0 || (ogg % 64) != 0) return;
  const CacheStats stats = g_cache.stats();
  Log("summary: %llu load calls, %llu ogg, %llu prefetched hits (%.0f%%), %llu synchronous "
      "decodes (%.0f ms total)",
      (unsigned long long)g_calls.load(), (unsigned long long)ogg,
      (unsigned long long)g_cache_hits.load(),
      ogg != 0 ? 100.0 * (double)g_cache_hits.load() / (double)ogg : 0.0,
      (unsigned long long)g_sync_decodes.load(), (double)g_sync_ms.load());
  Log("summary: cache %.1f/%.1f MB, %llu prefetched files, %llu evictions, %llu failures",
      (double)g_cache.bytes() / 1048576.0, (double)g_cache.budget() / 1048576.0,
      (unsigned long long)stats.prefetched, (unsigned long long)stats.evicted,
      (unsigned long long)g_failures.load());
}

// The whole ogg path. It is kept apart from the detour so that the detour can
// turn anything that escapes (only allocations can throw) into a clean
// hand-over to SDL.
SDL_AudioSpecStub* HandleLoad(void* src, int freesrc, SDL_AudioSpecStub* spec, uint8_t** audio_buf,
                              uint32_t* audio_len, bool* stream_closed) {
  if (!EnsureLayout(src)) {
    return CallOriginal(src, freesrc, spec, audio_buf, audio_len);
  }

  uint8_t magic[4] = {0};
  if (!PeekMagic(g_layout, src, magic) || !LooksLikeOgg(magic, 4)) {
    // A handful of pass-through notes make it obvious from the log that the hook
    // is live even on a vanilla install, which has no Ogg sounds at all.
    const uint64_t seen = g_passthrough.fetch_add(1, std::memory_order_relaxed);
    if (seen < 3) {
      Log("load: pass-through for a non-Ogg stream (first bytes %02x %02x %02x %02x), SDL handles "
          "it as before",
          magic[0], magic[1], magic[2], magic[3]);
    }
    return CallOriginal(src, freesrc, spec, audio_buf, audio_len);
  }

  g_ogg_calls.fetch_add(1, std::memory_order_relaxed);

  uint8_t prefix[kFingerprintPrefix];
  size_t prefix_len = 0;
  int64_t total_size = -1;
  FingerprintStream(g_layout, src, prefix, &prefix_len, &total_size);
  const uint64_t fingerprint = Fingerprint(prefix, prefix_len, total_size);

  Pcm pcm;
  const bool hit = g_cache.Take(fingerprint, &pcm);
  if (hit) {
    g_cache_hits.fetch_add(1, std::memory_order_relaxed);
    if (g_config.verbose) {
      Log("load: cache hit, %d Hz %d ch %.2f s (%zu kB PCM)", pcm.rate, pcm.channels,
          (double)pcm.frames / (double)(pcm.rate != 0 ? pcm.rate : 1), pcm.bytes.size() / 1024);
    }
  } else {
    std::vector<uint8_t> raw;
    if (!ReadStream(g_layout, src, total_size, &raw)) {
      g_failures.fetch_add(1, std::memory_order_relaxed);
      Log("load: could not read the ogg stream (%zu bytes seen)", (size_t)raw.size());
      return CallOriginal(src, freesrc, spec, audio_buf, audio_len);
    }
    std::string error;
    double ms = 0;
    if (!DecodeOgg(raw.data(), raw.size(), &pcm, &error, &ms)) {
      g_failures.fetch_add(1, std::memory_order_relaxed);
      Log("load: ogg decode failed (%s); letting SDL report the failure", error.c_str());
      RwSeek(g_layout, src, 0, 0);
      return CallOriginal(src, freesrc, spec, audio_buf, audio_len);
    }
    g_sync_decodes.fetch_add(1, std::memory_order_relaxed);
    g_sync_ms.fetch_add((uint64_t)ms, std::memory_order_relaxed);
    if (g_config.verbose) {
      Log("load: decoded on demand, %d Hz %d ch %.2f s in %.1f ms (%s)", pcm.rate, pcm.channels,
          (double)pcm.frames / (double)(pcm.rate != 0 ? pcm.rate : 1), ms,
          total_size >= 0 ? "prefetcher missed" : "unknown size");
    }
  }

  // Synthesising the WAV is the last step that can fail, and it happens while
  // the game's stream is still untouched and rewound to 0, so the caller can
  // still fall back to SDL with it.
  std::vector<uint8_t> wav;
  BuildWav(pcm, &wav);

  // From here on the original stream is ours to consume, exactly like SDL would
  // when freesrc is set.
  if (freesrc != 0) {
    RwClose(g_layout, src);
    *stream_closed = true;
  }

  MemoryRwops memory;
  memory.Init(g_layout, wav.data(), wav.size());
  SDL_AudioSpecStub* result = CallOriginal(memory.ops(), 0, spec, audio_buf, audio_len);
  if (result == nullptr) {
    g_failures.fetch_add(1, std::memory_order_relaxed);
    Log("load: SDL_LoadWAV_RW rejected the synthesised WAV (%zu bytes)", wav.size());
  }
  LogSummaryIfDue();
  return result;
}

}  // namespace

// SDL_LoadWAV_RW(SDL_RWops*, int, SDL_AudioSpec*, Uint8**, Uint32*)
extern "C" SDL_AudioSpecStub* DetourLoadWav(void* src, int freesrc, SDL_AudioSpecStub* spec,
                                            uint8_t** audio_buf, uint32_t* audio_len) {
  g_calls.fetch_add(1, std::memory_order_relaxed);
  if (!g_ready.load(std::memory_order_acquire) || src == nullptr || spec == nullptr ||
      audio_buf == nullptr || audio_len == nullptr) {
    return CallOriginal(src, freesrc, spec, audio_buf, audio_len);
  }

  bool stream_closed = false;
  try {
    return HandleLoad(src, freesrc, spec, audio_buf, audio_len, &stream_closed);
  } catch (const std::exception& error) {
    Log("load: %s while handling an Ogg stream; handing the call to SDL untouched", error.what());
  } catch (...) {
    Log("load: unknown exception while handling an Ogg stream; handing the call to SDL untouched");
  }

  g_failures.fetch_add(1, std::memory_order_relaxed);
  // Once freesrc consumed the source, it must never be handed to SDL again.
  if (stream_closed) return nullptr;
  if (!stream_closed) RwSeek(g_layout, src, 0, 0);
  return CallOriginal(src, freesrc, spec, audio_buf, audio_len);
}

bool InitSoundHook(HMODULE self) {
  (void)self;
  LoadConfig(ModuleDir(), &g_config);
  g_cache.Configure((size_t)(g_config.cache_mb > 0 ? g_config.cache_mb : 0) * 1024u * 1024u);

  if (!g_image.InitFromModule(::GetModuleHandleW(nullptr))) {
    Log("init: could not read the game's own PE headers");
    return false;
  }

  Resolution resolution;
  bool resolved = false;
  if (g_config.override_stub_rva != 0) {
    resolution.stub_rva = g_config.override_stub_rva;
    if (StubSlot(g_image, resolution.stub_rva, &resolution.slot_rva, &resolution.slot_value)) {
      Log("init: using the configured override stub rva 0x%x", resolution.stub_rva);
      if (resolution.slot_value >= g_image.image_base() &&
          resolution.slot_value < g_image.image_base() + g_image.image_size()) {
        resolution.slot_target_rva = resolution.slot_value - g_image.image_base();
      }
      resolved = true;
    } else {
      Log("init: override stub rva 0x%x is not a dynapi trampoline", resolution.stub_rva);
    }
  }
  if (!resolved) {
    if (g_config.force_scan) {
      Log("init: force_scan is set, using the byte-pattern scan directly");
    } else {
      resolved = ResolveLoadWav(g_image, &resolution);
      if (!resolved) Log("init: anchor resolution failed, falling back to a byte-pattern scan");
    }
  }
  if (!resolved) resolved = ResolveByScan(g_image, &resolution);
  if (!resolved) {
    Log("init: SDL_LoadWAV_RW was not found in this build (neither by anchors nor by scan); the "
          "game stays unmodified");
    return false;
  }

  // Independent confirmation, useful before trusting a game build we have never
  // seen: the slot either holds SDL's bootstrap trampoline (before the first
  // audio call) or the real WAV parser, which this checks for RIFF/WAVE ids.
  uint32_t implementation = 0;
  if (resolution.slot_target_rva != 0) {
    const uint32_t target = (uint32_t)resolution.slot_target_rva;
    if (FunctionJumpsThroughSlot(g_image, target, resolution.slot_rva)) {
      implementation = FindInstalledImplementation(g_image, resolution.slot_rva);
      Log("init: slot holds SDL's bootstrap trampoline 0x%x; the dynapi installer will write "
          "0x%x into it",
          target, implementation);
    } else {
      implementation = target;
    }
  } else {
    Log("init: slot holds a pointer outside the image; refusing an unverified target");
    return false;
  }
  if (implementation != 0) {
    const bool wave_like = LooksLikeWaveLoader(g_image, implementation, 2);
    Log("init: implementation 0x%x %s SDL's WAV parser (RIFF/WAVE ids)", implementation,
        wave_like ? "is" : "does not look like");
    if (!wave_like) {
      Log("init: refusing to hook a target that is not a verified WAV parser");
      return false;
    }
  }

  if (implementation == 0) {
    Log("init: could not confirm the real WAV implementation; nothing is hooked");
    return false;
  }

  if (ProbeOnlyRequested(ModuleDir())) {
    Log("probe-only: resolution finished, nothing is hooked, prefetch not started");
    Log("probe-only: delete the flag file to enable the hook");
    return true;
  }

  // The slot is published first: the moment the jump below is in place another
  // thread can be inside DetourLoadWav and call through it.
  HMODULE pinned = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&DetourLoadWav), &pinned)) {
    Log("init: could not pin the hook DLL (error %lu)", ::GetLastError());
    return false;
  }
  g_slot_rva.store(resolution.slot_rva, std::memory_order_release);
  if (!InstallAbsoluteJump(g_image, resolution.stub_rva, (const void*)&DetourLoadWav, &g_patch)) {
    Log("init: hook installation failed; the game stays unmodified");
    g_slot_rva.store(0, std::memory_order_release);
    return false;
  }
  g_ready.store(true, std::memory_order_release);
  Log("init: hook live on rva 0x%x (call site 0x%x, slot 0x%x)", resolution.stub_rva,
      resolution.call_rva, resolution.slot_rva);

  try {
    StartPrefetch(g_config, &g_cache);
  } catch (const std::exception& error) {
    Log("prefetch: could not start (%s); hook remains live for on-demand decoding", error.what());
  } catch (...) {
    Log("prefetch: could not start; hook remains live for on-demand decoding");
  }
  return true;
}

}  // namespace oggsound
