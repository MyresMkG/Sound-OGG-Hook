# Stellaris 4.5：sound 目录参数全集、默认值与作用机制

分析日期：2026-10-08。主要依据：[stellaris_4.5_source.cpp](D:/zStudy/stellaris/stellaris_4.5_source.cpp)。文件头表明它是 **Linux x86-64 ELF 的 Ghidra 反编译文本**，不是开发者原始 C++ 源码。

辅助依据：工作目录中的 Linux ELF [stellaris_4_5](D:/zStudy/stellaris/stellaris_4_5)，以及本机 `D:\SteamLibrary\steamapps\common\Stellaris\sound` 下的 `.asset` 文件。ELF 用于还原 token 对应的参数拼写、确认浮点返回值与反编译遗漏的临时对象；**没有启动游戏做听感或 Windows 后端验证**。本文的源码行号均指上述 `.cpp`，汇编地址则指辅助 ELF 的原始虚拟地址，不能直接混用。

“默认值”指 **新定义省略该字段时解析器初始化的值**，不是原版文件填写的值。字符串默认空，不等于省略后一定有效；`name`、被引用的 `sound`、音频文件等通常需要显式提供。文末单列原版配置，避免混淆。

## 1. 六类定义的关系与加载规则

| 类型/块 | 定义什么 | 如何关联 | 对应解析器 |
| --- | --- | --- | --- |
| `sound` | 单个底层音频资源，默认文件及基础音量、声像、优先级 | `soundeffect.sounds` 按它的 `name` 引用 | `SSoundReader` |
| `soundeffect` | 游戏实际触发的音效行为，包括选音、循环、淡入淡出、距离和并发限制 | 包含若干 `sound` 候选，可引用 `falloff`，可被归入 `category` | `SSoundEffectReader` |
| `falloff` | 可复用的空间距离衰减曲线 | `soundeffect.falloff = 曲线名` | `SSoundFalloffReader` |
| `category` | 混音分类与分类压缩器 | `soundeffects = { 音效名 ... }` 指定成员；成员是 `soundeffect`，不是底层文件 | `SCategoryReader` |
| `compressor` | 对混音缓冲做动态范围压缩的参数块 | 放在 `category` 内；全局使用 `master_compressor` / `music_compressor` | `SCompressorReader` |
| `soundgroup` | 一套可激活的音频替换方案，例如不同顾问语音 | 可替换底层文件，或将一个音效重定向到另一音效 | `SSoundGroupReader` |

`compressor.asset` 是文件名，**文件内的顶层块实际为 `master_compressor` 与 `music_compressor`**。工厂顶层不接受一个独立的 `compressor = { ... }` 定义，也不支持给压缩器取 `name` 后引用。`category` 与 `soundgroup` 是不同系统：前者控制混音，后者控制替换。

加载器筛选 `.asset`，逐个解析，最后统一 `PostInit`：先创建底层 `sound`，再创建 `soundeffect`，接着分配 `category`，校验并创建 `soundgroup`。因此跨文件引用并不要求把 `sound` 写在 `soundeffect` 前面，但 `falloff` 在读到定义时便注册，音效创建时需要能找到它。文件名 `sound.asset`、`soundeffects.asset`、`falloff.asset` 本身不决定块类型；同一文件可以放多种合法顶层块。[E1、E2]

简化作用链：

```text
触发 soundeffect 名称
  → 按当前 soundgroup 做一次音效重定向
  → 检查 max_audible，随机选一个 sound
  → 按当前 soundgroup 选择替换文件或默认文件
  → delay / fade / volume / playbackrate / falloff / is3d
  → 按底层 sound.priority 与距离音量排序参与混音
  → 分类音量、分类 compressor（含音乐专用分类）
  → 合并到主缓冲 → master_compressor → 输出
```

## 2. category：全部 3 个参数

| 参数 | 类型/写法 | 引擎默认 | 含义与实际作用 | 证据 |
| --- | --- | --- | --- | --- |
| `name` | 字符串 | 空字符串 | 分类标识。按名称查找；同名分类在创建阶段会找到已有对象，可把其他文件中新声明的音效继续分配给它 | E3、E4 |
| `soundeffects` | 裸音效名列表，如 `{ click interface }` | 空列表 | 将这些 `soundeffect` 的分类指针设为本分类；不是混音候选池，也不是文件列表。找不到音效会报错 | E2、E3 |
| `compressor` | 嵌套参数块，见第 3 节 | 不写块时没有配置覆盖；新分类压缩器为禁用 | 写块后读取一个默认启用的压缩器描述，复制到分类，并标记“提供了压缩器配置”。`enabled = no` 可明确禁用 | E3、E4、A1 |

`category` **没有** `volume`、`parent`、`priority`、`max_audible` 等脚本参数。分类运行时音量初始为 `1`，由 `AudioCategorySetVolume` 改动。游戏设置明确按 `Effects`、`Ambient`、`Voice` 三个名字修改对应滑块音量；定义新的分类名会创建新分类，但不会自动出现一个新的设置滑块。`Weapon` 未在 `CGameApplication::UpdateAudioVolume` 的这三条分类更新分支中单独处理。[E4、E5]

一个音效只有一个分类指针；被多次分配会由后一次赋值替换，不会同时经过多个分类压缩器。未分类音效仍可进入主混音；启用分类诊断时会输出缺少分类的日志。[E2、E6]

## 3. compressor：全部 7 个参数

三种容器共用同一解析器与字段：

| 放置位置 | 作用范围 | 整块省略时 | 写出块但省略 `enabled` 时 |
| --- | --- | --- | --- |
| `category = { compressor = { ... } }` | 该分类中音效的合并信号 | 新分类禁用；已有同名分类不被一个无压缩器块的声明清空 | `yes` |
| `master_compressor = { ... }` | 分类、音乐、未分类声音合并后的主输出 | 新音频上下文的主压缩器禁用 | `yes` |
| `music_compressor = { ... }` | 音乐专用混音分类 | 未显式配置时维持音乐分类原有状态；新分类初始禁用 | `yes` |

初始状态可见 `AudioInit` 对主压缩器字段清零，并创建初始禁用的 `Music` 分类；整块存在时的启用默认则来自工厂的临时描述初始化。[E1、E32]

下面默认值指 **压缩器块存在、字段省略** 的情况。dB 字段与线性音量不要混用。

| 参数 | 类型/单位 | 脚本层默认 | 内部值/效果 | 证据 |
| --- | --- | --- | --- | --- |
| `enabled` | 布尔 `yes/no` | `yes` | 控制此压缩器；还受全局 `Audio.Compressor` 开关控制。禁用时本函数直接跳过，`postgain` 也不应用 | E7、E8 |
| `pregain` | 浮点，dB | `0` | 转为 `10^(dB/20)`，默认内部 `1`。**此实现乘在电平检测输入上**，使压缩更早/更晚触发；没有把这一倍数直接乘到输出样本上 | E7、E8 |
| `postgain` | 浮点，dB | `0` | 转为线性倍率，默认 `1`；乘到压缩后的输出样本上，正值补偿音量，负值衰减 | E7、E8 |
| `ratio` | 浮点，比率 R | `1` | 超阈值部分的压缩比例；`1` 不产生正常的向下压缩，`2` 为 2:1，越大压缩越强。解析器只对 `0` 特判改为 `1`，没有通用范围校验 | E7、E8 |
| `threshold` | 浮点，dB，参考满幅样本 | `0` | 检测包络的触发阈值；通常写负值，越低越容易压缩。内部仍保留 dB 值 | E7、E8 |
| `attacktime` | 浮点，秒 | `0` | 电平上升时检测包络跟随的时间常数；越小越快，`0` 对应平滑系数 `0`、即时跟随 | E7、E9 |
| `releasetime` | 浮点，秒 | `0` | 电平下降时检测包络回落的时间常数；越大回落越慢。原版音乐的 `100` 是 **100 秒**，不是 100 毫秒 | E7、E9 |

### 实际计算

`pregain/postgain` 的转换为 `exp2(dB × 0.1660964)`，数值等价于 `10^(dB/20)`。检测左右声道的绝对幅值，各自跟随包络，再取较大者，给左右声道使用同一个压缩倍率，因此具有立体声联动。[E8]

```text
P = 10^(pregain / 20)
Q = 10^(postgain / 20)
envL/envR = 平滑(abs(inputL/inputR) × P)
level_dB = 20 × log10(max(envL, envR) / 32768)，检测幅度有最低值保护
gain_reduction_dB = min(0, (threshold - level_dB) × (1 - 1 / ratio))
output = input × 10^(gain_reduction_dB / 20) × Q
```

这是源码里这一实现的公式；不能直接写成“先把输入放大 P 倍再把放大的声音输出”。例如 `ratio = 1`、`postgain = 0` 时，提高 `pregain` 本身不会把输出放大。分类压缩对分类中声音的**总和**起作用，多个声音叠加会一起推高检测电平。[E6、E8]

包络系数 `a = exp(-1 / (44100 × time))`，上升/下降选择 `attacktime/releasetime` 各自系数；这是指数跟随的时间常数，不是固定等待后突然改变音量。输入负时间、负比率等虽然缺少充分校验，不能按正常压缩器理解其结果。[E9]

## 4. falloff：全部 5 个参数

| 参数 | 类型/写法 | 引擎默认 | 含义与实际作用 | 证据 |
| --- | --- | --- | --- | --- |
| `name` | 字符串 | 空字符串 | 衰减曲线标识，供 `soundeffect.falloff` 引用 | E10 |
| `min_distance` | 浮点，游戏空间坐标距离 | `0` | 低于此距离，距离音量维持 `1`；从此处开始衰减 | E10、E11 |
| `max_distance` | 浮点，游戏空间坐标距离 | `100` | 距离音量衰减到 `0` 的位置；**不是立即销毁实例的距离**，另有 `2 × max_distance` 的远距离判定 | E10、E11、E12 |
| `height_scale` | 浮点，比例 | `1` | 将高度轴 Y 的相对距离乘此系数后再求距离；`0.5` 降低高度对距离的影响，`2` 增强。`<= 0` 被计算代码回退为 `1`，不能写 `0` 来消除高度影响 | E10、E11 |
| `type` | `linear` / `logarithmic` | `logarithmic`，枚举 `1` | `linear` 为 `1-t`；`logarithmic` 为 `-log10(0.1+0.9t)`，见下表。值比较不区分大小写；其他值报错并保留原值 | E10、E11、A2 |

### 距离与曲线公式

```text
h = height_scale > 0 ? height_scale : 1
d = sqrt(dx² + (h × dy)² + dz²)
span = max_distance - min_distance
t = clamp((d - min_distance) / (span > 0 ? span : 1), 0, 1)
linear:      Vdistance = 1 - t
logarithmic: Vdistance = -log10(0.1 + 0.9 × t)
too_far = d > 2 × max_distance
```

| 归一化距离 t | `linear` | 默认 `logarithmic` |
| --- | --- | --- |
| 0 | 1.0000 | 1.0000 |
| 0.25 | 0.7500 | 0.4881 |
| 0.50 | 0.5000 | 0.2596 |
| 0.75 | 0.2500 | 0.1107 |
| 1 | 0.0000 | 0.0000 |

同样的起止距离，默认对数曲线在中段更小。这里的距离来自声音位置与听者/摄像机矩阵的位置，不是游戏脚本中的超空间航道跳数或银河地图星系距离。[E11、E12]

`max_distance` 到 `2 × max_distance` 区间，距离音量已经为零，代码仍可能保留或创建底层声音。超过两倍距离，第一次启动的非循环音效可跳过创建；循环音效仍允许创建，并取得零距离音量。对已有声音，远距离分支检查的是**底层实例的循环标志**：非连续循环的底层声音可以按 `fade_out` 停止；连续 loop 的底层声音则跳过这段更新，未重新写零距离音量或方向声像。若听者直接跳出两倍范围，连续循环实例可能保留此前的距离音量；不能假定所有实例都会在远处立即静音或停止。调试扩展范围另有 `+500` 容差，不属于正常配置参数。[E12]

`falloff` 与 `is3d` 独立：即使 `is3d = no`，指定有效 `falloff` 仍会距离衰减；单写 `is3d = yes` 而不提供 `falloff`，这个音效更新路径不会计算方向声像。[E12]

## 5. soundgroup：全部 7 个参数，以及嵌套块

| 参数 | 类型/写法 | 引擎默认 | 含义与实际作用 | 证据 |
| --- | --- | --- | --- | --- |
| `name` | 字符串 | 空字符串，缺失会报错并拒绝注册 | 替换组标识。原版用于 `l_english`、顾问语音等名称；名字不天然产生混音分类或播放触发器 | E13 |
| `sort_order` | 整数 | `0` | 创建时按数值从小到大插入组列表，影响 `AudioGroupGet` 枚举顺序；不是声音混音优先级 | E13、E14 |
| `soundoverride` | 可重复的 `{ name = ... file = ... }` 块 | 无替换 | 给指定底层 `sound` 添加本组专用文件；保持该 `sound` 的音量、优先级等描述 | E13、E15 |
| `soundfileoverrides` | 映射块 `{ 底层sound名 = "文件" ... }` | 空映射 | `soundoverride` 的批量写法；键是底层 `sound`，值是音频路径 | E13、E15 |
| `soundeffectoverride` | 可重复的 `{ name = ... override = ... }` 块 | 无替换 | 将 `name` 指定的音效重定向到 `override` 指定的另一 `soundeffect` | E13、E16 |
| `soundeffectoverrides` | 映射块 `{ 原音效名 = 替代音效名 ... }` | 空映射 | 音效重定向的批量写法；两端都必须是已经定义的 `soundeffect` | E13、E16 |
| `is_extra` | 布尔 | 无持久默认字段；省略无变化 | **读取但丢弃**：本解析器把值读入栈上临时布尔，没有保存到组对象，也没有传给 `AudioGroupCreate`，不能确认有任何实际效果 | E13、A3 |

两个单项块的全部字段：

| 所在块 | 参数 | 类型 | 默认 | 作用 |
| --- | --- | --- | --- | --- |
| `soundoverride` | `name` | 字符串 | 空 | 被替换的底层 `sound` 名 |
| `soundoverride` | `file` | 路径字符串 | 空 | 当前组的替代文件；遵循第 6 节的相对路径规则 |
| `soundeffectoverride` | `name` | 字符串 | 空 | 原 `soundeffect` 名 |
| `soundeffectoverride` | `override` | 字符串 | 空 | 替代 `soundeffect` 名，不是文件路径 |

### 激活后的作用

音频上下文只保存一个当前激活组指针。切组时会重新加载带文件替换的声音，并更新音效的重定向指针；部分已有播放实例会被终止或重新加载，不能把切组视为对现有播放完全无影响。[E17]

触发时，`AudioSoundEffectPlayHandle` 最多读取**一层**重定向：`A → B` 后使用 B 的声音池、音量、循环、衰减和分类；不会再递归追踪 `B → C`。文件替换只换音频文件，不把底层 `sound` 的全部参数换掉。[E18]

找不到本组专用文件映射时回退默认 `file`；若无默认文件，加载函数还有使用首个专用文件的兜底路径。文件存在检查失败的替换记录不会正常加入。不要依赖“只有一组文件、其他组自动静音”这种假设。[E15、E19]

同名 `soundgroup` 在工厂阶段会报错，后定义不合并。一个组内重复的音效替换会报错并跳过后项；对同一个 sound 的同组文件替换，在注册已有对应记录时有“记录日志并覆盖文件”的代码。音效替换与底层文件替换不是同一种重复处理规则。[E13]

## 6. sound：全部 6 个参数

| 参数 | 类型/单位 | 引擎默认 | 含义与实际作用 | 证据 |
| --- | --- | --- | --- | --- |
| `name` | 字符串 | 空字符串 | 底层音频资源标识，供音效候选池或文件替换引用；不负责自动播放 | E20 |
| `file` | 路径字符串 | 空字符串 | 默认音频文件。普通路径相对当前 `.asset` 所在目录；前导 `/` 表示虚拟游戏根目录路径，解析时去掉这一个 `/` | E20、E21 |
| `volume` | 浮点，线性音量倍率 | `1` | 底层资源的基础音量，和音效音量等相乘；`0.5` 为振幅减半，不是减半 dB | E20、E22 |
| `pan` | 浮点，通常 `[-1,1]` | `0` | 固定声像：`-1` 左，`0` 中央，`1` 右；运行时 setter 钳制到此区间。**首次播放的应用时序存在疑点，见第 8 节** | E20、E22、E23 |
| `priority` | 浮点，相对排序系数 | `1` | 混音排序分数为 `priority × 当前距离音量`，排序后高分先处理；影响混音预算竞争，不改变音量本身，也不改变 `max_audible` | E20、E6 |
| `always_load` | 布尔 | `no` | `yes` 时在创建声音资源时尝试加载并保留基础引用；`no` 时按播放需要加载、引用归零后释放。**不是自动播放或循环开关** | E20、E22、E24 |

路径示例：

| `.asset` 的位置 | `file` | 解析结果 |
| --- | --- | --- |
| `sound/my_pack.asset` | `"ui/click.wav"` | `sound/ui/click.wav` |
| `sound/my_pack/defs.asset` | `"click.wav"` | `sound/my_pack/click.wav` |
| `sound/my_pack/defs.asset` | `"/sound/ui/click.wav"` | `sound/ui/click.wav` |

前导 `/` 是引擎 VFS 的根相对写法，不是操作系统的磁盘绝对路径。`soundoverride.file`、`soundfileoverrides` 采用同样的基目录规则。本次追到的底层加载路径调用 `SDL_LoadWAV_RW`；不能仅靠更换文件扩展名让这个 `sound` 加载路径获得 OGG 解码能力。[E19、E21]

基础音量与音效音量可概括为：

```text
音效随机音量 = clamp(soundeffect.volume + 随机偏移, 0, 1)
底层目标音量 = sound.volume × 音效随机音量
混音振幅再乘：运行时实例音量 × 距离音量 × 分类滑块 × 总音量/音效音量设置
随后经过分类压缩器与主压缩器
```

混音回调还把具体声道的倍率钳到可用区间；调大某个 `volume` 并不保证可以无限放大。`priority` 参与排序而不参与上述振幅乘法。[E6、E12、E22]

此 SDL 后端有约 248 个非音乐底层实例槽，但每个回调只让前 **32 个**有效样本实例以正常倍率混音，后续实例可继续推进时间而以零倍率处理；音乐实例在声音排序后加入，再反向遍历，所以这个 32 的预算不能简单理解为“允许 32 个音效再加无限音乐”。`max_audible = -1` 只取消单个音效的限制，不取消这些全局预算。[E6、E22]

## 7. soundeffect：全部 19 个参数，以及声音池

| 参数 | 类型/写法 | 引擎默认 | 含义与实际作用 | 证据 |
| --- | --- | --- | --- | --- |
| `name` | 字符串 | 空字符串 | 游戏调用的音效标识；和底层 `sound.name` 属于不同注册表，可同名 | E25、E26 |
| `sounds` | 嵌套候选块，见下表 | 空列表 | 每次挑选一个底层 `sound`；多项不代表同时叠加播放 | E25、E27、E28 |
| `volume` | 浮点，线性振幅 | `1` | 音效层基础音量；随机偏移加在此值上，随后钳制到 `[0,1]` | E25、E12 |
| `volume_random_offset` | 两个浮点 `{ min max }` | `{ 0 0 }` | 初次选音时均匀抽样 `min + U×(max-min)`，加到 `volume`；循环是否重抽由 `looping_volume_random_offset` 控制 | E25、E12 |
| `delay_random_offset` | 两个浮点，秒 `{ min max }` | `{ 0 0 }` | 推迟底层声音进入播放；**实际 D=max(0,U×(max-min))，没有加 min**。两端同为正数会得到零延迟 | E25、E12、A4 |
| `loop` | 布尔 | `no` | 持续循环直到调用停止；根据是否需要重新选音/随机化，使用底层连续循环，或每段结束后创建下一段 | E25、E12 |
| `looping_volume_random_offset` | 布尔 | `no` | `yes` 时每次循环段启动重新随机音量；仍使用 `volume_random_offset` 的范围，不是另一个范围 | E25、E12 |
| `looping_delay_random_offset` | 布尔 | `no` | `yes` 时每次循环段启动重新计算延迟；仍使用 `delay_random_offset` 的范围 | E25、E12 |
| `random_sound_when_looping` | 布尔 | **`yes`** | 候选数大于 1 时，每次循环重新抽选；`no` 时循环保持当前选中的候选。`loop=no` 时不会因此循环 | E25、E12 |
| `prevent_random_repetition` | 布尔 | `no` | 下一次抽选时临时把上一次候选的权重置零，再抽并恢复；记录在整个音效对象上，由不同实例共享，不是各实例独立历史 | E25、E28 |
| `playbackrate` | 浮点，百分数 | **`100`**；内部 `1` | 播放速率与音高一起改变；写 `120` 为内部 `1.2`，写 `1.2` 是内部 `0.012`，并非 1.2 倍 | E25、E12、E29 |
| `playbackrate_random_offset` | 两个浮点，百分点 `{ min max }` | `{ 0 0 }` | 两端先乘 `0.01`，随机结果加到基础内部速率；例如基础 `100` 加 `{-10 10}` 得 `0.9~1.1` | E25、E12 |
| `looping_playbackrate_random_offset` | 布尔 | `no` | 每次循环段重新随机播放速率，范围仍取 `playbackrate_random_offset` | E25、E12 |
| `fade_in` | 浮点，秒 | `0` | 启动底层声音时从零渐变到目标音量；分段循环重新创建底层声音时也会重新应用，连续底层循环不会逐圈重建 | E25、E12、E22 |
| `fade_out` | 浮点，秒 | `0` | 显式停止或某些远距离停止路径使用；不是要求一个正常播放到文件末尾的声音必定提前淡出 | E25、E12、E30 |
| `max_audible` | 整数 | **`-1`** | 该音效对象的可闻计数上限；任意负值走不限分支，`0` 则一开始就达到限制。溢出标记实例不增加计数；有 falloff 且策略为 silent 时，更新还能释放/重新取得计数 | E25、E12 |
| `max_audible_behaviour` | `fail` / `silent` | **`silent`**，枚举 `1` | `fail`：达到上限时不创建此次底层声音；`silent`：创建、标记溢出，不占计数；**实际静音与动态恢复位于 falloff 分支，无 falloff 时有差异**，见下文。值比较不区分大小写；非法值报错、保留旧值 | E25、E12 |
| `falloff` | 已定义的衰减曲线名 | 空；无衰减曲线指针 | 查找并绑定 `falloff`；查不到会记录缺失曲线日志。空值时不做这一距离曲线衰减 | E25、E26 |
| `is3d` | 布尔 | `no` | 在有 `falloff` 时计算相对听者右方向的声像并传入 `AudioSoundSet3DPan`；仅控制这个方向处理，不决定是否距离衰减 | E25、E12 |

候选池的全部字段：

| 所在层级 | 参数 | 类型/默认 | 作用 |
| --- | --- | --- | --- |
| `sounds` | `sound` | 名称；每个候选隐含 `weight = 1` | 普通候选，可重复多条 |
| `sounds` | `weighted_sound` | 嵌套块 | 带权候选，可与普通候选混用 |
| `weighted_sound` | `sound` | 字符串，默认空 | 对应底层 `sound` 名 |
| `weighted_sound` | `weight` | 整数，默认 `1` | 相对抽样权重；一般正数越大越容易选中 |

```pdx
sounds = {
    sound = clip_a
    weighted_sound = { sound = clip_b weight = 3 }
}
```

正常正权重时，上例选择 A/B 的概率约为 1/4、3/4。权重是整数，不支持脚本 `weight_modifier`。辅助函数把负权重按零统计；总正权重为零时回退为按随机数对候选数取模，所以 `weight = 0` 不保证在所有情况下永不被选中。[E27、E28、E31]

`prevent_random_repetition` 只排除上一项一次，不是洗牌袋或多项历史。只有一个候选时直接返回该候选；当临时排除后所有正权重均为零，兜底抽样仍可能重复。因此它不是无条件保证永不重复。[E28、E31]

### 随机化、循环与并发的组合

首次启动总会按候选数选择声音，并抽取已配置的随机参数。之后：

| 条件 | 实际循环方式 | 后果 |
| --- | --- | --- |
| 只有一个候选，且没有需要在循环重抽的非零随机范围 | 底层音频连续 loop | 最适合保持同一段连续播放；设置 `random_sound_when_looping=yes` 也没有第二个候选可换 |
| 多候选且 `random_sound_when_looping=yes` | 每段播放完成后选下一候选、重新创建底层声音 | 每段可能变化，也受淡入和随机延迟影响 |
| 开启循环音量/延迟/速率重抽，且对应范围非零 | 每段重新创建以应用随机参数 | 不等于连续采样循环；下一段有机会重新淡入或等待 |

`max_audible` 的计数存在于 **soundeffect 对象** 上，不是类别、文件或 `soundgroup` 总上限。两个不同音效引用同一 `sound`，仍分别计数；组将 A 重定向到 B 后，走 B 的计数和规则。计数与处理分支有关，不是根据最终扬声器是否发声统一实时判断。[E12、E18、E30]

| 并发策略/距离配置 | 启动时达到上限 | 后续更新 |
| --- | --- | --- |
| `fail`，有无 falloff 均可 | 放弃这次底层创建 | 已启动实例在普通零音量距离范围里没有 silent 模式的释放/恢复计数逻辑；停止/完成等路径才调整计数 |
| `silent` 且有有效 falloff | 创建实例，溢出标记令距离音量为 0，不增加可闻计数 | 在非 too_far 的距离更新分支中，`距离音量 × 运行时音效音量 <= 0` 时释放计数；该乘积再次为正且存在空位时，清除标记、重新计数并恢复声音。溢出实例本身可以继续推进播放时间 |
| `silent` 但无 falloff | 创建并标记溢出，不增加计数 | 无 falloff 路径直接写运行时音效音量，**未检查溢出静音标记**，也没有上述动态计数调整；静态代码显示此路径不能保证溢出实例真的静音 |

因此，`silent` 有距离曲线时支持正在运行的实例动态恢复，但它不是“等空位后从头播放”的队列。无距离曲线而需要确定抑制并发时，源码支持使用 `fail` 的明确放弃路径；无 falloff 的 silent 听感仍应实机验证。`fail` 只取消底层创建，外层 `SoundEffectInstance` 句柄可能早已返回，所以非空句柄也不证明此次确实发声。[E12、E18]

## 8. 反编译陷阱、特殊行为与不支持的参数

| 项目 | 核对结论 | 使用时的影响 |
| --- | --- | --- |
| 分类压缩器看起来被写成固定默认值 | `.cpp` 丢失栈上 `SCompressorReader` 的临时对象数据流；辅助 ELF 汇编在 `0x3c55d99` 后把读取后的栈数据复制到分类对象 | 不能据此声称原版分类压缩器参数全部无效；第 3 节按实际读取结果说明 |
| `CalculateVolume` 返回 `in_RAX` | Ghidra 误识别浮点返回值，丢失 XMM0 中的 `1-t` 和 `-log10(...)` | 曲线公式已用汇编还原，不把 `in_RAX` 当真实算法 |
| `delay_random_offset` 下界 | 源码与辅助汇编都只有 `U×(max-min)`，随后与零取最大，没有加 `min` | `{2 5}` 得 `0~3` 秒；`{2 2}` 得 `0`。要按普通 `min~max` 理解会出错 |
| `is_extra` | 解析 bool 到临时变量后返回，没有持久写入 | 属于接受语法但未发现效果的参数 |
| `silent` 与有无 falloff | 动态释放/恢复可闻计数、按溢出标记写零音量都在有效 falloff 分支；无 falloff 分支直接写实例音量 | 不能认为 silent 在两种路径中效果完全相同；详见第 7 节 |
| 远距离连续 loop | 已有底层连续循环实例在 too_far 分支跳过距离音量/声像更新 | 听者直接跨出两倍范围时可能保留此前值，不等于无条件静音 |
| `sound.pan` 首次播放 | `AudioSoundPlayHandle` 在将新实例写入上下文槽位**之前**调用 `AudioSoundPan`，而后者先通过句柄找实例，找不到就返回 | 静态 `pan` 虽被读取并尝试应用，但本 SDL 路径首次播放疑似不会生效；需实机确认。3D 更新及注册后的运行时改声像是另外的路径 |
| `height_scale = 0` | 距离代码把 `<=0` 回退成 1 | 不能以 0 实现忽略高度；可以用很小的正数降低影响 |
| 把默认 overflow 写成 fail | 默认枚举初始化为 1，读到 `silent` 同样写 1 | 引擎默认是 silent；原版文件经常显式写 fail，二者要分开 |
| 把播放速率写为倍数 | 解析时乘 `0.01` | `100` 才是默认速度 |
| 把 falloff 默认当 linear | 初始化枚举为 1，解析 `logarithmic` 也写 1 | 默认是 logarithmic |

各解析器的未列出 token 走 `CPersistent::ReadMember` 的通用未识别路径，没有看到额外声音参数处理。以下常见猜测在相应块中没有实现：

| 块 | 本次未找到支持的字段/别名 |
| --- | --- |
| `category` | `volume`、`parent`、`max_audible`、`priority` |
| `compressor` / 全局压缩器块 | `name`、`knee`、`makeup_gain`、`attack`、`release`；有效名字是 `postgain`、`attacktime`、`releasetime` |
| `falloff` | `rolloff`、`curve`、`exponent`、`inverse` 类型 |
| `sound` | `loop`、`pitch`、`stream`、`category`、`soundgroup` |
| `soundeffect` | `file`、`priority`、`pan`、`category`、`soundgroup`、`pitch`、`pitch_random`、`playback_rate`、美式拼法 `max_audible_behavior` |
| `weighted_sound` | 条件 trigger、脚本权重修正；此处就是底层 `sound` 和整数 `weight` |

尤其 `playback_rate` 虽然是引擎别处使用的 token，此音效解析器接受的是 **`playbackrate`**。token 全局存在不代表当前类型实现了它。本机数据也出现 `soundEffects` 的大小写写法，但没有据此增加一个独立参数；这里统一记为 `soundeffects`。

## 9. 可参考的完整写法

以下为说明性定义，两个 WAV 路径需要实际文件。参数示例不改变游戏已有资源；新分类不会自动产生设置滑块。

```pdx
sound = {
    name = demo_clip_a
    file = "my_audio/a.wav"  # 若定义文件在 sound/ 下，结果是 sound/my_audio/a.wav
    volume = 1.0
    priority = 1.0
    always_load = no
}
sound = {
    name = demo_clip_b
    file = "my_audio/b.wav"
}

falloff = {
    name = demo_falloff
    min_distance = 50
    max_distance = 500
    height_scale = 1
    type = logarithmic
}

soundeffect = {
    name = demo_effect
    sounds = {
        sound = demo_clip_a
        weighted_sound = { sound = demo_clip_b weight = 3 }
    }
    volume = 0.5
    volume_random_offset = { -0.05 0.05 }
    playbackrate = 100
    playbackrate_random_offset = { -5 5 }
    delay_random_offset = { 0 0.2 }
    loop = yes
    random_sound_when_looping = yes
    prevent_random_repetition = yes
    looping_volume_random_offset = yes
    looping_delay_random_offset = yes
    looping_playbackrate_random_offset = yes
    fade_in = 0.1
    fade_out = 0.5
    max_audible = 2
    max_audible_behaviour = fail
    falloff = demo_falloff
    is3d = yes
}

category = {
    name = demo_category
    soundeffects = { demo_effect }
    compressor = {
        enabled = yes
        pregain = 0
        postgain = 0
        ratio = 2
        threshold = -12
        attacktime = 0.02
        releasetime = 0.2
    }
}

soundgroup = {
    name = demo_voice_group
    sort_order = 10
    soundfileoverrides = {
        demo_clip_a = "my_audio/a_alternative.wav"
    }
    # 如需整套音效替换，先另行定义 demo_effect_alt，然后写：
    # soundeffectoverrides = { demo_effect = demo_effect_alt }
}
```

定义组只注册替换方案，还需要游戏相应的选择逻辑或音频组激活调用使用它；`soundgroup` 本身不是“播放此组所有声音”的命令。

## 10. 源码与汇编索引

参数表中的 E 编号均为静态源码证据。行号链接可直接打开对应位置；理解一个字段通常需要同时阅读解析器、初始化与使用点。

| 编号 | 定位 | 用途 |
| --- | --- | --- |
| E1 | [LoadAssetsAudio](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7014208)、[工厂 ReadMember](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7012700) | 加载 `.asset`、顶层块分派、默认初始化 |
| E2 | [PostInit](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7013606) | 创建顺序、分类分配、引用校验 |
| E3 | [SCategoryReader](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7017403)、[默认初始化](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7013552) | category 全字段与省略状态 |
| E4 | [AudioCategoryCreate](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7892256) | 分类运行时音量、压缩器状态 |
| E5 | [UpdateAudioVolume](D:/zStudy/stellaris/stellaris_4.5_source.cpp:195023) | Effects、Ambient、Voice 设置对应 |
| E6 | [SDLAudioMixCallback](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7890259) | 优先级×距离音量排序、32 实例混音预算、分类和总压缩 |
| E7 | [SCompressorReader](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7017308)、[全局默认](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7013539) | 压缩器全字段、dB 转换、ratio=0 回退 |
| E8 | [ApplyCompressor](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7897495) | 检测前增益、输出后增益、左右联动与压缩公式 |
| E9 | [SetGain](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7897452)、[SetMasterCompressor](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7892442) | 时间单位、44100 Hz 时间常数 |
| E10 | [SSoundFalloffReader](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7017150)、[默认初始化](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7012891) | 衰减参数、默认类型和距离 |
| E11 | [CalculateVolume](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7900916)、[另一同算法函数](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7897575) | 距离公式、两倍范围、线性与对数曲线 |
| E12 | [UpdateSoundEffectsInternal](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7881298) | 并发、选音、随机偏移、循环、位置、远距离处理 |
| E13 | [SSoundGroupReader](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7017471)、[组默认与登记](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7013252) | 组全字段、sort_order 默认、重复处理 |
| E14 | [AudioGroupCreate](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7882129) | 按 sort_order 升序插入组列表 |
| E15 | [SSoundOverrideReader](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7018206)、[批量文件覆盖](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7017688) | 单项/批量文件替换与存在性检查 |
| E16 | [SSoundEffectOverrideReader](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7019406) | 单项重定向的 name/override |
| E17 | [AudioGroupSetActive](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7892549) | 单一激活组、重载文件与重定向指针 |
| E18 | [AudioSoundEffectPlayHandle](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7883760) | 一层重定向、先返回外层句柄 |
| E19 | [AudioInternalSoundLoad](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7901928) | 文件选择、默认与专用文件兜底、WAV 加载 |
| E20 | [SSoundReader](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7016400)、[sound 默认](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7012914) | sound 全字段、浮点 priority、基础默认 |
| E21 | [SetFilePath](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7012531)、[加载时基目录](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7014263) | `.asset` 相对路径与前导斜杠 |
| E22 | [AudioSoundCreate](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7892701)、[AudioSoundPlayHandle](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7893023) | 文件预载、基础音量、实例槽、pan 调用时序 |
| E23 | [AudioSoundPan](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7893125)、[SetPanning](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7900761) | 声像范围与找实例路径 |
| E24 | [AudioUpdateInternal](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7892380) | 引用计数归零时释放声音缓冲 |
| E25 | [SSoundEffectReader](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7016503)、[音效默认初始化](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7012822) | 19 字段、百分数、布尔与枚举默认 |
| E26 | [AudioSoundEffectCreate](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7883275) | 描述复制、候选 sound 引用与 falloff 查找 |
| E27 | [SSoundsReader](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7016748)、[SSoundInstanceReader](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7016854) | 普通候选、weighted_sound、默认权重 1 |
| E28 | [CalculateNextRandomSoundToPlay](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7900867) | 带权选音、防止上一次重复 |
| E29 | [AudioSoundSetPlaybackRate](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7893355)、[混音重采样](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7891700) | 速率保存与重采样 |
| E30 | [AudioSoundEffectStop](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7883952) | fade_out、底层停止和并发计数 |
| E31 | [GetWeightedRandomAlwaysSuccess](D:/zStudy/stellaris/stellaris_4.5_source.cpp:8011801) | 非正权重、总权重为零的兜底 |
| E32 | [AudioInit](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7892173)、[AudioSetMusicCompressor](D:/zStudy/stellaris/stellaris_4.5_source.cpp:7892504) | 主压缩器与 Music 分类初始状态、音乐压缩器赋值 |
| A1 | 辅助 ELF `SCategoryReader::ReadMember`，`0x3c55d63` 起 | 栈上构造 compressor reader、读取、复制，修正文本的数据流遗漏 |
| A2 | 辅助 ELF `CAudioSoundEffectFalloff::CalculateVolume`，`0x41e2fcc` 起 | XMM0 浮点返回，linear 的减法和 logarithmic 的符号取反 |
| A3 | 辅助 ELF `SSoundGroupReader::ReadMember`，`0x3c564e2` 起 | `is_extra` 读取临时 bool 然后返回 |
| A4 | 辅助 ELF `UpdateSoundEffectsInternal`，`0x41cdb3c` 起 | 延迟只乘 max-min，然后 max(...,0) |

辅助材料保存在 [analysis/sound_4_5](D:/zStudy/stellaris/analysis/sound_4_5)：带原始行号的源码摘录、`tokens.txt`、汇编核对文件、只读分析脚本和本机 `.asset` 参数清单。token 通过辅助 ELF 的 `GetTokenArray` 初始化函数还原，再逐项对照 `.cpp` 的数字分支；没有用其他版本 Wiki 的字段代替源码证据。

## 11. 本机原版配置对照

以下表格由本机文件的实际内容生成，表示本次查看时安装目录的配置；它们不是引擎默认，也不保证与其他补丁、平台或 DLC 组合相同。

### 11.1 压缩器显式配置

| 来源/作用对象 | enabled | pregain dB | postgain dB | ratio | threshold dB | attacktime 秒 | releasetime 秒 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| master_compressor | 省略（默认 yes） | 5.0 | 9.0 | 2 | -10.0 | 0.8 | 0.2 |
| music_compressor | yes | 7 | -5 | 5.0 | -22.5 | 0.100 | 100 |
| category:Weapon | yes | 8.0 | -10.5 | 10.0 | -20.0 | 0.500 | 0.02 |
| category:Effects | yes | 2.0 | 1.5 | 10.0 | -8.0 | 0.30 | 0.020 |
| category:Voice | yes | 4.0 | -2.2 | 3.0 | -25 | 0.30 | 0.8 |
| category:Ambient | yes | 7.0 | 5.0 | 5.0 | -15.0 | 0.030 | 1.2 |

### 11.2 根目录 falloff.asset 的实际曲线

| name | min_distance | max_distance | height_scale | type |
| --- | --- | --- | --- | --- |
| falloff_50 | 50.0 | 100.0 | 1（省略） | logarithmic（省略） |
| falloff_100 | 50.0 | 200.0 | 1（省略） | logarithmic（省略） |
| falloff_200 | 30.0 | 200.0 | 1（省略） | logarithmic（省略） |
| falloff_300 | 40.0 | 300.0 | 1（省略） | logarithmic（省略） |
| falloff_400 | 100.0 | 8000.0 | 1（省略） | logarithmic（省略） |
| falloff_ambient | 200.0 | 1200.0 | 1（省略） | logarithmic（省略） |
| falloff_star | 100.0 | 550.0 | 1（省略） | linear |
| falloff_fleet | 200.0 | 700.0 | 1（省略） | linear |
| falloff_military | 60.0 | 300.0 | 1（省略） | logarithmic（省略） |
| falloff_stations | 60.0 | 650.0 | 1（省略） | linear |
| falloff_pulsar | 100.0 | 10000.0 | 1（省略） | logarithmic（省略） |
| falloff_weapons_distance | 800.0 | 20000.0 | 1（省略） | linear |
| falloff_weapons_close | 50.0 | 2500.0 | 1（省略） | logarithmic（省略） |
| falloff_weapons_xl | 50.0 | 4000.0 | 1（省略） | logarithmic（省略） |
| falloff_bombardment | 100.0 | 2900.0 | 1（省略） | linear |
| vfx_system_effects_audio | 1000.0 | 20000.0 | 1（省略） | logarithmic（省略） |
| falloff_toxic_star | 100.0 | 2000.0 | 1（省略） | linear |
| falloff_toxic_god | 100.0 | 1500.0 | 1（省略） | linear |
| falloff_cosmicstorm | 500.0 | 10500.0 | 1（省略） | linear |
| falloff_kaiju | 30.0 | 10000 | 1（省略） | logarithmic（省略） |

### 11.3 完整性核对

本机 sound 目录共扫描 114 个 .asset 文件（仅磁盘目录，不含未展开的 DLC 或 MOD 归档）。比对各类型实际出现的字段与解析器分支，现有文件使用的字段已全部覆盖；另外补入解析器接受但此批文件未使用的 sound.pan、soundgroup.soundeffectoverride、soundgroup.is_extra。

| 类型 | 解析器字段数（本层） | 附加嵌套字段 |
| --- | --- | --- |
| category | 3 | compressor 的 7 项 |
| compressor | 7 | master_compressor/music_compressor 使用同一 7 项 |
| falloff | 5 | 无 |
| soundgroup | 7 | soundoverride 的 2 项；soundeffectoverride 的 2 项；两种批量映射 |
| sound | 6 | 无 |
| soundeffect | 19 | sounds 的 2 种条目；weighted_sound 的 2 项 |

以上属于静态解析/调用链核对；未用游戏运行测试把所有声音参数逐一测量。第 8 节已把反编译遗漏、确认的特殊行为与尚需实测的声像问题分开说明。
