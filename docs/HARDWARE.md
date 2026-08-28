# CoreS3 SE ハードウェア調査結果

SPEC §4.0 の指示に従い、推測を排して一次情報から確定させた事項をここに記録する。
実装（特に `components/cores3se_bsp/`）はこの表を根拠とする。

## 出典

| # | 参照先 | 用途 |
|---|---|---|
| 1 | `m5stack/M5GFX` `src/M5GFX.cpp` (master) | **電源シーケンス・LCD・バックライト・タッチの確定情報** |
| 2 | `m5stack/M5Unified` `src/M5Unified.cpp` (master) | I2C/SPI/I2S ピン割り当て、CoreS3 と SE の差分 |
| 3 | 公式ドキュメント https://docs.m5stack.com/ja/core/M5CoreS3%20SE | 概要確認 |

M5GFX の CoreS3 自動検出ブロック（`M5GFX.cpp` の `CONFIG_IDF_TARGET_ESP32S3` / `pkg_ver == 0` 節）が、
動作実績のある初期化コードとして最も信頼できる。以下はそこから読み取った実値である。

## 1. 電源投入シーケンス（最重要）

I2C: **I2C ポート = SDA GPIO12 / SCL GPIO11**。以下の順で実行する。

### 1-1. AW9523B (0x58) — IO エキスパンダ

まず ID レジスタ `0x10` を読み **0x23** が返ることで存在確認する。

| レジスタ | 値 | 意味 |
|---|---|---|
| `0x02` | `0b00000111` または `0b00000101` を **bit set** | Output Port 0。P0_0=TOUCH_RST, P0_1=BOOST/USB_OTG_EN, P0_2=BUS_OUT_EN |
| `0x03` | `0b10000011` または `0b00000011` を **bit set** | Output Port 1。**P1_1 = LCD_RST**（HIGH でリセット解除） |
| `0x04` | `0b00011000` | CONFIG_P0（1=入力）。P0_3, P0_4 が入力、他は出力 |
| `0x05` | `0b00001100` | CONFIG_P1（1=入力）。**P1_2 = TOUCH_INT**, P1_3 が入力 |
| `0x11` | `0b00010000` | GCR。Port0 を Push-Pull モードにする |
| `0x12` | `0xFF` | LEDMODE_P0（全ビット1 = GPIO モード、LED電流モードではない） |
| `0x13` | `0xFF` | LEDMODE_P1 |

`0x02` / `0x03` の値が 2 通りあるのは M5GFX の分岐による:
GPIO35/36/37 を input_pullup にして読み、**3本とも 0**（＝SPIバスのプルアップが効いていない）なら
VBUS 5V 出力を有効化するため `0x02=0b111` / `0x03=0b10000011` を、そうでなければ
`0x02=0b101` / `0x03=0b00000011` を使う。USBホストモジュール等で5Vが出ていないと
信号線が電気を吸い込む組み合わせがあるための対処。本プロジェクトは外部モジュールを
使わないので後者（`0b101` / `0b00000011`）で足りるが、M5GFX と同じ判定を実装しておく。

### 1-2. AXP2101 (0x34) — PMIC

| レジスタ | 値 | 意味 |
|---|---|---|
| `0x90` | `0xBF` | LDOS ON/OFF control 0。ALDO1-4, BLDO1-2, **DLDO1(bit7)** を ON |
| `0x94` | `28` (= 33-5) | ALDO3 = 3.3V（無印CoreS3のGC0308カメラ用。SEには無いが無害） |
| `0x95` | `28` (= 33-5) | ALDO4 = 3.3V（TFカードスロット用） |

電圧値の式: `V = 0.5V + 0.1V * N` なので `N=28` → 3.3V。

### 1-3. バックライト = AXP2101 **DLDO1**

SPEC §4.2 の「PMIC の LDO 経由」は **DLDO1** で確定。

| レジスタ | 意味 |
|---|---|
| `0x90` bit7 | DLDO1 enable / disable |
| `0x99` | DLDO1 電圧設定 = 輝度 |

M5GFX の輝度マッピング: `reg0x99 = (brightness + 641) >> 5`（brightness は 0-255）。
- brightness=1   → 20 → 2.5V（最小）
- brightness=255 → 28 → 3.3V（最大）

つまり実効的な輝度レンジは **DLDO1 = 2.5V〜3.3V の 9 段階**しかない。
SPEC §1.4 の夜間減光はこの範囲で行う。brightness=0 のときのみ DLDO1 を OFF する。

## 2. LCD

| 項目 | 値 |
|---|---|
| コントローラ | **ILI9342C** または **ILI9342E**（個体差あり・§2-1参照） |
| パネルID | `_read_panel_id() & 0xFF == 0xE3` |
| SPI ホスト | SPI2_HOST |
| MOSI / SCLK | GPIO37 / GPIO36 |
| CS | GPIO3 |
| DC | GPIO35（**MISO と共用**） |
| RST | AW9523B P1_1（レジスタ `0x03` bit1） |
| SPI mode | 0、3-wire |
| 書き込み周波数 | 40 MHz（読み出し 16 MHz） |
| 色反転 | **invert = true**（必須） |
| offset_rotation | 3 / デフォルト rotation = 1（横向き） |

### 2-1. ILI9342C / ILI9342E の判別（表示が崩れたらここを疑う）

M5GFX は **タッチICのファームウェアIDでパネル世代を判別**している（LCD側からは読めないため）。
FT5x06 (0x38) に対し:

1. レジスタ `0x00` に `0x00` を書く（work mode）
2. `0xA3`=CIPHER, `0xA6`=FIRMID, `0xA8`=VENDID を読む（I2C 100kHz）
3. VENDID == `0x11`（M5Stack）であることを確認
4. FIRMID == `0x10` → **ILI9342C** / FIRMID == `0x12` → **ILI9342E**

ILI9342E の場合のみ追加の初期化コマンド列が必要（M5GFX `getIli9342EInitCommands()` 参照）。
本機の実測値はマイルストーン1のI2Cスキャン時にログ出力して確定させる。

### 2-2. GPIO35 共用問題

M5GFX は CS の上げ下げのたびに GPIO35 の出力機能を
`FSPIQ_OUT_IDX`(MISO) ⇔ `SIG_GPIO_OUT_IDX`(GPIO出力=DC) で切り替えている。
**本プロジェクトは microSD を使わない**ため、GPIO35 は常時 LCD DC 専用の
通常 GPIO 出力として扱えばよく、この切り替えは不要。SDドライバは初期化しないこと。

## 3. タッチ

| 項目 | 値 |
|---|---|
| IC | FT6336U（FT5x06 互換ドライバで動作） |
| I2C アドレス | 0x38 |
| 解像度 | 320 x 240 |
| RST | AW9523B P0_0 |
| INT | AW9523B P1_2 → AW9523B INTN → **GPIO21** |

INT は AW9523B 経由で GPIO21 に集約されている。M5Unified は GPIO21 をエッジ検出に使い、
読み出し後に AW9523B のレジスタ `0x00` と `0x01` を読んで INT をクリアしている
（AW9523B は「入力の変化」しか報告しないため、クリアを怠ると次のタッチを取りこぼす）。

**本プロジェクトはポーリング方式で実装する**（SPEC §4.2 の指示どおり。LVGL の入力デバイスは
既定でポーリングなので割り込みは不要で、上記のクリア漏れ問題も回避できる）。

## 4. I2S（オプションのビープ音用）

M5Unified の CoreS3 / CoreS3SE の設定値:

| 信号 | GPIO | 備考 |
|---|---|---|
| MCLK | 0 | |
| BCK | 34 | |
| WS (LRCK) | 33 | |
| DOUT (→ AW88298 スピーカー) | **13** | |
| DIN (← ES7210 マイク) | **14** | |
| I2S ポート | I2S_NUM_1 | |

> **注意:** SPEC §4.2 の表は DOUT=14 / DIN=13 となっているが、M5Unified の実コードでは
> **DOUT=13 / DIN=14 で逆**である。SPEC §4.2 自身が「公式ドキュメント内でも記載に揺れがある」と
> 断っているため、実装は一次情報である M5Unified 側（DOUT=13）を採用する。

## 5. その他のピン

| 用途 | ピン |
|---|---|
| 内部 I2C | SDA=12 / SCL=11 |
| Grove Port A（外部 I2C・本プロジェクト未使用） | SDA=2 / SCL=1 |
| microSD CS（本プロジェクト未使用） | GPIO4 |

## 6. CoreS3 と CoreS3 SE の差分

M5GFX は **GC0308 カメラ (I2C) が見つからないこと**をもって SE と判定している。
つまり SE で確実に無いのはカメラ。SPEC §4.4 の他の項目（IMU, 地磁気, 近接センサ, 内蔵バッテリー）も
参照するコードを書かない。

**マイクロフォン(ES7210)とスピーカーアンプ(AW88298)の実装有無は、M5Unified のコード上では
CoreS3 と SE で分岐していない**（同じ設定を共有している）ため、SE にも載っている可能性が高い。
ただし断定はせず、**マイルストーン1の I2C スキャン実測結果を正とする**。
音声は SPEC §1.4 のビープのみの補助機能なので、存在しなければ無効化してビルドを通す。

## 7. 実機の実測情報

| 項目 | 値 |
|---|---|
| USB | VID `0x303a` / PID `0x1001` = ESP32-S3 内蔵 USB-Serial-JTAG |
| シリアルポート (macOS) | `/dev/cu.usbmodem101` |
| MAC | `48:27:E2:7A:3B:B0` |

**esptool の自動リセットが効かない。** `--before default_reset` / `--before usb_reset` の
どちらでも `No serial data received` になる。書き込みのたびに手動でダウンロードモードに
入れる必要がある（底面 RST を3秒長押し → 緑インジケータ点灯 → 離す）。README 参照。

## 8. I2C スキャン実測結果

（マイルストーン1で実機から取得して記入する）
