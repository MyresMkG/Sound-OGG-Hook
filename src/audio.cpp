#include "audio.h"

#include <windows.h>

#include <cstring>

#include "log.h"

extern "C" {
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
}

namespace oggsound {
namespace {

constexpr size_t kMaxDecodedBytes = 512u * 1024u * 1024u;  // sanity cap

void AppendLe32(std::vector<uint8_t>* out, uint32_t v) {
  out->push_back((uint8_t)(v & 0xFF));
  out->push_back((uint8_t)((v >> 8) & 0xFF));
  out->push_back((uint8_t)((v >> 16) & 0xFF));
  out->push_back((uint8_t)((v >> 24) & 0xFF));
}

void AppendLe16(std::vector<uint8_t>* out, uint16_t v) {
  out->push_back((uint8_t)(v & 0xFF));
  out->push_back((uint8_t)((v >> 8) & 0xFF));
}

}  // namespace

double NowMs() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f{};
    ::QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER now{};
  ::QueryPerformanceCounter(&now);
  return (double)now.QuadPart * 1000.0 / (double)freq.QuadPart;
}

bool DecodeOgg(const uint8_t* data, size_t size, Pcm* out, std::string* error, double* ms) {
  const double start = NowMs();
  if (!LooksLikeOgg(data, size)) {
    *error = "no OggS magic";
    return false;
  }
  int err = 0;
  stb_vorbis* v = stb_vorbis_open_memory(data, (int)size, &err, nullptr);
  if (v == nullptr) {
    *error = "stb_vorbis_open_memory failed (error " + std::to_string(err) + ")";
    return false;
  }

  const stb_vorbis_info info = stb_vorbis_get_info(v);
  const int file_channels = info.channels;
  const int rate = (int)info.sample_rate;
  if (file_channels < 1 || rate < 1) {
    stb_vorbis_close(v);
    *error = "implausible stream header";
    return false;
  }
  // stb_vorbis downmixes when asked for fewer channels than the file has; the
  // engine mixes in stereo, so anything wider is folded down here instead of
  // making SDL's converter do it.
  const int channels = file_channels > 2 ? 2 : file_channels;
  const unsigned int total_frames = stb_vorbis_stream_length_in_samples(v);

  std::vector<uint8_t> pcm;
  if (total_frames != 0) {
    const uint64_t bytes = (uint64_t)total_frames * channels * 2;
    if (bytes > kMaxDecodedBytes) {
      stb_vorbis_close(v);
      *error = "decoded stream too large";
      return false;
    }
    pcm.resize((size_t)bytes);
    size_t frames_done = 0;
    while (frames_done < total_frames) {
      const int want = (int)(total_frames - frames_done);
      const int got = stb_vorbis_get_samples_short_interleaved(
          v, channels, (short*)(pcm.data() + frames_done * channels * 2), want * channels);
      if (got <= 0) break;
      frames_done += (size_t)got;
    }
    pcm.resize(frames_done * channels * 2);
    out->frames = (int)frames_done;
  } else {
    // Unknown length (rare): grow in place, one copy per chunk.
    const int chunk_frames = 8192;
    std::vector<short> scratch((size_t)chunk_frames * channels);
    for (;;) {
      const int got = stb_vorbis_get_samples_short_interleaved(
          v, channels, scratch.data(), chunk_frames * channels);
      if (got <= 0) break;
      const uint8_t* src = (const uint8_t*)scratch.data();
      pcm.insert(pcm.end(), src, src + (size_t)got * channels * 2);
      out->frames += got;
      if (pcm.size() > kMaxDecodedBytes) break;
    }
  }
  stb_vorbis_close(v);

  if (pcm.empty()) {
    *error = "decoder produced no samples";
    return false;
  }
  out->bytes = std::move(pcm);
  out->channels = channels;
  out->rate = rate;
  *ms = NowMs() - start;
  return true;
}

uint64_t EstimateDecodedBytes(const uint8_t* data, size_t size) {
  if (!LooksLikeOgg(data, size)) return 0;
  int err = 0;
  stb_vorbis* v = stb_vorbis_open_memory(data, (int)size, &err, nullptr);
  if (v == nullptr) return 0;
  const stb_vorbis_info info = stb_vorbis_get_info(v);
  const unsigned int frames = stb_vorbis_stream_length_in_samples(v);
  stb_vorbis_close(v);
  if (frames == 0 || info.channels < 1) return 0;
  const uint32_t channels = info.channels > 2 ? 2 : (uint32_t)info.channels;
  const uint64_t bytes = (uint64_t)frames * channels * 2;
  return bytes > kMaxDecodedBytes ? 0 : bytes;
}

void BuildWav(const Pcm& pcm, std::vector<uint8_t>* out) {  const uint32_t data_bytes = (uint32_t)pcm.bytes.size();
  const uint16_t block_align = (uint16_t)(pcm.channels * 2);
  const uint32_t byte_rate = (uint32_t)pcm.rate * block_align;

  out->clear();
  out->reserve(44 + data_bytes);
  out->insert(out->end(), {'R', 'I', 'F', 'F'});
  AppendLe32(out, 36 + data_bytes);
  out->insert(out->end(), {'W', 'A', 'V', 'E'});
  out->insert(out->end(), {'f', 'm', 't', ' '});
  AppendLe32(out, 16);
  AppendLe16(out, 1);  // PCM
  AppendLe16(out, (uint16_t)pcm.channels);
  AppendLe32(out, (uint32_t)pcm.rate);
  AppendLe32(out, byte_rate);
  AppendLe16(out, block_align);
  AppendLe16(out, 16);  // bits per sample
  out->insert(out->end(), {'d', 'a', 't', 'a'});
  AppendLe32(out, data_bytes);
  out->insert(out->end(), pcm.bytes.begin(), pcm.bytes.end());
}

uint64_t Fingerprint(const uint8_t* prefix, size_t prefix_len, int64_t total_size) {
  uint64_t hash = 1469598103934665603ull;  // FNV-1a 64
  const auto mix = [&hash](const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      hash ^= p[i];
      hash *= 1099511628211ull;
    }
  };
  const uint8_t size_bytes[8] = {
      (uint8_t)((uint64_t)total_size & 0xFF),      (uint8_t)(((uint64_t)total_size >> 8) & 0xFF),
      (uint8_t)(((uint64_t)total_size >> 16) & 0xFF),
      (uint8_t)(((uint64_t)total_size >> 24) & 0xFF),
      (uint8_t)(((uint64_t)total_size >> 32) & 0xFF),
      (uint8_t)(((uint64_t)total_size >> 40) & 0xFF),
      (uint8_t)(((uint64_t)total_size >> 48) & 0xFF),
      (uint8_t)(((uint64_t)total_size >> 56) & 0xFF)};
  mix(size_bytes, sizeof(size_bytes));
  mix(prefix, prefix_len);
  return hash;
}

}  // namespace oggsound
