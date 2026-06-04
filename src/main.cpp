#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <arduinoFFT.h>
#include <ArduinoJson.h>

// ─── Configurações Wi-Fi e MQTT ────────────────────────────────────────────
#define WIFI_SSID     "uaifai-tiradentes"
#define WIFI_PASSWORD "bemvindoaocesar"
#define MQTT_BROKER   "172.26.68.211"
#define MQTT_PORT     1883
#define MQTT_TOPIC    "bass/pitch"
#define DEVICE_ID     "esp32-bass-01"

// ─── Configurações de ADC e FFT ────────────────────────────────────────────
#define ADC_PIN          34
#define SAMPLE_RATE      8000
#define SAMPLES          8192
#define SAMPLE_PERIOD_US (1000000 / SAMPLE_RATE)

// ─── Limiares ──────────────────────────────────────────────────────────────
#define MIN_FREQ    30.0f
#define MAX_FREQ    1000.0f
#define NOISE_FLOOR 50.0f

// ─── Tamanho do histórico para benchmark ───────────────────────────────────
// N = 2000: evidencia diferença de complexidade sem estourar RAM
// 2 buffers × 2000 × 4 bytes = 16 KB — seguro para o heap do ESP32
#define HISTORY_SIZE 2000

// ─── Buffers FFT alocados no heap para não estourar o BSS (64 KB) ──────────
float* vReal = nullptr;
float* vImag = nullptr;
ArduinoFFT<float>* FFT = nullptr;

WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

// ═══════════════════════════════════════════════════════════════════════════
// VERTENTE 1 — Array com deslocamento linear O(n)
// A cada nova leitura com buffer cheio, todos os N elementos são deslocados.
// Custo cresce linearmente com HISTORY_SIZE.
// ═══════════════════════════════════════════════════════════════════════════
float naive_buf[HISTORY_SIZE];
int   naive_count = 0;

unsigned long naive_push(float val) {
  unsigned long t0 = micros();
  if (naive_count < HISTORY_SIZE) {
    naive_buf[naive_count++] = val;
  } else {
    for (int i = 0; i < HISTORY_SIZE - 1; i++)
      naive_buf[i] = naive_buf[i + 1];   // O(n) — desloca tudo
    naive_buf[HISTORY_SIZE - 1] = val;
  }
  return micros() - t0;
}

// ═══════════════════════════════════════════════════════════════════════════
// VERTENTE 2 — Ring Buffer (Buffer Circular) O(1)
// Usa índices head/count para inserção em tempo constante.
// Nenhum elemento é movido na memória.
// ═══════════════════════════════════════════════════════════════════════════
struct RingBuffer {
  float data[HISTORY_SIZE];
  int   head  = 0;
  int   count = 0;

  void push(float val) {
    data[head] = val;
    head = (head + 1) % HISTORY_SIZE;   // avanço circular — O(1)
    if (count < HISTORY_SIZE) count++;
  }

  float get(int i) const {
    int idx = (head - count + i + HISTORY_SIZE) % HISTORY_SIZE;
    return data[idx];
  }
};

RingBuffer ring;

unsigned long ring_push(float val) {
  unsigned long t0 = micros();
  ring.push(val);
  return micros() - t0;
}

// ─── Coleta de amostras com timer preciso ──────────────────────────────────
void collectSamples() {
  // debug: imprime min/max do sinal bruto
  int minVal = 4095, maxVal = 0;
  uint32_t nextSample = micros();
  for (int i = 0; i < SAMPLES; i++) {
    while (micros() < nextSample);
    nextSample += SAMPLE_PERIOD_US;
    int raw  = analogRead(ADC_PIN);
    minVal = min(minVal, raw);
    maxVal = max(maxVal, raw);
    vReal[i] = (float)(raw - 2048);
    vImag[i] = 0.0f;
  }
  Serial.printf("[ADC] min=%d max=%d amplitude=%d\n", minVal, maxVal, maxVal - minVal);
}


// ─── Detecção de pitch via FFT ─────────────────────────────────────────────
float detectPitch() {
  collectSamples();
  FFT->windowing(FFTWindow::Hamming, FFTDirection::Forward);
  FFT->compute(FFTDirection::Forward);
  FFT->complexToMagnitude();

  float maxMag  = NOISE_FLOOR;
  int   peakBin = -1;
  int   binMin  = (int)(MIN_FREQ * SAMPLES / SAMPLE_RATE);
  int   binMax  = (int)(MAX_FREQ * SAMPLES / SAMPLE_RATE);

  for (int i = binMin; i <= binMax; i++) {
    if (vReal[i] > maxMag) {
      maxMag  = vReal[i];
      peakBin = i;
    }
  }

  if (peakBin < 0) return -1.0f;

  // Proteção contra acesso fora dos limites na interpolação parabólica
  if (peakBin <= binMin || peakBin >= binMax)
    return (float)(peakBin * SAMPLE_RATE / SAMPLES);

  float alpha = vReal[peakBin - 1];
  float beta  = vReal[peakBin];
  float gamma = vReal[peakBin + 1];
  float delta = 0.5f * (alpha - gamma) / (alpha - 2.0f * beta + gamma);

  return (float)((peakBin + delta) * SAMPLE_RATE / SAMPLES);
}

// ─── Publica frequência + métricas de benchmark no MQTT ───────────────────
void publishData(float freq,
                 unsigned long lat_naive, unsigned long lat_ring,
                 uint32_t hb1, uint32_t ha1,
                 uint32_t hb2, uint32_t ha2) {

  // 384 bytes necessários para o documento com objetos aninhados v1/v2
  StaticJsonDocument<384> doc;
  doc["freq"]      = freq;
  doc["device_id"] = DEVICE_ID;
  doc["ts"]        = millis();

  JsonObject v1 = doc.createNestedObject("v1_naive");
  v1["lat_us"]      = lat_naive;
  v1["heap_before"] = hb1;
  v1["heap_after"]  = ha1;
  v1["n"]           = naive_count;

  JsonObject v2 = doc.createNestedObject("v2_ring");
  v2["lat_us"]      = lat_ring;
  v2["heap_before"] = hb2;
  v2["heap_after"]  = ha2;
  v2["n"]           = ring.count;

  char buffer[384];
  serializeJson(doc, buffer);
  mqttClient.publish(MQTT_TOPIC, buffer);

  Serial.printf(
    "[PITCH] %.2f Hz | V1: %lu µs (heap Δ%d) | V2: %lu µs (heap Δ%d)\n",
    freq,
    lat_naive, (int)ha1 - (int)hb1,
    lat_ring,  (int)ha2 - (int)hb2
  );
}

// ─── Wi-Fi e MQTT ──────────────────────────────────────────────────────────
void connectWiFi() {
  Serial.printf("Conectando ao Wi-Fi: %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.printf("\nConectado! IP: %s\n", WiFi.localIP().toString().c_str());
}

void connectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Conectando ao broker MQTT...");
    if (mqttClient.connect(DEVICE_ID)) {
      Serial.println(" OK");
    } else {
      Serial.printf(" Falhou (rc=%d). Tentando em 3s...\n", mqttClient.state());
      delay(3000);
    }
  }
}

// ──────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  vReal = new float[SAMPLES]();
  vImag = new float[SAMPLES]();
  FFT   = new ArduinoFFT<float>(vReal, vImag, SAMPLES, SAMPLE_RATE);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  connectWiFi();
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setBufferSize(512);   // payload com v1_naive+v2_ring chega a ~200 bytes
  connectMQTT();
  Serial.printf("Sistema pronto. HISTORY_SIZE = %d\n", HISTORY_SIZE);
}

void loop() {
  if (!mqttClient.connected()) connectMQTT();
  mqttClient.loop();

  float pitch = detectPitch();

  if (pitch > 0) {
    uint32_t hb1 = ESP.getFreeHeap();
    unsigned long lat1 = naive_push(pitch);
    uint32_t ha1 = ESP.getFreeHeap();

    uint32_t hb2 = ESP.getFreeHeap();
    unsigned long lat2 = ring_push(pitch);
    uint32_t ha2 = ESP.getFreeHeap();

    publishData(pitch, lat1, lat2, hb1, ha1, hb2, ha2);
  } else {
    Serial.println("Sem sinal detectado (ruído abaixo do limiar).");
  }
  // sem delay() — collectSamples() já consome ~1.024 s (8192/8000 Hz)
}
