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
#include <WiFiClientSecure.h>

// Remember to remove these before commiting in GitHub
String ssid = "BTH_Guest";
String password = "Glass91Volvo";

// "tft" is the graphics libary, which has functions to draw on the screen
TFT_eSPI tft = TFT_eSPI();

// Display dimentions
#define DISPLAY_WIDTH 320
#define DISPLAY_HEIGHT 170

// SMHI endpoint (Karlskrona)
const char* forecastUrl =
  "https://opendata-download-metfcst.smhi.se/"
  "api/category/pmp3g/version/2/"
  "geotype/point/lon/15.59/lat/56.18/data.json";

// Forecast data
struct ForecastHour {
  String time;
  float  temp;
  int    symbol;
};
ForecastHour forecast24h[24];

// JSON buffer for other parts of your code
StaticJsonDocument<10> dummyDoc;  // (not used in fetch)

// Prototypes
void showBootScreen();
int  centerX(const char *msg, int textSize);
bool fetch24hForecast();
void display24hForecastText();
void drawWeatherSymbol(int x, int y, int symbol);

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

  // Fetch + display
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
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(2);
  
  // Center text calculations
  int centerX = DISPLAY_WIDTH/2;
  
  tft.drawString("Weather Dashboard", centerX - tft.textWidth("Weather Dashboard", 2)/2, 40);
  tft.drawString("Version 2.0", centerX - tft.textWidth("Version 2.0", 2)/2, 70);
  tft.drawString("Team Group 6", centerX - tft.textWidth("Team Group 6", 2)/2, 100);
  
  delay(3000); // Display for 3 seconds
  tft.fillScreen(TFT_BLACK);
}

bool fetch24hForecast() {
  Serial.println("Fetching 24h forecast...");

  // 1) Secure client
  WiFiClientSecure client;
  client.setInsecure();  // for production, use client.setCACert(...)

  // 2) HTTPClient with settings
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.useHTTP10(true);                    // avoid chunked-encoding quirks
  http.addHeader("Accept", "application/json");

  if (!http.begin(client, forecastUrl)) {
    Serial.println("HTTP.begin() failed");
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("HTTP GET failed: %d\n", httpCode);
    http.end();
    return false;
  }

  // 3) Read full payload
  String payload = http.getString();
  http.end();

  Serial.printf("Payload length: %u\n", payload.length());
  if (payload.length() < 10) {
    Serial.println("⚠️ Payload too short, check network or URL");
    return false;
  }
  Serial.println("Payload preview:");
  Serial.println(payload.substring(0, 200));
  Serial.println("… end preview");

  // 4) Parse with DynamicJsonDocument
  const size_t JSON_CAPACITY = 150000;  
  DynamicJsonDocument doc(JSON_CAPACITY);
  auto err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("JSON parse failed: ");
    Serial.println(err.c_str());
    return false;
  }

  // 5) Extract first 24 entries
  JsonArray ts = doc["timeSeries"].as<JsonArray>();
  for (int i = 0; i < 24 && i < ts.size(); i++) {
    forecast24h[i].time   = String(ts[i]["validTime"].as<const char*>()).substring(11,16);
    forecast24h[i].temp   = 0;
    forecast24h[i].symbol = -1;
    for (JsonObject p : ts[i]["parameters"].as<JsonArray>()) {
      const char* name = p["name"].as<const char*>();
      if (strcmp(name, "t") == 0) {
        forecast24h[i].temp = p["values"][0].as<float>();
      } 
      else if (strcmp(name, "Wsymb2") == 0) {
        forecast24h[i].symbol = p["values"][0].as<int>();
      }
    }
  }

  Serial.println("Forecast parsed successfully");
  return true;
}

// —————— Display 24h forecast as text ——————
void display24hForecastText() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Next 24h:",  0, 0);
  for (int i = 0; i < 24; i++) {
    int y = 10 + i * 6;
    String line = forecast24h[i].time + "  " + String(forecast24h[i].temp,1) + "°C";
    tft.drawString(line, 0, y);
  }
}

// —————— US1.1 Boot Screen ——————
int centerX(const char *msg, int textSize) {
  return (DISPLAY_WIDTH - tft.textWidth(msg, textSize)) / 2;
}

void showBootScreen() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Weather Dashboard", centerX("Weather Dashboard",2),  40);
  tft.drawString("Version 2.0",         centerX("Version 2.0",        2),  70);
  tft.drawString("Team Group 6",        centerX("Team Group 6",       2), 100);
  delay(3000);
  tft.fillScreen(TFT_BLACK);
}


void drawWeatherSymbol(int x, int y, int symbol) {
  // Simple visual representations
  switch(symbol) {
    case 1: // Clear sky
      tft.fillCircle(x, y, 8, TFT_YELLOW);
      break;
    case 2: // Nearly clear
      tft.fillCircle(x, y, 8, TFT_YELLOW);
      tft.fillRect(x-2, y-8, 16, 8, TFT_BLACK);
      break;
    case 3: // Variable clouds
      tft.fillCircle(x, y, 8, TFT_YELLOW);
      tft.fillRect(x-4, y-8, 20, 8, TFT_BLACK);
      break;
    case 4: // Half clear
      tft.fillCircle(x, y, 8, TFT_YELLOW);
      tft.fillRect(x-6, y-8, 24, 8, TFT_BLACK);
      break;
    case 6: // Overcast
      tft.fillRect(x-8, y-8, 16, 8, TFT_DARKGREY);
      break;
    case 8: // Light showers
      tft.fillRect(x-8, y-8, 16, 8, TFT_DARKGREY);
      for (int i = -2; i <= 2; i++) {
        tft.drawFastVLine(x+i*3, y+10, 6, TFT_BLUE);
      }
      break;
    default:
      tft.drawString(String(symbol), x-5, y-5);
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