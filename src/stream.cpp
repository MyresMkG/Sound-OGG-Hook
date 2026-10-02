#include "stream.h"

#include <algorithm>
#include <cstring>

namespace oggsound {
namespace {

constexpr int kSeekSet = 0;
constexpr int kSeekEnd = 2;
constexpr size_t kMaxStreamBytes = 512u << 20;

}  // namespace

bool PeekMagic(const RwopsLayout& layout, void* src, uint8_t magic[4]) {
  RwSeek(layout, src, 0, kSeekSet);
  const size_t got = RwRead(layout, src, magic, 1, 4);
  RwSeek(layout, src, 0, kSeekSet);
  return got == 4;
}

void FingerprintStream(const RwopsLayout& layout, void* src, uint8_t* prefix, size_t* prefix_len,
                       int64_t* total_size) {
  *prefix_len = 0;
  *total_size = RwSeek(layout, src, 0, kSeekEnd);
  RwSeek(layout, src, 0, kSeekSet);

  const size_t want = kFingerprintPrefix;
  size_t done = 0;
  while (done < want) {
    const size_t got = RwRead(layout, src, prefix + done, 1, want - done);
    if (got == 0) break;
    done += got;
  }
  *prefix_len = done;
  RwSeek(layout, src, 0, kSeekSet);
}

bool ReadStream(const RwopsLayout& layout, void* src, int64_t known_size,
                std::vector<uint8_t>* out) {
  RwSeek(layout, src, 0, kSeekSet);
  if (known_size > 0 && (uint64_t)known_size < kMaxStreamBytes) {
    out->resize((size_t)known_size);
    size_t done = 0;
    while (done < out->size()) {
      const size_t want = (std::min<size_t>)(out->size() - done, 1u << 20);
      const size_t got = RwRead(layout, src, out->data() + done, 1, want);
      if (got == 0) break;
      done += got;
    }
    out->resize(done);
  } else {
    const size_t chunk = 1u << 20;
    std::vector<uint8_t> buffer(chunk);
    for (;;) {
      const size_t got = RwRead(layout, src, buffer.data(), 1, chunk);
      if (got == 0) break;
      out->insert(out->end(), buffer.begin(), buffer.begin() + got);
      if (out->size() > kMaxStreamBytes) break;
    }
  }
  RwSeek(layout, src, 0, kSeekSet);
  return !out->empty();
}

}  // namespace oggsound
