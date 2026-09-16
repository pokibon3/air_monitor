# Air Monitor

ESP32 + ST7789 LCD + SCD30 で、CO2濃度と温湿度を表示するシンプルなエアモニターです。  
PlatformIO (Arduino framework) でビルドします。

## 機能

- CO2濃度表示（ppm）
  - 4桁の7セグ風表示
  - 範囲は `0` から `9999` にクランプ
- 温度・湿度表示
  - 下段パネルに `TEMP` / `HUMI` を表示
- 気圧表示のON/OFF（コンパイル時切り替え）
  - `AIRMONITOR_SHOW_PRESSURE=0` のとき: 温度/湿度を 50:50 で表示
  - `AIRMONITOR_SHOW_PRESSURE=1` のとき: 温度/湿度/気圧を 3分割表示
- 背景表示
  - SPIFFS に `/bg.jpg` があれば表示
  - ない場合はフォールバック背景を描画
- ちらつき防止描画
  - 各パネルをスプライト（オフスクリーンバッファ）に描いてから一括転送
  - 表示値が変わらないフレームは再描画をスキップ
  - スプライト確保に失敗した場合は従来の直接描画にフォールバック
- シリアルログ出力（115200bps）
  - `CO2(ppm)` と温湿度を出力

## 開発環境

- OS: macOS / Linux / Windows
- 開発ツール: PlatformIO Core または VSCode + PlatformIO
- 対応マイコンボード: `ESP32-2432S028R`
- PlatformIOターゲット: `esp32dev`（互換設定）
- フレームワーク: Arduino
- 依存ライブラリ（`platformio.ini`）
  - `lovyan03/LovyanGFX@^1.2.7`
  - `adafruit/Adafruit SCD30@^1.0.11`
  - `adafruit/Adafruit BME280 Library@^2.3.0`

## 配線

### LCD (ST7789, SPI)

- `SCLK` -> GPIO14
- `MOSI` -> GPIO13
- `MISO` -> GPIO12
- `DC` -> GPIO2
- `CS` -> GPIO15
- `RST` -> 未使用（`-1`）
- `BL` -> GPIO21

### SCD30 (I2C)

- `SDA` -> GPIO22
- `SCL` -> GPIO27
- I2Cクロック: 100kHz

※ ESP32 Arduinoコアの `Wire.begin(sda, scl)` は第1引数がSDAです。コードは `Wire.begin(22, 27)` なので SDA=GPIO22 / SCL=GPIO27 になります。

## 使い方

### 1. 依存関係のインストール

PlatformIO が未導入なら先にインストールしてください。

```bash
pip install -U platformio
```

### 2. ビルド

```bash
pio run
```

### 3. 書き込み

```bash
pio run -t upload
```

必要なら `platformio.ini` の `upload_port` を設定してください。

### 4. シリアルモニタ

```bash
pio device monitor -b 115200
```

## 表示設定（マクロ）

`src/main.cpp` の以下マクロで気圧表示を切り替えます。

```cpp
#ifndef AIRMONITOR_SHOW_PRESSURE
#define AIRMONITOR_SHOW_PRESSURE 0
#endif
```

- `0`: 気圧表示OFF（温度・湿度を半分ずつ表示）
- `1`: 気圧表示ON（3分割表示）

## CO2キャリブレーション（強制再校正 / FRC）

CO2値が恒常的に高い・低い場合は、屋外などの新鮮な空気の中で強制再校正を実行してください。

1. デバイスを屋外か窓全開の場所に置く
2. **BOOTボタン（GPIO0）を押したまま**電源を入れる（またはリセット）
3. 画面に `CO2 Calibration` と3分間のカウントダウンが表示される
4. カウントダウン終了後、自動的に基準値 400ppm で再校正され `Calibration OK` と表示される

校正値はSCD30内部に保存されるため、一度実行すれば以降は不要です。

## 温度オフセット（自己発熱補正）

SCD30は自己発熱により実際より2〜5°C高く表示されるため、起動時にオフセット補正を設定しています。
補正量は `src/main.cpp` のマクロで調整できます（単位: 0.01°C、既定値 300 = 3.00°C）。

```cpp
#ifndef AIRMONITOR_TEMP_OFFSET_C100
#define AIRMONITOR_TEMP_OFFSET_C100 300
#endif
```

調整方法: 10分以上ウォームアップした後、信頼できる温度計と比較して
「(SCD30の表示温度 − 実際の温度) × 100」を設定してください。

## 画像背景を使う場合

SPIFFS に `/bg.jpg` を配置すると背景として表示されます。  
ファイルがない場合はコード内のフォールバック背景が使われます。

## 注意

- 自動校正（ASC）は使用していません。校正はBOOTボタンによる強制再校正（FRC）で行います。
- `press_hpa` は現状 `0.0` 固定です（BME280読み取りは未実装）。
