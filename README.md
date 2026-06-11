# Bass Tuner — Sistema IoT com ESP32

Identificador de notas musicais para contrabaixo em tempo real. O firmware no ESP32 captura o áudio via ADC, detecta a frequência com FFT e publica via MQTT. O backend em Python mapeia a frequência para a nota mais próxima e transmite para um dashboard web via WebSocket.

## Arquitetura

```
Contrabaixo
    │  (cabo P10)
    ▼
Circuito de condicionamento
(capacitor + divisor resistivo)
    │
    ▼
ESP32 — GPIO34 (ADC 12 bits)
    │
    ├─ Core 1: vTaskAudio
    │    coleta 8192 amostras → FFT → detecta pitch
    │
    └─ Core 0: vTaskMQTT
         publica em bass/pitch via MQTT
              │
              ▼
       Broker Mosquitto
              │
              ▼
      FastAPI Backend (main.py)
       freq → nota + cents
              │
              ▼
        WebSocket /ws
              │
              ▼
      Dashboard (index.html)
       visualização em tempo real
```

## Estrutura do Repositório

```
projeto-embarcados/
├── Dockerfile                 # Imagem do backend Python
├── docker-compose.yml         # Orquestra backend + Mosquitto
├── requirements.txt           # Dependências Python
├── platformio.ini             # Configuração PlatformIO (ESP32)
├── mosquitto/
│   └── mosquitto.conf         # Broker com acesso externo habilitado
└── src/
    ├── main.cpp               # Firmware ESP32 (FreeRTOS + FFT + MQTT)
    ├── main.py                # Backend FastAPI + WebSocket
    ├── index.html             # Dashboard web
    └── simulate_mqtt.py       # Simulador (testa sem ESP32)
```

## Pré-requisitos

- [Docker Desktop](https://www.docker.com/products/docker-desktop/) (backend + broker)
- [PlatformIO](https://platformio.org/) (para compilar e flashar o firmware)
- Cabo USB para o ESP32

---

## Como Rodar

### 1. Backend + Broker (Docker)

```bash
# Na raiz do projeto
docker compose up --build
```

O dashboard fica disponível em **http://localhost:8000**

Para parar:
```bash
docker compose down
```

---

### 2. Firmware ESP32

**2.1 Configure as credenciais** em `src/main.cpp`:

```cpp
#define WIFI_SSID     "sua-rede"
#define WIFI_PASSWORD "sua-senha"
#define MQTT_BROKER   "IP_DA_SUA_MAQUINA"   // ex: 192.168.1.10
#define MQTT_PORT     1883
```

> O IP do broker deve ser o IP WiFi da máquina que está rodando o Docker — não o IP do WSL.
> Para descobrir: `ipconfig` no Windows → adaptador Wi-Fi.

**2.2 Compile e faça o upload:**

```bash
pio run --target upload
```

**2.3 Monitore o Serial:**

```bash
pio device monitor --baud 115200
```

Saída esperada:
```
Conectando ao Wi-Fi: sua-rede....
Conectado! IP: 192.168.1.20
Conectando ao broker MQTT... OK
Sistema pronto.
Publicado: {"freq":41.23,"device_id":"esp32-bass-01","ts":12350}
```

---

### 3. Testar sem ESP32 (simulador)

Com o Docker rodando, em outro terminal:

```bash
cd src
python3 simulate_mqtt.py
```

Escolha um dos modos:
```
[1] Linha de baixo aleatória
[2] Varredura cromática (E1 → G2)
[3] Nota única com desvio progressivo (afinação)
[4] Cordas soltas em loop (E1 → A1 → D2 → G2)
```

---

## Circuito de Condicionamento de Sinal

O sinal do contrabaixo é AC e precisa ser centralizado em 1.65V antes de entrar no ADC do ESP32.

```
3.3V
 │
[10kΩ]
 │
 ├──────────────────► GPIO34 (ESP32)
 │          │
[10kΩ]   [10µF] ◄─── Tip do P10 (sinal)
 │          │
GND      Sleeve do P10 (GND)
```

| Componente | Valor | Função |
|---|---|---|
| C1 | 10 µF eletrolítico | Bloqueia DC do captador, deixa passar o sinal AC |
| R1, R2 | 10 kΩ | Divisor resistivo: polariza o sinal em 1.65V |
| GPIO | 34 (ADC1) | Entrada analógica 12 bits, suporta até 3.3V |

> **Atenção:** use resistores de **10kΩ** (não 100kΩ). Alta impedância torna o circuito sensível a interferências e o sinal do instrumento não consegue acionar o ADC.

---

## Firmware — Detalhes Técnicos

### FreeRTOS

O firmware utiliza dois recursos principais do FreeRTOS:

**Tasks em dual-core:**

| Task | Core | Prioridade | Responsabilidade |
|---|---|---|---|
| `vTaskAudio` | 1 | 2 | Coleta amostras + FFT + detecção de pitch |
| `vTaskMQTT` | 0 | 1 | WiFi + MQTT loop + publicação |

**Queue (`pitchQueue`):**
- Profundidade: 5 itens
- Payload: `float` (frequência em Hz)
- `vTaskAudio` produz → `vTaskMQTT` consome
- `xQueueSend(..., 0)`: não bloqueia a coleta se a fila estiver cheia
- `xQueueReceive(..., 100ms)`: mantém o `mqttClient.loop()` ativo em silêncio

### Processamento de Sinal

| Parâmetro | Valor |
|---|---|
| Taxa de amostragem | 8000 Hz |
| Janela FFT | 8192 amostras (~1s) |
| Resolução de frequência | ~0.98 Hz/bin |
| Janela espectral | Hamming |
| Faixa de detecção | 30 – 200 Hz |
| Gate de silêncio | amplitude ADC < 60 → descarta |
| NOISE_FLOOR | 50 |

**Correção de sub-harmônico:** após encontrar o pico da FFT, o firmware verifica se `peakBin / 2` possui ≥ 25% da energia do pico. Se sim, usa o sub-harmônico como fundamental — isso corrige o caso em que a FFT detecta o 2º harmônico em vez da fundamental (ex: E2 quando toca E1).

**Interpolação parabólica:** refina a frequência entre bins para aumentar a precisão além da resolução nominal de 0.98 Hz.

---

## Backend — API

| Endpoint | Método | Descrição |
|---|---|---|
| `/` | GET | Serve o dashboard HTML |
| `/status` | GET | Status da API |
| `/pitch/latest` | GET | Último pitch detectado |
| `/pitch/history` | GET | Histórico das últimas 60 leituras |
| `/notes` | GET | Tabela de notas de referência |
| `/ws` | WebSocket | Stream em tempo real |

### Payload MQTT (`bass/pitch`)

```json
{
  "freq": 41.23,
  "device_id": "esp32-bass-01",
  "ts": 123456
}
```

### Payload WebSocket

```json
{
  "note": "E1",
  "ref_freq": 41.20,
  "detected_freq": 41.23,
  "cents": +0.1,
  "tuning_status": "afinado",
  "device_id": "esp32-bass-01",
  "ts": 1718123456.789
}
```

`tuning_status` pode ser `"afinado"` (|cents| ≤ 5), `"agudo"` ou `"grave"`.

---

## Notas de Referência

Afinação padrão EADG para contrabaixo de 4 cordas:

| Corda | Nota | Frequência |
|---|---|---|
| 4ª | E1 (Mi) | 41.20 Hz |
| 3ª | A1 (Lá) | 55.00 Hz |
| 2ª | D2 (Ré) | 73.42 Hz |
| 1ª | G2 (Sol) | 98.00 Hz |

A tabela completa cobre E1 até F3 (com trastes até a 5ª casa de cada corda).

---

## Dependências

**Firmware (PlatformIO):**
- `knolleary/PubSubClient @ ^2.8`
- `kosme/arduinoFFT @ ^2.0`
- `bblanchon/ArduinoJson @ ^6.21`

**Backend (Python):**
- `fastapi==0.136.3`
- `uvicorn[standard]`
- `paho_mqtt==2.1.0`
