/* gemms: a GoldenEye music player in the style of classic XMMS.
 *
 * It does not play files.  It runs the gemusic engine live -- the ROM's own libaudio
 * on interp_mips, its audio microcode on the RSP interpreter -- about 19x faster
 * than real time, so looping tunes really loop and nothing but the ROM is needed.
 *
 * All artwork is drawn procedurally on a 275-pixel-wide canvas in the XMMS main
 * window layout; no skin bitmaps are used or shipped. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <atomic>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <algorithm>
#ifndef __EMSCRIPTEN__
#include <boost/program_options.hpp>
#endif
/* we drive the event loop from main() ourselves, so we do not want SDL's main
 * shim (which on macOS would redefine main and pull in SDL2main) */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <chrono>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#ifdef HAVE_GEMMS_BUILD_ID
#include "gemms_build_id.h"
#else
#define GEMMS_BUILD_ID "unstamped"
#endif

#include "engine.hh"
#include "ge_addrs.hh"
#include "pixfont.hh"

#ifndef __EMSCRIPTEN__
namespace po = boost::program_options;
#endif

/* ------------------------------------------------------------------ playlist */

struct track_t {
  int seq;
  std::string title;
  double seconds;        /* one pass */
  bool loops;
};

/* Readable level names for the 63 sequences, in the ROM's order. */
static const char *g_titles[63] = {
  nullptr, "Death (Short)", "GoldenEye Theme (Intro)", "Train", "Depot", "Multiplayer Theme 1",
  "Citadel", "Facility", "Control", "Dam", "Frigate", "Archives", "Silo", "Multiplayer Theme 2",
  "Streets", "Bunker 1", "Bunker 2", "Statue", "Elevator (Control)", "Cradle", nullptr,
  "Elevator (Caverns)", "Egyptian", "Folders (Menu)", "Watch (Pause)", "Aztec", "Caverns",
  "Death (Solo)", "Surface 2", "Train X", nullptr, "Facility X", "Depot X", "Control X",
  "Caverns X", "Dam X", "Frigate X", "Archives X", "Silo X", nullptr, "Streets X", "Bunker 2 X",
  "Bunker 1 X", "Jungle X", "Intro Swoosh", "Statue X", "Aztec X", "Egyptian X", "Cradle X",
  "Cuba", "Runway", "Runway Plane", "Multiplayer Theme 3", "Wind", "Guitar Gliss", "Jungle",
  "Runway X", "Surface 1", "Multiplayer Death", "Surface 2 X", "Surface 2 End", "Statue Part", "End"
};

/* ------------------------------------------------------------------ audio path */

static const int RING_FRAMES = 16384;             /* stereo frames, ~0.74 s */
static const int VIS_LEN = 4096;

enum class play_state_t { stopped, playing, paused };

struct player_t {
  /* engine thread -> audio callback */
  int16_t ring[RING_FRAMES * 2];
  std::atomic<uint32_t> ring_w{0}, ring_r{0};
  /* audio callback -> UI */
  float vis[VIS_LEN];
  std::atomic<uint32_t> vis_w{0};
  std::atomic<uint64_t> played{0};                 /* stereo frames since track start */
  std::atomic<int> volume{80};                     /* 0..100 */
  std::atomic<int> balance{0};                     /* -100..100 */
  std::atomic<bool> paused{false};
  /* UI -> engine thread */
  std::mutex cmd_lock;
  int cmd_seq = -1;                                /* start this sequence */
  double cmd_seek = -1.0;
  bool cmd_stop = false;
  std::atomic<bool> quit{false};
  /* engine thread -> UI */
  std::atomic<bool> track_done{false};
  std::atomic<bool> seeking{false};
  /* policy */
  std::atomic<bool> repeat{false};
  double pass_seconds = 0.0;
  bool track_loops = false;
  int loop_passes = 2;
  double fade_seconds = 8.0;
};

static uint32_t ring_used(const player_t &p) {
  return p.ring_w.load() - p.ring_r.load();
}

static void audio_cb(void *ud, Uint8 *stream, int len) {
  player_t &p = *static_cast<player_t*>(ud);
  int16_t *out = reinterpret_cast<int16_t*>(stream);
  int n = len / 4;
  int vol = p.volume.load(), bal = p.balance.load();
  int gl = vol * ((bal > 0) ? (100 - bal) : 100), gr = vol * ((bal < 0) ? (100 + bal) : 100);   /* /10000 */
  uint32_t r = p.ring_r.load(), w = p.ring_w.load(), vw = p.vis_w.load();
  int got = 0;
  for(int i = 0; i < n; i++) {
    int32_t l = 0, rr = 0;
    if(not(p.paused.load()) and r != w) {
      l = p.ring[2*(r % RING_FRAMES)];
      rr = p.ring[2*(r % RING_FRAMES) + 1];
      r++;
      got++;
    }
    p.vis[vw % VIS_LEN] = static_cast<float>(l + rr) * (0.5f / 32768.0f) * (vol / 100.0f);
    vw++;
    out[2*i] = static_cast<int16_t>((l * gl) / 10000);
    out[2*i+1] = static_cast<int16_t>((rr * gr) / 10000);
  }
  p.ring_r.store(r);
  p.vis_w.store(vw);
  p.played.fetch_add(static_cast<uint64_t>(got));
}

/* The engine as a resumable pump.  A native build spins it on its own thread; the
 * wasm build calls step() from the browser's main loop, because pthreads there mean
 * SharedArrayBuffer and cross-origin isolation headers for no real gain -- the
 * interpreter renders many times faster than real time either way. */
struct pump_t {
  player_t &p;
  engine_t &eng;
  bool active = false, sounded = false;
  uint64_t rendered = 0;                           /* stereo frames since track start */
  int n_silent = 0, cur_seq = -1;
  int16_t frame[2 * GE_FRAME_SAMPLES];
  const int silence_limit = 2 * GE_OUTPUT_RATE / GE_FRAME_SAMPLES;

  pump_t(player_t &p, engine_t &eng) : p(p), eng(eng) {}

  /* one iteration: returns false when there is nothing to do right now */
  bool step() {
    int seq = -1;
    double seek = -1.0;
    bool stop = false;
    {
      std::lock_guard<std::mutex> lk(p.cmd_lock);
      seq = p.cmd_seq; seek = p.cmd_seek; stop = p.cmd_stop;
      p.cmd_seq = -1; p.cmd_seek = -1.0; p.cmd_stop = false;
    }
    if(stop) {
      active = false;
    }
    if(seq >= 0 or (seek >= 0.0 and cur_seq >= 0)) {
      uint64_t target = (seek >= 0.0) ? static_cast<uint64_t>(seek * GE_OUTPUT_RATE) : 0;
      if(seq >= 0 or target < rendered) {           /* backwards means start over */
	cur_seq = (seq >= 0) ? seq : cur_seq;
	eng.start(cur_seq);
	rendered = 0;
	sounded = false;
	n_silent = 0;
      }
      p.seeking.store(target > rendered);
      auto seek_t0 = std::chrono::steady_clock::now();
      while(rendered + GE_FRAME_SAMPLES <= target and not(p.quit.load())) {   /* fast-forward */
	eng.render(frame);
	rendered += GE_FRAME_SAMPLES;
      }
      if(p.seeking.load()) {
	fprintf(stderr, "gemms: seek to %.1f s took %.2f s\n", seek,
		std::chrono::duration<double>(std::chrono::steady_clock::now() - seek_t0).count());
      }
      p.seeking.store(false);
      p.ring_r.store(p.ring_w.load());              /* drop queued audio */
      p.played.store(rendered);
      p.track_done.store(false);
      active = true;
    }
    if(not(active) or ring_used(p) + GE_FRAME_SAMPLES > RING_FRAMES) {
      return false;
    }
    eng.render(frame);
    /* when does this tune end?  jingles: two seconds of digital silence.  loopers:
     * after loop_passes passes, faded -- unless REPEAT is lit, then never. */
    bool silent = true;
    for(int i = 0; i < 2*GE_FRAME_SAMPLES; i++) {
      silent = silent and (frame[i] == 0);
    }
    sounded = sounded or not(silent);
    n_silent = silent ? (n_silent + 1) : 0;
    bool done = sounded and (n_silent >= silence_limit);
    if(p.track_loops and not(p.repeat.load())) {
      double t = static_cast<double>(rendered) / GE_OUTPUT_RATE;
      double t_end = p.loop_passes * p.pass_seconds + p.fade_seconds;
      double t_fade = t_end - p.fade_seconds;
      if(t > t_fade) {
	for(int i = 0; i < GE_FRAME_SAMPLES; i++) {
	  double ti = t + static_cast<double>(i) / GE_OUTPUT_RATE;
	  double gain = std::max(0.0, (t_end - ti) / p.fade_seconds);
	  frame[2*i] = static_cast<int16_t>(frame[2*i] * gain);
	  frame[2*i+1] = static_cast<int16_t>(frame[2*i+1] * gain);
	}
      }
      done = done or (t >= t_end);
    }
    uint32_t w = p.ring_w.load();
    for(int i = 0; i < GE_FRAME_SAMPLES; i++) {
      p.ring[2*((w + i) % RING_FRAMES)] = frame[2*i];
      p.ring[2*((w + i) % RING_FRAMES) + 1] = frame[2*i+1];
    }
    p.ring_w.store(w + GE_FRAME_SAMPLES);
    rendered += GE_FRAME_SAMPLES;
    if(done) {
      active = false;
      p.track_done.store(true);
    }
    return true;
  }
};

#ifndef __EMSCRIPTEN__
static void engine_thread(player_t *pp, engine_t *eng) {
  pump_t pump(*pp, *eng);
  while(not(pp->quit.load())) {
    if(not(pump.step())) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
}
#endif

/* ------------------------------------------------------------------ drawing */

static const int W = 275, MAIN_H = 116, PL_H = 174;
static const int PL_ROW_H = 9, PL_TOP = MAIN_H + 17, PL_ROWS = 15;

struct canvas_t {
  std::vector<uint32_t> px;
  int h;
  canvas_t(int h) : px(static_cast<size_t>(W) * h, 0xff000000u), h(h) {}
  void set(int x, int y, uint32_t c) {
    if(x >= 0 and x < W and y >= 0 and y < h) {
      px[static_cast<size_t>(y) * W + x] = c;
    }
  }
  void fill(int x, int y, int w, int hh, uint32_t c) {
    for(int j = y; j < y + hh; j++) {
      for(int i = x; i < x + w; i++) {
	set(i, j, c);
      }
    }
  }
  void vgrad(int x, int y, int w, int hh, uint32_t top, uint32_t bot) {
    for(int j = 0; j < hh; j++) {
      uint32_t c = 0xff000000u;
      for(int k = 0; k < 3; k++) {
	int a = (top >> (8*k)) & 0xff, b = (bot >> (8*k)) & 0xff;
	c |= static_cast<uint32_t>(a + (b - a) * j / std::max(1, hh - 1)) << (8*k);
      }
      fill(x, y + j, w, 1, c);
    }
  }
  /* a 1-pixel bevel: raised buttons are light on top/left, insets the reverse */
  void bevel(int x, int y, int w, int hh, uint32_t tl, uint32_t br) {
    fill(x, y, w, 1, tl);
    fill(x, y, 1, hh, tl);
    fill(x, y + hh - 1, w, 1, br);
    fill(x + w - 1, y, 1, hh, br);
  }
  int text57(int x, int y, const std::string &s, uint32_t c, int clip_x0 = 0, int clip_x1 = W) {
    for(char ch : s) {
      ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
      for(const glyph57_t &g : g_font57) {
	if(g.c == ch) {
	  for(int j = 0; j < 7; j++) {
	    for(int i = 0; i < 5; i++) {
	      if(g.rows[j][i] == '#' and (x + i) >= clip_x0 and (x + i) < clip_x1) {
		set(x + i, y + j, c);
	      }
	    }
	  }
	  break;
	}
      }
      x += 6;
    }
    return x;
  }
  int text35(int x, int y, const std::string &s, uint32_t c) {
    for(char ch : s) {
      ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
      for(const glyph35_t &g : g_font35) {
	if(g.c == ch) {
	  for(int j = 0; j < 5; j++) {
	    for(int i = 0; i < 3; i++) {
	      if(g.rows[j][i] == '#') {
		set(x + i, y + j, c);
	      }
	    }
	  }
	  break;
	}
      }
      x += 4;
    }
    return x;
  }
  /* 9x13 seven-segment digit; unlit segments stay faintly visible like a real LCD */
  void lcd_digit(int x, int y, int d, uint32_t on, uint32_t off) {
    static const uint8_t segs[10] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
    uint8_t m = (d >= 0 and d <= 9) ? segs[d] : 0;
    auto seg = [&](int bit, int sx, int sy, int sw, int sh) { fill(x + sx, y + sy, sw, sh, ((m >> bit) & 1) ? on : off); };
    seg(0, 2, 0, 5, 2);      /* a: top          */
    seg(1, 7, 1, 2, 5);      /* b: upper right  */
    seg(2, 7, 7, 2, 5);      /* c: lower right  */
    seg(3, 2, 11, 5, 2);     /* d: bottom       */
    seg(4, 0, 7, 2, 5);      /* e: lower left   */
    seg(5, 0, 1, 2, 5);      /* f: upper left   */
    seg(6, 2, 5, 5, 2);      /* g: middle       */
  }
};

/* palette: dark slate metal with a green LCD */
static const uint32_t C_BODY_T = 0xff3a4152u, C_BODY_B = 0xff1c202bu, C_HI = 0xff6f7a92u, C_LO = 0xff0b0d12u;
static const uint32_t C_BTN_T = 0xff59627au, C_BTN_B = 0xff2a3040u, C_GLYPH = 0xffd8dde8u;
static const uint32_t C_LCD = 0xff000000u, C_GREEN = 0xff00e800u, C_GREEN_DIM = 0xff001800u, C_TITLE = 0xffb4c0dcu;
static const uint32_t C_PL_TEXT = 0xff00d000u, C_PL_CUR = 0xffffffffu, C_PL_SEL = 0xff0a246au, C_LED_ON = 0xff30ff30u, C_LED_OFF = 0xff123012u;

struct rect_t { int x, y, w, h; bool has(int px, int py) const { return px >= x and px < x + w and py >= y and py < y + h; } };

/* XMMS main-window geometry */
static const rect_t R_TITLEBAR = {0, 0, W, 14}, R_CLOSE = {264, 3, 9, 9}, R_MIN = {244, 3, 9, 9};
static const rect_t R_LCD = {9, 22, 93, 38}, R_VIS = {24, 43, 76, 16}, R_SONG = {110, 24, 155, 12};
static const rect_t R_VOL = {107, 57, 68, 13}, R_BAL = {177, 57, 38, 13}, R_EQ = {219, 58, 23, 12}, R_PL = {242, 58, 23, 12};
static const rect_t R_POS = {16, 72, 248, 10};
static const rect_t R_PREV = {16, 88, 23, 18}, R_PLAY = {39, 88, 23, 18}, R_PAUSE = {62, 88, 23, 18}, R_STOP = {85, 88, 23, 18};
static const rect_t R_NEXT = {108, 88, 22, 18}, R_EJECT = {136, 89, 22, 16}, R_SHUF = {164, 89, 47, 15}, R_REP = {211, 89, 28, 15};
static const rect_t R_PLIST = {6, PL_TOP, W - 22, PL_ROWS * PL_ROW_H}, R_PLSCROLL = {W - 14, PL_TOP, 8, PL_ROWS * PL_ROW_H};

struct ui_t {
  std::vector<track_t> tracks;
  int cur = 0, sel = 0, pl_scroll = 0;
  play_state_t state = play_state_t::stopped;
  bool shuffle = false, show_pl = true;
  int pressed = -1;                     /* which transport button is held */
  int drag = 0;                         /* 1 volume, 2 balance, 3 position */
  double drag_pos = 0.0;
  float bars[19] = {0}, peaks[19] = {0};
  double marquee = 0.0;
  double demo_time = -1.0;              /* >= 0: fake state for --screenshot */
};

static std::string mmss(double s) {
  int t = static_cast<int>(s);
  char buf[16];
  snprintf(buf, sizeof(buf), "%d:%02d", t / 60, t % 60);
  return buf;
}

static double track_total(const player_t &p, const track_t &t) {
  return (t.loops and not(p.repeat.load())) ? (p.loop_passes * t.seconds + p.fade_seconds) : t.seconds;
}

static void draw_button(canvas_t &c, const rect_t &r, bool down) {
  c.vgrad(r.x, r.y, r.w, r.h, down ? C_BTN_B : C_BTN_T, down ? C_BTN_T : C_BTN_B);
  c.bevel(r.x, r.y, r.w, r.h, down ? C_LO : C_HI, down ? C_HI : C_LO);
}

static void draw_tri(canvas_t &c, int x, int y, int hh, bool right, uint32_t col) {
  for(int j = 0; j < hh; j++) {
    int w = (j < hh/2 + 1) ? (j + 1) : (hh - j);
    for(int i = 0; i < w; i++) {
      c.set(right ? (x + i) : (x - i), y + j, col);
    }
  }
}

static void draw(canvas_t &c, ui_t &ui, player_t &p) {
  /* ---- body and title bar ---- */
  c.vgrad(0, 0, W, MAIN_H, C_BODY_T, C_BODY_B);
  c.bevel(0, 0, W, MAIN_H, C_HI, C_LO);
  c.vgrad(1, 1, W - 2, 13, 0xff20263au, 0xff10131cu);
  const std::string name = "GOLDENEYE MULTIMEDIA SYSTEM";
  int tw = static_cast<int>(name.size()) * 6, tx = (W - tw) / 2;
  for(int y = 4; y <= 10; y += 2) {                 /* pinstripes either side of the name */
    c.fill(6, y, tx - 12, 1, 0xff55607cu);
    c.fill(tx + tw + 5, y, R_MIN.x - (tx + tw + 5) - 4, 1, 0xff55607cu);
  }
  c.text57(tx, 4, name, C_TITLE);
  draw_button(c, R_MIN, false);
  c.fill(R_MIN.x + 2, R_MIN.y + 6, 5, 1, C_GLYPH);
  draw_button(c, {254, 3, 9, 9}, false);
  c.fill(256, 5, 5, 1, C_GLYPH);
  draw_button(c, R_CLOSE, false);
  for(int i = 0; i < 5; i++) {
    c.set(R_CLOSE.x + 2 + i, R_CLOSE.y + 2 + i, C_GLYPH);
    c.set(R_CLOSE.x + 6 - i, R_CLOSE.y + 2 + i, C_GLYPH);
  }

  double t_now = (ui.demo_time >= 0.0) ? ui.demo_time : static_cast<double>(p.played.load()) / GE_OUTPUT_RATE;
  const track_t *trk = ui.tracks.empty() ? nullptr : &ui.tracks[ui.cur];

  /* ---- left LCD: state icon, time, spectrum ---- */
  c.fill(R_LCD.x, R_LCD.y, R_LCD.w, R_LCD.h, C_LCD);
  c.bevel(R_LCD.x - 1, R_LCD.y - 1, R_LCD.w + 2, R_LCD.h + 2, C_LO, C_HI);
  if(ui.state == play_state_t::playing) {
    draw_tri(c, 26, 28, 9, true, C_GREEN);
  }
  else if(ui.state == play_state_t::paused) {
    c.fill(26, 28, 3, 9, C_GREEN);
    c.fill(31, 28, 3, 9, C_GREEN);
  }
  else {
    c.fill(26, 28, 8, 8, C_GREEN);
  }
  {
    int t = static_cast<int>(t_now), m = (t / 60) % 100, s = t % 60;
    bool lit = ui.state != play_state_t::stopped;
    bool blink = (ui.state == play_state_t::paused) and ((SDL_GetTicks() / 500) % 2 == 1);
    uint32_t on = (lit and not(blink)) ? C_GREEN : C_GREEN_DIM;
    c.lcd_digit(48, 26, lit ? m / 10 : -1, on, C_GREEN_DIM);
    c.lcd_digit(60, 26, lit ? m % 10 : -1, on, C_GREEN_DIM);
    c.fill(72, 30, 2, 2, on);
    c.fill(72, 35, 2, 2, on);
    c.lcd_digit(78, 26, lit ? s / 10 : -1, on, C_GREEN_DIM);
    c.lcd_digit(90, 26, lit ? s % 10 : -1, on, C_GREEN_DIM);
  }
  for(int b = 0; b < 19; b++) {                     /* 19 bars, 3 px wide, 1 px gap */
    int hh = std::min(16, static_cast<int>(ui.bars[b] * 16.0f + 0.5f));
    for(int j = 0; j < hh; j++) {
      uint32_t col = (j < 9) ? 0xff18c818u : ((j < 13) ? 0xffd8d018u : 0xffe83018u);
      c.fill(R_VIS.x + 4*b, R_VIS.y + 15 - j, 3, 1, col);
    }
    int pk = std::min(15, static_cast<int>(ui.peaks[b] * 16.0f));
    if(pk > 0) {
      c.fill(R_VIS.x + 4*b, R_VIS.y + 15 - pk, 3, 1, 0xffc8c8c8u);
    }
  }

  /* ---- song title marquee and the little info fields ---- */
  c.fill(R_SONG.x, R_SONG.y, R_SONG.w, R_SONG.h, C_LCD);
  c.bevel(R_SONG.x - 1, R_SONG.y - 1, R_SONG.w + 2, R_SONG.h + 2, C_LO, C_HI);
  if(trk != nullptr) {
    char num[16];
    snprintf(num, sizeof(num), "%d. ", ui.cur + 1);
    std::string s = p.seeking.load() ? std::string("SEEKING...") : (std::string(num) + trk->title + " (" + mmss(track_total(p, *trk)) + ")");
    int sw = static_cast<int>(s.size()) * 6;
    if(sw <= R_SONG.w - 4) {
      c.text57(R_SONG.x + 3, R_SONG.y + 3, s, C_GREEN);
    }
    else {                                          /* too wide: scroll it, XMMS style */
      std::string loop = s + "  ***  ";
      int lw = static_cast<int>(loop.size()) * 6, off = static_cast<int>(ui.marquee) % lw;
      c.text57(R_SONG.x + 3 - off, R_SONG.y + 3, loop + loop, C_GREEN, R_SONG.x + 2, R_SONG.x + R_SONG.w - 2);
    }
  }
  c.fill(111, 43, 17, 9, C_LCD);
  c.text35(113, 45, "ROM", C_GREEN);
  c.text35(131, 45, "LIVE", 0xff9aa4bcu);
  c.fill(156, 43, 11, 9, C_LCD);
  c.text35(158, 45, "22", C_GREEN);
  c.text35(170, 45, "KHZ", 0xff9aa4bcu);
  c.text35(212, 43, "MONO", 0xff4a5268u);
  c.text35(237, 43, "STEREO", (ui.state == play_state_t::playing) ? C_LED_ON : 0xff9aa4bcu);

  /* ---- volume, balance, EQ/PL ---- */
  auto slider = [&](const rect_t &r, double frac, bool centred) {
    double heat = centred ? std::fabs(frac - 0.5) * 2.0 : frac;
    uint32_t col = 0xff000000u | (static_cast<uint32_t>(40 + 200 * heat) << 16) | (static_cast<uint32_t>(200 - 120 * heat) << 8) | 0x20u;
    c.fill(r.x, r.y + 4, r.w, 5, C_LCD);
    c.bevel(r.x - 1, r.y + 3, r.w + 2, 7, C_LO, C_HI);
    c.fill(r.x + 1, r.y + 5, r.w - 2, 3, col);
    rect_t k = {r.x + static_cast<int>(frac * (r.w - 14)), r.y + 1, 14, 11};
    draw_button(c, k, false);
    c.fill(k.x + 6, k.y + 3, 2, 5, C_GLYPH);
  };
  slider(R_VOL, p.volume.load() / 100.0, false);
  slider(R_BAL, (p.balance.load() + 100) / 200.0, true);
  draw_button(c, R_EQ, false);
  c.text35(R_EQ.x + 8, R_EQ.y + 4, "EQ", 0xff6a7288u);
  draw_button(c, R_PL, ui.show_pl);
  c.text35(R_PL.x + 8, R_PL.y + 4, "PL", ui.show_pl ? C_LED_ON : C_GLYPH);

  /* ---- position bar ---- */
  c.fill(R_POS.x, R_POS.y + 2, R_POS.w, 6, C_LCD);
  c.bevel(R_POS.x - 1, R_POS.y + 1, R_POS.w + 2, 8, C_LO, C_HI);
  if(trk != nullptr and ui.state != play_state_t::stopped) {
    double total = std::max(1.0, track_total(p, *trk));
    double frac = (ui.drag == 3) ? ui.drag_pos : std::min(1.0, std::fmod(t_now, total + 0.001) / total);
    rect_t k = {R_POS.x + static_cast<int>(frac * (R_POS.w - 29)), R_POS.y, 29, 10};
    draw_button(c, k, ui.drag == 3);
    for(int i = 0; i < 3; i++) {
      c.fill(k.x + 11 + 3*i, k.y + 3, 1, 4, C_GLYPH);
    }
  }

  /* ---- transport ---- */
  const rect_t *btn[6] = {&R_PREV, &R_PLAY, &R_PAUSE, &R_STOP, &R_NEXT, &R_EJECT};
  for(int i = 0; i < 6; i++) {
    draw_button(c, *btn[i], ui.pressed == i);
  }
  int o = 0;
  o = (ui.pressed == 0); c.fill(R_PREV.x + 6 + o, 93 + o, 2, 8, C_GLYPH); draw_tri(c, R_PREV.x + 15 + o, 93 + o, 8, false, C_GLYPH);
  o = (ui.pressed == 1); draw_tri(c, R_PLAY.x + 9 + o, 92 + o, 10, true, C_GLYPH);
  o = (ui.pressed == 2); c.fill(R_PAUSE.x + 7 + o, 93 + o, 3, 8, C_GLYPH); c.fill(R_PAUSE.x + 13 + o, 93 + o, 3, 8, C_GLYPH);
  o = (ui.pressed == 3); c.fill(R_STOP.x + 8 + o, 93 + o, 8, 8, C_GLYPH);
  o = (ui.pressed == 4); draw_tri(c, R_NEXT.x + 6 + o, 93 + o, 8, true, C_GLYPH); c.fill(R_NEXT.x + 14 + o, 93 + o, 2, 8, C_GLYPH);
  o = (ui.pressed == 5);
  for(int j = 0; j < 5; j++) {
    c.fill(R_EJECT.x + 10 - j + o, 92 + j + o, 2*j + 2, 1, C_GLYPH);
  }
  c.fill(R_EJECT.x + 6 + o, 99 + o, 10, 2, C_GLYPH);
  draw_button(c, R_SHUF, ui.shuffle);
  c.fill(R_SHUF.x + 4, R_SHUF.y + 5, 4, 4, ui.shuffle ? C_LED_ON : C_LED_OFF);
  c.text35(R_SHUF.x + 12, R_SHUF.y + 5, "SHUFFLE", C_GLYPH);
  draw_button(c, R_REP, p.repeat.load());
  c.fill(R_REP.x + 4, R_REP.y + 5, 4, 4, p.repeat.load() ? C_LED_ON : C_LED_OFF);
  c.text35(R_REP.x + 12, R_REP.y + 5, "REP", C_GLYPH);
  c.text35(246, 94, "N64", 0xff6a7288u);

  /* ---- playlist ---- */
  if(not(ui.show_pl)) {
    return;
  }
  c.vgrad(0, MAIN_H, W, PL_H, C_BODY_T, C_BODY_B);
  c.bevel(0, MAIN_H, W, PL_H, C_HI, C_LO);
  c.vgrad(1, MAIN_H + 1, W - 2, 13, 0xff20263au, 0xff10131cu);
  for(int y = MAIN_H + 4; y <= MAIN_H + 10; y += 2) {
    c.fill(6, y, 96, 1, 0xff55607cu);
    c.fill(172, y, 96, 1, 0xff55607cu);
  }
  c.text57(110, MAIN_H + 4, "PLAYLIST", C_TITLE);
  c.fill(R_PLIST.x, R_PLIST.y, R_PLIST.w + 1, R_PLIST.h, C_LCD);
  c.bevel(R_PLIST.x - 1, R_PLIST.y - 1, R_PLIST.w + 3, R_PLIST.h + 2, C_LO, C_HI);
  double all = 0.0;
  for(const track_t &t : ui.tracks) {
    all += track_total(p, t);
  }
  for(int row = 0; row < PL_ROWS; row++) {
    int i = ui.pl_scroll + row;
    if(i >= static_cast<int>(ui.tracks.size())) {
      break;
    }
    int y = R_PLIST.y + row * PL_ROW_H;
    if(i == ui.sel) {
      c.fill(R_PLIST.x, y, R_PLIST.w + 1, PL_ROW_H, C_PL_SEL);
    }
    uint32_t col = (i == ui.cur) ? C_PL_CUR : C_PL_TEXT;
    char num[16];
    snprintf(num, sizeof(num), "%d. ", i + 1);
    c.text57(R_PLIST.x + 3, y + 1, std::string(num) + ui.tracks[i].title, col, R_PLIST.x, R_PLIST.x + R_PLIST.w - 30);
    std::string tm = mmss(track_total(p, ui.tracks[i]));
    c.text57(R_PLIST.x + R_PLIST.w - 2 - static_cast<int>(tm.size()) * 6, y + 1, tm, col);
  }
  c.fill(R_PLSCROLL.x, R_PLSCROLL.y, R_PLSCROLL.w, R_PLSCROLL.h, C_LCD);
  int n = static_cast<int>(ui.tracks.size()), span = std::max(1, n - PL_ROWS);
  rect_t thumb = {R_PLSCROLL.x, R_PLSCROLL.y + (R_PLSCROLL.h - 18) * ui.pl_scroll / span, 8, 18};
  draw_button(c, thumb, false);
  char foot[64];
  snprintf(foot, sizeof(foot), "%d TUNES  %s", n, mmss(all).c_str());
  c.fill(6, MAIN_H + PL_H - 17, 150, 11, C_LCD);
  c.text57(9, MAIN_H + PL_H - 15, foot, C_PL_TEXT);
  c.text35(190, MAIN_H + PL_H - 12, "LIVE FROM YOUR ROM", 0xff6a7288u);
}

/* ------------------------------------------------------------------ spectrum */

static void fft(std::vector<float> &re, std::vector<float> &im) {
  size_t n = re.size();
  for(size_t i = 1, j = 0; i < n; i++) {
    size_t bit = n >> 1;
    for(; j & bit; bit >>= 1) {
      j ^= bit;
    }
    j ^= bit;
    if(i < j) {
      std::swap(re[i], re[j]);
      std::swap(im[i], im[j]);
    }
  }
  for(size_t len = 2; len <= n; len <<= 1) {
    float ang = -2.0f * static_cast<float>(M_PI) / static_cast<float>(len);
    for(size_t i = 0; i < n; i += len) {
      for(size_t k = 0; k < len/2; k++) {
	float wr = std::cos(ang * k), wi = std::sin(ang * k);
	size_t a = i + k, b = i + k + len/2;
	float xr = re[b]*wr - im[b]*wi, xi = re[b]*wi + im[b]*wr;
	re[b] = re[a] - xr; im[b] = im[a] - xi;
	re[a] += xr; im[a] += xi;
      }
    }
  }
}

static void update_spectrum(ui_t &ui, player_t &p, double dt) {
  const size_t N = 1024;
  std::vector<float> re(N), im(N, 0.0f);
  uint32_t w = p.vis_w.load();
  for(size_t i = 0; i < N; i++) {
    float hann = 0.5f - 0.5f * std::cos(2.0f * static_cast<float>(M_PI) * i / (N - 1));
    re[i] = p.vis[(w - N + i) % VIS_LEN] * hann;
  }
  fft(re, im);
  for(int b = 0; b < 19; b++) {                     /* log-spaced bands, 50 Hz .. 10 kHz */
    double f0 = 50.0 * std::pow(200.0, b / 19.0), f1 = 50.0 * std::pow(200.0, (b + 1) / 19.0);
    size_t k0 = std::max<size_t>(1, static_cast<size_t>(f0 * N / GE_OUTPUT_RATE));
    size_t k1 = std::max(k0 + 1, static_cast<size_t>(f1 * N / GE_OUTPUT_RATE));
    float m = 0.0f;
    for(size_t k = k0; k < k1 and k < N/2; k++) {
      m = std::max(m, std::sqrt(re[k]*re[k] + im[k]*im[k]));
    }
    float db = 20.0f * std::log10(m / (N / 4.0f) + 1e-6f);
    float v = std::min(1.0f, std::max(0.0f, (db + 60.0f) / 54.0f));
    bool live = ui.state == play_state_t::playing;
    ui.bars[b] = live ? std::max(v, ui.bars[b] - static_cast<float>(dt) * 2.5f) : std::max(0.0f, ui.bars[b] - static_cast<float>(dt) * 2.5f);
    ui.peaks[b] = std::max(ui.bars[b], ui.peaks[b] - static_cast<float>(dt) * 0.5f);
  }
}

/* ------------------------------------------------------------------ control */

static void send_play(ui_t &ui, player_t &p, int idx) {
  if(ui.tracks.empty()) {
    return;
  }
  int n = static_cast<int>(ui.tracks.size());
  ui.cur = ((idx % n) + n) % n;
  ui.sel = ui.cur;
  ui.pl_scroll = std::min(std::max(ui.pl_scroll, ui.cur - PL_ROWS + 1), ui.cur);
  ui.marquee = 0.0;
  const track_t &t = ui.tracks[ui.cur];
  p.pass_seconds = t.seconds;
  p.track_loops = t.loops;
  p.paused.store(false);
  std::lock_guard<std::mutex> lk(p.cmd_lock);
  p.cmd_seq = t.seq;
  p.cmd_seek = -1.0;
  ui.state = play_state_t::playing;
}

static void send_stop(ui_t &ui, player_t &p) {
  std::lock_guard<std::mutex> lk(p.cmd_lock);
  p.cmd_stop = true;
  p.ring_r.store(p.ring_w.load());
  p.played.store(0);
  ui.state = play_state_t::stopped;
}

static void next_track(ui_t &ui, player_t &p, int dir) {
  int n = static_cast<int>(ui.tracks.size());
  int idx = ui.shuffle ? (rand() % std::max(1, n)) : (ui.cur + dir);
  send_play(ui, p, idx);
}

static void toggle_pause(ui_t &ui, player_t &p) {
  if(ui.state == play_state_t::playing) {
    p.paused.store(true);
    ui.state = play_state_t::paused;
  }
  else if(ui.state == play_state_t::paused) {
    p.paused.store(false);
    ui.state = play_state_t::playing;
  }
}

static SDL_HitTestResult hit_test(SDL_Window *, const SDL_Point *pt, void *ud) {
  int scale = *static_cast<int*>(ud);
  int x = pt->x / scale, y = pt->y / scale;
  bool bar = R_TITLEBAR.has(x, y) or (y >= MAIN_H and y < MAIN_H + 14);
  return (bar and not(R_CLOSE.has(x, y)) and not(R_MIN.has(x, y))) ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
}

static bool save_bmp(const canvas_t &c, const std::string &name, int scale) {
  SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, W * scale, c.h * scale, 32, SDL_PIXELFORMAT_ARGB8888);
  if(s == nullptr) {
    return false;
  }
  for(int y = 0; y < c.h * scale; y++) {
    uint32_t *row = static_cast<uint32_t*>(s->pixels) + y * (s->pitch / 4);
    for(int x = 0; x < W * scale; x++) {
      row[x] = c.px[static_cast<size_t>(y / scale) * W + x / scale];
    }
  }
  bool ok = SDL_SaveBMP(s, name.c_str()) == 0;
  SDL_FreeSurface(s);
  return ok;
}

/* Shared by the native window and the browser canvas.  Returns false if the app
 * should quit; win is null in the browser, where there is nothing to minimise and
 * the page, not us, owns the window. */
static bool handle_event(const SDL_Event &ev, ui_t &ui, player_t &p, SDL_Window *win, int scale) {
  static uint32_t last_click = 0;
  if(ev.type == SDL_QUIT) {
	return false;
  }
  else if(ev.type == SDL_KEYDOWN) {
	double t_now = static_cast<double>(p.played.load()) / GE_OUTPUT_RATE;
	switch(ev.key.keysym.sym)
	  {
	  case SDLK_q: case SDLK_ESCAPE: return false; break;
	  case SDLK_z: next_track(ui, p, -1); break;
	  case SDLK_x: send_play(ui, p, ui.cur); break;
	  case SDLK_c: case SDLK_SPACE: toggle_pause(ui, p); break;
	  case SDLK_v: send_stop(ui, p); break;
	  case SDLK_b: next_track(ui, p, 1); break;
	  case SDLK_s: ui.shuffle = not(ui.shuffle); break;
	  case SDLK_r: p.repeat.store(not(p.repeat.load())); break;
	  case SDLK_UP: p.volume.store(std::min(100, p.volume.load() + 5)); break;
	  case SDLK_DOWN: p.volume.store(std::max(0, p.volume.load() - 5)); break;
	  case SDLK_RETURN: send_play(ui, p, ui.sel); break;
	  case SDLK_LEFT: case SDLK_RIGHT:
	    if(ui.state != play_state_t::stopped) {
	      std::lock_guard<std::mutex> lk(p.cmd_lock);
	      p.cmd_seek = std::max(0.0, t_now + ((ev.key.keysym.sym == SDLK_LEFT) ? -5.0 : 5.0));
	    }
	    break;
	  default: break;
	  }
  }
  else if(ev.type == SDL_MOUSEWHEEL) {
	int span = std::max(0, static_cast<int>(ui.tracks.size()) - PL_ROWS);
	ui.pl_scroll = std::min(span, std::max(0, ui.pl_scroll - 3 * ev.wheel.y));
  }
  else if(ev.type == SDL_MOUSEBUTTONDOWN and ev.button.button == SDL_BUTTON_LEFT) {
	int x = ev.button.x / scale, y = ev.button.y / scale;
	const rect_t *btn[6] = {&R_PREV, &R_PLAY, &R_PAUSE, &R_STOP, &R_NEXT, &R_EJECT};
	for(int i = 0; i < 6; i++) {
	  if(btn[i]->has(x, y)) {
	    ui.pressed = i;
	  }
	}
	if(R_CLOSE.has(x, y)) { return false; }
	else if(R_MIN.has(x, y)) { if(win != nullptr) { SDL_MinimizeWindow(win); } }
	else if(R_VOL.has(x, y)) { ui.drag = 1; }
	else if(R_BAL.has(x, y)) { ui.drag = 2; }
	else if(R_POS.has(x, y) and ui.state != play_state_t::stopped) { ui.drag = 3; }
	else if(R_SHUF.has(x, y)) { ui.shuffle = not(ui.shuffle); }
	else if(R_REP.has(x, y)) { p.repeat.store(not(p.repeat.load())); }
	else if(R_PL.has(x, y)) {
	  ui.show_pl = not(ui.show_pl);
	  if(win != nullptr) {
	    SDL_SetWindowSize(win, W * scale, (ui.show_pl ? (MAIN_H + PL_H) : MAIN_H) * scale);
	  }
	}
	else if(ui.show_pl and R_PLIST.has(x, y)) {
	  int i = ui.pl_scroll + (y - R_PLIST.y) / PL_ROW_H;
	  if(i < static_cast<int>(ui.tracks.size())) {
	    bool dbl = (i == ui.sel) and (SDL_GetTicks() - last_click < 400);
	    ui.sel = i;
	    last_click = SDL_GetTicks();
	    if(dbl) {
	      send_play(ui, p, i);
	    }
	  }
	}
	else if(ui.show_pl and R_PLSCROLL.has(x, y)) { ui.drag = 4; }
  }
  if(ev.type == SDL_MOUSEMOTION or ev.type == SDL_MOUSEBUTTONDOWN) {
	int x = ((ev.type == SDL_MOUSEMOTION) ? ev.motion.x : ev.button.x) / scale;
	int y = ((ev.type == SDL_MOUSEMOTION) ? ev.motion.y : ev.button.y) / scale;
	auto frac = [&](const rect_t &r, int knob) { return std::min(1.0, std::max(0.0, static_cast<double>(x - r.x - knob/2) / (r.w - knob))); };
	if(ui.drag == 1) { p.volume.store(static_cast<int>(frac(R_VOL, 14) * 100.0 + 0.5)); }
	else if(ui.drag == 2) {
	  int b = static_cast<int>(frac(R_BAL, 14) * 200.0 + 0.5) - 100;
	  p.balance.store((std::abs(b) < 12) ? 0 : b);              /* a detent at centre */
	}
	else if(ui.drag == 3) { ui.drag_pos = frac(R_POS, 29); }
	else if(ui.drag == 4) {
	  int span = std::max(0, static_cast<int>(ui.tracks.size()) - PL_ROWS);
	  ui.pl_scroll = static_cast<int>(std::min(1.0, std::max(0.0, static_cast<double>(y - R_PLSCROLL.y - 9) / (R_PLSCROLL.h - 18))) * span + 0.5);
	}
  }
  if(ev.type == SDL_MOUSEBUTTONUP and ev.button.button == SDL_BUTTON_LEFT) {
	int x = ev.button.x / scale, y = ev.button.y / scale;
	const rect_t *btn[6] = {&R_PREV, &R_PLAY, &R_PAUSE, &R_STOP, &R_NEXT, &R_EJECT};
	if(ui.pressed >= 0 and btn[ui.pressed]->has(x, y)) {
	  switch(ui.pressed)
	    {
	    case 0: next_track(ui, p, -1); break;
	    case 1: send_play(ui, p, ui.cur); break;
	    case 2: toggle_pause(ui, p); break;
	    case 3: send_stop(ui, p); break;
	    case 4: next_track(ui, p, 1); break;
	    default: send_stop(ui, p); ui.cur = ui.sel = ui.pl_scroll = 0; break;   /* eject: back to the top */
	    }
	}
	if(ui.drag == 3 and not(ui.tracks.empty())) {                 /* XMMS seeks on release */
	  std::lock_guard<std::mutex> lk(p.cmd_lock);
	  p.cmd_seek = ui.drag_pos * track_total(p, ui.tracks[ui.cur]);
	}
	ui.pressed = -1;
	ui.drag = 0;
  }
  return true;
}

/* ---------------------------------------------------------------- browser front end */
#ifdef __EMSCRIPTEN__

/* One global session, because the browser hands us the ROM asynchronously and then
 * drives everything from the main loop.  No worker thread: see pump_t. */
/* the canvas is this many device pixels per UI pixel; mouse coordinates arrive in
 * canvas pixels, so the same number has to divide them again */
static const int WEB_SCALE = 2;

namespace {
  struct web_t {
    std::vector<uint8_t> rom;
    engine_t *eng = nullptr;
    pump_t *pump = nullptr;
    player_t *p = nullptr;
    ui_t ui;
    canvas_t *canvas = nullptr;
    SDL_Window *win = nullptr;
    SDL_Renderer *ren = nullptr;
    SDL_Texture *tex = nullptr;
    SDL_AudioDeviceID dev = 0;
    int have_freq = 0, have_channels = 0, have_samples = 0;
    double read_frac = 0.0;
    uint64_t ticks = 0, rendered = 0, pulls = 0;   /* main loop / engine / audio */
    bool have_rom = false;
    std::string message = "DROP A GOLDENEYE 007 (NGEE) ROM HERE";
    uint32_t last_ms = 0;
  };
  web_t g_web;
}

/* called from JavaScript once the user has picked a file */
extern "C" EMSCRIPTEN_KEEPALIVE void gemms_load_rom(const uint8_t *data, int len) {
  web_t &w = g_web;
  if(len < 0x43865a or memcmp(data + 0x3b, "NGEE", 4) != 0) {
    w.message = "THAT IS NOT A BIG-ENDIAN GOLDENEYE 007 (NGEE) ROM";
    return;
  }
  w.rom.assign(data, data + len);
  delete w.pump;
  delete w.eng;
  w.eng = new engine_t(w.rom);                  /* no jit in the browser */
  w.pump = new pump_t(*w.p, *w.eng);
  w.ui.tracks.clear();
  for(int s = 0; s < w.eng->n_sequences() and s < 63; s++) {
    seq_info_t info = ge_sequence_info(w.rom, s);
    if(not(info.empty) and g_titles[s] != nullptr) {
      w.ui.tracks.push_back({s, g_titles[s], info.seconds, info.loops});
    }
  }
  w.have_rom = true;
  send_play(w.ui, *w.p, 0);
}

/* Web Audio pulls from here.  The browser's context runs at its own rate (usually
 * 48 kHz) while the console's is 22047, so resample on the way out; the fractional
 * read position lives across calls.  Writes interleaved stereo float, and returns
 * how many frames actually came from the ring (short means we underran). */
extern "C" EMSCRIPTEN_KEEPALIVE int gemms_pull(float *out, int frames, int out_rate) {
  g_web.pulls++;
  player_t &p = *g_web.p;
  double step = static_cast<double>(GE_OUTPUT_RATE) / ((out_rate > 0) ? out_rate : GE_OUTPUT_RATE);
  int vol = p.volume.load(), bal = p.balance.load();
  double gl = vol * ((bal > 0) ? (100 - bal) : 100) / 10000.0 / 32768.0;
  double gr = vol * ((bal < 0) ? (100 + bal) : 100) / 10000.0 / 32768.0;
  uint32_t r = p.ring_r.load(), w = p.ring_w.load(), vw = p.vis_w.load();
  double frac = g_web.read_frac;
  int got = 0;
  bool paused = p.paused.load();
  for(int i = 0; i < frames; i++) {
    float l = 0.0f, rr = 0.0f;
    if(not(paused) and r < w) {
      /* interpolate towards the next frame when there is one; on the very last
       * frame hold it instead, so the ring can always drain to empty -- otherwise
       * a leftover frame stops the track ever being seen as finished */
      const int16_t *a = &p.ring[2*(r % RING_FRAMES)];
      const int16_t *b = ((r + 1) < w) ? &p.ring[2*((r + 1) % RING_FRAMES)] : a;
      l = static_cast<float>((a[0] + (b[0] - a[0]) * frac) * gl);
      rr = static_cast<float>((a[1] + (b[1] - a[1]) * frac) * gr);
      frac += step;
      while(frac >= 1.0 and r < w) {
	frac -= 1.0;
	r++;
	got++;
      }
    }
    out[2*i] = l;
    out[2*i+1] = rr;
    p.vis[vw % VIS_LEN] = 0.5f * (l + rr);
    vw++;
  }
  p.ring_r.store(r);
  p.vis_w.store(vw);
  g_web.read_frac = frac;
  p.played.fetch_add(static_cast<uint64_t>(got));
  return got;
}

/* what the page shows in its diagnostics line */
extern "C" EMSCRIPTEN_KEEPALIVE int gemms_ticks() { return static_cast<int>(g_web.ticks); }
extern "C" EMSCRIPTEN_KEEPALIVE int gemms_rendered() { return static_cast<int>(g_web.rendered); }
extern "C" EMSCRIPTEN_KEEPALIVE int gemms_pulls() { return static_cast<int>(g_web.pulls); }
extern "C" EMSCRIPTEN_KEEPALIVE const char *gemms_build_id() { return GEMMS_BUILD_ID; }
extern "C" EMSCRIPTEN_KEEPALIVE int gemms_audio_dev() { return static_cast<int>(g_web.dev); }
extern "C" EMSCRIPTEN_KEEPALIVE int gemms_audio_freq() { return g_web.have_freq; }
extern "C" EMSCRIPTEN_KEEPALIVE int gemms_audio_channels() { return g_web.have_channels; }
extern "C" EMSCRIPTEN_KEEPALIVE int gemms_ring_fill() { return g_web.p ? static_cast<int>(ring_used(*g_web.p)) : -1; }
extern "C" EMSCRIPTEN_KEEPALIVE int gemms_played_frames() { return g_web.p ? static_cast<int>(g_web.p->played.load()) : -1; }

static void web_splash(canvas_t &c, const std::string &msg) {
  c.vgrad(0, 0, W, MAIN_H, C_BODY_T, C_BODY_B);
  c.bevel(0, 0, W, MAIN_H, C_HI, C_LO);
  c.vgrad(1, 1, W - 2, 13, 0xff20263au, 0xff10131cu);
  c.text57(34, 4, "GOLDENEYE MULTIMEDIA SYSTEM", C_TITLE);
  c.fill(9, 26, W - 18, MAIN_H - 36, C_LCD);
  c.bevel(8, 25, W - 16, MAIN_H - 34, C_LO, C_HI);
  int x = (W - static_cast<int>(msg.size()) * 6) / 2;
  c.text57((x < 12) ? 12 : x, 52, msg, C_GREEN, 10, W - 10);
  c.text35(84, 72, "NOTHING IS SENT ANYWHERE", 0xff6a7288u);
  {
    std::string b = std::string("BUILD ") + GEMMS_BUILD_ID;
    c.text35((W - static_cast<int>(b.size()) * 4) / 2, 88, b, 0xff4a5268u);
  }
}

static void web_frame() {
  web_t &w = g_web;
  w.ticks++;
  SDL_Event ev;
  while(SDL_PollEvent(&ev)) {
    if(w.have_rom) {
      bool pl_was = w.ui.show_pl;
      handle_event(ev, w.ui, *w.p, w.win, WEB_SCALE);
      if(w.ui.show_pl != pl_was) {                 /* the PL button resizes the canvas */
	SDL_SetWindowSize(w.win, W * WEB_SCALE,
			  (w.ui.show_pl ? (MAIN_H + PL_H) : MAIN_H) * WEB_SCALE);
      }
    }
  }
  uint32_t now = SDL_GetTicks();
  double dt = (w.last_ms == 0) ? 0.016 : (now - w.last_ms) * 1e-3;
  w.last_ms = now;
  if(w.have_rom) {
    /* keep the ring topped up; the interpreter is far faster than real time, so a
     * bounded number of frames per tick is plenty and keeps the browser responsive */
    for(int i = 0; i < 64 and w.pump->step(); i++) {
      w.rendered++;
    }
    if(w.ui.state == play_state_t::playing) {
      w.ui.marquee += dt * 30.0;
      if(w.p->track_done.load() and ring_used(*w.p) == 0) {   /* fully drained */
	w.p->track_done.store(false);
	bool at_end = (w.ui.cur + 1 >= static_cast<int>(w.ui.tracks.size())) and not(w.ui.shuffle);
	if(w.p->repeat.load()) {
	  send_play(w.ui, *w.p, w.ui.cur);
	}
	else if(at_end) {
	  send_stop(w.ui, *w.p);
	}
	else {
	  next_track(w.ui, *w.p, 1);
	}
      }
    }
    update_spectrum(w.ui, *w.p, dt);
    draw(*w.canvas, w.ui, *w.p);
  }
  else {
    web_splash(*w.canvas, w.message);
  }
  SDL_UpdateTexture(w.tex, nullptr, w.canvas->px.data(), W * 4);
  SDL_Rect src = {0, 0, W, (w.have_rom and w.ui.show_pl) ? (MAIN_H + PL_H) : MAIN_H};
  SDL_RenderClear(w.ren);
  SDL_RenderCopy(w.ren, w.tex, &src, nullptr);
  SDL_RenderPresent(w.ren);
}

int main() {
  web_t &w = g_web;
  w.p = new player_t();
  SDL_SetMainReady();
  if(SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return -1;
  }
  /* no SDL audio here: the page creates the Web Audio context inside a user
   * gesture (iOS will not start one otherwise) and pulls through gemms_pull. */
  w.win = SDL_CreateWindow("gemms", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
			   W * WEB_SCALE, (MAIN_H + PL_H) * WEB_SCALE, 0);
  w.ren = SDL_CreateRenderer(w.win, -1, 0);
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  w.tex = SDL_CreateTexture(w.ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, MAIN_H + PL_H);
  w.canvas = new canvas_t(MAIN_H + PL_H);
  emscripten_set_main_loop(web_frame, 0, 1);
  return 0;
}

#else
int main(int argc, char *argv[]) {
  std::string rom_name = "GoldenEye.z64", shot;
  int scale = 2, start_track = 1;
  double start_at = 0.0;
  bool use_jit = true;
  player_t *pp = new player_t();
  player_t &p = *pp;
  try {
    po::options_description desc("Options");
    desc.add_options()
      ("help,h", "print help")
      ("rom,r", po::value<std::string>(&rom_name), "GoldenEye 007 (NGEE) big-endian ROM")
      ("scale,x", po::value<int>(&scale), "integer pixel scale (default 2)")
      ("track,t", po::value<int>(&start_track), "playlist entry to start on")
      ("at", po::value<double>(&start_at), "start this many seconds into the tune")
      ("passes", po::value<int>(&p.loop_passes), "passes through a looping tune before it fades (default 2)")
      ("fade", po::value<double>(&p.fade_seconds), "fade-out seconds (default 8)")
      ("screenshot", po::value<std::string>(&shot), "draw one frame to this .bmp and exit (no window, no audio)")
      ("no-jit", "use the RSP interpreter even in a build with the LLVM translator")
      ;
    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);
    use_jit = vm.count("no-jit") == 0;
    if(vm.count("help")) {
      std::cout << desc << "\nkeys: z prev, x play, c pause, v stop, b next, s shuffle, r repeat,\n"
		<< "      left/right seek 5 s, up/down volume, enter plays the selected tune, q quits\n";
      return 0;
    }
  }
  catch(po::error &e) {
    std::cerr << "command-line error : " << e.what() << "\n";
    return -1;
  }
  scale = std::max(1, std::min(8, scale));

  std::ifstream in(rom_name, std::ios::binary);
  std::vector<uint8_t> rom((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if(rom.size() < 0x43865a or memcmp(&rom[0x3b], "NGEE", 4) != 0) {
    fprintf(stderr, "%s is not a big-endian GoldenEye 007 (NGEE) image\n", rom_name.c_str());
    return -1;
  }
  engine_t eng(rom, use_jit);
  ui_t ui;
  for(int s = 0; s < eng.n_sequences() and s < 63; s++) {
    seq_info_t info = ge_sequence_info(rom, s);
    if(not(info.empty) and g_titles[s] != nullptr) {
      ui.tracks.push_back({s, g_titles[s], info.seconds, info.loops});
    }
  }

  if(not(shot.empty())) {                           /* headless: a posed frame for the README */
    ui.cur = ui.sel = std::min<int>(6, static_cast<int>(ui.tracks.size()) - 1);
    ui.state = play_state_t::playing;
    ui.demo_time = 83.0;
    for(int b = 0; b < 19; b++) {
      ui.bars[b] = 0.25f + 0.7f * std::fabs(std::sin(0.9f * b + 0.6f)) * (1.0f - b / 26.0f);
      ui.peaks[b] = std::min(1.0f, ui.bars[b] + 0.12f);
    }
    canvas_t c(MAIN_H + PL_H);
    draw(c, ui, p);
    return save_bmp(c, shot, scale) ? 0 : -1;
  }

  SDL_SetMainReady();
  if(SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return -1;
  }
  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = GE_OUTPUT_RATE;                       /* the console's true rate; SDL resamples if it must */
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 1024;
  want.callback = audio_cb;
  want.userdata = &p;
  SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if(dev == 0) {
    fprintf(stderr, "SDL_OpenAudioDevice: %s\n", SDL_GetError());
    return -1;
  }
  SDL_Window *win = SDL_CreateWindow("gemms", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
				     W * scale, (MAIN_H + PL_H) * scale, SDL_WINDOW_BORDERLESS);
  SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
  if(win == nullptr or ren == nullptr) {
    fprintf(stderr, "SDL window: %s\n", SDL_GetError());
    return -1;
  }
  SDL_SetWindowHitTest(win, hit_test, &scale);
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, MAIN_H + PL_H);

  std::thread worker(engine_thread, &p, &eng);
  SDL_PauseAudioDevice(dev, 0);
  send_play(ui, p, start_track - 1);
  if(start_at > 0.0) {
    std::lock_guard<std::mutex> lk(p.cmd_lock);
    p.cmd_seek = start_at;
  }

  canvas_t canvas(MAIN_H + PL_H);
  uint32_t last = SDL_GetTicks(), last_click = 0;
  bool running = true;
  while(running) {
    SDL_Event ev;
    while(SDL_PollEvent(&ev)) {
      running = handle_event(ev, ui, p, win, scale) and running;
    }

    uint32_t now = SDL_GetTicks();
    double dt = (now - last) * 1e-3;
    last = now;
    if(ui.state == play_state_t::playing) {
      ui.marquee += dt * 30.0;
      if(p.track_done.load() and ring_used(p) == 0) {               /* drained: what next? */
	p.track_done.store(false);
	bool at_end = (ui.cur + 1 >= static_cast<int>(ui.tracks.size())) and not(ui.shuffle);
	if(p.repeat.load()) {
	  send_play(ui, p, ui.cur);
	}
	else if(at_end) {
	  send_stop(ui, p);
	}
	else {
	  next_track(ui, p, 1);
	}
      }
    }
    update_spectrum(ui, p, dt);
    draw(canvas, ui, p);
    SDL_UpdateTexture(tex, nullptr, canvas.px.data(), W * 4);
    SDL_Rect src = {0, 0, W, ui.show_pl ? (MAIN_H + PL_H) : MAIN_H};
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, &src, nullptr);
    SDL_RenderPresent(ren);
    uint32_t spent = SDL_GetTicks() - now;          /* vsync may be absent: cap at ~60 fps ourselves */
    if(spent < 16) {
      SDL_Delay(16 - spent);
    }
  }

  fprintf(stderr, "gemms: RSP backend %s\n", eng.jit_active() ? "llvm" : "interpreter");
  fprintf(stderr, "gemms: stopped on playlist entry %d (%s) at %s, state %s\n", ui.cur + 1,
	  ui.tracks.empty() ? "-" : ui.tracks[ui.cur].title.c_str(),
	  mmss(static_cast<double>(p.played.load()) / GE_OUTPUT_RATE).c_str(),
	  (ui.state == play_state_t::playing) ? "playing" : ((ui.state == play_state_t::paused) ? "paused" : "stopped"));
  p.quit.store(true);
  worker.join();
  SDL_CloseAudioDevice(dev);
  SDL_DestroyTexture(tex);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  SDL_Quit();
  delete pp;
  return 0;
}
#endif
