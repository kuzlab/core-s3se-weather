# 実装仕様書: 洗濯物判断用 天気表示端末

**ターゲット:** M5Stack CoreS3 SE (ESP32-S3)
**フレームワーク:** ESP-IDF v5.3 以降
**言語:** C
**目的:** 千葉県浦安市の1時間ごとの降水予測を取得し、洗濯物を「干す / 取り込む」判断に使える形で常時表示する

---

## 0. このドキュメントの使い方

あなた（Claude Code）はこのリポジトリをゼロから作る。以下を順に読んでから着手すること。

1. §1〜§3 で何を作るかを把握する
2. §4 のハードウェア制約は**推測で埋めず、必ず一次情報を確認する**（§4.0 参照）
3. §9 のマイルストーン順に実装し、各マイルストーンで実機確認できる状態にする

不明点があれば実装を進める前に質問すること。特に電源シーケンス周りを推測で書くと画面が一切点灯せず、原因の切り分けに非常に時間がかかる。

---

## 1. 機能要件

### 1.1 中核機能

固定地点（浦安市）の気象データを定期取得し、以下を画面に常時表示する。

| 表示要素 | 内容 |
|---|---|
| 判定バナー | 4段階の判定を色分けした大きな帯で表示 |
| サマリ文 | 「15:00〜18:00 2.4mm 最大80%」「あと約4時間で降り出します」 |
| 時系列グラフ | 直近12または24時間の1時間ごと降水量と降水確率 |
| ヘッダ | 地点名、最終更新時刻、取得成否 |

### 1.2 判定ロジック

「その時間が雨か」の定義:

```
is_rainy(slot) := (slot.precipitation_mm >= RAIN_MM) || (slot.pop_percent >= RAIN_POP)
```

現在時刻を含むスロットから前方24時間を走査し、最初に `is_rainy` となるスロットまでの時間 `h` で判定する。

| 条件 | 判定 | 表示文言 | 色 |
|---|---|---|---|
| `h == 0` | `V_RAINING` | 取り込め | 赤 |
| `1 <= h <= BRING_IN_H` | `V_BRING_IN` | まもなく取り込め | 橙 |
| `BRING_IN_H < h <= CAUTION_H` | `V_CAUTION` | 短時間なら可 | 黄 |
| それ以外 / 24時間雨なし | `V_OK` | 干してOK | 緑 |

降雨ブロック（連続する `is_rainy` スロット）について、開始時刻・終了時刻・合計降水量・最大降水確率を算出してサマリ文に使う。

**ナウキャストによる上書き（§2.2 を有効化した場合のみ）:** 直近60分のレーダー降水強度の最大値が `NOWCAST_MM_H` 以上なら、予報側の判定に関わらず最低でも `V_BRING_IN` に引き上げる。レーダー実況は予報モデルより直近の精度が高いため。

### 1.3 しきい値（すべて Kconfig で設定可能にすること）

| 名前 | 既定値 | 意味 |
|---|---|---|
| `RAIN_MM` | 0.2 | これ以上/時を雨とみなす [mm] |
| `RAIN_POP` | 50 | これ以上の降水確率を雨とみなす [%] |
| `NOWCAST_MM_H` | 0.5 | ナウキャスト警戒しきい値 [mm/h] |
| `BRING_IN_H` | 2 | 何時間以内の雨で「取り込め」とするか |
| `CAUTION_H` | 5 | 何時間以内の雨で「短時間なら可」とするか |
| `REFRESH_SEC` | 600 | 予報の再取得間隔 |
| `NOWCAST_SEC` | 300 | ナウキャストの再取得間隔 |

これらは運用しながら追い込む前提の値である。ハードコードせず `idf.py menuconfig` から変更できること。

### 1.4 操作

- **画面タップ:** グラフ表示レンジを 24時間 ⇔ 12時間 でトグルし、同時に即時再取得を行う
- **判定悪化時:** `V_OK`/`V_CAUTION` から `V_BRING_IN`/`V_RAINING` に遷移した瞬間のみ、スピーカーで短いビープを2音鳴らす。同じ状態が続く間は鳴らさない（鳴りっぱなしは実用性を殺す）
- **夜間減光:** 22:00〜06:00 はバックライト輝度を下げる

---

## 2. 外部API

### 2.1 Open-Meteo（必須・APIキー不要）

```
GET https://api.open-meteo.com/v1/forecast
  ?latitude=35.6536
  &longitude=139.9019
  &hourly=precipitation,precipitation_probability
  &timezone=Asia%2FTokyo
  &timeformat=unixtime
  &forecast_days=2
  &models=best_match
```

レスポンス（`timeformat=unixtime` 指定時）:

```json
{
  "utc_offset_seconds": 32400,
  "hourly": {
    "time": [1756339200, 1756342800, ...],
    "precipitation": [0.0, 0.0, 0.3, ...],
    "precipitation_probability": [0, 5, 40, ...]
  }
}
```

**重要な注意点:**

- `timeformat=unixtime` は必須。ISO8601文字列だとレスポンスが数倍に膨らみ、パースも面倒になる
- `time` の値は**常にUTCのunixtime**。`timezone` パラメータは日境界の決定に使われるだけで、返る整数値はUTC。表示時は `localtime_r()` に任せる（システムTZをJSTに設定済みであること）
- `precipitation` は**直前1時間の積算値**。瞬時値ではない
- `precipitation_probability` は `null` が返り得る。null安全に処理すること
- `models` は `best_match`（地点ごとに最高解像度モデルを自動選択、日本ではJMA MSM 5kmメッシュが選ばれる）を既定とする。`jma_seamless` を明示指定できるよう Kconfig で切り替え可能にすること
- ライセンスは CC BY 4.0。個人利用の範囲だが、README に出典を明記すること

### 2.2 Yahoo! YOLP 気象情報API（オプション・要Client ID）

```
GET https://map.yahooapis.jp/weather/V1/place
  ?coordinates=139.9019,35.6536
  &output=json
  &appid=<Client ID>
```

- **座標の順序が経度,緯度**である点に注意（Open-Meteoと逆）
- レスポンスは `Feature[0].Property.WeatherList.Weather[]` に10分刻みの配列。各要素は `Type`（`observation` = レーダー実況 / `forecast` = 予測）、`Date`（`YYYYMMDDHHMM`）、`Rainfall`（mm/h）
- 現在時刻の実測値から60分後までの予測値が得られる。これは予報モデルではなく気象レーダー由来なので、直近の精度が予報より高い
- 無料枠は1アプリ1日5万アクセス。5分間隔なら1日288回で全く問題ない
- Kconfig の `CONFIG_LW_ENABLE_NOWCAST` でビルド時に有効/無効を切り替える。既定は無効（Client ID未取得でもビルドが通ること）

---

## 3. ソフトウェア構成

### 3.1 コンポーネント方針

| 用途 | 使うもの |
|---|---|
| HTTP(S) | `esp_http_client` + `esp_crt_bundle_attach` |
| JSON | `cJSON`（ESP-IDF同梱） |
| 時刻同期 | `esp_netif_sntp`（`ntp.nict.jp`, `pool.ntp.org`） |
| LCD駆動 | `esp_lcd` + `esp_lcd_ili9341` コンポーネント |
| GUI | LVGL v9 + `esp_lvgl_port`（Component Manager経由） |
| タッチ | `esp_lcd_touch_ft5x06`（FT6336Uはこれで動く） |
| 設定保存 | NVS |

**TLSは必ず `esp_crt_bundle` で証明書検証を行うこと。** 検証スキップは実装しない。ESP-IDFは標準でルートCAバンドルを持っているので、これは追加コストなしで正しくやれる。

### 3.2 タスク構成

FreeRTOSタスクを2本立てる。

```
net_task   (prio 5, stack 8192)
  └ Wi-Fi接続維持 → SNTP同期 → 定期的にAPI取得 → パース → 判定
     → 結果をmutex保護された共有構造体に書き、UIへイベント通知

ui_task    (prio 4, stack 8192)
  └ LVGLタイマハンドラを回す
     → 通知を受けて画面更新、1分ごとに時刻表示とカレントスロット位置を更新
```

- 共有データは `app_state_t` を1つ作り、mutexで保護する
- LVGLのAPIは `esp_lvgl_port` のロック（`lvgl_port_lock()` / `unlock()`）の内側からのみ呼ぶ。ui_task 以外から直接触らない
- ネットワーク取得はUIをブロックしないこと。取得中もタッチが効き、画面が固まらないこと

### 3.3 データ構造

```c
typedef struct {
    time_t t;        // スロット開始時刻 (UTC unixtime)
    float  mm;       // 直前1時間の降水量 [mm]
    int    pop;      // 降水確率 [%] (欠損時は 0)
} hour_slot_t;

typedef enum {
    V_UNKNOWN = 0, V_OK, V_CAUTION, V_BRING_IN, V_RAINING
} verdict_t;

typedef struct {
    verdict_t verdict;
    int       hours_to_rain;   // -1 = 24h以内に雨なし
    time_t    rain_start;
    time_t    rain_end;
    float     rain_total_mm;
    int       rain_peak_pop;
} judgement_t;

typedef struct {
    hour_slot_t slots[48];
    int         n_slots;
    judgement_t judge;
    float       nowcast_max_mm_h;  // 未取得時は -1.0f
    time_t      last_update;
    bool        last_fetch_ok;
} app_state_t;
```

---

## 4. ハードウェア

### 4.0 まず一次情報を確認すること（重要）

CoreS3 SEの電源投入シーケンスは自明ではない。以下を必ず参照してから実装すること。

1. **公式ドキュメント:** https://docs.m5stack.com/ja/core/M5CoreS3%20SE
2. **回路図PDF:** https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/498/Sch_M5_CoreS3_SE_v1.0.pdf
3. **M5Unifiedの初期化コード:** GitHubの `m5stack/M5Unified` にある CoreS3 向けボード初期化処理。AXP2101の各レールに何がぶら下がっているか、どの順で立ち上げるかが動作実績のあるコードとして書かれている。**これが最も信頼できる情報源**なので、レール割り当てはここから読み取ること
4. **ドライバ実装の参考:** `m5stack/M5CoreS3`（Arduino版）の AXP2101 / AW9523B 周り

### 4.1 ペリフェラル一覧

| デバイス | I2Cアドレス | バス |
|---|---|---|
| AXP2101 (PMIC) | 0x34 | 内部I2C |
| AW9523B (IOエキスパンダ) | 0x58 | 内部I2C |
| BM8563 (RTC) | 0x51 | 内部I2C |
| FT6336U (タッチ) | 0x38 | 内部I2C |
| ES7210 (マイク入力コーデック) | 0x40 | 内部I2C |
| AW88298 (スピーカーアンプ) | 0x36 | 内部I2C |

### 4.2 ピンアサイン

**内部I2C:** SDA = GPIO12, SCL = GPIO11

**SPI (LCD + microSD 共用):**

| 信号 | GPIO |
|---|---|
| MOSI | 37 |
| SCK | 36 |
| LCD CS | 3 |
| LCD DC | 35 |
| SD MISO | 35 |
| SD CS | 4 |

> **⚠️ 最重要の落とし穴:** LCDのDCとmicroSDのMISOが**どちらもGPIO35**で物理的に共用されている。LCDとSDを同時に使うことはできず、切り替えのたびにピン機能を再設定する必要がある。M5GFXはこれを内部で吸収しているが、ESP-IDFで生書きすると必ずここで踏む。**本プロジェクトはmicroSDを使わない**ので、GPIO35はLCD DC専用として扱い、SDドライバは初期化しないこと。将来ログ保存でSDを使いたくなった場合は、この共用を前提とした排他制御が別途必要になる。

**LCDリセット・バックライト:**

| 信号 | 接続先 |
|---|---|
| LCD RST | AW9523B の P1_1 |
| LCD バックライト | AXP2101 の LDO出力（回路図で要確認） |

> バックライトはGPIOのPWMではなく**PMICのLDO経由**。ここを知らないと「初期化は通っているのに真っ暗」になる。M5Unifiedのコードでどのレールをどう制御しているか確認すること。輝度制御もLDO電圧の調整で行う。

**タッチ:**

| 信号 | 接続先 |
|---|---|
| I2C | 内部I2C (SDA 12 / SCL 11) |
| TOUCH_RST | AW9523B の P0_0 |
| TOUCH_INT | AW9523B の P1_2 |

> 割り込みピンがIOエキスパンダ側にあるため、ESP32から直接エッジ割り込みを取れない。**ポーリング方式で実装すること**（LVGLの入力デバイスは既定でポーリングなので問題ない）。

**I2S（スピーカー・オプション機能）:**

| 信号 | GPIO |
|---|---|
| BCK | 34 |
| WCK/LRCK | 33 |
| MCLK | 0 |
| DOUT (→AW88298) | 14 |
| DIN (←ES7210) | 13 |

> 公式ドキュメント内でもペリフェラル表とM5-Bus表でMCLK/LRCKの記載に揺れがある。ビープ音のみの用途なので、動かない場合は深追いせず**音声機能を無効化してビルドが通る構成にすること**（判定表示が本体であり、音は補助）。

**Grove Port A（将来の外部センサ用、本プロジェクトでは未使用）:** SDA = GPIO2, SCL = GPIO1

### 4.3 電源に関する前提

- **CoreS3 SE にバッテリーは内蔵されていない**（無印CoreS3は500mAh、CoreS3-Liteは200mAh、SEはなし）。USB給電での常時稼働を前提とする。バッテリー残量表示のUIは作らないこと
- AXP2101 と AW9523B の組み合わせで電源の入出力方向を制御している。`BUS_OUT_EN` / `USB_OTG_EN` の状態設定が必要な場合がある

### 4.4 CoreS3 SE に存在しないもの

以下は無印CoreS3にはあるがSEでは削除されている。**これらを参照するコードを書かないこと。** Arduino版のCoreS3サンプルを流用する際の主な失敗要因である。

- カメラ (GC0308)
- IMU (BMI270)
- 地磁気センサ (BMM150)
- 近接センサ (LTR-553ALS)
- 内蔵バッテリー

---

## 5. 画面デザイン

解像度 320 x 240、横向き（`rotation = 1`）。

```
┌────────────────────────────────────────┐
│ 浦安市                        更新 14:32 │  ← 12px, グレー
├────────────────────────────────────────┤
│ ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓ │
│ ┃          干してOK                  ┃ │  ← 24px 白抜き / 判定色の帯
│ ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛ │
│                                        │
│ 15:00〜18:00  2.4mm  最大80%            │  ← 14px 白
│ あと約4時間で降り出します                 │
│ 直近60分レーダー: 最大 0.0 mm/h          │  ← ナウキャスト有効時のみ
│                                        │
│ 最大 2.4mm/h ／ 24時間表示               │  ← 10px グレー
│ ▁▁▁▂▅█▇▃▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁              │  ← グラフ
│ 12  15  18  21  0   3   6   9          │  ← 3時間ごとに時刻ラベル
└────────────────────────────────────────┘
```

**グラフの仕様:**
- 背景の薄い青バー = 降水確率（高さは0〜100%を全高にマップ）
- 前景の明るいシアンのバー = 降水量（高さは 0〜max(1.0, 表示範囲内の最大値) にマップ）
- 降水量が 0 より大きいが表示上つぶれる場合は最低2px確保する
- `is_rainy` が真のスロットは色を変えて識別できるようにする

**判定色:**

| 判定 | RGB |
|---|---|
| V_OK | `#2EA043` |
| V_CAUTION | `#D89B00` |
| V_BRING_IN | `#E05000` |
| V_RAINING | `#C02030` |

### 5.1 日本語フォント

LVGLの標準フォントに日本語は含まれない。以下の方針で対応すること。

1. Noto Sans JP から**このアプリで実際に使う文字だけをサブセット化**したフォントを生成する。必要な文字は40字程度（浦安市、更新、失敗、干してOK、短時間なら可、まもなく取り込め、取り込め、時間、分、降、雨、直近、最大、表示、切替 など）
2. LVGLのフォントコンバータで16px / 24px の2サイズを生成し、`components/fonts/` に配置
3. **生成スクリプトをリポジトリに含めること。** 文言を変更したときに再生成できないと保守できなくなる

数字・記号・英字は LVGL 標準の Montserrat を併用してよい。

---

## 6. sdkconfig 要件

```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_QUAD=y          # CoreS3 SE は Quad PSRAM (8MB)
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
CONFIG_LV_COLOR_DEPTH_16=y
CONFIG_LV_COLOR_16_SWAP=y          # ILI9342C 用（表示色がおかしければここを疑う）
```

- LVGLの描画バッファはPSRAMに確保する（8MB あるので余裕がある）
- パーティションテーブルは16MB Flash向けのカスタムCSVを用意する。OTAは今回不要だが、将来入れられるようfactory + 2 OTAスロットの構成にしておくこと

---

## 7. 成果物

```
laundry-weather/
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions.csv
├── README.md                  # ビルド・書き込み手順、Wi-Fi設定方法、出典表記
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild      # §1.3 のしきい値、座標、Wi-Fi、APIキー
│   ├── main.c
│   ├── app_state.c/.h         # 共有状態とmutex
│   ├── weather_openmeteo.c/.h # 取得とパース
│   ├── weather_nowcast.c/.h   # Yahoo（条件コンパイル）
│   ├── judge.c/.h             # §1.2 の判定ロジック（純粋関数、副作用なし）
│   └── ui.c/.h                # LVGL画面構築と更新
├── components/
│   ├── axp2101/               # PMICドライバ
│   ├── aw9523b/               # IOエキスパンダドライバ
│   ├── bm8563/                # RTCドライバ
│   ├── cores3se_bsp/          # 上記をまとめたボード初期化 (bsp_init())
│   └── fonts/                 # サブセット日本語フォント + 生成スクリプト
└── test/
    └── test_judge.c           # 判定ロジックの単体テスト
```

`judge.c` は**ネットワークにもLVGLにも依存しない純粋なロジック**として切り出すこと。ここが唯一しきい値調整で何度も触る部分になるので、ホスト側でテストできる形にしておく価値が高い。

---

## 8. 品質要件

### 8.1 堅牢性

このデバイスは**電源を入れたまま放置される**前提。以下は必須。

- Wi-Fi切断時は指数バックオフで自動再接続を試み続ける。再起動でごまかさない
- API取得失敗時は**前回取得したデータを表示し続ける**。画面をクリアしたりエラー画面で埋めたりしない。ヘッダの「更新」表示を「失敗 HH:MM」に変えて、最終成功時刻がわかるようにする
- JSONパース失敗、配列長不一致、null値、想定外の型をすべてハンドルする。Open-Meteoの `precipitation_probability` は実際に null を返すことがある
- HTTPレスポンスサイズに上限を設け、超過時は打ち切る（メモリ枯渇の防止）
- 連続稼働でヒープが単調減少しないこと。`esp_get_free_heap_size()` を定期的にログ出力し、数日単位でのリークがないことを確認できるようにする

### 8.2 時刻の扱い

- システムTZは `JST-9` に設定する（`setenv("TZ", "JST-9", 1); tzset();`）
- SNTP同期完了後、**BM8563 RTCに時刻を書き込む**。電源復帰後にWi-Fi接続前でも時刻表示が成立するようにするため
- 起動時はRTCから読んで暫定表示し、SNTP同期後に上書きする
- SNTP未同期の状態で判定を行わないこと（現在時刻が不定だとスロット位置が狂う）

### 8.3 ログ

`ESP_LOGI` でタグを分けて出力する。最低限、取得URL（APIキーはマスクすること）、HTTPステータス、パースしたスロット数、判定結果、空きヒープ。

---

## 9. マイルストーン

各段階で実機で確認できる状態にしてから次に進むこと。まとめて書いて最後に通電すると切り分けができなくなる。

| # | 内容 | 完了条件 |
|---|---|---|
| 1 | プロジェクト雛形 + BSP | I2Cスキャンで §4.1 の全アドレスが検出される |
| 2 | 電源シーケンス + LCD点灯 | 画面に単色塗りつぶしが表示される |
| 3 | LVGL + 日本語フォント | 「浦安市」が正しく表示される |
| 4 | タッチ入力 | タップ座標がログに出る |
| 5 | Wi-Fi + SNTP + RTC | 現在時刻が画面に表示される |
| 6 | Open-Meteo取得とパース | スロット数と最初の数件がログに出る |
| 7 | 判定ロジック + 単体テスト | `test_judge` が通る |
| 8 | 画面の完成 | §5 のレイアウトが再現される |
| 9 | 堅牢性対応 | Wi-Fiを落としても表示が維持され、復帰後に更新される |
| 10 | ナウキャスト（オプション） | Client ID設定時に直近60分の値が表示される |

**マイルストーン2が最大の難所。** ここで詰まったら推測でレジスタを叩かず、§4.0 の一次情報に戻ること。

---

## 10. README に含めること

- ESP-IDFのバージョンと環境構築手順
- `idf.py set-target esp32s3` から `idf.py -p <PORT> flash monitor` までの手順
- **ダウンロードモードへの入り方:** 底面のRSTボタンを3秒長押しし、緑のインジケータが点灯したら離す。離すと緑が消灯してダウンロードモードに入る。（USB-CDC経由の自動リセットが効かない場合に必要）
- Wi-Fi資格情報とAPIキーの設定方法（`idf.py menuconfig`）
- しきい値の意味と、実際に運用しながらどう追い込むか
- データ出典表記: Open-Meteo (CC BY 4.0)、Yahoo! JAPAN Web Services

---

## 11. スコープ外（今回作らない）

以下は将来の拡張候補。**今回は実装しないこと。** 先に降水判定を実用レベルにするのが優先。

- 乾燥指数（湿度・風速・日射から算出）
- microSDへのログ保存（§4.2 のGPIO35共用問題があるため設計が必要）
- Webからの設定変更UI
- 複数地点対応
- MQTT / Home Assistant連携
- OTA更新
