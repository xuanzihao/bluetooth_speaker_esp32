Bluetooth Speaker
=================

Bluetooth Speaker based on ESP32 chip with dynamic vision effects output.

## Main Features

* A2DP Audio Streaming
* I2S & PDM Input / I2S Output
* VFX Output (GIF / Audio FFT / Rainbow / Star Sky / ...)
* BLE Control Interface (for VFX Output)
* Audio Prompt (Connected / Disconnected / WakeUp / Sleep)
* OTA Firmware Update (via SPP Profile)
* Sleep & WakeUp Key

## Preparing

### Obtain the Source

```
git clone --recursive https://github.com/redchenjs/bluetooth_speaker_esp32.git
```

### Update an existing repository

```
git pull
git submodule update --init --recursive
```

### Setup the Tools

```
./esp-idf/install.sh
```

## Building

### Setup the environment variables

```
export IDF_PATH=$PWD/esp-idf
source ./esp-idf/export.sh
```

### Configure

```
idf.py menuconfig
```

* All project configurations are under the `Bluetooth Speaker` menu.

### Flash & Monitor

```
idf.py flash monitor
```

## VFX on ST7789 135x240 LCD Panel (VU Meter)

<img src="docs/st7789vu.png">

## VFX on ST7789 135x240 LCD Panel (GIF)

<img src="docs/st7789gif.png">

## VFX on ST7735 80x160 LCD Panel (Linear Spectrum)

<img src="docs/st7735lin.png">

## VFX on ST7735 80x160 LCD Panel (CUBE0414 Simulation)

<img src="docs/st7735sim.png">

## VFX on ST7789 135x240 LCD Panel (Logarithmic Spectrum)

<img src="docs/st7789log.png">

## VFX on CUBE0414 8x8x8 Music Light Cube

<img src="docs/cube0414.png">

## Videos Links

* [音乐全彩光立方演示](https://www.bilibili.com/video/av25188707)
