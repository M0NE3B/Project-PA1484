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

WiFiClient wifi_client;

String api_url = "https://opendata-download-metfcst.smhi.se/api/category/pmp3g/"
                 "version/2/geotype/point/lon/15.5904/lat/56.1824/data.json";

DynamicJsonDocument weatherDoc(200000); // Buffer for SMHI JSON

// Structure to hold forecast data
struct Forecast {
  String time;
  float temperature;
  int symbol;
};

Forecast forecast24h[24]; // Holds the first 24 hourly forecasts

// Function Declarations
void showBootScreen(); // Boot Screen (US1.1)
bool fetchWeather();
void parseForecast();
void drawForecast();
int centerX(const char *msg, int textSize);

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

  // Fetch and Display Forecast (US1.2)
  if (fetchWeather()) {
    parseForecast();
    drawForecast();
  } else {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED);
    tft.drawString("Weather fetch failed", 10, 10);
  }
  Serial.print("Free heap before weather fetch: ");
  Serial.println(ESP.getFreeHeap());
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

// Weather Fetching (US1.2)
bool fetchWeather() {
  HTTPClient http;
  Serial.println("Requesting weather data...");

  // Create secure client for HTTPS
  WiFiClientSecure *client = new WiFiClientSecure;
  client->setInsecure();

  if (!http.begin(*client, api_url)) {
    Serial.println("HTTP begin failed!");
    delete client;
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.print("HTTP GET failed, code: ");
    Serial.println(httpCode);
    http.end();
    delete client;
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  String jsonStr = "";
  bool foundStart = false;

  unsigned long lastRead = millis();
  unsigned long timeout = 5000; // 5s max wait time for full stream

  while (millis() - lastRead < timeout) {
    if (stream->available()) {
      char c = stream->read();
      if (!foundStart && c == '{') {
        foundStart = true;
        jsonStr = "{";
      } else if (foundStart) {
        jsonStr += c;
      }
      lastRead = millis(); // Reset timer on new data
    } else {
      delay(10);
    }
  }

  http.end();
  delete client;

  Serial.print("Stream read length: ");
  Serial.println(jsonStr.length());
  Serial.println("First 200 characters of JSON:");
  Serial.println(jsonStr.substring(0, 200));

  DeserializationError error = deserializeJson(weatherDoc, jsonStr);
  if (error) {
    Serial.print("JSON parsing failed: ");
    Serial.println(error.c_str());
    return false;
  }

  Serial.println("JSON parsed successfully!");
  return true;
}

void parseForecast() {
  JsonArray series = weatherDoc["timeSeries"].as<JsonArray>();
  int count = 0;

  for (JsonObject entry : series) {
    if (count >= 24)
      break;

    String time = entry["validTime"];
    JsonArray params = entry["parameters"];

    float temp = 0;
    int symbol = -1;

    for (JsonObject param : params) {
      if (!param["values"] || param["values"].size() == 0)
        continue;

      String name = param["name"];
      if (name == "t")
        temp = param["values"][0];
      if (name == "Wsymb2")
        symbol = param["values"][0];
    }

    forecast24h[count] = {time, temp, symbol};
    count++;
  }
}

void drawForecast() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);

  tft.drawString("Karlskrona 24h Forecast", 10, 5);

  for (int i = 0; i < 8; i++) { // Show 8 items (every 3 hours)
    int idx = i * 3;
    int x = i * 40 + 10;
    int y = 40;

    // TEMP
    String tempStr = String(forecast24h[idx].temperature, 0) + "°C";
    tft.drawString(tempStr, x, y + 25);

    // TIME (HH:MM)
    String timeStr = forecast24h[idx].time.substring(11, 16);
    tft.drawString(timeStr, x, y + 45); // Shows hour label under forecast

    // SYMBOL ICON (placeholder)
    if (forecast24h[idx].symbol != -1) {
      tft.drawCircle(x + 10, y, 10, TFT_YELLOW); // Placeholder icon
      tft.drawString(String(forecast24h[idx].symbol), x + 5,
                     y - 12); // Show symbol #
    } else {
      tft.drawString("?", x + 10, y); // Fallback for missing symbol
    }
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