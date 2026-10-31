# ESP32-WROOM + e-Paper I2Cモジュール 文字表示 (ESP-IDF)

ESP32-WROOM（初代ESP32）で、[e-Paper I2Cモジュール（スイッチサイエンス 製品コード3887 / CRESCENT-009）](https://www.switch-science.com/products/3887) に文字を表示する ESP-IDF プロジェクトです。

このモジュールの中身:

- Waveshare製 1.54インチ E-Ink パネル（200×200、白黒）
- 元はSPI接続のパネルを、**SC18IS602B（I2C→SPIブリッジIC）** でI2C化したもの
- パネルの制御線（CS/BUSY/DC/RST）はブリッジICのGPIO経由で操作する（GPIO0=CS、GPIO1=BUSY、GPIO2=DC、GPIO3=RST）
- 電源 3.3V、Groveコネクタ

メーカー提供のArduino/STM32サンプルを土台に、ESP-IDF v5.x の新I2CマスタAPIへ移植しました。サンプルはSPI転送が1バイトずつ＋10ms待ちで全画面表示に数分かかる構成でしたが、本プロジェクトではブリッジICの200バイトバッファを活かして転送をまとめ、**全画面でも数秒**で表示できるようにしています（実測は実機確認時に追記予定）。

## 配線

Groveコネクタ → ESP32-WROOM開発ボードへ4本だけ。

| モジュール (Grove) | 線色の目安 | ESP32 |
|---|---|---|
| VCC | 赤 | 3.3V |
| GND | 黒 | GND |
| SCL | 黄 | GPIO22 |
| SDA | 白 | GPIO21 |

- ピンを変えたい場合は `main/epaper_config.h` の `EPAPER_I2C_SDA_GPIO / SCL_GPIO`
- SC18IS602B のI2Cアドレスは **0x28**（7bit表記）で固定。メーカーサンプルにある `0x50` は8bit表記で同じもの

## ⚠️ 最初に確認: パネルが V1 か V2 か

このモジュールには互換性のない2世代のパネルがあります（ハード配線は共通、初期化方法が違う）。

- **基板裏面に `[V2]` マークがある → V2**
- マークが無い → V1
- 2020年4月以降に出荷されたものは V2

本プロジェクトの初期値は **V1**（`main/epaper_config.h` の `EPAPER_DEFAULT_PANEL`、手元の実機がV1だったため）。間違った世代で初期化すると、無表示になったり全面が乱れたりします。書き込み後にシリアルモニタで切り替えられるので、まず両方試すのが早いです（下記）。

## ビルドと書き込み

```bash
cd esp32-epaper-i2c
idf.py set-target esp32     # 初回のみ
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor   # ポートは環境に合わせて
```

`idf.py monitor` の終了は `Ctrl+]`。

## 動作とシリアル操作

起動すると:

1. デモ画面を表示（タイトルバー、「Hello, e-Paper!」、枠と円の描画サンプル）
2. 起動後の経過時間 `HH:MM:SS` を1秒ごとに**部分リフレッシュ**で更新（全面がチラつかない）
3. 約1時間に1回だけ全画面リフレッシュして残像をリセット

シリアルモニタ（115200bps）で文字列＋Enterを送ると、その文字列を画面いっぱいに表示します。コマンド:

| 入力 | 動作 |
|---|---|
| `Hello!` など任意の文字列 | その文字列を大きく中央表示（長さでフォント自動選択） |
| `!v1` / `!v2` | パネル世代を切り替えて再表示（再ビルド不要） |
| `!demo` | デモ画面（カウンタ表示）に戻る |
| `!clear` | 白紙にしてパネルをスリープ（電子ペーパーは無電源でも表示が残る） |

表示できる文字は **ASCII（半角英数・記号）のみ** です。日本語については次節。

## 自分のプログラムから使う

ドライバは「フレームバッファに描いてから一括表示」という形です。

```c
#include "sc18is602.h"
#include "epaper.h"
#include "epaper_config.h"

// 初期化 (最初に1回)
sc18is602_init(EPAPER_I2C_SDA_GPIO, EPAPER_I2C_SCL_GPIO,
               SC18IS602_I2C_ADDR, EPAPER_I2C_FREQ_HZ);
epaper_init(EPAPER_PANEL_V1);          // 裏面に [V2] がある個体は EPAPER_PANEL_V2

// 描画 (200x200, 原点=左上)
epaper_fb_clear(EPAPER_WHITE);
epaper_fb_draw_string(10, 10, "Hello!", &Font24, EPAPER_BLACK);
epaper_fb_draw_rect(0, 0, 199, 199, EPAPER_BLACK);
epaper_fb_draw_line(0, 100, 199, 100, EPAPER_BLACK);
epaper_fb_fill_circle(170, 170, 12, EPAPER_BLACK);

// 表示に反映
epaper_display_full();                 // 全画面リフレッシュ (数秒)
// 一部だけ変えたあとは部分リフレッシュが速い:
epaper_display_window(0, 0, 200, 40);  // x, y, w, h (x/w は8の倍数に丸められる)

epaper_sleep();                        // 表示を残したままスリープ
```

フォントは `Font8 / Font12 / Font16 / Font24` の4種（メーカーサンプル同梱のもの）。他にも `epaper_fb_draw_pixel / draw_char / draw_circle / fill_rect / draw_bitmap` があります。詳細は `main/epaper.h` のコメント参照。

## 日本語を表示したい場合

フォントがASCIIのみのため、日本語はPCでビットマップ化して送ります。変換ツールを同梱しています:

```bash
pip install Pillow
python3 tools/make_bitmap.py "こんにちは" --size 32 --out text_bitmap.h
# 生成された text_bitmap.h を main/ にコピーして:
```

```c
#include "text_bitmap.h"
epaper_fb_draw_bitmap(10, 80, TEXT_BITMAP_WIDTH, TEXT_BITMAP_HEIGHT,
                      TEXT_BITMAP_DATA, EPAPER_BLACK);
epaper_display_full();
```

プレビューPNG（`text_bitmap.h.png`）も一緒に出力されるので、書き込む前に見た目を確認できます。

## トラブルシューティング

| 症状 | 確認すること |
|---|---|
| ログに `no response at I2C addr 0x28` | 配線（SDA/SCLの入れ替わり・3.3V・GND）とGroveコネクタの向き |
| 起動ログは正常だが無表示・表示が乱れる | パネル世代違いの可能性大。モニタで `!v1` / `!v2` を試す |
| `BUSY timeout` が出続ける | 世代違い、またはパネルのリボンケーブル抜けかけ |
| 部分更新で前の絵がうっすら残る | 電子ペーパーの性質。時々の全画面リフレッシュで消える（デモは1時間ごとに実施。気になる場合は表示切替のたびに `epaper_display_full()` を使う） |
| 表示が上下逆さま | V1/V2の行順処理は本ドライバで吸収済みのはず。逆になる場合は世代設定が逆の可能性 |

## 実装メモ

- **土台**: メーカー公式リポジトリ [meerstern/e-Paper_I2C_Module](https://github.com/meerstern/e-Paper_I2C_Module) のArduinoサンプル（V1: `epd1in54`、V2: `epd1in54_V2`、SC18IS602ドライバ、`epdpaint`、STMフォント）。初期化シーケンス・LUT・レジスタ値（SC18IS602B: 設定0xF0=0x00、GPIO有効0xF6=0x0E、方向0xF7=0x59）はサンプルと一致させてある
- **高速化**: SC18IS602B は200バイトのデータバッファを持ち、I2CのSTOP条件でSPI送信が始まる仕様（NXPデータシート Rev.7）。そのため (1) データ列を最大200バイトずつ一括転送、(2) DC切替はバイトごとではなくデータ列ごとに1回、(3) サンプルの固定10ms待ちをやめ、SPI送信時間ぶんの待ち＋NACK時のリトライに変更
- **V1の作法**: パネル内のRAM面は表示のたびに切り替わるため、全画面表示では同じ絵を2回書く。部分更新は専用LUTに切り替えてから矩形だけ書く（[`epd1in54` サンプルの手順どおり）
- **V2の作法**: 初期化はOTP波形ロード型。全画面表示後に旧画像RAM（0x26）へも同じ絵を入れ、以降の部分更新（0x24へ全フレーム送信＋差分リフレッシュ）が正しく効くようにしている。V2はRAM書き込み方向が下向き（Y減少）なので、送信時に行順を反転してV1と同じ見た目に揃えている

## 検証状況

- ESP-IDF **v5.4.2** / ターゲット esp32 でビルド確認済み（`.bin` の生成まで）
- 手元の実機は **V1** パネルと確認済み（2026-10-31）のため初期値をV1に設定。表示動作の実機確認はこれから。初回はシリアルログで `bridge ready` → `panel ready: V1` が出るかを確認してください

## ファイル構成

```
esp32-epaper-i2c/
├── CMakeLists.txt / sdkconfig.defaults
├── README.md
├── main/
│   ├── CMakeLists.txt
│   ├── main.c            デモ (画面描画・カウンタ・シリアル受付)
│   ├── epaper_config.h   ピン・アドレス・初期パネル世代の設定
│   ├── epaper.c / .h     パネルドライバ (V1/V2両対応) + フレームバッファ描画API
│   ├── sc18is602.c / .h  SC18IS602B ブリッジドライバ
│   ├── fonts.h, font8/12/16/24.c   メーカーサンプル同梱フォント (PROGMEM除去のみ)
└── tools/
    └── make_bitmap.py    日本語テキスト → 1bitビットマップCヘッダ変換
```
