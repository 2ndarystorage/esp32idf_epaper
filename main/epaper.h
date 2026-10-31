/*
 * epaper.h - e-Paper I2Cモジュール (Waveshare 1.54" 200x200) ドライバ + 描画API
 *
 * 使い方の流れ:
 *   epaper_init(panel);                 // 初期化 (panel = EPAPER_PANEL_V1 / V2)
 *   epaper_fb_clear(EPAPER_WHITE);      // フレームバッファ (200x200, RAM上) に描く
 *   epaper_fb_draw_string(10, 10, "Hello!", &Font16, EPAPER_BLACK);
 *   epaper_display_full();              // 全画面リフレッシュで実表示
 *   ... 一部だけ変えたら ...
 *   epaper_display_window(0, 100, 200, 32);  // その矩形だけ部分リフレッシュ
 *   epaper_sleep();                     // 電子ペーパーはこのまま表示が残る
 *
 * 色は EPAPER_BLACK=0 / EPAPER_WHITE=1 (バッファのbitそのまま: 1=白)。
 * フォントはメーカーサンプル同梱の Font8 / Font12 / Font16 / Font24 (fonts.h)。
 * 対応文字は ASCII 0x20..0x7E のみ。日本語フォントは入っていないので、
 * 日本語を出したい場合はPC側でビットマップ化して
 * epaper_fb_draw_bitmap() で描く (READMEに手順あり)。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "fonts.h"
#include "epaper_config.h"

#define EPAPER_WIDTH   200
#define EPAPER_HEIGHT  200
#define EPAPER_FB_SIZE (EPAPER_WIDTH * EPAPER_HEIGHT / 8)   /* 5000 bytes */

#define EPAPER_BLACK 0
#define EPAPER_WHITE 1

/* 初期化。panel は EPAPER_PANEL_V1 / EPAPER_PANEL_V2 (epaper_config.h)。
 * 起動時に1回呼ぶ。再初期化 (世代切替・スリープ復帰) のときも呼び直してよい。 */
esp_err_t epaper_init(int panel);

/* ディープスリープさせる (表示は保持される。復帰は epaper_init の呼び直し) */
void epaper_sleep(void);

/* ---- フレームバッファ描画 (この段階ではまだ実表示は変わらない) ---- */
void epaper_fb_clear(int color);
void epaper_fb_draw_pixel(int x, int y, int color);
void epaper_fb_draw_char(int x, int y, char c, const sFONT *font, int color);
void epaper_fb_draw_string(int x, int y, const char *text, const sFONT *font, int color);
void epaper_fb_draw_line(int x0, int y0, int x1, int y1, int color);
void epaper_fb_draw_rect(int x0, int y0, int x1, int y1, int color);         /* 枠 */
void epaper_fb_fill_rect(int x0, int y0, int x1, int y1, int color);         /* 塗り */
void epaper_fb_draw_circle(int x, int y, int radius, int color);
void epaper_fb_fill_circle(int x, int y, int radius, int color);
/* 1bitビットマップを描く (msb=左端, bit=1の画素を color で描く)。
 * w は8の倍数推奨。日本語テキストのビットマップ表示などに使う。 */
void epaper_fb_draw_bitmap(int x, int y, int w, int h, const uint8_t *bits, int color);

/* ---- 実表示への反映 ---- */
/* 全画面リフレッシュ。V1は全画面LUT、V2は初期化直後の状態で呼ぶ想定。
 * 表示後に部分リフレッシュを使うためのベース画像も内部で揃える。 */
esp_err_t epaper_display_full(void);

/* 矩形領域だけ部分リフレッシュ (残像が少なく高速)。
 * x と w は8の倍数に切り捨てられる。epaper_display_full() の後に使うこと。
 * V2では仕組み上、内部的に全フレームを送り直す (パネル側が差分だけ描く)。 */
esp_err_t epaper_display_window(int x, int y, int w, int h);

/* フレームバッファそのもの (上級者向け: 直接いじりたい場合) */
uint8_t *epaper_framebuffer(void);
