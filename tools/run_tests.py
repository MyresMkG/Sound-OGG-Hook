"""Offline HOI4 resolver and actual injector integration checks (no game launch).

py -3 tools/run_tests.py [--game PATH] [--ogg PATH] [--no-build]
Uses MinGW-w64 from PATH/MINGW_BIN; all fixtures stay in this source directory.
"""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parents[1]
RELEASE = WORKSPACE / 'full_releases' / 'sound_ogg_hook'
BUILD = ROOT / 'build'
CASES = ROOT / 'test_out'


def run(args, *, cwd=ROOT, env=None, quiet=False):
    p = subprocess.run([str(x) for x in args], cwd=cwd, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
    text = p.stdout.decode('utf-8', errors='replace').replace('\r\n', '\n')
    if not quiet or p.returncode:
        print(text, end='', flush=True)
    if p.returncode:
        raise RuntimeError(f'command failed ({p.returncode}): {args}')
    return text


def pe_base(path):
    b = path.read_bytes()
    nt = struct.unpack_from('<I', b, 0x3c)[0]
    return struct.unpack_from('<Q', b, nt + 24 + 24)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', type=Path, default=WORKSPACE / 'hoi4_1.19.3.exe')
    ap.add_argument('--ogg', type=Path)
    ap.add_argument('--no-build', action='store_true')
    args = ap.parse_args()
    if 'MINGW_BIN' in os.environ:
        os.environ['PATH'] = os.environ['MINGW_BIN'] + os.pathsep + os.environ['PATH']
    game = args.game.resolve()
    ogg = args.ogg
    if ogg is None:
        files = list(Path(r'D:\SteamLibrary\steamapps\common\Hearts of Iron IV\music').glob('*.ogg'))
        if not files:
            raise RuntimeError('provide --ogg with a valid mono/stereo Ogg Vorbis sample')
        ogg = min(files, key=lambda p: p.stat().st_size)
    ogg = ogg.resolve()
    if not args.no_build:
        env = dict(os.environ, OGG_FILE=str(ogg))
        run(['cmd', '/c', str(ROOT / 'build.bat')], env=env)
    md5 = hashlib.md5(game.read_bytes()).hexdigest()
    print('GAME', game, 'MD5', md5, flush=True)
    expected = ['0x20bd830', '0x23c1787', '0x30b6988'] if md5 == '2c13d60db727e59dd21b4f27654c4a66' else []
    for method in ([], ['--scan']):
        run([BUILD / 'verify_resolver.exe', game, *expected, *method])
    installed = Path(r'D:\SteamLibrary\steamapps\common\Hearts of Iron IV\hoi4.exe')
    if installed.exists() and installed.resolve() != game:
        print('INSTALLED GAME MD5', hashlib.md5(installed.read_bytes()).hexdigest(), flush=True)
        for method in ([], ['--scan']):
            run([BUILD / 'verify_resolver.exe', installed, *method])
    run([BUILD / 'selftest.exe', ogg, ROOT / 'release'])

    host = BUILD / 'integration_host.exe'
    common = ['g++', '-std=c++17', '-O2', '-DNDEBUG', '-Wall', '-Wextra', '-Werror',
              '-static', '-static-libgcc', '-static-libstdc++', '-municode',
              '-Wl,--dynamicbase,--high-entropy-va', '-I' + str(ROOT / 'src')]
    run([*common, '-o', host, ROOT / 'tools/integration_host.cpp', ROOT / 'tools/fixture.S',
         ROOT / 'src/audio.cpp', ROOT / 'src/image.cpp', ROOT / 'src/log.cpp',
         BUILD / 'stb_vorbis.o', '-ldxgi', '-luser32'])
    symbols = run(['nm', '-n', host], quiet=True)
    stub = next(int(line.split()[0], 16) for line in symbols.splitlines()
                if line.split()[-1:] == ['FixtureStub']) - pe_base(host)
    run([BUILD / 'verify_resolver.exe', host])
    run([BUILD / 'verify_resolver.exe', host, '--scan'])

    tests = [
        ('sync', 'normal', 'prefetch=0\n', 0, False),
        ('prefetch', 'normal', 'threads=1\nscan_roots=0\n', 0, False),
        ('scan', 'normal', 'prefetch=0\nforce_scan=1\n', 0, False),
        ('override', 'normal', f'prefetch=0\nloadwav_stub_rva=0x{stub:x}\n', 0, False),
        ('injector_probe', 'probe', '', 1, False),
        ('own_probe', 'probe', '', 0, True),
        ('foreign', 'foreign', '', 0, False),
        ('reject', 'reject', f'loadwav_stub_rva=0x{stub:x}\n', 0, False),
        ('outside', 'outside', f'loadwav_stub_rva=0x{stub:x}\n', 0, False),
    ]
    for name, mode, config, probe, own_flag in tests:
        print('CASE', name, flush=True)
        case = CASES / name
        mod = case / 'injected_mods/sound_ogg_hook'
        mod.mkdir(parents=True, exist_ok=True)
        exe = case / ('other.exe' if mode == 'foreign' else 'hoi4.exe')
        shutil.copy2(host, exe)
        shutil.copy2(WORKSPACE / 'full_releases/hoi4_mod_injector/dxgi.dll', case / 'dxgi.dll')
        shutil.copy2(RELEASE / 'sound_ogg_hook.dll', mod / 'sound_ogg_hook.dll')
        (mod / 'sound_ogg_hook.ini').write_text(config, encoding='utf-8')
        (case / 'injected_mods/hoi4_mod_injector.ini').write_text(
            f'[injector]\ndelay_ms=700\nprobe={probe}\n', encoding='utf-8')
        for f in ('sound_ogg_hook.log', 'sound_ogg_hook_probe_only.txt', 'diplo_action_hook_probe_only.txt'):
            (mod / f).unlink(missing_ok=True)
        if own_flag:
            (mod / 'sound_ogg_hook_probe_only.txt').touch()
        if name == 'sync':  # normal injector mode must remove a stale probe flag
            (mod / 'diplo_action_hook_probe_only.txt').touch()
        if name == 'prefetch':
            sound = case / 'integrated_dlc/test_dlc/sound'
            sound.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ogg, sound / 'sample.ogg')
            (sound / 'sample.asset').write_text('sound={name="fixture" file="sample.ogg"}\n'
                                                'music={file="music_only.ogg"}', encoding='utf-8')
        result = run([exe, ogg, mode, hex(pe_base(host))], cwd=case)
        assert 'relocated=1' in result, 'fixture must actually relocate to exercise ASLR'
        log = (mod / 'sound_ogg_hook.log').read_text(encoding='utf-8', errors='replace') if (mod / 'sound_ogg_hook.log').exists() else ''
        print(log, end='', flush=True)
        if mode == 'normal':
            assert 'hook live' in log and 'WAV/Ogg/invalid/freesrc/PCM=OK' in result
            assert 'pass-through' in log and 'decoded on demand' in log and 'ogg decode failed' in log
            if name == 'prefetch':
                assert 'load: cache hit' in log and 'integrated_dlc' in log
            if name == 'sync':
                assert not (mod / 'diplo_action_hook_probe_only.txt').exists()
        elif mode == 'probe':
            assert 'probe-only: resolution finished' in log and 'hook live' not in log
            assert 'prefetch: starting' not in log
        elif mode == 'foreign':
            assert not log
        elif mode == 'reject':
            assert 'refusing to hook a target' in log and 'hook live' not in log
        elif mode == 'outside':
            assert 'refusing an unverified target' in log and 'hook live' not in log
        if mode != 'foreign':
            injector = (case / 'injected_mods/hoi4_mod_injector.log').read_text(encoding='utf-8', errors='replace')
            assert 'sound_ogg_hook.dll' in injector and 'ok' in injector
    print(f'ALL PASSED: real game resolver, selftest, fixture resolver and {len(tests)} injector cases')
    print('DLL SHA256', hashlib.sha256((RELEASE / 'sound_ogg_hook.dll').read_bytes()).hexdigest())


if __name__ == '__main__':
    main()
