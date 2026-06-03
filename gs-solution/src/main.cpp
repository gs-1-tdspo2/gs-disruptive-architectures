#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_BMP085.h>
#include <time.h>           // Biblioteca para cliente NTP e formatação de tempo

// ─── Wi-Fi ───────────────────────────────────────────────────────────────────
const char* ssid     = "Wokwi-GUEST";
const char* password = "";

// ─── MQTT ────────────────────────────────────────────────────────────────────
const char* mqtt_server = "mqtt-dashboard.com";
const int   mqtt_port   = 1883;

const char* TOPIC_TELEMETRIA = "app/estacoes/AMANAJE-SP-RP-001/telemetria";
const char* TOPIC_STATUS     = "app/estacoes/AMANAJE-SP-RP-001/status";
const char* TOPIC_COMANDO    = "app/estacoes/AMANAJE-SP-RP-001/alertas";

// ─── Pinos ───────────────────────────────────────────────────────────────────
#define PIN_POT_POLUICAO 35
#define PIN_TRIG          5
#define PIN_ECHO         18
#define PIN_LED_VERDE    17
#define PIN_LED_VERMELHO 16
#define PIN_BUZZER       12 // [GAP 1] GPIO 12 conforme PRD

#define ULTRASONIC_TIMEOUT_US 30000UL

// ─── OLED ────────────────────────────────────────────────────────────────────
#define BUZZER_FREQ_HZ      1000
#define BUZZER_LEDC_CHANNEL    0
#define BUZZER_LEDC_RES       10

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

unsigned long lastTelemetria    = 0;
unsigned long lastStatus        = 0;
unsigned long startTime         = 0;

// ─── Estado do alerta recebido via MQTT (Java → ESP32) ───────────────────────
struct AlertaMQTT {
  bool  ativo         = false;   // true enquanto a tela de alerta deve ser exibida
  unsigned long recebidoMs = 0;  // millis() no momento do recebimento
  char  nivelRisco[12]   = "";
  char  tipoPrincipal[20]= "";
  int   score            = 0;
  char  mensagem[80]     = "";
  bool  ledVerde         = true;
  bool  ledVermelho      = false;
  bool  buzzer           = false;
} alertaMQTT;

#define ALERTA_OLED_DURACAO_MS 5000  // Tempo que a tela de alerta fica visível

// ─── Callback MQTT ───────────────────────────────────────────────────────────
// Recebe o payload de alertas publicado pelo Java e atualiza o estado global.
// Campos esperados: ledVerde, ledVermelho, buzzer, nivelRisco,
//                   tipoRiscoPrincipal, score, mensagem.
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];

  Serial.println("-------------------------------------");
  Serial.println("[MQTT] Topico  : " + String(topic));
  Serial.println("[MQTT] Tamanho : " + String(length) + " bytes");
  Serial.println("[MQTT] Payload : " + msg);
  Serial.println("-------------------------------------");

  // ── Extrai ledVerde ──────────────────────────────────────────────────────
  int idxLV = msg.indexOf("\"ledVerde\"");
  if (idxLV >= 0) {
    String sub = msg.substring(idxLV + 10);
    sub.trim();
    if (sub.startsWith(":")) { sub = sub.substring(1); sub.trim(); }
    alertaMQTT.ledVerde = sub.startsWith("true");
  }

  // ── Extrai ledVermelho ───────────────────────────────────────────────────
  int idxLR = msg.indexOf("\"ledVermelho\"");
  if (idxLR >= 0) {
    String sub = msg.substring(idxLR + 13);
    sub.trim();
    if (sub.startsWith(":")) { sub = sub.substring(1); sub.trim(); }
    alertaMQTT.ledVermelho = sub.startsWith("true");
  }

  // ── Extrai buzzer ────────────────────────────────────────────────────────
  int idxBZ = msg.indexOf("\"buzzer\"");
  if (idxBZ >= 0) {
    String sub = msg.substring(idxBZ + 8);
    sub.trim();
    if (sub.startsWith(":")) { sub = sub.substring(1); sub.trim(); }
    alertaMQTT.buzzer = sub.startsWith("true");
  }

  // ── Extrai nivelRisco ────────────────────────────────────────────────────
  int idxNR = msg.indexOf("\"nivelRisco\"");
  if (idxNR >= 0) {
    int ini = msg.indexOf('"', idxNR + 12);
    int fim = (ini >= 0) ? msg.indexOf('"', ini + 1) : -1;
    if (ini >= 0 && fim > ini) {
      String val = msg.substring(ini + 1, fim);
      val.toCharArray(alertaMQTT.nivelRisco, sizeof(alertaMQTT.nivelRisco));
    }
  }

  // ── Extrai tipoRiscoPrincipal ────────────────────────────────────────────
  int idxTP = msg.indexOf("\"tipoRiscoPrincipal\"");
  if (idxTP >= 0) {
    int ini = msg.indexOf('"', idxTP + 20);
    int fim = (ini >= 0) ? msg.indexOf('"', ini + 1) : -1;
    if (ini >= 0 && fim > ini) {
      String val = msg.substring(ini + 1, fim);
      val.toCharArray(alertaMQTT.tipoPrincipal, sizeof(alertaMQTT.tipoPrincipal));
    }
  }

  // ── Extrai score ─────────────────────────────────────────────────────────
  int idxSC = msg.indexOf("\"score\"");
  if (idxSC >= 0) {
    int ini = idxSC + 7;
    while (ini < (int)msg.length() && (msg[ini] == ':' || msg[ini] == ' ')) ini++;
    alertaMQTT.score = msg.substring(ini).toInt();
  }

  // ── Extrai mensagem (primeiros 79 chars) ─────────────────────────────────
  int idxMSG = msg.indexOf("\"mensagem\"");
  if (idxMSG >= 0) {
    int ini = msg.indexOf('"', idxMSG + 10);
    int fim = (ini >= 0) ? msg.indexOf('"', ini + 1) : -1;
    if (ini >= 0 && fim > ini) {
      String val = msg.substring(ini + 1, fim);
      val.toCharArray(alertaMQTT.mensagem, sizeof(alertaMQTT.mensagem));
    }
  }

  // ── Aplica LEDs imediatamente ────────────────────────────────────────────
  digitalWrite(PIN_LED_VERDE,    alertaMQTT.ledVerde    ? HIGH : LOW);
  digitalWrite(PIN_LED_VERMELHO, alertaMQTT.ledVermelho ? HIGH : LOW);

  // Buzzer: se o campo buzzer=true, ativa a sirene; caso contrário, desliga.
  if (alertaMQTT.buzzer) {
    ledcWriteTone(BUZZER_LEDC_CHANNEL, BUZZER_FREQ_HZ);
  } else {
    ledcWrite(BUZZER_LEDC_CHANNEL, 0);
  }

  // ── Log de confirmação dos campos parseados ──────────────────────────────
  Serial.println("[ALERTA] nivelRisco    : " + String(alertaMQTT.nivelRisco));
  Serial.println("[ALERTA] tipoPrincipal : " + String(alertaMQTT.tipoPrincipal));
  Serial.println("[ALERTA] score         : " + String(alertaMQTT.score));
  Serial.println("[ALERTA] ledVerde      : " + String(alertaMQTT.ledVerde    ? "true" : "false"));
  Serial.println("[ALERTA] ledVermelho   : " + String(alertaMQTT.ledVermelho ? "true" : "false"));
  Serial.println("[ALERTA] buzzer        : " + String(alertaMQTT.buzzer      ? "true" : "false"));
  Serial.println("[ALERTA] mensagem      : " + String(alertaMQTT.mensagem));
  Serial.println("[ALERTA] Atuadores aplicados. OLED em modo alerta por 5s.");

  // ── Ativa exibição do alerta na OLED ────────────────────────────────────
  alertaMQTT.ativo      = true;
  alertaMQTT.recebidoMs = millis();
}

// ─── Wi-Fi + NTP ─────────────────────────────────────────────────────────────
void setup_wifi() {
  delay(10);
  Serial.println("\n--- Conectando ao WiFi ---");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi OK: " + WiFi.localIP().toString());
  Serial.println("MAC: " + WiFi.macAddress());

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
    Serial.print("[MQTT] Conectando ao broker " + String(mqtt_server) + "...");
    String id = "ESP32-ST001-" + String(random(0xffff), HEX);
    if (client.connect(id.c_str())) {
      Serial.println(" OK! ClientID: " + id);
      bool subOk = client.subscribe(TOPIC_COMANDO);
      Serial.println("[MQTT] Subscribe em '" + String(TOPIC_COMANDO) + "': " + (subOk ? "OK" : "FALHOU"));
    } else {
      Serial.println("[MQTT] Falhou rc=" + String(client.state()) + ". Retry 5s...");
      delay(5000);
    }
  }
}

// ─── OLED – tela de telemetria normal ────────────────────────────────────────
void atualizarDisplay(float dist, bool distValida, float pressao, float pm25, float incX) {
  if (!oledDisponivel) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);  display.println("   Sistema OK       ");
  display.setCursor(0, 10);
  if (distValida) { char b[22]; snprintf(b,22,"Agua %5.1fcm",dist);    display.println(b); }
  else              display.println("Agua  --ERRO--");
  display.setCursor(0, 20);
  { char b[22]; snprintf(b,22,"Pres %6.1fhPa",pressao);  display.println(b); }
  display.setCursor(0, 30);
  { char b[22]; snprintf(b,22,"PM25 %5.1fug/m3",pm25);   display.println(b); }
  display.setCursor(0, 40);
  { char b[22]; snprintf(b,22,"Inc  %5.1f graus",incX);  display.println(b); }
  display.setCursor(0, 52);
  display.println(WiFi.localIP().toString());
  display.display();
}

// ─── OLED – tela de alerta recebido do Java ──────────────────────────────────
// Exibida por ALERTA_OLED_DURACAO_MS ms após receber mensagem no tópico alertas.
void exibirTelaAlerta() {
  if (!oledDisponivel) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Linha 0 – nível de risco em destaque
  display.setCursor(0, 0);
  char titulo[22];
  snprintf(titulo, sizeof(titulo), "RISCO %s", alertaMQTT.nivelRisco);
  display.println(titulo);

  // Linha 1 – tipo principal
  display.setCursor(0, 12);
  char tipo[22];
  snprintf(tipo, sizeof(tipo), "Tipo: %s", alertaMQTT.tipoPrincipal);
  display.println(tipo);

  // Linha 2 – score
  display.setCursor(0, 24);
  char sc[22];
  snprintf(sc, sizeof(sc), "Score: %d", alertaMQTT.score);
  display.println(sc);

  // Linhas 3-4 – trecho da mensagem (max 2 linhas de 21 chars)
  display.setCursor(0, 36);
  char linha1[22], linha2[22];
  strncpy(linha1, alertaMQTT.mensagem,      21); linha1[21] = '\0';
  strncpy(linha2, alertaMQTT.mensagem + 21, 21); linha2[21] = '\0';
  display.println(linha1);
  display.setCursor(0, 48);
  display.println(linha2);

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

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
  // Buffer padrão do PubSubClient é 128 bytes — payloads de alerta chegam a
  // ~250 bytes e seriam descartados silenciosamente sem essa linha.
  client.setBufferSize(512);

  startTime = millis();
  Serial.println("Sistema pronto.");
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
  if (!client.connected()) reconnect();
  client.loop();

  unsigned long agora = millis();

  // ── Gerencia expiração da tela de alerta MQTT ─────────────────────────────
  if (alertaMQTT.ativo && (agora - alertaMQTT.recebidoMs >= ALERTA_OLED_DURACAO_MS)) {
    alertaMQTT.ativo = false;
    Serial.println("[ALERTA] Tela de alerta encerrada. Retornando à telemetria.");
  }

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

    // 5. Timestamp NTP
    char timestampISO[25];
    struct tm timeinfo;
    if (getLocalTime(&timeinfo))
      strftime(timestampISO, sizeof(timestampISO), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    else {
      snprintf(timestampISO, sizeof(timestampISO), "1970-01-01T00:00:00");
      Serial.println("[AVISO] NTP indisponivel.");
    }

    // 6. OLED: tela de alerta MQTT ou telemetria normal
    if (alertaMQTT.ativo) {
      exibirTelaAlerta();
    } else {
      atualizarDisplay(distanciaAguaCm, distValida, pressaoHpa, poluicaoPm25, abs(inclinacaoX));
    }

    // 7. MQTT – Telemetria
    char telemetria[400];
    snprintf(telemetria, sizeof(telemetria),
      "{\"stationCode\":\"AMANAJE-SP-RP-001\","
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
      "{\"stationCode\":\"AMANAJE-SP-RP-001\","
      "\"mac\":\"%s\","
      "\"uptimeSeg\":%lu,"
      "\"rssi\":%d,"
      "\"ip\":\"%s\","
      "\"versaoFirmware\":\"1.4.0\"}",
      WiFi.macAddress().c_str(), (agora - startTime) / 1000, WiFi.RSSI(),
      WiFi.localIP().toString().c_str());
    client.publish(TOPIC_STATUS, statusPayload);
    Serial.print("[STATUS] "); Serial.println(statusPayload);
  }
}