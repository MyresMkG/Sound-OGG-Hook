// Simulated audio host, named hoi4.exe, loaded by the released injector proxy.
// It verifies the real detour and bootstrap, but is not a real game/audio device.
#include <windows.h>
#include <dxgi.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <vector>
#include "audio.h"
#include "image.h"
#include "rwops.h"

struct Spec {
  int rate; uint16_t format; uint8_t channels, silence;
  uint16_t samples, padding; uint32_t size; void* callback; void* userdata;
};
struct Ops {
  int64_t (*size)(void*);
  int64_t (*seek)(void*, int64_t, int);
  size_t (*read)(void*, void*, size_t, size_t);
  void* write;
  int (*close)(void*);
  uint32_t type = 0, pad = 0;
  const std::vector<uint8_t>* bytes = nullptr;
  size_t pos = 0;
  int closed = 0;
};
int64_t Size(void* p) { return static_cast<Ops*>(p)->bytes->size(); }
int64_t Seek(void* p, int64_t off, int whence) {
  auto& o = *static_cast<Ops*>(p);
  int64_t n = off + (whence == 1 ? o.pos : whence == 2 ? o.bytes->size() : 0);
  if (n < 0 || static_cast<size_t>(n) > o.bytes->size()) return -1;
  o.pos = static_cast<size_t>(n); return n;
}
size_t Read(void* p, void* dst, size_t size, size_t count) {
  auto& o = *static_cast<Ops*>(p);
  if (size == 0 || o.closed) return 0;
  size_t n = (std::min)(count, (o.bytes->size() - o.pos) / size);
  std::memcpy(dst, o.bytes->data() + o.pos, n * size); o.pos += n * size; return n;
}
int Close(void* p) { ++static_cast<Ops*>(p)->closed; return 0; }
Ops MakeOps(const std::vector<uint8_t>& b) {
  Ops o{}; o.size = Size; o.seek = Seek; o.read = Read; o.close = Close; o.bytes = &b;
  return o;
}

extern "C" Spec* FixtureLoader(void*, int, Spec*, uint8_t**, uint32_t*);
extern "C" unsigned char FixtureStub[];
extern "C" uint64_t FixtureSlot;

extern "C" __attribute__((noinline)) Spec* FixtureWave(
    void* source, int freesrc, Spec* spec, uint8_t** out, uint32_t* length) {
  auto* o = static_cast<Ops*>(source);
  uint8_t h[44]{};
  bool ok = o->read(source, h, 1, 44) == 44;
  // These immediate constants are deliberately visible to the resolver.
  ok = ok && *reinterpret_cast<uint32_t*>(h) == 0x46464952 &&
       *reinterpret_cast<uint32_t*>(h + 8) == 0x45564157;
  if (ok) {
    std::memcpy(length, h + 40, 4);
    *out = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), 0, *length));
    ok = *out && o->read(source, *out, 1, *length) == *length;
    std::memcpy(&spec->rate, h + 24, 4);
    spec->channels = h[22]; spec->format = 0x8010;
  }
  if (freesrc) o->close(source);
  return ok ? spec : nullptr;
}

DWORD WINAPI Idle(void*) { return 0; }
int wmain(int argc, wchar_t** argv) {
  if (argc < 4) return 2;
  const uint64_t preferred = std::wcstoull(argv[3], nullptr, 0);
  // MinGW may update its live ImageBase field. Restore the on-disk preferred
  // value before the injector wakes, so this test also catches trusting it.
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(dos) + dos->e_lfanew);
  DWORD old = 0, ignored = 0;
  if (!VirtualProtect(&nt->OptionalHeader.ImageBase, sizeof(uint64_t), PAGE_READWRITE, &old)) return 9;
  nt->OptionalHeader.ImageBase = preferred;
  VirtualProtect(&nt->OptionalHeader.ImageBase, sizeof(uint64_t), old, &ignored);
  std::ifstream file(argv[1], std::ios::binary);
  std::vector<uint8_t> ogg{std::istreambuf_iterator<char>(file), {}};
  if (ogg.empty()) return 3;
  const bool probe = wcscmp(argv[2], L"probe") == 0;
  const bool foreign = wcscmp(argv[2], L"foreign") == 0;
  const bool reject = wcscmp(argv[2], L"reject") == 0;
  const bool outside = wcscmp(argv[2], L"outside") == 0;
  if (reject) FixtureSlot = reinterpret_cast<uint64_t>(&Size);
  if (outside) FixtureSlot = reinterpret_cast<uint64_t>(&GetTickCount);
  GUID iid{}; void* factory = nullptr;
  (void)CreateDXGIFactory1(iid, &factory); // import the actual injector proxy
  HANDLE t = CreateThread(nullptr, 0, Idle, nullptr, 0, nullptr);
  if (t) CloseHandle(t);
  // Run a message pump so the injector's timer wakes after its 700 ms fallback.
  const ULONGLONG start = GetTickCount64();
  while (GetTickCount64() - start < 4000) {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    Sleep(10);
  }
  oggsound::Image image;
  if (!image.InitFromModule(GetModuleHandleW(nullptr)) ||
      image.image_base() != reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr))) return 4;
  std::printf("ASLR preferred=0x%llx actual=0x%llx relocated=%d\n",
              static_cast<unsigned long long>(preferred),
              static_cast<unsigned long long>(image.image_base()),
              preferred != image.image_base());
  const bool patched = FixtureStub[0] == 0xff && FixtureStub[1] == 0x25;
  if (probe || foreign || reject || outside) {
    std::printf("unpatched=%d live-base-ok=1\n", !patched);
    return patched ? 5 : 0;
  }
  if (!patched) { std::puts("FAIL: hook not installed"); return 6; }

  oggsound::Pcm pcm; std::string error;
  double decode_ms = 0;
  if (!oggsound::DecodeOgg(ogg.data(), ogg.size(), &pcm, &error, &decode_ms)) return 7;
  std::vector<uint8_t> wav; oggsound::BuildWav(pcm, &wav);
  auto check = [&](const std::vector<uint8_t>& bytes, bool expected, int freesrc) {
    Ops ops = MakeOps(bytes); Spec spec{}; uint8_t* out = nullptr; uint32_t len = 0;
    bool loaded = FixtureLoader(&ops, freesrc, &spec, &out, &len) != nullptr;
    bool ok = loaded == expected && ops.closed == freesrc;
    if (loaded) ok = ok && spec.rate == pcm.rate && spec.channels == pcm.channels &&
                     len == pcm.bytes.size() && std::memcmp(out, pcm.bytes.data(), len) == 0;
    if (out) HeapFree(GetProcessHeap(), 0, out);
    return ok;
  };
  bool ok = check(wav, true, 0) && check(wav, true, 1) &&
            check(ogg, true, 0) && check(ogg, true, 1) &&
            check(std::vector<uint8_t>{'O','g','g','S',0,0,0}, false, 1);
  std::printf("hooked=%d live-base-ok=1 WAV/Ogg/invalid/freesrc/PCM=%s\n", patched, ok ? "OK" : "FAIL");
  return ok ? 0 : 8;
}
