/*
 * e-Paper Text Display for ESP32-WROOM
 *
 * ESP32-WROOM + e-Paper I2Cモジュール (スイッチサイエンス 3887) で
 * 文字を表示するデモ。
 *
 * 動作:
 *  1. 起動するとデモ画面 (タイトル/説明/枠/円) を全画面表示
 *  2. 以後は経過時間 HH:MM:SS だけを1秒ごとに部分更新
 *  3. シリアルモニタ (UART0, 115200bps) で行を送ると、その文字列を大きく表示
 *     コマンド:
 *       !v1 / !v2   パネル世代を切り替えて再表示 (裏面の [V2] マーク有無と合わせる)
 *       !demo       デモ画面に戻る
 *       !clear      白紙にしてスリープ
 *  文字は ASCII のみ (フォントの制約)。日本語は README のビットマップ手順で。
 *
 * 配線 (Grove): 赤=3.3V, 黒=GND, 黄=SCL(GPIO22), 白=SDA(GPIO21)
 * 対象: ESP-IDF v5.x / ESP32-WROOM
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "epaper.h"
#include "sc18is602.h"
#include "epaper_config.h"

static const char *TAG = "MAIN";

static SemaphoreHandle_t s_epd_lock;         /* パネル操作の排他 */
static volatile int s_panel = EPAPER_DEFAULT_PANEL;
static volatile bool s_demo_mode = true;     /* false の間はカウンタ更新を止める */
static char s_message[64] = "";              /* シリアルで受けた表示文字列 */

/* カウンタ表示エリア (x, w は8の倍数に揃える) */
#define COUNTER_X 32
#define COUNTER_Y 118
#define COUNTER_W 136   /* Font24 (幅17) x 8文字 = 136 */
#define COUNTER_H 28

static void draw_demo_screen(void)
{
    epaper_fb_clear(EPAPER_WHITE);
    /* タイトルバー (黒帯 + 白文字) */
    epaper_fb_fill_rect(0, 0, 199, 34, EPAPER_BLACK);
    epaper_fb_draw_string(10, 9, "ESP32-WROOM", &Font16, EPAPER_WHITE);
    epaper_fb_draw_string(140, 12, "e-Paper", &Font12, EPAPER_WHITE);
    /* 本文 */
    epaper_fb_draw_string(18, 48, "Hello, e-Paper!", &Font16, EPAPER_BLACK);
    epaper_fb_draw_string(10, 76, "1.54 inch 200x200", &Font12, EPAPER_BLACK);
    epaper_fb_draw_string(10, 92, "I2C via SC18IS602B", &Font12, EPAPER_BLACK);
    /* カウンタのラベルと枠 */
    epaper_fb_draw_string(10, 122, "UP:", &Font12, EPAPER_BLACK);
    epaper_fb_draw_rect(COUNTER_X - 4, COUNTER_Y - 4,
                        COUNTER_X + COUNTER_W + 3, COUNTER_Y + COUNTER_H + 3,
                        EPAPER_BLACK);
    /* フッタ */
    epaper_fb_draw_line(0, 168, 199, 168, EPAPER_BLACK);
    epaper_fb_draw_string(10, 176, "Type text + Enter", &Font12, EPAPER_BLACK);
    epaper_fb_draw_string(10, 190, "on serial monitor", &Font8, EPAPER_BLACK);
    epaper_fb_fill_circle(184, 158, 6, EPAPER_BLACK);
    epaper_fb_draw_circle(166, 158, 6, EPAPER_BLACK);
}

static void draw_counter(bool full)
{
    char buf[16];
    long long sec = esp_timer_get_time() / 1000000;
    snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld",
             (sec / 3600) % 100, (sec / 60) % 60, sec % 60);
    /* カウンタ領域だけ白で消してから描き直す */
    epaper_fb_fill_rect(COUNTER_X, COUNTER_Y,
                        COUNTER_X + COUNTER_W - 1, COUNTER_Y + COUNTER_H - 1,
                        EPAPER_WHITE);
    epaper_fb_draw_string(COUNTER_X, COUNTER_Y + 2, buf, &Font24, EPAPER_BLACK);
    if (full) {
        epaper_display_full();
    } else {
        epaper_display_window(COUNTER_X, COUNTER_Y, COUNTER_W, COUNTER_H);
    }
}

static void draw_message_screen(const char *text)
{
    epaper_fb_clear(EPAPER_WHITE);
    epaper_fb_draw_rect(0, 0, 199, 199, EPAPER_BLACK);
    epaper_fb_draw_string(10, 10, "MESSAGE:", &Font12, EPAPER_BLACK);
    epaper_fb_draw_line(0, 28, 199, 28, EPAPER_BLACK);

    /* 長さに応じて入る最大のフォントを選ぶ (入りきらない分は切り捨て) */
    const sFONT *font = &Font24;
    size_t len = strlen(text);
    if (len * Font24.Width > 192) font = &Font16;
    if (len * font->Width > 192) font = &Font12;
    if (len * font->Width > 192) font = &Font8;
    char disp[64];
    size_t max_chars = 192 / (size_t)font->Width;
    if (len > max_chars) len = max_chars;
    memcpy(disp, text, len);
    disp[len] = '\0';
    text = disp;
    int text_w = (int)len * font->Width;
    int x = (200 - text_w) / 2;
    if (x < 4) x = 4;
    int y = (200 - font->Height) / 2;
    epaper_fb_draw_string(x, y, text, font, EPAPER_BLACK);

    epaper_fb_draw_string(10, 178, "!demo = back", &Font12, EPAPER_BLACK);
    epaper_display_full();
}

/* 1秒ごとのカウンタ部分更新 */
static void counter_task(void *arg)
{
    int tick = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!s_demo_mode) continue;
        if (xSemaphoreTake(s_epd_lock, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        draw_counter(false);
        xSemaphoreGive(s_epd_lock);
        /* 安全のため時々 (約1時間ごと) 全画面更新で残像をリセット */
        if (++tick >= 3600) {
            tick = 0;
            if (xSemaphoreTake(s_epd_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (s_demo_mode) { draw_demo_screen(); draw_counter(true); }
                xSemaphoreGive(s_epd_lock);
            }
        }
    }
}

static void handle_line(const char *line)
{
    if (strcmp(line, "!v1") == 0 || strcmp(line, "!v2") == 0) {
        s_panel = (line[2] == '1') ? EPAPER_PANEL_V1 : EPAPER_PANEL_V2;
        ESP_LOGI(TAG, "switch panel to V%c and redraw", line[2]);
        xSemaphoreTake(s_epd_lock, portMAX_DELAY);
        if (epaper_init(s_panel) == ESP_OK) {
            if (s_demo_mode) { draw_demo_screen(); draw_counter(true); }
            else             { draw_message_screen(s_message); }
        }
        xSemaphoreGive(s_epd_lock);
        return;
    }
    if (strcmp(line, "!demo") == 0) {
        s_demo_mode = true;
        xSemaphoreTake(s_epd_lock, portMAX_DELAY);
        epaper_init(s_panel);
        draw_demo_screen();
        draw_counter(true);
        xSemaphoreGive(s_epd_lock);
        return;
    }
    if (strcmp(line, "!clear") == 0) {
        s_demo_mode = false;
        xSemaphoreTake(s_epd_lock, portMAX_DELAY);
        epaper_fb_clear(EPAPER_WHITE);
        epaper_display_full();
        epaper_sleep();
        xSemaphoreGive(s_epd_lock);
        ESP_LOGI(TAG, "cleared and sleeping. send !demo or text to wake");
        return;
    }
    /* それ以外は表示文字列として扱う */
    strncpy(s_message, line, sizeof(s_message) - 1);
    s_message[sizeof(s_message) - 1] = '\0';
    s_demo_mode = false;
    xSemaphoreTake(s_epd_lock, portMAX_DELAY);
    epaper_init(s_panel);   /* スリープ復帰も兼ねる (初期化で起きる) */
    draw_message_screen(s_message);
    xSemaphoreGive(s_epd_lock);
}

static void uart_task(void *arg)
{
    char line[64];
    int pos = 0;
    uint8_t ch;
    ESP_LOGI(TAG, "type text + Enter to display / !v1 !v2 !demo !clear");
    while (1) {
        int len = uart_read_bytes(UART_NUM_0, &ch, 1, pdMS_TO_TICKS(100));
        if (len <= 0) continue;
        if (ch == '\n' || ch == '\r') {
            if (pos > 0) {
                line[pos] = '\0';
                handle_line(line);
                pos = 0;
            }
        } else if (pos < (int)sizeof(line) - 1) {
            line[pos++] = (char)ch;
        }
    }
}

void app_main(void)
{
    s_epd_lock = xSemaphoreCreateMutex();

    esp_err_t err = sc18is602_init(EPAPER_I2C_SDA_GPIO, EPAPER_I2C_SCL_GPIO,
                                   SC18IS602_I2C_ADDR, EPAPER_I2C_FREQ_HZ);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SC18IS602 init failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "wiring: 3.3V / GND / SDA=GPIO%d / SCL=GPIO%d",
                 EPAPER_I2C_SDA_GPIO, EPAPER_I2C_SCL_GPIO);
        return;
    }
    err = epaper_init(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "e-Paper init failed. If the panel is the other version,");
        ESP_LOGE(TAG, "type !v1 or !v2 on the serial monitor to try the other driver.");
        /* パネル応答がなくてもUART受付は生かしておく (切替で復帰できる) */
    } else {
        draw_demo_screen();
        draw_counter(true);
        ESP_LOGI(TAG, "demo screen shown (panel V%d)", s_panel);
    }

    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    xTaskCreate(counter_task, "counter", 4096, NULL, 5, NULL);
    xTaskCreate(uart_task, "uart", 4096, NULL, 5, NULL);
}
