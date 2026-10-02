// Decoded-PCM store shared by the prefetch workers and the hook.
//
// Entries are handed over, not shared: a load that hits removes the entry and
// the game owns the samples afterwards, so a sound is never held twice (the
// game keeps its decoded copy for as long as it keeps the sound loaded).
#pragma once

#include <cstdint>
#include <list>
#include <mutex>
#include <unordered_map>

#include "audio.h"

namespace oggsound {

struct CacheStats {
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t prefetched = 0;
  uint64_t evicted = 0;
  uint64_t too_big = 0;
  uint64_t prefetched_bytes = 0;
  double prefetch_ms = 0;
};

class Cache {
 public:
  void Configure(size_t budget_bytes);

  bool Take(uint64_t fingerprint, Pcm* out);
  bool Put(uint64_t fingerprint, Pcm&& pcm);
  bool Contains(uint64_t fingerprint) const;
  bool HasRoom(size_t bytes) const;

  size_t bytes() const;
  size_t budget() const;
  CacheStats stats() const;

 private:
  struct Entry {
    uint64_t fingerprint = 0;
    Pcm pcm;
  };
  using List = std::list<Entry>;

  mutable std::mutex mu_;
  List lru_;  // front = newest
  std::unordered_map<uint64_t, List::iterator> index_;
  size_t budget_ = 0;
  size_t used_ = 0;
  CacheStats stats_;
};

}  // namespace oggsound
