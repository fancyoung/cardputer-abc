/*
 * Cardputer-Adv ABC 早教游戏 v1.1.0
 *
 * 按字母键播放该字母音频;画面分两段 + 音频驱动:
 *   前 INTRO_MS(念字母名/发音): 大字母 平滑换色 + 弹跳 + 跟响度脉动
 *   之后(单词/唱段):           轮换 [字母, 图1..imgN],切换跟随音乐重音(响度峰值)
 * 控制: 字母键播放(可打断) / 空格 暂停继续 / =、- 音量(屏显音量条) / 其它键 "叮"
 * 结束后: 合影 ~8s → 慢速彩虹待机屏 + 调暗背光 → 歌曲播完满 60s(含合影)熄背光省电;任意键秒唤醒(按键也重置 60s)
 *
 * 板子: M5Stack Cardputer-Adv (ESP32-S3FN8, 8MB Flash, 无 PSRAM); PSRAM=Disabled
 * 编译/刷机 FQBN: m5stack:esp32:m5stack_cardputer:USBMode=default,UploadMode=cdc
 *   USBMode=default(TinyUSB)= U盘模式(MSC)必需;UploadMode=cdc = TinyUSB 下 1200bps-touch 复位,免按 BOOT 键
 * 音频: 从 SD 流式播放(3×8KB 内部 RAM 缓冲轮替 + Speaker.playRaw),边播边算 RMS 驱动画面
 */
#include <SPI.h>
#include <SD.h>
#include <M5Cardputer.h>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include <Preferences.h>
#include "hershey_az.h"
#include "src/libhelix-mp3/mp3dec.h"     // 数字音频:卡根 /audio/<n>/ 直接放 mp3,helix 解码
#include <USB.h>                         // U 盘模式:把 SD 暴露给电脑当移动盘(需 USBMode=default 编译)
#include <USBMSC.h>

#define SD_SPI_SCK_PIN  40
#define SD_SPI_MISO_PIN 39
#define SD_SPI_MOSI_PIN 14
#define SD_SPI_CS_PIN   12

static const uint32_t SEG_GAP_MS    = 350;
static const uint32_t INTRO_MS      = 6000;   // 写字 showcase 总时长
static const uint32_t SHOW_MS       = 700;    // 大写先整字出现
static const uint32_t WRITE_MS      = 2000;   // 每个字母一笔笔写出来的时长(大写、小写各一遍)
static const uint32_t ANIM_MS       = 40;     // 动画帧间隔(~25fps)
static const uint32_t IDLE_FRAME_MS = 1200;   // 待机屏换色
static const uint32_t SETTLE_MS     = 8000;   // 结束后合影停留时长
// 熄屏时长可在设置里调,存 NVS;0=永不熄。预设档位见 SCROFF_PRESETS
static const uint32_t SCROFF_PRESETS[] = {30000, 60000, 120000, 300000, 0};
static const int      SCROFF_N = sizeof(SCROFF_PRESETS) / sizeof(SCROFF_PRESETS[0]);
static const uint32_t FINALE_STEP_MS = 700;   // 合影逐格入镜:无重音时的兜底间隔(踩拍优先)
static const uint32_t FINALE_MIN_MS  = 200;   // 两格入镜最小间隔(防一串快重音连开)
static const uint32_t IMG_MIN_MS    = 650;    // 图片最小停留(防重音抖动)
static const uint32_t IMG_MAX_MS    = 1800;   // 没重音也兜底切
static const uint32_t FRAME_MS_DEFAULT = 110; // 翻页默认帧间隔(~9fps);包可在 pack.txt 第2行 fps=N 覆盖
static const int      MAX_FRAMES     = 60;    // 每段动画帧数自动探测上限(帧数随包而定,固件不写死)
static const size_t   CHUNK_SAMPLES = 4096;
static const int      NUM_BUFS      = 3;
static const int      MAX_IMAGES    = 3;
static const uint8_t  DEFAULT_VOLUME = 180;
static const uint8_t  BRIGHT_ON = 90, BRIGHT_DIM = 25;

static const uint16_t PALETTE[] = {TFT_RED,  TFT_ORANGE,  TFT_YELLOW, TFT_GREEN,
                                   TFT_CYAN, TFT_MAGENTA, TFT_PINK,   TFT_WHITE};
static const int PALETTE_N = sizeof(PALETTE) / sizeof(PALETTE[0]);

enum State : uint8_t { ST_IDLE, ST_PLAYING, ST_GAP, ST_PAUSED, ST_SETTINGS };

State    state       = ST_IDLE;
State    pausedFrom  = ST_PLAYING;
String   packRoot    = "/packs/default";
char     curLetter   = 0;
std::vector<String> segs;
size_t   segIdx      = 0;
String   segCover    = "";       // 当前音频封面图路径(<stem>.jpg),"" = 无
String   segTitle    = "";       // 当前音频标题(<stem>.txt 内容,无则文件名)
bool     segHasCover = false;
bool     playerChromeDrawn = false;   // 封面+标题已画(切歌才重画;viz 每帧只重画自己那块)

File     audioFile;
int16_t  bufs[NUM_BUFS][CHUNK_SAMPLES];
int      bufIdx      = 0;
uint32_t streamRate  = 24000;
size_t   dataRemaining = 0;

// —— mp3 流式解码(helix)——
static const int MP3_INBUF = 4096;       // mp3 原始字节滑窗
HMP3Decoder mp3dec   = nullptr;
uint8_t  mp3in[MP3_INBUF];
int      mp3avail    = 0;                // mp3in 里有效字节数
uint8_t* mp3ptr      = mp3in;            // 当前解码读指针(指向 mp3in 内)
bool     segIsMp3    = false;            // 当前 seg 是 mp3(走 helix)还是 wav(走 PCM)
bool     mp3Eof      = false;            // mp3 解码到尾
bool     fileEof     = false;            // 源文件读完
size_t   dataStart   = 0;        // data chunk 在文件里的起始字节(seek 用)
size_t   dataBytes   = 0;        // data chunk 总字节

uint8_t* dingBuf     = nullptr;
size_t   dingSize    = 0;
uint8_t* fxBuf       = nullptr;  // 功能提示音缓冲(复用)
static const size_t FXCAP = 96 * 1024;
uint32_t gapStart    = 0;
uint32_t lastFrame   = 0;
int      frameNo     = 0;
int      imgCount    = 0;
int      volume      = DEFAULT_VOLUME;

uint32_t letterStart = 0;        // 当前字母开始播放的时刻(跨段)
int      rotIdx      = 0;        // 0=字母, 1..imgCount=图片
uint32_t lastSwitch  = 0;
uint32_t idleSince   = 0;
int      finaleStep  = 0;        // 合影已入镜的项数(0=未开始)
bool     finaleMode  = false;    // 是否进入合影(cue=9 触发,或音频结束)
uint32_t finaleAnchor = 0;       // 合影开始时刻(入镜动画基准)
uint32_t finaleLastReveal = 0;   // 上一格入镜的时刻(踩拍/兜底节流用)
uint32_t volOverlayUntil = 0;
uint8_t  curBright   = BRIGHT_ON;
uint32_t lastInteract = 0;       // 最近一次按键/进待机的时刻(熄屏倒计时基准)
uint32_t screenOffMs = 60000;    // 当前熄屏时长(从 NVS 载入,设置里可调;0=永不熄)
Preferences prefs;
bool     animFx      = true;     // 图片/字母动效开关(设置里 M 切,存 NVS);关=静态不跳不脉动
char     imgStyle    = 'A';      // 画风(设置里 S 切,存 NVS):'A'=像素 / 'B'=flat;图片在 <字母>/<style>/ 下
uint8_t  vizMode     = 1;        // 数字播放可视化(设置里改 / Fn+V 快切,存 NVS):0=大数字 1=声波条 2=圆脉冲
bool     nightMode   = false;    // 夜间/护眼模式:屏全程黑只放音,Esc/Del 弹提示,Fn+Del 进设置改回(存 NVS)
uint32_t hintUntil   = 0;        // 夜间模式提示显示截止时刻(到点自动熄屏)
int      selRow      = 0;        // 设置菜单光标行(◀▶ 移动,Enter 改)

// 音频驱动:实时响度
float    levelSmooth = 0;        // 0..1 平滑响度(脉动用)
float    beatAvg     = 0;        // 长期均值(峰值检测基线)
bool     beatHit     = false;    // 检测到重音(被画面消费)

// cue 时间轴(与歌词对齐;align_cues.py 生成的 cues.txt)
static const int MAX_CUES = 48;
static const uint32_t CUE_LEAD = 150;   // 图比词提前出现的毫秒
static const uint32_t MIN_DWELL = 400;  // 图最短停留(防快速抖动;钉帧动作拍不受此限)
static const uint32_t MIN_EFF_HOLD = 450; // 钉帧动作帧最短保持:切到非动作帧前先停够(让"最后一口"看得见)
uint32_t cueT[MAX_CUES]; uint8_t cueImg[MAX_CUES]; uint8_t cueVar[MAX_CUES];
int8_t   cueFrm[MAX_CUES];        // 第4列=钉定的 effect 帧号(动作拍卡在 crunch 上);-1=非钉帧(motion 自动轮播)
int      cueN = 0;
int      curImg = -1;            // 当前显示:-1/0=字母, >0=图号
int      curVar = 0;             // 当前 variant:0=motion 1=effect
int      curFrm = -1;            // 当前钉帧号(-1=非钉帧)
uint32_t lastImgChange = 0;
// 翻页动画状态(预渲染帧 w<n><m|e><NN>.jpg 轮播;无帧则退回静态 img<n>.jpg)
// 帧数自动探测、速度按包配置 —— "动多少帧/多快/怎么动"全在 SD,固件只是哑播放器
int      flipWord = 0, flipVar = 0, flipFrame = 0, flipCount = 0;
uint32_t flipLast = 0;
uint32_t frameMs  = FRAME_MS_DEFAULT;  // 当前包的翻页速度(loadPackFps 从 pack.txt 读)

M5Canvas canvas(&M5Cardputer.Display);
bool     haveCanvas  = false;
M5Canvas coverSprite(&M5Cardputer.Display);   // 78x78 封面(切歌解码一次,每帧 blit)
bool     haveCover   = false;
USBMSC   msc;                                  // U 盘模式:SD 暴露给电脑
bool     usbMscActive = false;

// ---- 待机表情脸(原生自绘:画在已有的 canvas 上,无额外大内存;no-PSRAM 板子友好)----
// 设计:ABC 引导屏为主画面,表情脸"间歇探头"几秒再缩回;临熄屏前露脸犯困;空闲键也能召出。
static const uint32_t FACE_FRAME_MS  = 33;     // 脸动画 ~30fps
static const uint32_t FACE_SHOW_MS   = 5000;   // 每次探头显示时长
static const uint32_t FACE_POP_GAP   = 22000;  // 两次探头间隔基准(再加随机)
static const uint32_t FACE_DROWSY_MS = 12000;  // 临熄屏前多久开始露脸犯困
static const uint16_t FACE_FG = 0x4FFF;        // 脸色(青);想切单色感/换配色,只改这两行
static const uint16_t FACE_BG = 0x0000;
enum FaceExpr : uint8_t { FX_NEUTRAL, FX_HAPPY, FX_SLEEPY, FX_SURPRISE, FX_WINK };
FaceExpr faceExpr      = FX_NEUTRAL;
uint32_t faceExprUntil = 0;     // 临时表情到期(0=常驻)
uint32_t faceShowUntil = 0;     // 探头显示到期(0=当前显示 ABC)
uint32_t faceNextPop   = 0;     // 下次探头时刻
uint32_t faceBlinkAt   = 0;     // 正在眨眼:开始时刻(0=没眨)
uint32_t faceNextBlink = 0;
float    faceGX = 0, faceGY = 0, faceGTX = 0, faceGTY = 0;  // 注视:当前/目标(-1..1)

// 设置菜单(家长用):Fn+Backspace 组合键进入(两键键盘对角,娃单手乱拍极难同按),数字键选内容包
static const int      MAX_PACKS_SHOWN  = 7;      // 一屏最多列几个包(数字键 1..N 选)
std::vector<String> packDirs;                    // /packs/ 下的子目录(包 slug)
std::vector<String> packLabels;                  // 对应显示名(读 pack.txt,无则用 slug)

// 字母走包目录;数字音频走卡根 /audio/<数字>/(直接丢 mp3,免 build_pack)
String letterDir() {
  if (curLetter >= '0' && curLetter <= '9') return "/audio/" + String(curLetter);
  return packRoot + "/" + curLetter;
}
// 图片按画风分子目录:<字母>/<A|B>/。音频(10-full.wav)/cues.txt 仍在 letterDir() 下,两风共享
String styleDir() { return letterDir() + "/" + imgStyle; }
String baseName(const String& n) { int s = n.lastIndexOf('/'); return s >= 0 ? n.substring(s + 1) : n; }

void setBright(uint8_t b) { if (b != curBright) { curBright = b; M5Cardputer.Display.setBrightness(b); } }

uint16_t hsv565(float h) {  // h:0..360
  float x = 1 - fabsf(fmodf(h / 60.0f, 2) - 1), r, g, b;
  if (h < 60)      { r = 1; g = x; b = 0; }
  else if (h < 120){ r = x; g = 1; b = 0; }
  else if (h < 180){ r = 0; g = 1; b = x; }
  else if (h < 240){ r = 0; g = x; b = 1; }
  else if (h < 300){ r = x; g = 0; b = 1; }
  else             { r = 1; g = 0; b = x; }
  return M5Cardputer.Display.color565(r * 255, g * 255, b * 255);
}

void fatal(const char* msg) {
  auto& d = M5Cardputer.Display;
  d.fillScreen(TFT_BLACK); d.setFont(&fonts::FreeSansBold9pt7b); d.setTextSize(1);
  d.setTextDatum(middle_center); d.setTextColor(TFT_RED, TFT_BLACK);
  d.drawString(msg, d.width() / 2, d.height() / 2);
  while (true) delay(100);
}

uint8_t* loadWholeFile(const String& path, size_t* outSize) {
  File f = SD.open(path);
  if (!f || f.isDirectory()) return nullptr;
  size_t sz = f.size();
  uint8_t* buf = (uint8_t*)malloc(sz);
  if (buf) f.read(buf, sz);
  f.close(); *outSize = sz; return buf;
}

bool openWavStream(const String& path) {
  audioFile = SD.open(path);
  if (!audioFile || audioFile.isDirectory()) { if (audioFile) audioFile.close(); return false; }
  uint8_t hdr[12];
  if (audioFile.read(hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
    audioFile.close(); return false;
  }
  streamRate = 24000;
  uint8_t ch[8];
  while (audioFile.read(ch, 8) == 8) {
    uint32_t sz = ch[4] | (ch[5] << 8) | (ch[6] << 16) | ((uint32_t)ch[7] << 24);
    if (!memcmp(ch, "fmt ", 4)) {
      uint8_t fmt[16]; uint32_t want = sz < 16 ? sz : 16;
      audioFile.read(fmt, want);
      streamRate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
      audioFile.seek(audioFile.position() + (sz - want) + (sz & 1));
    } else if (!memcmp(ch, "data", 4)) {
      dataStart = audioFile.position(); dataBytes = sz; dataRemaining = sz; return true;
    } else {
      audioFile.seek(audioFile.position() + sz + (sz & 1));
    }
  }
  audioFile.close(); return false;
}

// 打开 mp3:跳过开头 ID3v2 标签(让 helix 快速对齐第一帧),复位解码状态
bool openMp3Stream(const String& path) {
  audioFile = SD.open(path);
  if (!audioFile || audioFile.isDirectory()) { if (audioFile) audioFile.close(); return false; }
  uint8_t h[10];
  if (audioFile.read(h, 10) == 10 && !memcmp(h, "ID3", 3)) {
    uint32_t sz = ((uint32_t)(h[6] & 0x7f) << 21) | ((h[7] & 0x7f) << 14) | ((h[8] & 0x7f) << 7) | (h[9] & 0x7f);
    audioFile.seek(10 + sz);                          // 跳过整个 ID3v2 区
  } else {
    audioFile.seek(0);
  }
  mp3avail = 0; mp3ptr = mp3in; mp3Eof = false; fileEof = false;
  return true;
}

void resolvePack() {
  if (SD.exists("/packs/active.txt")) {
    File f = SD.open("/packs/active.txt"); String name = f.readString(); f.close(); name.trim();
    if (name.length() && SD.exists("/packs/" + name)) { packRoot = "/packs/" + name; return; }
  }
  File d = SD.open("/packs");
  if (d && d.isDirectory()) {
    for (File e = d.openNextFile(); e; e = d.openNextFile())
      if (e.isDirectory()) { packRoot = "/packs/" + baseName(e.name()); d.close(); return; }
    d.close();
  }
  fatal("no pack in /packs/");
}

// 读当前包 pack.txt 第2行可选 "fps=N"(或纯数字)→ 设翻页速度;无则用默认。包自定义,固件不写死
void loadPackFps() {
  frameMs = FRAME_MS_DEFAULT;
  File f = SD.open(packRoot + "/pack.txt");
  if (!f || f.isDirectory()) { if (f) f.close(); return; }
  f.readStringUntil('\n');                          // 第1行=显示名,跳过
  String l2 = f.readStringUntil('\n'); f.close(); l2.trim();
  int eq = l2.indexOf('=');
  if (eq >= 0) l2 = l2.substring(eq + 1);
  int fps = l2.toInt();
  if (fps >= 1 && fps <= 60) frameMs = 1000 / fps;
}

// ---------- 画面 ----------
uint16_t letterColor() { return PALETTE[(curLetter - 'A' + PALETTE_N) % PALETTE_N]; }

// 画单个 Hershey 字形到 g(不清屏/不 push)。progress=写到哪,centerX 横向中心,scale 统一缩放
void drawGlyphInto(lgfx::LovyanGFX* g, const HGlyph& gl, float progress, int centerX, float scale, int bounce, uint16_t color) {
  if (progress <= 0 || gl.total == 0) return;
  int xmin = 127, xmax = -128;
  for (int i = 0; i < gl.total; i++) { xmin = std::min(xmin, (int)gl.pts[i].x); xmax = std::max(xmax, (int)gl.pts[i].x); }
  float cx = (xmin + xmax) / 2.0f;
  int H = g->height();
  float topm = (H - (HERSHEY_YMAX - HERSHEY_YMIN) * scale) / 2.0f + bounce + H / 16.0f;  // +H/16:静止点下移,跳动不冲顶
  auto SX = [&](float x) { return (int)((x - cx) * scale + centerX); };
  auto SY = [&](float y) { return (int)((y - HERSHEY_YMIN) * scale + topm); };
  const int LW = 5;
  float reveal = progress * gl.total;
  int ptBase = 0;
  for (int p = 0; p < gl.npoly; p++) {
    int L = gl.polylen[p];
    for (int j = 0; j < L - 1; j++) {
      int gi = ptBase + j;
      const HPoint& a = gl.pts[gi]; const HPoint& b = gl.pts[gi + 1];
      if (reveal >= gi + 1) {
        g->drawWideLine(SX(a.x), SY(a.y), SX(b.x), SY(b.y), LW, color);
      } else if (reveal > gi) {
        float f = reveal - gi;
        g->drawWideLine(SX(a.x), SY(a.y), SX(a.x + (b.x - a.x) * f), SY(a.y + (b.y - a.y) * f), LW, color);
        return;                                      // 写到笔尖即止
      }
    }
    ptBase += L;
  }
}

// 同屏画一对大小写:pu/pl = 大写/小写各自写到哪;左大右小,共用基线
void drawLettersFrame(float pu, float pl, float scaleMul, int bounce, uint16_t color) {
  lgfx::LovyanGFX* g = haveCanvas ? (lgfx::LovyanGFX*)&canvas : (lgfx::LovyanGFX*)&M5Cardputer.Display;
  g->fillScreen(TFT_BLACK);
  if (curLetter >= 'A' && curLetter <= 'Z') {
    float scale = (110.0f / (HERSHEY_YMAX - HERSHEY_YMIN)) * scaleMul;
    int W = g->width();
    drawGlyphInto(g, HERSHEY_UP[curLetter - 'A'], pu, W * 0.30f, scale, bounce, color);
    drawGlyphInto(g, HERSHEY_LO[curLetter - 'A'], pl, W * 0.70f, scale, bounce, color);
  } else if (curLetter >= '0' && curLetter <= '9') {   // 数字:hershey 无字形,用内置字体画大数字
    int W = g->width(), H = g->height();
    g->setTextDatum(middle_center);
    g->setFont(&fonts::FreeSansBold24pt7b); g->setTextSize(2);
    g->setTextColor(color, TFT_BLACK);
    char b[2] = {curLetter, 0};
    g->drawString(b, W / 2, H / 2 + bounce + H / 16);
  }
  if (haveCanvas) canvas.pushSprite(0, 0);
}

// 字母动画帧:大写出现→写大写→写小写→两者保持(换色/弹跳/脉动)
void drawLetterAnim() {
  uint32_t t = millis(); uint32_t playMs = t - letterStart;
  uint16_t col = hsv565(fmodf(t * 0.06f, 360));
  int bounce = (int)(-fabsf(sinf(t * 0.006f)) * 9);   // 只向上 hop(静止点已下移,见 drawGlyphInto +H/16)
  float lv = levelSmooth; if (lv > 1) lv = 1;
  uint32_t e1 = SHOW_MS + WRITE_MS, e2 = SHOW_MS + 2 * WRITE_MS;
  float pu, pl, sMul = 1.0f; int bnc = 0;
  if (playMs < SHOW_MS)      { pu = 1; pl = 0; }                                   // 大写整字出现
  else if (playMs < e1)      { pu = (float)(playMs - SHOW_MS) / WRITE_MS; pl = 0; } // 写大写
  else if (playMs < e2)      { pu = 1; pl = (float)(playMs - e1) / WRITE_MS; }      // 写小写
  else                       { pu = 1; pl = 1; if (animFx) { sMul = 1.0f + lv * 0.2f; bnc = bounce; } } // 保持(动效开才跳/脉动)
  drawLettersFrame(pu, pl, sMul, bnc, col);
}

void drawLetterStatic(uint16_t col) {
  auto& d = M5Cardputer.Display;
  d.fillScreen(TFT_BLACK); d.setTextDatum(middle_center);
  d.setFont(&fonts::FreeSansBold24pt7b); d.setTextSize(2);
  d.setTextColor(col, TFT_BLACK);
  char buf[4] = {curLetter, ' ', (char)tolower(curLetter), 0};
  d.drawString(buf, d.width() / 2, d.height() / 2);
}

// ——— 数字音频「播放器界面」:左封面方块 + 顶标题 + 右侧 viz 区 ———
// 封面+标题画一次(切歌重画),viz 只重画自己那块,互不擦除。

// viz 取色:固定亮色调色板缓慢轮换(避开 hsv 纯蓝那种发暗的色,小孩看着亮)
uint16_t vizCol(uint32_t now) {
  static const uint16_t PAL[] = {0x07FF, 0xFFE0, 0x07E0, 0xFD20, 0xF81F, 0xFFFF};  // 青/黄/绿/橙/品红/白
  return PAL[(now / 1400) % 6];
}

// 全部画进 canvas(双缓冲,无闪烁)。区域已由 drawPlayer 的 fillScreen 清过,各 viz 不再单独填黑。
// viz: 声波条
void vizBars(lgfx::LovyanGFX* g, uint32_t now, int rx, int ry, int rw, int rh) {
  float lv = levelSmooth; if (lv > 1) lv = 1;
  uint16_t col = vizCol(now);
  const int N = 10, gap = 4;
  int bw = (rw - (N - 1) * gap) / N; if (bw < 3) bw = 3;
  int x0 = rx + (rw - (N * bw + (N - 1) * gap)) / 2;
  int baseY = ry + rh - 2;
  for (int i = 0; i < N; i++) {
    float ph = sinf(now * 0.012f + i * 0.7f) * 0.5f + 0.5f;
    int h = (int)(6 + lv * (rh - 12) * (0.35f + 0.65f * ph));
    g->fillRect(x0 + i * (bw + gap), baseY - h, bw, h, col);
  }
}

// viz: 圆脉冲(数字在中心)
void vizPulse(lgfx::LovyanGFX* g, uint32_t now, int rx, int ry, int rw, int rh) {
  float lv = levelSmooth; if (lv > 1) lv = 1;
  uint16_t col = vizCol(now);
  int cx = rx + rw / 2, cy = ry + rh / 2;
  int rmax = (rw < rh ? rw : rh) / 2 - 5;
  int r = (int)(rmax * 0.42f + lv * rmax * 0.58f);
  g->drawCircle(cx, cy, r + 5, col);
  g->fillCircle(cx, cy, r, col);
  g->setTextDatum(middle_center); g->setFont(&fonts::FreeSansBold18pt7b);
  g->setTextColor(TFT_BLACK, col);
  char b[2] = {curLetter, 0}; g->drawString(b, cx, cy);
}

// viz: 大数字(缓变色)
void vizNumber(lgfx::LovyanGFX* g, uint32_t now, int rx, int ry, int rw, int rh) {
  g->setTextDatum(middle_center); g->setFont(&fonts::FreeSansBold24pt7b);
  g->setTextColor(vizCol(now), TFT_BLACK);
  char b[2] = {curLetter, 0}; g->drawString(b, rx + rw / 2, ry + rh / 2);
}

// 标题(中文 efont,UTF-8 安全截断)+ 第几首,画进 g
void drawPlayerTitle(lgfx::LovyanGFX* g) {
  g->setTextDatum(top_left); g->setFont(&fonts::efontCN_16); g->setTextColor(TFT_CYAN, TFT_BLACK);
  String t = segTitle;
  while (t.length() > 0 && g->textWidth(t) > 232) {        // 整字符删(别从多字节中间切)
    int cut = t.length() - 1;
    while (cut > 0 && ((uint8_t)t[cut] & 0xC0) == 0x80) cut--;
    t.remove(cut);
  }
  g->drawString(t, 6, 4);
  if (segs.size() > 1) {
    g->setFont(&fonts::Font2); g->setTextDatum(top_right); g->setTextColor(TFT_DARKGREY, TFT_BLACK);
    g->drawString(String((int)segIdx + 1) + "/" + String((int)segs.size()), 236, 6);
  }
}

// 数字音频「播放器界面」一帧:标题 + viz,整屏画进 canvas 再 push(无闪烁)
void drawPlayer(uint32_t now) {
  lgfx::LovyanGFX* g = haveCanvas ? (lgfx::LovyanGFX*)&canvas : (lgfx::LovyanGFX*)&M5Cardputer.Display;
  g->fillScreen(TFT_BLACK);
  drawPlayerTitle(g);
  int rx, ry = 30, rw, rh = 100;
  if (segHasCover) {                                  // 左封面 78x78 + viz 占右侧
    g->drawRect(5, 27, 80, 80, TFT_DARKGREY);
    coverSprite.pushSprite(g, 6, 28);
    rx = 92; rw = 142;
  } else { rx = 6; rw = 228; }                        // 无封面:viz 占整块
  if (vizMode == 1)      vizBars(g, now, rx, ry, rw, rh);
  else if (vizMode == 2) vizPulse(g, now, rx, ry, rw, rh);
  else                   vizNumber(g, now, rx, ry, rw, rh);
  if (haveCanvas) canvas.pushSprite(0, 0);
}

// 数字音频暂停画面:标题 + 居中两根暂停竖条(不再是 "1 1")
void drawPlayerPaused() {
  lgfx::LovyanGFX* g = haveCanvas ? (lgfx::LovyanGFX*)&canvas : (lgfx::LovyanGFX*)&M5Cardputer.Display;
  g->fillScreen(TFT_BLACK);
  drawPlayerTitle(g);
  g->fillRect(104, 56, 13, 44, TFT_WHITE);            // ❚❚ 暂停符
  g->fillRect(123, 56, 13, 44, TFT_WHITE);
  if (haveCanvas) canvas.pushSprite(0, 0);
}

void drawImage(int n) {
  String p = styleDir() + "/w" + String(n) + "m00.jpg";   // 静态/兜底=该词 motion 第 0 帧(休止姿)
  M5Cardputer.Display.drawJpgFile(SD, p.c_str(), 0, 0);
}

// 翻页帧路径: <字母>/<style>/w<词号><m=motion|e=effect><两位帧号>.jpg
String framePath(int n, int var, int idx) {
  char b[8]; snprintf(b, sizeof(b), "w%d%c%02d", n, var ? 'e' : 'm', idx);
  return styleDir() + "/" + b + ".jpg";
}
// 数某词某 variant 实际有几帧(w<n><v>00,01,… 数到缺为止)—— 帧数随包,固件不写死
int countFrames(int n, int var) {
  int c = 0;
  while (c < MAX_FRAMES && SD.exists(framePath(n, var, c))) c++;
  return c;
}

// 切到某词某 variant 的翻页;没该 variant 帧退 motion,再没有就 flipCount=0(静态兜底)
void setFlip(int n, int var) {
  if (n == flipWord && var == flipVar) return;
  flipWord = n; flipVar = var; flipFrame = 0; flipLast = 0; flipCount = 0;
  if (n > 0) {
    flipCount = countFrames(n, var);
    if (flipCount == 0 && var) { flipVar = 0; flipCount = countFrames(n, 0); }  // 没特效帧退动作帧
  }
}

// 每帧推进:有帧按 frameMs(包可配)轮播,无帧画一次静态 jpg
void drawFlipTick(uint32_t now) {
  if (flipWord <= 0) return;
  if (!animFx) { if (flipFrame == 0) { drawImage(flipWord); flipFrame = 1; } return; }  // 关动效:只显静态图,不翻页
  if (flipCount == 0) {
    if (flipFrame == 0) { drawImage(flipWord); flipFrame = 1; }
    return;
  }
  if (now - flipLast < frameMs) return;
  flipLast = now;
  M5Cardputer.Display.drawJpgFile(SD, framePath(flipWord, flipVar, flipFrame).c_str(), 0, 0);
  flipFrame = (flipFrame + 1) % flipCount;
}

// 合影逐格入镜:踩拍优先(检测到重音 beatHit 就开下一格),无重音则按 FINALE_STEP_MS 兜底。
// 字母(idx0)立即出;音频还在放(cue=9 触发)时跟鼓点开图,音频已停(结束触发)时退回定时。
void drawFinaleProgress(uint32_t now) {
  int maxItems = 1 + imgCount;
  if (finaleStep == 0) {                                  // 起步:清屏 + 字母立即入镜
    M5Cardputer.Display.fillScreen(TFT_BLACK);
    drawFinaleItem(0); finaleStep = 1; finaleLastReveal = now; beatHit = false;
    return;
  }
  if (finaleStep >= maxItems) return;                     // 全部入镜完毕
  bool onBeat   = beatHit && (now - finaleLastReveal >= FINALE_MIN_MS);
  bool fallback = (now - finaleLastReveal >= FINALE_STEP_MS);
  if (onBeat || fallback) {
    beatHit = false;                                      // 消费重音(避免被别处再用)
    drawFinaleItem(finaleStep); finaleStep++; finaleLastReveal = now;
  }
}

// 结尾合影:2×2 格,idx0=字母(左上)idx1/2/3=图(右上/左下/右下),逐个入镜
void drawFinaleItem(int idx) {
  auto& d = M5Cardputer.Display;
  const int CW = 116, CH = 65;
  static const int CX[4] = {2, 122, 2, 122};
  static const int CY[4] = {2, 2, 68, 68};
  if (idx == 0) {
    d.fillRect(CX[0], CY[0], CW, CH, TFT_BLACK);
    d.setTextDatum(middle_center);
    d.setFont(&fonts::FreeSansBold24pt7b); d.setTextSize(1.1);
    d.setTextColor(letterColor(), TFT_BLACK);
    char b[4] = {curLetter, ' ', (char)tolower(curLetter), 0};
    d.drawString(b, CX[0] + CW / 2, CY[0] + CH / 2);
  } else {
    String p = styleDir() + "/w" + String(idx) + "m00.jpg";   // 合影格=该词 motion 第 0 帧缩到 0.48
    d.drawJpgFile(SD, p.c_str(), CX[idx], CY[idx], 0, 0, 0, 0, 0.48f, 0.48f);
  }
}

void faceSetExpr(FaceExpr e, uint32_t holdMs) {
  faceExpr = e;
  faceExprUntil = holdMs ? millis() + holdMs : 0;
}

// 一只眼:happy=顶部月牙(⌒);否则圆角竖块,按 openRatio 压扁(眨眼/犯困)
static void drawEye(lgfx::LovyanGFX* g, int cx, int cy, int w, int h, float openRatio, bool happy) {
  if (happy) {
    int r = w / 2;
    g->fillCircle(cx, cy, r, FACE_FG);
    g->fillCircle(cx, cy + (int)(r * 0.6f), (int)(r * 1.1f), FACE_BG);  // 挖掉下半 → 留顶月牙
    return;
  }
  int eh = (int)(h * openRatio); if (eh < 4) eh = 4;
  g->fillRoundRect(cx - w / 2, cy - eh / 2, w, eh, w / 2, FACE_FG);
}

// 按当前 face 状态画一帧(月牙技法避开 arc 角度朝向的不确定性)
void drawFace(uint32_t now) {
  lgfx::LovyanGFX* g = haveCanvas ? (lgfx::LovyanGFX*)&canvas : (lgfx::LovyanGFX*)&M5Cardputer.Display;
  int W = g->width(), H = g->height();
  g->fillScreen(FACE_BG);

  bool sleepy = (faceExpr == FX_SLEEPY);
  float open = 1.0f;
  if (sleepy) open = 0.32f;                          // 困:半闭
  else if (faceBlinkAt) {
    uint32_t bp = now - faceBlinkAt;
    if (bp >= 150) faceBlinkAt = 0;
    else open = fabsf((float)bp / 75.0f - 1.0f);     // 1→0→1
  }
  int bob = (int)(sinf(now / 1400.0f * 6.2832f) * (sleepy ? 1.0f : 1.6f));  // 呼吸:整脸轻微上下
  faceGX += (faceGTX - faceGX) * 0.18f;              // 注视缓动
  faceGY += (faceGTY - faceGY) * 0.18f;

  int eyeW = (int)(W * 0.16f), eyeH = (int)(H * 0.40f);
  int gx = (int)(faceGX * W * 0.05f), gy = (int)(faceGY * H * 0.06f);
  int eyeY = (int)(H * 0.44f) + bob + gy;
  int lx = (int)(W * 0.34f) + gx, rx = (int)(W * 0.66f) + gx;

  bool happy = (faceExpr == FX_HAPPY || faceExpr == FX_WINK);
  bool winkL = (faceExpr == FX_WINK);
  drawEye(g, lx, eyeY, eyeW, eyeH, winkL ? 0.12f : open, happy && !winkL);
  drawEye(g, rx, eyeY, eyeW, eyeH, open, happy);

  int mx = W / 2, my = (int)(H * 0.74f) + bob;       // 嘴
  if (faceExpr == FX_SURPRISE) {
    g->fillCircle(mx, my, (int)(H * 0.07f), FACE_FG);          // O 形张嘴
    g->fillCircle(mx, my, (int)(H * 0.04f), FACE_BG);
  } else if (happy) {
    int r = (int)(W * 0.09f);                                  // 笑:底部月牙(‿)
    g->fillCircle(mx, my, r, FACE_FG);
    g->fillCircle(mx, my - (int)(r * 0.6f), (int)(r * 1.1f), FACE_BG);
  } else if (sleepy) {
    g->fillRoundRect(mx - (int)(W * 0.05f), my, (int)(W * 0.10f), 3, 1, FACE_FG);  // 平嘴
  } else {
    g->fillRoundRect(mx - (int)(W * 0.04f), my, (int)(W * 0.08f), 4, 2, FACE_FG);  // 中性小嘴
  }

  if (haveCanvas) canvas.pushSprite(0, 0);
}

// 探头一次:重置表情 + 瞟一眼,显示 FACE_SHOW_MS 后由 updateDisplay 缩回 ABC
void startFacePop(uint32_t now, FaceExpr e) {
  faceExpr = e; faceExprUntil = 0;
  faceBlinkAt = 0; faceNextBlink = now + 1200;
  faceGX = faceGY = 0;
  faceGTX = (random(3) - 1) * 0.7f; faceGTY = (random(3) - 1) * 0.4f;  // 探头时瞟一眼
  faceShowUntil = now + FACE_SHOW_MS;
}

// 探头期间一拍:眨眼 + 表情到期,再画
void faceTick(uint32_t now) {
  if (faceExprUntil && now >= faceExprUntil) { faceExpr = FX_NEUTRAL; faceExprUntil = 0; }
  if (faceExpr == FX_NEUTRAL || faceExpr == FX_HAPPY) {
    if (!faceBlinkAt && now >= faceNextBlink) { faceBlinkAt = now; faceNextBlink = now + 1800 + random(2500); }
  }
  drawFace(now);
}

// ABC 引导屏(彩虹大字 + 提示)—— 待机主画面
void drawAbcIdle() {
  auto& d = M5Cardputer.Display;
  d.fillScreen(TFT_BLACK); d.setTextDatum(middle_center);
  d.setFont(&fonts::FreeSansBold24pt7b); d.setTextSize(1);
  const char* s[] = {"A", "B", "C"};
  for (int i = 0; i < 3; i++) {
    d.setTextColor(PALETTE[(frameNo + i) % PALETTE_N], TFT_BLACK);
    d.drawString(s[i], d.width() / 2 - 60 + i * 60, 50);
  }
  d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(TFT_DARKGREY, TFT_BLACK);
  d.drawString("press any key!", d.width() / 2, 110);
}

void drawIdle() {           // 一次性入口(开机/退设置/选包后):回到 ABC 引导屏
  faceShowUntil = 0; faceNextPop = 0;
  faceExpr = FX_NEUTRAL; faceExprUntil = 0; faceBlinkAt = 0;
  frameNo = 0; drawAbcIdle();
}

void drawPaused() {
  if (curLetter >= '0' && curLetter <= '9') { drawPlayerPaused(); return; }   // 数字:播放器暂停画面
  drawLetterStatic(letterColor());
  auto& d = M5Cardputer.Display;
  d.setFont(&fonts::FreeSansBold24pt7b); d.setTextSize(1);
  d.setTextDatum(top_right); d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("||", d.width() - 6, 2); d.setTextDatum(middle_center);
}

void drawVolume() {
  auto& d = M5Cardputer.Display;
  d.fillScreen(TFT_BLACK); d.setTextDatum(middle_center);
  d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("VOLUME", d.width() / 2, 28);
  const int n = 10, bw = 16, gap = 5, h = 34;
  int on = (volume - 30) * n / (255 - 30);
  int total = n * bw + (n - 1) * gap, x0 = (d.width() - total) / 2, y = 70;
  for (int i = 0; i < n; i++)
    d.fillRect(x0 + i * (bw + gap), y, bw, h, i < on ? TFT_GREEN : 0x2104);
}

void updateDisplay() {
  uint32_t now = millis();
  if (now < volOverlayUntil) return;
  if (state == ST_PAUSED || state == ST_SETTINGS) return;   // 菜单是静态画面,进/选时画一次
  if (nightMode) {                                          // 夜间模式:屏常黑只放音;Esc/Del 提示显示完自动熄
    if (hintUntil) { if (now < hintUntil) return; hintUntil = 0; }
    setBright(0); return;
  }

  if (state == ST_PLAYING || state == ST_GAP) {
    if (finaleMode) { drawFinaleProgress(now); return; }   // 已进合影,锁定到结束
    uint32_t playMs = now - letterStart;
    if (curLetter >= '0' && curLetter <= '9') {      // 数字=放音频:播放器界面(标题+viz,canvas 双缓冲无闪)
      if (now - lastFrame >= ANIM_MS) { lastFrame = now; drawPlayer(now); }
      return;
    }
    if (playMs < INTRO_MS || imgCount == 0) {        // 字母:第一段或无图 → 动画字母(教学:写字 + 脉动)
      if (now - lastFrame >= ANIM_MS) { lastFrame = now; drawLetterAnim(); }
      return;
    }
    // 第二段:优先按 cue 时间轴切图(与歌词对齐);无 cue 文件则退回重音驱动
    if (cueN > 0) {
      int want = -1, wantVar = 0, wantFrm = -1;       // -1 = 显示字母
      for (int i = 0; i < cueN; i++) {
        if (cueT[i] <= playMs + CUE_LEAD) { want = cueImg[i]; wantVar = cueVar[i]; wantFrm = cueFrm[i]; } else break;
      }
      if (want >= 9) {                                // 合影 cue:唱到结尾立即进合影
        finaleMode = true; finaleAnchor = now; finaleStep = 0;
        drawFinaleProgress(now); return;
      }
      bool changed = (want != curImg || wantVar != curVar || wantFrm != curFrm);
      bool pinned  = (wantFrm >= 0);                  // 钉帧动作拍:不受 MIN_DWELL 限制(要跟上 crunch crunch)
      // 正在显示动作帧、要切到非动作帧时,先保持 MIN_EFF_HOLD(最后一口停留);切到下一动作拍则不挡
      bool holdBlock = (curFrm >= 0 && !pinned && now - lastImgChange < MIN_EFF_HOLD);
      if (changed && !holdBlock && (pinned || now - lastImgChange >= MIN_DWELL)) {
        curImg = want; curVar = wantVar; curFrm = wantFrm; lastImgChange = now;
        if (want <= 0) { flipWord = 0; }
        else if (pinned) {                            // 直接画钉定的 effect 帧并保持(不轮播,不被静态图覆盖)
          flipWord = want; flipVar = 1; flipCount = 0; flipFrame = 0;
          M5Cardputer.Display.drawJpgFile(SD, framePath(want, 1, curFrm).c_str(), 0, 0);
        } else { setFlip(want, wantVar); }
      }
      if (curImg > 0 && curFrm < 0) drawFlipTick(now);  // 非钉帧才轮播(motion);钉帧已画好保持
      else if (curImg <= 0 && now - lastFrame >= ANIM_MS) { lastFrame = now; drawLetterAnim(); }
      return;
    }
    // 退路:重音驱动轮换 [字母, 图1..imgN](无 cue 文件;用 motion 翻页)
    bool due = (beatHit && now - lastSwitch >= IMG_MIN_MS) || (now - lastSwitch >= IMG_MAX_MS);
    if (due || lastSwitch == 0) {
      beatHit = false;
      rotIdx = (rotIdx + 1) % (imgCount + 1);
      lastSwitch = now;
      if (rotIdx > 0) setFlip(rotIdx, 0); else flipWord = 0;
    }
    if (rotIdx > 0) drawFlipTick(now);
    else if (now - lastFrame >= ANIM_MS) { lastFrame = now; drawLetterAnim(); }
    beatHit = false;
    return;
  }

  // ST_IDLE
  if (curLetter >= '0' && curLetter <= '9') {     // 数字播完:不进合影,回纯待机
    curLetter = 0; finaleMode = false; finaleStep = 0; frameNo = 0; lastFrame = 0;
    setBright(BRIGHT_DIM); return;
  }
  if (curLetter) {
    if (!finaleMode) { finaleMode = true; finaleAnchor = now; finaleStep = 0; }  // 没 cue 触发就在结束时起合影
    if (now - idleSince < SETTLE_MS) {            // 合影逐个入镜并保持
      drawFinaleProgress(now);
    } else {
      curLetter = 0; finaleMode = false; finaleStep = 0; frameNo = 0; lastFrame = 0;
      setBright(BRIGHT_DIM);                          // 进纯待机调暗;熄屏倒计时已从歌曲播完起算
    }
    return;
  }
  if (screenOffMs > 0 && now - lastInteract > screenOffMs) { setBright(0); return; }  // 久无操作:熄背光(任意键唤醒);0=永不熄

  // 临熄屏前:表情脸露脸犯困("睡眠前显示")
  bool drowsy = (screenOffMs > 0) &&
                (now - lastInteract > (screenOffMs > FACE_DROWSY_MS ? screenOffMs - FACE_DROWSY_MS : screenOffMs / 2));
  if (drowsy) {
    if (now - lastFrame >= FACE_FRAME_MS) { lastFrame = now; faceExpr = FX_SLEEPY; faceExprUntil = 0; drawFace(now); }
    return;
  }
  // 表情脸"间歇探头"几秒,其余时间显示 ABC 引导屏
  if (faceShowUntil) {
    if (now >= faceShowUntil) {                       // 探头结束 → 缩回 ABC,排下次
      faceShowUntil = 0; faceNextPop = now + FACE_POP_GAP + random(12000);
      lastFrame = 0; frameNo = 0; drawAbcIdle(); return;
    }
    if (now - lastFrame >= FACE_FRAME_MS) { lastFrame = now; faceTick(now); }
    return;
  }
  if (faceNextPop == 0) faceNextPop = now + FACE_POP_GAP;
  if (now >= faceNextPop) { startFacePop(now, FX_HAPPY); lastFrame = 0; return; }  // 时间到 → 探头
  if (now - lastFrame < IDLE_FRAME_MS) return;        // ABC 引导屏:彩虹换色
  lastFrame = now; frameNo++; drawAbcIdle();
}

// ---------- 音频 ----------
// mp3 内嵌封面(ID3v2 APIC)→ 解进 coverSprite(78x78 contain)。成功返回 true
bool loadEmbeddedCover(const String& path) {
  File f = SD.open(path);
  if (!f) return false;
  uint8_t h[10];
  bool ok = false;
  if (f.read(h, 10) == 10 && !memcmp(h, "ID3", 3)) {
    int ver = h[3];
    uint32_t tagSize = ((uint32_t)(h[6] & 0x7f) << 21) | ((h[7] & 0x7f) << 14) | ((h[8] & 0x7f) << 7) | (h[9] & 0x7f);
    uint32_t pos = 0;
    while (pos + 10 < tagSize) {
      uint8_t fh[10];
      if (f.read(fh, 10) != 10 || fh[0] == 0) break;   // 读完/进入 padding
      uint32_t fsz = (ver >= 4)
        ? (((uint32_t)(fh[4] & 0x7f) << 21) | ((fh[5] & 0x7f) << 14) | ((fh[6] & 0x7f) << 7) | (fh[7] & 0x7f))   // v2.4 synchsafe
        : (((uint32_t)fh[4] << 24) | (fh[5] << 16) | (fh[6] << 8) | fh[7]);                                       // v2.3
      pos += 10 + fsz;
      if (!memcmp(fh, "APIC", 4) && fsz > 0 && fsz < 140000) {   // 封一般 <140KB,过大跳过防 OOM
        uint8_t* buf = (uint8_t*)malloc(fsz);
        if (buf && (uint32_t)f.read(buf, fsz) == fsz) {
          uint8_t enc = buf[0];
          int i = 1;
          while (i < (int)fsz && buf[i] != 0) i++; i++;           // 跳 mime\0
          i++;                                                    // 图片类型 1 字节
          if (enc == 1 || enc == 2) { while (i + 1 < (int)fsz && !(buf[i] == 0 && buf[i + 1] == 0)) i += 2; i += 2; }  // UTF-16 描述 \0\0
          else { while (i < (int)fsz && buf[i] != 0) i++; i++; }   // 单字节描述 \0
          if (i < (int)fsz) {
            coverSprite.fillScreen(TFT_BLACK);
            ok = coverSprite.drawJpg(buf + i, fsz - i, 0, 0, 78, 78, 0, 0, 0.0f, 0.0f);   // zoom=0 → contain 78x78
          }
        }
        if (buf) free(buf);
        break;                                          // APIC 找到一张即停(不论解码成败)
      }
      f.seek(f.position() + fsz);
    }
  }
  f.close();
  return ok;
}

// 目录兜底封面:<dir>/cover.jpg|folder.jpg|cover.png|folder.png → coverSprite
bool loadFolderCover(const String& dir) {
  const char* names[] = {"/cover.jpg", "/folder.jpg", "/cover.png", "/folder.png"};
  for (auto nm : names) {
    String p = dir + nm;
    if (!SD.exists(p)) continue;
    coverSprite.fillScreen(TFT_BLACK);
    bool png = p.endsWith(".png");
    bool ok = png ? coverSprite.drawPngFile(SD, p.c_str(), 0, 0, 78, 78, 0, 0, 0.0f, 0.0f)
                  : coverSprite.drawJpgFile(SD, p.c_str(), 0, 0, 78, 78, 0, 0, 0.0f, 0.0f);
    if (ok) return true;
  }
  return false;
}

// 读当前音频的封面+标题(随 segIdx 变),供播放器界面用
void loadSegMeta() {
  String name = segs[segIdx];
  int dot = name.lastIndexOf('.');
  String stem = (dot > 0) ? name.substring(0, dot) : name;
  segTitle = stem;                                    // 标题=文件名(中文已能显示)
  segHasCover = false;
  if (haveCover) {                                    // 封面:mp3 内嵌优先,否则目录 cover.jpg/folder.jpg
    if (segIsMp3 && loadEmbeddedCover(letterDir() + "/" + name)) segHasCover = true;
    else if (loadFolderCover(letterDir())) segHasCover = true;
  }
  playerChromeDrawn = false;
}

void beginSegment() {
  while (segIdx < segs.size()) {
    String fn = segs[segIdx]; String lo = fn; lo.toLowerCase();
    bool isMp3 = lo.endsWith(".mp3");
    bool ok = isMp3 ? openMp3Stream(letterDir() + "/" + fn) : openWavStream(letterDir() + "/" + fn);
    if (ok) { segIsMp3 = isMp3; bufIdx = 0; state = ST_PLAYING; loadSegMeta(); return; }
    segIdx++;
  }
  state = ST_IDLE; idleSince = millis(); finaleStep = 0;
  lastInteract = idleSince;                            // 歌曲播完起算熄屏 60s(含 8s 合影)
}

void startLetter(char c) {
  M5Cardputer.Speaker.stop(0);
  if (audioFile) audioFile.close();
  curLetter = c; segIdx = 0; frameNo = 0; lastFrame = 0; dataRemaining = 0;
  finaleStep = 0; finaleMode = false; letterStart = millis(); rotIdx = 0; lastSwitch = 0;
  levelSmooth = 0; beatAvg = 0; beatHit = false;
  segs.clear();
  File d = SD.open(letterDir());
  if (d && d.isDirectory()) {
    for (File e = d.openNextFile(); e; e = d.openNextFile()) {
      if (e.isDirectory()) continue;
      String n = baseName(e.name()); String ln = n; ln.toLowerCase();
      if (n.startsWith(".")) continue;                // 跳过 macOS 隐藏文件/资源叉(._xxx)
      if (ln.endsWith(".wav") || ln.endsWith(".mp3")) segs.push_back(n);
    }
    d.close();
  }
  std::sort(segs.begin(), segs.end());
  if ((c >= '0' && c <= '9') && segs.empty()) {     // 空数字目录:给提示,不进播放/合影黑屏
    auto& d = M5Cardputer.Display;
    d.fillScreen(TFT_BLACK); d.setTextDatum(middle_center);
    d.setFont(&fonts::FreeSansBold24pt7b); d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    char b[2] = {c, 0}; d.drawString(b, 120, 50);
    d.setFont(&fonts::Font2); d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.drawString("no audio here", 120, 100);
    state = ST_IDLE; curLetter = 0; idleSince = millis(); lastInteract = idleSince;
    return;
  }
  imgCount = 0;   // 词数=有几个 w<n>m00(随包,与画风无关,但按当前 style 目录探测)
  for (int i = 1; i <= MAX_IMAGES; i++) {
    if (SD.exists(styleDir() + "/w" + String(i) + "m00.jpg")) imgCount = i; else break;
  }
  // 载入 cue 时间轴(每行 "time img [variant [frame]]";frame=钉定 effect 帧,卡在动作拍上)
  cueN = 0; curImg = -1; curVar = 0; curFrm = -1; lastImgChange = 0;
  flipWord = 0; flipFrame = 0; flipCount = 0;
  File cf = SD.open(letterDir() + "/cues.txt");
  if (cf) {
    while (cf.available() && cueN < MAX_CUES) {
      String line = cf.readStringUntil('\n'); line.trim();
      if (line.length() == 0) continue;
      int f[4] = {0, 0, 0, -1}, fi = 0, start = 0;   // time, img, var, frame(默认-1)
      for (int k = 0; k <= line.length() && fi < 4; k++) {
        if (k == line.length() || line[k] == ' ') {
          if (k > start) { f[fi++] = line.substring(start, k).toInt(); }
          start = k + 1;
        }
      }
      if (fi >= 2) {
        cueT[cueN] = f[0]; cueImg[cueN] = f[1]; cueVar[cueN] = f[2]; cueFrm[cueN] = (fi >= 4 ? f[3] : -1);
        cueN++;
      }
    }
    cf.close();
  }
  beginSegment();
}

// 把一块 PCM 的幅度并入 viz/重音状态
void feedLevel(int16_t* p, int samples) {
  uint32_t sum = 0; int cnt = 0;
  for (int i = 0; i < samples; i += 8) { sum += abs(p[i]); cnt++; }
  float amp = cnt ? (float)sum / cnt : 0;
  float lv = amp / 6000.0f;
  if (lv > levelSmooth) levelSmooth = lv; else levelSmooth = levelSmooth * 0.85f + lv * 0.15f;
  if (amp > beatAvg * 1.6f && amp > 1200) beatHit = true;
  beatAvg = beatAvg * 0.9f + amp * 0.1f;
}

void pumpMp3() {
  while (!mp3Eof && M5Cardputer.Speaker.isPlaying(0) < 2) {
    // 补料:把剩余字节挪到头部,从文件读满滑窗
    if (!fileEof && mp3avail < MP3_INBUF) {
      if (mp3avail && mp3ptr != mp3in) memmove(mp3in, mp3ptr, mp3avail);
      mp3ptr = mp3in;
      int rd = audioFile.read(mp3in + mp3avail, MP3_INBUF - mp3avail);
      if (rd <= 0) fileEof = true; else mp3avail += rd;
    }
    if (mp3avail <= 0) { mp3Eof = true; break; }
    int off = MP3FindSyncWord(mp3ptr, mp3avail);
    if (off < 0) {                                    // 这窗没找到帧头:丢弃(保留尾部几字节防截断)
      int keep = mp3avail > 8 ? 8 : 0;
      mp3ptr += (mp3avail - keep); mp3avail = keep;
      if (fileEof && mp3avail == 0) mp3Eof = true;
      if (fileEof) { mp3Eof = true; }
      continue;
    }
    mp3ptr += off; mp3avail -= off;
    int err = MP3Decode(mp3dec, &mp3ptr, &mp3avail, bufs[bufIdx], 0);
    if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) {
      if (fileEof) { mp3Eof = true; break; }          // 没更多数据了 → 结束
      break;                                          // 等下次补料(mp3ptr/avail 已保留)
    }
    if (err) { if (mp3avail > 0) { mp3ptr++; mp3avail--; } continue; }   // 坏帧:跳一字节重找
    MP3FrameInfo fi; MP3GetLastFrameInfo(mp3dec, &fi);
    int n = fi.outputSamps;                           // 交织后总 int16 数
    if (n <= 0) continue;
    int16_t* pcm = bufs[bufIdx];
    if (fi.nChans == 2) {                             // 立体声 → 折成单声道(喇叭是单声道)
      int m = n / 2; for (int i = 0; i < m; i++) pcm[i] = (int16_t)(((int)pcm[2 * i] + pcm[2 * i + 1]) / 2);
      n = m;
    }
    feedLevel(pcm, n);
    M5Cardputer.Speaker.playRaw(pcm, n, fi.samprate, false, 1, 0, false);
    bufIdx = (bufIdx + 1) % NUM_BUFS;
  }
}

void pumpAudio() {
  if (segIsMp3) { pumpMp3(); return; }
  while (dataRemaining > 0 && M5Cardputer.Speaker.isPlaying(0) < 2) {
    size_t want = sizeof(bufs[0]);
    if (want > dataRemaining) want = dataRemaining;
    size_t n = audioFile.read((uint8_t*)bufs[bufIdx], want);
    if (n == 0) { dataRemaining = 0; break; }
    dataRemaining -= n;
    feedLevel(bufs[bufIdx], n / 2);                  // 平均幅度 → 驱动画面/重音
    M5Cardputer.Speaker.playRaw(bufs[bufIdx], n / 2, streamRate, false, 1, 0, false);
    bufIdx = (bufIdx + 1) % NUM_BUFS;
  }
}

void togglePause() {
  if (state == ST_PLAYING || state == ST_GAP) {
    pausedFrom = state; M5Cardputer.Speaker.stop(0); state = ST_PAUSED; drawPaused();
  } else if (state == ST_PAUSED) {
    if (pausedFrom == ST_GAP) { gapStart = millis(); state = ST_GAP; } else state = ST_PLAYING;
    lastFrame = 0;
  }
}

void playDing() { if (dingBuf) M5Cardputer.Speaker.playWav(dingBuf, dingSize, 1, 1, true); }

// 功能提示音(中英),/packs/<包>/_fx/<name>.wav;没有就退回叮
void playFx(const char* name) {
  if (!fxBuf) { playDing(); return; }
  M5Cardputer.Speaker.stop(1);
  File f = SD.open(packRoot + "/_fx/" + name + ".wav");
  if (!f || f.isDirectory()) { if (f) f.close(); playDing(); return; }
  size_t sz = f.size(); if (sz > FXCAP) sz = FXCAP;
  f.read(fxBuf, sz); f.close();
  M5Cardputer.Speaker.playWav(fxBuf, sz, 1, 1, true);
}

void setVol(int v) {
  volume = constrain(v, 30, 255); M5Cardputer.Speaker.setVolume(volume);
  drawVolume(); volOverlayUntil = millis() + 1500;
}

// 左/右键:整体切到上一个/下一个字母(A↔Z 环绕)。无当前字母时,下=A 上=Z
void navLetter(int dir) {
  int base = curLetter ? (curLetter - 'A') : (dir > 0 ? -1 : 26);
  int idx = (base + dir + 26) % 26;
  startLetter('A' + idx);
}

// 数字音频:切到上/下一个(目录内多音频,循环)。默认顺序自动连播,这里是手动跳。
void navSeg(int dir) {
  int n = (int)segs.size();
  if (n < 2) return;
  M5Cardputer.Speaker.stop(0);
  if (audioFile) audioFile.close();
  segIdx = ((int)segIdx + dir + n) % n;
  frameNo = 0; lastFrame = 0; dataRemaining = 0; letterStart = millis(); rotIdx = 0; lastSwitch = 0;
  beginSegment();
}

// ---------- 设置菜单(家长用) ----------
// 包的显示名:读 /packs/<dir>/pack.txt 第一行;没有就用目录名
String packDisplayName(const String& dir) {
  File f = SD.open("/packs/" + dir + "/pack.txt");
  if (f && !f.isDirectory()) {
    String s = f.readStringUntil('\n'); f.close(); s.trim();
    if (s.length()) return s;
  }
  if (f) f.close();
  return dir;
}

// 扫描 /packs/ 下所有内容包目录(跳过 _/. 开头,如 _fx 之类)
void scanPacks() {
  packDirs.clear(); packLabels.clear();
  File d = SD.open("/packs");
  if (d && d.isDirectory()) {
    for (File e = d.openNextFile(); e; e = d.openNextFile()) {
      if (!e.isDirectory()) continue;
      String n = baseName(e.name());
      if (n.startsWith("_") || n.startsWith(".")) continue;
      packDirs.push_back(n);
    }
    d.close();
  }
  std::sort(packDirs.begin(), packDirs.end());
  for (auto& n : packDirs) packLabels.push_back(packDisplayName(n));
}

String curPackDir() { return packRoot.substring(String("/packs/").length()); }

// 熄屏档位显示名(30s / 1min / never)
String screenOffLabel(uint32_t ms) {
  if (ms == 0) return "never";
  if (ms % 60000 == 0) return String(ms / 60000) + "min";
  return String(ms / 1000) + "s";
}

// 按 0 在预设档位间轮换熄屏时长,存 NVS
void cycleScreenOff() {
  int idx = 0;
  for (int i = 0; i < SCROFF_N; i++) if (SCROFF_PRESETS[i] == screenOffMs) { idx = i; break; }
  idx = (idx + 1) % SCROFF_N;
  screenOffMs = SCROFF_PRESETS[idx];
  prefs.putUInt("scroff", screenOffMs);
  lastInteract = millis();                           // 调完重置倒计时,别立刻黑
}

void drawSettings() {       // 交互菜单:Enter 移到下一项,◀/▶ 改值,退格退出
  auto& d = M5Cardputer.Display;
  d.fillScreen(TFT_BLACK); d.setTextDatum(top_left);
  d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(TFT_CYAN, TFT_BLACK);
  d.drawString("SETTINGS", 6, 3);
  d.setFont(&fonts::Font2);
  String packName = packDirs.empty() ? "(none)" : packDisplayName(curPackDir());
  String vals[7] = {
    "motion fx : " + String(animFx ? "on" : "off"),
    "style     : " + String(imgStyle == 'A' ? "A pixel" : "B flat"),
    "digit viz : " + String(vizName()),
    "screen off: " + screenOffLabel(screenOffMs),
    "pick pack : " + packName,
    "USB drive : <,> to enter",                       // 选中按 ◀/▶ → 重启进 U 盘模式
    "night mode: " + String(nightMode ? "on" : "off"),// 屏全程黑只放音(护眼/睡前)
  };
  int y = 20;
  for (int i = 0; i < 7; i++) {
    bool sel = (i == selRow);
    d.setTextColor(sel ? TFT_GREEN : TFT_YELLOW, TFT_BLACK);
    d.drawString(String(sel ? "> " : "  ") + vals[i], 8, y); y += 15;
  }
  d.setTextColor(TFT_DARKGREY, TFT_BLACK);
  d.setFont(&fonts::Font0);                                   // 小字号 hint:避免底部被 135px 屏下边缘裁切
  d.drawString("up/dn:move  <,>:change  BkSp:exit", 6, 127);
}

// 改当前选中行的值(dir=+1/-1)。pack 行循环切包(原地)
void changeRow(int dir) {
  switch (selRow) {
    case 0: animFx = !animFx; prefs.putBool("animfx", animFx); break;
    case 1: imgStyle = (imgStyle == 'A') ? 'B' : 'A'; prefs.putUChar("style", (uint8_t)imgStyle); break;
    case 2: vizMode = (vizMode + 3 + dir) % 3; prefs.putUChar("viz", vizMode); break;
    case 3: cycleScreenOff(); break;
    case 4:
      if (!packDirs.empty()) {
        String cur = curPackDir(); int idx = 0;
        for (size_t i = 0; i < packDirs.size(); i++) if (packDirs[i] == cur) { idx = i; break; }
        applyPack(packDirs[(idx + dir + (int)packDirs.size()) % packDirs.size()]);
      }
      break;
    case 5:                                            // U 盘模式:存标志 → 重启进 MSC
      prefs.putBool("usbmsc", true); delay(120); ESP.restart();
      break;
    case 6:                                            // 夜间模式开关(屏全程黑只放音;退设置后生效)
      nightMode = !nightMode; prefs.putBool("night", nightMode);
      break;
  }
}

void handleSettingsKeys(Keyboard_Class::KeysState& st) {
  if (st.del) { exitSettings(); return; }            // 退格 = 退出
  if (st.enter) { selRow = (selRow + 1) % 7; drawSettings(); return; }   // Enter = 下一项
  for (auto c : st.word) {
    if (c == ';') { selRow = (selRow + 6) % 7; drawSettings(); return; } // ▲ 上一项
    if (c == '.') { selRow = (selRow + 1) % 7; drawSettings(); return; } // ▼ 下一项
    if (c == ' ') { selRow = (selRow + 1) % 7; drawSettings(); return; } // 空格 = 下一项(保留)
    if (c == ',') { changeRow(-1); drawSettings(); return; }             // ◀ = 改值(上一个)
    if (c == '/') { changeRow(+1); drawSettings(); return; }             // ▶ = 改值(下一个)
  }
}

void enterSettings() {
  M5Cardputer.Speaker.stop(0);
  if (audioFile) audioFile.close();
  curLetter = 0; finaleMode = false; finaleStep = 0;
  state = ST_SETTINGS; selRow = 0;
  setBright(BRIGHT_ON);
  scanPacks();
  drawSettings();
}

void exitSettings() {
  state = ST_IDLE; curLetter = 0; finaleMode = false; finaleStep = 0;
  frameNo = 0; lastFrame = 0;
  if (nightMode) setBright(0);                         // 夜间模式:退设置即熄屏
  else drawIdle();
}

// 切包:写 active.txt(先删再写)+ 重载包级资源。不显 OK/不退出(菜单里原地切)
void applyPack(const String& dir) {
  if (SD.exists("/packs/active.txt")) SD.remove("/packs/active.txt");
  File f = SD.open("/packs/active.txt", FILE_WRITE);
  if (f) { f.print(dir); f.close(); }
  packRoot = "/packs/" + dir;
  loadPackFps();
  if (dingBuf) { free(dingBuf); dingBuf = nullptr; }
  dingBuf = loadWholeFile(packRoot + "/ding.wav", &dingSize);
  playFx("pack");                                  // 确认音(无则退回叮)
}

const char* vizName() { static const char* V[] = {"number", "bars", "pulse"}; return V[vizMode <= 2 ? vizMode : 0]; }

// Fn+键改设置后的反馈:在帮助屏里就重画帮助屏,否则弹一条 1.2s 提示(不打断播放)
void flashMsg(const String& m) {
  auto& d = M5Cardputer.Display;
  d.fillScreen(TFT_BLACK); d.setTextDatum(middle_center);
  d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(TFT_CYAN, TFT_BLACK);
  d.drawString(m, 120, 67);
  volOverlayUntil = millis() + 1200;
}
void afterSetting(const String& msg) {
  if (state == ST_SETTINGS) drawSettings(); else flashMsg(msg);
}

// 夜间模式提示:点亮屏幕,告诉用户如何进设置改回;~2.5s 后由 updateDisplay 自动熄回
void drawNightHint() {
  auto& d = M5Cardputer.Display;
  setBright(BRIGHT_ON);
  d.fillScreen(TFT_BLACK); d.setTextDatum(middle_center);
  d.setFont(&fonts::FreeSansBold9pt7b);
  d.setTextColor(TFT_CYAN, TFT_BLACK);  d.drawString("Night mode", 120, 48);
  d.setTextColor(TFT_WHITE, TFT_BLACK); d.drawString("Fn + Del = settings", 120, 80);
  hintUntil = millis() + 2500;
}

void handleKeys() {
  if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;
  if (!nightMode) setBright(BRIGHT_ON);               // 夜间模式不点亮(按键只放音)
  lastInteract = millis();                            // 任意键唤醒并重置熄屏倒计时
  auto st = M5Cardputer.Keyboard.keysState();

  // Fn 组合:进设置菜单(全部设置在里面改);Fn+V 唯一快捷=数字播放时快切 viz(与菜单联动同一值)
  if (st.fn) {
    if (st.del) { if (state == ST_SETTINGS) exitSettings(); else { scanPacks(); enterSettings(); } return; }   // Fn+退格 = 开/关菜单
    for (auto c : st.word) {
      char lc = (c >= 'A' && c <= 'Z') ? c + 32 : c;
      if (lc == 'v') { vizMode = (vizMode + 1) % 3; prefs.putUChar("viz", vizMode); afterSetting("digit viz: " + String(vizName())); return; }  // Fn+V 快切 viz
      if (c == '/' || c == '?') { if (state != ST_SETTINGS) { scanPacks(); enterSettings(); } return; }   // Fn+? = 设置菜单
    }
    return;                                             // Fn + 其它键 = 忽略
  }
  if (state == ST_SETTINGS) { handleSettingsKeys(st); return; }   // 设置菜单:方向键导航
  if (nightMode) {                                    // 夜间模式:普通 Esc/Del → 弹提示(教用户 Fn+Del 进设置改回)
    bool esc = st.del;
    for (auto c : st.word) if (c == '`' || c == 0x1b) esc = true;
    if (esc) { drawNightHint(); return; }
  }

  if (st.enter) { togglePause(); playFx(state == ST_PAUSED ? "pause" : "play"); return; }
  for (auto c : st.word) {
    if (c == ' ') { togglePause(); playFx(state == ST_PAUSED ? "pause" : "play"); return; }
    if (c == ',') { if (curLetter >= '0' && curLetter <= '9') navSeg(-1); else navLetter(-1); return; }  // ◀ 数字时=上一个音频,否则上一个字母
    if (c == '/') { if (curLetter >= '0' && curLetter <= '9') navSeg(+1); else navLetter(+1); return; }  // ▶ 数字时=下一个音频,否则下一个字母
    if (isalpha((unsigned char)c)) { startLetter(toupper(c)); return; }
    if (isdigit((unsigned char)c)) { startLetter(c); return; }     // 数字键 = 第 c 项歌/音频(/packs/<包>/<数字>/)
    if (c == '=' || c == ']') { setVol(volume + 25); playFx("louder"); return; }   // = 或 ] 放大
    if (c == '-' || c == '[') { setVol(volume - 25); playFx("quieter"); return; }   // - 或 [ 减小
  }
  if (st.space) { togglePause(); playFx(state == ST_PAUSED ? "pause" : "play"); return; }
  // 没分配任务的键(数字/符号):待机时召出表情脸 + 给个表情,娃乱按也有反馈
  if (state == ST_IDLE && !curLetter) {
    static const FaceExpr fun[] = {FX_HAPPY, FX_SURPRISE, FX_WINK};
    startFacePop(millis(), fun[random(3)]);
    lastFrame = 0;
  }
  playDing();
}

// 把 SD /packs 结构打到串口(诊断"按键没反应":看 active.txt、包名、字母/数字目录与文件)
void dumpSDTree(Print& o) {
  o.println("\n==== SD DUMP BEGIN ====");
  File af = SD.open("/packs/active.txt");
  if (af) { String a = af.readStringUntil('\n'); a.trim(); o.println("active.txt=[" + a + "]"); af.close(); }
  else o.println("active.txt MISSING");
  o.println("packRoot=" + packRoot);
  File pr = SD.open("/packs");
  if (!pr || !pr.isDirectory()) { o.println("/packs MISSING!"); o.println("==== SD DUMP END ===="); return; }
  for (File p = pr.openNextFile(); p; p = pr.openNextFile()) {
    String pname = baseName(p.name());
    if (!p.isDirectory()) { o.println("(file) " + pname + "  " + String((unsigned)p.size())); continue; }
    o.println("PACK " + pname + "/");
    File pk = SD.open("/packs/" + pname);
    for (File s = pk.openNextFile(); s; s = pk.openNextFile()) {
      String sname = baseName(s.name());
      if (!s.isDirectory()) { o.println("  " + sname + "  " + String((unsigned)s.size())); continue; }
      File sd2 = SD.open("/packs/" + pname + "/" + sname);
      bool isDigit = (sname.length() == 1 && sname[0] >= '0' && sname[0] <= '9');
      int cnt = 0; String detail = "";
      for (File f = sd2.openNextFile(); f; f = sd2.openNextFile()) {
        cnt++;
        if (isDigit) detail += "      " + baseName(f.name()) + "  " + String((unsigned)f.size()) + "\n";
      }
      sd2.close();
      o.println("  " + sname + "/  (" + String(cnt) + (isDigit ? " files, DIGIT)" : " files)"));
      if (detail.length()) o.print(detail);
    }
    pk.close();
  }
  pr.close();
  // 数字音频新路径:卡根 /audio/<0-9>/
  o.println("-- /audio (digit mp3/wav) --");
  File au = SD.open("/audio");
  if (!au || !au.isDirectory()) o.println("  /audio MISSING");
  else {
    for (File g = au.openNextFile(); g; g = au.openNextFile()) {
      String gn = baseName(g.name());
      if (!g.isDirectory()) continue;
      File gd = SD.open("/audio/" + gn); int c = 0; String det = "";
      for (File f = gd.openNextFile(); f; f = gd.openNextFile()) { c++; det += "      " + baseName(f.name()) + "  " + String((unsigned)f.size()) + "\n"; }
      gd.close();
      o.println("  " + gn + "/  (" + String(c) + ")"); o.print(det);
    }
    au.close();
  }
  o.println("==== SD DUMP END ====");
}

// —— U 盘模式(USB MSC):把 SD 扇区暴露给电脑,当移动盘拖文件 ——
static int32_t mscRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  uint32_t s = SD.sectorSize(); if (!s) return -1;
  for (uint32_t x = 0; x < bufsize / s; x++)
    if (!SD.readRAW((uint8_t*)buffer + x * s, lba + x)) return -1;
  return bufsize;
}
static int32_t mscWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
  uint32_t s = SD.sectorSize(); if (!s) return -1;
  for (uint32_t x = 0; x < bufsize / s; x++)
    if (!SD.writeRAW(buffer + x * s, lba + x)) return -1;
  return bufsize;
}
static bool mscStartStop(uint8_t power_condition, bool start, bool load_eject) { return true; }

// 进 U 盘模式:停播放、把 SD 暴露成 USB 盘、画提示屏。退出 = 电脑弹出后按任意键重启
void enterUsbMsc() {
  M5Cardputer.Speaker.stop(0);
  if (audioFile) audioFile.close();
  usbMscActive = true;
  auto& d = M5Cardputer.Display;
  d.fillScreen(TFT_BLACK); d.setTextDatum(top_left);
  d.setFont(&fonts::efontCN_16);
  d.setTextColor(TFT_CYAN, TFT_BLACK);  d.drawString("U 盘模式", 8, 6);
  d.setTextColor(TFT_WHITE, TFT_BLACK); d.drawString("电脑里会多出一个盘,", 8, 34);
  d.drawString("把文件拖进去即可", 8, 56);
  d.setTextColor(TFT_YELLOW, TFT_BLACK);
  d.drawString("完成: 电脑先\"弹出\"盘,", 8, 88);
  d.drawString("再按任意键重启", 8, 110);
  msc.vendorID("Cardputr"); msc.productID("ABC-SD"); msc.productRevision("1.0");
  msc.onRead(mscRead); msc.onWrite(mscWrite); msc.onStartStop(mscStartStop);
  msc.mediaPresent(true);
  msc.begin(SD.numSectors(), SD.sectorSize());
  USB.begin();
}

void setup() {
  Serial.begin(115200);
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  prefs.begin("abc", false);                          // NVS 提前:开机即判夜间模式 / U 盘模式
  nightMode = prefs.getBool("night", false);
  if (nightMode) {
    setBright(0);                                      // 夜间模式:开机直接黑,不亮屏(护眼/睡前)
  } else {
    M5Cardputer.Display.setBrightness(BRIGHT_ON);
    auto& d = M5Cardputer.Display;                     // 开机立刻显 ABC 屏,免黑屏让娃以为没开机(SD 初始化期间也有得看)
    d.fillScreen(TFT_BLACK); d.setTextDatum(middle_center);
    d.setFont(&fonts::FreeSansBold24pt7b); d.setTextSize(1);
    const uint16_t sc[3] = {TFT_RED, TFT_GREEN, TFT_BLUE}; const char* ss = "ABC";
    for (int i = 0; i < 3; i++) { d.setTextColor(sc[i], TFT_BLACK); char b[2] = {ss[i], 0}; d.drawString(b, d.width() / 2 - 60 + i * 60, 50); }
    d.setFont(&fonts::FreeSansBold9pt7b); d.setTextColor(TFT_DARKGREY, TFT_BLACK);
    d.drawString("press any key!", d.width() / 2, 110);
  }
  M5Cardputer.Speaker.setVolume(volume);
  randomSeed(micros());                               // 表情脸随机:眨眼/动一动/表情键

  canvas.setColorDepth(16);
  haveCanvas = canvas.createSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height());
  coverSprite.setColorDepth(16);
  haveCover = coverSprite.createSprite(78, 78);

  SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
  if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) fatal("SD card?");

  if (!SD.exists("/audio")) {                          // 首启脚手架:空卡也给现成结构,免看文档
    SD.mkdir("/audio");
    for (char c = '0'; c <= '9'; c++) SD.mkdir("/audio/" + String(c));
    File r = SD.open("/audio/README.txt", FILE_WRITE);
    if (r) {
      r.print("数字键音频播放器\n\n"
              "把音频放进 0-9 各目录,按对应数字键播放;< / > 切上一首/下一首。\n"
              "支持 mp3 / wav。封面:音频内嵌图,或在目录里放 cover.jpg。\n"
              "标题用文件名,所以文件名起好就是标题。\n");
      r.close();
    }
  }

  if (prefs.getBool("usbmsc", false)) {                // 设置里选了 U 盘模式 → 重启进这里
    prefs.putBool("usbmsc", false);                    // 一次性:清标志,退出重启即回正常
    enterUsbMsc();
    return;                                            // 不再初始化 ABC,停在 U 盘模式
  }

  mp3dec = MP3InitDecoder();                           // 数字音频 mp3 解码器(失败则数字目录只能放 wav)
  resolvePack();
  // dumpSDTree(Serial);                              // 诊断用:遍历整卡打串口(上千帧很慢)— 正常启动不跑,需要排查时再开
  loadPackFps();
  dingBuf = loadWholeFile(packRoot + "/ding.wav", &dingSize);
  fxBuf = (uint8_t*)malloc(FXCAP);                    // 功能提示音复用缓冲
  screenOffMs = prefs.getUInt("scroff", 60000);
  animFx = prefs.getBool("animfx", true);
  imgStyle = (char)prefs.getUChar("style", (uint8_t)'A');   // 画风:默认 A 像素
  if (imgStyle != 'A' && imgStyle != 'B') imgStyle = 'A';
  vizMode = prefs.getUChar("viz", 1); if (vizMode > 2) vizMode = 1;   // 数字可视化:默认声波条(不与标题/数字键重影)
  if (!nightMode) drawIdle();                          // 夜间模式开机保持黑屏
  lastInteract = millis();                            // 开机待机也起熄屏倒计时
}

void loop() {
  M5Cardputer.update();
  if (usbMscActive) {                                  // U 盘模式:任意键 → 重启回正常
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) { delay(80); ESP.restart(); }
    delay(10); return;
  }
  handleKeys();
  switch (state) {
    case ST_PLAYING:
      pumpAudio();
      if ((segIsMp3 ? mp3Eof : dataRemaining == 0) && !M5Cardputer.Speaker.isPlaying(0)) {
        if (audioFile) audioFile.close(); gapStart = millis(); state = ST_GAP;
      }
      break;
    case ST_GAP:
      if (millis() - gapStart >= SEG_GAP_MS) { segIdx++; beginSegment(); }
      break;
    case ST_IDLE:
    case ST_PAUSED:
    case ST_SETTINGS: break;
  }
  updateDisplay();
  delay(1);
}
