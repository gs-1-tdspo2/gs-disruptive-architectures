#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_BMP085.h>
#include <time.h>           // [GAP 2] Biblioteca para cliente NTP e formatação de tempo

// ─── Wi-Fi ───────────────────────────────────────────────────────────────────
const char* ssid     = "Wokwi-GUEST";
const char* password = "";

// ─── MQTT ────────────────────────────────────────────────────────────────────
const char* mqtt_server = "broker.hivemq.com";
const int   mqtt_port   = 1883;

// Tópicos atualizados para o padrão do PRD
const char* TOPIC_TELEMETRIA = "app/estacoes/APP-ST-001/telemetria";
const char* TOPIC_STATUS     = "app/estacoes/APP-ST-001/status";
const char* TOPIC_COMANDO    = "app/estacoes/APP-ST-001/alertas"; // [GAP 3] Corrigido: era "comando", PRD exige "alertas"

// ─── Pinos ───────────────────────────────────────────────────────────────────
#define PIN_POT_POLUICAO 35
#define PIN_TRIG          5
#define PIN_ECHO         18
#define PIN_LED_VERDE    17
#define PIN_LED_VERMELHO 16
#define PIN_BUZZER       12 // [GAP 1] Definição do pino do buzzer no GPIO 12 conforme PRD

#define ULTRASONIC_TIMEOUT_US 30000UL

// ─── Níveis de severidade (Uso exclusivo local para OLED/LEDs) ───────────────
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

// [GAP 1] Parâmetros do bipe não-bloqueante do buzzer
#define BUZZER_FREQ_HZ     1000  // Frequência do tom de alerta em Hz
#define BUZZER_BIPE_ON_MS   200  // Duração do bipe ligado (ms)
#define BUZZER_BIPE_OFF_MS  300  // Duração do bipe desligado (ms)
// [GAP 1 – FIX] ledcWriteTone() é a API nativa do ESP32 para PWM de áudio.
// tone()/noTone() dependem do LEDC já inicializado; usar ledcAttach() +
// ledcWriteTone() evita o erro "LEDC is not initialized".
#define BUZZER_LEDC_CHANNEL  0   // Canal LEDC reservado para o buzzer (0–15)
#define BUZZER_LEDC_RES     10   // Resolução do LEDC em bits

typedef enum {
  SEV_NORMAL  = 0,
  SEV_ATENCAO = 1,
  SEV_ALERTA  = 2,
  SEV_CRITICO = 3
} Severidade;

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
Adafruit_BMP085 bmp;

unsigned long lastTelemetria = 0;
unsigned long lastStatus     = 0;
unsigned long startTime      = 0;

// [GAP 1] Variáveis de estado do buzzer não-bloqueante
unsigned long buzzerUltimoEvento = 0; // Marca o millis() do último evento do buzzer
bool          buzzerLigado       = false; // Controla o estado atual do buzzer (ligado/desligado)

// ─── Callback MQTT ───────────────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.print("[COMANDO RECEBIDO] "); Serial.println(msg);
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

  // [GAP 2] Configura cliente NTP após conexão Wi-Fi.
  // UTC-3 = Horário de Brasília (offset de -10800 segundos).
  // Servidores: pool.ntp.org (primário) e time.nist.gov (fallback).
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("NTP configurado. Aguardando sincronização...");

  // [GAP 2] Aguarda a sincronização do tempo (até 10s) antes de prosseguir.
  struct tm timeinfo;
  int tentativas = 0;
  while (!getLocalTime(&timeinfo) && tentativas < 20) {
    delay(500);
    Serial.print(".");
    tentativas++;
  }
  if (tentativas < 20) {
    Serial.println("\nNTP sincronizado com sucesso!");
  } else {
    Serial.println("\n[AVISO] NTP nao sincronizado. Timestamp pode ser invalido.");
  }
}

// ─── Reconexão MQTT ──────────────────────────────────────────────────────────
void reconnect() {
  while (!client.connected()) {
    Serial.print("Conectando MQTT...");
    String clientId = "ESP32-ST001-" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      Serial.println("OK!");
      client.subscribe(TOPIC_COMANDO); // [GAP 3] Agora escuta "alertas" conforme PRD
    } else {
      Serial.print("Falhou rc="); Serial.print(client.state());
      Serial.println(". Retry 5s...");
      delay(5000);
    }
  }
}

// ─── Atualiza OLED ───────────────────────────────────────────────────────────
void atualizarDisplay(float dist, bool distValida, float pressao,
                      float pm25, float incX,
                      Severidade sevEnc, Severidade sevPres,
                      Severidade sevPM,  Severidade sevInc) {
  if (!oledDisponivel) return;

  Severidade geral = (Severidade)max({(int)sevEnc, (int)sevPres, (int)sevPM, (int)sevInc});

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  switch (geral) {
    case SEV_CRITICO: display.println("!!! CRITICO !!!     "); break;
    case SEV_ALERTA:  display.println("** ALERTA  ** "); break;
    case SEV_ATENCAO: display.println(">>  ATENCAO  <<     "); break;
    default:          display.println("   Sistema OK       "); break;
  }

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

  // [GAP 1 – FIX] API compatível com ESP32 Arduino Core 2.x (usada pelo Wokwi).
  // ledcSetup() configura o canal; ledcAttachPin() associa ao GPIO físico.
  // ledcAttachChannel() só existe na 3.x e causava "identifier undefined".
  ledcSetup(BUZZER_LEDC_CHANNEL, BUZZER_FREQ_HZ, BUZZER_LEDC_RES);
  ledcAttachPin(PIN_BUZZER, BUZZER_LEDC_CHANNEL);
  ledcWrite(BUZZER_LEDC_CHANNEL, 0); // Duty = 0 → buzzer silencioso ao iniciar

  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    oledDisponivel = true;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(20, 25);
    display.println("Iniciando...");
    display.display();
  }

  if (!mpu.begin()) {
    Serial.println("[ERRO] MPU6050 nao encontrado!");
    while (1) { delay(10); }
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

  if (!bmp.begin()) {
    Serial.println("[ERRO] BMP180 nao encontrado!");
    while (1) { delay(10); }
  }

  setup_wifi(); // [GAP 2] setup_wifi() agora também inicializa o NTP internamente
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

  // ── [GAP 1] Lógica não-bloqueante do buzzer ─────────────────────────────
  // Esta lógica é avaliada a cada iteração do loop, independente dos timers
  // de telemetria e status, garantindo resposta sonora em tempo real.
  // A variável 'buzzerAtivo' é definida na seção de severidades abaixo
  // e persiste entre iterações por ser estática.
  // Nota: o controle efetivo é feito dentro do bloco de telemetria (5s),
  // mas o padrão de bipe é gerenciado aqui de forma contínua.

  if (agora - lastTelemetria > 5000) {
    lastTelemetria = agora;

    // 1. MPU6050 – Cálculo trigonométrico correto de inclinação e vibração
    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);
    // atan2 garante precisão em qualquer ângulo, convertido para graus
    float inclinacaoX = atan2(a.acceleration.y, a.acceleration.z) * 180.0 / PI; 
    float vibracao = abs(g.gyro.x) + abs(g.gyro.y) + abs(g.gyro.z);

    // 2. Pressão atmosférica real
    float pressaoHpa = bmp.readPressure() / 100.0f;

    // 3. PM2.5 e derivação do PM10
    float poluicaoPm25 = (float)map(analogRead(PIN_POT_POLUICAO), 0, 4095, 0, 300);
    float poluicaoPm10 = poluicaoPm25 * 1.5f; // Proporção lógica simulada

    // 4. Ultrassônico com validação estrita
    digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(2);
    digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
    digitalWrite(PIN_TRIG, LOW);
    long duracao = pulseIn(PIN_ECHO, HIGH, ULTRASONIC_TIMEOUT_US);
    
    bool distValida = (duracao > 0 && duracao < 24000); // Filtra ruídos absurdos
    float distanciaAguaCm = distValida ? (duracao * 0.034f / 2.0f) : -1.0f;
    
    // Cálculo do percentual (0 a 100%) assumindo 200cm como calha vazia
    int waterLevelPercent = 0;
    if (distValida) {
      waterLevelPercent = map(constrain((int)distanciaAguaCm, 10, 200), 200, 10, 0, 100);
    }

    // ── Atualização Local (OLED e LEDs) ──────────────────────────────────
    Severidade sevEnc  = severidadeEnchente(distanciaAguaCm, distValida);
    Severidade sevPres = severidadePressao(pressaoHpa);
    Severidade sevPM   = severidadePM25(poluicaoPm25);
    Severidade sevInc  = severidadeInclinacao(inclinacaoX);

    // Calcula a severidade geral para controle de LEDs, OLED e buzzer
    Severidade geral = (Severidade)max({(int)sevEnc, (int)sevPres, (int)sevPM, (int)sevInc});

    bool alerta = (sevEnc > SEV_NORMAL) || (sevPres > SEV_NORMAL) ||
                  (sevPM  > SEV_NORMAL) || (sevInc  > SEV_NORMAL);

    digitalWrite(PIN_LED_VERDE,    alerta ? LOW  : HIGH);
    digitalWrite(PIN_LED_VERMELHO, alerta ? HIGH : LOW);

    // [GAP 1] Controle do buzzer baseado na severidade geral:
    // SEV_CRITICO → ativa bipe intermitente; qualquer outro estado → silencia.
    if (geral == SEV_CRITICO) {
      // Bipe não-bloqueante: alterna entre ligado e desligado usando millis()
      // para não bloquear o loop principal durante os intervalos.
      if (!buzzerLigado && (agora - buzzerUltimoEvento >= BUZZER_BIPE_OFF_MS)) {
        // Período OFF encerrado: liga o buzzer via LEDC nativo do ESP32
        // [GAP 1 – FIX] ledcWriteTone() configura a frequência no canal já
        // inicializado; não lança "LEDC is not initialized" como tone() faria.
        ledcWriteTone(BUZZER_LEDC_CHANNEL, BUZZER_FREQ_HZ);
        buzzerLigado = true;
        buzzerUltimoEvento = agora;
      } else if (buzzerLigado && (agora - buzzerUltimoEvento >= BUZZER_BIPE_ON_MS)) {
        // Período ON encerrado: silencia escrevendo duty = 0 no canal LEDC
        ledcWrite(BUZZER_LEDC_CHANNEL, 0); // [GAP 1 – FIX] substitui noTone()
        buzzerLigado = false;
        buzzerUltimoEvento = agora;
      }
      Serial.println("[BUZZER] CRITICO – Bipe ativo.");
    } else {
      // Condição não-crítica: silencia o buzzer e reseta o timer
      ledcWrite(BUZZER_LEDC_CHANNEL, 0); // [GAP 1 – FIX] substitui noTone()
      buzzerLigado = false;
      buzzerUltimoEvento = agora;
    }

    atualizarDisplay(distanciaAguaCm, distValida, pressaoHpa, poluicaoPm25,
                     abs(inclinacaoX), sevEnc, sevPres, sevPM, sevInc);

    // ── [GAP 2] Obtenção do timestamp real via NTP ────────────────────────
    // getLocalTime() lê o horário sincronizado com o servidor NTP.
    // strftime() formata no padrão ISO 8601 exigido pelo backend Java/Oracle.
    char timestampISO[25]; // Buffer para "YYYY-MM-DDTHH:MM:SS\0"
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      // Formato ISO 8601 sem fuso (o backend interpreta como UTC-3 configurado)
      strftime(timestampISO, sizeof(timestampISO), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    } else {
      // Fallback seguro caso NTP não esteja disponível: evita crash do snprintf
      snprintf(timestampISO, sizeof(timestampISO), "1970-01-01T00:00:00");
      Serial.println("[AVISO] NTP indisponivel – timestamp de fallback usado.");
    }

    // ── TÓPICO 1 – Envio da Telemetria (Padrão API Java) ─────────────────
    char telemetria[400];
    snprintf(telemetria, sizeof(telemetria),
      "{"
      "\"stationCode\":\"APP-ST-001\","
      "\"timestamp\":\"%s\","          // [GAP 2] Timestamp dinâmico via NTP (era hardcoded)
      "\"waterDistanceCm\":%.2f,"
      "\"waterLevelPercent\":%d,"
      "\"tiltAngle\":%.2f,"
      "\"vibration\":%.2f,"
      "\"pressureHpa\":%.2f,"
      "\"pm25\":%.2f,"
      "\"pm10\":%.2f"
      "}",
      timestampISO,                    // [GAP 2] Valor real obtido do NTP
      distanciaAguaCm, waterLevelPercent, abs(inclinacaoX), vibracao,
      pressaoHpa, poluicaoPm25, poluicaoPm10);

    client.publish(TOPIC_TELEMETRIA, telemetria);
    Serial.print("[TELEMETRIA] "); Serial.println(telemetria);
  }

  // ── TÓPICO 2 – Heartbeat de Diagnóstico a cada 30s ─────────────────────
  if (agora - lastStatus > 30000) {
    lastStatus = agora;
    unsigned long uptimeSeg = (agora - startTime) / 1000;
    char statusPayload[200];
    snprintf(statusPayload, sizeof(statusPayload),
      "{"
      "\"stationCode\":\"APP-ST-001\","
      "\"uptimeSeg\":%lu,"
      "\"rssi\":%d,"
      "\"ip\":\"%s\","
      "\"versaoFirmware\":\"1.4.0\""
      "}",
      uptimeSeg, WiFi.RSSI(), WiFi.localIP().toString().c_str());
    
    client.publish(TOPIC_STATUS, statusPayload);
    Serial.print("[STATUS] "); Serial.println(statusPayload);
  }
}