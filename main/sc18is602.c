/*
 * sc18is602.c - SC18IS602B ドライバ実装 (ESP-IDF v5.x 新I2CマスタAPI)
 *
 * レジスタマップ (NXP SC18IS602B データシート / メーカーサンプルより):
 *   0xF0  SPI設定        0x00 = MSB first, SPI mode 0, クロック 1.843MHz
 *   0xF1  割り込みクリア
 *   0xF4  GPIOデータ出力
 *   0xF5  GPIOデータ入力 (このIDを送った直後のI2Cリードで値が返る)
 *   0xF6  GPIO有効       0x0E = GPIO0だけSS0(SPI用)にし、GPIO1..3をGPIOにする
 *   0xF7  GPIO方向       0x59 = GPIO0,2,3: 出力(01), GPIO1: 入力(10)
 *                        (各ピン2bit: 00=準双方向 01=push-pull出力 10=入力 11=opendrain)
 * SPI転送のファンクションIDは 0x01 (SS0を使う) + データ列。
 * データは一旦200バイトの内部バッファに溜まり、I2CのSTOP条件で
 * SPI送信が始まる。そのため1回の転送は200バイトが上限。
 *
 * 注意: SPI送信中に次のI2C書き込みをするとICはACKを返さないことが
 * ある (メーカーのArduinoサンプルもACKが返るまでリトライしている)。
 * ここでも全書き込みを短いリトライ付きにしている。
 */
#include "sc18is602.h"
#include <string.h>
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SC18IS602";

#define REG_CONFIG     0xF0
#define REG_GPIO_WRITE 0xF4
#define REG_GPIO_READ  0xF5
#define REG_GPIO_EN    0xF6
#define REG_GPIO_DIR   0xF7

#define FUNC_SPI_SS0   0x01    /* SS0 でSPI転送するファンクションID */
#define SPI_BUF_MAX    200     /* SC18IS602B のデータバッファ長 */

static i2c_master_dev_handle_t s_dev = NULL;
static uint8_t s_gpio_out = 0x00;   /* GPIO出力レジスタのシャドウ */

/* ACKが返るまで少し待ってリトライするI2C書き込み */
static esp_err_t i2c_write_retry(const uint8_t *buf, size_t len)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 100; i++) {
        err = i2c_master_transmit(s_dev, buf, len, 50);
        if (err == ESP_OK) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return err;
}

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_write_retry(buf, sizeof(buf));
}

esp_err_t sc18is602_init(int sda_gpio, int scl_gpio, uint8_t addr, int freq_hz)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) return err;

    /* まず在否を確認する (配線ミスの切り分けが格段に楽になる) */
    err = i2c_master_probe(bus, addr, 200);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no response at I2C addr 0x%02X - check wiring / 3.3V", addr);
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = freq_hz,
    };
    err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) return err;

    /* メーカーサンプルの ResetDevice() と同じ初期値 */
    ESP_RETURN_ON_ERROR(write_reg(REG_CONFIG, 0x00), TAG, "config");
    s_gpio_out = 0x00;
    ESP_RETURN_ON_ERROR(write_reg(REG_GPIO_WRITE, s_gpio_out), TAG, "gpio out");
    ESP_RETURN_ON_ERROR(write_reg(REG_GPIO_EN, 0x0E), TAG, "gpio enable");
    ESP_RETURN_ON_ERROR(write_reg(REG_GPIO_DIR, 0x59), TAG, "gpio dir");
    ESP_LOGI(TAG, "bridge ready (addr=0x%02X, SDA=%d SCL=%d)", addr, sda_gpio, scl_gpio);
    return ESP_OK;
}

esp_err_t sc18is602_gpio_write(int pin, int value)
{
    if (value) s_gpio_out |= (uint8_t)(1u << pin);
    else       s_gpio_out &= (uint8_t)~(1u << pin);
    return write_reg(REG_GPIO_WRITE, s_gpio_out);
}

int sc18is602_gpio_read(int pin)
{
    /* メーカーサンプルと同じ手順: F5 + ダミー(0x0F) を書いてから1バイト読む */
    uint8_t wbuf[2] = { REG_GPIO_READ, 0x0F };
    esp_err_t err = i2c_write_retry(wbuf, sizeof(wbuf));
    if (err != ESP_OK) return -1;
    uint8_t val = 0;
    err = i2c_master_receive(s_dev, &val, 1, 100);
    if (err != ESP_OK) return -1;
    return (val >> pin) & 0x01;
}

esp_err_t sc18is602_spi_write(const uint8_t *data, size_t len)
{
    uint8_t chunk[1 + SPI_BUF_MAX];
    chunk[0] = FUNC_SPI_SS0;
    while (len > 0) {
        size_t n = len > SPI_BUF_MAX ? SPI_BUF_MAX : len;
        memcpy(chunk + 1, data, n);
        esp_err_t err = i2c_write_retry(chunk, n + 1);
        if (err != ESP_OK) return err;
        /* SPI送信 (1.843MHz) が終わるまで待つ: nバイト=約 n*4.4us + 余裕 */
        esp_rom_delay_us((unsigned)(n * 6 + 300));
        data += n;
        len -= n;
    }
    return ESP_OK;
}
