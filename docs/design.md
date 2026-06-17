# Cardputer-Adv ABC 早教游戏 — 设计文档

> 2026-06-12 · v0.2 · 目标用户:3 岁幼儿

## 目标

利用 Cardputer-Adv 的 56 键全键盘做"每按一键必有声音反馈"的英语字母早教机,内容以**内容包**形式整体可替换。

## 核心交互(已确认,2026-06-12 二次修订)

- 按任意**字母键** → 连播该字母目录下全部 `.wav`(按文件名排序)。**当前内容包每字母只有一段音频:字母名×2 在前 + 拼读音×3 在后,合在一个文件里**;单词、Suno jingle 后续作为追加段(`20-words.wav` / `30-jingle.wav`)加入,固件不改
- 播放期间屏幕**大字母(Aa)↔ 配图 交替闪动**(~700ms),颜色随机变换
- 按**新字母键**随时打断当前播放,立即切新字母(3 岁狂按键场景的核心体验)
- **非字母键** → 轻快"叮"一声(独立音频通道,不打断字母流程;不让任何键"哑");`=`/`-` 调音量(家长用)
- **内容包是一等公民(确认:"版本"= D 内容包)**:不同配音、不同口音(英音/美音)、不同内容(拼音/数字…),每个 = `/packs/<名字>/` 一个目录;切换 = 改 `/packs/active.txt` 一行文字,固件不动
- 首个包:**美音 + 小朋友声**(`us-kid`)

### 按键控制(2026-06-13 v0.8,已确认)

| 键 | 行为 |
|---|---|
| 字母键 | 播该字母(可被新字母打断)|
| 空格 / 回车 | 暂停 / 继续 |
| `=` / `-` 或 `]` / `[` | 音量 +/- |
| `,`(◀ 左) / `/`(▶ 右) | **上一个 / 下一个字母**(整字母切换,A↔Z 环绕)|
| **Fn + Backspace** | **进设置菜单**(家长用,两键键盘对角,娃单手乱拍极难同按)|
| 设置内:`0` 键 | 轮换熄屏时长(30s/1min/2min/5min/never,存 NVS)|
| 设置内:数字键 `1..N` | 选第 N 个内容包(列表 `*` 标当前)|
| 设置内:Backspace | 退出设置 |
| 其它任意键 | "叮" |

- **方向键改为整字母切换**(原 Fn+方向键的段内 word-seek 弃用):对 3 岁玩具,整字母切换更直观,且 `,` `/` 是普通 ASCII 键(必进 word 缓冲),不依赖任何 Fn 重映射,稳。
- **设置 = 组合键直接进**(不长按):Fn+Backspace 本身就是对角双键,娃几乎不可能同按,长按只是给家长添堵。
- 诊断保留:按住 **Fn + 任意键** 顶部蓝条显示真实键码,排查键位用(正常玩不触发)。

## 技术选型(决策记录)

| 决策 | 选择 | 原因 / 放弃了什么 |
|---|---|---|
| 平台 | **Arduino (C++)** | 拍板直接走 B。UiFlow2 对 Adv 的 ES8311+SD 音频链无已验证案例(2025-10 新机);Arduino 路线有现成跑通项目佐证 |
| 音频播放 | **WAV 整文件载入 PSRAM + M5Unified `Speaker.playWav()`** | 放弃 ESP8266Audio MP3 流式方案:它要求锁 1.9.7(2.x 与 ESP-IDF5 不兼容)+ 自写 AudioOutput 类。Adv 有 8MB PSRAM,4MB 缓冲足够 30s+ WAV(24kHz mono ≈ 1.5MB);playWav 是 M5Unified 原生 API,打断(`stop()`)和双通道混音都是白送的 |
| 加载方式 | **分块异步加载(64KB/loop)** | 同步读 1.5MB 约 1s 会卡键盘;分块读期间键盘/屏幕动画照常响应 |
| 段结构 | **目录内 .wav 文件名排序连播**(v0.2,替代 v0.1 的写死 4 段) | 确认内容包是核心概念后,固件不该知道"name/phonics/words/jingle"这些语义——加段/减段/换包全靠文件,零代码改动 |
| 包选择 | **`/packs/active.txt` 写包名** | 放弃按键切包:娃会乱按误切;家长插电脑改一行文字足够。后续真要按键切再说(YAGNI) |

### 已核实的关键事实(2026-06-12)

- Cardputer-Adv:Stamp-S3A(**ESP32-S3FN8,8MB Flash,无 PSRAM** —— 官方文档 docs.m5stack.com/en/core/Cardputer-Adv 核实),ES8311 codec + NS4150B + 1W 喇叭,56 键,1.14" 240×135 ST7789V2,microSD
- ⚠️ **踩坑(2026-06-12 上机)**:最初误以为有 8MB PSRAM(没核实),固件按"4MB PSRAM 整块载入 WAV"做,上机串口报 `octal_psram/quad_psram: chip is not connected`。真相:此机无 PSRAM。**改为从 SD 流式播放**(3×8KB 内部 RAM 缓冲轮替 + `Speaker.playRaw` 入队,队列深度 2),PSRAM=disabled。教训:硬件内存规格务必先查官方文档
- SD SPI 引脚:SCK=40 / MISO=39 / MOSI=14 / CS=12(来源:跑通的 mp3-player-winamp-cardputer-adv 项目)
- 库版本:M5Cardputer ≥ 1.1.1(Adv 支持),自带 M5Unified ≥ 0.2.10 / M5GFX ≥ 0.2.17
- `Speaker.playWav(const uint8_t* wav, size_t len, uint32_t repeat=1, int channel=-1, bool stop_current=false)`(M5Unified 源码核实)
- 键盘 API:`Keyboard.isChange()` / `isPressed()` / `keysState()` → `.word`(字符列表)/`.enter`/`.del`(官方文档核实)
- **键盘映射真相(2026-06-13 查 M5Cardputer 库源码核实,解决"方向键不工作")**:ADV 用 **TCA8418** 矩阵控制器(原版 Cardputer 是直连 GPIO),库按 `M5.getBoard()==board_M5CardputerADV` 自动选 reader,但**两板共用同一张 `_key_value_map`**。关键:**`Fn` 不重映射任何键,只是把 `keysState().fn` 置 true**——`Fn+;` 得到的是 `fn=true` + `word=[';']`,base 字符照常进缓冲。所以原"Fn+方向键 seek"理论上该触发,之前不工作八成是 Fn 在 ADV 上检测/键位差异。**对策:方向用普通 `,`(◀)`/`(▶)键、不要 Fn**(普通 ASCII 键必进 word,零依赖);设置入口用 `fn`+`del` 两个**布尔 flag**(库直接给,不经 `_key_value_map` 的位置假设,板无关最稳)。`M5Cardputer.update()` 每帧刷 keysState,组合键可逐帧轮询
- 方向键丝印:`;`▲ `.`▼ `,`◀ `/`▶(home/底排),本固件只用左右两枚 `,` `/`
- **编译已验证(2026-06-12,arduino-cli)**:FQBN `m5stack:esp32:m5stack_cardputer:PSRAM=opi`,板包 m5stack:esp32 **3.3.7**(M5Stack 官方 index),库 M5Cardputer 1.1.1 / M5Unified 0.2.17 / M5GFX 0.2.22。Sketch 622KB(47% of 1.2MB APP 分区),编译零警告级错误
- M5Stack 板包(含 3.3.7)**没有 Cardputer-Adv 专属板定义**,用原版 `m5stack_cardputer` 板定义编译;差异:原版 StampS3 无 PSRAM,板定义 PSRAM 默认 Disabled,**必须手动加 `PSRAM=opi`**(Adv 是 8MB OPI PSRAM),Flash 参数(QIO 80MHz / 4MB 默认分区)两者通用
- 编译期坑(已修):`#include <SD.h>` 必须在 `<M5Cardputer.h>` **之前**——M5GFX 的 `drawJpgFile(SDFS&, ...)` 所需 `DataWrapperT<fs::SDFS>` 特化由 `#if defined(_SD_H_)` 守卫,include 顺序反了会报 "abstract class" 编译错

## 固件状态机

```
IDLE ──字母键──> 扫描字母目录.wav列表 → LOADING(分块读 SD→PSRAM) ──读完──> PLAYING ──播完──> GAP(350ms) ──> 下一段 LOADING
  ^                                                                                  └─全部段播完──> IDLE(屏幕留在该字母继续闪)
  └────────────── 任意时刻新字母键:Speaker.stop() → 重置到新字母 ──────────────────────────┘
非字母键:任意状态下 ding.wav 走通道 1,与通道 0 混音,互不打断
```

## SD 卡布局

见 `assets-spec.md`(权威版)。要点:`/packs/<包名>/<字母>/NN-xxx.wav` + `img1..3.jpg`,`/packs/active.txt` 选包。

## 屏幕

- 播放/待机闪动:大写+小写字母(FreeSansBold,随机亮色,黑底)↔ img1..3 轮换,700ms 节奏
- **结尾合影(2×2 格)逐格入镜踩拍(2026-06-13)**:字母先立即出,后续 3 格**跟鼓点开**——`drawFinaleProgress` 复用 `pumpAudio` 的 `beatHit`(检测到重音就开下一格,`FINALE_MIN_MS`=200 防一串快重音连开),无重音则 `FINALE_STEP_MS`=700 兜底。cue=9 触发时音频还在放(有鼓点跟拍),歌曲结束触发时音频已停(退回定时)。`finaleAnchor` 已不参与计时(改用 `finaleLastReveal` 节流),仅保留赋值
- 开机待机:彩虹 "A B C" + 提示

## 省电 / 熄屏(2026-06-13)

- **背光是绝对耗电大头**(ESP32 待机几十 mA,背光更甚)。三级:播放/操作中 = `BRIGHT_ON`(90)→ 歌曲播完合影 8s → 纯待机 = `BRIGHT_DIM`(25)彩虹屏 → **`SCREEN_OFF_MS`(60s)到 = 背光熄(setBright(0)),停止重绘**。任意键 `handleKeys` 顶部 `setBright(BRIGHT_ON)` + 刷新 `lastInteract` 秒唤醒。
- **熄屏倒计时锚定「歌曲播完那一刻」**(`beginSegment` 收尾把 `lastInteract = idleSince`),所以 60s 从歌曲结束起算、含那 8s 合影(songEnd+60s 熄)。`lastInteract` 还在「按键」「开机」刷新,任意键重置 60s。**不锚 keypress**——否则长歌期间会从上次按键算、可能刚播完就黑。
- **熄屏时长在设置里调**(2026-06-13):进设置按 `0` 轮换 30s/1min/2min/5min/never,存 **NVS**(`Preferences "abc"/scroff`),掉电不丢,默认 60s。`screenOffMs==0` = 永不熄。运行时变量 `screenOffMs`,开机从 NVS 载入。
- **没做 CPU 深睡/light sleep**:深睡要从键盘矩阵(TCA8418 INT)唤醒,没上机不敢碰(怕醒不来);熄背光已拿下绝大部分功耗。真要更省,后续上机再验:① `setCpuFrequencyMhz(80)` 熄屏时降频(唤醒/出声前恢复 240,防 I2S 抖)② TCA8418 INT 接 GPIO 做 `esp_light_sleep` 唤醒。

## 风险与遗留

1. **拼读音 TTS 质量**:TTS 念纯音素(/æ/、/b/)效果未知,近似拼写表见 assets-spec;现在拼读和字母名合成一个文件,单文件内试错即可
2. TTS 供应商/音色未定(候选 MiniMax 美音童声,需账号选音色激活)— 下一步
3. 配图未生成(Seedream/ARK,先样张后批跑)— 下一步
4. v1.1 候选:单词段 + Suno jingle 段(歌词已备好 26 首)、整首字母表主题曲、英音包/拼音包
