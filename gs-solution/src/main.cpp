#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_SSD1306.h>

// ─── Wi-Fi ───────────────────────────────────────────────────────────────────
const char* ssid     = "Wokwi-GUEST";
const char* password = "";

// ─── MQTT ────────────────────────────────────────────────────────────────────
const char* mqtt_server = "broker.hivemq.com";
const int   mqtt_port   = 1883;

const char* TOPIC_TELEMETRIA = "fiap/global_solution/estacao1/telemetria";
const char* TOPIC_ALERTAS    = "fiap/global_solution/estacao1/alertas";
const char* TOPIC_STATUS     = "fiap/global_solution/estacao1/status";
const char* TOPIC_COMANDO    = "fiap/global_solution/estacao1/comando";

// ─── Pinos ───────────────────────────────────────────────────────────────────
#define PIN_POT_PRESSAO  34
#define PIN_POT_POLUICAO 35
#define PIN_TRIG          5
#define PIN_ECHO         18
#define PIN_LED_VERDE    17   // SAÍDA 1 – status normal
#define PIN_LED_VERMELHO 16   // SAÍDA 2 – qualquer alerta ativo

#define ULTRASONIC_TIMEOUT_US 30000UL

// ─── Níveis de severidade ────────────────────────────────────────────────────
// Cada sensor possui 3 faixas de risco progressivas.
// O sistema alerta o backend e as pessoas com antecedência antes do crítico.

// Enchente: sensor no topo aponta para baixo → distância pequena = perigo
#define ENC_ATENCAO_CM   120.0f   // água a menos de 120 cm do sensor
#define ENC_ALERTA_CM     80.0f   // água a menos de 80 cm
#define ENC_CRITICO_CM    50.0f   // água a menos de 50 cm

// Poluição PM2.5 (ug/m³) — baseado nas faixas da OMS/CONAMA
#define PM_ATENCAO        50.0f   // moderado
#define PM_ALERTA        100.0f   // ruim
#define PM_CRITICO       200.0f   // muito ruim / perigoso

// Inclinação (graus) — encosta em movimento
#define INC_ATENCAO       15.0f
#define INC_ALERTA        25.0f
#define INC_CRITICO       35.0f

// Pressão atmosférica (hPa) — baseado na escala Saffir-Simpson (NOAA) e meteorologia real.
// Pressão padrão ao nível do mar: 1013.25 hPa (NOAA).
// Range simulado: 1030 (pot zerado, alta pressão) → 980 (pot máximo, furacão Cat.1).
#define PRES_ATENCAO    1005.0f   // baixa pressão moderada — tempo mudando
#define PRES_ALERTA      992.0f   // depressão tropical / tempestade severa (Saffir-Simpson)
#define PRES_CRITICO     980.0f   // limiar furacão Categoria 1 (NOAA/Saffir-Simpson)

// ─── Enum de severidade ──────────────────────────────────────────────────────
typedef enum {
  SEV_NORMAL  = 0,
  SEV_ATENCAO = 1,
  SEV_ALERTA  = 2,
  SEV_CRITICO = 3
} Severidade;

const char* nomeSeveridade(Severidade s) {
  switch (s) {
    case SEV_ATENCAO: return "ATENCAO";
    case SEV_ALERTA:  return "ALERTA";
    case SEV_CRITICO: return "CRITICO";
    default:          return "NORMAL";
  }
}

// ─── Calcula severidade de cada sensor ───────────────────────────────────────
Severidade severidadeEnchente(float dist, bool valida) {
  if (!valida) return SEV_NORMAL;
  if (dist < ENC_CRITICO_CM)  return SEV_CRITICO;
  if (dist < ENC_ALERTA_CM)   return SEV_ALERTA;
  if (dist < ENC_ATENCAO_CM)  return SEV_ATENCAO;
  return SEV_NORMAL;
}

Severidade severidadePM25(float pm) {
  if (pm > PM_CRITICO)  return SEV_CRITICO;
  if (pm > PM_ALERTA)   return SEV_ALERTA;
  if (pm > PM_ATENCAO)  return SEV_ATENCAO;
  return SEV_NORMAL;
}

Severidade severidadeInclinacao(float inc) {
  float v = abs(inc);
  if (v > INC_CRITICO)  return SEV_CRITICO;
  if (v > INC_ALERTA)   return SEV_ALERTA;
  if (v > INC_ATENCAO)  return SEV_ATENCAO;
  return SEV_NORMAL;
}

// Pressão baixa → risco de tempestade → alerta abaixo dos thresholds
// Usa <= para incluir o valor exato do threshold (ex: 980.00 = CRITICO)
Severidade severidadePressao(float hpa) {
  if (hpa <= PRES_CRITICO)  return SEV_CRITICO;
  if (hpa <= PRES_ALERTA)   return SEV_ALERTA;
  if (hpa <= PRES_ATENCAO)  return SEV_ATENCAO;
  return SEV_NORMAL;
}

// ─── OLED 128×64 ─────────────────────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledDisponivel = false;

WiFiClient   espClient;
PubSubClient client(espClient);
Adafruit_MPU6050 mpu;

unsigned long lastTelemetria = 0;
unsigned long lastStatus     = 0;
unsigned long startTime      = 0;

// ─── Callback MQTT ───────────────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.print("[COMANDO] "); Serial.print(topic);
  Serial.print(" | "); Serial.println(msg);
  if (msg.indexOf("reset") >= 0) ESP.restart();
}

// ─── Wi-Fi ───────────────────────────────────────────────────────────────────
void setup_wifi() {
  delay(10);
  Serial.println("\n--- Conectando ao WiFi ---");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi OK: " + WiFi.localIP().toString());
}

// ─── Reconexão MQTT ──────────────────────────────────────────────────────────
void reconnect() {
  while (!client.connected()) {
    Serial.print("Conectando MQTT...");
    String clientId = "ESP32-GS-" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      Serial.println("OK!");
      client.subscribe(TOPIC_COMANDO);
    } else {
      Serial.print("Falhou rc="); Serial.print(client.state());
      Serial.println(". Retry 5s...");
      delay(5000);
    }
  }
}

// ─── Publica alerta individual se severidade > NORMAL ────────────────────────
void publicarAlertaSensor(const char* tipo, const char* unidade,
                           float valor, Severidade sev) {
  if (sev == SEV_NORMAL) return;
  char payload[220];
  snprintf(payload, sizeof(payload),
    "{\"codigoEstacao\":\"APP-ST-001\","
    "\"tipo\":\"%s\","
    "\"valor\":%.2f,"
    "\"unidade\":\"%s\","
    "\"severidade\":\"%s\"}",
    tipo, valor, unidade, nomeSeveridade(sev));
  client.publish(TOPIC_ALERTAS, payload);
  Serial.print("[ALERTA] "); Serial.println(payload);
}

// ─── Atualiza OLED ───────────────────────────────────────────────────────────
// Linha 0 (y=0):  status geral
// Linhas 1-4:     valor de cada sensor com indicador de severidade inline
// Linha 5 (y=56): alertas ativos em abreviatura, ex: [ENC:CRIT][PM:ATEN]
void atualizarDisplay(float dist, bool distValida, float pressao,
                      float pm25, float incX,
                      Severidade sevEnc, Severidade sevPres,
                      Severidade sevPM,  Severidade sevInc) {
  if (!oledDisponivel) return;

  // Severidade geral = maior entre todas
  Severidade geral = (Severidade)max({(int)sevEnc, (int)sevPres,
                                      (int)sevPM,  (int)sevInc});

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // ── Linha 0: status geral ─────────────────────────────────────────────────
  display.setCursor(0, 0);
  switch (geral) {
    case SEV_CRITICO: display.println("!!! CRITICO !!!     "); break;
    case SEV_ALERTA:  display.println("**  ALERTA  **      "); break;
    case SEV_ATENCAO: display.println(">>  ATENCAO  <<     "); break;
    default:          display.println("   Sistema OK       "); break;
  }

  // ── Linhas 1-4: sensores com badge de severidade ──────────────────────────
  // Formato: "LABEL  valor un [SEV]" — badge omitido se NORMAL
  auto badge = [](Severidade s) -> const char* {
    switch (s) {
      case SEV_CRITICO: return "[!!!]";
      case SEV_ALERTA:  return "[ ! ]";
      case SEV_ATENCAO: return "[ ? ]";
      default:          return "     ";
    }
  };

  display.setCursor(0, 10);
  if (distValida) {
    char buf[22];
    snprintf(buf, sizeof(buf), "Agua %5.1fcm %s", dist, badge(sevEnc));
    display.println(buf);
  } else {
    display.println("Agua  --ERRO--      ");
  }

  display.setCursor(0, 20);
  { char buf[22];
    snprintf(buf, sizeof(buf), "Pres %6.1fhP%s", pressao, badge(sevPres));
    display.println(buf); }

  display.setCursor(0, 30);
  { char buf[22];
    snprintf(buf, sizeof(buf), "PM25 %5.1fug %s", pm25, badge(sevPM));
    display.println(buf); }

  display.setCursor(0, 40);
  { char buf[22];
    snprintf(buf, sizeof(buf), "Incl %5.1f gr%s", incX, badge(sevInc));
    display.println(buf); }

  // ── Linha 5: resumo de alertas ativos ────────────────────────────────────
  // Mostra só os sensores fora do normal com seu nível abreviado
  display.setCursor(0, 52);
  String resumo = "";
  auto abrev = [](Severidade s) -> const char* {
    switch (s) {
      case SEV_CRITICO: return "CRT";
      case SEV_ALERTA:  return "ALT";
      case SEV_ATENCAO: return "ATN";
      default:          return "";
    }
  };
  if (sevEnc  != SEV_NORMAL) { resumo += "ENC:"; resumo += abrev(sevEnc);  resumo += " "; }
  if (sevPres != SEV_NORMAL) { resumo += "PRE:"; resumo += abrev(sevPres); resumo += " "; }
  if (sevPM   != SEV_NORMAL) { resumo += "PM:";  resumo += abrev(sevPM);   resumo += " "; }
  if (sevInc  != SEV_NORMAL) { resumo += "INC:"; resumo += abrev(sevInc);  resumo += " "; }
  if (resumo.length() == 0) resumo = "APP-ST-001";
  display.println(resumo);

  display.display();
}

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  pinMode(PIN_LED_VERDE,    OUTPUT);
  pinMode(PIN_LED_VERMELHO, OUTPUT);
  digitalWrite(PIN_LED_VERDE,    HIGH);
  digitalWrite(PIN_LED_VERMELHO, LOW);

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);

  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    oledDisponivel = true;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(20, 25);
    display.println("Iniciando...");
    display.display();
  } else {
    Serial.println("[AVISO] OLED ausente. Continuando sem display.");
  }

  if (!mpu.begin()) {
    Serial.println("[ERRO] MPU6050 nao encontrado!");
    while (1) { delay(10); }
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  startTime = millis();
  Serial.println("Sistema pronto.");
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
  if (!client.connected()) reconnect();
  client.loop();

  unsigned long agora = millis();

  if (agora - lastTelemetria > 5000) {
    lastTelemetria = agora;

    // 1. MPU6050 – inclinação
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);
    float inclinacaoX = (a.acceleration.x / 9.81) * 90.0;
    float inclinacaoY = (a.acceleration.y / 9.81) * 90.0;

    // 2. Pressão atmosférica (pot1) — cast explícito: map() retorna long
    // Pot zerado = 1030 hPa (alta pressão, tempo bom — NORMAL).
    // Pot no máximo = 980 hPa (limiar furacão Cat.1, Saffir-Simpson/NOAA — CRITICO).
    // Girar o pot simula queda de pressão com a chegada de uma tempestade.
    float pressaoHpa = (float)map(analogRead(PIN_POT_PRESSAO), 0, 4095, 1030, 980);

    // 3. PM2.5 (pot2)
    float poluicaoPm25 = (float)map(analogRead(PIN_POT_POLUICAO), 0, 4095, 0, 300);

    // 4. Ultrassônico com timeout — distância 0 = leitura inválida
    digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(2);
    digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
    digitalWrite(PIN_TRIG, LOW);
    long duracao = pulseIn(PIN_ECHO, HIGH, ULTRASONIC_TIMEOUT_US);
    bool distValida     = (duracao > 0);
    float distanciaAguaCm = distValida ? (duracao * 0.034f / 2.0f) : -1.0f;

    // ── Severidade de cada sensor ─────────────────────────────────────────
    Severidade sevEnc  = severidadeEnchente(distanciaAguaCm, distValida);
    Severidade sevPres = severidadePressao(pressaoHpa);
    Severidade sevPM   = severidadePM25(poluicaoPm25);
    Severidade sevInc  = severidadeInclinacao(inclinacaoX);

    // Qualquer sensor fora do normal acende LED vermelho
    bool alerta = (sevEnc > SEV_NORMAL) || (sevPres > SEV_NORMAL) ||
                  (sevPM  > SEV_NORMAL) || (sevInc  > SEV_NORMAL);

    digitalWrite(PIN_LED_VERDE,    alerta ? LOW  : HIGH);
    digitalWrite(PIN_LED_VERMELHO, alerta ? HIGH : LOW);

    // ── OLED ─────────────────────────────────────────────────────────────
    atualizarDisplay(distanciaAguaCm, distValida, pressaoHpa, poluicaoPm25,
                     inclinacaoX, sevEnc, sevPres, sevPM, sevInc);

    // ── TÓPICO 1 – Telemetria completa ───────────────────────────────────
    char telemetria[450];
    snprintf(telemetria, sizeof(telemetria),
      "{\"codigoEstacao\":\"APP-ST-001\","
      "\"distanciaAguaCm\":%.2f,\"distValida\":%s,"
      "\"sevEnchente\":\"%s\","
      "\"pressaoHpa\":%.2f,\"sevPressao\":\"%s\","
      "\"poluicaoPm25\":%.2f,\"sevPM25\":\"%s\","
      "\"inclinacaoX\":%.2f,\"inclinacaoY\":%.2f,\"sevInclinacao\":\"%s\","
      "\"alertaGeral\":%s}",
      distValida ? distanciaAguaCm : -1.0f,
      distValida ? "true" : "false",
      nomeSeveridade(sevEnc),
      pressaoHpa,    nomeSeveridade(sevPres),
      poluicaoPm25,  nomeSeveridade(sevPM),
      inclinacaoX, inclinacaoY, nomeSeveridade(sevInc),
      alerta ? "true" : "false");

    client.publish(TOPIC_TELEMETRIA, telemetria);
    Serial.print("[TELEMETRIA] "); Serial.println(telemetria);

    // ── TÓPICO 2 – 1 mensagem por sensor com alerta ativo ────────────────
    // Cada sensor publica independentemente com seu nível real de severidade
    publicarAlertaSensor("ENCHENTE",     "cm",    distanciaAguaCm, sevEnc);
    publicarAlertaSensor("TEMPESTADE",   "hPa",   pressaoHpa,      sevPres);
    publicarAlertaSensor("POLUICAO",     "ug/m3", poluicaoPm25,    sevPM);
    publicarAlertaSensor("DESLIZAMENTO", "graus", abs(inclinacaoX),sevInc);
  }

  // ── TÓPICO 3 – Heartbeat a cada 30s ──────────────────────────────────────
  if (agora - lastStatus > 30000) {
    lastStatus = agora;
    unsigned long uptimeSeg = (agora - startTime) / 1000;
    char statusPayload[220];
    snprintf(statusPayload, sizeof(statusPayload),
      "{\"codigoEstacao\":\"APP-ST-001\","
      "\"uptimeSeg\":%lu,\"rssi\":%d,"
      "\"ip\":\"%s\",\"oled\":%s,"
      "\"firmware\":\"1.4.0\"}",
      uptimeSeg, WiFi.RSSI(),
      WiFi.localIP().toString().c_str(),
      oledDisponivel ? "true" : "false");
    client.publish(TOPIC_STATUS, statusPayload);
    Serial.print("[STATUS] "); Serial.println(statusPayload);
  }
}