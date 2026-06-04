"""
backend/main.py
FastAPI + MQTT + WebSocket
- Assina bass/pitch no broker Mosquitto
- Mapeia frequência → nota musical (com desvio em cents)
- Recebe e repassa métricas de performance das Vertentes 1 e 2
- Transmite resultado via WebSocket para o dashboard
"""

import asyncio
import json
import math
import time
from contextlib import asynccontextmanager
from typing import Optional

import paho.mqtt.client as mqtt
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware

# ─── Configurações ────────────────────────────────────────────────────────────
MQTT_BROKER = "172.26.68.211"
MQTT_PORT   = 1883
MQTT_TOPIC  = "bass/pitch"

# ─── Tabela de notas do contrabaixo ──────────────────────────────────────────
# Frequências das 4 cordas (afinação padrão E1-A1-D2-G2) + 5 trastes por corda
BASS_NOTES: list[tuple[str, float]] = [
    # Corda E (Mi)
    ("E1",  41.20), ("F1",  43.65), ("F#1", 46.25), ("G1",  49.00), ("G#1", 51.91), ("A1",  55.00),
    # Corda A (Lá)
    ("A1",  55.00), ("A#1", 58.27), ("B1",  61.74), ("C2",  65.41), ("C#2", 69.30), ("D2",  73.42),
    # Corda D (Ré)
    ("D2",  73.42), ("D#2", 77.78), ("E2",  82.41), ("F2",  87.31), ("F#2", 92.50), ("G2",  98.00),
    # Corda G (Sol)
    ("G2",  98.00), ("G#2",103.83), ("A2", 110.00), ("A#2",116.54), ("B2", 123.47), ("C3", 130.81),
    # Extensão (harm. e cordas de 5)
    ("C3", 130.81), ("C#3",138.59), ("D3", 146.83), ("D#3",155.56), ("E3", 164.81), ("F3", 174.61),
]

# Remove duplicatas mantendo a ordem
seen: set[str] = set()
BASS_NOTES_UNIQUE: list[tuple[str, float]] = []
for name, freq in BASS_NOTES:
    key = f"{name}-{freq}"
    if key not in seen:
        seen.add(key)
        BASS_NOTES_UNIQUE.append((name, freq))

# ─── Mapeamento de frequência → nota ─────────────────────────────────────────
def freq_to_note(freq: float) -> dict:
    best_name  = "?"
    best_freq  = 0.0
    best_cents = float("inf")

    for name, ref_freq in BASS_NOTES_UNIQUE:
        cents = 1200.0 * math.log2(freq / ref_freq)
        if abs(cents) < abs(best_cents):
            best_cents = cents
            best_name  = name
            best_freq  = ref_freq

    if abs(best_cents) <= 5:
        tuning_status = "afinado"
    elif best_cents > 5:
        tuning_status = "agudo"
    else:
        tuning_status = "grave"

    return {
        "note":          best_name,
        "ref_freq":      round(best_freq, 2),
        "detected_freq": round(freq, 2),
        "cents":         round(best_cents, 1),
        "tuning_status": tuning_status,
    }

# ─── WebSocket Manager ────────────────────────────────────────────────────────
class ConnectionManager:
    def __init__(self):
        self.active: list[WebSocket] = []

    async def connect(self, ws: WebSocket):
        await ws.accept()
        self.active.append(ws)

    def disconnect(self, ws: WebSocket):
        self.active.remove(ws)

    async def broadcast(self, data: dict):
        dead = []
        for ws in self.active:
            try:
                await ws.send_json(data)
            except Exception:
                dead.append(ws)
        for ws in dead:
            self.active.remove(ws)

manager = ConnectionManager()

# ─── Estado global ────────────────────────────────────────────────────────────
latest_pitch: Optional[dict] = None
pitch_history: list[dict]    = []   # últimas 60 leituras de pitch
perf_history: list[dict]     = []   # histórico de métricas de performance

# ─── MQTT callbacks ───────────────────────────────────────────────────────────
def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print(f"✅ MQTT conectado ao broker {MQTT_BROKER}")
        client.subscribe(MQTT_TOPIC)
    else:
        print(f"❌ Falha ao conectar MQTT (rc={rc})")

def on_message(client, userdata, msg):
    global latest_pitch, pitch_history, perf_history
    try:
        payload = json.loads(msg.payload.decode())
        freq    = float(payload.get("freq", 0))

        if freq <= 0:
            return

        note_info = freq_to_note(freq)
        note_info["ts"]        = time.time()
        note_info["device_id"] = payload.get("device_id", "esp32")

        # ── Métricas de performance das duas vertentes ──
        v1 = payload.get("v1_naive", {})
        v2 = payload.get("v2_ring",  {})

        perf = {
            "ts":            note_info["ts"],
            "n":             v1.get("n", 0),
            "v1_lat_us":     v1.get("lat_us",  0),
            "v2_lat_us":     v2.get("lat_us",  0),
            "v1_heap_delta": v1.get("heap_after", 0) - v1.get("heap_before", 0),
            "v2_heap_delta": v2.get("heap_after", 0) - v2.get("heap_before", 0),
        }

        note_info["perf"] = perf
        latest_pitch = note_info

        pitch_history.append(note_info)
        if len(pitch_history) > 60:
            pitch_history.pop(0)

        perf_history.append(perf)
        if len(perf_history) > 200:
            perf_history.pop(0)

        asyncio.run_coroutine_threadsafe(
            manager.broadcast(note_info),
            app.state.loop
        )

        print(
            f"🎸 {note_info['note']} | {freq:.1f} Hz | {note_info['cents']:+.1f}¢ "
            f"| V1: {perf['v1_lat_us']} µs | V2: {perf['v2_lat_us']} µs "
            f"| n={perf['n']}"
        )

    except Exception as e:
        print(f"Erro ao processar mensagem MQTT: {e}")

# ─── Startup / Shutdown ───────────────────────────────────────────────────────
mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1)
mqtt_client.on_connect = on_connect
mqtt_client.on_message = on_message

@asynccontextmanager
async def lifespan(app: FastAPI):
    app.state.loop = asyncio.get_event_loop()
    mqtt_client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
    mqtt_client.loop_start()
    yield
    mqtt_client.loop_stop()
    mqtt_client.disconnect()

# ─── FastAPI app ──────────────────────────────────────────────────────────────
app = FastAPI(title="Bass Tuner API", lifespan=lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# ─── Endpoints REST ───────────────────────────────────────────────────────────
@app.get("/")
def root():
    return {"status": "online", "service": "bass-tuner-api"}

@app.get("/pitch/latest")
def get_latest():
    """Retorna o último pitch detectado."""
    if not latest_pitch:
        return {"message": "Nenhum sinal detectado ainda."}
    return latest_pitch

@app.get("/pitch/history")
def get_history():
    """Retorna o histórico das últimas 60 leituras."""
    return {"count": len(pitch_history), "history": pitch_history}

@app.get("/perf/history")
def get_perf_history():
    """Retorna o histórico das métricas de performance das duas vertentes."""
    return {"count": len(perf_history), "history": perf_history}

@app.get("/notes")
def list_notes():
    """Retorna a tabela de notas do contrabaixo usada como referência."""
    return [{"note": n, "freq": f} for n, f in BASS_NOTES_UNIQUE]

# ─── WebSocket ────────────────────────────────────────────────────────────────
@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket):
    await manager.connect(ws)
    # Envia o estado mais recente imediatamente ao conectar
    if latest_pitch:
        await ws.send_json(latest_pitch)
    try:
        while True:
            await ws.receive_text()   # mantém conexão aberta
    except WebSocketDisconnect:
        manager.disconnect(ws)
