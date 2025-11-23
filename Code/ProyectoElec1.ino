#include <WiFi.h>
#include <WebServer.h>

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

volatile float maxTimeSec   = 3.0f;     // Tiempo máximo en segundos
volatile int   maxCycles    = 0;        // Cantidad máxima de ciclos a ejecutar
volatile int   cycleCounter = 0;        // Contador de ciclos ejecutados

// Valor de voltaje calculado (ADC * 10)
volatile int   voltaje      = 0;
volatile int   voltajeMax   = 0;        // Máximo voltaje medido en el último ciclo

// Flags de control
volatile bool runWaveform  = false;     // Solo arranca luego de recibir instrucciones
volatile bool updateSignal = false;     // Para reiniciar configuración en el task

// ---------------------------------------------------------
// HTML Webpage
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
  html += "<h3>Voltaje máximo medido: " + String(voltajeMax) + " V</h3>\n";

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
// Web handlers
// ---------------------------------------------------------
void handleRoot() {
  server.send(200, "text/html", webpage());
}

void handleUpdate() {
  if (server.hasArg("frequency")) {
    freqValue = server.arg("frequency").toInt();
  }
  if (server.hasArg("duty")) {
    dutyValue = server.arg("duty").toInt();
  }
  if (server.hasArg("maxtime")) {
    maxTimeSec = server.arg("maxtime").toFloat();
  }

  // Límites
  if (freqValue < 50)  freqValue = 50;
  if (freqValue > 200) freqValue = 200;

  if (dutyValue < 10)  dutyValue = 10;
  if (dutyValue > 90)  dutyValue = 90;

  if (maxTimeSec < 10.0f)  maxTimeSec = 10.0f;
  if (maxTimeSec > 30.0f)  maxTimeSec = 30.0f;

  // Recalcular periodo
  periodSec = 1.0f / (float)freqValue;

  // Calcular cuántos ciclos entran en ese tiempo (con margen 10%)
  maxCycles = (int)(maxTimeSec * (float)freqValue) * 1.1f;
  if (maxCycles < 1) maxCycles = 1;

  // Reiniciar contador y habilitar generación
  cycleCounter = 0;
  voltajeMax   = 0;     // reinicio del máximo para la nueva corrida
  runWaveform  = true;
  updateSignal = true;

  // LED: está corriendo
  digitalWrite(ledRunning, HIGH);
  digitalWrite(ledStopped, LOW);

  Serial.println("----- UPDATE RECEIVED -----");
  Serial.print("Frequency: ");
  Serial.print(freqValue);
  Serial.print(" Hz  -> Period = ");
  Serial.print(periodSec * 1000.0f, 3);
  Serial.println(" ms");

  Serial.print("Duty: ");
  Serial.print(dutyValue);
  Serial.println(" %");

  Serial.print("Max time: ");
  Serial.print(maxTimeSec);
  Serial.print(" s  -> Max cycles = ");
  Serial.println(maxCycles);

  server.send(200, "text/html",
    "<h2>Values updated!</h2>"
    "<p>Generation started.</p>"
    "<a href=\"/\">Back</a>"
  );
}

// ---------------------------------------------------------
// Waveform task
// ---------------------------------------------------------
// Patrón de 4 estados (un ciclo completo):
// 1) Ambos LOW  (OFF)
// 2) Pin1 HIGH  (ON)
// 3) Ambos LOW  (OFF)
// 4) Pin2 HIGH  (ON)
//
// La suma de los 4 tiempos = periodo
// La suma de los estados ON = duty% del periodo.
void waveformTask(void* parameter) {
  while (true) {

    // Si no hay orden de generar, dejar pines y LEDs en estado "detenido"
    if (!runWaveform) {
      digitalWrite(digitalPin1, LOW);
      digitalWrite(digitalPin2, LOW);

      digitalWrite(ledRunning, LOW);
      digitalWrite(ledStopped, HIGH);

      vTaskDelay(10 / portTICK_PERIOD_MS);
      continue;
    }

    if (updateSignal) {
      updateSignal = false;
      Serial.println("Restarting waveform with new settings...");
    }

    // LED: está corriendo
    digitalWrite(ledRunning, HIGH);
    digitalWrite(ledStopped, LOW);

    // Copiar parámetros volátiles localmente
    float localPeriod    = periodSec;
    int   localDuty      = dutyValue;
    int   localMaxCycles = maxCycles;

    if (localDuty < 1)   localDuty = 1;
    if (localDuty > 99)  localDuty = 99;

    float dutyFrac = (float)localDuty / 100.0f;

    // 2 segmentos ON, 2 segmentos OFF
    float onTimeSec  = dutyFrac * localPeriod / 2.0f;
    float offTimeSec = (1.0f - dutyFrac) * localPeriod / 2.0f;

    TickType_t onTicks  = (TickType_t)((onTimeSec  * 1000.0f) / portTICK_PERIOD_MS);
    TickType_t offTicks = (TickType_t)((offTimeSec * 1000.0f) / portTICK_PERIOD_MS);

    if (onTicks == 0)  onTicks  = 1;
    if (offTicks == 0) offTicks = 1;

    // ------------ Estado 1: ambos LOW (OFF) ------------
    digitalWrite(digitalPin1, LOW);
    digitalWrite(digitalPin2, LOW);
    vTaskDelay(offTicks);
    if (updateSignal) continue;

    // ------------ Estado 2: pin1 HIGH (ON) -------------
    digitalWrite(digitalPin1, HIGH);
    digitalWrite(digitalPin2, LOW);
    vTaskDelay(onTicks);
    if (updateSignal) continue;

    // ------------ Estado 3: ambos LOW (OFF) ------------
    digitalWrite(digitalPin1, LOW);
    digitalWrite(digitalPin2, LOW);
    vTaskDelay(offTicks);
    if (updateSignal) continue;

    // ------------ Estado 4: pin2 HIGH (ON) -------------
    digitalWrite(digitalPin1, LOW);
    digitalWrite(digitalPin2, HIGH);
    vTaskDelay(onTicks);
    if (updateSignal) continue;

    // Terminó un ciclo completo
    cycleCounter++;

    // --- Medición ADC al final del ciclo ---
    int lectura = analogRead(adcPin);
    voltaje = lectura * 3.3 / (255*1.34);

    if (voltaje > voltajeMax) {
      voltajeMax = voltaje;
    }

    Serial.print("ADC raw: ");
    Serial.print(lectura);
    Serial.print("  -> voltaje = ");
    Serial.println(voltaje);

    // Freno por sobrevoltaje (> 35 V)
    if (voltaje > 35) {
      Serial.println("Overvoltage detected (>35V). Stopping waveform.");
      runWaveform = false;
      digitalWrite(digitalPin1, LOW);
      digitalWrite(digitalPin2, LOW);

      // LED: detenido
      digitalWrite(ledRunning, LOW);
      digitalWrite(ledStopped, HIGH);

      // vuelve al inicio del while, donde ya queda en modo detenido
      continue;
    }

    if (cycleCounter >= localMaxCycles) {
      // Alcanzó el máximo de ciclos, detener y poner todo en LOW
      runWaveform = false;
      digitalWrite(digitalPin1, LOW);
      digitalWrite(digitalPin2, LOW);

      // LED: detenido
      digitalWrite(ledRunning, LOW);
      digitalWrite(ledStopped, HIGH);

      Serial.print("Max cycles reached: ");
      Serial.println(cycleCounter);
    }
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

  // Pin ADC
  pinMode(adcPin, INPUT);

  // Asegurar que arrancan en LOW
  digitalWrite(digitalPin1, LOW);
  digitalWrite(digitalPin2, LOW);

  // Arranca detenido: LED STOP encendido
  digitalWrite(ledRunning, LOW);
  digitalWrite(ledStopped, HIGH);

  Serial.println("Connecting to WiFi...");

  // IP fija 10.169.229.71
  IPAddress local_IP(10,169,229,71);
  IPAddress gateway(10,169,229,1);
  IPAddress subnet(255,255,255,0);

  WiFi.config(local_IP, gateway, subnet);
  WiFi.begin(ssid, password);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(300);
    if (millis() - startAttempt > 10000) {
      Serial.println("\nERROR: WiFi connection timed out.");
      return;
    }
  }

  Serial.println("\nConnected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.on("/update", handleUpdate);
  server.begin();
  Serial.println("Web server running...");

  // Task de generación de forma de onda
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

// ---------------------------------------------------------
void loop() {
  server.handleClient();
}