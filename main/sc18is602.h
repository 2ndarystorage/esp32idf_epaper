/*
 * sc18is602.h - SC18IS602B (I2C -> SPIブリッジIC) ドライバ
 *
 * e-Paper I2Cモジュール上では、このICが ESP32 と E-Inkパネルの間に入る。
 *   - パネルへのSPI通信 (CS は SS0 が自動で担当)
 *   - パネルの制御線はこのICのGPIO経由:
 *       GPIO0 = CS (SS0として使用), GPIO1 = BUSY(入力),
 *       GPIO2 = DC, GPIO3 = RST
 *
 * 実装はメーカー提供サンプル (Arduino/STM32版) のレジスタ操作を
 * ESP-IDF v5.x の新I2CマスタAPIに移植し、SPI転送だけ高速化している:
 * サンプルは1バイトずつ転送+10ms待ちだったが、本ICは200バイトの
 * バッファを持つので、最大200バイトを1回のSPI転送にまとめられる。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/* SC18IS602B の GPIO番号 (= パネルの信号線) */
#define SC18_PIN_CS    0
#define SC18_PIN_BUSY  1
#define SC18_PIN_DC    2
#define SC18_PIN_RST   3

/* I2Cバス初期化 + ブリッジIC初期化 (GPIO方向: BUSYだけ入力、他は出力) */
esp_err_t sc18is602_init(int sda_gpio, int scl_gpio, uint8_t addr, int freq_hz);

/* GPIO出力 (DC / RST 用)。pin は SC18_PIN_* */
esp_err_t sc18is602_gpio_write(int pin, int value);

/* GPIO入力 (BUSY 用)。戻り値 0/1。失敗時は -1 */
int sc18is602_gpio_read(int pin);

/* SPI転送 (書き込み専用)。len は任意長可 (内部で200バイトずつ分割)。
 * CS(SS0) は転送の間だけ自動でLowになる。MISOは使わない (パネルは書き込みのみ)。 */
esp_err_t sc18is602_spi_write(const uint8_t *data, size_t len);
