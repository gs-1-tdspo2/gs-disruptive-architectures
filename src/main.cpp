
// Configurações de WiFi
const char *SSID = "Wokwi-GUEST"; // não precisa alterar no simulador
const char *PASSWORD = "";        // 

// Configurações de MQTT
const char *BROKER_MQTT = "broker.hivemq.com"; // seu broker mqtt
const int BROKER_PORT = 1883;
const char *ID_MQTT = "esp32_mqtt";
const char *TOPIC_SUBSCRIBE_LED = "fiap/iot/led";  // seu topico SUB
const char *TOPIC_PUBLISH_TEMP_HUMI = "fiap/iot/temphumi"; // seu tópico PUB