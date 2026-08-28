# 開発ログ: M5Stack CoreS3 SE で洗濯物判断端末を作る

「洗濯物を干していいか、取り込むべきか」を一目で教えてくれる端末を、
M5Stack CoreS3 SE と ESP-IDF で作った記録。

構想から実装完了まで、はまったところを中心にまとめる。
コードは https://github.com/kuzlab/core-s3se-weather にある。

---

## 何を作ったか

浦安市の1時間ごとの降水予測を Open-Meteo から取得し、320x240 の画面に常時表示する。

- 4段階の判定を色分けした帯で表示（ほしてだいじょうぶ／みじかめならOK／そろそろとりこもう／あめ！とりこんで）
- 「あと4時間くらいでふりそう」のようなサマリ文
- 直近12/24時間の降水量と降水確率のグラフ
- いま降っていれば雲から雨粒が落ちるアニメーション
- 判定が悪化した瞬間だけビープ

USB給電で電源を入れっぱなしにして使う。

---

## 構想: 先に仕様書を書いた

いきなりコードを書かず、まず実装仕様書を1本書いた。機能要件、判定ロジック、
API、タスク構成、データ構造、ハードウェア、画面デザイン、品質要件、
マイルストーンまで。

これが後々効いた。特に効いたのは次の2点。

### 1. しきい値を「運用しながら追い込む前提の値」と明記した

判定ロジックはこうなっている。

```c
is_rainy(slot) := (slot.precipitation_mm >= RAIN_MM) || (slot.pop_percent >= RAIN_POP)
```

`RAIN_MM = 0.2`、`RAIN_POP = 50` といった値は、正解が事前に分かるものではない。
仕様書に「これらは運用しながら追い込む前提の値である。ハードコードせず
`idf.py menuconfig` から変更できること」と書いておいたおかげで、
最初から全部 Kconfig 化する方針が固まった。

### 2. 「一次情報を確認せよ」と釘を刺した

仕様書のハードウェアの章に、こう書いた。

> CoreS3 SEの電源投入シーケンスは自明ではない。（中略）
> M5Unifiedの初期化コード。AXP2101の各レールに何がぶら下がっているか、
> どの順で立ち上げるかが動作実績のあるコードとして書かれている。
> **これが最も信頼できる情報源**なので、レール割り当てはここから読み取ること

これは正しかった。詳しくは後述する。

### 3. マイルストーンを10段階に切って「各段階で実機確認」を義務づけた

> まとめて書いて最後に通電すると切り分けができなくなる

結果的にこれが最も価値のある指示だった。実際、後述する TLS の不具合は
「1回目は成功、2回目以降が全滅」という形で出たので、段階的に確認していなければ
原因の切り分けに何倍も時間がかかったはずだ。

---

## はまりどころ

### 1. Apple Silicon で x86_64 版の CMake を使うとビルドすら通らない

最初の `idf.py set-target esp32s3` でいきなり転んだ。

```
ImportError: dlopen(.../pydantic_core/_pydantic_core.cpython-311-darwin.so):
  (mach-o file, but is an incompatible architecture (have 'arm64', need 'x86_64'))
```

ESP-IDF の Python 仮想環境には arm64 の wheel が入っているのに、
「x86_64 が必要」と言われる。Python 自体は arm64 で動くことを確認しても直らない。

原因は **CMake だった**。

```
$ file /usr/local/bin/cmake
/usr/local/bin/cmake: Mach-O 64-bit executable x86_64
```

Homebrew (Intel版) で入れた CMake が x86_64 バイナリで、Rosetta 上で動く。
**macOS では universal binary の子プロセスは親のアーキテクチャ設定を継承する**ため、
CMake が起動する Python も x86_64 として立ち上がり、arm64 の wheel を読めなくなる。

ESP-IDF 同梱の CMake/Ninja を入れれば解決する。

```bash
cd $IDF_PATH
python tools/idf_tools.py install cmake ninja
```

`export.sh` がこれらを PATH の先頭に置くので、以降は意識しなくていい。

エラーメッセージは Python と wheel を指しているのに、犯人は CMake だった。
**「誰がそのプロセスを起動したか」まで遡らないと分からない**タイプの罠。

### 2. CoreS3 SE はソフトウェアリセットが一切効かない

書き込みは成功するのに、シリアル出力が1バイトも来ない。

```
$ idf.py -p /dev/cu.usbmodem101 flash
...
Leaving...
Hard resetting via RTS pin...
Done
```

「Hard resetting」と表示されているのに、実際にはリセットされていない。
決め手は次の確認だった。

```
$ esptool.py -p /dev/cu.usbmodem101 --before no_reset chip_id
Detecting chip type... ESP32-S3
Stub is already running. No upload is necessary.
```

`--before no_reset`（リセットを試みない）で即座に繋がる＝
**書き込み用のフラッシャスタブが動いたまま**だった。この状態ではアプリが起動せず、
ダウンロードモードは無出力なのでログもゼロになる。

試して効かなかったもの:

- esptool の `--after hard_reset`
- pyserial から RTS を操作しての EN 制御
- RST ボタンの短押し

**効くのは USB ケーブルの抜き差しだけ**だった。

この基板は ESP32-S3 内蔵の USB-Serial-JTAG（VID `0x303a` / PID `0x1001`）で
PC と繋がっている。自動リセットのための DTR/RTS 制御が効かない個体らしい。

#### 対処: 診断ログを起動時1回ではなく定期出力にした

これが一番効いた設計変更だった。起動の瞬間を捕まえないと情報が取れないのは、
以後ずっと足枷になる。20秒ごとに I2C スキャンとヒープ状況を出すようにして、
「抜き差しのタイミング」と「ログ取得のタイミング」を同期させる必要をなくした。

```c
/* The diagnostic block repeats on a timer rather than printing once at boot,
 * because this board accepts no software reset: the only way to restart it
 * is to unplug USB, and printing once would mean racing the log capture
 * against that replug. */
```

### 3. シリアルモニタを開いたままだと書き込みが失敗する

書き込みが途中で止まる現象に何度か遭遇した。

```
A serial exception error occurred: device reports readiness to read but
returned no data (device disconnected or multiple access on port?)
```

```
A fatal error occurred: Packet content transfer stopped (received 0 bytes)
```

基板の不具合に見えたが、原因は**自分のログ取得スクリプトがポートを掴んだまま**
だっただけ。エラーメッセージに `multiple access on port?` と書いてあるのに、
ハードウェアの問題だと思い込んで esptool のオプションを試行錯誤してしまった。

しかも運が悪いことに、ブートローダ領域の消去直後に止まったので
「基板が起動しなくなった」状態になり、余計に焦った。
（ダウンロードモードに入り直して書き込めば復旧する）

**書き込み前にポートを掴んでいるプロセスを必ず終了すること。**

### 4. PSRAM がヒープ枯渇を隠す（一番たちが悪かった）

これが今回の最大の落とし穴。

Wi-Fi と Open-Meteo 取得が動き出したあと、**1回目の取得は成功するのに
2回目以降が全部失敗する**という現象が出た。

```
E esp-x509-crt-bundle: PK verify failed with error 0x4290
E esp-x509-crt-bundle: Certificate matched but signature verification failed
E esp-tls-mbedtls: mbedtls_ssl_handshake returned -0x3000
```

「証明書は一致したが署名検証に失敗」。証明書バンドルの不具合か、
時刻がずれているのかと疑ったが、どちらも違った。

エラーコードを分解すると答えが出る。

```
0x4290 = MBEDTLS_ERR_RSA_PUBLIC_FAILED (0x4280) + MBEDTLS_ERR_MPI_ALLOC_FAILED (0x0010)
```

**暗号処理中のメモリ確保失敗**だった。暗号のバグでも証明書の問題でもない。

Wi-Fi と LVGL が内部 RAM を消費した状態で、mbedtls は既定で内部 RAM から確保する。
1回目は足りて、2回目以降は足りない。

#### 診断ログ自体が問題を隠していた

ここが本当の教訓。仕様書には「連続稼働でヒープが単調減少しないこと。
`esp_get_free_heap_size()` を定期的にログ出力し」と書いてあり、その通りに実装していた。
そして、こう報告していた。

```
free heap: 8201964 bytes, min ever: 8189672
```

**「8.2MB 空いていて完全に安定」。嘘ではないが、まったく役に立たない数字だった。**

`esp_get_free_heap_size()` は PSRAM を含む合計値を返す。CoreS3 SE には 8MB の
PSRAM が載っているので、内部 RAM が枯渇していてもこの数字はびくともしない。

内訳を出すように直したら、実態が見えた。

```
heap: total 8202360 free / internal 132867 free,
      largest internal block 86016, min internal ever 32863
```

**内部 RAM の最小値は 32,863 バイトまで落ちていた。** これが TLS を殺していた。

#### 修正

mbedtls の割り当てを PSRAM に逃がす。

```
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y
CONFIG_MBEDTLS_DYNAMIC_FREE_CA_CERT=y
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=2048
```

結果、連続取得が全部通るようになった。

```
I esp-x509-crt-bundle: Certificate validated
I openmeteo: HTTP 200, content-length 0
I openmeteo: read 1240 bytes
I openmeteo: parsed 48 slots
（6回連続成功、失敗0回）
```

**教訓:** PSRAM 搭載機で `esp_get_free_heap_size()` だけを見るのは危険。
`heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` と、
**`heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)`** を必ず併記する。
確保が失敗するかどうかは合計ではなく最大連続ブロックで決まる。

さらに言えば、ヒープログの初回が300秒後にしか出ない作りにもなっていて、
**問題が実際に起きる起動直後の時間帯に記録が無かった**。これも直した。

### 5. 電源シーケンス投入前後で I2C スキャンの結果が変わる

マイルストーン1の完了条件は「I2Cスキャンで全アドレスが検出される」だった。
念のため電源シーケンスの前後で2回スキャンするようにしたら、差が出た。

```
--- I2C scan (before power-on sequence) ---
  0x34  AXP2101 (PMIC)
  0x40  ES7210 (mic codec)
  0x51  BM8563 (RTC)
  0x58  AW9523B (IO expander)
  4 device(s) responded

--- I2C scan (after power-on sequence) ---
  0x34  AXP2101 (PMIC)
  0x36  AW88298 (spk amp)     ← 増えた
  0x38  FT6336U (touch)       ← 増えた
  0x40  ES7210 (mic codec)
  0x51  BM8563 (RTC)
  0x58  AW9523B (IO expander)
  6 device(s) responded
```

**タッチIC とスピーカーアンプは、電源シーケンスを通すまで応答しない。**
AXP2101 のレール投入と AW9523B のリセット解除に依存している。

つまり電源シーケンスを省略すると「タッチICが見つからない」という、
**原因から遠い形で失敗する**。仕様書が警告していた通りの構造だった。

> ここを知らないと「初期化は通っているのに真っ暗」になる

ちなみにこのスキャンで、仕様書では断定できなかった
「CoreS3 SE にマイクとスピーカーアンプは載っているのか」も確定した。載っていた。

### 6. 電源シーケンスは M5GFX のソースが唯一の正解

仕様書の指示通り、レジスタ値は推測せず m5stack/M5GFX の CoreS3 自動検出ブロックから
読み取った。分かったこと:

- **バックライトは AXP2101 の DLDO1**（レジスタ `0x90` bit7 で on/off、`0x99` で電圧＝輝度）
- 輝度マッピングは `reg0x99 = (brightness + 641) >> 5`
  → **実効レンジは DLDO1 = 2.5V〜3.3V の9段階しかない**
- LCD の RST は AW9523B の P1_1（レジスタ `0x03` bit1）
- AW9523B は Port0 を Push-Pull に、両ポートを GPIO モード（LEDモードではない）に設定する必要がある

特にバックライトは、知らないと「初期化は全部成功しているのに画面が真っ暗」になる。
GPIO の PWM を探しても永遠に見つからない。

### 7. LVGL v9 に `LV_COLOR_16_SWAP` は無い

仕様書の sdkconfig 要件に `CONFIG_LV_COLOR_16_SWAP=y` と書いてあった
（「表示色がおかしければここを疑う」というコメント付きで）。

これは LVGL v8 までの設定で、**v9 では廃止されている**。

v9 でのバイトスワップは `esp_lvgl_port` の設定で行う。

```c
.flags = {
    .swap_bytes = true,   // これが v8 の LV_COLOR_16_SWAP に相当
},
```

espressif/esp-bsp の `m5stack_core_s3` が `BSP_LCD_BIGENDIAN=1` を
この `swap_bytes` に渡していて、それが裏付けになった。

esp_lcd 経由での CoreS3 の駆動方法は、M5GFX とは別の一次情報として
esp-bsp が使える。設定はこうだった。

```c
esp_lcd_new_panel_ili9341(...)                    // ILI9342C は ili9341 ドライバで動く
.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR
esp_lcd_panel_invert_color(panel, true)           // 必須。無いと色が反転する
esp_lcd_panel_swap_xy(panel, false)
```

検証は原色を順に全画面表示させて行った。単色1回だけだと「表示できている」ことしか
分からないが、**赤→緑→青→白→黒を順に出せば色順序の誤りも同時に検出できる**。

### 8. パネル世代の判別が M5GFX の既知テーブルから外れていた

CoreS3 のパネルには ILI9342C と ILI9342E の個体差があり、E なら追加の初期化コマンドが要る。
M5GFX は **タッチICのファームウェアIDでパネル世代を判別**している
（LCD側からは読めないため）。

実機の値を読んでみたら、こうなった。

| レジスタ | 実測値 | M5GFX が期待する値 |
|---|---|---|
| `0xA3` CIPHER | 0x64 | （判定に未使用） |
| `0xA6` FIRMID | 0x03 | 0x10 (ILI9342C) / 0x12 (ILI9342E) |
| `0xA8` VENDID | 0x20 | 0x11 (M5Stack) |

**既知テーブルに載っていない個体だった。**
M5GFX に同じコードを走らせても "version read failed" で ILI9342C にフォールバックする。

ただし CIPHER の `0x64` は **FT6336U のチップID そのもの**なので、
読み出し自体は成功している。単に M5GFX が知るロットと違うだけ。
ILI9342C として扱って正常に表示できた。

### 9. 資料同士が食い違う: I2S の DOUT/DIN

仕様書の I2S ピン表は DOUT=14 / DIN=13 だったが、
M5Unified の実コードは **DOUT=13 / DIN=14 で逆**だった。

仕様書自身が「公式ドキュメント内でもペリフェラル表と M5-Bus 表で記載に揺れがある」と
断っていた箇所なので、**動作実績のあるコード側を採用**した。結果、音は正しく鳴った。

### 10. Wi-Fi の SSID は大文字小文字を区別する（そして UI がそれを伝えなかった）

Wi-Fi が繋がらない。

```
W wifi: disconnected (reason 201)
I wifi: reconnecting in 1 s
W wifi: disconnected (reason 201)
I wifi: reconnecting in 2 s
...（4, 8, 16, 32 秒とバックオフ）
```

reason 201 = `WIFI_REASON_NO_AP_FOUND`。原因は SSID のタイポで、
`Tp-Link_235C` ではなく `TP-Link_235C`（P も大文字）が正しかった。

**ここで指摘されたのが本質的だった。**

> 「接続中、から進まないね。時刻待ち、にもなってる。
> WiFi繋がったかどうかがわからないのが良くないのでは」

その通りだった。画面はずっと「接続中」を出し続けていて、
**ハングしているのか接続を試み続けているのか区別がつかない**。

外から見ると、以下は全部同じ「繋がらない」に見える。

- SSID のタイポ
- パスワード間違い
- 5GHz 専用の AP（ESP32-S3 は 2.4GHz のみ）
- 電波が届いていない

#### 対処: 画面に原因を名指しさせた

ヘッダを段階ごとに別の語にした。

```
未接続 → 接続中 → 同期中 → 取得中 → 更新 14:32
```

サマリ欄に失敗理由を出すようにした。

| 状況 | 表示 |
|---|---|
| AP が見つからない | 「Wi-Fiにつながりません」／「TP-Link_235C が見つかりません」 |
| 認証失敗 | 「パスワードが違うようです」 |
| その他の切断 | 「電波が届いていないようです」 |

加えて、「AP が見つからない」が続いたときだけ**見えている 2.4GHz の AP を
スキャンしてログに列挙**する診断も入れた（毎回やると再接続が遅くなるので1回だけ）。

仕様書 §8.1 は「取得失敗時は前回データを表示し続ける」までしか要求していなかったが、
**起動時に何も表示できない状態の説明責任も同じ性質の問題**だと判断した。

今回のようなタイポなら、シリアルログを見なくても画面だけで原因にたどり着ける。

### 11. Kconfig に浮動小数点型が無い

しきい値 `RAIN_MM = 0.2` を Kconfig 化しようとして気づいた。
**Kconfig に float 型は無い。** 100倍の整数で持つことにした。

```
config LW_RAIN_MM_X100
    int "Rain threshold [mm/h x100]"
    default 20
    help
        Stored x100 because Kconfig has no float type: 20 = 0.20 mm.
```

座標も同様に microdegrees（`35653600` = 35.6536）で持っている。

### 12. `CONFIG_LWIP_SNTP_MAX_SERVERS` の既定値は 1

NTP サーバを2つ指定したらビルド警告が出た。

```
warning: excess elements in array initializer
```

`ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("ntp.nict.jp", "pool.ntp.org"))`
と書いても、`CONFIG_LWIP_SNTP_MAX_SERVERS` の既定値が **1** なので配列が溢れる。

**警告だけでビルドは通る**ので、気づかないと2つ目が黙って無視される。

```
CONFIG_LWIP_SNTP_MAX_SERVERS=2
```

ついでに引っかかったのが、**`sdkconfig.defaults` は既存の `sdkconfig` を上書きしない**こと。
設定を追加したら `sdkconfig` を消して再生成する必要がある。

### 13. `mktime()` と `timegm()` の罠

RTC (BM8563) は UTC を保持している。これを `struct tm` から `time_t` に変換するとき、
`mktime()` を使うと **ローカルタイム（JST）として解釈されて9時間ずれる**。

正しくは `timegm()` だが、これが ESP-IDF の newlib で確実に宣言されているとは限らず、
実際 `implicit declaration of function 'timegm'` で怒られた。

どちらの間違いも**黙って9時間ずれる**という最悪の壊れ方をするので、
自前で書くことにした。

```c
/* struct tm (UTC) -> unixtime. mktime() would apply the JST offset, and
 * timegm() is not dependably declared by the toolchain's newlib, so the
 * conversion is done here. */
static time_t utc_tm_to_time(const struct tm *tm)
```

### 14. 音量スケールを線形にしたのは設計ミスだった

ビープを実装して鳴らしてもらったら、こう返ってきた。

> 「ビープ音なった。音大き過ぎる。びっくりしたわ。1/5くらいでよい」

振幅を 1/5 にして、ついでに `LW_BEEP_VOLUME`（1〜10）として Kconfig 化した。
再度鳴らしてもらうと:

> 「まだまぁまぁ大きいね。全然小さくて良い」

ここで気づいた。**線形スケールにしたのが間違いだった。**
人が感じる音量は対数的なので、線形の 1〜10 では
**使える小音量が全部下1〜2段に詰まって選択肢にならない**。

約4dB刻みのテーブルに作り直した。

```c
/* The steps are about 4dB apart rather than evenly spaced, because loudness
 * is perceived logarithmically: a linear scale crams every usable quiet
 * setting into its bottom one or two steps and leaves nothing to choose
 * between. */
static const int AUDIO_VOLUME_TABLE[10] = {
    142, 226, 358, 568, 900, 1427, 2262, 3586, 5684, 9000
};
```

| 設定 | 振幅 |
|---|---|
| 10 | 9000（最初にびっくりした音量） |
| 5 | 900 |
| 3 | 358（既定値） |
| 1 | 142 |

既定を低めにしたのは、**このビープは予告なく鳴るので、
驚かされるほうが聞き逃すより困る**という判断。画面が主で、音は注意を引くだけの補助。

---

## 効いた設計判断

### 判定ロジックを純粋関数として切り出した

`judge.c` はネットワークにも LVGL にも ESP-IDF にも依存しない。
おかげでホストの `cc` だけでテストできる。

```bash
$ ./test/run.sh
test_judge

all dry -> V_OK
rain in the current slot -> V_RAINING
hour-offset boundaries
...
43 checks, 0 failure(s)
```

しきい値の境界値（`bring_in_h`/`caution_h` の前後）、等号の成立、
降雨ブロックの範囲算出、24時間ホライズンの端、データが足りない場合、
ナウキャストの「上げるだけで下げない」性質などをカバーしている。

**しきい値調整で何度も触る部分だからこそ、実機なしで検証できる形にしておく価値が高い。**

### フォントのサブセットを「文字リスト」ではなく「UI文字列」から生成した

LVGL の標準フォントに日本語は無いので、Noto Sans JP から必要な文字だけを
サブセット化する必要がある。

素直にやると「使う文字のリスト」を手で管理することになるが、
**文言を変えたときに必ず同期が壊れる**。

そこで、UI に出る文字列そのものを `ui_strings.txt` に列挙し、
生成スクリプトが**そこから文字集合を自動抽出**する形にした。

```bash
$ ./components/fonts/generate.sh
Subset: 243 bytes of UTF-8, characters:
  ☀☁☂〜あいうかがきくこさしじすずせそだちっつてでとなにのはびふぶほまみめもゅょよらり...
Generating lw_font_jp_16.c (16px, 4bpp)...
Generating lw_font_jp_24.c (24px, 4bpp)...
```

文言を変えて再生成すれば必ず整合する。実際、UI 刷新で文言を全面的に書き換えたが、
この仕組みのおかげで豆腐（■）が1文字も出なかった。

### 診断ログを「定期出力」にした

前述の通り。リセットが効かない基板では、これが無いと調査が成立しない。

---

## UI は最後に作り直した

一通り動いたあと、こういう要望をもらった。

> 1. ダークではなくもっと明るい雰囲気の配色にして欲しい
> 2. 「取り込め」というような率直かつ口調が強い言葉遣いではなく、もっとアットホームな言葉遣いで
> 3. ところどころアイコンを表示するなど親しみやすいUIに
> 4. いま降っているのであればアニメーションを表示するなど画面のぱっと見でわかる情報を追加して欲しい

仕様書には暗い背景に白抜き文字、判定色も彩度の高い4色と書いてあったが、
**生活空間に置く機器としては確かに硬すぎた**。

### 配色

背景を温かみのある生成り（`#F6F3EC`）に。判定バナーは
**淡い同系色の背景に濃い同系色の文字**という組み合わせにした。
白抜き文字を彩度の高い色に乗せるより柔らかく見え、しかもコントラストは高く保てる。

| 判定 | 背景 | 文字 |
|---|---|---|
| だいじょうぶ | `#E7F6EA` | `#2E7D4F` |
| みじかめ | `#FFF4DC` | `#A9760A` |
| そろそろ | `#FFE9DC` | `#C0551D` |
| あめ | `#FFE4E6` | `#C0392B` |

### 言葉遣い

| 前 | 後 |
|---|---|
| 取り込め | あめ！とりこんで |
| 干してOK | ほしてだいじょうぶ |
| まもなく取り込め | そろそろとりこもう |
| 今降っています | いまふっています |
| あと約4時間で降り出します | あと4時間くらいでふりそう |
| 失敗 14:32 | 更新できず 14:32 |

エラー文言も和らげたが、**原因を名指しする具体性は保った**。
柔らかくした結果、何が悪いのか分からなくなっては本末転倒なので。

### アイコン

フォントに ☀☁☂ のグリフはあった（Noto Sans JP に含まれている）が、
単色で素っ気ないので **LVGL の図形で描いて色を付けた**。
太陽は黄色の円＋淡いハロー、雲は3つの円＋台座。

### 雨のアニメーション

いま降っている時だけ、雲から3粒の水滴が落ちる。
**落下タイミングを 1/3 周期ずつずらしてある。**
同時に落とすと「棒が並んで動いている」ようにしか見えず、雨に見えない。

---

## 数字

| 項目 | 値 |
|---|---|
| ファームウェアサイズ | 約 1.4 MB（3MB パーティションの 54% 空き） |
| フォント（16px + 24px サブセット） | 69文字 + ASCII |
| 内部 RAM 空き（実行中） | 約 130 KB、最小 32 KB |
| ホストテスト | 43 チェック |
| I2C デバイス | 6個（AXP2101 / AW9523B / BM8563 / FT6336U / ES7210 / AW88298） |

---

## まとめ: 今回の教訓

1. **一次情報にあたる。** 電源シーケンスは推測で書けない。
   動作実績のあるコード（M5GFX / M5Unified / esp-bsp）が唯一の正解。
2. **PSRAM 搭載機で `esp_get_free_heap_size()` だけを見ない。**
   内部 RAM の空きと最大連続ブロックを併記する。
   合計値は問題を隠す。
3. **診断ログが問題を隠していないか疑う。**
   「安定しています」という報告が、見るべき数字を見ていないだけのことがある。
4. **外から区別がつかない失敗は、機器自身に説明させる。**
   「接続中」のまま止まる表示はハングと区別がつかない。
5. **人間の知覚に関わるパラメータを線形スケールにしない。**
   音量しかり、輝度しかり。
6. **段階的に実機確認する。** まとめて書いて最後に通電すると切り分けができない。
7. **エラーメッセージを最後まで読む。** `multiple access on port?` と
   書いてあるのに、ハードウェアの問題だと思い込んで時間を溶かした。

---

## 残っている宿題

- **Wi-Fi 切断からの復帰が未検証。** 無限リトライとバックオフは
  SSID タイポの時に実証されたが、切断 → 復帰の経路は通っていない
- **ナウキャスト（Yahoo! YOLP）は見送り。** Client ID 未取得。
  Kconfig と条件コンパイルの枠は用意してある
- しきい値の実運用での追い込み。使ってみないと分からない

---

## リンク

- リポジトリ: https://github.com/kuzlab/core-s3se-weather
- ハードウェア調査結果（出典と実測値）: [`docs/HARDWARE.md`](HARDWARE.md)
- 実装仕様書: [`SPEC_laundry_weather_cores3se.md`](../SPEC_laundry_weather_cores3se.md)

### 使ったもの

- [Open-Meteo](https://open-meteo.com/) — 気象予報データ (CC BY 4.0)
- [m5stack/M5GFX](https://github.com/m5stack/M5GFX)、[m5stack/M5Unified](https://github.com/m5stack/M5Unified) — 電源シーケンスとペリフェラルの一次情報
- [espressif/esp-bsp](https://github.com/espressif/esp-bsp) — esp_lcd 経由での CoreS3 駆動の参考
- [Noto Sans JP](https://github.com/notofonts/noto-cjk) — 日本語フォント (SIL OFL 1.1)
