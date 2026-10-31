// User_Setup.h per ESP32 CYD 2.8" (ESP32-2432S028R)
// Copia questo file in: Documenti/Arduino/libraries/TFT_eSPI/User_Setup.h
// (sovrascrivendo quello esistente)

#define USER_SETUP_INFO "CYD_2432S028R"

// Driver: la maggior parte dei CYD 2.8 usa ILI9341.
// Se lo schermo resta bianco/colori sbagliati (versione con 2 porte USB),
// commenta ILI9341_2_DRIVER e attiva ST7789_DRIVER.
#define ILI9341_2_DRIVER
// #define ST7789_DRIVER
// #define TFT_INVERSION_ON

#define TFT_WIDTH  240
#define TFT_HEIGHT 320

#define TFT_BL   21
#define TFT_BACKLIGHT_ON HIGH

#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1

#define USE_HSPI_PORT

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

#define SPI_FREQUENCY       55000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000
