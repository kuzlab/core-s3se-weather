# laundry-weather

M5Stack **CoreS3 SE** 用の、洗濯物を「干す / 取り込む」判断のための天気表示端末。

千葉県浦安市の1時間ごとの降水予測を [Open-Meteo](https://open-meteo.com/) から取得し、
4段階の判定バナー・サマリ文・降水量グラフを 320x240 の画面に常時表示する。
USB給電で電源を入れっぱなしにして使うことを前提としている。

実装仕様は [`SPEC_laundry_weather_cores3se.md`](SPEC_laundry_weather_cores3se.md)、
ハードウェアの調査結果は [`docs/HARDWARE.md`](docs/HARDWARE.md) を参照。

## 現在の状態

| # | マイルストーン | 状態 |
|---|---|---|
| 1 | プロジェクト雛形 + BSP（I2Cスキャン） | **完了**（6デバイス全検出） |
| 2 | 電源シーケンス + LCD点灯 | **完了**（全画面塗りつぶし・色順序も確認） |
| 3 | LVGL + 日本語フォント | 作業中 |
| 4 | タッチ入力 | 未着手 |
| 5 | Wi-Fi + SNTP + RTC | 未着手 |
| 6 | Open-Meteo取得とパース | 未着手 |
| 7 | 判定ロジック + 単体テスト | **完了**（ホストで43チェック通過） |
| 8 | 画面の完成 | 未着手 |
| 9 | 堅牢性対応 | 未着手 |
| 10 | ナウキャスト（オプション） | 未着手 |

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

## データ出典

- 気象予報データ: [Open-Meteo](https://open-meteo.com/) — ライセンス **CC BY 4.0**
- ナウキャスト（有効化した場合）: [Yahoo! JAPAN Web Services](https://developer.yahoo.co.jp/webapi/map/openlocalplatform/v1/weather.html)

Web Services by Yahoo! JAPAN (https://developer.yahoo.co.jp/about)
