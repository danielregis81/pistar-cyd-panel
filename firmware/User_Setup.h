// User_Setup do TFT_eSPI para a CYD 2.8" (ESP32-2432S028R, ILI9341).
// Copie para a pasta da biblioteca TFT_eSPI substituindo o User_Setup.h
// (veja o README). Se as cores ficarem invertidas, troque TFT_BGR por TFT_RGB
// ou comente TFT_INVERSION_ON.
#define ILI9341_2_DRIVER
#define TFT_RGB_ORDER TFT_BGR
#define TFT_INVERSION_ON
#define TFT_WIDTH  240
#define TFT_HEIGHT 320
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1
#define TFT_BL   21
#define TFT_BACKLIGHT_ON HIGH
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
