# GS IoT — Estacao de Monitoramento Ambiental Urbano

Projeto desenvolvido para a Global Solution 2026 da FIAP, disciplina Disruptive Architectures: IoT, IoB & Generative IA.

O sistema implementa uma estacao de monitoramento ambiental urbano utilizando um microcontrolador ESP32, com foco na prevencao e antecipacao de quatro categorias de desastres naturais comuns em regioes metropolitanas brasileiras: enchentes, deslizamentos de encosta, tempestades e poluicao do ar.

---

## Integrantes

| Nome | RM |
|---|---|
| Gustavo Crevelari | 561408 |
| Lucca Gomes | 561996 |
| Rafaela Ferreira | 561671 |
| Victor Sabelli | 566224 |

---

## Tecnologias Utilizadas

- **Microcontrolador:** ESP32 DevKit C v4
- **Plataforma de simulacao:** Wokwi
- **IDE / Build system:** PlatformIO (Visual Studio Code)
- **Linguagem:** C++ (Arduino framework)
- **Protocolo de comunicacao:** MQTT sobre Wi-Fi (TCP/IP)
- **Broker MQTT:** HiveMQ Public Broker (`broker.hivemq.com`, porta 1883)
- **Bibliotecas Arduino:**
  - `WiFi.h` — conectividade Wi-Fi nativa do ESP32
  - `PubSubClient` — cliente MQTT
  - `Adafruit_MPU6050` — driver do acelerometro/giroscopio
  - `Adafruit_SSD1306` — driver do display OLED
  - `Adafruit_Sensor` — camada de abstracao de sensores Adafruit
  - `Wire.h` — comunicacao I2C

---

## Arquitetura do Hardware

### Diagrama de componentes

```
ESP32 DevKit C v4
|
|-- I2C (GPIO 21/22) --> MPU6050 (acelerometro)
|-- I2C (GPIO 21/22) --> SSD1306 OLED 128x64
|-- GPIO 34 (ADC)    --> Potenciometro 1 (pressao atmosferica)
|-- GPIO 35 (ADC)    --> Potenciometro 2 (poluicao PM2.5)
|-- GPIO 5  (OUTPUT) --> HC-SR04 TRIG (ultrassonico)
|-- GPIO 18 (INPUT)  --> HC-SR04 ECHO (ultrassonico)
|-- GPIO 17 (OUTPUT) --> LED Verde  + resistor 220 ohm
|-- GPIO 16 (OUTPUT) --> LED Vermelho + resistor 220 ohm
```

O MPU6050 e o display OLED compartilham o mesmo barramento I2C. O display opera no endereco padrao `0x3C`.

---

## Sensores — O que cada um representa

### MPU6050 — Acelerometro e Giroscopio (GPIO 21/22, I2C)

Representa um sensor de inclinacao instalado em uma encosta ou estrutura de contencao.

O angulo de inclinacao e calculado a partir da aceleracao linear nos eixos X e Y em relacao a forca gravitacional (9,81 m/s2). Na vida real, um sensor desse tipo detectaria o movimento lento ou abrupto do solo antes ou durante um deslizamento.

No Wokwi, o MPU6050 pode ser manipulado clicando no componente e arrastando para simular inclinacao.

Faixas de risco:

| Inclinacao (graus) | Severidade |
|---|---|
| Abaixo de 15 | NORMAL |
| 15 a 25 | ATENCAO |
| 25 a 35 | ALERTA |
| Acima de 35 | CRITICO |

---

### Potenciometro 1 (GPIO 34) — Pressao Atmosferica

Simula um barometro digital (como o BMP280 em uma implementacao fisica real).

O valor analogico lido (0 a 4095) e mapeado de forma **invertida** para o range de 1030 a 980 hPa. O range e invertido porque pressao alta (pot zerado) indica tempo bom, e pressao baixa (pot no maximo) indica tempestade se aproximando — o que e fisicamente correto.

Os thresholds sao baseados na escala Saffir-Simpson da NOAA e em dados meteorologicos reais. A pressao media padrao ao nivel do mar e 1013,25 hPa (NOAA).

Mapeamento do potenciometro:

| Posicao do pot | Pressao simulada | Situacao real |
|---|---|---|
| Zero (repouso) | 1030 hPa | Alta pressao, tempo estavel |
| ~40% | ~1013 hPa | Pressao padrao ao nivel do mar |
| ~60% | ~1005 hPa | Baixa pressao moderada |
| ~85% | ~992 hPa | Depressao tropical |
| Maximo | 980 hPa | Limiar de furacao Categoria 1 |

Faixas de risco:

| Pressao (hPa) | Severidade |
|---|---|
| Acima de 1005 | NORMAL |
| 992 a 1005 | ATENCAO |
| 980 a 992 | ALERTA |
| 980 ou abaixo | CRITICO |

---

### Potenciometro 2 (GPIO 35) — Poluicao do Ar (PM2.5)

Simula um sensor de particulas em suspensao (como o PMS5003 em uma implementacao fisica real).

O valor analogico e mapeado para o range de 0 a 300 microgramas por metro cubico (ug/m3), que e a escala real de concentracao de PM2.5. Os thresholds sao baseados nas diretrizes da OMS e nos padroes do CONAMA.

Faixas de risco:

| PM2.5 (ug/m3) | Severidade |
|---|---|
| Abaixo de 50 | NORMAL |
| 50 a 100 | ATENCAO |
| 100 a 200 | ALERTA |
| Acima de 200 | CRITICO |

---

### HC-SR04 — Sensor Ultrassonico (GPIO 5/18)

Representa um sensor de nivel de agua instalado no topo de um bueiro, galeria pluvial ou embaixo de uma ponte, apontado para baixo em direcao a superficie da agua.

A logica e **inversa** ao que se poderia supor intuitivamente:

- Rio baixo (situacao normal): a agua esta longe do sensor, a distancia medida e **grande**.
- Rio em enchente: a agua sobe em direcao ao sensor, a distancia medida **diminui**.

Portanto, o alerta dispara quando a distancia cai abaixo dos thresholds, nao quando aumenta.

O sensor opera com um timeout de 30 ms (equivalente a aproximadamente 510 cm de alcance maximo). Leituras invalidas (sem eco) sao tratadas como dado ausente e nao disparam alerta falso.

Faixas de risco:

| Distancia ate a agua (cm) | Severidade |
|---|---|
| Acima de 120 | NORMAL |
| 80 a 120 | ATENCAO |
| 50 a 80 | ALERTA |
| Abaixo de 50 | CRITICO |

---

### LED Verde (GPIO 17) — Saida 1

Aceso continuamente enquanto todos os sensores estiverem dentro da faixa NORMAL. Apaga imediatamente ao primeiro alerta de qualquer sensor.

### LED Vermelho (GPIO 16) — Saida 2

Apagado em condicao normal. Acende imediatamente quando qualquer sensor sair da faixa NORMAL (independente do nivel de severidade).

### Display OLED SSD1306 128x64 (I2C, 0x3C) — Interface Local

Exibe o estado completo do sistema em tempo real, atualizado a cada 5 segundos. Layout:

```
!!! CRITICO !!!          <- status geral (pior severidade ativa)
Agua  25.3cm [!!!]       <- distancia + badge de severidade
Pres 1013.0hP            <- pressao (sem badge = NORMAL)
PM25  85.2ug [ ! ]       <- PM2.5 com badge ALERTA
Incl   3.1 gr            <- inclinacao (sem badge = NORMAL)
ENC:CRT PM:ALT           <- resumo dos alertas ativos
```

Badges de severidade:

| Badge | Significado |
|---|---|
| (sem badge) | NORMAL |
| `[ ? ]` | ATENCAO |
| `[ ! ]` | ALERTA |
| `[!!!]` | CRITICO |

O sistema opera normalmente mesmo se o OLED nao for detectado na inicializacao.

---

## Comunicacao MQTT

Broker: `broker.hivemq.com`, porta `1883`, sem autenticacao (broker publico).

### Topico 1 — Telemetria Completa

`fiap/global_solution/estacao1/telemetria`

Publicado a cada 5 segundos com todos os valores dos sensores e suas severidades.

Exemplo de payload:

```json
{
  "codigoEstacao": "APP-ST-001",
  "distanciaAguaCm": 95.40,
  "distValida": true,
  "sevEnchente": "ATENCAO",
  "pressaoHpa": 1010.00,
  "sevPressao": "NORMAL",
  "poluicaoPm25": 120.00,
  "sevPM25": "ALERTA",
  "inclinacaoX": 8.20,
  "inclinacaoY": 1.10,
  "sevInclinacao": "NORMAL",
  "alertaGeral": true
}
```

---

### Topico 2 — Alertas Individuais

`fiap/global_solution/estacao1/alertas`

Publicado a cada ciclo de 5 segundos **apenas para os sensores com severidade acima de NORMAL**. Cada sensor gera sua propria mensagem independente — se tres sensores estiverem em alerta simultaneamente, tres mensagens sao publicadas neste topico.

Exemplo de payload:

```json
{
  "codigoEstacao": "APP-ST-001",
  "tipo": "POLUICAO",
  "valor": 120.00,
  "unidade": "ug/m3",
  "severidade": "ALERTA"
}
```

Tipos possiveis: `ENCHENTE`, `TEMPESTADE`, `POLUICAO`, `DESLIZAMENTO`.

Severidades possiveis: `ATENCAO`, `ALERTA`, `CRITICO`.

---

### Topico 3 — Status / Heartbeat

`fiap/global_solution/estacao1/status`

Publicado a cada 30 segundos com informacoes de saude do dispositivo.

Exemplo de payload:

```json
{
  "codigoEstacao": "APP-ST-001",
  "uptimeSeg": 3600,
  "rssi": -62,
  "ip": "10.13.37.2",
  "oled": true,
  "firmware": "1.4.0"
}
```

---

### Topico 4 — Comando (subscrito)

`fiap/global_solution/estacao1/comando`

O dispositivo assina este topico e aguarda comandos enviados pelo backend. Atualmente suporta o comando `reset`, que reinicia o ESP32 remotamente.

---

## Como Executar no Wokwi

1. Acesse [wokwi.com](https://wokwi.com) e crie um novo projeto ESP32.
2. Substitua o conteudo do `diagram.json` pelo arquivo `diagram.json` deste repositorio.
3. Substitua o conteudo do `main.cpp` (dentro de `src/`) pelo arquivo `src/main.cpp` deste repositorio.
4. Certifique-se de que as seguintes bibliotecas estao declaradas no `libraries.txt` ou `wokwi.toml`:
   - `Adafruit MPU6050`
   - `Adafruit SSD1306`
   - `Adafruit BusIO`
   - `PubSubClient`
5. Inicie a simulacao. O Serial Monitor exibira o progresso da conexao Wi-Fi e MQTT.
6. Para monitorar os dados em tempo real pelo navegador, acesse o cliente WebSocket publico do HiveMQ em `http://www.hivemq.com/demos/websocket-client/` e assine os topicos listados acima.

### Como simular alertas

| Sensor | Acao no Wokwi | Efeito |
|---|---|---|
| Pressao atmosferica | Girar o potenciometro 1 para a direita | Pressao cai de 1030 para 980 hPa |
| Poluicao PM2.5 | Girar o potenciometro 2 para a direita | PM2.5 sobe de 0 para 300 ug/m3 |
| Nivel de agua | Diminuir a distancia no HC-SR04 | Simula subida do rio |
| Deslizamento | Clicar e inclinar o MPU6050 | Simula movimento de encosta |

---

## Estrutura do Repositorio

```
gs-iot/
|-- src/
|   `-- main.cpp          <- codigo-fonte principal do firmware
|-- diagram.json          <- circuito do Wokwi
|-- platformio.ini        <- configuracao do PlatformIO
|-- .gitignore
|-- LICENSE
`-- README.md
```

---

## Licenca

Consulte o arquivo `LICENSE` neste repositorio.
