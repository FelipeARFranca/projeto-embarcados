# 🎸 Bass Tuner — Sistema IoT com ESP32

Identificador de notas musicais para contrabaixo usando ESP32, MQTT, FastAPI e WebSocket.

## Arquitetura

```
Contrabaixo → (P10 → divisor de tensão) → ESP32
                                             │
                                        ADC + FFT
                                       (detecção de pitch)
                                             │
                                        MQTT publish
                                       (bass/pitch)
                                             │
                                      Broker Mosquitto
                                             │
                                      FastAPI Backend
                                    (mapeamento freq→nota)
                                             │
                                        WebSocket
                                             │
                                      Dashboard HTML
                                    (visualização em tempo real)
```

## Estrutura do Repositório

```
├── README.md
├── /docs                    # Relatório PDF (MNR/ABNT2) + imagens
├── /applications
│   ├── /backend             # FastAPI + MQTT + WebSocket
│   │   ├── main.py
│   │   └── requirements.txt
│   └── /frontend            # Dashboard HTML/CSS/JS
│       └── index.html
├── /esp32-esp8266           # Firmware (FreeRTOS via PlatformIO)
│   ├── main.cpp
│   └── platformio.ini
└── /schematics              # Diagramas do circuito
```

## Como Rodar

### 1. Broker MQTT
```bash
# Instalar Mosquitto
sudo apt install mosquitto mosquitto-clients

# Iniciar
sudo systemctl start mosquitto
```

### 2. Backend
```bash
cd applications/backend
pip install -r requirements.txt
uvicorn main:app --reload --port 8000
```

### 3. Frontend
Abra `applications/frontend/index.html` no navegador.
Ou sirva com: `python -m http.server 3000`

### 4. Firmware ESP32
```bash
cd esp32-esp8266
# Edite main.cpp com suas credenciais Wi-Fi e IP do broker
pio run --target upload
```

## Circuito

Entrada de áudio via cabo P10 (captador passivo do contrabaixo):
- Capacitor 10µF: acoplamento DC
- 2x Resistor 100kΩ: divisor de tensão para centralizar sinal em 1.65V
- Resistor 10kΩ: proteção do ADC
- GPIO34 do ESP32 (ADC1, 12 bits)

## Configurações de Detecção
- Taxa de amostragem: 8000 Hz
- Tamanho da janela FFT: 8192 amostras
- Resolução de frequência: ~0.97 Hz
- Janela: Hamming (reduz vazamento espectral)
- Faixa válida: 30–400 Hz

## Tópicos MQTT

| Tópico       | Payload                                      |
|-------------|----------------------------------------------|
| `bass/pitch` | `{"freq": 82.5, "device_id": "esp32-bass-01", "ts": 12345}` |

## API REST

| Endpoint          | Descrição                        |
|-------------------|----------------------------------|
| `GET /`           | Status da API                    |
| `GET /pitch/latest` | Último pitch detectado         |
| `GET /pitch/history` | Histórico das últimas 60 leituras |
| `GET /notes`      | Tabela de notas de referência    |
| `WS  /ws`         | WebSocket — stream em tempo real |
