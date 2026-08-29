# laundry-weather

M5Stack **CoreS3 SE** 用の、洗濯物を「干す / 取り込む」判断のための天気表示端末。

千葉県浦安市の1時間ごとの降水予測を [Open-Meteo](https://open-meteo.com/) から取得し、
4段階の判定バナー・サマリ文・降水量グラフを 320x240 の画面に常時表示する。
USB給電で電源を入れっぱなしにして使うことを前提としている。

実装仕様は [`SPEC_laundry_weather_cores3se.md`](SPEC_laundry_weather_cores3se.md)、
ハードウェアの調査結果は [`docs/HARDWARE.md`](docs/HARDWARE.md) を参照。

構想から完成までの経緯とはまりどころは [`docs/DEVLOG.md`](docs/DEVLOG.md) にまとめてある。
CoreS3 SE で ESP-IDF を使う人が同じ穴に落ちないように、実際のログ付きで記録した。

## 現在の状態

| # | マイルストーン | 状態 |
|---|---|---|
| 1 | プロジェクト雛形 + BSP（I2Cスキャン） | **完了**（6デバイス全検出） |
| 2 | 電源シーケンス + LCD点灯 | **完了**（全画面塗りつぶし・色順序も確認） |
| 3 | LVGL + 日本語フォント | **完了**（「浦安市」表示を確認） |
| 4 | タッチ入力 | **完了**（タップ座標のログとレンジ切替） |
| 5 | Wi-Fi + SNTP + RTC | **完了**（RTC書き戻しまで確認） |
| 6 | Open-Meteo取得とパース | **完了**（48スロット取得） |
| 7 | 判定ロジック + 単体テスト | **完了**（ホストで43チェック通過） |
| 8 | 画面の完成 | **完了**（実データで「取り込め」表示を確認） |
| 9 | 堅牢性対応 | 作業中 |
| 10 | ナウキャスト（オプション） | 見送り（Client ID 未取得。コードの枠は用意済み） |

## 必要な環境

- **ESP-IDF v5.3 以降**（開発は v5.5-dev / master で行っている）
- M5Stack CoreS3 SE 本体と USB-C ケーブル

### Apple Silicon Mac での注意

ESP-IDF のビルドは、**arm64 ネイティブの CMake** を使わないと失敗する。

Homebrew などで入れた `/usr/local/bin/cmake` が x86_64 バイナリの場合、
それが Rosetta 上で動き、**子プロセスの Python も x86_64 として起動される**。
すると ESP-IDF の Python 仮想環境にある arm64 の wheel（`pydantic_core` など）が
読み込めず、`incompatible architecture (have 'arm64', need 'x86_64')` で
コンフィグ段階から落ちる。

ESP-IDF 同梱の CMake / Ninja を入れれば解決する:

```bash
cd $IDF_PATH
python tools/idf_tools.py install cmake ninja
```

`export.sh` がこれらを PATH の先頭に置くので、以降は意識しなくてよい。

## ビルドと書き込み

```bash
. $HOME/esp/esp-idf/export.sh     # 環境に合わせてパスを変える
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem101 flash monitor
```

`monitor` を抜けるには `Ctrl-]`。

### ダウンロードモードへの入り方（重要）

CoreS3 SE は ESP32-S3 内蔵の USB-Serial-JTAG（VID `0x303a` / PID `0x1001`）で
PCと繋がるが、**esptool の自動リセットが効かない**。
`Failed to connect to Espressif device: No serial data received.` が出たら、
手動でダウンロードモードに入れる:

1. 本体**底面の RST ボタンを3秒間長押し**する
2. 緑のインジケータが点灯したら**離す**
3. 離すと緑が消灯し、ダウンロードモードに入る
4. この状態で `idf.py -p <PORT> flash` を実行する

書き込み後は通常どおり起動する。

#### `idf.py flash` が接続に失敗する場合

`idf.py flash` は `--before=default_reset` を使うため、ダウンロードモードに
入っているにもかかわらず接続を壊すことがある。その場合は esptool を直接呼ぶ:

```bash
cd build
esptool.py --chip esp32s3 -p <PORT> -b 460800 \
    --before no_reset --after no_reset write_flash @flash_args
```

書き込みが途中で `Packet content transfer stopped` などで止まった場合は、
フラッシュが中途半端な状態になっていて起動しない。RSTの3秒長押しで
ダウンロードモードに入り直してから、もう一度書き込めば復旧する。

#### シリアルポートの排他に注意

`idf.py monitor` やログ取得スクリプトがポートを開いたままだと、書き込みは
`multiple access on port?` や `Packet content transfer stopped` で失敗する。
**書き込み前にポートを掴んでいるプロセスを必ず終了すること。**
基板側の不具合に見えるが、原因はこれであることが多い。

### この基板はソフトウェアリセットが効かない

実機で確認した限り、**リセットを起こす手段は物理的な電源再投入だけ**である。
以下はいずれも効果がなかった:

- esptool の `--after hard_reset`（`Hard resetting via RTS pin` と表示はされるが実際には
  リセットされず、USB CDC デバイスの再列挙も起きない）
- pyserial から RTS を操作しての EN 制御
- RST ボタンの短押し

そのため書き込み後は **USBケーブルを抜き差しして起動させる**必要がある。
書き込み直後はフラッシャスタブが動いたままで、この状態ではアプリが起動せず
シリアル出力も一切出ない（`esptool --before no_reset` で接続できてしまう状態）。

この制約があるため、ブリングアップ用の診断ログは起動時1回ではなく定期的に出力する
作りにしてある。起動の瞬間をログ取得と同期させる必要をなくすため。

### ホスト側の単体テスト

判定ロジックは ESP-IDF に依存しないので、実機なしで検証できる:

```bash
./test/run.sh
```

## Wi-Fi 資格情報と APIキーの設定

**資格情報をリポジトリにコミットしないこと。** `sdkconfig` は `.gitignore` で除外している。

設定方法は2通り。

### 方法1: `sdkconfig.defaults.local`（推奨）

リポジトリ直下に `sdkconfig.defaults.local` を作る。このファイルは `.gitignore` 済みで、
トップレベルの `CMakeLists.txt` が存在すれば自動で読み込む。

```
CONFIG_LW_WIFI_SSID="your-ssid"
CONFIG_LW_WIFI_PASSWORD="your-password"
```

作成後は `idf.py fullclean` してからビルドすると確実に反映される。
チェックアウトし直してもこのファイルさえ再作成すれば済むので、こちらを勧める。

### 方法2: menuconfig

```bash
idf.py menuconfig
# Laundry Weather -> Wi-Fi -> Wi-Fi SSID / Wi-Fi password
```

書き込み先は `sdkconfig`（gitignore 済み）。

### ナウキャスト（オプション）

Yahoo! YOLP 気象情報APIによるレーダー実況は既定で無効。
使う場合は Client ID を取得のうえ:

```
CONFIG_LW_ENABLE_NOWCAST=y
CONFIG_LW_YAHOO_CLIENT_ID="your-client-id"
```

Client ID も秘密情報なので `sdkconfig.defaults.local` に置く。

## しきい値の調整

`idf.py menuconfig` の **Laundry Weather → Thresholds** から変更できる。
Kconfig には浮動小数点型がないため、mm の値は 100倍の整数で持っている。

| 設定 | 既定値 | 意味 |
|---|---|---|
| `LW_RAIN_MM_X100` | 20 (= 0.2mm) | これ以上/時の降水量を「雨」とみなす |
| `LW_RAIN_POP` | 50 | これ以上の降水確率を「雨」とみなす |
| `LW_NOWCAST_MM_H_X100` | 50 (= 0.5mm/h) | ナウキャストの警戒しきい値 |
| `LW_BRING_IN_H` | 2 | 何時間以内の雨で「取り込め」とするか |
| `LW_CAUTION_H` | 5 | 何時間以内の雨で「短時間なら可」とするか |
| `LW_REFRESH_SEC` | 600 | 予報の再取得間隔 [秒] |
| `LW_NOWCAST_SEC` | 300 | ナウキャストの再取得間隔 [秒] |
| `LW_BEEP_VOLUME` | 3 | 判定悪化時のビープ音量（1〜10、約4dB刻み） |
| `LW_UI_STALL_RESTART_SEC` | 300 | 画面が更新できないまま何秒経ったら再起動するか（0で無効） |

ビープは判定が悪化した瞬間にだけ、予告なく鳴る。家の中で驚かされるほうが
聞き逃すより困るので既定値は低めにしてある。本体が生活空間から離れた場所に
あるなら上げるとよい。

### 時間帯によって「何を判断するか」が変わる

同じ予報でも、朝と夕方では知りたいことが違う。まだ干していない朝10時に
「取り込め」と言われても意味がないので、時間帯で問いを切り替えている。

| 時間帯 | 画面が答えている問い | 見る範囲 | 表示例 |
|---|---|---|---|
| 5〜11時 | きょう干せそうか | 今から `LW_DRY_END_HOUR` まで | 「きょうは ほしどき」「いま あめです」 |
| 12〜19時 | 取り込むべきか | 前方24時間で最初に降る時刻まで | 「ほしてだいじょうぶ」「あめ！とりこんで」 |
| 20〜翌4時 | 次の日中は干せそうか | 次の `LW_DRY_START_HOUR`〜`LW_DRY_END_HOUR` | 「あした ほせそう」「あした きびしそう」 |

朝と夜は「この時間帯ぜんぶ乾いているか」という**窓判定**で、昼間の
「あと何時間で降るか」という前方走査とは別のロジック（`judge_window()`）を使う。

窓判定の等級:

- 窓の先頭から雨 → 最も強い警告（乾く隙がない）
- 窓の1/3以上が雨 → 「きびしそう」
- 少しだけ雨 → 「びみょう」
- 雨なし → 「ほせそう」

夕方に1時間だけ降るのと、午後がまるごと雨なのを同じ扱いにしないため。

**ビープは12〜19時のみ鳴る。** 干していない時間帯に「取り込め」と鳴っても
邪魔なだけで、深夜なら害しかない。判定の悪化自体はログには残る。

境界時刻と乾燥時間帯は menuconfig の **Laundry Weather → Time-of-day framing**
から変更できる。

| 設定 | 既定値 |
|---|---|
| `LW_MORNING_START_HOUR` | 5 |
| `LW_DAY_START_HOUR` | 12 |
| `LW_NIGHT_START_HOUR_OUTLOOK` | 20 |
| `LW_DRY_START_HOUR` | 9 |
| `LW_DRY_END_HOUR` | 17 |

### 運用しながらの追い込みかた

既定値は出発点であって正解ではない。実際に使いながら次の観点で調整する。

- **「取り込め」が出たのに降らなかった**が続く → `LW_RAIN_POP` を上げる（50→60など）。
  降水確率は広い範囲の平均なので、都市部では過大に出やすい。
- **降り出してから気づいた** → `LW_BRING_IN_H` を上げる（2→3）。
  ただし上げすぎると「まもなく取り込め」が常時出て意味をなさなくなる。
- **霧雨で警告が出る** → `LW_RAIN_MM_X100` を上げる（20→30）。
  0.2mm/h は「傘が要るか微妙」程度の弱い雨。
- **判定が頻繁に切り替わる** → `LW_REFRESH_SEC` はそのままに、まず上の3つを見直す。
  取得間隔を延ばしても判定の安定性は上がらない。

短時間の急な雨を捉えたい場合は、しきい値をいじるよりナウキャストを有効化するほうが効く。
予報モデルより気象レーダー実況のほうが直近の精度が高いため。

## 既知の不具合と対処

### LVGL が LCD の転送完了を永久に待つことがある

**症状:** 画面が更新されなくなり、タッチも効かなくなる。ただし再起動はせず、
バックライトは点いたまま。シリアル出力も止まる。

**原因:** LCD への描画転送の完了割り込みが届かないと、LVGL は
`lv_refr.c` の `while(disp->flushing);` で待ち続ける。
このループには**タイムアウトも yield も無い**。しかも LVGL タスクは
LVGL ロックを握ったままこれを回すので、画面を更新しようとする全タスクが
巻き添えで止まる。クラッシュではないのでウォッチドッグも発火しない。

JTAG で halt して確認した実際の状態:

```
Thread 3 "taskLVGL":  CPU1 PC = wait_for_flushing + 40   ← ロックを保持したままスピン
Thread 9 "ui":        lvgl_port_lock(0) で無限待ち (xTicksToWait = 4294967295)
```

**なぜ完了通知が失われたのかは未特定。** ただし当時は内部 RAM が最小 32KB まで
落ちており、後述の描画バッファ縮小でこれが 120KB 以上に改善している。
同じ根から来ていた可能性が高い。

### 描画バッファは PSRAM 指定でも内部 RAM を消費する

`esp_lvgl_port` に `buff_spiram = true` を渡していても、**描画バッファ1面分に
相当する内部 RAM が別途確保される**（実測）。

全画面ダブルバッファ（320x240）にしていたときの実測値:

```
internal RAM after display: 232,599 free
internal RAM after LVGL:     71,119 free   ← 161,480 バイト消費
→ E wifi: Expected to init 10 rx buffer, actual is 4   （Wi-Fi 初期化が失敗）
```

40行バッファに縮小した後:

```
internal RAM after lvgl_port_init: 225,555 free
internal RAM after add_disp:       199,891 free   ← 25,664 バイト（バッファ1面分とほぼ一致）
internal RAM after Wi-Fi:          120,567 free / 最大ブロック 47,104
```

**LVGL は無効化された領域しか描画しない**ので、バッファを大きくしても得るものはない。
「PSRAM に余裕があるから」でサイズを決めてはいけない。

**対処:**

1. `LW_UI_STALL_RESTART_SEC`（既定300秒）で、画面が更新できない状態が続いたら
   自動再起動する。原因が何であれ確実に復帰する
2. SPI の `max_transfer_sz` を実際のフラッシュサイズに合わせた。以前は
   全画面フラッシュが15分割されていて、完了通知を落とす機会がその分多かった
3. `lvgl_port_lock` にタイムアウトを設定（`esp_lvgl_port` は引数0を無限待ちとして扱う）

再発する場合は、LVGL の `flush_wait_cb` を差し替えて転送完了待ち自体に
タイムアウトを設ける（無限ハングを1フレーム落ちに変える）改修が次の手になる。

### JTAG で中を見る方法

この基板は USB-Serial-JTAG なので、追加ハードなしでデバッガを繋げられる。
ハングしたときは**電源を切る前に**これで状態を取ること。

```bash
openocd -f board/esp32s3-builtin.cfg &
xtensa-esp32s3-elf-gdb -batch \
    -ex "target remote :3333" \
    -ex "thread apply all bt" \
    build/laundry-weather.elf
```

OpenOCD はアタッチ時に halt するだけでリセットはしないので、状態は保存される。

## 仕様書からの意図的な変更点

`SPEC_laundry_weather_cores3se.md` は実装の出発点であり、実機で動かしながら
以下は意図的に変えている。仕様書側は更新していないので、差分をここに残す。

| 項目 | 仕様書 | 実装 | 理由 |
|---|---|---|---|
| 配色 | 暗い背景に白抜き文字 | 明るい生成り背景、淡い同系色の帯に濃い同系色の文字 | 生活空間に置く機器のため。白抜き文字を彩度の高い色に乗せるより柔らかく、コントラストも高い |
| 判定の文言 | 「取り込め」「干してOK」 | 「あめ！とりこんで」「ほしてだいじょうぶ」 | 家庭内で通りすがりに読まれる表示のため、命令ではなく提案の口調に |
| アイコン | 規定なし | 太陽・雲・雨粒を LVGL の図形で描画 | フォントの ☀☁☂ グリフは単色で素っ気ないため |
| 降雨中の表示 | 規定なし | 雲から水滴が落ちるアニメーション | ぱっと見で「いま降っている」と分かるようにするため |
| `LV_COLOR_16_SWAP` | sdkconfig に指定 | `esp_lvgl_port` の `swap_bytes` | LVGL v9 でこの Kconfig は廃止された |
| I2S DOUT/DIN | DOUT=14 / DIN=13 | DOUT=13 / DIN=14 | M5Unified の実コードに合わせた。仕様書自身が記載の揺れを認めている |
| 起動時の表示 | 規定なし | Wi-Fi の失敗理由を画面に名指し | 「接続中」のまま止まる表示はハングと区別がつかないため |

## データ出典とライセンス

### データ

- 気象予報データ: [Open-Meteo](https://open-meteo.com/) — ライセンス **CC BY 4.0**
- ナウキャスト（有効化した場合）: [Yahoo! JAPAN Web Services](https://developer.yahoo.co.jp/webapi/map/openlocalplatform/v1/weather.html)

Web Services by Yahoo! JAPAN (https://developer.yahoo.co.jp/about)

### このリポジトリ

コードとドキュメントは **MIT License**（[`LICENSE`](LICENSE)）。
ただし以下のフォントは別ライセンスなので注意。

### フォント

`components/fonts/` の日本語フォントは **Noto Sans JP** のサブセットで、
**SIL Open Font License 1.1** に従います。
詳細は [`components/fonts/NOTICE.md`](components/fonts/NOTICE.md)、
全文は [`components/fonts/LICENSE-NotoSansJP.txt`](components/fonts/LICENSE-NotoSansJP.txt) を参照。

### 依存コンポーネント

ESP-IDF、LVGL、`esp_lvgl_port`、`esp_lcd_ili9341`、`esp_lcd_touch_ft5x06` は
Component Manager 経由で取得され、リポジトリには含まれない（`managed_components/` は
`.gitignore` 対象）。それぞれのライセンスは取得先を参照。

ハードウェアの初期化手順は [m5stack/M5GFX](https://github.com/m5stack/M5GFX) と
[m5stack/M5Unified](https://github.com/m5stack/M5Unified)、および
[espressif/esp-bsp](https://github.com/espressif/esp-bsp) のソースを一次情報として
読み取った。調査結果と出典は [`docs/HARDWARE.md`](docs/HARDWARE.md) に記録している。
