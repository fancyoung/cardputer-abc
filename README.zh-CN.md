# Cardputer ABC — 给幼儿的掌上 ABC 早教机

> 跑在 **M5Stack Cardputer-ADV** 上的「按任意键必有声音」英语字母早教玩具:按字母键播一段拼读短歌,屏幕一笔笔写出字母并让单词动起来,数字键还能当小音乐播放器。所有内容都在可整体替换的**内容包**里——换配音、口音、语言都不用动固件。

📖 **English → [README.md](README.md)**

[![release](https://img.shields.io/github/v/release/fancyoung/cardputer-abc)](https://github.com/fancyoung/cardputer-abc/releases/latest)

> **⬇️ 上手:** 从 [**Releases**](https://github.com/fancyoung/cardputer-abc/releases/latest) 下固件 `.bin` + 现成 SD 包,或用 **M5Burner**(搜 *Cardputer ABC*)刷。SD 包要放进 FAT32 卡——只刷固件不够。

---

## 缘起

一开始它只是个播放器。我在这台小小的 M5Cardputer 上装了播放器,给娃放我之前做的儿歌,他高兴坏了,好几天拿着不撒手,逢人就说**「这是爸爸给我的小电脑」**。家里也有别的小屏设备他喜欢,但**实体键盘**让他笃定:这台才是「真电脑」。于是我想:怎么把这块键盘给一个 3 岁娃用到极致?下面这一切,就是答案。(现在他真的开始从里面认字母了——而且不知为何,按 **E** 的次数远超其它键。)

## 这是什么

跑在 **M5Stack Cardputer-ADV** 上的单文件 Arduino 固件,把它的 56 键键盘变成一台幼儿早教玩具。设计铁律:**每个键都出声**——3 岁娃狂拍键盘,永远不该拍到哑键。

一台机器两个模式:

- **ABC 模式(字母键 A–Z)**:按字母 → 播该字母的拼读短歌(字母名×2 + 拼读×3 + 单词),同时屏幕:
  1. **一笔一笔写出**这个字母(大写再小写,Hershey 矢量字体),
  2. 然后**大字母 ↔ 单词配图**交替闪动,跟着歌的响度踩拍,
  3. 每个词先一段 **motion** 动画,唱到结尾 3 连动作词时切 **effect** 特效(咬 3 口 / 警笛 3 声…),
  4. 结尾 ~8s 合影 → 慢速彩虹待机屏 + 会探头的**表情脸**。
  - 任何新字母键**立即打断**当前播放(3 岁狂按键场景的核心体验)。
- **播放器模式(数字键 0–9)**:每个数字播一个目录 `/audio/<n>/` 里你自己的音频(`mp3 / wav / m4a / aac / flac / ogg / opus`),mp3 用 libhelix 在机内直解。显示内嵌**封面**(ID3 APIC,或目录里 `cover.jpg` 兜底)+ **标题**(ID3 或文件名)+ 可切换的**可视化**(大数字 / 声波条 / 圆脉冲)。`◀ ▶` 切上下首。

非字母键 → 独立声道一声**「叮」**:有反馈但不打断播放。

## 亮点 & 设计取舍

- **没有 PSRAM**:Cardputer-ADV 是 Stamp-S3A(ESP32-S3FN8,8MB Flash,无 PSRAM),音频**从 SD 卡流式播放**,不整块载入内存。
- **内容包是一等公民**:一个「版本」= 一个目录 `/packs/<名字>/`,换配音 / 口音 / 语言就是换个文件夹;设置菜单切或改 `/packs/active.txt` 一行字,固件零改动。
- **动画 = 预渲染翻页帧**:难搞的像素活在电脑上用 Pillow 做,设备只翻 240×135 的 JPG。
- **U 盘模式(USB MSC)**:把 SD 卡当 USB 移动盘,免拔卡拖文件(见下文)。
- **省电 + 表情脸**:歌播完背光先暗再熄、任意键秒唤醒;待机表情脸间歇探头、临睡犯困。

## 硬件

| | |
|---|---|
| 板子 | **M5Stack Cardputer-ADV**(Stamp-S3A:ESP32-S3FN8,8MB Flash,无 PSRAM)|
| 音频 | ES8311 codec + NS4150B 功放 + 1W 喇叭 |
| 屏幕 | 1.14" 240×135 ST7789V2 |
| 输入 | 56 键键盘(TCA8418 矩阵控制器)|
| 存储 | microSD(FAT32)|

> 原版(非 ADV)Cardputer 同 SoC、同 SD 引脚,**理论上可能直接跑**,但这里**未测试**。ADV 是已验证的目标板。

## 按键

| 键 | 行为 |
|---|---|
| 字母 A–Z | 播该字母(可被任何新字母打断)|
| 空格 / 回车 | 暂停 / 继续 |
| `=` / `-` 或 `[` / `]` | 音量 +/- |
| `,`(◀)/ `/`(▶)| 上 / 下一个字母——播放器模式下=上 / 下一首 |
| 数字 `0`–`9` | 播放音乐目录 `/audio/<n>/` |
| **Fn + Backspace**(或 **Fn + ?**)| 进设置菜单(对角两键,娃单手乱拍极难同按)|
| Fn + V | 快切播放器可视化 |
| 其它任意键 | 「叮」|

**设置菜单**(`;`▲ `.`▼ 或回车移动光标;`,`◀ `/`▶ 改值;退格退出):动效开关 · 配图风格(像素 / 扁平)· 播放器可视化 · 熄屏时长(存 NVS)· 选内容包 · **U 盘模式**。

## 编译 & 刷机

Arduino IDE 或 `arduino-cli`,板包 **`m5stack:esp32`**,库 **M5Cardputer ≥ 1.1.1**(自动带 M5Unified / M5GFX)。

```bash
# USBMode=default 开启 U 盘(MSC)模式;UploadMode=cdc 在 TinyUSB 下用
# 1200bps-touch 复位,免按 BOOT 键。
arduino-cli compile \
  --fqbn m5stack:esp32:m5stack_cardputer:USBMode=default,UploadMode=cdc \
  firmware/cardputer_abc

arduino-cli upload -p <端口> \
  --fqbn m5stack:esp32:m5stack_cardputer:USBMode=default,UploadMode=cdc \
  firmware/cardputer_abc
```

机器无 PSRAM,**保持 PSRAM 关闭**(此板定义默认即关)。不想装工具链 → 用 **M5Burner** 刷,预编译 .bin 做法见 `docs/publish/m5burner.md`。

## 怎么把内容拷进 SD 卡

卡是普通 **FAT32**,两种方式拷文件:

1. **拔出 microSD** 在电脑上拷——最直接。
2. **U 盘模式(免拔卡)**:用 USB 线把设备插上电脑,进**设置菜单**(Fn + Backspace),选 **USB drive** 确认。设备重启后,SD 卡会在电脑上变成一个 **USB 移动盘**,直接把歌 / 包拖进去,拖完重启设备即回正常模式。(前提是固件用 `USBMode=default` 刷的,上面的 FQBN 已经设好。)

卡内结构:

```
SD 根目录(FAT32)
├── packs/
│   ├── active.txt          # 一行:当前包的文件夹名
│   └── us-kid/             # 一个内容包
│       ├── A/ … Z/         # 每字母:10-full.wav + 翻页帧 + 配图
│       ├── ding.wav
│       └── pack.txt        # 可选:人话名;第 2 行 "fps=N" 覆盖翻页帧率
└── audio/                  # 播放器内容(你自己的文件)
    ├── 1/  twinkle.mp3
    └── 2/  count.mp3  cover.jpg
```

首次开机若无 `/audio` 会自动建脚手架。播放器对**任何**丢进去的音频都能放。ABC 模式的内容包从 [Releases](../../releases) 下载现成的,解压到 SD 根即可——或自带内容。

## 目录结构

```
firmware/cardputer_abc/    单文件 Arduino 固件(.ino)+ 内嵌 libhelix-mp3 + Hershey 字体
docs/                      设计 + 决策记录、播放器规格、屏幕频闪 / 护眼说明
```

关键文档:`docs/design.md`(决策记录)、`docs/digit-audio-player.md`(播放器模式)、`docs/display-flicker-and-eye-care.md`(拍屏频闪 / 护眼)。

> ABC 内容包(歌 + 动画帧 + 配图)作为可下载资源放在 [Releases](../../releases) 页,**不进**这个固件仓——本仓只放固件;内容生成管线放在仓库之外。

## 许可

Apache-2.0,见 [LICENSE](LICENSE) 和 [NOTICE](NOTICE)。Apache-2.0 要求二次分发改动版时**标明改了哪些文件**并保留署名声明。内嵌的 `libhelix-mp3` 是 RealNetworks 代码,遵循 RPSL/RCSL(不在 Apache-2.0 覆盖内)——见其文件头。可下载的内容包内含 OpenMoji 配图(CC BY-SA 4.0)和 Suno 生成的歌曲(Suno 条款)——见各包自带的 CREDITS。
