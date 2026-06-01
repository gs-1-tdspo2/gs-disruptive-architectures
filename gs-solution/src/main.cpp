#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_BMP085.h>
#include <time.h>           // [GAP 2] Biblioteca para cliente NTP e formatação de tempo
#include <WebServer.h>      // [REST] WebServer embutido no ESP32 Arduino Core (sem lib extra)

// ─── Wi-Fi ───────────────────────────────────────────────────────────────────
const char* ssid     = "Wokwi-GUEST";
const char* password = "";

// ─── MQTT ────────────────────────────────────────────────────────────────────
const char* mqtt_server = "broker.hivemq.com";
const int   mqtt_port   = 1883;

const char* TOPIC_TELEMETRIA = "app/estacoes/APP-ST-001/telemetria";
const char* TOPIC_STATUS     = "app/estacoes/APP-ST-001/status";
const char* TOPIC_COMANDO    = "app/estacoes/APP-ST-001/alertas"; // [GAP 3] era "comando"

// ─── Pinos ───────────────────────────────────────────────────────────────────
#define PIN_POT_POLUICAO 35
#define PIN_TRIG          5
#define PIN_ECHO         18
#define PIN_LED_VERDE    17
#define PIN_LED_VERMELHO 16
#define PIN_BUZZER       12 // [GAP 1] GPIO 12 conforme PRD

#define ULTRASONIC_TIMEOUT_US 30000UL

// ─── Limites de severidade ────────────────────────────────────────────────────
#define ENC_ATENCAO_CM   120.0f
#define ENC_ALERTA_CM     80.0f
#define ENC_CRITICO_CM    50.0f
#define PM_ATENCAO        50.0f
#define PM_ALERTA        100.0f
#define PM_CRITICO       200.0f
#define INC_ATENCAO       15.0f
#define INC_ALERTA        25.0f
#define INC_CRITICO       35.0f
#define PRES_ATENCAO    1005.0f
#define PRES_ALERTA      992.0f
#define PRES_CRITICO     980.0f

// ─── Buzzer LEDC ─────────────────────────────────────────────────────────────
#define BUZZER_FREQ_HZ      1000
#define BUZZER_BIPE_ON_MS    200
#define BUZZER_BIPE_OFF_MS   300
#define BUZZER_LEDC_CHANNEL    0  // [GAP 1 – FIX] API Core 2.x
#define BUZZER_LEDC_RES       10

// ─── Severidade ──────────────────────────────────────────────────────────────
typedef enum { SEV_NORMAL=0, SEV_ATENCAO=1, SEV_ALERTA=2, SEV_CRITICO=3 } Severidade;

Severidade severidadeEnchente(float d, bool v) {
  if (!v) return SEV_NORMAL;
  if (d < ENC_CRITICO_CM) return SEV_CRITICO;
  if (d < ENC_ALERTA_CM)  return SEV_ALERTA;
  if (d < ENC_ATENCAO_CM) return SEV_ATENCAO;
  return SEV_NORMAL;
}
Severidade severidadePM25(float pm) {
  if (pm > PM_CRITICO) return SEV_CRITICO;
  if (pm > PM_ALERTA)  return SEV_ALERTA;
  if (pm > PM_ATENCAO) return SEV_ATENCAO;
  return SEV_NORMAL;
}
Severidade severidadeInclinacao(float inc) {
  float v = abs(inc);
  if (v > INC_CRITICO) return SEV_CRITICO;
  if (v > INC_ALERTA)  return SEV_ALERTA;
  if (v > INC_ATENCAO) return SEV_ATENCAO;
  return SEV_NORMAL;
}
Severidade severidadePressao(float hpa) {
  if (hpa <= PRES_CRITICO) return SEV_CRITICO;
  if (hpa <= PRES_ALERTA)  return SEV_ALERTA;
  if (hpa <= PRES_ATENCAO) return SEV_ATENCAO;
  return SEV_NORMAL;
}
const char* sevStr(Severidade s) {
  switch(s) {
    case SEV_CRITICO: return "CRITICO";
    case SEV_ALERTA:  return "ALERTA";
    case SEV_ATENCAO: return "ATENCAO";
    default:          return "NORMAL";
  }
}

// ─── OLED ────────────────────────────────────────────────────────────────────
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledDisponivel = false;

// ─── Objetos globais ──────────────────────────────────────────────────────────
WiFiClient   espClient;
PubSubClient client(espClient);
Adafruit_MPU6050 mpu;
Adafruit_BMP085  bmp;
WebServer        server(80); // [REST] WebServer na porta 80

unsigned long lastTelemetria    = 0;
unsigned long lastStatus        = 0;
unsigned long startTime         = 0;
unsigned long buzzerUltimoEvento = 0;
bool          buzzerLigado       = false;

// ─── [REST] Snapshot da última leitura – compartilhado com os handlers HTTP ──
// Estrutura preenchida a cada ciclo de telemetria (5s) e lida pelos endpoints.
struct Snapshot {
  char  timestamp[25];
  float waterDistanceCm;
  int   waterLevelPercent;
  float tiltAngle;
  float vibration;
  float pressureHpa;
  float pm25;
  float pm10;
  char  sevEnchente[10];
  char  sevPressao[10];
  char  sevPM[10];
  char  sevInclinacao[10];
  char  geralSev[10];
} snap;

bool snapValido = false; // Torna-se true após a primeira leitura

// ─── [REST] Helper: adiciona headers CORS para o dashboard poder consumir ────
void corsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin",  "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.sendHeader("Cache-Control",                "no-cache");
}

// ─── [REST] GET /sensor ───────────────────────────────────────────────────────
// Retorna a última leitura completa de todos os sensores em JSON.
void handleSensor() {
  corsHeaders();
  if (!snapValido) {
    server.send(503, "application/json",
      "{\"erro\":\"Aguardando primeira leitura dos sensores\"}");
    return;
  }
  char json[512];
  snprintf(json, sizeof(json),
    "{"
    "\"stationCode\":\"APP-ST-001\","
    "\"timestamp\":\"%s\","
    "\"waterDistanceCm\":%.2f,"
    "\"waterLevelPercent\":%d,"
    "\"tiltAngle\":%.2f,"
    "\"vibration\":%.4f,"
    "\"pressureHpa\":%.2f,"
    "\"pm25\":%.2f,"
    "\"pm10\":%.2f,"
    "\"severidade\":{"
      "\"enchente\":\"%s\","
      "\"pressao\":\"%s\","
      "\"qualidadeAr\":\"%s\","
      "\"inclinacao\":\"%s\","
      "\"geral\":\"%s\""
    "}"
    "}",
    snap.timestamp,
    snap.waterDistanceCm, snap.waterLevelPercent,
    snap.tiltAngle, snap.vibration,
    snap.pressureHpa, snap.pm25, snap.pm10,
    snap.sevEnchente, snap.sevPressao,
    snap.sevPM, snap.sevInclinacao, snap.geralSev);
  server.send(200, "application/json", json);
}

// ─── [REST] GET /status ───────────────────────────────────────────────────────
// Retorna diagnóstico da estação: uptime, IP, RSSI, firmware.
void handleStatus() {
  corsHeaders();
  unsigned long uptimeSeg = (millis() - startTime) / 1000;
  char json[300];
  snprintf(json, sizeof(json),
    "{"
    "\"stationCode\":\"APP-ST-001\","
    "\"versaoFirmware\":\"1.4.0\","
    "\"uptimeSeg\":%lu,"
    "\"ip\":\"%s\","
    "\"rssi\":%d,"
    "\"mqttConectado\":%s,"
    "\"snapValido\":%s"
    "}",
    uptimeSeg,
    WiFi.localIP().toString().c_str(),
    WiFi.RSSI(),
    client.connected() ? "true" : "false",
    snapValido         ? "true" : "false");
  server.send(200, "application/json", json);
}

// ─── [REST] GET /alertas ──────────────────────────────────────────────────────
// Retorna apenas o nível de severidade geral e quais sensores estão em alerta.
void handleAlertas() {
  corsHeaders();
  if (!snapValido) {
    server.send(503, "application/json",
      "{\"erro\":\"Aguardando primeira leitura dos sensores\"}");
    return;
  }
  char json[300];
  snprintf(json, sizeof(json),
    "{"
    "\"stationCode\":\"APP-ST-001\","
    "\"timestamp\":\"%s\","
    "\"geralSeveridade\":\"%s\","
    "\"alertas\":{"
      "\"enchente\":\"%s\","
      "\"pressao\":\"%s\","
      "\"qualidadeAr\":\"%s\","
      "\"inclinacao\":\"%s\""
    "}"
    "}",
    snap.timestamp, snap.geralSev,
    snap.sevEnchente, snap.sevPressao,
    snap.sevPM, snap.sevInclinacao);
  server.send(200, "application/json", json);
}

// ─── [REST] GET / ─────────────────────────────────────────────────────────────
// Rota raiz: lista os endpoints disponíveis (auto-documentação).
void handleRoot() {
  corsHeaders();
  server.send(200, "application/json",
    "{"
    "\"estacao\":\"APP-ST-001\","
    "\"endpoints\":["
      "{\"GET\":\"/sensor\",  \"desc\":\"Ultima leitura completa dos sensores\"},"
      "{\"GET\":\"/status\",  \"desc\":\"Diagnostico e saude da estacao\"},"
      "{\"GET\":\"/alertas\", \"desc\":\"Nivel de severidade e alertas ativos\"}"
    "]"
    "}");
}

// ─── Callback MQTT ───────────────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.print("[ALERTA RECEBIDO] "); Serial.println(msg);
  if (msg.indexOf("reset") >= 0) ESP.restart();
}

// ─── Wi-Fi + NTP ─────────────────────────────────────────────────────────────
void setup_wifi() {
  delay(10);
  Serial.println("\n--- Conectando ao WiFi ---");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi OK: " + WiFi.localIP().toString());

  // [GAP 2] NTP para Horário de Brasília (UTC-3)
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("NTP configurado. Aguardando sincronizacao...");
  struct tm timeinfo;
  int t = 0;
  while (!getLocalTime(&timeinfo) && t++ < 20) { delay(500); Serial.print("."); }
  Serial.println(t < 20 ? "\nNTP OK!" : "\n[AVISO] NTP sem sync.");
}

// ─── Reconexão MQTT ──────────────────────────────────────────────────────────
void reconnect() {
  while (!client.connected()) {
    Serial.print("Conectando MQTT...");
    String id = "ESP32-ST001-" + String(random(0xffff), HEX);
    if (client.connect(id.c_str())) {
      Serial.println("OK!");
      client.subscribe(TOPIC_COMANDO);
    } else {
      Serial.printf("Falhou rc=%d. Retry 5s...\n", client.state());
      delay(5000);
    }
  }
}

// ─── OLED ────────────────────────────────────────────────────────────────────
void atualizarDisplay(float dist, bool distValida, float pressao,
                      float pm25, float incX,
                      Severidade sevEnc, Severidade sevPres,
                      Severidade sevPM,  Severidade sevInc) {
  if (!oledDisponivel) return;
  Severidade geral = (Severidade)max({(int)sevEnc,(int)sevPres,(int)sevPM,(int)sevInc});
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  switch (geral) {
    case SEV_CRITICO: display.println("!!! CRITICO !!!     "); break;
    case SEV_ALERTA:  display.println("** ALERTA  **       "); break;
    case SEV_ATENCAO: display.println(">>  ATENCAO  <<     "); break;
    default:          display.println("   Sistema OK       "); break;
  }
  auto badge = [](Severidade s) -> const char* {
    switch(s){case SEV_CRITICO:return "[!!!]";case SEV_ALERTA:return "[ ! ]";case SEV_ATENCAO:return "[ ? ]";default:return "     ";}
  };
  display.setCursor(0, 10);
  if (distValida) { char b[22]; snprintf(b,22,"Agua %5.1fcm %s",dist,badge(sevEnc)); display.println(b); }
  else display.println("Agua  --ERRO--      ");
  display.setCursor(0, 20);
  { char b[22]; snprintf(b,22,"Pres %6.1fhP%s",pressao,badge(sevPres)); display.println(b); }
  display.setCursor(0, 30);
  { char b[22]; snprintf(b,22,"PM25 %5.1fug %s",pm25,badge(sevPM)); display.println(b); }
  display.setCursor(0, 40);
  { char b[22]; snprintf(b,22,"Incl %5.1f gr%s",incX,badge(sevInc)); display.println(b); }

  // [REST] Linha extra no OLED: exibe o IP para facilitar acesso à API
  display.setCursor(0, 52);
  display.println(WiFi.localIP().toString());

  display.display();
}

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  pinMode(PIN_LED_VERDE,    OUTPUT); digitalWrite(PIN_LED_VERDE,    HIGH);
  pinMode(PIN_LED_VERMELHO, OUTPUT); digitalWrite(PIN_LED_VERMELHO, LOW);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);

  // [GAP 1 – FIX] Core 2.x: ledcSetup + ledcAttachPin em vez de ledcAttachChannel
  ledcSetup(BUZZER_LEDC_CHANNEL, BUZZER_FREQ_HZ, BUZZER_LEDC_RES);
  ledcAttachPin(PIN_BUZZER, BUZZER_LEDC_CHANNEL);
  ledcWrite(BUZZER_LEDC_CHANNEL, 0);

  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    oledDisponivel = true;
    display.clearDisplay();
    display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
    display.setCursor(20, 25); display.println("Iniciando...");
    display.display();
  }

  if (!mpu.begin()) { Serial.println("[ERRO] MPU6050!"); while(1) delay(10); }
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

  if (!bmp.begin()) { Serial.println("[ERRO] BMP180!"); while(1) delay(10); }

  setup_wifi(); // Wi-Fi + NTP

  // [REST] Registra rotas e inicia o WebServer na porta 80
  server.on("/",       handleRoot);
  server.on("/sensor", handleSensor);
  server.on("/status", handleStatus);
  server.on("/alertas",handleAlertas);
  server.begin();
  Serial.println("[REST] WebServer iniciado em http://" + WiFi.localIP().toString());

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  startTime = millis();
  Serial.println("Sistema pronto.");
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
  if (!client.connected()) reconnect();
  client.loop();
  server.handleClient(); // [REST] Processa requisições HTTP a cada iteração do loop

  unsigned long agora = millis();

  if (agora - lastTelemetria > 5000) {
    lastTelemetria = agora;

    // 1. MPU6050
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);
    float inclinacaoX = atan2(a.acceleration.y, a.acceleration.z) * 180.0 / PI;
    float vibracao    = abs(g.gyro.x) + abs(g.gyro.y) + abs(g.gyro.z);

    // 2. BMP180
    float pressaoHpa = bmp.readPressure() / 100.0f;

    // 3. Potenciômetro → PM2.5 / PM10
    float poluicaoPm25 = (float)map(analogRead(PIN_POT_POLUICAO), 0, 4095, 0, 300);
    float poluicaoPm10 = poluicaoPm25 * 1.5f;

    // 4. HC-SR04
    digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(2);
    digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
    digitalWrite(PIN_TRIG, LOW);
    long duracao = pulseIn(PIN_ECHO, HIGH, ULTRASONIC_TIMEOUT_US);
    bool  distValida       = (duracao > 0 && duracao < 24000);
    float distanciaAguaCm  = distValida ? (duracao * 0.034f / 2.0f) : -1.0f;
    int   waterLevelPercent = 0;
    if (distValida)
      waterLevelPercent = map(constrain((int)distanciaAguaCm, 10, 200), 200, 10, 0, 100);

    // 5. Severidades
    Severidade sevEnc  = severidadeEnchente(distanciaAguaCm, distValida);
    Severidade sevPres = severidadePressao(pressaoHpa);
    Severidade sevPM   = severidadePM25(poluicaoPm25);
    Severidade sevInc  = severidadeInclinacao(inclinacaoX);
    Severidade geral   = (Severidade)max({(int)sevEnc,(int)sevPres,(int)sevPM,(int)sevInc});

    // 6. LEDs
    bool alerta = geral > SEV_NORMAL;
    digitalWrite(PIN_LED_VERDE,    alerta ? LOW  : HIGH);
    digitalWrite(PIN_LED_VERMELHO, alerta ? HIGH : LOW);

    // 7. Buzzer não-bloqueante [GAP 1]
    if (geral == SEV_CRITICO) {
      if (!buzzerLigado && (agora - buzzerUltimoEvento >= BUZZER_BIPE_OFF_MS)) {
        ledcWriteTone(BUZZER_LEDC_CHANNEL, BUZZER_FREQ_HZ);
        buzzerLigado = true; buzzerUltimoEvento = agora;
      } else if (buzzerLigado && (agora - buzzerUltimoEvento >= BUZZER_BIPE_ON_MS)) {
        ledcWrite(BUZZER_LEDC_CHANNEL, 0);
        buzzerLigado = false; buzzerUltimoEvento = agora;
      }
      Serial.println("[BUZZER] CRITICO – Bipe ativo.");
    } else {
      ledcWrite(BUZZER_LEDC_CHANNEL, 0);
      buzzerLigado = false; buzzerUltimoEvento = agora;
    }

    // 8. Timestamp NTP [GAP 2]
    char timestampISO[25];
    struct tm timeinfo;
    if (getLocalTime(&timeinfo))
      strftime(timestampISO, sizeof(timestampISO), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    else {
      snprintf(timestampISO, sizeof(timestampISO), "1970-01-01T00:00:00");
      Serial.println("[AVISO] NTP indisponivel.");
    }

    // 9. [REST] Atualiza snapshot global para os endpoints HTTP
    strncpy(snap.timestamp,        timestampISO,        sizeof(snap.timestamp));
    snap.waterDistanceCm   = distanciaAguaCm;
    snap.waterLevelPercent = waterLevelPercent;
    snap.tiltAngle         = abs(inclinacaoX);
    snap.vibration         = vibracao;
    snap.pressureHpa       = pressaoHpa;
    snap.pm25              = poluicaoPm25;
    snap.pm10              = poluicaoPm10;
    strncpy(snap.sevEnchente,  sevStr(sevEnc),  sizeof(snap.sevEnchente));
    strncpy(snap.sevPressao,   sevStr(sevPres), sizeof(snap.sevPressao));
    strncpy(snap.sevPM,        sevStr(sevPM),   sizeof(snap.sevPM));
    strncpy(snap.sevInclinacao,sevStr(sevInc),  sizeof(snap.sevInclinacao));
    strncpy(snap.geralSev,     sevStr(geral),   sizeof(snap.geralSev));
    snapValido = true;

    // 10. OLED
    atualizarDisplay(distanciaAguaCm, distValida, pressaoHpa, poluicaoPm25,
                     abs(inclinacaoX), sevEnc, sevPres, sevPM, sevInc);

    // 11. MQTT – Telemetria [TÓPICO 1]
    char telemetria[400];
    snprintf(telemetria, sizeof(telemetria),
      "{\"stationCode\":\"APP-ST-001\","
      "\"timestamp\":\"%s\","
      "\"waterDistanceCm\":%.2f,"
      "\"waterLevelPercent\":%d,"
      "\"tiltAngle\":%.2f,"
      "\"vibration\":%.2f,"
      "\"pressureHpa\":%.2f,"
      "\"pm25\":%.2f,"
      "\"pm10\":%.2f}",
      timestampISO, distanciaAguaCm, waterLevelPercent,
      abs(inclinacaoX), vibracao, pressaoHpa, poluicaoPm25, poluicaoPm10);
    client.publish(TOPIC_TELEMETRIA, telemetria);
    Serial.print("[TELEMETRIA] "); Serial.println(telemetria);
  }

  // MQTT – Status [TÓPICO 2]
  if (agora - lastStatus > 30000) {
    lastStatus = agora;
    char statusPayload[200];
    snprintf(statusPayload, sizeof(statusPayload),
      "{\"stationCode\":\"APP-ST-001\","
      "\"uptimeSeg\":%lu,"
      "\"rssi\":%d,"
      "\"ip\":\"%s\","
      "\"versaoFirmware\":\"1.4.0\"}",
      (agora - startTime) / 1000, WiFi.RSSI(),
      WiFi.localIP().toString().c_str());
    client.publish(TOPIC_STATUS, statusPayload);
    Serial.print("[STATUS] "); Serial.println(statusPayload);
  }
}