/*
 * epaper.c - e-Paper I2Cモジュール用ドライバ実装
 *
 * パネル制御シーケンスは、メーカー提供サンプル (Arduino版) の
 *   V1: epd1in54.cpp    (LUTを送る旧世代パネル, SSD1681系)
 *   V2: epd1in54_V2.cpp (OTP波形を使う新世代パネル。基板裏に [V2] マーク)
 * を ESP-IDF 向けに移植したもの。描画は epdpaint.cpp のアルゴリズムを
 * フルフレームバッファ (200x200 = 5000バイト) 向けに書き直した。
 *
 * 座標系: 原点は左上、x右方向、y下方向 (200x200)。
 *   V1は RAM書き込みが y増加方向なのでバッファをそのまま送る。
 *   V2は y減少方向 (初期値199から) なので、送信時に行順を反転して
 *   同じ見た目になるようにしている。
 */
#include "epaper.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "sc18is602.h"

static const char *TAG = "EPAPER";

static int s_panel = EPAPER_DEFAULT_PANEL;
static bool s_v1_partial = false;      /* V1: 部分更新LUTに切替済みか */
static uint8_t s_fb[EPAPER_FB_SIZE];
static uint8_t s_rev[EPAPER_FB_SIZE];  /* V2送信用: 行順反転コピー */

/* ================= 低レベル通信 ================= */

static esp_err_t send_command(uint8_t cmd)
{
    ESP_RETURN_ON_ERROR(sc18is602_gpio_write(SC18_PIN_DC, 0), TAG, "dc");
    return sc18is602_spi_write(&cmd, 1);
}

static esp_err_t send_data1(uint8_t data)
{
    ESP_RETURN_ON_ERROR(sc18is602_gpio_write(SC18_PIN_DC, 1), TAG, "dc");
    return sc18is602_spi_write(&data, 1);
}

static esp_err_t send_data_buf(const uint8_t *data, size_t len)
{
    ESP_RETURN_ON_ERROR(sc18is602_gpio_write(SC18_PIN_DC, 1), TAG, "dc");
    return sc18is602_spi_write(data, len);
}

static void delay_ms(int ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

/* BUSYピンは High=処理中, Low=完了 (V1/V2共通) */
static esp_err_t wait_idle(int timeout_ms)
{
    int waited = 0;
    while (1) {
        int busy = sc18is602_gpio_read(SC18_PIN_BUSY);
        if (busy == 0) return ESP_OK;
        if (busy < 0) {
            ESP_LOGW(TAG, "BUSY read failed");
            return ESP_FAIL;
        }
        if (waited >= timeout_ms) {
            ESP_LOGW(TAG, "BUSY timeout (%d ms) - panel version mismatch?", timeout_ms);
            return ESP_ERR_TIMEOUT;
        }
        delay_ms(20);
        waited += 20;
    }
}

static void hw_reset(void)
{
    if (s_panel == EPAPER_PANEL_V2) {
        /* V2サンプルの Reset() */
        sc18is602_gpio_write(SC18_PIN_RST, 1); delay_ms(200);
        sc18is602_gpio_write(SC18_PIN_RST, 0); delay_ms(10);
        sc18is602_gpio_write(SC18_PIN_RST, 1); delay_ms(200);
    } else {
        /* V1サンプルの Reset() */
        sc18is602_gpio_write(SC18_PIN_RST, 0); delay_ms(200);
        sc18is602_gpio_write(SC18_PIN_RST, 1); delay_ms(200);
    }
}

/* ================= V1 (epd1in54) ================= */

static const uint8_t lut_full_update[30] = {
    0x02, 0x02, 0x01, 0x11, 0x12, 0x12, 0x22, 0x22,
    0x66, 0x69, 0x69, 0x59, 0x58, 0x99, 0x99, 0x88,
    0x00, 0x00, 0x00, 0x00, 0xF8, 0xB4, 0x13, 0x51,
    0x35, 0x51, 0x51, 0x19, 0x01, 0x00
};
static const uint8_t lut_partial_update[30] = {
    0x10, 0x18, 0x18, 0x08, 0x18, 0x18, 0x08, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x13, 0x14, 0x44, 0x12,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static esp_err_t v1_set_lut(const uint8_t *lut)
{
    ESP_RETURN_ON_ERROR(send_command(0x32), TAG, "lut cmd");   /* WRITE_LUT_REGISTER */
    for (int i = 0; i < 30; i++) {
        ESP_RETURN_ON_ERROR(send_data1(lut[i]), TAG, "lut data");
    }
    return ESP_OK;
}

static esp_err_t v1_init(const uint8_t *lut)
{
    hw_reset();
    ESP_RETURN_ON_ERROR(send_command(0x01), TAG, "init");       /* DRIVER_OUTPUT_CONTROL */
    ESP_RETURN_ON_ERROR(send_data1((EPAPER_HEIGHT - 1) & 0xFF), TAG, "init");
    ESP_RETURN_ON_ERROR(send_data1(((EPAPER_HEIGHT - 1) >> 8) & 0xFF), TAG, "init");
    ESP_RETURN_ON_ERROR(send_data1(0x00), TAG, "init");
    ESP_RETURN_ON_ERROR(send_command(0x0C), TAG, "init");       /* BOOSTER_SOFT_START */
    ESP_RETURN_ON_ERROR(send_data1(0xD7), TAG, "init");
    ESP_RETURN_ON_ERROR(send_data1(0xD6), TAG, "init");
    ESP_RETURN_ON_ERROR(send_data1(0x9D), TAG, "init");
    ESP_RETURN_ON_ERROR(send_command(0x2C), TAG, "init");       /* WRITE_VCOM_REGISTER */
    ESP_RETURN_ON_ERROR(send_data1(0xA8), TAG, "init");
    ESP_RETURN_ON_ERROR(send_command(0x3A), TAG, "init");       /* SET_DUMMY_LINE_PERIOD */
    ESP_RETURN_ON_ERROR(send_data1(0x1A), TAG, "init");
    ESP_RETURN_ON_ERROR(send_command(0x3B), TAG, "init");       /* SET_GATE_TIME */
    ESP_RETURN_ON_ERROR(send_data1(0x08), TAG, "init");
    ESP_RETURN_ON_ERROR(send_command(0x11), TAG, "init");       /* DATA_ENTRY_MODE: Xinc/Yinc */
    ESP_RETURN_ON_ERROR(send_data1(0x03), TAG, "init");
    ESP_RETURN_ON_ERROR(v1_set_lut(lut), TAG, "init lut");
    s_v1_partial = (lut == lut_partial_update);
    return ESP_OK;
}

static esp_err_t v1_set_memory_area(int x0, int y0, int x1, int y1)
{
    ESP_RETURN_ON_ERROR(send_command(0x44), TAG, "area");       /* RAM X start/end */
    ESP_RETURN_ON_ERROR(send_data1((x0 >> 3) & 0xFF), TAG, "area");
    ESP_RETURN_ON_ERROR(send_data1((x1 >> 3) & 0xFF), TAG, "area");
    ESP_RETURN_ON_ERROR(send_command(0x45), TAG, "area");       /* RAM Y start/end */
    ESP_RETURN_ON_ERROR(send_data1(y0 & 0xFF), TAG, "area");
    ESP_RETURN_ON_ERROR(send_data1((y0 >> 8) & 0xFF), TAG, "area");
    ESP_RETURN_ON_ERROR(send_data1(y1 & 0xFF), TAG, "area");
    ESP_RETURN_ON_ERROR(send_data1((y1 >> 8) & 0xFF), TAG, "area");
    return ESP_OK;
}

static esp_err_t v1_set_pointer(int x, int y)
{
    ESP_RETURN_ON_ERROR(send_command(0x4E), TAG, "ptr");        /* RAM X counter */
    ESP_RETURN_ON_ERROR(send_data1((x >> 3) & 0xFF), TAG, "ptr");
    ESP_RETURN_ON_ERROR(send_command(0x4F), TAG, "ptr");        /* RAM Y counter */
    ESP_RETURN_ON_ERROR(send_data1(y & 0xFF), TAG, "ptr");
    ESP_RETURN_ON_ERROR(send_data1((y >> 8) & 0xFF), TAG, "ptr");
    return wait_idle(5000);
}

static esp_err_t v1_display_frame(void)
{
    ESP_RETURN_ON_ERROR(send_command(0x22), TAG, "disp");       /* DISPLAY_UPDATE_CONTROL_2 */
    ESP_RETURN_ON_ERROR(send_data1(0xC4), TAG, "disp");
    ESP_RETURN_ON_ERROR(send_command(0x20), TAG, "disp");       /* MASTER_ACTIVATION */
    ESP_RETURN_ON_ERROR(send_command(0xFF), TAG, "disp");       /* TERMINATE_FRAME_READ_WRITE */
    return wait_idle(10000);
}

/* ================= V2 (epd1in54_V2) ================= */

static esp_err_t v2_init(void)
{
    hw_reset();
    ESP_RETURN_ON_ERROR(wait_idle(5000), TAG, "v2 reset busy");
    ESP_RETURN_ON_ERROR(send_command(0x12), TAG, "v2 init");    /* SWRESET */
    ESP_RETURN_ON_ERROR(wait_idle(5000), TAG, "v2 swreset busy");

    ESP_RETURN_ON_ERROR(send_command(0x01), TAG, "v2 init");    /* Driver output control */
    ESP_RETURN_ON_ERROR(send_data1(0xC7), TAG, "v2 init");
    ESP_RETURN_ON_ERROR(send_data1(0x00), TAG, "v2 init");
    ESP_RETURN_ON_ERROR(send_data1(0x01), TAG, "v2 init");

    ESP_RETURN_ON_ERROR(send_command(0x11), TAG, "v2 init");    /* Data entry: Xinc/Ydec */
    ESP_RETURN_ON_ERROR(send_data1(0x01), TAG, "v2 init");

    ESP_RETURN_ON_ERROR(send_command(0x44), TAG, "v2 init");    /* RAM X start/end */
    ESP_RETURN_ON_ERROR(send_data1(0x00), TAG, "v2 init");
    ESP_RETURN_ON_ERROR(send_data1(0x18), TAG, "v2 init");      /* (0x18+1)*8 = 200 */

    ESP_RETURN_ON_ERROR(send_command(0x45), TAG, "v2 init");    /* RAM Y start/end */
    ESP_RETURN_ON_ERROR(send_data1(0xC7), TAG, "v2 init");      /* start = 199 */
    ESP_RETURN_ON_ERROR(send_data1(0x00), TAG, "v2 init");
    ESP_RETURN_ON_ERROR(send_data1(0x00), TAG, "v2 init");      /* end = 0 */
    ESP_RETURN_ON_ERROR(send_data1(0x00), TAG, "v2 init");

    ESP_RETURN_ON_ERROR(send_command(0x3C), TAG, "v2 init");    /* Border waveform */
    ESP_RETURN_ON_ERROR(send_data1(0x01), TAG, "v2 init");

    ESP_RETURN_ON_ERROR(send_command(0x18), TAG, "v2 init");    /* Temp sensor = internal */
    ESP_RETURN_ON_ERROR(send_data1(0x80), TAG, "v2 init");

    ESP_RETURN_ON_ERROR(send_command(0x22), TAG, "v2 init");    /* Load temp + waveform */
    ESP_RETURN_ON_ERROR(send_data1(0xB1), TAG, "v2 init");
    ESP_RETURN_ON_ERROR(send_command(0x20), TAG, "v2 init");

    ESP_RETURN_ON_ERROR(send_command(0x4E), TAG, "v2 init");    /* RAM X counter = 0 */
    ESP_RETURN_ON_ERROR(send_data1(0x00), TAG, "v2 init");
    ESP_RETURN_ON_ERROR(send_command(0x4F), TAG, "v2 init");    /* RAM Y counter = 199 */
    ESP_RETURN_ON_ERROR(send_data1(0xC7), TAG, "v2 init");
    ESP_RETURN_ON_ERROR(send_data1(0x00), TAG, "v2 init");
    return wait_idle(5000);
}

static esp_err_t v2_refresh(uint8_t ctrl2)
{
    ESP_RETURN_ON_ERROR(send_command(0x22), TAG, "v2 refresh");
    ESP_RETURN_ON_ERROR(send_data1(ctrl2), TAG, "v2 refresh");
    ESP_RETURN_ON_ERROR(send_command(0x20), TAG, "v2 refresh");
    return wait_idle(10000);
}

/* V2は y が下向きに減るので、行順を反転したコピーを作る */
static const uint8_t *fb_for_v2(void)
{
    const int row = EPAPER_WIDTH / 8;
    for (int j = 0; j < EPAPER_HEIGHT; j++) {
        memcpy(&s_rev[j * row], &s_fb[(EPAPER_HEIGHT - 1 - j) * row], row);
    }
    return s_rev;
}

/* ================= 公開API: 初期化/表示 ================= */

esp_err_t epaper_init(int panel)
{
    s_panel = panel;
    esp_err_t err = (panel == EPAPER_PANEL_V2) ? v2_init() : v1_init(lut_full_update);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "panel ready: %s", panel == EPAPER_PANEL_V2 ? "V2" : "V1");
    } else {
        ESP_LOGE(TAG, "panel init failed (%s): %s",
                 panel == EPAPER_PANEL_V2 ? "V2" : "V1", esp_err_to_name(err));
    }
    return err;
}

void epaper_sleep(void)
{
    if (s_panel == EPAPER_PANEL_V2) {
        send_command(0x10);                 /* deep sleep, check code 0x01 */
        send_data1(0x01);
        delay_ms(200);
        sc18is602_gpio_write(SC18_PIN_RST, 0);
    } else {
        send_command(0x10);                 /* DEEP_SLEEP_MODE */
        wait_idle(5000);
    }
    ESP_LOGI(TAG, "panel sleeping (image retained)");
}

esp_err_t epaper_display_full(void)
{
    if (s_panel == EPAPER_PANEL_V2) {
        const uint8_t *img = fb_for_v2();
        ESP_RETURN_ON_ERROR(send_command(0x24), TAG, "full");   /* write RAM (new) */
        ESP_RETURN_ON_ERROR(send_data_buf(img, EPAPER_FB_SIZE), TAG, "full");
        ESP_RETURN_ON_ERROR(v2_refresh(0xF7), TAG, "full");     /* full refresh */
        /* 部分更新のベースとして旧画像RAM(0x26)にも同じ絵を入れておく */
        ESP_RETURN_ON_ERROR(send_command(0x26), TAG, "full base");
        ESP_RETURN_ON_ERROR(send_data_buf(img, EPAPER_FB_SIZE), TAG, "full base");
        return ESP_OK;
    }

    /* V1: フル更新LUTで初期化し直し、2つのRAM面の両方に同じ絵を書く
       (表示のたびに面が入れ替わる仕様のため。メーカーサンプルと同じ作法) */
    ESP_RETURN_ON_ERROR(v1_init(lut_full_update), TAG, "full init");
    for (int pass = 0; pass < 2; pass++) {
        ESP_RETURN_ON_ERROR(v1_set_memory_area(0, 0, EPAPER_WIDTH - 1, EPAPER_HEIGHT - 1), TAG, "full");
        ESP_RETURN_ON_ERROR(v1_set_pointer(0, 0), TAG, "full");
        ESP_RETURN_ON_ERROR(send_command(0x24), TAG, "full");   /* WRITE_RAM */
        ESP_RETURN_ON_ERROR(send_data_buf(s_fb, EPAPER_FB_SIZE), TAG, "full");
        ESP_RETURN_ON_ERROR(v1_display_frame(), TAG, "full");
    }
    return ESP_OK;
}

esp_err_t epaper_display_window(int x, int y, int w, int h)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > EPAPER_WIDTH)  w = EPAPER_WIDTH - x;
    if (y + h > EPAPER_HEIGHT) h = EPAPER_HEIGHT - y;
    x &= ~7;
    w &= ~7;
    if (w <= 0 || h <= 0) return ESP_ERR_INVALID_ARG;

    if (s_panel == EPAPER_PANEL_V2) {
        /* V2の部分更新は「全フレームを送り、パネル側が差分だけ描く」方式
           (メーカーの DisplayPart)。RAM領域を絞るより確実なのでこれを使う */
        const uint8_t *img = fb_for_v2();
        ESP_RETURN_ON_ERROR(send_command(0x24), TAG, "window");
        ESP_RETURN_ON_ERROR(send_data_buf(img, EPAPER_FB_SIZE), TAG, "window");
        return v2_refresh(0xFF);                                /* partial refresh */
    }

    /* V1: 部分更新LUTに切り替えてから、矩形だけ書き込む */
    if (!s_v1_partial) {
        ESP_RETURN_ON_ERROR(v1_init(lut_partial_update), TAG, "window init");
    }
    int x1 = x + w - 1;
    int y1 = y + h - 1;
    ESP_RETURN_ON_ERROR(v1_set_memory_area(x, y, x1, y1), TAG, "window");
    ESP_RETURN_ON_ERROR(v1_set_pointer(x, y), TAG, "window");
    ESP_RETURN_ON_ERROR(send_command(0x24), TAG, "window");      /* WRITE_RAM */
    const int row_bytes = EPAPER_WIDTH / 8;
    const int win_bytes = w / 8;
    /* DCはHighのまま行ごとに送る (CSは行単位で区切られてよい) */
    ESP_RETURN_ON_ERROR(sc18is602_gpio_write(SC18_PIN_DC, 1), TAG, "window");
    for (int j = y; j <= y1; j++) {
        esp_err_t err = sc18is602_spi_write(&s_fb[j * row_bytes + x / 8], win_bytes);
        if (err != ESP_OK) return err;
    }
    return v1_display_frame();
}

uint8_t *epaper_framebuffer(void) { return s_fb; }

/* ================= フレームバッファ描画 ================= */

void epaper_fb_clear(int color)
{
    memset(s_fb, color ? 0xFF : 0x00, sizeof(s_fb));
}

void epaper_fb_draw_pixel(int x, int y, int color)
{
    if (x < 0 || x >= EPAPER_WIDTH || y < 0 || y >= EPAPER_HEIGHT) return;
    int idx = (x + y * EPAPER_WIDTH) / 8;
    if (color) s_fb[idx] |=  (uint8_t)(0x80 >> (x % 8));
    else       s_fb[idx] &= (uint8_t)~(0x80 >> (x % 8));
}

void epaper_fb_draw_char(int x, int y, char c, const sFONT *font, int color)
{
    if (c < ' ' || c > '~') c = ' ';
    int bytes_per_col = font->Width / 8 + (font->Width % 8 ? 1 : 0);
    unsigned int offset = (unsigned)(c - ' ') * font->Height * bytes_per_col;
    const uint8_t *ptr = &font->table[offset];
    for (int j = 0; j < font->Height; j++) {
        for (int i = 0; i < font->Width; i++) {
            if (ptr[(i / 8)] & (0x80 >> (i % 8))) {
                epaper_fb_draw_pixel(x + i, y + j, color);
            }
        }
        ptr += bytes_per_col;
    }
}

void epaper_fb_draw_string(int x, int y, const char *text, const sFONT *font, int color)
{
    int cx = x;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') { cx = x; y += font->Height + 2; continue; }
        epaper_fb_draw_char(cx, y, *p, font, color);
        cx += font->Width;
    }
}

void epaper_fb_draw_line(int x0, int y0, int x1, int y1, int color)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (1) {
        epaper_fb_draw_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void epaper_fb_draw_rect(int x0, int y0, int x1, int y1, int color)
{
    epaper_fb_draw_line(x0, y0, x1, y0, color);
    epaper_fb_draw_line(x0, y1, x1, y1, color);
    epaper_fb_draw_line(x0, y0, x0, y1, color);
    epaper_fb_draw_line(x1, y0, x1, y1, color);
}

void epaper_fb_fill_rect(int x0, int y0, int x1, int y1, int color)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int yy = y0; yy <= y1; yy++) {
        for (int xx = x0; xx <= x1; xx++) {
            epaper_fb_draw_pixel(xx, yy, color);
        }
    }
}

void epaper_fb_draw_circle(int x, int y, int radius, int color)
{
    int xp = -radius, yp = 0, err = 2 - 2 * radius;
    do {
        epaper_fb_draw_pixel(x - xp, y + yp, color);
        epaper_fb_draw_pixel(x + xp, y + yp, color);
        epaper_fb_draw_pixel(x + xp, y - yp, color);
        epaper_fb_draw_pixel(x - xp, y - yp, color);
        int e2 = err;
        if (e2 <= yp) {
            err += ++yp * 2 + 1;
            if (-xp == yp && e2 <= xp) e2 = 0;
        }
        if (e2 > xp) err += ++xp * 2 + 1;
    } while (xp <= 0);
}

void epaper_fb_fill_circle(int x, int y, int radius, int color)
{
    for (int yy = -radius; yy <= radius; yy++) {
        for (int xx = -radius; xx <= radius; xx++) {
            if (xx * xx + yy * yy <= radius * radius) {
                epaper_fb_draw_pixel(x + xx, y + yy, color);
            }
        }
    }
}

void epaper_fb_draw_bitmap(int x, int y, int w, int h, const uint8_t *bits, int color)
{
    int row_bytes = w / 8 + (w % 8 ? 1 : 0);
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            if (bits[j * row_bytes + i / 8] & (0x80 >> (i % 8))) {
                epaper_fb_draw_pixel(x + i, y + j, color);
            }
        }
    }
}
