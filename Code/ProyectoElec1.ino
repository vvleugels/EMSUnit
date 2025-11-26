#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>

// ---------------------------------------------------------
// 1. WiFi credentials
// ---------------------------------------------------------
const char* ssid     = "FES-ELEC";
const char* password = "ITBA1234";

// ---------------------------------------------------------
// 2. Web server on port 80
// ---------------------------------------------------------
WebServer server(80);

// ---------------------------------------------------------
// 3. Pins
// ---------------------------------------------------------
const int digitalPin1 = 26;    // Salida 1 hacia L298N
const int digitalPin2 = 27;    // Salida 2 hacia L298N

const int ledRunning  = 23;    // LED encendido cuando está funcionando
const int ledStopped  = 22;    // LED encendido cuando está detenido

// Pin ADC para medir 0–3.3 V
const int adcPin      = 36;    // GPIO36 = ADC1_CH0 (recomendado con WiFi)

// Parámetros actualizables desde la web
volatile int   freqValue    = 50;       // Hz
volatile int   dutyValue    = 67;       // %
volatile float periodSec    = 1.0f / 50.0f;  // Periodo en segundos

volatile float maxTimeSec   = 3.0f;     // Tiempo máximo
volatile int   maxCycles    = 0;        // Cantidad máxima de ciclos
volatile int   cycleCounter = 0;        // Contador

volatile int   voltaje      = 0;
volatile int   voltajeMax   = 0;

volatile bool runWaveform  = false;     
volatile bool updateSignal = false;     

// ---------------------------------------------------------
// HTML Webpage (tu HTML original)
// ---------------------------------------------------------
String webpage() {
  String html;
  html  = "<!DOCTYPE html>\n";
  html += "<html>\n<head>\n<title>ESP32 Control</title>\n";
  html += "<style>\nbody { font-family: Arial; margin: 40px; }\n";
  html += ".slider { width: 300px; }\n</style>\n</head>\n<body>\n";
  html += "<h2>ESP32 Frequency, Duty & Time Control</h2>\n";

  html += "<form action=\"/update\">\n";

  html += "<label>Frequency (50 - 200 Hz): </label><br>\n";
  html += "<input type=\"range\" name=\"frequency\" min=\"50\" max=\"200\" value=\"50\" class=\"slider\">\n";
  html += "<span id=\"freqOut\">50</span> Hz<br><br>\n";

  html += "<label>Duty cycle (10 - 90 %): </label><br>\n";
  html += "<input type=\"range\" name=\"duty\" min=\"10\" max=\"90\" value=\"67\" class=\"slider\">\n";
  html += "<span id=\"dutyOut\">67</span> %<br><br>\n";

  html += "<label>Max time (10 - 30 s): </label><br>\n";
  html += "<input type=\"range\" name=\"maxtime\" min=\"10\" max=\"30\" value=\"10\" class=\"slider\">\n";
  html += "<span id=\"timeOut\">10</span> s<br><br><br>\n";

  html += "<input type=\"submit\" value=\"SEND\">\n";
  html += "</form>\n";

  // Mostrar voltaje máximo medido
  html += "<h3>Voltaje maximo medido: " + String(voltajeMax) + " V</h3>\n";

  html += "<script>\n";
  html += "const sliders = document.querySelectorAll(\"input[type=range]\");\n";
  html += "sliders.forEach(sl => {\n";
  html += "  sl.oninput = () => {\n";
  html += "    if (sl.name === \"frequency\") {\n";
  html += "      document.getElementById(\"freqOut\").innerHTML = sl.value;\n";
  html += "    }\n";
  html += "    if (sl.name === \"duty\") {\n";
  html += "      document.getElementById(\"dutyOut\").innerHTML = sl.value;\n";
  html += "    }\n";
  html += "    if (sl.name === \"maxtime\") {\n";
  html += "      document.getElementById(\"timeOut\").innerHTML = sl.value;\n";
  html += "    }\n";
  html += "  };\n";
  html += "});\n";
  html += "</script>\n";

  html += "</body>\n</html>\n";
  return html;
}

// ---------------------------------------------------------
// Web Handlers
// ---------------------------------------------------------
void handleRoot() {
  server.send(200, "text/html", webpage());
}

void handleUpdate() {
  if (server.hasArg("frequency")) freqValue = server.arg("frequency").toInt();
  if (server.hasArg("duty"))      dutyValue = server.arg("duty").toInt();
  if (server.hasArg("maxtime"))   maxTimeSec = server.arg("maxtime").toFloat();

  // Límites
  if (freqValue < 50)  freqValue = 50;
  if (freqValue > 200) freqValue = 200;

  if (dutyValue < 10)  dutyValue = 10;
  if (dutyValue > 90)  dutyValue = 90;

  if (maxTimeSec < 10.0f) maxTimeSec = 10.0f;
  if (maxTimeSec > 30.0f) maxTimeSec = 30.0f;

  // Recalcular periodo
  periodSec = 1.0f / (float)freqValue;
  maxCycles = (int)(maxTimeSec * freqValue) * 1.1f;
  if (maxCycles < 1) maxCycles = 1;

  cycleCounter = 0;
  voltajeMax   = 0;
  runWaveform  = true;
  updateSignal = true;

  digitalWrite(ledRunning, HIGH);
  digitalWrite(ledStopped, LOW);

  Serial.println("----- UPDATE RECEIVED -----");
  Serial.printf("Frequency: %d Hz -> Period %.3f ms\n", freqValue, periodSec * 1000);
  Serial.printf("Duty: %d %%\n", dutyValue);
  Serial.printf("MaxTime: %.1f s -> MaxCycles %d\n", maxTimeSec, maxCycles);

  server.send(200, "text/html",
    "<h2>Values updated!</h2>"
    "<p>Generation started.</p>"
    "<a href=\"/\">Back</a>"
  );
}

// ---------------------------------------------------------
// Waveform Task (tu lógica original intacta)
// ---------------------------------------------------------
void waveformTask(void* parameter) {
  while (true) {
    if (!runWaveform) {
      digitalWrite(digitalPin1, LOW);
      digitalWrite(digitalPin2, LOW);
      digitalWrite(ledRunning, LOW);
      digitalWrite(ledStopped, HIGH);
      vTaskDelay(10);
      continue;
    }

    if (updateSignal) {
      updateSignal = false;
      Serial.println("Restarting waveform with new settings...");
    }

    digitalWrite(ledRunning, HIGH);
    digitalWrite(ledStopped, LOW);

    float localPeriod = periodSec;
    int   localDuty   = dutyValue;
    int   localMaxCycles = maxCycles;

    if (localDuty < 1)   localDuty = 1;
    if (localDuty > 99)  localDuty = 99;

    float dutyFrac = localDuty / 100.0f;

    float onTimeSec  = dutyFrac * localPeriod / 2.0f;
    float offTimeSec = (1.0f - dutyFrac) * localPeriod / 2.0f;

    TickType_t onTicks  = max(1, (int)((onTimeSec  * 1000) / portTICK_PERIOD_MS));
    TickType_t offTicks = max(1, (int)((offTimeSec * 1000) / portTICK_PERIOD_MS));

    digitalWrite(digitalPin1, LOW);
    digitalWrite(digitalPin2, LOW);
    vTaskDelay(offTicks);
    if (updateSignal) continue;

    digitalWrite(digitalPin1, HIGH);
    digitalWrite(digitalPin2, LOW);
    vTaskDelay(onTicks);
    if (updateSignal) continue;

    digitalWrite(digitalPin1, LOW);
    digitalWrite(digitalPin2, LOW);
    vTaskDelay(offTicks);
    if (updateSignal) continue;

    digitalWrite(digitalPin1, LOW);
    digitalWrite(digitalPin2, HIGH);
    vTaskDelay(onTicks);
    if (updateSignal) continue;

    cycleCounter++;

    int lectura = analogRead(adcPin);
    voltaje = lectura * 3.3 / (255 * 1.34);
    if (voltaje > voltajeMax) voltajeMax = voltaje;

    Serial.printf("ADC raw: %d -> voltaje %d V\n", lectura, voltaje);

    if (voltaje > 35) {
      Serial.println("Overvoltage >35V. STOP.");
      runWaveform = false;
      continue;
    }

    if (cycleCounter >= localMaxCycles) {
      Serial.printf("Max cycles reached: %d\n", cycleCounter);
      runWaveform = false;
      continue;
    }

    vTaskDelay(1);
  }
}

// ---------------------------------------------------------
// Setup
// ---------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println("\nStarting ESP32...");

  pinMode(digitalPin1, OUTPUT);
  pinMode(digitalPin2, OUTPUT);

  pinMode(ledRunning, OUTPUT);
  pinMode(ledStopped, OUTPUT);

  pinMode(adcPin, INPUT);

  // Arranca detenido
  digitalWrite(ledRunning, LOW);
  digitalWrite(ledStopped, HIGH);

  // ------------------------------
  // W I F I   (DHCP + mDNS)
  // ------------------------------
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  Serial.print("Connecting to WiFi");

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
    Serial.print(".");
    delay(300);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nERROR: WiFi connection timed out.");
    return;
  }

  Serial.println("\nConnected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  // mDNS → esp32.local
  if (MDNS.begin("esp32")) {
    Serial.println("mDNS responder started: http://esp32.local/");
  } else {
    Serial.println("ERROR starting mDNS");
  }

  // Servidor web
  server.on("/", handleRoot);
  server.on("/update", handleUpdate);
  server.begin();
  Serial.println("Web server running...");

  // Task waveform
  xTaskCreatePinnedToCore(
    waveformTask,
    "Waveform",
    4096,
    NULL,
    1,
    NULL,
    1
  );
}

void loop() {
  server.handleClient();
}
