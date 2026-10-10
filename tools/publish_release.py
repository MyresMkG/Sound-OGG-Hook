"""Package only the DLL proven by build/validation_20261010.txt, with docs.

py -3 tools/publish_release.py
No game installation is modified; output stays under full_releases.
"""
from pathlib import Path
import hashlib
import shutil
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parents[1]
OUT = WORKSPACE / 'full_releases/sound_ogg_hook'


class PE:
    def __init__(self, path):
        self.bytes = Path(path).read_bytes()
        self.nt = struct.unpack_from('<I', self.bytes, 0x3c)[0]
        assert self.bytes[:2] == b'MZ' and self.bytes[self.nt:self.nt + 4] == b'PE\0\0'
        self.machine, count = struct.unpack_from('<HH', self.bytes, self.nt + 4)
        size = struct.unpack_from('<H', self.bytes, self.nt + 20)[0]
        self.optional = self.nt + 24
        assert struct.unpack_from('<H', self.bytes, self.optional)[0] == 0x20b
        first = self.optional + size
        self.sections = [struct.unpack_from('<IIII', self.bytes, first + i * 40 + 8)
                         for i in range(count)]  # virtual size, RVA, raw size, raw offset

    def at(self, rva, size):
        for virtual_size, va, raw_size, raw in self.sections:
            if va <= rva and rva + size <= va + raw_size:
                start = raw + rva - va
                return self.bytes[start:start + size]
        raise ValueError(f'unmapped file RVA 0x{rva:x}')

    def cstring(self, rva):
        return self.at(rva, 128).split(b'\0', 1)[0].decode('ascii')

    def imports(self):
        rva = struct.unpack_from('<I', self.bytes, self.optional + 112 + 8)[0]
        out = []
        while rva:
            desc = struct.unpack('<IIIII', self.at(rva, 20))
            if not any(desc):
                break
            out.append(self.cstring(desc[3]))
            rva += 20
        return out


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    dll = OUT / 'sound_ogg_hook.dll'
    log = (ROOT / 'build/validation_20261010.txt').read_text(encoding='utf-8-sig')
    digest = sha(dll)
    assert 'ALL PASSED: real game resolver, selftest, fixture resolver and 9 injector cases' in log
    assert 'DLL SHA256 ' + digest in log, 'published DLL must exactly match the tested build'
    pe = PE(dll)
    assert pe.machine == 0x8664
    imports = pe.imports()
    assert not any(x.lower().startswith(('libgcc', 'libstdc++', 'libwinpthread')) for x in imports)
    evidence = []
    for path in (WORKSPACE / 'hoi4_1.19.3.exe', Path(r'D:\SteamLibrary\steamapps\common\Hearts of Iron IV\hoi4.exe')):
        if not path.exists():
            continue
        game = PE(path)
        code = game.at(0x20bd830, 16)
        assert code[:3] == bytes.fromhex('48ff25') and code[7:14] == b'\xcc' * 7
        evidence.append(f'{path}\n  MD5 {hashlib.md5(game.bytes).hexdigest()}\n'
                        f'  stub RVA 0x20bd830, 8-byte aligned, bytes {code.hex()}')
    for name in ('README.md', 'sound_ogg_hook.ini'):
        shutil.copy2(ROOT / 'release' / name, OUT / name)
    intro = (
        'HOI4 sound_ogg_hook 验证记录 — 2026-10-10\n\n'
        '结论：MinGW x64 构建通过（-Wall -Wextra -Werror），selftest 全通过。\n'
        '两份真实 HOI4 1.19.3 文件的锚点/扫描解析均通过；模拟宿主的两种解析均通过。\n'
        '使用已发布 hoi4_mod_injector/dxgi.dll 的 9 个集成用例全部通过：\n'
        '按需解码、预取命中（integrated_dlc）、纯扫描、手动 RVA、注入器 probe、\n'
        '手动 probe、非 HOI4 宿主不启动、非 WAV 目标拒绝、镜像外目标拒绝。\n'
        '正常用例核对 WAV/Ogg 的完整 PCM、freesrc=0/1、损坏 Ogg 的失败行为。\n'
        '全部模拟宿主确实启用了 ASLR，首选基址与实际基址不同；头部保持磁盘首选地址。\n'
        '正常加载还验证了注入器删除旧探测标记。发布 DLL 与测试 DLL 的 SHA256 相同。\n'
        '以下测试不等同于真实游戏运行或音效试听；本次未启动真实游戏、未修改游戏安装。\n\n'
        f'DLL SHA256 {digest}\nMachine 0x{pe.machine:x} (AMD64)\n'
        'Imports: ' + ', '.join(imports) + '\n\n真实文件补丁前提检查：\n' +
        '\n'.join(evidence) + '\n\n完整自动验证输出：\n\n'
    )
    record = OUT / '验证记录_20261010.txt'
    record.write_text(intro + log, encoding='utf-8')
    files = [dll, OUT / 'sound_ogg_hook.ini', OUT / 'README.md', record]
    manifest = OUT / 'SHA256SUMS.txt'
    manifest.write_text(''.join(f'{sha(p)}  {p.name}\n' for p in files), encoding='utf-8')
    files.append(manifest)
    archive = WORKSPACE / 'full_releases/sound_ogg_hook_hoi4_20261010.zip'
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED) as z:
        for p in files:
            z.write(p, 'sound_ogg_hook/' + p.name)
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
        for p in files:
            assert z.read('sound_ogg_hook/' + p.name) == p.read_bytes()
    print('Published', OUT)
    print('Archive', archive, 'SHA256', sha(archive))
    print('DLL SHA256', digest)


if __name__ == '__main__':
    main()
