#pragma once

// Local display setup; no changes to the shared TFT_eSPI installation.
// Verified against Waveshare's ESP32S3_1.3inch schematic and Arduino demo.
#define ST7789_DRIVER
#define TFT_WIDTH 240
#define TFT_HEIGHT 240
// This panel uses BGR in MADCTL. RGB swapped red and blue on the physical LCD.
#define TFT_RGB_ORDER TFT_BGR
#define TFT_INVERSION_ON
// Explicit SPI2 selection avoids TFT_eSPI 2.5.43's default S3 port mismatch.
#define USE_FSPI_PORT
#define TFT_MISO -1
#define TFT_MOSI 41
#define TFT_SCLK 40
#define TFT_CS 39
#define TFT_DC 38
#define TFT_RST 42
#define TFT_BL 20
#define TFT_BACKLIGHT_ON HIGH
#define LOAD_GLCD
#define LOAD_FONT2
#define SPI_FREQUENCY 40000000
#define SPI_READ_FREQUENCY 20000000
