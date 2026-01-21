#include <U8g2lib.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <esp_adc_cal.h>
#include <math.h>

#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

// ================== PIN DEFINITIONS ==================
#define DHTPIN   14
#define DHTTYPE  DHT11
#define MQ_PIN   34

#define OLED_CLK 16
#define OLED_MOSI 17
#define OLED_CS  18
#define OLED_DC  19
#define OLED_RST 4

// ================== OLED OBJECT ==================
U8G2_SSD1306_128X64_NONAME_F_4W_SW_SPI u8g2(
  U8G2_R0, OLED_CLK, OLED_MOSI, OLED_CS, OLED_DC, OLED_RST
);

// ================== SENSORS ==================
DHT dht(DHTPIN, DHTTYPE);
Adafruit_BMP280 bmp;
esp_adc_cal_characteristics_t adc_chars;

// ================== GLOBAL SENSOR VALUES ==================
float tempDHT, hum;
float tempBMP, press, altitude;
float dewPoint, heatIndex;
float airDensity, virtualTemp;

int   mqRaw;
float co2PPM;
float nh3PPM, benzenePPM, alcoholPPM;
float vocIndex;
float aqiScore;
String aqiLabel;

unsigned long pageTime = 0;
int page = 0; // 0..3 OLED pages

// ================== WIFI / DNS / ASYNC SERVER ==================
const char* ap_ssid = "ESP Sense";
const char* ap_pass = "espsense123";

// Optional STA credentials (leave empty to skip STA)
const char* sta_ssid = "";
const char* sta_pass = "";

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
DNSServer dnsServer;
const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1);
IPAddress apGateway(192, 168, 4, 1);
IPAddress apSubnet(255, 255, 255, 0);

// ================== DRAWING UTILS ==================
void drawBar(int x, int y, int w, int h, float val, float vmin, float vmax) {
  if (val < vmin) val = vmin;
  if (val > vmax) val = vmax;
  float ratio = (val - vmin) / (vmax - vmin);
  int bw = (int)(w * ratio);
  u8g2.drawFrame(x, y, w, h);
  if (bw > 0) u8g2.drawBox(x, y, bw, h);
}

// Dew point (Magnus)
float dewPointMagnus(float T, float RH) {
  const float a = 17.62;
  const float b = 243.12;
  float gamma = log(RH / 100.0) + (a * T) / (b + T);
  return (b * gamma) / (a - gamma);
}

// Air density (dry air)
float computeAirDensity(float T_C, float P_hPa) {
  float T_K  = T_C + 273.15;
  float P_Pa = P_hPa * 100.0;
  return P_Pa / (287.058 * T_K);
}

// Virtual temperature
float computeVirtualTemp(float T_C, float RH, float P_hPa) {
  float T_K = T_C + 273.15;
  float es = 6.112 * exp((17.67 * T_C) / (T_C + 243.5));
  float e  = RH / 100.0 * es;
  float w  = 0.622 * e / (P_hPa - e);
  float q  = w / (1.0 + w);
  float Tv = T_K * (1.0 + 0.61 * q);
  return Tv - 273.15;
}

// ================== MQ135 CURVES (approx.) ==================
float mq135CO2ppm(float RS, float R0) {
  const float A = 2.0e7;
  const float B = -2.6;
  return A * pow(RS / R0, B);
}

float mq135NH3ppm(float RS, float R0_NH3) {
  const float A = 1.0e4;
  const float B = -1.5;
  return A * pow(RS / R0_NH3, B);
}

float mq135Benzeneppm(float RS, float R0_BENZ) {
  const float A = 5.0e5;
  const float B = -2.0;
  return A * pow(RS / R0_BENZ, B);
}

float mq135Alcoholppm(float RS, float R0_ALC) {
  const float A = 3.0e3;
  const float B = -1.2;
  return A * pow(RS / R0_ALC, B);
}

float computeVOCIndex(float RS, float R0_clean) {
  float ratio = RS / R0_clean;
  if (ratio < 0.2) ratio = 0.2;
  if (ratio > 5.0) ratio = 5.0;
  float idx = (5.0 - ratio) / (5.0 - 0.2) * 100.0;
  if (idx < 0) idx = 0;
  if (idx > 100) idx = 100;
  return idx;
}

void computeAQIFromCO2(float ppm) {
  if (ppm < 400) ppm = 400;
  if (ppm > 5000) ppm = 5000;
  aqiScore = (ppm - 400.0) * 500.0 / (5000.0 - 400.0);

  if (aqiScore <= 50)        aqiLabel = "GOOD";
  else if (aqiScore <= 100)  aqiLabel = "MODERATE";
  else if (aqiScore <= 150)  aqiLabel = "USG";
  else if (aqiScore <= 200)  aqiLabel = "UNHEALTHY";
  else if (aqiScore <= 300)  aqiLabel = "V.UNHLTH";
  else                       aqiLabel = "HAZARDOUS";
}

// ================== OLED PAGES ==================
void drawPageDHT11() {
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(0, 10, "DHT11 TEMP/HUM");
  u8g2.setFont(u8g2_font_6x13_tf);

  u8g2.setCursor(0, 24);
  u8g2.print("T:"); u8g2.print(tempDHT, 1); u8g2.print("C");
  drawBar(40, 16, 84, 8, tempDHT, 0, 50);

  u8g2.setCursor(0, 40);
  u8g2.print("H:"); u8g2.print(hum, 0); u8g2.print("%");
  drawBar(40, 32, 84, 8, hum, 0, 100);

  u8g2.setCursor(0, 56);
  u8g2.print("HI:"); u8g2.print(heatIndex, 1);
  u8g2.print(" DP:"); u8g2.print(dewPoint, 1);
}

void drawPageBMP280() {
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(0, 10, "BMP280 TEMP/P/ALT");
  u8g2.setFont(u8g2_font_6x13_tf);

  u8g2.setCursor(0, 24);
  u8g2.print("TB:"); u8g2.print(tempBMP, 1); u8g2.print("C");

  u8g2.setCursor(0, 40);
  u8g2.print("P:"); u8g2.print(press, 0); u8g2.print("hPa");
  drawBar(40, 32, 84, 8, press, 950, 1050);

  u8g2.setCursor(0, 56);
  u8g2.print("Alt:"); u8g2.print(altitude, 0); u8g2.print("m ");
  u8g2.print("R:"); u8g2.print(airDensity, 2);
}

void drawPageMQ135_CO2() {
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(0, 10, "MQ135 CO2/AQI");
  u8g2.setFont(u8g2_font_6x13_tf);

  u8g2.setCursor(0, 24);
  u8g2.print("CO2:");
  u8g2.print(co2PPM, 0);
  u8g2.print("ppm");

  drawBar(0, 28, 128, 8, co2PPM, 400, 5000);

  u8g2.setCursor(0, 45);
  u8g2.print("AQI:");
  u8g2.print(aqiScore, 0);

  drawBar(0, 49, 90, 8, aqiScore, 0, 500);
  u8g2.setCursor(94, 57);
  u8g2.print(aqiLabel);
}

void drawPageMQ135_Multi() {
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(0, 10, "MQ135 MULTI-GAS");
  u8g2.setFont(u8g2_font_6x13_tf);

  u8g2.setCursor(0, 24);
  u8g2.print("NH3:");
  u8g2.print(nh3PPM, 0);
  u8g2.print("ppm");
  drawBar(0, 28, 128, 6, nh3PPM, 0, 300);

  u8g2.setCursor(0, 41);
  u8g2.print("Bz:");
  u8g2.print(benzenePPM, 0);
  u8g2.print("ppm");
  drawBar(0, 45, 128, 6, benzenePPM, 0, 1000);

  u8g2.setCursor(0, 58);
  u8g2.print("Alc:");
  u8g2.print(alcoholPPM, 0);
  u8g2.print(" VI:");
  u8g2.print(vocIndex, 0);
}

// ================== HTML (Async, WebSocket client) ==================
const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>ESP Sense</title>
<style>
body{margin:0;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Ubuntu,sans-serif;
background:radial-gradient(circle at top,#1a2a6c,#000428);color:#f5f5f5;}
.container{max-width:1100px;margin:0 auto;padding:16px;}
.header{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px;}
.title{font-size:1.6rem;font-weight:600;letter-spacing:.04em;}
.badge{font-size:.75rem;padding:4px 10px;border-radius:999px;
background:linear-gradient(135deg,#00c6ff,#0072ff);box-shadow:0 0 12px rgba(0,198,255,.45);}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));gap:16px;}
.card{position:relative;padding:16px;border-radius:18px;
background:rgba(15,23,42,.86);backdrop-filter:blur(14px);
box-shadow:0 18px 40px rgba(0,0,0,.65);border:1px solid rgba(148,163,184,.25);}
.card::before{content:'';position:absolute;inset:0;border-radius:inherit;
border:1px solid rgba(148,163,184,.2);opacity:.35;pointer-events:none;}
.card-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:10px;}
.card-title{font-size:.95rem;font-weight:600;color:#e5e7eb;letter-spacing:.04em;text-transform:uppercase;}
.card-sub{font-size:.75rem;color:#9ca3af;}
.value{font-size:2rem;font-weight:600;margin:4px 0;color:#f9fafb;}
.unit{font-size:.9rem;color:#9ca3af;margin-left:4px;}
.pill-row{display:flex;flex-wrap:wrap;gap:6px;margin-top:6px;}
.pill{font-size:.7rem;padding:3px 9px;border-radius:999px;background:rgba(55,65,81,.9);color:#e5e7eb;}
.indicator{margin-top:6px;height:8px;border-radius:999px;background:rgba(31,41,55,1);overflow:hidden;}
.indicator-fill{height:100%;border-radius:inherit;background:linear-gradient(90deg,#22c55e,#eab308,#ef4444);}
.chip{font-size:.75rem;padding:3px 10px;border-radius:999px;background:rgba(15,118,110,.9);color:#a5f3fc;}
.chip.bad{background:rgba(127,29,29,.9);color:#fecaca;}
.chip.ok{background:rgba(88,28,135,.9);color:#e9d5ff;}
.muted{font-size:.75rem;color:#9ca3af;margin-top:4px;}
.footer{margin-top:20px;font-size:.7rem;color:#6b7280;text-align:center;}
@media(max-width:600px){.title{font-size:1.3rem;}.card{padding:14px;}.value{font-size:1.7rem;}}
</style>
</head>
<body>
<div class="container">
  <div class="header">
    <div class="title">ESP Sense</div>
    <div class="badge" id="statusBadge">Connecting...</div>
  </div>

  <div class="grid">
    <!-- Comfort card -->
    <div class="card">
      <div class="card-header">
        <div>
          <div class="card-title">Indoor comfort</div>
          <div class="card-sub">DHT11 + BMP280</div>
        </div>
        <span class="chip ok">Live</span>
      </div>
      <div class="value"><span id="tempDHT">--</span><span class="unit">&deg;C</span></div>
      <div class="pill-row">
        <span class="pill">Humidity: <span id="hum">--</span>%</span>
        <span class="pill">Pressure: <span id="press">--</span> hPa</span>
        <span class="pill">BMP Temp: <span id="tempBMP">--</span> &deg;C</span>
      </div>
      <div class="indicator"><div class="indicator-fill" id="tempBar" style="width:0%;"></div></div>
      <div class="muted">Comfort ~20–26 &deg;C, 40–60% RH.</div>
    </div>

    <!-- Air quality card -->
    <div class="card">
      <div class="card-header">
        <div>
          <div class="card-title">Air quality</div>
          <div class="card-sub">MQ135 CO&#8322; / AQI proxy</div>
        </div>
        <span class="chip" id="aqiChip">--</span>
      </div>
      <div class="value"><span id="co2">--</span><span class="unit">ppm</span></div>
      <div class="pill-row">
        <span class="pill">AQI: <span id="aqiScore">--</span></span>
        <span class="pill">Category: <span id="aqiLabel">--</span></span>
      </div>
      <div class="indicator"><div class="indicator-fill" id="aqiBar" style="width:0%;"></div></div>
      <div class="muted">Open windows or ventilation if AQI &gt; 100.</div>
    </div>

    <!-- Gases & VOCs card -->
    <div class="card">
      <div class="card-header">
        <div>
          <div class="card-title">Gases & VOCs</div>
          <div class="card-sub">MQ135 (NH&#8323;, benzene, VOC)</div>
        </div>
        <span class="chip">Sensor mix</span>
      </div>
      <div class="pill-row">
        <span class="pill">NH&#8323;: ~<span id="nh3">--</span> ppm</span>
        <span class="pill">Benzene: ~<span id="benz">--</span> ppm</span>
      </div>
      <div class="pill-row">
        <span class="pill">Alcohol: ~<span id="alc">--</span> ppm</span>
        <span class="pill">VOC index: <span id="voc">--</span></span>
      </div>
      <div class="indicator"><div class="indicator-fill" id="vocBar" style="width:0%;"></div></div>
      <div class="muted">Qualitative only; based on MQ135 curves.</div>
    </div>
  </div>

  <div class="footer">
    ESP Sense · AP: ESP Sense · mDNS: espsense.local · WebSocket updates in real time.
  </div>
</div>

<script>
let ws;
function connectWS(){
  const proto = (location.protocol === 'https:') ? 'wss:' : 'ws:';
  const url = proto + '//' + location.host + '/ws';
  ws = new WebSocket(url);
  const badge = document.getElementById('statusBadge');

  ws.onopen = () => {
    badge.textContent = 'Live WebSocket';
    badge.style.background = 'linear-gradient(135deg,#22c55e,#16a34a)';
  };

  ws.onclose = () => {
    badge.textContent = 'Reconnecting...';
    badge.style.background = 'linear-gradient(135deg,#f97316,#ea580c)';
    setTimeout(connectWS, 2000);
  };

  ws.onerror = () => {
    badge.textContent = 'Error';
    badge.style.background = 'linear-gradient(135deg,#ef4444,#b91c1c)';
  };

  ws.onmessage = (event) => {
    try {
      const d = JSON.parse(event.data);
      // Basic values
      document.getElementById('tempDHT').textContent = d.tempDHT.toFixed(1);
      document.getElementById('hum').textContent     = d.hum.toFixed(0);
      document.getElementById('press').textContent   = d.press.toFixed(1);
      document.getElementById('tempBMP').textContent = d.tempBMP.toFixed(1);

      document.getElementById('co2').textContent      = d.co2PPM.toFixed(0);
      document.getElementById('aqiScore').textContent = d.aqiScore.toFixed(0);
      document.getElementById('aqiLabel').textContent = d.aqiLabel;

      document.getElementById('nh3').textContent  = d.nh3PPM.toFixed(0);
      document.getElementById('benz').textContent = d.benzenePPM.toFixed(0);
      document.getElementById('alc').textContent  = d.alcoholPPM.toFixed(0);
      document.getElementById('voc').textContent  = d.vocIndex.toFixed(0);

      // Bars
      document.getElementById('tempBar').style.width =
        Math.min(100, Math.max(0, (d.tempDHT/40)*100)).toFixed(0) + '%';
      document.getElementById('aqiBar').style.width =
        Math.min(100, Math.max(0, (d.aqiScore/500)*100)).toFixed(0) + '%';
      document.getElementById('vocBar').style.width =
        Math.min(100, Math.max(0, d.vocIndex)).toFixed(0) + '%';

      // AQI chip style
      const chip = document.getElementById('aqiChip');
      chip.textContent = d.aqiLabel;
      if (d.aqiScore <= 100) {
        chip.classList.remove('bad'); chip.classList.add('ok');
      } else {
        chip.classList.remove('ok'); chip.classList.add('bad');
      }
    } catch(e){
      console.error('WS parse error', e);
    }
  };
}

window.addEventListener('load', connectWS);
</script>
</body>
</html>
)HTML";

// ================== WebSocket handler ==================
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    // Could send initial snapshot here if needed
  } else if (type == WS_EVT_DISCONNECT) {
    // client disconnected
  } else if (type == WS_EVT_DATA) {
    // Currently no commands from client; ignore
  }
}

// Broadcast current sensor values as JSON to all connected clients
void wsBroadcastSensors() {
  if (!ws.count()) return; // no clients

  String json = "{";
  json += "\"tempDHT\":" + String(tempDHT, 2) + ",";
  json += "\"hum\":" + String(hum, 2) + ",";
  json += "\"tempBMP\":" + String(tempBMP, 2) + ",";
  json += "\"press\":" + String(press, 2) + ",";
  json += "\"co2PPM\":" + String(co2PPM, 2) + ",";
  json += "\"nh3PPM\":" + String(nh3PPM, 2) + ",";
  json += "\"benzenePPM\":" + String(benzenePPM, 2) + ",";
  json += "\"alcoholPPM\":" + String(alcoholPPM, 2) + ",";
  json += "\"vocIndex\":" + String(vocIndex, 2) + ",";
  json += "\"aqiScore\":" + String(aqiScore, 2) + ",";
  json += "\"aqiLabel\":\"" + aqiLabel + "\"";
  json += "}";

  ws.textAll(json);
}

// ================== Captive portal helpers (Async) ==================
String hostHeader;

// captive redirect decision (standard pattern for AsyncWebServer)[web:62][web:54]
bool isCaptivePortal(AsyncWebServerRequest *request) {
  String host = request->host();
  if (!host.endsWith(F(".local")) && !host.equals(WiFi.softAPIP().toString())) {
    return true;
  }
  return false;
}

void handleRoot(AsyncWebServerRequest *request) {
  if (isCaptivePortal(request)) {
    request->redirect(String("http://") + apIP.toString());
    return;
  }
  request->send_P(200, "text/html", INDEX_HTML);
}

void handleNotFound(AsyncWebServerRequest *request) {
  // For any unknown path, redirect to root for captive experience
  request->redirect("/");
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);

  // OLED & sensors
  u8g2.begin();
  Wire.begin();
  dht.begin();
  if (!bmp.begin(0x76)) {
    Serial.println("BMP280 not found!");
  }
  esp_adc_cal_characterize(
    ADC_UNIT_1,
    ADC_ATTEN_DB_11,
    ADC_WIDTH_BIT_12,
    1100,
    &adc_chars
  );

  // WiFi AP with static IP
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIP, apGateway, apSubnet);
  WiFi.softAP(ap_ssid, ap_pass);
  Serial.print("SoftAP IP: ");
  Serial.println(WiFi.softAPIP());

  // DNS server: answer all queries with AP IP (captive portal)
  dnsServer.start(DNS_PORT, "*", apIP);

  // Optional STA for mDNS on LAN
  if (strlen(sta_ssid)) {
    WiFi.begin(sta_ssid, sta_pass);
    Serial.print("Connecting STA");
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
      delay(500); Serial.print('.');
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("STA IP: ");
      Serial.println(WiFi.localIP());
    }
  }

  // mDNS on STA if available
  if (MDNS.begin("espsense")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS started: http://espsense.local");
  } else {
    Serial.println("mDNS start failed");
  }

  // WebSocket setup
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // HTTP routes
  server.on("/", HTTP_ANY, handleRoot);

  // captive/check URLs -> root
  server.on("/generate_204", HTTP_ANY, [](AsyncWebServerRequest *req){ handleRoot(req); });
  server.on("/gen_204",      HTTP_ANY, [](AsyncWebServerRequest *req){ handleRoot(req); });
  server.on("/fwlink",       HTTP_ANY, [](AsyncWebServerRequest *req){ handleRoot(req); });
  server.on("/hotspot-detect.html", HTTP_ANY, [](AsyncWebServerRequest *req){ handleRoot(req); });
  server.on("/ncsi.txt",     HTTP_ANY, [](AsyncWebServerRequest *req){ handleRoot(req); });

  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("Async HTTP/WebSocket server started");
}

// ================== LOOP ==================
unsigned long lastWsPush = 0;

void loop() {
  // DNS for captive portal
  dnsServer.processNextRequest();

  // ---- SENSOR READS ----
  tempDHT = dht.readTemperature();
  hum     = dht.readHumidity();

  tempBMP = bmp.readTemperature();
  press   = bmp.readPressure() / 100.0F;
  altitude = bmp.readAltitude(1013.25);

  mqRaw = analogRead(MQ_PIN);
  float voltage = esp_adc_cal_raw_to_voltage(mqRaw, &adc_chars) / 1000.0; // V

  float RL = 10.0;                                // kΩ
  float RS = RL * (3.3 - voltage) / voltage;      // kΩ

  static float R0_CO2   = 3.6;
  static float R0_NH3   = 3.6;
  static float R0_BENZ  = 3.6;
  static float R0_ALC   = 3.6;
  static float R0_clean = 3.6;

  co2PPM      = mq135CO2ppm(RS, R0_CO2);
  nh3PPM      = mq135NH3ppm(RS, R0_NH3);
  benzenePPM  = mq135Benzeneppm(RS, R0_BENZ);
  alcoholPPM  = mq135Alcoholppm(RS, R0_ALC);
  vocIndex    = computeVOCIndex(RS, R0_clean);
  computeAQIFromCO2(co2PPM);

  dewPoint    = dewPointMagnus(tempDHT, hum);
  heatIndex   = dht.computeHeatIndex(tempDHT, hum, false);
  airDensity  = computeAirDensity(tempDHT, press);
  virtualTemp = computeVirtualTemp(tempDHT, hum, press);

  // ---- OLED PAGES ----
  if (millis() - pageTime > 5000) {
    page = (page + 1) % 4;
    pageTime = millis();
  }

  u8g2.clearBuffer();
  if (page == 0)      drawPageDHT11();
  else if (page == 1) drawPageBMP280();
  else if (page == 2) drawPageMQ135_CO2();
  else                drawPageMQ135_Multi();
  u8g2.sendBuffer();

  // ---- WebSocket broadcast (e.g., every 1 second) ----
  if (millis() - lastWsPush > 1000) {
    wsBroadcastSensors();
    lastWsPush = millis();
  }

  // no delay needed; Async server + DNS are non-blocking
}
