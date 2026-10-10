# sound_ogg_hook：HOI4 Ogg Vorbis 音效支持

Windows x64 DLL，通过 `hoi4_mod_injector` 加载，让 HOI4 的音效 `.asset` 可以引用 Ogg Vorbis。原版 WAV 继续由游戏的 SDL 加载，Ogg 在内存中解码并合成 WAV，输出缓冲仍由 SDL 分配、由游戏照常释放。音乐播放器原本支持 Ogg，此插件主要用于音效。

## 安装

先按 `hoi4_mod_injector` 的说明安装代理 DLL。把本目录的 `sound_ogg_hook.dll` 和 `sound_ogg_hook.ini` 放到：

```text
Hearts of Iron IV/
├── hoi4.exe
├── dxgi.dll                     hoi4_mod_injector 的代理（示例）
└── injected_mods/
    ├── hoi4_mod_injector.ini
    └── sound_ogg_hook/
        ├── sound_ogg_hook.dll
        └── sound_ogg_hook.ini
```

注入器只加载 `injected_mods/<一级子目录>/*.dll`。不要把音效 DLL 放在 `injected_mods` 本层或再深一层。此 DLL 只在宿主文件名为 `hoi4.exe` 时启动。正常使用须在注入器 INI 的 `[injector]` 下设置 `probe=0`，然后重新启动游戏。

这只是音频插件，普通 HOI4 内容模组仍须在启动器或 `dlc_load.json` 中启用。

## 内容模组示例

在已启用的普通内容模组中放置 `sound/example.ogg`，并在 `sound/example.asset` 中写：

```text
sound = {
    name = "my_ogg_sound"
    file = "example.ogg"
    volume = 1.0
}
```

由游戏脚本或界面引用该音效名。扩展名 `.ogg` 必须实际包含 **Ogg Vorbis**；Ogg Opus、MP3、FLAC 不在本插件支持范围内。支持单声道和立体声，游戏继续负责后续格式转换。

## 配置与诊断

参数及取值写在随包 INI 的中文注释中。配置与 `sound_ogg_hook.log` 均位于音效 DLL 旁边；每次启动重写日志。注入器自己的日志在 `injected_mods/hoi4_mod_injector.log`。

- `init: hook live`：安装成功。
- `load: pass-through ... 52 49 46 46`：原版 WAV 正常通过。
- `load: cache hit`：使用后台预解码缓存。
- `load: decoded on demand`：按需解码成功。
- `probe-only: resolution finished`：仅检查地址，没有安装钩子。
- `not installed`：查看之前的解析或校验错误，当前未启用 Ogg 支持。

注入器设 `probe=1` 会在 DLL 旁生成 `diplo_action_hook_probe_only.txt`，此插件识别该约定；改回 `probe=0` 会由注入器清除。手动放置 `sound_ogg_hook_probe_only.txt` 也能探测，但须手动删除该文件才能恢复正常安装。

预取扫描游戏的 `sound`、`dlc`、`integrated_dlc`、系统“文档”下 `Paradox Interactive/Hearts of Iron IV/mod`、`.mod` 描述文件的 `path` 和当前 Steam 库的工坊 `394360`。只处理磁盘上能访问的 `.asset` 所引用的 Ogg 音效，跳过 `music` 块。压缩模组、其他 Steam 库或未找到的路径可能不预取；游戏最终提供 Ogg 流时仍会按需解码。扫描不等同于启动器启用列表，可能预取未启用模组的文件；缓存受预算限制。

## 兼容与验证

2026-10-10 移植目标为 HOI4 1.19.3 Windows x64。动态解析 SDL 的加载桩，校验真实实现含 RIFF/WAVE 标识后才安装；不使用固定版本地址。兼容 ASLR。安装后 DLL 留驻至进程退出，卸载或更新请先退出游戏。

具体测试结果见随包 `验证记录_20261010.txt`。离线解析和模拟宿主测试不能代替实际游戏试听；本次没有宣称完成真实游戏 Ogg 音效试听，也未验证其他版本、多人游戏或 Linux/macOS。

源码与一键构建位于 `source_code/sound_ogg_hook_src`。插件不修改磁盘上的 `hoi4.exe`。

## 源码构建与验证

源码由群星版移植，当前行为与本 README 均以 HOI4 为准，旧版群星实机记录不代表此构建通过了 HOI4 实机试听。

```text
sound_ogg_hook_src/
├── build.bat                    MinGW-w64，一键构建并运行 selftest
├── src/                         挂钩、PE 解析、解码、缓存、预取与日志
├── release/                     发布 README 和默认 INI 的源文件
├── tools/
│   ├── verify_resolver.cpp      读取真实 HOI4 可执行文件，验证动态解析
│   ├── selftest.cpp             WAV、RWops、指纹、缓存、asset、配置与真实解码
│   ├── fixture.S                Win64 dynapi 桩、bootstrap 和安装器模拟
│   ├── integration_host.cpp     模拟音效宿主，核对完整 PCM 与 freesrc
│   ├── run_tests.py             真实注入器加载上述宿主的自动验证
│   └── publish_release.py       核对已测 DLL、写验证记录与校验和、生成 ZIP
├── build/                       工具构建及原始验证记录
└── test_out/                    自动测试临时宿主、INI 和日志
```

需要 64 位 MinGW-w64 的 gcc/g++。放在 PATH 中，或设置 `MINGW_BIN`。工具为静态链接，不要求玩家额外安装 libstdc++、libgcc 或 winpthread DLL；仍使用 Windows 系统 UCRT。

```bat
build.bat
set MINGW_BIN=D:\Program Files\mingw64\bin
build.bat
```

默认发布到 `..\..\full_releases\sound_ogg_hook`。设置 `OUTDIR` 可改输出路径；设置 `OGG_FILE` 可提供单声道/立体声 Ogg Vorbis 自测样本。未提供样本时，selftest 尝试读取本机 HOI4 音乐。构建以 `-Wall -Wextra -Werror` 编译；默认配置从 `release/sound_ogg_hook.ini` 检验，输出目录已有 INI 保留，README 更新。

```bat
set OUTDIR=D:\tmp\hoi4_ogg
set OGG_FILE=D:\audio\sample.ogg
build.bat
build\verify_resolver.exe "D:\SteamLibrary\steamapps\common\Hearts of Iron IV\hoi4.exe"
build\verify_resolver.exe "D:\SteamLibrary\steamapps\common\Hearts of Iron IV\hoi4.exe" --scan
build\selftest.exe "D:\audio\sample.ogg" release
py -3 tools\run_tests.py
py -3 tools\run_tests.py --no-build --ogg "D:\audio\sample.ogg"
py -3 tools\publish_release.py
```

自动验证用 Python 3 标准库和 MinGW 工具。集成用例使用本工程已发布的 `full_releases/hoi4_mod_injector/dxgi.dll`，所有文件写在当前源码目录内，不启动或修改真实游戏。默认对工作区的 `hoi4_1.19.3.exe` 和本机已安装的 `hoi4.exe` 各跑锚点与扫描两条解析；`--game` 可指定其他 PE 文件，但固定 RVA 断言仅对 MD5 为 `2c13d60db727e59dd21b4f27654c4a66` 的已核对样本启用。

### HOI4 1.19.3 的定位证据

对本工作区的 `hoi4_1.19.3.exe` 和本机已安装的 `hoi4.exe`，两条解析均得到：

| 项目 | RVA |
| --- | --- |
| 音效加载函数 | `0x23c1680..0x23c1bdf` |
| SDL_LoadWAV_RW 调用 | `0x23c1787` |
| dynapi 桩 | `0x20bd830` |
| 可写跳转槽 | `0x30b6988` |
| bootstrap | `0x20b5f30` |
| 安装器写入的 WAV 实现 | `0x20d2140` |

`hoi4_1.19.3_win_source.cpp` 的 `FUN_1423c1680` 可独立核对：局部 RWops 含 size/seek/read/write/close/type/hidden；调用 WAV 桩后执行音频转换，最后通过 SDL 释放原始缓冲。本文 RVA 只用于核对，不是 DLL 的硬编码地址。

### 实现约束

解析链为日志字符串 → RIP 引用 → `.pdata` 函数边界 → 五参数调用 → dynapi 桩/槽 → bootstrap 安装器 → RIFF/WAVE 实现校验。锚点失败后自动扫描；无法验证实现时拒绝安装，连手动 RVA 覆盖也须验证。

实时模块地址显式使用 `GetModuleHandleW(nullptr)` 返回值，文件解析仍使用 PE 的首选基址。测试将 ASLR 宿主的头部 ImageBase 保持为磁盘首选地址，验证解析与补丁不会误用该地址。

补丁只改原 dynapi 桩的入口，不改 SDL 自己管理的槽，因此第一次调用经过 bootstrap 后仍可读到 SDL 新安装的真实实现。使用已有 INT3 填充构造绝对跳转，入口按 8 字节对齐原子写入。已安装的 DLL 会 pin 到进程退出，避免卸载后跳转到释放的代码。

DLL 依赖 HOI4 mod injector 在游戏 CRT 已初始化后加载；不要把此音效 DLL 当作启动时的系统代理直接导入。DllMain 只检查宿主名并启动工作线程，日志、配置和解析在工作线程执行。

预取对普通目录和 .mod 的 path 生效；archive 不展开。扫描可能包含未启用模组，首次加载按流内容指纹匹配缓存；缓存未命中时同步解码。宽于立体声的 Vorbis 输入由原解码器折叠成立体声，但本次自动验证样本为立体声。

最终测试和发布文件的 SHA256 见成品目录的验证记录。实际游戏启动、Ogg 音效试听、完整游戏流程和联机兼容仍须另行验证。
