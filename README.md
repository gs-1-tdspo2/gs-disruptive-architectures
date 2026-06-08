# Amanajé — Estação de Monitoramento de Risco Ambiental

Projeto desenvolvido para a Global Solution 2026 da FIAP, disciplina Disruptive Architectures: IoT, IoB & Generative IA.

O sistema implementa uma estação de monitoramento ambiental utilizando um microcontrolador ESP32, com foco na prevenção e antecipação de desastres naturais comuns em regiões metropolitanas brasileiras: enchentes, deslizamentos de encosta, tempestades e poluição do ar.

A estação coleta dados de múltiplos sensores a cada 5 segundos, publica telemetria e status via MQTT e recebe alertas processados por um backend Java. Ao receber um alerta, o dispositivo aciona LED vermelho e buzzer conforme o nível de risco determinado pelo backend, e exibe o resultado no display OLED local.

---

## Integrantes

| Nome | RM |
|---|---|
| Gustavo Crevelari | 561408 |
| Lucca Gomes | 561996 |
| Rafaela Ferreira | 561671 |
| Victor Sabelli | 566224 |

---

## Links do Projeto

| Recurso | URL |
|---|---|
| Repositório | [github.com/gs-1-tdspo2/gs-disruptive-architectures](https://github.com/gs-1-tdspo2/gs-disruptive-architectures) |
| API Java | [gs-java-advanced.onrender.com/](https://gs-java-advanced.onrender.com/) |
| Site | [https://amanaje.vercel.app/](https://amanaje.vercel.app/) |
| Dashboard (Exemplo do Vídeo) | [https://amanaje.vercel.app/estacoes/9?idRegiao=8](https://amanaje.vercel.app/estacoes/9?idRegiao=8) |
| Vídeo de Apresentação | [EM PROGRESSO](https://youtu.be/seu-video) |

> A API Java está hospedada no Render em plano gratuito. A primeira requisição pode demorar para receber resposta enquanto o servidor inicializa.

---

## Tecnologias Utilizadas

- **Microcontrolador:** ESP32 DevKit C v4
- **Plataforma de simulação:** Wokwi
- **IDE / Build system:** PlatformIO (Visual Studio Code)
- **Linguagem:** C++ (Arduino framework)
- **Protocolo de comunicação:** MQTT sobre Wi-Fi (TCP/IP)
- **Broker MQTT:** mqtt-dashboard.com (porta 1883, sem autenticação)
- **Bibliotecas Arduino:**
  - `WiFi.h` — conectividade Wi-Fi nativa do ESP32
  - `PubSubClient` — cliente MQTT (buffer configurado para 512 bytes)
  - `Adafruit_MPU6050` — driver do acelerômetro e giroscópio
  - `Adafruit_BMP085` — driver do barômetro BMP180
  - `Adafruit_SSD1306` — driver do display OLED
  - `Adafruit_Sensor` — camada de abstração de sensores Adafruit
  - `Wire.h` — comunicação I2C
  - `time.h` — cliente NTP e formatação de timestamp ISO 8601

---

## Arquitetura do Hardware

```
ESP32 DevKit C v4
|
|-- I2C (GPIO 21/22) --> MPU6050 (acelerômetro/giroscópio)
|-- I2C (GPIO 21/22) --> BMP180 (barômetro)
|-- I2C (GPIO 21/22) --> SSD1306 OLED 128x64 (endereço 0x3C)
|-- GPIO 35 (ADC)    --> Potenciômetro (poluição PM2.5 / PM10)
|-- GPIO 5  (OUTPUT) --> HC-SR04 TRIG (sensor ultrassônico)
|-- GPIO 18 (INPUT)  --> HC-SR04 ECHO (sensor ultrassônico)
|-- GPIO 17 (OUTPUT) --> LED Verde  + resistor 220 ohm
|-- GPIO 16 (OUTPUT) --> LED Vermelho + resistor 220 ohm
|-- GPIO 12 (LEDC)   --> Buzzer (canal LEDC 0, 1000 Hz)
```

O MPU6050, BMP180 e o display OLED compartilham o mesmo barramento I2C.

---

## Sensores — O que cada um representa

### MPU6050 — Acelerômetro e Giroscópio (I2C)

Representa um sensor de inclinação instalado em uma encosta ou estrutura de contenção. O ângulo de inclinação é calculado a partir da aceleração linear nos eixos Y e Z em relação a força gravitacional. Na vida real, detectaria o movimento lento ou abrupto do solo antes ou durante um deslizamento.

No Wokwi, o MPU6050 pode ser manipulado clicando no componente e arrastando para simular inclinação.

### BMP180 — Barômetro (I2C)

Mede a pressão atmosférica real. Pressão em queda indica aproximação de frentes frias ou tempestades. O valor é lido diretamente do sensor em Pascals e convertido para hPa.

### Potenciômetro (GPIO 35) — Poluição do Ar (PM2.5 / PM10)

Simula um sensor de partículas em suspensão (como o PMS5003 em uma implementação física real). O valor analógico (0 a 4095) é mapeado para 0 a 300 ug/m3 de PM2.5. O valor de PM10 é derivado multiplicando PM2.5 por 1,5.

### HC-SR04 — Sensor Ultrassônico de Nível de Água (GPIO 5/18)

Representa um sensor instalado no topo de um bueiro ou embaixo de uma ponte, apontado para baixo em direção à superfície da água. A lógica é inversa ao que se poderia supor: rio baixo significa distância grande; rio em enchente significa distância diminuindo. O alerta dispara quando a distância cai abaixo dos limiares definidos pelo backend.

Opera com timeout de 30 ms (~510 cm de alcance máximo). Leituras inválidas são tratadas como dado ausente e não disparam alerta falso.

---

## Saídas — LEDs, Buzzer e OLED

### LED Verde (GPIO 17)

Aceso por padrão ao iniciar o sistema. Apaga quando o backend envia `ledVerde: false` no payload de alerta.

### LED Vermelho (GPIO 16)

Apagado por padrão. Acende quando o backend envia `ledVermelho: true` — indica risco ALTO ou CRITICO.

### Buzzer (GPIO 12)

Silencioso por padrão. Ativado em 1000 Hz quando o backend envia `buzzer: true` — indica risco CRITICO.

Mapeamento de nível de risco para atuadores (definido pelo backend Java):

| nivelRisco | LED Verde | LED Vermelho | Buzzer |
|---|---|---|---|
| BAIXO | true | false | false |
| MODERADO | true | false | false |
| ALTO | false | true | false |
| CRITICO | false | true | true |

### Display OLED SSD1306 128x64 (I2C, 0x3C)

Exibe telemetria em tempo real, atualizada a cada 5 segundos. Quando o backend publica um alerta de nível MODERADO, ALTO ou CRITICO, a tela alterna automaticamente entre a tela de alerta e a tela de telemetria a cada 4 segundos, garantindo que o operador veja sempre os dois contextos. Para nível BAIXO, a tela de alerta não é exibida.

Tela de telemetria normal:

```
   Sistema OK
Agua  25.3cm
Pres 1013.0hPa
PM25   0.0ug/m3
Inc    0.0 graus
10.13.37.2
```

Tela de alerta (exibida ao receber payload do backend):

```
RISCO CRITICO
Tipo: ENCHENTE
Score: 91
Risco critico detectado.
Acionar alerta preventiv
```

O sistema opera normalmente mesmo se o OLED não for detectado na inicialização.

---

## Comunicação MQTT

Broker: `mqtt-dashboard.com`, porta `1883`, sem autenticação.

O buffer do cliente MQTT está configurado em 512 bytes para suportar os payloads de alerta (~250 bytes). O buffer padrão de 128 bytes descartaria as mensagens silenciosamente.

Para monitorar os tópicos em tempo real pelo navegador, acesse o [HiveMQ WebSocket Client](https://www.hivemq.com/demos/websocket-client/), conecte ao broker `mqtt-dashboard.com` na porta `8000` (WebSocket) e assine os tópicos listados abaixo.

---

### Tópico 1 — Telemetria

`app/estacoes/AMANAJE-SP-RP-001/telemetria`

Publicado a cada 5 segundos com todos os valores dos sensores.

```json
{
  "stationCode": "AMANAJE-SP-RP-001",
  "timestamp": "2026-06-03T19:47:38",
  "waterDistanceCm": 399.93,
  "waterLevelPercent": 0,
  "tiltAngle": 0.00,
  "vibration": 0.00,
  "pressureHpa": 1013.27,
  "pm25": 0.00,
  "pm10": 0.00
}
```

---

### Tópico 2 — Status

`app/estacoes/AMANAJE-SP-RP-001/status`

Publicado a cada 30 segundos com informações de saúde do dispositivo.

```json
{
  "stationCode": "AMANAJE-SP-RP-001",
  "mac": "24:0A:C4:00:01:10",
  "uptimeSeg": 3600,
  "rssi": -78,
  "ip": "10.13.37.2",
  "versaoFirmware": "1.4.0"
}
```

---

### Tópico 3 — Alertas (subscrito)

`app/estacoes/AMANAJE-SP-RP-001/alertas`

O dispositivo assina este tópico e aguarda o resultado da análise publicado pelo backend Java. Ao receber o payload, o ESP32 aplica imediatamente o estado dos atuadores e atualiza o OLED.

```json
{
  "stationCode": "AMANAJE-SP-RP-001",
  "nivelRisco": "CRITICO",
  "tipoRiscoPrincipal": "ENCHENTE",
  "score": 91,
  "alerta": true,
  "ledVerde": false,
  "ledVermelho": true,
  "buzzer": true,
  "mensagem": "Risco critico detectado. Acionar alerta preventivo imediatamente.",
  "timestamp": "2026-06-03T18:40:00"
}
```

---

## Como Executar o Projeto

O projeto utiliza **VSCode + PlatformIO + extensão Wokwi** para simulação local. Não é necessário acessar o Wokwi online.

### Pré-requisitos

- [Visual Studio Code](https://code.visualstudio.com/)
- Extensão [PlatformIO IDE](https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide) instalada no VSCode
- Extensão [Wokwi for VS Code](https://marketplace.visualstudio.com/items?itemName=wokwi.wokwi-vscode) instalada no VSCode
- Conta Wokwi com licença ativa (gratuita para uso pessoal — faça login em [wokwi.com](https://wokwi.com) e siga as instruções da extensão para vincular a licença)

### Passos

1. Clone o repositório e abra a pasta raiz no VSCode.
2. Aguarde o PlatformIO inicializar o ambiente e baixar as dependências automaticamente — isso pode levar alguns minutos na primeira vez.
3. Clique em **Build** na barra inferior do PlatformIO (ícone de checkmark) ou use o atalho `Ctrl+Alt+B`. O firmware compilado será gerado em `.pio/build/amanaje/firmware.bin`.
4. Abra o arquivo `amanaje/diagram.json` no VSCode. A extensão Wokwi detectará o arquivo automaticamente.
5. Caso seja solicitado, vincule sua licença Wokwi pela paleta de comandos (`Ctrl+Shift+P` > `Wokwi: Request License`).
6. Pressione `F1` > `Wokwi: Start Simulator` ou clique no botão de play que aparece no canto superior esquerdo do `diagram.json`.
7. O Serial Monitor integrado exibirá o progresso da conexão Wi-Fi, sincronização NTP e conexão MQTT.

---

### Como simular leituras

| Componente | Como interagir | Efeito simulado |
|---|---|---|
| **Potenciômetro (slider)** | Clicar e arrastar o slider para a direita | PM2.5 sobe de 0 a 300 ug/m3; PM10 é derivado (x1,5) |
| **HC-SR04** | Clicar no sensor e reduzir o valor de distância | Simula subida do nível da água em direção ao sensor |
| **MPU6050** | Clicar no componente e arrastar para inclinar | Simula movimento de encosta ou deslizamento |
| **BMP180** | Clicar no sensor e arrastar no sentido horizontal | Simula queda ou aumento de pressão atmosférica e temperatura |

### Como testar o recebimento de alertas

Publique o payload abaixo no tópico `app/estacoes/AMANAJE-SP-RP-001/alertas` usando o [HiveMQ WebSocket Client](https://www.hivemq.com/demos/websocket-client/) ou qualquer outro cliente MQTT (ex: MQTT Explorer):

```json
{
  "stationCode": "AMANAJE-SP-RP-001",
  "nivelRisco": "CRITICO",
  "tipoRiscoPrincipal": "ENCHENTE",
  "score": 91,
  "alerta": true,
  "ledVerde": false,
  "ledVermelho": true,
  "buzzer": true,
  "mensagem": "Risco critico detectado. Acionar alerta preventivo imediatamente.",
  "timestamp": "2026-06-03T18:40:00"
}
```

O Serial Monitor confirmará o recebimento exibindo todos os campos parseados e o estado aplicado aos atuadores. O LED vermelho acenderá, o buzzer será ativado e o OLED alternará entre a tela de alerta e a telemetria a cada 4 segundos.

---

## Estrutura do Repositório

```
gs-disruptive-architectures/
├── amanaje/
|   ├── src/
|   |   └── main.cpp          <- firmware principal do ESP32
|   ├── diagram.json          <- circuito do Wokwi
|   └── wokwi.toml            <- configuração de bibliotecas do Wokwi
├── src/
|   └── main.cpp              <- configurações de Wifi e MQTT
├── .gitignore
├── platformio.ini            <- configuração do PlatformIO
└── README.md
```