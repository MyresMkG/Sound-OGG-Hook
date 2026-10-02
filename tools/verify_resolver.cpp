// Host-side check of the anchor resolver: run it against a stellaris.exe on
// disk and compare what it finds with known values.
//
//   verify_resolver.exe <stellaris.exe> [expected_stub_rva] [expected_call_rva] [expected_slot_rva]
//
// Exit code 0 when everything (including the given expectations) matched.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "image.h"
#include "log.h"
#include "resolver.h"

using namespace oggsound;

namespace {

uint32_t ParseNumber(const wchar_t* text) {
  return (uint32_t)wcstoul(text, nullptr, 0);
}

bool Eq(uint32_t got, uint32_t expected) { return expected == 0 || got == expected; }

}  // namespace

int wmain(int argc, wchar_t** argv) {
  LogInit(nullptr);
  bool force_scan = false;
  std::vector<const wchar_t*> args;
  for (int i = 1; i < argc; ++i) {
    if (wcscmp(argv[i], L"--scan") == 0) {
      force_scan = true;
    } else {
      args.push_back(argv[i]);
    }
  }
  if (args.empty()) {
    wprintf(L"usage: verify_resolver.exe <stellaris.exe> [stub_rva] [call_rva] [slot_rva] [--scan]\n"
            L"  --scan  skip the log-string anchors and use the byte-pattern scan only\n");
    return 2;
  }
  const std::wstring path = args[0];
  const uint32_t expect_stub = args.size() > 1 ? ParseNumber(args[1]) : 0;
  const uint32_t expect_call = args.size() > 2 ? ParseNumber(args[2]) : 0;
  const uint32_t expect_slot = args.size() > 3 ? ParseNumber(args[3]) : 0;

  Image img;
  if (!img.InitFromFile(path)) {
    wprintf(L"FAIL: could not read %ls\n", path.c_str());
    return 2;
  }

  Resolution res;
  bool resolved = false;
  if (!force_scan) {
    resolved = ResolveLoadWav(img, &res);
  } else {
    wprintf(L"method          : byte-pattern scan only (--scan)\n");
  }
  if (!resolved) {
    if (!force_scan) wprintf(L"method          : anchors failed, byte-pattern scan\n");
    resolved = ResolveByScan(img, &res);
  }
  if (!resolved) {
    wprintf(L"FAIL: resolution failed for %ls (see sound_ogg_hook.log)\n", path.c_str());
    return 1;
  }

  wprintf(L"file            : %ls\n", path.c_str());
  wprintf(L"resolved by     : %s\n", res.anchor != nullptr ? res.anchor : "(unknown)");
  wprintf(L"sound loader    : 0x%x..0x%x (size %u)\n", res.loader_rva, res.loader_end,
          res.loader_end - res.loader_rva);
  wprintf(L"load call       : 0x%x\n", res.call_rva);
  wprintf(L"stub            : 0x%x\n", res.stub_rva);
  wprintf(L"slot            : 0x%x (holds 0x%llx)\n", res.slot_rva,
          (unsigned long long)res.slot_value);

  bool ok = true;
  if (!Eq(res.stub_rva, expect_stub)) {
    wprintf(L"FAIL: stub 0x%x, expected 0x%x\n", res.stub_rva, expect_stub);
    ok = false;
  }
  if (!Eq(res.call_rva, expect_call)) {
    wprintf(L"FAIL: call 0x%x, expected 0x%x\n", res.call_rva, expect_call);
    ok = false;
  }
  if (!Eq(res.slot_rva, expect_slot)) {
    wprintf(L"FAIL: slot 0x%x, expected 0x%x\n", res.slot_rva, expect_slot);
    ok = false;
  }

  // The slot normally holds SDL's bootstrap trampoline in a freshly loaded
  // image; the real implementation is what the dynapi installer writes into it.
  // Both are resolved here without running anything, and the real one is checked
  // for the RIFF/WAVE identifiers that make it a WAV parser.
  uint32_t implementation = 0;
  const uint32_t slot_target = (uint32_t)res.slot_target_rva;
  if (slot_target != 0) {
    if (FunctionJumpsThroughSlot(img, slot_target, res.slot_rva)) {
      implementation = FindInstalledImplementation(img, res.slot_rva);
      wprintf(L"slot holds      : bootstrap trampoline 0x%x\n", slot_target);
      wprintf(L"installed impl  : 0x%x (from the dynapi installer)\n", implementation);
    } else {
      implementation = slot_target;
      wprintf(L"installed impl  : 0x%x (already in the slot)\n", implementation);
    }
  } else {
    wprintf(L"installed impl  : outside the image (SDL not initialised in this file image)\n");
  }

  if (implementation != 0) {
    const bool wave_like = LooksLikeWaveLoader(img, implementation, 2);
    wprintf(L"impl is WAV     : %ls (RIFF/WAVE ids)\n", wave_like ? L"yes" : L"NO");
    if (!wave_like) {
      wprintf(L"FAIL: the implementation does not look like SDL's WAV parser\n");
      ok = false;
    }
  } else {
    wprintf(L"FAIL: could not determine the installed implementation\n");
    ok = false;
  }

  wprintf(ok ? L"OK\n" : L"FAILED\n");
  return ok ? 0 : 1;
}
