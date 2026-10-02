// Ogg decoding, WAV synthesis and content fingerprints.
//
// Decoding uses a vendored stb_vorbis (public domain, one file, no ABI
// dependency on anything inside the game). It decodes straight into the
// interleaved 16-bit buffer we hand to SDL later, so there is at most one copy
// between the Vorbis decoder and the game's own audio buffer.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oggsound {

struct Pcm {
  std::vector<uint8_t> bytes;  // interleaved signed 16-bit little-endian
  int channels = 0;
  int rate = 0;
  int frames = 0;

  bool empty() const { return bytes.empty() || channels == 0 || rate == 0; }
};

inline bool LooksLikeOgg(const uint8_t* data, size_t size) {
  return size >= 4 && data[0] == 'O' && data[1] == 'g' && data[2] == 'g' && data[3] == 'S';
}

// Whole-file decode. |error| receives a human readable reason on failure and
// |ms| the wall time spent, which is what the log reports.
bool DecodeOgg(const uint8_t* data, size_t size, Pcm* out, std::string* error, double* ms);

// Exact size the decode would produce (bytes of interleaved S16), read from the
// Vorbis headers alone - no decoding. The prefetcher uses it to decide whether a
// file still fits the cache budget *before* spending time and a temporary buffer
// on it. Returns 0 when the size cannot be determined.
uint64_t EstimateDecodedBytes(const uint8_t* data, size_t size);

// RIFF/WAVE (uncompressed PCM) wrapper around |pcm|. What SDL_LoadWAV_RW would
// have produced for an equivalent .wav, byte for byte.
void BuildWav(const Pcm& pcm, std::vector<uint8_t>* out);

// Identity of a file's content: its size plus a hash of the leading bytes.
// Prefetch computes it from disk, the hook computes it from the stream the game
// opened, and a match means "this is the same audio".
constexpr size_t kFingerprintPrefix = 16 * 1024;
uint64_t Fingerprint(const uint8_t* prefix, size_t prefix_len, int64_t total_size);

double NowMs();

}  // namespace oggsound
