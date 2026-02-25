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

- `SCL` -> GPIO22
- `SDA` -> GPIO27
- I2Cクロック: 100kHz

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

## 画像背景を使う場合

SPIFFS に `/bg.jpg` を配置すると背景として表示されます。  
ファイルがない場合はコード内のフォールバック背景が使われます。

## 注意

- 現在の実装では、CO2キャリブレーション（強制再校正やASC設定）を明示的に設定していません。
- `press_hpa` は現状 `0.0` 固定です（BME280読み取りは未実装）。
