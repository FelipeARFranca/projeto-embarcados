"""
simulate_mqtt.py
Simula o ESP32 enviando frequências de notas de contrabaixo via MQTT.
Uso: python3 simulate_mqtt.py
"""

import json
import time
import math
import random
import paho.mqtt.client as mqtt

# ─── Config ───────────────────────────────────────────────────────────────────
MQTT_BROKER = "localhost"
MQTT_PORT   = 1883
MQTT_TOPIC  = "bass/pitch"

# ─── Notas do contrabaixo (nome → frequência de referência) ──────────────────
NOTES = {
    "E1":  41.20, "F1":  43.65, "F#1": 46.25, "G1":  49.00,
    "G#1": 51.91, "A1":  55.00, "A#1": 58.27, "B1":  61.74,
    "C2":  65.41, "C#2": 69.30, "D2":  73.42, "D#2": 77.78,
    "E2":  82.41, "F2":  87.31, "F#2": 92.50, "G2":  98.00,
}

# ─── Modos de simulação ───────────────────────────────────────────────────────
MODES = {
    "1": "Linha de baixo aleatória",
    "2": "Varredura cromática (E1 → G2)",
    "3": "Nota única com desvio progressivo",
    "4": "Corda por corda (cordas soltas)",
}

def build_payload(freq: float) -> str:
    return json.dumps({
        "freq":      round(freq, 2),
        "device_id": "esp32-simulator",
        "ts":        int(time.time() * 1000),
    })

def send(client, freq, label=""):
    payload = build_payload(freq)
    client.publish(MQTT_TOPIC, payload)
    print(f"  → {freq:.2f} Hz  {label}")

def connect_mqtt() -> mqtt.Client:
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1)
    client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
    client.loop_start()
    print(f"✅ Conectado ao broker {MQTT_BROKER}:{MQTT_PORT}\n")
    return client

# ─── Modos ────────────────────────────────────────────────────────────────────
def mode_random_bassline(client):
    """Toca uma linha de baixo aleatória com pequenas variações de afinação."""
    print("🎸 Modo: Linha de baixo aleatória  (Ctrl+C para parar)\n")
    note_list = list(NOTES.items())
    while True:
        name, ref = random.choice(note_list)
        # Adiciona desvio aleatório de ±30 cents para simular desafinação
        cents_offset = random.uniform(-30, 30)
        freq = ref * (2 ** (cents_offset / 1200))
        status = "✓ afinado" if abs(cents_offset) <= 5 else (
                 "▲ agudo"   if cents_offset > 0 else "▼ grave")
        send(client, freq, f"{name}  {cents_offset:+.1f}¢  {status}")
        time.sleep(0.4)

def mode_chromatic_sweep(client):
    """Percorre todas as notas em ordem cromática, uma por uma."""
    print("🎸 Modo: Varredura cromática E1 → G2\n")
    note_list = list(NOTES.items())
    for name, ref in note_list:
        send(client, ref, f"{name}  (afinado)")
        time.sleep(0.8)
    print("\n  Varredura concluída.")

def mode_single_note_drift(client):
    """Toca A1 repetidamente, simulando uma corda sendo afinada aos poucos."""
    print("🎸 Modo: Afinando A1 (55 Hz) progressivamente  (Ctrl+C para parar)\n")
    ref   = NOTES["A1"]
    drift = -40.0   # começa 40 cents abaixo
    step  =   2.0   # sobe 2 cents por leitura
    while True:
        drift = max(-50, min(50, drift))
        freq  = ref * (2 ** (drift / 1200))
        status = "✓ afinado" if abs(drift) <= 5 else (
                 "▲ agudo"   if drift > 0 else "▼ grave")
        send(client, freq, f"A1  {drift:+.1f}¢  {status}")
        drift += step
        if drift > 50:
            drift = -40.0   # reinicia
            print("  --- reiniciando ciclo ---")
        time.sleep(0.3)

def mode_open_strings(client):
    """Toca as 4 cordas soltas em loop: E1 → A1 → D2 → G2."""
    print("🎸 Modo: Cordas soltas em loop  (Ctrl+C para parar)\n")
    strings = [("E1", NOTES["E1"]), ("A1", NOTES["A1"]),
               ("D2", NOTES["D2"]), ("G2", NOTES["G2"])]
    while True:
        for name, ref in strings:
            # Pequena variação de ±8 cents
            cents = random.uniform(-8, 8)
            freq  = ref * (2 ** (cents / 1200))
            send(client, freq, f"{name}  {cents:+.1f}¢")
            time.sleep(0.6)

# ─── Main ─────────────────────────────────────────────────────────────────────
def main():
    print("=" * 50)
    print("  Bass Tuner — Simulador MQTT")
    print("=" * 50)
    print()
    for k, v in MODES.items():
        print(f"  [{k}] {v}")
    print()

    choice = input("Escolha um modo: ").strip()
    if choice not in MODES:
        print("Opção inválida.")
        return

    client = connect_mqtt()

    try:
        if   choice == "1": mode_random_bassline(client)
        elif choice == "2": mode_chromatic_sweep(client)
        elif choice == "3": mode_single_note_drift(client)
        elif choice == "4": mode_open_strings(client)
    except KeyboardInterrupt:
        print("\n\nSimulação encerrada.")
    finally:
        client.loop_stop()
        client.disconnect()

if __name__ == "__main__":
    main()