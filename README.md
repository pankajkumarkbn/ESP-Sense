# ESP Sense – ESP32 Real‑Time Environmental Dashboard (Async/WebSocket + Captive Portal)

ESP Sense is a **self‑contained environmental monitor** based on an ESP32. It reads **temperature, humidity, pressure, altitude, and air quality** (CO₂ proxy, NH₃, benzene, alcohol, VOC index) and shows them:

- On a **0.96" SSD1306 OLED** with multi‑page, bar‑style UI.  
- In a **modern web dashboard** that updates **in real time via WebSockets**.

When you connect to the ESP32 Wi‑Fi AP `ESP Sense`, a **captive portal** pops up automatically on most devices and opens the dashboard.  

***

## Features

- **Sensors & metrics**
  - DHT11: temperature, relative humidity.  
  - BMP280: temperature, barometric pressure, altitude.  
  - MQ135: CO₂ proxy, NH₃, benzene, alcohol, VOC index (all approximate, curve‑based).  
  - Derived:
    - Dew point (Magnus formula).  
    - Heat index (felt temperature).  
    - Air density.  
    - Virtual temperature.  
    - CO₂‑based AQI‑like score (0–500) + qualitative label (GOOD → HAZARDOUS).

- **OLED UI**
  - 0.96" 128×64 SSD1306 via **U8g2** full‑buffer mode.  
  - Rotating pages every 5 seconds:
    1. DHT11 – temperature, humidity, heat index, dew point.  
    2. BMP280 – temperature, pressure, altitude, air density.  
    3. MQ135 – CO₂ ppm and AQI‑like bar + label.  
    4. MQ135 multi‑gas – NH₃, benzene, alcohol, VOC index.  

- **Web UI (Async + WebSocket)**
  - Backend: **ESPAsyncWebServer** + **AsyncWebSocket** on `/ws`. [github](https://github.com/me-no-dev/ESPAsyncWebServer)
  - Frontend:
    - Single‑page HTML/CSS (glassmorphism cards).  
    - WebSocket client in JavaScript:
      - Connects to `ws://<host>/ws`.  
      - Receives JSON frames with all sensor values.  
      - Updates DOM and bar indicators instantly (no reload). [techtutorialsx](https://techtutorialsx.com/2018/08/14/esp32-async-http-web-server-websockets-introduction/)
  - No meta refresh timers; all updates are push‑based.

- **Captive portal**
  - ESP32 runs a **Wi‑Fi SoftAP**:
    - SSID: `ESP Sense`.  
    - Password: `espsense123` (change in code).  
    - Static IP: `192.168.4.1`.  
  - **DNSServer** answers all DNS queries with the AP IP.  
  - Async HTTP routes handle:
    - `/`, `/generate_204`, `/gen_204`, `/fwlink`, `/hotspot-detect.html`, `/ncsi.txt`, etc.  
    - Non‑local `Host:` headers are redirected to the AP IP.  
  - On most phones/laptops this triggers the OS captive portal popup that loads the dashboard. [blog.bajonczak](https://blog.bajonczak.com/implementing-a-captive-portal-for-your-esp-device/)

- **Networking**
  - AP + optional STA:
    - AP for captive portal / offline use.  
    - Optional STA connection to your router for local network access.  
  - mDNS:
    - When STA is connected, dashboard is reachable at `http://espsense.local` on supporting systems. [randomnerdtutorials](https://randomnerdtutorials.com/esp32-async-web-server-espasyncwebserver-library/)

***

## Hardware

### Components

- ESP32 DevKit (WROOM‑32 or similar).  
- DHT11 temperature/humidity sensor.  
- BMP280 pressure sensor (I²C).  
- MQ135 gas sensor module.  
- 0.96" SSD1306 OLED (SPI, 128×64).  
- Jumper wires, breadboard or custom PCB, 5V USB power.

### Pinout

```text
DHT11 data     -> GPIO 14
MQ135 analog   -> GPIO 34 (ADC1)

BMP280 (I²C)
  SDA          -> GPIO 21
  SCL          -> GPIO 22

OLED (SSD1306, U8g2 SW SPI)
  CLK (SCL)    -> GPIO 16
  MOSI (SDA)   -> GPIO 17
  CS           -> GPIO 18
  DC           -> GPIO 19
  RST          -> GPIO 4
```

MQ135 computations assume **RL ≈ 10 kΩ** and require calibration for realistic ppm values.

***

## Software Architecture

### Stack

- **Arduino core for ESP32**  
- **U8g2** – OLED graphics.  
- **DHT** – DHT11 driver.  
- **Adafruit BMP280** (+ Adafruit Unified Sensor) – BMP280 driver.  
- **ESPAsyncWebServer** + **AsyncTCP** – Async HTTP + WebSocket server. [github](https://github.com/me-no-dev/ESPAsyncWebServer)
- **AsyncWebSocket** – `/ws` endpoint for real‑time sensor JSON. [randomnerdtutorials](https://randomnerdtutorials.com/esp32-websocket-server-sensor/)
- **DNSServer** – captive portal DNS.  
- **ESPmDNS** – mDNS (espsense.local).

### Data flow

1. **Sensors**  
   - Loop periodically:
     - Read DHT11, BMP280 and the MQ135 analog value.  
     - Convert MQ135 reading to Rs and apply power‑law curves to estimate gas ppm.  
     - Calculate derived metrics (dew point, heat index, density, virtual temp, AQI).  

2. **OLED**  
   - Every 5 seconds, page index increments (0–3).  
   - `drawPage*()` functions render each page into the U8g2 buffer.  
   - Common `drawBar()` draws compact bar indicators.  

3. **Async HTTP + WebSocket**  
   - `AsyncWebServer`:
     - Serves the embedded HTML/JS at `/`.  
     - Handles captive portal redirects and OS connectivity URLs.  
   - `AsyncWebSocket` (`/ws`):
     - On a timer (e.g., every 1 second) the ESP builds a JSON snapshot:
       - `tempDHT`, `hum`, `tempBMP`, `press`, `co2PPM`, `nh3PPM`, `benzenePPM`, `alcoholPPM`, `vocIndex`, `aqiScore`, `aqiLabel`.  
     - Broadcasts this JSON to all connected clients (`ws.textAll(...)`). [dfrobot](https://www.dfrobot.com/blog-1117.html)

4. **Frontend**  
   - JS creates a WebSocket to `/ws`.  
   - On each `message`:
     - Parse JSON.  
     - Update text, units and bar widths in the three cards.  
     - Adjust AQI chip color based on AQI value (GOOD vs bad).  
   - A small reconnection loop auto‑reconnects if the WS drops.

***

## Installation & Setup

### 1. Install ESP32 core

Add the ESP32 boards URL in Arduino IDE Boards Manager (if not already), then install “ESP32 by Espressif Systems”. [randomnerdtutorials](https://randomnerdtutorials.com/esp32-async-web-server-espasyncwebserver-library/)

### 2. Install libraries

Via Library Manager / Git:

- **U8g2**  
- **DHT sensor library** (by Adafruit)  
- **Adafruit BMP280 Library** + **Adafruit Unified Sensor**  
- **ESPAsyncWebServer** (me-no-dev) [github](https://github.com/me-no-dev/ESPAsyncWebServer)
- **AsyncTCP** (for ESP32) [github](https://github.com/me-no-dev/ESPAsyncWebServer)

### 3. Clone and open

```bash
git clone https://github.com/<your-username>/<your-repo>.git
cd <your-repo>
```

- Open the main `.ino` file (ESP Sense) in Arduino IDE or VS Code + PlatformIO.

### 4. Configure (optional)

In the sketch you can tweak:

- `ap_ssid` / `ap_pass` – AP SSID + password.  
- `sta_ssid` / `sta_pass` – STA credentials (if you want it on your home Wi‑Fi).  
- WebSocket push interval (default ~1 s).

### 5. Flash

- Select board: **ESP32 Dev Module** (or similar).  
- Select the correct COM port.  
- Upload.

***

## How to Use

### OLED

Right after boot:

- The OLED cycles through the 4 pages every ~5 seconds.  
- Use it as a quick glance display if you don’t want to open the browser.

### Wi‑Fi AP + Captive Portal

1. On your phone/laptop:  
   - Connect to Wi‑Fi network: `ESP Sense`  
   - Password: `espsense123` (unless changed).  

2. Right after connecting:  
   - Many devices will show **“Sign in to network”** or similar.  
   - Tap it; the ESP Sense dashboard opens automatically.  

3. If the captive portal does not appear:  
   - Open any HTTP website (e.g., `http://example.com`) – it should redirect.  
   - Or directly open `http://192.168.4.1/`.

### Real‑time dashboard

- Once the page loads:
  - The status badge will switch from “Connecting…” to “Live WebSocket”.  
  - Cards will start populating with live data, updating roughly once per second.  
  - Bars animate as values change.

### mDNS (optional, STA mode)

If you configured STA:

- On the same LAN, open `http://espsense.local` on devices that support mDNS (Windows with Apple software, macOS, Linux, many phones). [randomnerdtutorials](https://randomnerdtutorials.com/esp32-async-web-server-espasyncwebserver-library/)

***

## Calibration & Limitations

- **DHT11**  
  - Good for basic comfort readings; not lab‑grade.  

- **BMP280**  
  - Provides stable temperature and pressure; altitude is relative to sea‑level pressure constant used.  

- **MQ135**  
  - All gas ppm and AQI values are **indicative only**:
    - Based on Rs/R₀ curves from the MQ135 datasheet (approximate).  
    - Influenced by temperature, humidity and other gases.  
  - For better realism:
    - Pre‑heat MQ135 for several hours.  
    - Calibrate R₀ in known environments (e.g., outdoors ~400 ppm CO₂).  
    - Optionally add compensation terms in the code.

Treat gas data as **trends and relative changes**, not absolute, certified measurements.

***

## Folder Structure

A simple layout might look like:

```text
.
├── src/
│   └── esp_sense_async_ws.ino    # Main sketch (sensors + OLED + Async server + WS + captive portal)
├── README.md
└── (optional) docs/, img/, etc.
```

Later you can split into modules:

- `sensors.cpp` / `sensors.h`  
- `oled_ui.cpp` / `oled_ui.h`  
- `web_server.cpp` / `web_server.h`  

***

## Roadmap / Ideas

- Add **configuration page**:
  - Tune MQ135 calibration constants and thresholds from the browser.  
  - Change AP/STA credentials without recompiling.  
- Log data to **SPIFFS/LittleFS/SD** and draw historical charts.  
- Add **OTA updates**.  
- Support extra sensors:
  - BME280/BME680 for more accurate environmental data.  
  - PM sensors (e.g., PMS5003) for real particulate AQI.  
- Use **Server‑Sent Events** or JSON streaming for alternative real‑time patterns.

***

## License

```text
MIT License – see LICENSE for details.
```

***

## Credits

- Uses **ESPAsyncWebServer + AsyncTCP** for asynchronous HTTP and WebSocket on ESP32. [dfrobot](https://www.dfrobot.com/blog-1117.html)
- WebSocket update pattern inspired by common ESP32 sensor dashboard examples. [techtutorialsx](https://techtutorialsx.com/2018/08/14/esp32-async-http-web-server-websockets-introduction/)
