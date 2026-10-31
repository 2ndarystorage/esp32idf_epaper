/*
 * epaper_config.h - このプロジェクトの全体設定はここだけ変更すればよい
 *
 * 対象ハード:
 *   ESP32-WROOM (初代ESP32) + e-Paper I2Cモジュール
 *   (スイッチサイエンス製品コード 3887 / CRESCENT-009)
 *   = Waveshare 1.54インチ 200x200 E-Ink + SC18IS602B (I2C->SPIブリッジ)
 */
#pragma once

/* ---- I2C 配線 (Groveコネクタ) ---- */
#define EPAPER_I2C_SDA_GPIO   21
#define EPAPER_I2C_SCL_GPIO   22
#define EPAPER_I2C_FREQ_HZ    400000   /* SC18IS602B の上限は 400kHz */

/*
 * SC18IS602B の I2Cアドレス (7bit)。
 * モジュールのアドレスピンは A2=A1=A0=0 固定なので 0x28。
 * (メーカーサンプル中の表記 0x50 は 8bit表記で、同じものを指す)
 */
#define SC18IS602_I2C_ADDR    0x28

/* ---- パネル世代の初期値 ----
 * 基板裏面に [V2] マークがあるものは V2、無ければ V1。
 * 2020年4月以降に出荷されたモジュールは V2。
 * ここはあくまで起動時の初期値で、書き込み後にシリアルモニタで
 *   !v1 / !v2
 * と打てば再ビルドなしで切り替えて試せる (README参照)。
 */
#define EPAPER_PANEL_V1  1
#define EPAPER_PANEL_V2  2
#define EPAPER_DEFAULT_PANEL  EPAPER_PANEL_V2
