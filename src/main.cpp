#include <Arduino.h>
#include <SPIFFS.h>
#include <Wire.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Adafruit_SCD30.h>

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel_instance;
  lgfx::Bus_SPI _bus_instance;
  lgfx::Light_PWM _light_instance;

 public:
  LGFX(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 3;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = 1;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc = 2;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs = 15;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.memory_width = 240;
      cfg.memory_height = 320;
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 16;
      cfg.dummy_read_bits = 1;
      cfg.readable = false;
      cfg.invert = false;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel_instance.config(cfg);
    }

    {
      auto cfg = _light_instance.config();
      cfg.pin_bl = 21;
      cfg.invert = false;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }

    setPanel(&_panel_instance);
  }
};

static LGFX lcd;
static Adafruit_SCD30 scd30;

// 0: pressure panel hidden (TEMP/HUMI only, 50:50), 1: pressure panel shown.
#ifndef AIRMONITOR_SHOW_PRESSURE
#define AIRMONITOR_SHOW_PRESSURE 0
#endif

static uint16_t co2_ppm = 0;
static bool sensor_ok = false;
static bool bg_loaded = false;
static float temp_c = 0.0f;
static float humid_rh = 0.0f;
static float press_hpa = 0.0f;

static const uint8_t SEG_A = 1 << 0;
static const uint8_t SEG_B = 1 << 1;
static const uint8_t SEG_C = 1 << 2;
static const uint8_t SEG_D = 1 << 3;
static const uint8_t SEG_E = 1 << 4;
static const uint8_t SEG_F = 1 << 5;
static const uint8_t SEG_G = 1 << 6;

static const uint8_t DIGIT_SEGMENTS[10] = {
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,
    SEG_B | SEG_C,
    SEG_A | SEG_B | SEG_D | SEG_E | SEG_G,
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_G,
    SEG_B | SEG_C | SEG_F | SEG_G,
    SEG_A | SEG_C | SEG_D | SEG_F | SEG_G,
    SEG_A | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,
    SEG_A | SEG_B | SEG_C,
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G,
};

void drawSegmentH(int x, int y, int w, int t, bool on, uint16_t onColor, uint16_t offColor) {
  lcd.fillRoundRect(x, y, w, t, t / 2, on ? onColor : offColor);
}

void drawSegmentV(int x, int y, int t, int h, bool on, uint16_t onColor, uint16_t offColor) {
  lcd.fillRoundRect(x, y, t, h, t / 2, on ? onColor : offColor);
}

void drawDigit(int x, int y, int w, int h, int digit, uint16_t onColor, uint16_t offColor) {
  const int t = w / 6;
  const int half = h / 2;

  uint8_t seg = 0;
  if (digit >= 0 && digit <= 9) {
    seg = DIGIT_SEGMENTS[digit];
  }

  drawSegmentH(x + t, y, w - 2 * t, t, seg & SEG_A, onColor, offColor);
  drawSegmentV(x + w - t, y + t, t, half - t, seg & SEG_B, onColor, offColor);
  drawSegmentV(x + w - t, y + half, t, half - t, seg & SEG_C, onColor, offColor);
  drawSegmentH(x + t, y + h - t, w - 2 * t, t, seg & SEG_D, onColor, offColor);
  drawSegmentV(x, y + half, t, half - t, seg & SEG_E, onColor, offColor);
  drawSegmentV(x, y + t, t, half - t, seg & SEG_F, onColor, offColor);
  drawSegmentH(x + t, y + half - t / 2, w - 2 * t, t, seg & SEG_G, onColor, offColor);
}

void drawFallbackAnimeBg() {
  const int w = lcd.width();
  const int h = lcd.height();
  const uint16_t top = lcd.color565(18, 8, 34);
  const uint16_t bottom = lcd.color565(2, 6, 18);

  for (int y = 0; y < h; ++y) {
    uint8_t blend = (uint8_t)map(y, 0, h - 1, 0, 255);
    uint8_t r = ((top >> 11) & 0x1F) * (255 - blend) / 255 + ((bottom >> 11) & 0x1F) * blend / 255;
    uint8_t g = ((top >> 5) & 0x3F) * (255 - blend) / 255 + ((bottom >> 5) & 0x3F) * blend / 255;
    uint8_t b = (top & 0x1F) * (255 - blend) / 255 + (bottom & 0x1F) * blend / 255;
    lcd.drawFastHLine(0, y, w, (r << 11) | (g << 5) | b);
  }

  for (int i = 0; i < 24; ++i) {
    int x = (i * 37 + 29) % w;
    int y = (i * 53 + 17) % (h / 2);
    uint16_t c = (i % 3 == 0) ? lcd.color565(255, 80, 180) : lcd.color565(90, 160, 255);
    lcd.fillCircle(x, y, 1 + (i % 2), c);
  }

  // Stylized anime-like silhouette.
  lcd.fillEllipse(248, 96, 34, 38, lcd.color565(30, 22, 50));      // hair
  lcd.fillTriangle(214, 130, 282, 130, 248, 200, lcd.color565(20, 18, 30));  // body
  lcd.fillCircle(248, 96, 18, lcd.color565(255, 224, 210));        // face
  lcd.fillCircle(242, 94, 2, lcd.color565(20, 20, 30));            // eye
  lcd.fillCircle(254, 94, 2, lcd.color565(20, 20, 30));            // eye
  lcd.fillRoundRect(236, 70, 24, 8, 4, lcd.color565(255, 130, 180));  // hair ribbon
}

void drawBackground() {
  bg_loaded = false;
  if (SPIFFS.begin(false) && SPIFFS.exists("/bg.jpg")) {
    lcd.drawJpgFile(SPIFFS, "/bg.jpg", 0, 0, lcd.width(), lcd.height());
    bg_loaded = true;
    return;
  }
  drawFallbackAnimeBg();
}

void drawDisplayFrame() {
  const int w = lcd.width();
  if (!bg_loaded) {
    drawBackground();
  }
  lcd.fillRect(0, 0, w, 36, lcd.color565(6, 10, 18));
  lcd.fillRoundRect(64, 0, w - 128, 28, 8, lcd.color565(10, 24, 38));
  lcd.drawRoundRect(64, 0, w - 128, 28, 8, lcd.color565(96, 205, 255));
  lcd.setTextDatum(middle_center);
  lcd.setTextColor(lcd.color565(200, 245, 255));
  lcd.setFont(&fonts::Font4);
  lcd.drawString("Air Monitor", w / 2, 18);
}

void drawCO2(uint16_t ppm) {
  const int w = lcd.width();
  const int panelX = 2;
  const int panelW = w - 4;
  const int panelY = 36;
  const int panelH = 128;
  // 7-seg glyph ratio fixed to W:H = 3:4.
  const int digitW = 72;
  const int gap = 6;
  const int totalDigitsW = digitW * 4 + gap * 3;
  const int x0 = panelX + (panelW - totalDigitsW) / 2;
  const int y0 = panelY + 24;
  const int digitH = (digitW * 4) / 3;

  uint16_t onColor = lcd.color565(80, 255, 220);
  uint16_t offColor = lcd.color565(16, 52, 48);

  int d0 = (ppm / 1000) % 10;
  int d1 = (ppm / 100) % 10;
  int d2 = (ppm / 10) % 10;
  int d3 = ppm % 10;

  lcd.fillRoundRect(panelX, panelY, panelW, panelH, 10, lcd.color565(0, 16, 24));
  lcd.drawRoundRect(panelX, panelY, panelW, panelH, 10, lcd.color565(70, 205, 255));
  lcd.setTextDatum(top_left);
  lcd.setTextColor(lcd.color565(140, 230, 255));
  lcd.setFont(&fonts::Font2);
  lcd.drawString("CO2", panelX + 12, panelY + 6);
  lcd.setTextDatum(top_right);
  lcd.drawString("PPM", panelX + panelW - 12, panelY + 6);

  drawDigit(x0 + (digitW + gap) * 0, y0, digitW, digitH, d0, onColor, offColor);
  drawDigit(x0 + (digitW + gap) * 1, y0, digitW, digitH, d1, onColor, offColor);
  drawDigit(x0 + (digitW + gap) * 2, y0, digitW, digitH, d2, onColor, offColor);
  drawDigit(x0 + (digitW + gap) * 3, y0, digitW, digitH, d3, onColor, offColor);
}

void drawEnv(float temp, float rh, float hpa) {
  const int panelX = 2;
  const int panelY = 168;
  const int panelW = lcd.width() - 4;
  const int panelH = 70;

  float tVal = isfinite(temp) ? temp : 0.0f;
  float hVal = isfinite(rh) ? rh : 0.0f;
  if (tVal < 0.0f) tVal = 0.0f;
  if (tVal > 99.9f) tVal = 99.9f;
  if (hVal < 0.0f) hVal = 0.0f;
  if (hVal > 99.9f) hVal = 99.9f;

  int t10 = (int)roundf(tVal * 10.0f);  // XX.X
  int h10 = (int)roundf(hVal * 10.0f);  // XX.X
#if AIRMONITOR_SHOW_PRESSURE
  int p = (int)roundf(hpa);
  if (p < 0) p = 0;
  if (p > 9999) p = 9999;
#else
  (void)hpa;
#endif

  const int sections = AIRMONITOR_SHOW_PRESSURE ? 3 : 2;
  const int sectionW = panelW / sections;
  const int yDigits = panelY + 22;
  const uint16_t onColor = lcd.color565(180, 245, 255);
  const uint16_t offColor = lcd.color565(18, 60, 62);
  const uint16_t boxBg = lcd.color565(8, 14, 24);
  const uint16_t boxBorder = lcd.color565(70, 170, 210);

  for (int i = 0; i < sections; ++i) {
    int bx = panelX + i * sectionW + 1;
    int bw = sectionW - 2;
    lcd.fillRoundRect(bx, panelY, bw, panelH, 8, boxBg);
    lcd.drawRoundRect(bx, panelY, bw, panelH, 8, boxBorder);
  }

  // Keep original 7-seg glyph aspect ratio (W:H = 3:4).
  const int digitW = 24;
  const int digitH = 32;
  const int gap = 3;
  const int dotGap = 6;
  const int tempTotalW = digitW * 3 + gap * 2 + dotGap;
  int tX = panelX + sectionW / 2 - tempTotalW / 2;
  drawDigit(tX, yDigits, digitW, digitH, (t10 / 100) % 10, onColor, offColor);
  drawDigit(tX + digitW + gap, yDigits, digitW, digitH, (t10 / 10) % 10, onColor, offColor);
  int t3x = tX + (digitW + gap) * 2 + dotGap;
  drawDigit(t3x, yDigits, digitW, digitH, t10 % 10, onColor, offColor);
  lcd.fillCircle(t3x - 4, yDigits + digitH - 4, 2, onColor);

  int hX = panelX + sectionW + sectionW / 2 - tempTotalW / 2;
  drawDigit(hX, yDigits, digitW, digitH, (h10 / 100) % 10, onColor, offColor);
  drawDigit(hX + digitW + gap, yDigits, digitW, digitH, (h10 / 10) % 10, onColor, offColor);
  int h3x = hX + (digitW + gap) * 2 + dotGap;
  drawDigit(h3x, yDigits, digitW, digitH, h10 % 10, onColor, offColor);
  lcd.fillCircle(h3x - 4, yDigits + digitH - 4, 2, onColor);

  lcd.setFont(&fonts::Font2);
  lcd.setTextColor(lcd.color565(120, 200, 235));
  lcd.setTextDatum(top_left);
  lcd.drawString("TEMP", panelX + 4, panelY + 4);
  lcd.drawString("HUMI", panelX + sectionW + 4, panelY + 4);

  // Unit text: right-aligned above the last digit, staying within that digit width.
  lcd.setTextDatum(top_right);
  lcd.drawString("C", t3x + digitW - 1, panelY + 6);
  lcd.drawString("%", h3x + digitW - 1, panelY + 6);

#if AIRMONITOR_SHOW_PRESSURE
  const int pDigitW = digitW;
  const int pDigitH = digitH;
  const int pGap = 2;
  const int pTotalW = pDigitW * 4 + pGap * 3;
  int pX = panelX + sectionW * 2 + sectionW / 2 - pTotalW / 2;
  drawDigit(pX, yDigits, pDigitW, pDigitH, (p / 1000) % 10, onColor, offColor);
  drawDigit(pX + pDigitW + pGap, yDigits, pDigitW, pDigitH, (p / 100) % 10, onColor, offColor);
  drawDigit(pX + (pDigitW + pGap) * 2, yDigits, pDigitW, pDigitH, (p / 10) % 10, onColor, offColor);
  drawDigit(pX + (pDigitW + pGap) * 3, yDigits, pDigitW, pDigitH, p % 10, onColor, offColor);
  lcd.drawString("PRES", panelX + sectionW * 2 + 4, panelY + 4);
  lcd.drawString("hPa", pX + (pDigitW + pGap) * 3 + pDigitW - 1, panelY + 6);
#endif
}

void drawStatus(const char* msg, uint16_t color) {
  (void)msg;
  (void)color;
}

void refreshAll() {
  drawCO2(co2_ppm);
  drawEnv(temp_c, humid_rh, press_hpa);
}

void setup() {
  Serial.begin(115200);

  lcd.init();
  lcd.setRotation(3);  // 320x240
  lcd.setBrightness(255);

  drawBackground();
  drawDisplayFrame();

  Wire.begin(22, 27);  // SCL=22, SDA=27
  Wire.setClock(100000);

  sensor_ok = scd30.begin();
  if (!sensor_ok) {
    co2_ppm = 0;
    temp_c = 0.0f;
    humid_rh = 0.0f;
  }

  refreshAll();
}

void loop() {
  if (sensor_ok && scd30.dataReady()) {
    if (scd30.read()) {
      float v = scd30.CO2;
      if (v < 0) v = 0;
      if (v > 9999) v = 9999;
      co2_ppm = (uint16_t)(v + 0.5f);
      temp_c = scd30.temperature;
      humid_rh = scd30.relative_humidity;
      if (!isfinite(temp_c) || !isfinite(humid_rh)) {
        temp_c = 0.0f;
        humid_rh = 0.0f;
      }
      press_hpa = 0.0f;
      refreshAll();
      Serial.print("CO2(ppm): ");
      Serial.println(co2_ppm);
      Serial.printf("T: %.1f C, H: %.1f %%\n", temp_c, humid_rh);
    } else {
      co2_ppm = 0;
      temp_c = 0.0f;
      humid_rh = 0.0f;
      press_hpa = 0.0f;
      refreshAll();
    }
  } else if (!sensor_ok) {
    co2_ppm = 0;
    temp_c = 0.0f;
    humid_rh = 0.0f;
    press_hpa = 0.0f;
  }

  delay(200);
}
