#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <arduinoFFT.h>
#include <ArduinoJson.h>

// ─── Configurações Wi-Fi e MQTT ─────────────────────────────────────────────
#define WIFI_SSID     "SEU_SSID"
#define WIFI_PASSWORD "SUA_SENHA"
#define MQTT_BROKER   "192.168.x.x"   // IP do broker Mosquitto
#define MQTT_PORT     1883
#define MQTT_TOPIC    "bass/pitch"
#define DEVICE_ID     "esp32-bass-01"

// ─── Configurações de ADC e FFT ─────────────────────────────────────────────
#define ADC_PIN       34
#define SAMPLE_RATE   8000             // 8 kHz — suficiente para contrabaixo (40–400 Hz)
#define SAMPLES       8192             // N amostras → resolução de ~0.97 Hz
#define SAMPLE_PERIOD_US (1000000 / SAMPLE_RATE)

// ─── Limiares ────────────────────────────────────────────────────────────────
#define MIN_FREQ      30.0f            // Mi grave do contrabaixo ~41 Hz
#define MAX_FREQ      400.0f           // Harmônicos superiores
#define NOISE_FLOOR   200.0f           // Magnitude mínima para considerar sinal válido

// ─── Buffers FFT ─────────────────────────────────────────────────────────────
double vReal[SAMPLES];
double vImag[SAMPLES];

ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, SAMPLES, SAMPLE_RATE);

WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

// ─────────────────────────────────────────────────────────────────────────────
void connectWiFi() {
  Serial.printf("Conectando ao Wi-Fi: %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
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
      delay(3000);
    }
  }
}

// ─── Coleta de amostras com timer preciso ─────────────────────────────────────
void collectSamples() {
  uint32_t nextSample = micros();
  for (int i = 0; i < SAMPLES; i++) {
    // Aguarda o momento exato da próxima amostra
    while (micros() < nextSample);
    nextSample += SAMPLE_PERIOD_US;

    // Lê o ADC (12 bits: 0–4095) e centraliza em torno de zero
    int raw = analogRead(ADC_PIN);
    vReal[i] = (double)(raw - 2048);   // Remove o offset DC (1.65V → 2048)
    vImag[i] = 0.0;
  }
}

// ─── Detecção de pitch via FFT ─────────────────────────────────────────────
float detectPitch() {
  collectSamples();

  // Janela de Hamming reduz vazamento espectral
  FFT.windowing(FFTWindow::Hamming, FFTDirection::Forward);
  FFT.compute(FFTDirection::Forward);
  FFT.complexToMagnitude();

  // Busca o pico de maior magnitude dentro da faixa do contrabaixo
  double maxMag   = NOISE_FLOOR;
  int    peakBin  = -1;
  int    binMin   = (int)(MIN_FREQ * SAMPLES / SAMPLE_RATE);
  int    binMax   = (int)(MAX_FREQ * SAMPLES / SAMPLE_RATE);

  for (int i = binMin; i <= binMax; i++) {
    if (vReal[i] > maxMag) {
      maxMag  = vReal[i];
      peakBin = i;
    }
  }

  if (peakBin < 0) return -1.0f;   // Sem sinal válido detectado

  // Interpolação parabólica para maior precisão (sub-bin)
  double alpha = vReal[peakBin - 1];
  double beta  = vReal[peakBin];
  double gamma = vReal[peakBin + 1];
  double delta = 0.5 * (alpha - gamma) / (alpha - 2.0 * beta + gamma);

  float freq = (float)((peakBin + delta) * SAMPLE_RATE / SAMPLES);
  return freq;
}

// ─── Publica resultado no MQTT ─────────────────────────────────────────────
void publishPitch(float freq) {
  StaticJsonDocument<128> doc;
  doc["freq"]      = serialized(String(freq, 2));
  doc["device_id"] = DEVICE_ID;
  doc["ts"]        = millis();

  char buffer[128];
  serializeJson(doc, buffer);

  mqttClient.publish(MQTT_TOPIC, buffer);
  Serial.printf("Publicado: %s\n", buffer);
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  analogReadResolution(12);           // ADC de 12 bits (0–4095)
  analogSetAttenuation(ADC_11db);     // Faixa 0–3.3V

  connectWiFi();
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  connectMQTT();

  Serial.println("Sistema pronto. Iniciando detecção de pitch...");
}

void loop() {
  if (!mqttClient.connected()) connectMQTT();
  mqttClient.loop();

  float pitch = detectPitch();

  if (pitch > 0) {
    publishPitch(pitch);
  } else {
    Serial.println("Sem sinal detectado (ruído abaixo do limiar).");
  }

  // Pequena pausa para não saturar o broker (ajuste conforme necessidade)
  delay(200);
}
