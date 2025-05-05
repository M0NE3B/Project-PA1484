#include "esp_adc_cal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pin_config.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
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

// City table & dynamic URL builders
struct City {
  const char *name;
  float lat, lon;
};

constexpr City cities[] = {{"Karlskrona", 56.18f, 15.59f},
                           {"Stockholm", 59.3293f, 18.0686f},
                           {"Gothenburg", 57.7089f, 11.9746f},
                           {"Malmo", 55.6050f, 13.0038f}};

constexpr uint8_t cityCount = sizeof(cities) / sizeof(cities[0]);
uint8_t currentCityIndex = 0; // will sync in loadSettings()

struct Settings {
  String city;
  String parameter;
  bool useFahrenheit;
};

Settings settings;
Preferences prefs;

struct Param {
  const char *key;
  const char *label;
};
const Param paramOptions[] = {{"temperature_2m", "Temperature"},
                              {"relative_humidity_2m", "Relative Humidity"},
                              {"windspeed_10m", "Windspeed"},
                              {"precipitation", "Precipitation"}};
const uint8_t paramCount = sizeof(paramOptions) / sizeof(paramOptions[0]);
uint8_t paramIndex = 0;

void loadSettings() {
  prefs.begin("weather", false);
  settings.city = prefs.getString("city", "Karlskrona");
  settings.parameter = prefs.getString("param", paramOptions[0].key);
  settings.useFahrenheit = prefs.getBool("fahrenheit", false);
  prefs.end();
  // sync the index so makeForecastUrl/HistoryUrl pick the right lat/lon
  for (uint8_t i = 0; i < cityCount; ++i) {
    if (settings.city == cities[i].name) {
      currentCityIndex = i;
      break;
    }
  }
}

void saveSettings() {
  prefs.begin("weather", false);
  prefs.putString("city", settings.city);
  prefs.putString("param", settings.parameter);
  prefs.putBool("fahrenheit", settings.useFahrenheit);
  prefs.end();
}

// Structure to hold one hour of forecast data
struct ForecastHour {
  String time;
  float temp;
  int symbol;
};

ForecastHour forecast24h[24];

// hourly/7‑day history buffer
static const int HISTORY_DAYS = 30;
static const int HOURS_PER_DAY = 24;

// labels and values [day][hour]
String historyDayLabels[HISTORY_DAYS];
float historyHourly[HISTORY_DAYS][HOURS_PER_DAY];
int historyDaysFetched = 0;

// which day is showing now (0 = oldest, HISTORY_DAYS−1 = most recent)
int currentHistoryDay = HISTORY_DAYS - 1;

// US3.2B: Scroll through 6 h windows in the 24 h forecast
static uint8_t forecastStart = 0;

// after your HISTORY_DAYS/HOURS_PER_DAY definitions
static int historyHoursFetched[HISTORY_DAYS];

// human‑friendly names for parameters
String parameterLabel(const String &p) {
  if (p == "temperature_2m")
    return "Temperature";
  if (p == "relative_humidity_2m")
    return "Humidity";
  if (p == "windspeed_10m")
    return "Wind Speed";
  if (p == "precipitation")
    return "Precipitation";
  return p;
}

// round helpers
int roundDown5(int v) { return (v / 5) * 5; }
int roundUp5(int v) { return ((v + 4) / 5) * 5; }

// Screens
enum Screen {
  SCREEN_MENU,
  SCREEN_FORECAST,
  SCREEN_HISTORY,
  SCREEN_SETTINGS,
  SCREEN_PARAM_SELECT,
  SCREEN_CITY_SELECT
};

Screen currentScreen = SCREEN_MENU;

String makeForecastUrl() {
  auto &c = cities[currentCityIndex];
  String url = String("https://opendata-download-metfcst.smhi.se/") +
               "api/category/pmp3g/version/2/" + "geotype/point/lon/" +
               String(c.lon, 5) + "/lat/" + String(c.lat, 5) + "/data.json";

  Serial.print("Forecast URL: ");
  Serial.println(url);

  return url;
}

String makeHistoryUrl() {
  auto &c = cities[currentCityIndex];

  // compute today and 7 days ago in UTC
  time_t now = time(nullptr);
  struct tm tm_end, tm_start;
  gmtime_r(&now, &tm_end);
  tm_start = tm_end;
  tm_start.tm_mday -= HISTORY_DAYS; // go back N days
  mktime(&tm_start);                // normalize

  char bufStart[11], bufEnd[11];
  strftime(bufStart, sizeof(bufStart), "%Y-%m-%d", &tm_start);
  strftime(bufEnd, sizeof(bufEnd), "%Y-%m-%d", &tm_end);

  char url[256];
  String fieldList = String(paramOptions[0].key);
  for (uint8_t i = 1; i < paramCount; ++i) {
    fieldList += "," + String(paramOptions[i].key);
  }

  snprintf(url, sizeof(url),
           "https://archive-api.open-meteo.com/v1/archive?"
           "latitude=%.5f&longitude=%.5f&"
           "hourly=%s&"
           "start_date=%s&end_date=%s&timezone=Europe%%2FStockholm",
           c.lat, c.lon, fieldList.c_str(), bufStart, bufEnd);

  Serial.print("History URL: ");
  Serial.println(url);
  return String(url);
}

// Menu items and index
const char *menuItems[] = {"Forecast", "History", "Settings"};
const uint8_t menuItemCount = sizeof(menuItems) / sizeof(menuItems[0]);
uint8_t menuIndex = 0;

// Settings items and index
const char *settingsItems[] = {"City", "Parameter", "Change unit",
                               "Reset to default"};
const uint8_t settingsCount = sizeof(settingsItems) / sizeof(settingsItems[0]);
uint8_t settingsIndex = 0;

static const uint8_t VISIBLE_CITY_COUNT = 5; // show 5 at a time
uint8_t cityIndex = 0;  // which entry is currently highlighted
uint8_t cityScroll = 0; // top of the scrolling window

// Forward declaration for menu renderer
void showMenuScreen();

void showSettingsScreen();

// Function prototypes
void showBootScreen();
bool fetch24hForecast();
void displayHistoryGraph(int startDay);
void displayForecastCard(uint8_t idx);
void drawWeatherSymbol(int x, int y, int symbol, int r);
bool fetchHistoryData();

// convert raw °C → display unit
float toDisplayTemp(float celsius) {
  if (settings.useFahrenheit) {
    return celsius * 9.0f / 5.0f + 32.0f;
  } else {
    return celsius;
  }
}
// helper for unit label
const char *unitLabel() { return settings.useFahrenheit ? "F" : "C"; }

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

  configTime(0, 0, "pool.ntp.org", "time.nist.gov"); // set up NTP
  delay(2000); // give it a moment to fetch time

  // Show boot screen (User story US1.1)
  showBootScreen();

  // Loading settings
  loadSettings();

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

  delay(100);

  down = (digitalRead(PIN_BUTTON_1) == LOW);
  up = (digitalRead(PIN_BUTTON_2) == LOW);

  if (!down && !up)
    return;

  // HISTORY
  if (currentScreen == SCREEN_HISTORY) {
    if (down && up) {
      currentScreen = SCREEN_MENU;
      Serial.println("Returning to menu…");
      showMenuScreen();
      delay(300);
    } else if (down) {
      // older day
      currentHistoryDay = max(0, currentHistoryDay - 1);
      displayHistoryGraph(currentHistoryDay);
      delay(200);
    } else if (up) {
      // newer day
      currentHistoryDay = min(historyDaysFetched - 1, currentHistoryDay + 1);
      displayHistoryGraph(currentHistoryDay);
      delay(200);
    }
    while (digitalRead(PIN_BUTTON_1) == LOW ||
           digitalRead(PIN_BUTTON_2) == LOW) {
    }
    return;
  }

  // FORECAST
  if (currentScreen == SCREEN_FORECAST) {
    if (down && up) {
      currentScreen = SCREEN_MENU;
      Serial.println("Returning to menu…");
      showMenuScreen();
      delay(200);
    } else if (down) {
      forecastStart = (forecastStart + 23) % 24;
      displayForecastCard(forecastStart);
    } else if (up) {
      forecastStart = (forecastStart + 1) % 24;
      displayForecastCard(forecastStart);
    }
    while (digitalRead(PIN_BUTTON_1) == LOW ||
           digitalRead(PIN_BUTTON_2) == LOW) {
    }
    return;
  }

  // SETTINGS
  if (currentScreen == SCREEN_SETTINGS) {
    if (down && up) {
      unsigned long t0 = millis();
      while (digitalRead(PIN_BUTTON_1) == LOW &&
             digitalRead(PIN_BUTTON_2) == LOW) {
        if (millis() - t0 > 500) {
          currentScreen = SCREEN_MENU;
          Serial.println("Returning to menu…");
          showMenuScreen();
          while (digitalRead(PIN_BUTTON_1) == LOW ||
                 digitalRead(PIN_BUTTON_2) == LOW) {
          }
          delay(300);
          return;
        }
      }
      switch (settingsIndex) {
      case 0: // City
        Serial.println("City settings");
        cityIndex = currentCityIndex;
        if (cityCount <= VISIBLE_CITY_COUNT) {
          cityScroll = 0;

        } else {
          cityScroll =
              min<uint8_t>(currentCityIndex, +cityCount - VISIBLE_CITY_COUNT);
        }
        currentScreen = SCREEN_CITY_SELECT;
        showCityScreen();
        // wait until user releases both buttons
        while (digitalRead(PIN_BUTTON_1) == LOW ||
               digitalRead(PIN_BUTTON_2) == LOW) {
        }
        return;
        break;
      case 1: // Parameter
        Serial.println("Parameter settings");
        for (uint8_t i = 0; i < paramCount; i++) {
          if (settings.parameter == paramOptions[i].key) {
            paramIndex = i;
            break;
          }
        }
        currentScreen = SCREEN_PARAM_SELECT;
        showParameterScreen();
        while (digitalRead(PIN_BUTTON_1) == LOW ||
               digitalRead(PIN_BUTTON_2) == LOW) {
        }
        return;
      case 2: // Unit
        Serial.println("Unit changed");
        settings.useFahrenheit = !settings.useFahrenheit;
        break;
      case 3:
        Serial.println("Reset settings");
        settings = {"Karlskrona", paramOptions[0].key, false};
        break;
      }
      saveSettings();
      showSettingsScreen();
      delay(300);
      return;
    }

    if (down) {
      settingsIndex = (settingsIndex + 1) % settingsCount;
      showSettingsScreen();
      delay(200);
    } else if (up) {
      settingsIndex = (settingsIndex + settingsCount - 1) % settingsCount;
      showSettingsScreen();
      delay(200);
    }
    return;
  }

  if (currentScreen == SCREEN_PARAM_SELECT) {
    if (down && up) {
      settings.parameter = paramOptions[paramIndex].key;
      saveSettings();
      currentScreen = SCREEN_SETTINGS;
      showSettingsScreen();

      while (digitalRead(PIN_BUTTON_1) == LOW ||
             digitalRead(PIN_BUTTON_2) == LOW) {
      }
      delay(300);
      return;
    }

    if (down) {
      paramIndex = (paramIndex + 1) % paramCount;
      showParameterScreen();
    }
    if (up) {
      paramIndex = (paramIndex + paramCount - 1) % paramCount;
      showParameterScreen();
    }
  }

  // CITY SELECT
  if (currentScreen == SCREEN_CITY_SELECT) {
    bool down = (digitalRead(PIN_BUTTON_1) == LOW);
    bool up = (digitalRead(PIN_BUTTON_2) == LOW);
    if (down && up) {
      // confirm selection
      currentCityIndex = cityIndex;
      settings.city = cities[cityIndex].name;
      saveSettings();

      Serial.print("City chosen: ");
      Serial.println(settings.city);

      currentScreen = SCREEN_SETTINGS;
      showSettingsScreen();
      // wait for release
      while (digitalRead(PIN_BUTTON_1) == LOW ||
             digitalRead(PIN_BUTTON_2) == LOW) {
      }
      delay(300);
      return;
    }
    if (down) {
      if (cityIndex < cityCount - 1) {
        cityIndex++;
        if (cityIndex >= cityScroll + VISIBLE_CITY_COUNT)
          cityScroll++;
      }
      showCityScreen();
      delay(200);
      return;
    }
    if (up) {
      if (cityIndex > 0) {
        cityIndex--;
        if (cityIndex < cityScroll)
          cityScroll--;
      }
      showCityScreen();
      delay(200);
      return;
    }
  }

  // MENU
  if (currentScreen == SCREEN_MENU) {
    if (down && up) {
      switch (menuIndex) {
      case 0:
        currentScreen = SCREEN_FORECAST;
        fetch24hForecast();
        displayForecastCard(forecastStart = 0);
        break;
      case 1:
        if (fetchHistoryData()) {
          // build a YYYY‑MM‑DD string for “now” in UTC
          time_t now = time(nullptr);
          struct tm now_tm;
          localtime_r(&now, &now_tm); //  ⟵ normalize into a tm struct
          char today[11];
          strftime(today, sizeof(today), "%Y-%m-%d", &now_tm);

          Serial.printf("  today=%s, lastDayLabel=%s\n", today,
                        historyDayLabels[currentHistoryDay].c_str());

          if (historyDayLabels[currentHistoryDay] == String(today)) {
            // re‑fetch 24h forecast into forecast24h[]
            fetch24hForecast();
            // copy each of the 24 forecast hours into historyHourly
            for (int h = 0; h < 24; h++) {
              historyHourly[currentHistoryDay][h] = forecast24h[h].temp;
            }
            // mark that you have exactly 24 valid hours today
            historyHoursFetched[currentHistoryDay] = 24;
          }

          currentScreen = SCREEN_HISTORY;
          displayHistoryGraph(currentHistoryDay);
        }
        break;
      case 2:
        currentScreen = SCREEN_SETTINGS;
        settingsIndex = 0;
        Serial.println("Showing settings...");
        showSettingsScreen();
        break;
      }
    } else if (down) {
      menuIndex = (menuIndex + 1) % menuItemCount;
      showMenuScreen();
    } else if (up) {
      menuIndex = (menuIndex + menuItemCount - 1) % menuItemCount;
      showMenuScreen();
    }
    while (digitalRead(PIN_BUTTON_1) == LOW ||
           digitalRead(PIN_BUTTON_2) == LOW) {
    }
    return;
  }
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
  http.begin(makeForecastUrl());
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
    forecast24h[i].time =
        String(ts[i]["validTime"].as<const char *>()).substring(11, 16);
    forecast24h[i].temp = 0;
    forecast24h[i].symbol = -1;
    for (JsonObject p : ts[i]["parameters"].as<JsonArray>()) {
      const char *name = p["name"].as<const char *>();
      if (strcmp(name, "t") == 0) {
        forecast24h[i].temp = p["values"][0].as<float>();
      } else if (strcmp(name, "Wsymb2") == 0) {
        forecast24h[i].symbol = p["values"][0].as<int>();
      }
    }
  }

  Serial.println("Forecast parsed successfully");
  return true;
}

// Fetch daily historical data from Open-Meteo, skipping any days with nulls
bool fetchHistoryData() {
  Serial.println("Fetching history…");
  String url = makeHistoryUrl();

  HTTPClient http;
  http.begin(url);
  int code = http.GET();
  Serial.printf("HTTP GET code: %d\n", code);
  if (code != HTTP_CODE_OK) {
    Serial.printf("HTTP GET failed: %d — %s\n", code,
                  http.errorToString(code).c_str());
    http.end();
    return false;
  }

  // Read the full payload
  String body = http.getString();
  http.end();

  // Parse JSON
  const size_t JSON_CAP = 200000;
  DynamicJsonDocument doc(JSON_CAP);
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.print("JSON parse failed: ");
    Serial.println(err.c_str());
    return false;
  }

  // Pull out the arrays
  JsonArray times = doc["hourly"]["time"].as<JsonArray>();
  JsonArray vals = doc["hourly"][settings.parameter.c_str()].as<JsonArray>();

  const uint8_t paramCount = sizeof(paramOptions) / sizeof(paramOptions[0]);

  int totalSize = times.size();
  int need = HISTORY_DAYS * HOURS_PER_DAY;
  int start = max(0, totalSize - need);
  int count = min(need, totalSize);

  // Clear previous data
  for (int d = 0; d < HISTORY_DAYS; d++) {
    historyHoursFetched[d] = 0;
  }

  // Fill, skipping days with any nulls
  for (int i = 0; i < count; i++) {
    int idx = start + i;
    int day = i / HOURS_PER_DAY;
    int hr = i % HOURS_PER_DAY;

    // If this hour is null, skip the rest of the day
    if (vals[idx].isNull()) {
      // advance i so that (i % 24) → 23, then the loop i++ jumps to next day
      i += (HOURS_PER_DAY - hr) - 1;
      continue;
    }

    // Store the timestamp (for the label) and the value
    const char *ts = times[idx].as<const char *>();
    historyDayLabels[day] = String(ts).substring(0, 10);
    historyHourly[day][hr] = vals[idx].as<float>();
    historyHoursFetched[day] = hr + 1;
  }

  // Count only the fully-populated days
  historyDaysFetched = 0;
  for (int d = 0; d < HISTORY_DAYS; d++) {
    if (historyHoursFetched[d] == HOURS_PER_DAY) {
      historyDaysFetched++;
    }
  }
  currentHistoryDay = historyDaysFetched - 1;

  Serial.printf("  Got %d full days\n", historyDaysFetched);
  return historyDaysFetched > 0;
}

void displayForecastCard(uint8_t idx) {
  Serial.printf("Forecast %02u: %s, %.1f°C, code %d\n", idx,
                forecast24h[idx].time.c_str(), forecast24h[idx].temp,
                forecast24h[idx].symbol);

  tft.fillScreen(TFT_BLACK);

  // Header + city
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("24 hour forecast:", 10, 10);

  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString(cities[currentCityIndex].name, 10, 35);

  // Time + temperature
  tft.setTextSize(3);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(forecast24h[idx].time, 10, 70);

  float dispTemp = toDisplayTemp(forecast24h[idx].temp);
  String tempStr = String(dispTemp, 1);

  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.setCursor(10, 110);
  tft.print(tempStr);

  int16_t x = 15 + tft.textWidth(tempStr);
  int16_t y = 119 - tft.fontHeight() / 4;
  tft.drawCircle(x, y, 2, TFT_YELLOW);
  tft.drawString(unitLabel(), x + 6, 110);

  // Icon
  const int R = 30;
  int iconX = DISPLAY_WIDTH - R - 20;
  int iconY = 80 + R / 2;
  drawWeatherSymbol(iconX, iconY, forecast24h[idx].symbol, R);

  // Prev / Next
  tft.setTextSize(1);

  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Next >", DISPLAY_WIDTH - 10, 10);

  tft.setTextDatum(BR_DATUM);
  tft.drawString("Prev >", DISPLAY_WIDTH - 10, DISPLAY_HEIGHT - 10);
}

// Precompute Y‑ticks for temperature
const int TEMP_C_TICKS[] = {-10, 0, 10, 20, 30};
const int NUM_C_TICKS = sizeof(TEMP_C_TICKS) / sizeof(TEMP_C_TICKS[0]);

void displayHistoryGraph(int dayIdx) {
  tft.fillScreen(TFT_BLACK);

  // 1) Header split left/right
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  // left: param + date
  tft.setTextDatum(TL_DATUM);
  String leftHdr = String(parameterLabel(settings.parameter)) + " on " +
                   historyDayLabels[dayIdx];
  tft.drawString(leftHdr, 10, 10);
  // right: city
  tft.setTextDatum(TR_DATUM);
  tft.drawString(cities[currentCityIndex].name, DISPLAY_WIDTH - 10, 10);

  // 2) Margins and axes endpoints
  const int M_L = 40, M_R = 10, M_T = 25, M_B = 20;
  const int gx0 = M_L;
  const int gy0 = DISPLAY_HEIGHT - M_B;
  const int gx1 = DISPLAY_WIDTH - M_R;
  const int gy1 = M_T;

  // 3) Draw axes
  tft.drawLine(gx0, gy0, gx1, gy0, TFT_WHITE);
  tft.drawLine(gx0, gy0, gx0, gy1, TFT_WHITE);

  // 4) Compute dynamic Y‑bounds over only hours that exist
  int hoursAvail = historyHoursFetched[dayIdx];
  float mn = 1e6, mx = -1e6;
  for (int h = 0; h < hoursAvail; h++) {
    float v = historyHourly[dayIdx][h];
    if (settings.parameter == "t")
      v = toDisplayTemp(v);
    mn = min(mn, v);
    mx = max(mx, v);
  }
  int yMin = roundDown5(floor(mn));
  int yMax = roundUp5(ceil(mx));
  if (yMin == yMax)
    yMax = yMin + 5;

  // 5) Y‑ticks every 5 units
  tft.setTextSize(1);
  tft.setTextDatum(TR_DATUM);
  int tickCount = 5;
  float rawStep = float(yMax - yMin) / (tickCount - 1);
  int step5 = roundUp5(ceil(rawStep)); // e.g. 17→20, 3→5

  for (int v = yMin; v <= yMax; v += step5) {
    int yy = map(v, yMin, yMax, gy0, gy1);
    tft.drawLine(gx0 - 3, yy, gx0, yy, TFT_WHITE);
    tft.drawString(String(v), gx0 - 5, yy);
  }

  // 6) X‑ticks every 4h from 0→24
  tft.setTextDatum(TC_DATUM);
  for (int xh = 0; xh <= HOURS_PER_DAY; xh += 4) {
    int xx = gx0 + (gx1 - gx0) * xh / HOURS_PER_DAY;
    tft.drawLine(xx, gy0, xx, gy0 + 3, TFT_WHITE);
    tft.drawString(String(xh), xx, gy0 + 5);
  }

  // 7) Plot the line+dots only for valid hours
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  int prevX = -1, prevY = -1;
  for (int h = 0; h < hoursAvail; h++) {
    float v = historyHourly[dayIdx][h];
    if (settings.parameter == "t")
      v = toDisplayTemp(v);
    int x = gx0 + (gx1 - gx0) * h / HOURS_PER_DAY;
    int y = map(v, yMin, yMax, gy0, gy1);

    // dot
    tft.fillCircle(x, y, 3, TFT_YELLOW);
    // line
    if (prevX >= 0)
      tft.drawLine(prevX, prevY, x, y, TFT_YELLOW);

    prevX = x;
    prevY = y;
  }
}

void showSettingsScreen() {
  tft.fillScreen(TFT_BLACK);

  // Title
  tft.setTextSize(2);
  tft.setTextColor(TFT_BLUE, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Settings:", 10, 10);

  // Options list
  const int lineH = 30;
  for (uint8_t i = 0; i < settingsCount; ++i) {
    int y = 40 + i * lineH;
    if (i == settingsIndex) {
      tft.fillRect(0, y - 2, DISPLAY_WIDTH, lineH, TFT_DARKGREY);
      tft.setTextColor(TFT_YELLOW, TFT_DARKGREY);
    } else {
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
    }
    tft.setTextSize(2);
    tft.drawString(settingsItems[i], 10, y);
  }

  // Current values
  tft.setTextSize(1);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);

  String sCity = "City: " + settings.city;
  String sParam = "Parameter: ";
  for (uint8_t i = 0; i < paramCount; ++i) {
    if (settings.parameter == paramOptions[i].key) {
      sParam += paramOptions[i].label;
      break;
    }
  }
  String sUnits = "Units:";

  // Measure widths to align left edges
  int wCity = tft.textWidth(sCity);
  int wParam = tft.textWidth(sParam);
  int wUnits = tft.textWidth(sUnits);
  int maxW = max(max(wCity, wParam), wUnits);

  int leftX = DISPLAY_WIDTH - 10 - maxW;

  int fh = tft.fontHeight();
  int vgap = fh + 4;
  int startY = 12;

  tft.drawString(sCity, leftX, startY);
  tft.drawString(sParam, leftX, startY + vgap);
  tft.drawString(sUnits, leftX, startY + 2 * vgap);

  int cx = leftX + wUnits + 4;
  int cy = startY + 2 * vgap - fh / 2 + 3;
  tft.drawCircle(cx, cy, 1, TFT_GREEN);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(settings.useFahrenheit ? "F" : "C", cx + 3 + 2,
                 startY + 2 * vgap);
}

void showParameterScreen() {
  tft.fillScreen(TFT_BLACK);

  // 1) Header
  tft.setTextSize(2);
  tft.setTextColor(TFT_BLUE, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Choose parameter:", 10, 10);

  // 2) List items
  tft.setTextSize(2);
  tft.setTextDatum(TL_DATUM);
  int fh = tft.fontHeight();
  int lineH = fh + 6;
  int startY = 50;

  for (uint8_t i = 0; i < paramCount; ++i) {
    int y = startY + i * lineH;
    if (i == paramIndex) {
      // highlight the selected row
      tft.fillRect(0, y - 2, DISPLAY_WIDTH, lineH, TFT_DARKGREY);
      tft.setTextColor(TFT_YELLOW, TFT_DARKGREY);
    } else {
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
    }
    tft.drawString(paramOptions[i].label, 10, y);
  }
}

void showCityScreen() {
  tft.fillScreen(TFT_BLACK);

  // 1) Draw the header from the top‑left
  tft.setTextSize(2);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_BLUE, TFT_BLACK);
  tft.drawString("Choose city:", 10, 10);

  // 2) Draw the list in the same datum & color
  tft.setTextSize(2);
  tft.setTextDatum(TL_DATUM);
  int fh = tft.fontHeight();
  int lineH = fh + 6;
  int startY = 50;

  for (uint8_t i = 0; i < VISIBLE_CITY_COUNT; ++i) {
    uint8_t idx = cityScroll + i;
    if (idx >= cityCount)
      break;

    int y = startY + i * lineH;
    bool selected = (idx == cityIndex);

    if (selected) {
      tft.fillRect(0, y - 2, DISPLAY_WIDTH, lineH, TFT_DARKGREY);
      tft.setTextColor(TFT_YELLOW, TFT_DARKGREY);
    } else {
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
    }
    tft.drawString(cities[idx].name, 10, y);
  }

  // 3) Finally draw the arrows (centered) with their own datum & color
  tft.setTextSize(1);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  if (cityScroll > 0) {
    tft.drawString("^", DISPLAY_WIDTH / 2, startY - lineH / 2);
  }
  if (cityScroll + VISIBLE_CITY_COUNT < cityCount) {
    tft.drawString("v", DISPLAY_WIDTH / 2, startY + VISIBLE_CITY_COUNT * lineH);
  }
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
    "Error! Please make sure <User_Setups/Setup06_LilyGo_T_Display_S3.h> is selected in <TFT_eSPI/User_Setup_Select.h>"
#error                                                                         \
    "Error! Please make sure <User_Setups/Setup06_LilyGo_T_Display_S3.h> is selected in <TFT_eSPI/User_Setup_Select.h>"
#error                                                                         \
    "Error! Please make sure <User_Setups/Setup06_LilyGo_T_Display_S3.h> is selected in <TFT_eSPI/User_Setup_Select.h>"
#error                                                                         \
    "Error! Please make sure <User_Setups/Setup06_LilyGo_T_Display_S3.h> is selected in <TFT_eSPI/User_Setup_Select.h>"
#endif

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#error                                                                         \
    "The current version is not supported for the time being, please use a version below Arduino ESP32 3.0"
#endif