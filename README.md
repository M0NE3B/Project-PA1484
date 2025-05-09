# ESP32-S3 Weather Dashboard

A menu-driven weather dashboard for the ESP32-S3 LilyGO, fetching 24 h forecasts from SMHI and up to 30 days of historical data from Open-Meteo.

---

## 🚀 Features

- **US1: Start-Up**  
  - [x] US1.1: Show boot screen with version & team number for 3 s  
  - [x] US1.2: Display next 24 h temperature forecast on startup  
  - [x] US1.2B: Show temperature + weather symbols (clear, rain, etc.)

- **US2: Menu Navigation**  
  - [x] US2.1: Navigate between Forecast, History, Settings with two buttons  
  - [x] US2.2: Open menu from anywhere by holding both buttons  
  - [x] US2.2B: Confirm long-hold opens menu overview

- **US3: Historical Data**  
  - [x] US3.1: Menu option to view historical weather data  
  - [x] US3.2: Show up to 30 days of daily data for selected parameter  
  - [x] US3.2B: Scroll in 6 h windows through 24 h history

- **US4: Settings & Configuration**  
  - [x] US4.1: Settings menu to choose options  
  - [x] US4.2: Select weather parameter (Temperature, Humidity, Wind Speed, Precipitation)  
  - [x] US4.3: Select city from list (Karlskrona, Stockholm, Gothenburg, Malmö, Ronneby, Helsingborg, Uppsala, Linköping, Lund, Örebro)  
  - [x] US4.4: Reset settings to default  
  - [x] US4.5: Persist default city & parameter across restarts  
  - [x] US4.6: Units choice (°C/°F) is saved

> **US5 (Extended map forecast) has been skipped for this project.**

---

## 🛠️ Getting Started

### Prerequisites

- **IDE:** Visual Studio Code  
- **Extension:** PlatformIO IDE ([install](https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide))  
- **Board:** ESP32-S3 LilyGO (setup in `platformio.ini`)  
- **Libraries:** All dependencies managed by PlatformIO (`ArduinoJson`, `TFT_eSPI`, etc.)

### Setup & Build

1. Clone this repo and open in VS Code.  
2. Install PlatformIO when prompted.  
3. Edit `src/main.cpp` (or `project.ino`) to set your **SSID**/ **PASSWORD**.  
4. Connect your ESP32 via USB.  
5. In the PlatformIO sidebar, click **Build** then **Upload**.

### Running

- Upon reset, you’ll see the boot screen (3 s), then the main menu.  
- Use the two buttons to move **Up**/ **Down**; press **both** to select.

---

## 📁 Repository Structure

```text
/
├─ src/
│  └─ main.cpp      ← your entire application
├─ lib/             ← any custom libraries
├─ assets/          ← screenshots, diagrams
├─ platformio.ini   ← project & board config
└─ README.md        ← you are here
