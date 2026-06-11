#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <arduinoFFT.h>
#include <ArduinoJson.h>

#define WIFI_SSID        "uaifai-tiradentes"
#define WIFI_PASSWORD    "bemvindoaocesar"
#define MQTT_BROKER      "172.26.69.14"
#define MQTT_PORT        1883
#define MQTT_TOPIC       "bass/pitch"
#define DEVICE_ID        "esp32-bass-01"

#define ADC_PIN          34
#define SAMPLE_RATE      8000
#define SAMPLES          8192
#define SAMPLE_PERIOD_US (1000000 / SAMPLE_RATE)
#define MIN_FREQ         30.0f
#define MAX_FREQ         200.0f
#define NOISE_FLOOR      50.0f
#define SILENCE_THRESHOLD 60

// float em vez de double — economiza 65 KB de RAM
float vReal[SAMPLES];
float vImag[SAMPLES];

ArduinoFFT<float> FFT = ArduinoFFT<float>(vReal, vImag, SAMPLES, SAMPLE_RATE);

WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

// Queue que conecta vTaskAudio → vTaskMQTT (profundidade 5, payload = float)
QueueHandle_t pitchQueue;

// ─── Rede ─────────────────────────────────────────────────────────────────────

void connectWiFi() {
  Serial.printf("Conectando ao Wi-Fi: %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.print(".");
  }
  Serial.printf("\nConectado! IP: %s\n", WiFi.localIP().toString().c_str());
}

void connectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Conectando ao broker MQTT...");
    if (mqttClient.connect(DEVICE_ID)) {
      Serial.println(" OK");
    } else {
      Serial.printf(" Falhou (rc=%d). Tentando em 3s...\n", mqttClient.state());
      vTaskDelay(pdMS_TO_TICKS(3000));
    }
  }
}

// ─── Áudio / FFT ─────────────────────────────────────────────────────────────

// Retorna amplitude pico-a-pico do sinal (gate de silêncio)
int collectSamples() {
  int rawMin = 4095, rawMax = 0;
  uint32_t nextSample = micros();
  for (int i = 0; i < SAMPLES; i++) {
    while (micros() < nextSample);
    nextSample += SAMPLE_PERIOD_US;
    int raw = analogRead(ADC_PIN);
    if (raw < rawMin) rawMin = raw;
    if (raw > rawMax) rawMax = raw;
    vReal[i] = (float)(raw - 2048);
    vImag[i] = 0.0f;
  }
  return rawMax - rawMin;
}

float detectPitch() {
  int amp = collectSamples();
  if (amp < SILENCE_THRESHOLD) {
    Serial.printf("Silêncio (amp=%d)\n", amp);
    return -1.0f;
  }

  FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
  FFT.compute(FFTDirection::Forward);
  FFT.complexToMagnitude();

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

  // Verifica sub-harmônico: se peakBin/2 tem ≥25% da energia do pico,
  // o fundamental real está uma oitava abaixo (corrige E2→E1, A2→A1, etc.)
  int halfBin = peakBin / 2;
  if (halfBin >= binMin && vReal[halfBin] >= maxMag * 0.25f)
    peakBin = halfBin;

  if (peakBin <= binMin || peakBin >= binMax)
    return (float)(peakBin * SAMPLE_RATE / SAMPLES);

  float alpha = vReal[peakBin - 1];
  float beta  = vReal[peakBin];
  float gamma = vReal[peakBin + 1];
  float delta = 0.5f * (alpha - gamma) / (alpha - 2.0f * beta + gamma);

  return (float)((peakBin + delta) * SAMPLE_RATE / SAMPLES);
}

void publishPitch(float freq) {
  StaticJsonDocument<128> doc;
  doc["freq"]      = freq;
  doc["device_id"] = DEVICE_ID;
  doc["ts"]        = millis();

  char buffer[128];
  serializeJson(doc, buffer);
  mqttClient.publish(MQTT_TOPIC, buffer);
  Serial.printf("Publicado: %s\n", buffer);
}

// ─── Tasks FreeRTOS ───────────────────────────────────────────────────────────

// Core 1 — coleta amostras e detecta pitch; envia resultado pela queue
void vTaskAudio(void *pvParameters) {
  for (;;) {
    float freq = detectPitch();
    if (freq > 0) {
      // Descarta se a queue estiver cheia (não bloqueia a coleta)
      xQueueSend(pitchQueue, &freq, 0);
    }
    // collectSamples já consome ~1s; nenhum delay adicional necessário
  }
}

// Core 0 — mantém WiFi/MQTT e publica cada frequência recebida da queue
void vTaskMQTT(void *pvParameters) {
  connectWiFi();
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  connectMQTT();

  float freq;
  for (;;) {
    if (!mqttClient.connected()) connectMQTT();
    mqttClient.loop();

    // Aguarda até 100 ms por um novo pitch; enquanto isso o loop MQTT continua
    if (xQueueReceive(pitchQueue, &freq, pdMS_TO_TICKS(100)) == pdTRUE) {
      publishPitch(freq);
    }
  }
}

// ─── Setup ────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  pitchQueue = xQueueCreate(5, sizeof(float));

  // Prioridade 2 > 1 garante que o áudio não seja interrompido pelo MQTT
  xTaskCreatePinnedToCore(vTaskMQTT,  "MQTT",  4096, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(vTaskAudio, "Audio", 4096, NULL, 2, NULL, 1);
}

// loop() não é usada — as tasks FreeRTOS assumem o controle
void loop() {
  vTaskDelete(NULL);
}
