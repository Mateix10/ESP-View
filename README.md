# ESP-View
This is a music visualiser + DAC that does exactly that - it plays the music through USB, and visualises it.

![Render image](Assets/Render.png)

<sub>Also, a better name will be much appreciated :)</sub>

## PCB
The PCB is designed in kicad.

![Schematic image](Assets/Schematic.png)

![PCB image](Assets/PCB.png)

## Case
The case features a screw and a little groove where the PCB slides.
It has a slick, modern, round design.

Designed in Fusion 360.

![Case image](Assets/Case1.png)

## Firmware 
The whole thing is powered by a ESP32, paired with a PCM5102A DAC.
The firmware is written in Arduino IDE

To interface with the ESP32, you need to install board support in Arduino IDE.
You can do that by going to File -> Preferences -> Additional Board Manager URLs, and add https://espressif.github.io/arduino-esp32/package_esp32_index.json

After that, go to Tools -> Board -> Boards Manager. Search for esp32 and install "esp32" by Espressif.

**WARNING**: Version 3.x is required for proper TinyUSB audio support.

Required libraries:
- NeoPixelBus by Makuna
- arduinoFFT
- TinyUSB (Usually bundled with the EPS32 Arduino Core)

All of these can be installed through the Arduino IDE's library manager.

The Firmware isn't currently tested.


## BOM
| Name | Cost |Link|
|--------|--------|--------|
| 1000uF Electrolytic Capacitor | $0.48 |https://www.optimusdigital.ro/ro/componente-electronice-condensatoare/3004-condensator-electrolitic-de-1000-uf-la-35-v.html|
| LED Matrix | $24.98 |https://www.superlightingled.com/5v-16x16-sk6812-rgbw-led-neopixel-matrix-display-screen-panel-51w-p-6835.html|
| DAC | $22.80 |https://www.optimusdigital.ro/ro/audio-altele/5658-modul-audio-stereo-dac-pcm5102a-cu-interfaa-pcm-32-bii-384-khz.html|
| Level Shifter| $3.19 |https://www.aliexpress.com/item/1005007123530432.html|
|ESP32-S3-DevKitC-1| $18.50 |https://www.aliexpress.com/item/1005003979778978.html|
