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
