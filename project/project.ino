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
#include <esp_task_wdt.h>
#include <time.h>

// Remember to remove these before commiting in GitHub
String ssid = "BTH_Guest";
String password = "Glass91Volvo";

// "tft" is the graphics libary, which has functions to draw on the screen
TFT_eSPI tft = TFT_eSPI();

// Display dimentions
#define DISPLAY_WIDTH 320
#define DISPLAY_HEIGHT 170

// SMHI API endpoint for Karlskrona
const char* forecastUrl =
  "https://opendata-download-metfcst.smhi.se/"
  "api/category/pmp3g/version/2/"
  "geotype/point/lon/15.5904/lat/56.1824/data.json";

// Data structure for one hour’s forecast
struct ForecastHour {
  String time;    // ISO timestamp
  float  temp;    // °C
  int    symbol;  // SMHI icon code
};
ForecastHour forecast24h[24];

// JSON buffer (≈250 KB)
StaticJsonDocument<250000> forecastDoc;

WiFiClient wifi_client;

// Function Declarations
void showBootScreen();                          // Boot Screen (US1.1)
int centerX(const char *msg, int textSize);
bool fetch24hForecast();                        // US1.2 fetch + parse
void display24hForecastText();                  // US1.2 display

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

  // Show Boot Screen (US1.1)
  showBootScreen();

  // Fetch & display forecast (US1.2)
  if (fetch24hForecast()) {
    display24hForecastText();
  } else {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextSize(2);
    tft.drawString("Fetch failed", 10, 10);
    Serial.println("Failed to get forecast.");
  }
}

/**
 * This is the main loop function that runs continuously after setup.
 * Add your code here to perform tasks repeatedly.
 */
void loop() {}

// Boot Screen (US1.1)
int centerX(const char *msg, int textSize) {
  return (DISPLAY_WIDTH - tft.textWidth(msg, textSize)) / 2;
}

void showBootScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  const char *title = "Weather Dashboard";
  const char *version = "Version: 2.0";
  const char *team = "Team: Group 6";

  tft.drawString(title, centerX(title, 2), 40);
  tft.drawString(version, centerX(version, 2), 70);
  tft.drawString(team, centerX(team, 2), 100);

  delay(3000);
  tft.fillScreen(TFT_BLACK);
}

bool fetch24hForecast() {
  Serial.println("Fetching 24h forecast...");
  HTTPClient http;
  WiFiClientSecure* secureClient = new WiFiClientSecure();
  secureClient->setInsecure();

  if (!http.begin(*secureClient, forecastUrl)) {
    Serial.println("HTTP begin failed");
    delete secureClient;
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("HTTP GET failed: %d\n", code);
    http.end();
    delete secureClient;
    return false;
  }

  String payload = http.getString();
  http.end();
  delete secureClient;

  DeserializationError err = deserializeJson(forecastDoc, payload);
  if (err) {
    Serial.print("JSON parse failed: ");
    Serial.println(err.c_str());
    return false;
  }

  JsonArray ts = forecastDoc["timeSeries"].as<JsonArray>();
  for (int i = 0; i < 24 && i < ts.size(); i++) {
    forecast24h[i].time   = String(ts[i]["validTime"].as<const char*>());
    forecast24h[i].temp   = 0;
    forecast24h[i].symbol = -1;

    for (JsonObject p : ts[i]["parameters"].as<JsonArray>()) {
      const char* name = p["name"].as<const char*>();
      if (strcmp(name, "t") == 0) {
        forecast24h[i].temp = p["values"][0].as<float>();
      } else if (strcmp(name, "Wsymb2") == 0) {
        forecast24h[i].symbol = p["values"][0].as<int>();
      }
    }
  }

  Serial.println("Forecast parsed OK");
  return true;
}

// Simple text list: “00h: 5.3 °C”
void display24hForecastText() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.drawString("Next 24h Temp (°C)", 0, 0);

  for (int i = 0; i < 24; i++) {
    int y = 12 + i * 6;
    String hour = forecast24h[i].time.substring(11, 13);
    String line = hour + "h: " + String(forecast24h[i].temp, 1);
    tft.drawString(line, 0, y);
  }
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