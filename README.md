# Cardputer ABC — a pocket ABC toy for toddlers

> A press-any-key, always-makes-a-sound English-alphabet learning toy for a 3-year-old, running on the **M5Stack Cardputer-ADV**. Every letter key plays a short phonics song, the screen writes the letter stroke-by-stroke and animates the word, and the number keys turn it into a tiny music player. All content lives in swappable **content packs** — change the voice, accent, or language without touching the firmware.

📖 **中文文档 → [README.zh-CN.md](README.zh-CN.md)**

![status](https://img.shields.io/badge/status-on--device%20verified-brightgreen) ![board](https://img.shields.io/badge/board-Cardputer--ADV-blue) ![license](https://img.shields.io/badge/license-Apache--2.0-green)

---

## Why I built it

It started as a music player. I loaded this little M5Cardputer with custom kids' songs I'd made for my toddler, and he was hooked — he carried it around for days telling everyone *"this is the little computer daddy gave me."* We have other small-screen gadgets he likes, but the **physical keyboard** is what made him certain *this* one is a real computer. So I asked myself: how do I make the most of that keyboard for a 3-year-old? Everything below is the answer. (He's now learning his letters from it — and mashing **E** far more than any other key, for reasons no one understands.)

## What it is

A single-file Arduino firmware for the **M5Stack Cardputer-ADV** that turns its 56-key keyboard into a toddler-proof learning toy. The design rule: **every key makes a sound** — a 3-year-old mashing keys must never hit silence.

Two modes share one device:

- **ABC mode (letter keys A–Z)** — press a letter → it plays that letter's short phonics song (letter name ×2 + phonics ×3 + words), while the screen:
  1. writes the letter **stroke by stroke** (uppercase then lowercase, Hershey vector font),
  2. then alternates the **big letter ↔ word pictures**, beat-synced to the song's loudness,
  3. plays a per-word **motion** animation, then an **effect** animation on the 3-beat action words (bite ×3, siren ×3, …),
  4. ends with an ~8s group photo → slow rainbow standby screen + a peeking **standby face**.
  - Any new letter key interrupts instantly (the core experience for a key-mashing toddler).
- **Music-player mode (number keys 0–9)** — each digit plays a folder `/audio/<n>/` of your own audio (`mp3 / wav / m4a / aac / flac / ogg / opus`). MP3 is decoded on-device with libhelix. Shows the embedded **cover art** (ID3 APIC, or a `cover.jpg` fallback) + **title** (ID3 tag or filename) + a switchable **visualizer** (big number / bars / pulse). `◀ ▶` skip tracks.

Non-letter keys give a quick **"ding"** on a separate audio channel — feedback without interrupting playback.

## Highlights & design notes

- **No PSRAM.** The Cardputer-ADV is a Stamp-S3A (**ESP32-S3FN8, 8 MB flash, _no_ PSRAM**). Audio is **streamed from SD**, not loaded whole into memory.
- **Content packs are first-class.** A "version" = a directory `/packs/<name>/`. Different voice / accent / language is just a different folder. Switch in the on-device settings menu or by editing one line in `/packs/active.txt` — the firmware needs no code changes to add or swap content.
- **Animations are pre-rendered flip frames.** The hard pixel work is done on a computer (Pillow); the device just flips through 240×135 JPGs.
- **USB drive mode (MSC).** Expose the SD card over USB to drag files without pulling the card (see below).
- **Power saving.** After a song ends the backlight dims, then turns off; any key wakes it. A standby face peeks out and gets drowsy before sleep.

## Hardware

| | |
|---|---|
| Board | **M5Stack Cardputer-ADV** (Stamp-S3A: ESP32-S3FN8, 8 MB flash, no PSRAM) |
| Audio | ES8311 codec + NS4150B amp + 1 W speaker |
| Display | 1.14" 240×135 ST7789V2 |
| Input | 56-key keyboard (TCA8418 matrix controller) |
| Storage | microSD (FAT32) |

> The original (non-ADV) Cardputer shares the same SoC and SD pinout, so it *may* run unmodified, but it's **untested** here. ADV is the verified target.

## Controls

| Key | Action |
|---|---|
| Letter A–Z | Play that letter (interruptible by any new letter) |
| Space / Enter | Pause / resume |
| `=` / `-` or `[` / `]` | Volume up / down |
| `,` (◀) / `/` (▶) | Previous / next letter — or previous / next track in player mode |
| Number `0`–`9` | Play music folder `/audio/<n>/` |
| **Fn + Backspace** (or **Fn + ?**) | Open settings menu (two opposite-corner keys — a toddler can't hit both one-handed) |
| Fn + V | Quick-cycle the player visualizer |
| Any other key | "ding" |

**Settings menu** (navigate with `;`▲ `.`▼ or Enter; change a value with `,`◀ `/`▶; Backspace exits): motion fx · image style (pixel / flat) · player visualizer · screen-off timeout (saved to NVS) · pick content pack · **USB drive mode**.

## Build & flash

Arduino IDE or `arduino-cli`, board package **`m5stack:esp32`**, library **M5Cardputer ≥ 1.1.1** (pulls in M5Unified / M5GFX).

```bash
# USBMode=default enables USB-drive (MSC) mode; UploadMode=cdc does a 1200bps-touch
# reset under TinyUSB so you don't have to press BOOT.
arduino-cli compile \
  --fqbn m5stack:esp32:m5stack_cardputer:USBMode=default,UploadMode=cdc \
  firmware/cardputer_abc

arduino-cli upload -p <PORT> \
  --fqbn m5stack:esp32:m5stack_cardputer:USBMode=default,UploadMode=cdc \
  firmware/cardputer_abc
```

The board has no PSRAM, so leave PSRAM disabled (the default for this board definition). No toolchain? Flash via **M5Burner** instead — see `docs/publish/m5burner.md` for the prebuilt-binary recipe.

## Getting content onto the SD card

The card is plain **FAT32**. Two ways to copy files onto it:

1. **Pull the microSD** and copy on your computer — the obvious way.
2. **USB drive mode (no card removal)** — plug the device into your computer over USB, open the **settings menu** (Fn + Backspace), select **USB drive**, and confirm. The device reboots and the SD card shows up as a **USB mass-storage drive** on your computer. Drag songs/packs over, then reboot the device to return to normal. (This relies on the firmware being flashed with `USBMode=default`, which the FQBN above already sets.)

Card layout:

```
SD root (FAT32)
├── packs/
│   ├── active.txt          # one line: the active pack's folder name
│   └── us-kid/             # a content pack
│       ├── A/ … Z/         # per-letter: 10-full.wav + flip frames + images
│       ├── ding.wav
│       └── pack.txt        # optional: human-readable name; line 2 "fps=N" overrides flip fps
└── audio/                  # music-player content (your own files)
    ├── 1/  twinkle.mp3
    └── 2/  count.mp3  cover.jpg
```

On first boot the firmware auto-creates an `/audio` scaffold if it's missing. The music player works with **any** audio you drop in. For ABC mode, download a ready-to-use content pack from the [Releases](../../releases) page and extract it to the SD root — or bring your own.

## Repo layout

```
firmware/cardputer_abc/    Single-file Arduino firmware (.ino) + bundled libhelix-mp3 + Hershey font
docs/                      Design + decision log, player-mode spec, display/eye-care notes
```

Key docs: `docs/design.md` (decision log), `docs/digit-audio-player.md` (player mode), `docs/display-flicker-and-eye-care.md` (filming the screen / eye-care).

> ABC content packs (songs + animation frames + art) are distributed separately as a downloadable resource on the [Releases](../../releases) page — they're **not** part of this firmware repo, which is kept firmware-only. The content-generation pipeline lives outside this repo.

## License

Apache-2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE). Apache-2.0 requires anyone redistributing a modified version to state which files they changed and to keep the attribution notices. The bundled `libhelix-mp3` is RealNetworks' code under the RPSL/RCSL (not covered by Apache-2.0) — see its headers. Downloadable content packs bundle OpenMoji art (CC BY-SA 4.0) and Suno-generated songs (Suno's terms) — see each pack's own CREDITS.
