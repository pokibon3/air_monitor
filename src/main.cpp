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

// SCD30 self-heating compensation, in 0.01 degC units (300 = 3.00 degC).
// Tune by comparing against a reference thermometer after 10+ min warm-up.
#ifndef AIRMONITOR_TEMP_OFFSET_C100
#define AIRMONITOR_TEMP_OFFSET_C100 300
#endif

// Hold BOOT (GPIO0) at power-on to run forced recalibration in fresh air.
static const int PIN_CAL_BUTTON = 0;
static const uint32_t CAL_WARMUP_MS = 180000;  // SCD30 needs >2 min of stable readings before FRC
static const uint16_t CAL_REFERENCE_PPM = 400;

static uint16_t co2_ppm = 0;
static bool sensor_ok = false;
static bool bg_loaded = false;
static float temp_c = 0.0f;
static float humid_rh = 0.0f;
static float press_hpa = 0.0f;

// Flicker-free rendering: panels are drawn off-screen and pushed in one shot.
// Falls back to direct drawing if sprite allocation fails.
static LGFX_Sprite co2_spr(&lcd);
static LGFX_Sprite env_spr(&lcd);
static bool spr_ok = false;
// Transparent key color so the rounded panel corners keep the wallpaper.
static const uint16_t SPR_TRANSP = 0xF81F;

// Last values actually drawn; redraws are skipped while they are unchanged.
static int last_co2 = -1;
static int last_t10 = -1;
static int last_h10 = -1;
static int last_p = -1;

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

void drawSegmentH(lgfx::LovyanGFX& g, int x, int y, int w, int t, bool on, uint16_t onColor, uint16_t offColor) {
  g.fillRoundRect(x, y, w, t, t / 2, on ? onColor : offColor);
}

void drawSegmentV(lgfx::LovyanGFX& g, int x, int y, int t, int h, bool on, uint16_t onColor, uint16_t offColor) {
  g.fillRoundRect(x, y, t, h, t / 2, on ? onColor : offColor);
}

void drawDigit(lgfx::LovyanGFX& g, int x, int y, int w, int h, int digit, uint16_t onColor, uint16_t offColor) {
  const int t = w / 6;
  const int half = h / 2;

  uint8_t seg = 0;
  if (digit >= 0 && digit <= 9) {
    seg = DIGIT_SEGMENTS[digit];
  }

  drawSegmentH(g, x + t, y, w - 2 * t, t, seg & SEG_A, onColor, offColor);
  drawSegmentV(g, x + w - t, y + t, t, half - t, seg & SEG_B, onColor, offColor);
  drawSegmentV(g, x + w - t, y + half, t, half - t, seg & SEG_C, onColor, offColor);
  drawSegmentH(g, x + t, y + h - t, w - 2 * t, t, seg & SEG_D, onColor, offColor);
  drawSegmentV(g, x, y + half, t, half - t, seg & SEG_E, onColor, offColor);
  drawSegmentV(g, x, y + t, t, half - t, seg & SEG_F, onColor, offColor);
  drawSegmentH(g, x + t, y + half - t / 2, w - 2 * t, t, seg & SEG_G, onColor, offColor);
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

void drawCO2(uint16_t ppm, bool force = false) {
  if (!force && (int)ppm == last_co2) {
    return;
  }
  last_co2 = ppm;

  const int panelW = lcd.width() - 4;
  const int panelH = 128;
  // Off-screen target when available; direct LCD drawing as fallback.
  lgfx::LovyanGFX& g = spr_ok ? static_cast<lgfx::LovyanGFX&>(co2_spr)
                              : static_cast<lgfx::LovyanGFX&>(lcd);
  const int panelX = spr_ok ? 0 : 2;
  const int panelY = spr_ok ? 0 : 36;
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

  if (spr_ok) {
    co2_spr.fillSprite(SPR_TRANSP);
  }
  g.fillRoundRect(panelX, panelY, panelW, panelH, 10, lcd.color565(0, 16, 24));
  g.drawRoundRect(panelX, panelY, panelW, panelH, 10, lcd.color565(70, 205, 255));
  g.setTextDatum(top_left);
  g.setTextColor(lcd.color565(140, 230, 255));
  g.setFont(&fonts::Font2);
  g.drawString("CO2", panelX + 12, panelY + 6);
  g.setTextDatum(top_right);
  g.drawString("PPM", panelX + panelW - 12, panelY + 6);

  drawDigit(g, x0 + (digitW + gap) * 0, y0, digitW, digitH, d0, onColor, offColor);
  drawDigit(g, x0 + (digitW + gap) * 1, y0, digitW, digitH, d1, onColor, offColor);
  drawDigit(g, x0 + (digitW + gap) * 2, y0, digitW, digitH, d2, onColor, offColor);
  drawDigit(g, x0 + (digitW + gap) * 3, y0, digitW, digitH, d3, onColor, offColor);

  if (spr_ok) {
    co2_spr.pushSprite(2, 36, SPR_TRANSP);
  }
}

void drawEnv(float temp, float rh, float hpa, bool force = false) {
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
  const int p = 0;
  (void)hpa;
#endif

  if (!force && t10 == last_t10 && h10 == last_h10 && p == last_p) {
    return;
  }
  last_t10 = t10;
  last_h10 = h10;
  last_p = p;

  const int panelW = lcd.width() - 4;
  const int panelH = 70;
  // Off-screen target when available; direct LCD drawing as fallback.
  lgfx::LovyanGFX& g = spr_ok ? static_cast<lgfx::LovyanGFX&>(env_spr)
                              : static_cast<lgfx::LovyanGFX&>(lcd);
  const int panelX = spr_ok ? 0 : 2;
  const int panelY = spr_ok ? 0 : 168;

  const int sections = AIRMONITOR_SHOW_PRESSURE ? 3 : 2;
  const int sectionW = panelW / sections;
  const int yDigits = panelY + 22;
  const uint16_t onColor = lcd.color565(180, 245, 255);
  const uint16_t offColor = lcd.color565(18, 60, 62);
  const uint16_t boxBg = lcd.color565(8, 14, 24);
  const uint16_t boxBorder = lcd.color565(70, 170, 210);

  if (spr_ok) {
    env_spr.fillSprite(SPR_TRANSP);
  }
  for (int i = 0; i < sections; ++i) {
    int bx = panelX + i * sectionW + 1;
    int bw = sectionW - 2;
    g.fillRoundRect(bx, panelY, bw, panelH, 8, boxBg);
    g.drawRoundRect(bx, panelY, bw, panelH, 8, boxBorder);
  }

  // Keep original 7-seg glyph aspect ratio (W:H = 3:4).
  const int digitW = 24;
  const int digitH = 32;
  const int gap = 3;
  const int dotGap = 6;
  const int tempTotalW = digitW * 3 + gap * 2 + dotGap;
  int tX = panelX + sectionW / 2 - tempTotalW / 2;
  drawDigit(g, tX, yDigits, digitW, digitH, (t10 / 100) % 10, onColor, offColor);
  drawDigit(g, tX + digitW + gap, yDigits, digitW, digitH, (t10 / 10) % 10, onColor, offColor);
  int t3x = tX + (digitW + gap) * 2 + dotGap;
  drawDigit(g, t3x, yDigits, digitW, digitH, t10 % 10, onColor, offColor);
  g.fillCircle(t3x - 4, yDigits + digitH - 4, 2, onColor);

  int hX = panelX + sectionW + sectionW / 2 - tempTotalW / 2;
  drawDigit(g, hX, yDigits, digitW, digitH, (h10 / 100) % 10, onColor, offColor);
  drawDigit(g, hX + digitW + gap, yDigits, digitW, digitH, (h10 / 10) % 10, onColor, offColor);
  int h3x = hX + (digitW + gap) * 2 + dotGap;
  drawDigit(g, h3x, yDigits, digitW, digitH, h10 % 10, onColor, offColor);
  g.fillCircle(h3x - 4, yDigits + digitH - 4, 2, onColor);

  g.setFont(&fonts::Font2);
  g.setTextColor(lcd.color565(120, 200, 235));
  g.setTextDatum(top_left);
  g.drawString("TEMP", panelX + 4, panelY + 4);
  g.drawString("HUMI", panelX + sectionW + 4, panelY + 4);

  // Unit text: right-aligned above the last digit, staying within that digit width.
  g.setTextDatum(top_right);
  g.drawString("C", t3x + digitW - 1, panelY + 6);
  g.drawString("%", h3x + digitW - 1, panelY + 6);

#if AIRMONITOR_SHOW_PRESSURE
  const int pDigitW = digitW;
  const int pDigitH = digitH;
  const int pGap = 2;
  const int pTotalW = pDigitW * 4 + pGap * 3;
  int pX = panelX + sectionW * 2 + sectionW / 2 - pTotalW / 2;
  drawDigit(g, pX, yDigits, pDigitW, pDigitH, (p / 1000) % 10, onColor, offColor);
  drawDigit(g, pX + pDigitW + pGap, yDigits, pDigitW, pDigitH, (p / 100) % 10, onColor, offColor);
  drawDigit(g, pX + (pDigitW + pGap) * 2, yDigits, pDigitW, pDigitH, (p / 10) % 10, onColor, offColor);
  drawDigit(g, pX + (pDigitW + pGap) * 3, yDigits, pDigitW, pDigitH, p % 10, onColor, offColor);
  g.drawString("PRES", panelX + sectionW * 2 + 4, panelY + 4);
  g.drawString("hPa", pX + (pDigitW + pGap) * 3 + pDigitW - 1, panelY + 6);
#endif

  if (spr_ok) {
    env_spr.pushSprite(2, 168, SPR_TRANSP);
  }
}

void drawStatus(const char* msg, uint16_t color) {
  (void)msg;
  (void)color;
}

void refreshAll(bool force = false) {
  drawCO2(co2_ppm, force);
  drawEnv(temp_c, humid_rh, press_hpa, force);
}

void runForcedCalibration() {
  const int w = lcd.width();
  const int h = lcd.height();
  const uint16_t bg = lcd.color565(6, 10, 18);

  lcd.fillScreen(bg);
  lcd.setTextDatum(middle_center);
  lcd.setFont(&fonts::Font4);
  lcd.setTextColor(lcd.color565(200, 245, 255));
  lcd.drawString("CO2 Calibration", w / 2, h / 2 - 40);
  lcd.setFont(&fonts::Font2);
  lcd.setTextColor(lcd.color565(140, 230, 255));
  lcd.drawString("Keep the sensor in fresh air", w / 2, h / 2 - 12);
  Serial.println("FRC: warming up in fresh air...");

  const uint32_t start = millis();
  int lastShown = -1;
  while (millis() - start < CAL_WARMUP_MS) {
    if (scd30.dataReady()) {
      scd30.read();
    }
    int remain = (int)((CAL_WARMUP_MS - (millis() - start)) / 1000);
    if (remain != lastShown) {
      lastShown = remain;
      lcd.fillRect(0, h / 2 + 8, w, 32, bg);
      lcd.setFont(&fonts::Font4);
      lcd.drawString(String(remain) + " s", w / 2, h / 2 + 24);
    }
    delay(200);
  }

  bool ok = scd30.forceRecalibrationWithReference(CAL_REFERENCE_PPM);
  lcd.fillRect(0, h / 2 + 8, w, 32, bg);
  lcd.setFont(&fonts::Font4);
  lcd.setTextColor(ok ? lcd.color565(80, 255, 220) : lcd.color565(255, 90, 90));
  lcd.drawString(ok ? "Calibration OK" : "Calibration FAILED", w / 2, h / 2 + 24);
  Serial.println(ok ? "FRC done (reference 400 ppm)" : "FRC failed");
  delay(3000);
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_CAL_BUTTON, INPUT_PULLUP);

  lcd.init();
  lcd.setRotation(1);  // 320x240 (180 deg from rotation 3)
  lcd.setBrightness(255);

  co2_spr.setColorDepth(16);
  env_spr.setColorDepth(16);
  spr_ok = co2_spr.createSprite(lcd.width() - 4, 128) != nullptr;
  if (spr_ok && env_spr.createSprite(lcd.width() - 4, 70) == nullptr) {
    co2_spr.deleteSprite();
    spr_ok = false;
  }
  if (!spr_ok) {
    Serial.println("sprite alloc failed; falling back to direct drawing");
  }

  drawBackground();
  drawDisplayFrame();

  Wire.begin(22, 27);  // SDA=22, SCL=27
  Wire.setClock(100000);

  sensor_ok = scd30.begin();
  if (sensor_ok) {
    scd30.setTemperatureOffset(AIRMONITOR_TEMP_OFFSET_C100);
    if (digitalRead(PIN_CAL_BUTTON) == LOW) {
      runForcedCalibration();
      drawBackground();
      drawDisplayFrame();
    }
  } else {
    co2_ppm = 0;
    temp_c = 0.0f;
    humid_rh = 0.0f;
  }

  refreshAll(true);
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
