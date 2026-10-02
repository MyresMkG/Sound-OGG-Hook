#include "cache.h"

namespace oggsound {

void Cache::Configure(size_t budget_bytes) {
  std::lock_guard<std::mutex> guard(mu_);
  budget_ = budget_bytes;
}

bool Cache::Take(uint64_t fingerprint, Pcm* out) {
  std::lock_guard<std::mutex> guard(mu_);
  const auto it = index_.find(fingerprint);
  if (it == index_.end()) {
    ++stats_.misses;
    return false;
  }
  List::iterator entry = it->second;
  const size_t bytes = entry->pcm.bytes.size();
  *out = std::move(entry->pcm);
  lru_.erase(entry);
  index_.erase(it);
  used_ -= bytes;
  ++stats_.hits;
  return true;
}

bool Cache::Put(uint64_t fingerprint, Pcm&& pcm) {
  const size_t bytes = pcm.bytes.size();
  std::lock_guard<std::mutex> guard(mu_);
  if (bytes == 0 || bytes > budget_) {
    ++stats_.too_big;
    return false;
  }
  while (used_ + bytes > budget_ && !lru_.empty()) {
    const Entry& oldest = lru_.back();
    used_ -= oldest.pcm.bytes.size();
    index_.erase(oldest.fingerprint);
    lru_.pop_back();
    ++stats_.evicted;
  }
  if (used_ + bytes > budget_) {
    ++stats_.too_big;
    return false;
  }
  lru_.push_front(Entry{fingerprint, std::move(pcm)});
  index_[fingerprint] = lru_.begin();
  used_ += bytes;
  ++stats_.prefetched;
  stats_.prefetched_bytes += bytes;
  return true;
}

bool Cache::Contains(uint64_t fingerprint) const {
  std::lock_guard<std::mutex> guard(mu_);
  return index_.find(fingerprint) != index_.end();
}

bool Cache::HasRoom(size_t bytes) const {
  std::lock_guard<std::mutex> guard(mu_);
  return bytes <= budget_ - used_;
}

size_t Cache::bytes() const {
  std::lock_guard<std::mutex> guard(mu_);
  return used_;
}

size_t Cache::budget() const {
  std::lock_guard<std::mutex> guard(mu_);
  return budget_;
}

CacheStats Cache::stats() const {
  std::lock_guard<std::mutex> guard(mu_);
  return stats_;
}

}  // namespace oggsound
