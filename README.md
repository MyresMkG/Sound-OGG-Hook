# sound_ogg_hook —— 让群星音效系统接受 Ogg Vorbis

注入式 DLL。群星（Stellaris）的音效系统只会解析 RIFF/WAVE，本 DLL 在 SDL 唯一的
WAV 加载入口处挂钩，把 **Ogg Vorbis** 音效先解码成 PCM、再以“合成的 WAV”交给游戏原本
的加载代码，于是 `sound/*.asset` 里可以直接写 `file = "sound/xxx.ogg"`。

- 音乐播放器本来就是 libvorbisfile（只认 Ogg），音效侧只认 WAV —— 详细证据见本文档第 2 节。
- **WAV 路径完全不受影响**：非 Ogg 流原样交给真正的 `SDL_LoadWAV_RW`，包括它的报错行为。
- 目标进程里没有任何磁盘改动：DLL 只改自己进程内的一处跳转（可还原），关掉游戏即消失。

（注：使用deepseek-v4.1-flash编写，harness为Kimi Code）

---

## 1. 目录

```
sound_ogg_hook_src/
├── build.bat                 一键编译（MinGW-w64）
├── README.md                 本文件
├── src/
│   ├── dllmain.cpp           入口：独立线程里做事，避开 loader lock
│   ├── hook.cpp              detour + 初始化编排 + 统计日志
│   ├── resolver.cpp          锚点解析（字符串 → 函数 → 调用点 → dynapi 桩）
│   ├── patch.cpp             桩入口的 14 字节绝对跳转（原子写入）
│   ├── image.cpp             只读 PE64 视图（活模块 / 磁盘文件两种来源）
│   ├── rwops.cpp             SDL_RWops 布局识别 + 内存 RWops
│   ├── stream.cpp            通过 RWops 嗅探/指纹/整读（读完必定回绕到 0）
│   ├── audio.cpp             stb_vorbis 包装、WAV 合成、内容指纹
│   ├── cache.cpp             预解码缓存（LRU + 预算）
│   ├── prefetch.cpp          后台扫描与预解码（多线程）
│   ├── sound_asset.cpp       音效 asset 解析 + file = "..." 的路径解析
│   ├── config.cpp            sound_ogg_hook.ini + probe 标志
│   ├── log.cpp               日志（DLL 旁 + OutputDebugString）
│   └── stb_vorbis.c          vendored 解码器（公有领域，v1.22）
└── tools/
    ├── verify_resolver.cpp   宿主机侧锚点验证（离线解析 stellaris.exe）
    └── selftest.cpp          宿主机侧功能自测（WAV、指纹、RWops、缓存、真实解码）
```

编译产物：DLL 落在 `..\sound_ogg_hook_dll\`，两个验证工具落在 `build\`。
`build.bat` 共 5 步：stb_vorbis → DLL → verify_resolver → selftest → **跑一遍 selftest**
（最后一步会解析随包发布的 `sound_ogg_hook.ini` 并断言等于默认值，键名写错即构建失败）。

```
build.bat                    rem 需要 PATH 里有 MinGW-w64 的 g++，或 set MINGW_BIN=...
build\verify_resolver.exe <stellaris.exe> [stub_rva] [call_rva] [slot_rva]
build\selftest.exe [some.ogg] [dir_with_sound_ogg_hook.ini]
```

---

## 2. 为什么只能这么挂

反编译产物（`..\stellaris_4.5_source.cpp`）里的结论：

| 事实 | 证据 |
| --- | --- |
| 音效解码只有一处：`SDL_LoadWAV_RW` | `AudioInternalSoundLoad`，全工程只有这一次调用（`stellaris_4.5_source.cpp:7902042`） |
| SDL 只解析 RIFF/WAVE | `SDL_LoadWAV_RW_REAL`（`:7535100`），`WaveCheckFormat` 只认 PCM/ADPCM/A-law/µ-law/IMA（`:7535761`），MPEG 明确拒绝 |
| 路径没有扩展名白名单 | `SetFilePath` 只拼路径（`:7012533`），门槛只有 `PdxAudio::DoesFileExist`（`:7012990`）；唯一出现 `.ogg` 字面量的是 mod 打包器（`:7055335`） |
| 引擎随后强制统一格式 | `SDL_BuildAudioCVT(..., 0x8010, 2, 0xac44)`（`:7902076`），即 44.1 kHz/S16/立体声 |

所以唯一可行的落点就是那个唯一的 WAV 加载调用（4.5 的 SDL 是 dynapi 间接层，调用会先落到
一个 `jmp qword ptr [rip+disp32]` 桩上），本 DLL 就改这一个桩。

### 挂钩点怎么找（不写死地址）

1. 在 `.rdata` 找 `Failed to load audio "%s". SDL Error: "%s"\n`（备选：`For best performance
   and quality sound files ...`、`SDL failed allocation in pdx_audio ...`）。
2. 扫 `.text` 里 RIP 相对引用，用 `.pdata` 异常目录确定它属于哪个函数 → `AudioInternalSoundLoad`。
3. 在函数体内找 `mov qword ptr [rsp+0x20], reg`（Win64 第 5 个参数）之后 64 字节内的
   `call rel32`，且目标是 `48 FF 25` 桩、且调用后 12 字节内有 `test rax, rax` → 就是那个调用点。
4. 从桩里解出 `jmp qword ptr [rip+disp32]` 的槽地址（`.data`，可写）。
5. **独立复核**：找到 dynapi 安装器写入该槽的 `lea rcx,[rip+impl]`，对 `impl` 扫描 RIFF/WAVE
   魔数，确认它确实是波形解析器（`verify_resolver.exe` 与 probe 模式都会打印这条）。

挂钩后 detour 需要“调原函数”时，直接读槽里的当前值并 call —— 这样不用复制那条 RIP 相对
指令，也不用管 SDL 自举后槽被改写。

### 为什么绕成“合成 WAV”

游戏拿到 PCM 后会：`SDL_BuildAudioCVT` → 用 **SDL 自己的分配器** `SDL_malloc` 申请缓冲 →
`SDL_ConvertAudio` → `SDL_FreeWAV` 释放我们交出的缓冲（`:7902090` 附近）。谁交缓冲谁就得让
SDL 能释放它。把解码结果装进 RIFF/WAVE、喂给真正的 `SDL_LoadWAV_RW`，SDL 就会用**它自己的
分配器**申请缓冲、并把 `SDL_AudioSpec` 填得和读真 WAV 一模一样，整条下游路径零改动。

### 预解码 + 缓存（零延迟）

- **指纹**：`总字节数 + 前 16 KiB 的 FNV-1a`。预取时从磁盘算，加载时通过游戏给我们的
  RWops 算；两者一致才算命中 —— 猜错只会退化成同步解码，**绝不会放错声音**。
- **预取**：后台线程扫描游戏目录（含 `dlc\*`）、`文档\Paradox Interactive\Stellaris\mod\*`、
  Steam 创意工坊 `281990\*`，读遍 `sound\` 下的每个 `.asset`，收集其中 `file = "..."` 引用到的
  `.ogg`（`music = { ... }` 块整块跳过——那些曲子由音乐播放器用 libvorbisfile 播放，永远不经过
  被挂钩的音效入口；没有任何 asset 引用的散装 `.ogg` 同样不预取），按发现顺序解码进缓存，受
  预算与单文件上限约束。日志里这一行就是这条规则的产物：
  `scan finished, 6 .ogg on disk, 4 referenced by sound assets, 4 queued`。
- `file = "..."` 的路径按引擎的顺序找：先 `<root>\<entry>`（`sound/xxx.ogg` 这种写法），再
  `<root>\sound\<entry>`（原版约定），最后 asset 自己所在的目录；取第一个存在的文件。
- **不读 `always_load`**：那个标记只决定游戏是在加载阶段还是首次播放时解码，与之无关；
  恰恰是 `always_load = no` 的音效需要预取，才能做到“不写它也是零延迟”。
  两个预算判断都只看字节：磁盘上超过 `prefetch_max_file_mb` 的直接跳过；其余先用
  Vorbis 头算出解码后的确切体积，剩余缓存放不下也跳过（不会“解完再丢”）。
- **加载时**：先做 4 字节魔数探测（非 Ogg 立刻原样透传）；是 Ogg 就用指纹查缓存，命中则
  直接把 PCM 装 WAV 交出去，未命中才在调用线程上解码。
- 缓存条目**交付即移除**（游戏自己会常驻一份），不会让同一个音效占两份内存。

### 线程与性能

- 运行期每帧 0 开销：混音只读 PCM，我们的代码只在“加载”时运行。
- 本机实测（`selftest.exe`，v1.22 stb_vorbis，`-O2`）：解码吞吐 **约 100–112 MB/s PCM**，
  即 **600–660× 实时**，折算 **每秒 44.1 kHz 立体声约 1.7 ms**。
  真实样本：383 KB 的原版环境音 WAV → 49 KB Ogg（2.14 s）解码 **4.1 ms**。
- 也就是说即使预取没命中，单条音效的同步解码也只有几毫秒；预取的意义是把这几毫秒从
  “首次播放的那一帧”挪到后台。
- 预取线程数默认 `max(1, min(4, 核心数/2))`；缓存默认 256 MB，超出即停止预取（不会挤爆内存）。

---

## 3. 配置

`sound_ogg_hook.ini` 在 `..\sound_ogg_hook_dll\` 里随 DLL 发布，放在 DLL 旁边即生效。
**文件内每个参数都有中文注释**（用途、取值范围、取舍），且所有值都等于代码里的默认值，
所以"放着不管"与"没有这个文件"行为完全一样：

```ini
cache_mb = 256              ; 预取缓存上限（解码后的 PCM，MB），0 = 不缓存=预取无效
threads = 0                 ; 预取线程数，0 = 自动 max(1, min(4, 核心/2))，上限 64
prefetch = 1                ; 0 = 关闭扫描与预取，全部改成按需解码
prefetch_max_file_mb = 24   ; 单文件超过这个大小不预取（不影响能否播放）
scan_roots = 1              ; 0 = 不扫创意工坊目录
verbose = 1                 ; 0 = 只记关键行（错误 + 每 64 条 ogg 的汇总）
force_scan = 0              ; 1 = 跳过锚点，直接走字节特征扫描
loadwav_stub_rva = 0        ; 应急：直接指定桩的 RVA（0x...），跳过锚点解析
```

解析器（`config.cpp`）容忍 `#`/`;` 注释、空行、`[节名]`；键名写错只会在日志里记一行
`config: unknown key ...`，不会影响游戏。`build.bat` 的最后一步会用 `selftest.exe` 把这
个随包发布的 ini 实际解析一遍并断言等于默认值，键名写错会让构建失败。

### probe 模式

DLL 旁边放 `sound_ogg_hook_probe_only.txt`（或注入器的 `--probe` 生成的
`diplo_action_hook_probe_only.txt`），DLL 就只解析地址、装钩子前停下，把每一步写进日志：

```
resolve:   sound loader 0x1b0e1a0..0x1b0e6b9 (size 1305)
resolve:   SDL_LoadWAV_RW call at 0x1b0e39b -> stub 0x1d9a8f0 -> slot 0x27f50a8
init: slot holds SDL's bootstrap trampoline 0x1d98ac0; the dynapi installer will write 0x1daeae0 into it
init: implementation 0x1daeae0 is SDL's WAV parser (RIFF/WAVE ids)
```

新版本游戏只要锚点仍成立就能用；万一不行，用 `loadwav_stub_rva` 应急并反馈。

---

## 4. 验证记录

### 4.1 宿主机锚点验证（`verify_resolver.exe`，2026-09-28）

| 项目 | 4.5.0（`stellaris_4.5.exe`） | 4.5.1（Steam 已安装） |
| --- | --- | --- |
| 音效加载函数 | `0x1b0d680..0x1b0db99` | `0x1b0e1a0..0x1b0e6b9` |
| WAV 调用点 | `0x1b0d87b` | `0x1b0e39b` |
| dynapi 桩 | `0x1d99dd0` | `0x1d9a8f0` |
| 跳转槽 | `0x27f40a8` | `0x27f50a8` |
| SDL 安装的真实实现 | `0x1dadfc0` | `0x1daeae0` |
| 实现含 RIFF/WAVE 魔数 | 是 | 是 |
| 断言结果 | `OK` | `OK` |

两版都通过 `verify_resolver.exe <exe> <stub> <call> <slot>` 的断言，且“真实实现”是工具**独立
解析**出来的（不是写死的期望值），与人工分析一致。两条解析路径都试过：`--scan`（纯字节
特征扫描，不使用任何日志字符串）在两版上也给出**同一组地址**，见
`..\sound_ogg_hook_dll\验证记录_离线工具.txt`。

小贴士：从 Git Bash 调用工具时，含中文的路径会被 MSYS 的参数转换破坏（工具收到乱码路径），
请用 cmd / PowerShell 调用，或先把 exe 复制到一个纯 ASCII 路径下再验证。

### 4.2 功能自测（`selftest.exe`）

覆盖：WAV 头字段与长度、指纹的稳定性/内容敏感性、内存 RWops 的 seek/read/短读/越界、
缓存预算与淘汰、真实 Ogg 解码、以及**“预取算出的指纹 == 加载时从流里算出的指纹”**这条
关键匹配链路（用游戏自带的 `music\*.ogg`）。结果：`all checks passed`。
原始输出见 `..\sound_ogg_hook_dll\验证记录_离线工具.txt`。

### 4.3 实机验证（2026-09-28，Steam v4.5.1 + 21 个 mod + diplo_action_hook 同时注入）

用 `stellaris_mod_injector.exe` 启动游戏两次，测试 mod 里放两个 `.ogg` 音效
（一个 `always_load = yes`、一个 `no`，内容取自原版环境音转码，383 kB WAV → 49 kB Ogg）。
原始日志：`..\sound_ogg_hook_dll\验证日志_实机_预取命中.log` 与 `..._按需解码.log`。

| 运行 | 配置 | 关键日志 |
| --- | --- | --- |
| 1 | 默认（预取开） | `resolve: sound loader 0x1b0e1a0..0x1b0e6b9` → `call at 0x1b0e39b -> stub 0x1d9a8f0 -> slot 0x27f50a8` → `init: implementation 0x1daeae0 is SDL's WAV parser`；`patch: hooked rva 0x1d9a8f0`；`prefetch: 412 root(s) to scan`，扫到并解码了 3 个 ogg；`hook: SDL_RWops layout: size@0x0 seek@0x8 read@0x10 close@0x20 hidden@0x30`；3 行 `load: pass-through for a non-Ogg stream (first bytes 52 49 46 46)`（原版 wav）；**`load: cache hit, 44100 Hz 2 ch 2.14 s (369 kB PCM)`** |
| 2 | `prefetch = 0` | `config: ... prefetch off`；`load: decoded on demand, 44100 Hz 2 ch 2.14 s in 4.4 ms (prefetcher missed)` |
| 3 | `cache_mb = 8`（触发预算分支） | `prefetch: skipping ...\Cosmic Housekeeper.ogg (30.1 MB decoded would not fit the remaining cache, 0.4/8.0 MB used)`（**预解码前**就跳过，不再“解完再丢”）；`prefetch: 1 decoded (0.4 MB, 4 ms total), ... 1 skipped over budget, 0 over prefetch_max_file_mb, 0 failed`；该 ogg 仍以 `load: cache hit` 提供 |

六条路径全部在真进程里走通：地址解析、dynapi 桩改写、RWops 布局识别、非 Ogg 透传、
预取命中、按需解码，外加预取的两条预算分支；游戏正常进主菜单、无崩溃、与 diplo_action_hook
共存。（验证用的测试 mod、注入配置与 `dlc_load.json` 改动事后已全部还原。）

### 4.4 审查修复与复验（2026-09-29）

4.1–4.3 是修订前那版构建的记录（挂钩、解码、缓存这三条路径没有改动）。当天按审查结论改了
以下几处，**其中三处影响行为**：

| 问题 | 处理 |
| --- | --- |
| 补丁先写入、跳转槽地址后发布：那个窗口里进来的调用会把 MZ 头当地址 call | 先发布槽地址（`g_slot_rva` 原子写），再写补丁；`CallOriginal` 在未就绪时返回 nullptr |
| `g_layout_state` 置 1 早于 `g_layout` 赋值，另一个线程可能读到全 0 布局 | 先写布局，再加锁 + release 发布状态 |
| 预取会解码**永远命不中**的 music：`sound\music\*.ogg` 由音乐播放器（libvorbisfile）播放，不经过被挂钩的音效入口 | 改成只预取 asset 引用到的 ogg：新增 `sound_asset.cpp` 解析 `sound\**\*.asset` 的 `file = "..."`，`music = { }` 块整块跳过；没有 asset 引用的散装 ogg 不再入队 |

其余：detour 与两个预取线程都套上异常保护（分配失败不再穿进游戏栈）；`VirtualProtect`
按整段恢复而不是只恢复第一页；`cache_mb` / `threads` / `prefetch_max_file_mb` 做钳制；
`SDL_RWops` 布局识别加上 `type` 字段判别（修掉 pre-2.0.20 布局会被误判成 modern 的问题）；
删掉从未被调用的 `RemoveAbsoluteJump` 与另外两处死代码。

复验结果（同机、改完当天）：

| 项目 | 结果 |
| --- | --- |
| `build.bat`（5 步） | 全过；`-Wall -Wextra` 零警告；末步 selftest `all checks passed` |
| `verify_resolver.exe` 4.5.1 / 4.5.0 ×（锚点 / `--scan`） | 四组 `OK`，地址与 4.1 表逐字一致 |
| `selftest.exe` | 新增 asset 解析用例（嵌套块、`music` 跳过、注释、三种路径解析）全过；解码 107–111 MB/s |
| 预取规则实盘核对 | 扫本机全部 44 个含 `sound\` 的根（游戏 + dlc + 文档 mod + 创意工坊）：原版 114 个 asset、6508 条 `file =` 全部解析成功（0 条未解析）；原先唯一会被预取的 ogg 是工坊音乐 `Cosmic Housekeeper.ogg`，现在正确跳过 |
| 构造样本（每种写法各一条） | `sound\sfx\one.ogg`（`sound\` 相对）、`sound\two.ogg`（根目录相对）、嵌套 `category={soundeffects={sound=…}}` 里的 `three.ogg`、asset 同目录的 `deep.ogg` —— 共 4 条入队；`music = { }` 的 `track.ogg` 与无人引用的 `stray.ogg` 不入队 |

4.3 里 `prefetch: 412 root(s) to scan` 那类行仍然适用，只是 `queued` 现在只会是被 asset 引用到
的文件；扫描结束那行新增了两个计数：
`prefetch: scan finished, N .ogg on disk, K referenced by sound assets, M queued`。

---

## 5. 跨版本兼容性

### 不写死地址，两条独立解析路径

| 路径 | 依赖 | 状态 |
| --- | --- | --- |
| 锚点（首选） | 四个日志字符串之一仍被该函数引用（`Failed to load audio "%s". SDL Error: "%s"\n` 等） | 4.5.0 / 4.5.1 实测通过 |
| 字节扫描（自动兜底） | 只要“第 5 个参数写到 `[rsp+0x20]` + 目标是 dynapi 桩 + 调用后紧跟 `test rax, rax`”这个代码形状还在 | 用 `--scan` 在两版给出同一地址；实机（`force_scan = 1`）也跑通 |

字节扫描会列出全部候选（两版各 3 个），对每个候选**回溯 dynapi 安装器**取出真实实现，再要求
其调用链里含 `RIFF`/`WAVE` 魔数；**只有唯一命中才挂钩**，多个命中就拒绝（宁可不挂也不猜）。

### 成立的前提（以及不成立时会发生什么）

1. 该版本仍用 SDL 的 dynapi 层加载 WAV（桩是 `48 FF 25`，槽在 `.data`）；
2. 音效加载仍集中在一个 `SDL_LoadWAV_RW` 调用上（4.5 全工程实测只有一处）；
3. 调用形状仍是“第 5 参数压栈 + 调用后 `test rax, rax`”（扫描路径的唯一依赖）。

任一条不成立：**不挂钩、不改游戏、不崩溃**——日志写明卡在哪一步，游戏按原版运行。这是刻意
设计的安全网，所以“未知版本”最坏的结果是“没效果”，而不是“进不去游戏”。

### 已知边界

- 更早的年代如果音频后端不是这套 SDL 封装（例如 FMOD 时代），两条路径都找不到目标 →
  不生效（无害）。本机没有那些二进制，无法断言具体从哪一版开始是现在这套。
- 若某版本改用别的入口（按文件名的 `SDL_LoadWAV`、或改走 `SDL_AudioStream`），前提 2 不成立
  → 不生效（无害）。
- SDL 大版本升级导致桩/表结构变化 → 不生效（无害）。`SDL_RWops` 的字段布局是**运行时识别**的
  （SDL 2.0.20 前后各一套），本身不构成版本限制。
- 本 DLL 是 PE64，只适用于 Windows 版（含 Proton 下跑的 Windows 版）。

### 想确认某个版本能不能用

```
build\verify_resolver.exe "<该版本的 stellaris.exe>" [桩RVA] [调用RVA] [槽RVA] [--scan]
```

打印解析结果 + 独立回溯出的真实实现 + RIFF/WAVE 校验；`--scan` 强制走扫描路径。更直接的办法
是在游戏里用 probe 模式（DLL 旁边放 `sound_ogg_hook_probe_only.txt`）跑一次，日志会写全套结论
但不挂钩。应急开关：`force_scan = 1`（强制扫描）、`loadwav_stub_rva = 0x...`（手动指定桩）。

---

## 6. 已知限制

- **只预取 asset 引用到的 .ogg**：`sound\` 下没有被任何 `.asset` 的 `file = "..."` 引用到的 ogg
  （包括 `music = { }` 块里的曲子）一律不预取——它们永远不会经过被挂钩的音效入口。引用写法
  超出「根目录相对 / `sound\` 相对 / asset 同目录」这三种时也预取不到，只是退回按需解码。
- **打包成 .zip 的 mod 不会被预取**：只在磁盘上找散装文件。命不中缓存时按需解码，功能正常，
  只是少了“零延迟”。
- 多声道（>2）的 Ogg 会被 stb_vorbis 下混成 2 声道；引擎本来也只混立体声。
- 解码失败时不主动设置 SDL 错误信息：直接把非 Ogg 的流原样交给 SDL，由它报出它自己的错误
  （游戏日志里因此还是有意义的 `Failed to load audio ... SDL Error: ...`）。
- 缓存上限之外的音效会退化为按需解码（几毫秒/条）。
- 只处理 `SDL_LoadWAV_RW` 这一条路径：音乐（libvorbisfile）本来就支持 Ogg，无需处理。
