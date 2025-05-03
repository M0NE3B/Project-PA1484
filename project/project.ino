#include "esp_adc_cal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pin_config.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include <time.h>

// Remember to remove these before commiting in GitHub
String ssid = "TP-link_S41F";
String password = "325v2hk4";

// "tft" is the graphics libary, which has functions to draw on the screen
TFT_eSPI tft = TFT_eSPI();

// Display dimentions
#define DISPLAY_WIDTH 320
#define DISPLAY_HEIGHT 170

// SMHI forecast endpoint for Karlskrona
const char *forecastUrl = "https://opendata-download-metfcst.smhi.se/"
                          "api/category/pmp3g/version/2/"
                          "geotype/point/lon/15.59/lat/56.18/data.json";

// Structure to hold one hour of forecast data
struct ForecastHour {
  String time;
  float temp;
  int symbol;
};

ForecastHour forecast24h[24];

const char *currentCity = "Karlskrona";

// US3.2B: Scroll through 6 h windows in the 24 h forecast
static const uint8_t kScrollHours =
    6; // how many hours to jump per button press
static uint8_t forecastStart =
    0; // index (0–23) of the forecast hour to start from

// Menu (US2.1 / US2.2B)
enum Screen {
  SCREEN_MENU,
  SCREEN_FORECAST,
  // SCREEN_HISTORY,
  // SCREEN_SETTINGS
};
Screen currentScreen = SCREEN_MENU;

// Menu items and index
const char *menuItems[] = {"Forecast", "History", "Settings"};
const uint8_t menuItemCount = sizeof(menuItems) / sizeof(menuItems[0]);
uint8_t menuIndex = 0;

// Forward declaration for menu renderer
void showMenuScreen();

// Dummy JSON buffer (not used in fetch)
StaticJsonDocument<10> dummyDoc;

// Function prototypes
void showBootScreen();
int centerX(const char *msg, int textSize);
bool fetch24hForecast();
void displayForecastCard(uint8_t idx);
void drawWeatherSymbol(int x, int y, int symbol, int r);

/**
 * Setup function
 * This function is called once when the program starts to initialize the
 * program and set up the hardware. Carefull when modifying this function.
 */
void setup() {
  // Initialize Serial for debugging
  Serial.begin(115200);
  // Wait for the Serial port to be ready
  while (!Serial)
    ;
  Serial.println("Starting ESP32 program...");
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  pinMode(PIN_BUTTON_1, INPUT_PULLUP);
  pinMode(PIN_BUTTON_2, INPUT_PULLUP);

  // Connect to WIFI
  WiFi.begin(ssid, password);

  // Will be stuck here until a proper wifi is configured
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Connecting to WiFi...", 10, 10);
    Serial.println("Attempting to connect to WiFi...");
  }

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString("Connected to WiFi", 10, 10);
  Serial.println("Connected to WiFi");

  // Show boot screen (User story US1.1)
  showBootScreen();

  // Menu startup (US2.1 /2.2B )
  currentScreen = SCREEN_MENU;
  Serial.println("Showing Menu…");
  showMenuScreen();
}

/**
 * This is the main loop function that runs continuously after setup.
 * Add your code here to perform tasks repeatedly.
 */
void loop() {
  bool down = (digitalRead(PIN_BUTTON_1) == LOW);
  bool up = (digitalRead(PIN_BUTTON_2) == LOW);

  // if any button pressed, debounce & decide
  if (down || up) {
    delay(100); // wait to see if they really meant both
    bool down2 = (digitalRead(PIN_BUTTON_1) == LOW);
    bool up2 = (digitalRead(PIN_BUTTON_2) == LOW);

    // Forecast navigation
    if (currentScreen == SCREEN_FORECAST) {
      if (down2 && up2) {
        // hold–both to go back to menu
        currentScreen = SCREEN_MENU;
        showMenuScreen();
        delay(200);
      } else if (down2) {
        // Go back one hour
        forecastStart = (forecastStart + 23) % 24;
        displayForecastCard(forecastStart);
      } else if (up2) {
        // Go forward one hour
        forecastStart = (forecastStart + 1) % 24;
        displayForecastCard(forecastStart);
      }

      // wait for release before next loop
      while (digitalRead(PIN_BUTTON_1) == LOW ||
             digitalRead(PIN_BUTTON_2) == LOW) {
      }
      return;
    }

    // Menu navigation
    if (currentScreen == SCREEN_MENU) {
      if (down2 && up2) {
        // Both tapped, select current menu item
        switch (menuIndex) {
        case 0:
          currentScreen = SCREEN_FORECAST;
          forecastStart = 0;
          if (fetch24hForecast())
            displayForecastCard(forecastStart);
          break;
          // case 1: … etc …
        }
      } else if (down2) {
        // Move selection down
        menuIndex = (menuIndex + 1) % menuItemCount;
        showMenuScreen();
      } else if (up2) {
        // Move selection up
        menuIndex = (menuIndex + menuItemCount - 1) % menuItemCount;
        showMenuScreen();
      }

      // wait until both released
      while (digitalRead(PIN_BUTTON_1) == LOW ||
             digitalRead(PIN_BUTTON_2) == LOW) {
      }
      return;
    }
  }
}

// Centering helper for texts
int centerX(const char *msg, int textSize) {
  return (DISPLAY_WIDTH - tft.textWidth(msg, textSize)) / 2;
}

// Display a simple boot screen with version info
void showBootScreen() {
  Serial.println("Showing boot screen…");
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);

  int cx = DISPLAY_WIDTH / 2;
  tft.drawString("Weather Dashboard",
                 cx - tft.textWidth("Weather Dashboard", 2) / 2, 40);
  tft.drawString("Version 3.0", cx - tft.textWidth("Version 3.0", 2) / 2, 70);
  tft.drawString("Team Group 6", cx - tft.textWidth("Team Group 6", 2) / 2,
                 100);

  delay(3000); // Show for 3 seconds
  Serial.println("Boot screen done");
  tft.fillScreen(TFT_BLACK);
}

// Fetch the next 24 hours of forecast from SMHI
bool fetch24hForecast() {
  Serial.println("Fetching 24h forecast…");

  HTTPClient http;
  // 1) Start the connection
  http.useHTTP10(true);
  http.begin(forecastUrl);
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");

  // 2) Send the request
  int httpCode = http.GET();
  Serial.print("HTTP GET code: ");
  Serial.println(httpCode);
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("HTTP GET failed: %d — %s\n", httpCode,
                  http.errorToString(httpCode).c_str());
    http.end();
    return false;
  }

  // 3) Parse directly from the stream
  const size_t JSON_CAPACITY = 200000;
  DynamicJsonDocument doc(JSON_CAPACITY);
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();

  if (err) {
    Serial.print("JSON parse failed: ");
    Serial.println(err.c_str());
    return false;
  }

  // 4) Extract the first 24 entries
  JsonArray ts = doc["timeSeries"].as<JsonArray>();
  Serial.printf("Got %u timeSeries entries\n", ts.size());
  for (int i = 0; i < 24 && i < ts.size(); i++) {
    String vt = ts[i]["validTime"].as<const char *>();
    forecast24h[i].time = vt.substring(11, 16);
    forecast24h[i].temp = 0;
    forecast24h[i].symbol = -1;
    for (JsonObject p : ts[i]["parameters"].as<JsonArray>()) {
      const char *name = p["name"].as<const char *>();
      if (strcmp(name, "t") == 0)
        forecast24h[i].temp = p["values"][0].as<float>();
      else if (strcmp(name, "Wsymb2") == 0)
        forecast24h[i].symbol = p["values"][0].as<int>();
    }
  }

  Serial.println("Forecast parsed successfully");
  return true;
}

void displayForecastCard(uint8_t idx) {
  // Log to Serial
  Serial.printf("Forecast → %02u: %s, %.1f°C, code %d\n", idx,
                forecast24h[idx].time.c_str(), forecast24h[idx].temp,
                forecast24h[idx].symbol);

  // 1) Clear screen
  tft.fillScreen(TFT_BLACK);

  // 2) Header + city
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("24 hour forecast:", 10, 10);

  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString(currentCity, 10, 35);

  // 3) Time (big) + temperature underneath
  tft.setTextSize(3);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(forecast24h[idx].time, 10, 70);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  String tmp = String(forecast24h[idx].temp, 1) + "°C";
  tft.drawString(tmp, 10, 110);

  // 4) Huge icon on the right
  const int R = 30;
  int iconX = DISPLAY_WIDTH - R - 20;
  int iconY = 80 + R / 2;
  drawWeatherSymbol(iconX, iconY, forecast24h[idx].symbol, R);

  // 5) Prev / Next labels:
  tft.setTextSize(1);
  // Next >  (top-right)
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Next >", DISPLAY_WIDTH - 10, 10);
  // < Prev (bottom-right)
  tft.setTextDatum(BR_DATUM);
  tft.drawString("Prev >", DISPLAY_WIDTH - 10, DISPLAY_HEIGHT - 10);
}

void drawWeatherSymbol(int x, int y, int symbol, int r) {
  tft.setTextDatum(MC_DATUM);
  int off = r / 2;

  switch (symbol) {
  // — Clear & Sun —
  case 1: { // Clear sky
    tft.fillCircle(x, y, r, TFT_YELLOW);

    int rayLen = r * 0.4;
    int gap = r * 0.1;
    int start = r + gap;

    // Sun rays
    tft.drawLine(x - start, y - start, x - start - rayLen, y - start - rayLen,
                 TFT_YELLOW);
    tft.drawLine(x + start, y - start, x + start + rayLen, y - start - rayLen,
                 TFT_YELLOW);
    tft.drawLine(x - start, y + start, x - start - rayLen, y + start + rayLen,
                 TFT_YELLOW);
    tft.drawLine(x + start, y + start, x + start + rayLen, y + start + rayLen,
                 TFT_YELLOW);
  } break;

  case 2: // nearly clear
    tft.fillCircle(x, y, r, TFT_YELLOW);
    break;

  // — Peeking sun behind cloud —
  case 3: // variable cloudiness
  case 4: // halfclear sky
    // sun
    tft.fillCircle(x - off, y - off, r, TFT_YELLOW);
    tft.drawCircle(x - off, y - off, r, TFT_LIGHTGREY);
    // cloud
    drawCloud(x + off / 1.5, y, r, TFT_WHITE);
    break;

  // — Cloudy / Overcast —
  case 5: // cloudy
  case 6: // overcast
    drawCloud(x, y, r, TFT_WHITE);
    break;

  // — Fog —
  case 7: {
    uint16_t col = TFT_LIGHTGREY;
    int spacing = r / 3;
    int wavelength = r * 2;
    int amplitude = r / 4;
    for (int line = 0; line < 3; line++) {
      int y0 = y - r + line * spacing * 2;
      for (int xx = x - r; xx <= x + r; xx++) {
        float t = float(xx - (x - r)) / wavelength * TWO_PI;
        int yy = y0 + sin(t) * amplitude;
        tft.drawPixel(xx, yy, col);
      }
    }
  } break;

  // — Rain & Showers —
  case 8:
  case 9:
  case 10: // showers
  case 18:
  case 19:
  case 20: // steady rain
    drawCloud(x, y - off / 2, r, TFT_LIGHTGREY);
    for (int i = -1; i <= 1; i++) {
      int dx = x + i * off;
      int dy = y + off;
      tft.fillTriangle(dx, dy, dx - r / 6, dy + r / 2, dx + r / 6, dy + r / 2,
                       TFT_BLUE);
    }
    break;

  // — Thunder —
  case 11:
  case 21: { // Thunder
    // draw cloud behind the bolt
    drawCloud(x, y - off / 2, r, TFT_LIGHTGREY);
    // Bolt
    int bx = x, by = y + off / 4;
    tft.drawLine(bx, by, bx - r / 4, by + r / 2, TFT_YELLOW);
    tft.drawLine(bx - r / 4, by + r / 2, bx + r / 4, by + r / 2, TFT_YELLOW);
    tft.drawLine(bx + r / 4, by + r / 2, bx, by + r, TFT_YELLOW);
  } break;

  // — Sleet —
  case 12:
  case 13:
  case 14:
  case 22:
  case 23:
  case 24:
    drawCloud(x, y - off / 2, r, TFT_LIGHTGREY);
    {
      int dy = y + off;
      // water-drop
      int ddx = x - off / 2;
      tft.fillTriangle(ddx, dy, ddx - r / 6, dy + r / 2, ddx + r / 6,
                       dy + r / 2, TFT_BLUE);
      // snowflake
      int fx = x + off / 2, fy = y + off, sr = r / 4;
      tft.drawLine(fx - sr, fy, fx + sr, fy, TFT_WHITE);
      tft.drawLine(fx, fy - sr, fx, fy + sr, TFT_WHITE);
      tft.drawLine(fx - sr, fy - sr, fx + sr, fy + sr, TFT_WHITE);
      tft.drawLine(fx - sr, fy + sr, fx + sr, fy - sr, TFT_WHITE);
      tft.drawLine(fx - sr, fy - sr / 2, fx + sr, fy + sr / 2, TFT_WHITE);
    }
    break;

  // — Snow —
  case 15:
  case 16:
  case 17:
  case 25:
  case 26:
  case 27:
    drawCloud(x, y - off / 2, r, TFT_LIGHTGREY);
    {
      uint16_t col = TFT_WHITE;
      int sr = r / 4, oy = y + off;
      for (int i = -1; i <= 1; i += 2) {
        int sx = x + i * off;
        // Snowflake
        tft.drawLine(sx - sr, oy, sx + sr, oy, col);
        tft.drawLine(sx, oy - sr, sx, oy + sr, col);
        tft.drawLine(sx - sr, oy - sr, sx + sr, oy + sr, col);
        tft.drawLine(sx - sr, oy + sr, sx + sr, oy - sr, col);
        tft.drawLine(sx - sr, oy - sr / 2, sx + sr, oy + sr / 2, col);
      }
    }
    break;

  // — Fallback —
  default:
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("?", x, y);
    break;
  }

  tft.setTextDatum(TL_DATUM);
}

// Render the Menu screen
void showMenuScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);

  // 1) Title
  tft.setTextSize(2);
  tft.setTextColor(TFT_BLUE, TFT_BLACK);
  tft.drawString("Menu:", 10, 10);

  // 2) List all menu items, highlighting the current one
  const int lineHeight = 30;
  const int startY = 40;

  for (uint8_t i = 0; i < menuItemCount; i++) {
    int y = startY + i * lineHeight;

    if (i == menuIndex) {
      // highlight background for selected item
      tft.fillRect(0, y - 2, DISPLAY_WIDTH, lineHeight, TFT_DARKGREY);
      tft.setTextColor(TFT_YELLOW, TFT_DARKGREY);
    } else {
      tft.setTextColor(TFT_WHITE);
    }

    tft.setTextSize(2);
    tft.drawString(menuItems[i], 10, y);
  }
}

void drawCloud(int x, int y, int r, uint16_t Cloud_Color) {
  // Bigger circle
  tft.fillCircle(x + r / 4, y, r, Cloud_Color);
  tft.drawCircle(x + r / 4, y, r, TFT_DARKGREY);
  // Smaller circle
  tft.fillCircle(x - r * 0.8, y + r / 2.8, r * 0.7, Cloud_Color);
  tft.drawCircle(x - r * 0.8, y + r / 2.8, r * 0.7, TFT_DARKGREY);
  // Fix outline
  tft.fillCircle(((x + r / 4) + (x - r * 0.8)) / 2, (y + (y + r / 2.8)) / 2,
                 r * 0.7, Cloud_Color);
}

// TFT Pin check
//////////////////
// DO NOT TOUCH //
//////////////////
#if PIN_LCD_WR != TFT_WR || PIN_LCD_RD != TFT_RD || PIN_LCD_CS != TFT_CS ||    \
    PIN_LCD_DC != TFT_DC || PIN_LCD_RES != TFT_RST || PIN_LCD_D0 != TFT_D0 ||  \
    PIN_LCD_D1 != TFT_D1 || PIN_LCD_D2 != TFT_D2 || PIN_LCD_D3 != TFT_D3 ||    \
    PIN_LCD_D4 != TFT_D4 || PIN_LCD_D5 != TFT_D5 || PIN_LCD_D6 != TFT_D6 ||    \
    PIN_LCD_D7 != TFT_D7 || PIN_LCD_BL != TFT_BL ||                            \
    TFT_BACKLIGHT_ON != HIGH || 170 != TFT_WIDTH || 320 != TFT_HEIGHT
#error                                                                         \
    "Error! Please make sure <User_Setups/Setup206_LilyGo_T_Display_S3.h> is selected in <TFT_eSPI/User_Setup_Select.h>"
#error                                                                         \
    "Error! Please make sure <User_Setups/Setup206_LilyGo_T_Display_S3.h> is selected in <TFT_eSPI/User_Setup_Select.h>"
#error                                                                         \
    "Error! Please make sure <User_Setups/Setup206_LilyGo_T_Display_S3.h> is selected in <TFT_eSPI/User_Setup_Select.h>"
#error                                                                         \
    "Error! Please make sure <User_Setups/Setup206_LilyGo_T_Display_S3.h> is selected in <TFT_eSPI/User_Setup_Select.h>"
#endif

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#error                                                                         \
    "The current version is not supported for the time being, please use a version below Arduino ESP32 3.0"
#endif