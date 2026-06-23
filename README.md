# StackChan HTTP API

Turn your [StackChan](https://github.com/stack-chan/stack-chan) into a WiFi-controlled robot with a simple HTTP API. All AI/ML processing runs on a host computer — the ESP32 is just a "dumb terminal" that handles hardware.

## Architecture

```
Host Computer (Mac/PC/RPi)              StackChan (ESP32-S3)
┌───────────────────────┐               ┌────────────────────┐
│                       │               │                    │
│  Your AI/App/Script   │─── HTTP ────▶ │  HTTP API Server   │
│                       │               │                    │
│  find_face.py         │◀── JPEG ──── │  Camera (GC0308)   │
│  (MediaPipe +         │─── servo ──▶  │  Servos (yaw/pitch)│
│   InsightFace)        │─── audio ──▶  │  Speaker           │
│                       │◀── audio ─── │  Microphone        │
│  face_tracker.py      │─── expr ───▶  │  Avatar Display    │
│  speak.py             │               │  Touch Sensors     │
│                       │               │                    │
└───────────────────────┘               └────────────────────┘
```

**Why this approach?** The ESP32 can't run ML models, but it has great hardware (camera, servos, mic, speaker, touch). By exposing everything over HTTP, you can use any language/framework/AI on your host computer to control it.

## Hardware

- M5Stack StackChan Kit (CoreS3 ESP32-S3)
  - GC0308 camera, dual microphone, speaker
  - 2x servos (horizontal ±128°, vertical 0-90°)
  - Touch sensors (3-zone capacitive on top)
  - 2.0" color display (320x240)

## Quick Start

### 1. Flash the Firmware

```bash
# Install arduino-cli and M5Stack board support
brew install arduino-cli   # macOS
arduino-cli core install m5stack:esp32

# Configure WiFi
cp firmware/config.h.example firmware/config.h
# Edit config.h with your WiFi credentials and IP settings

# Compile and upload
arduino-cli compile --fqbn m5stack:esp32:m5stack_cores3 firmware/
arduino-cli upload --fqbn m5stack:esp32:m5stack_cores3 --port /dev/cu.usbmodem* firmware/
```

### 2. Verify

Open `http://<stackchan-ip>/` in your browser. You should see the control panel.

### 3. Install Host Dependencies

```bash
pip install opencv-python mediapipe numpy

# Optional: for face identity verification
pip install insightface onnxruntime
```

### 4. Find a Face

```bash
# Detect any face
python host/find_face.py --host <stackchan-ip>

# With identity verification
python host/register_face.py my_photo1.jpg my_photo2.jpg -o my_face.pkl
python host/find_face.py --host <stackchan-ip> --face-db my_face.pkl

# Continuous tracking
python host/face_tracker.py --host <stackchan-ip>
```

## HTTP API Reference

All endpoints return JSON (except `/camera` which returns JPEG).

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Control panel (HTML) |
| `/face?expr=<expr>` | GET | Set expression: `neutral`, `happy`, `sad`, `angry`, `sleepy`, `doubt`, `love`, `eyeroll` |
| `/servo?yaw=N&pitch=N&speed=N` | GET | Move servos. Yaw: -1280 to 1280. Pitch: 0 to 900. Speed: 0 to 1000 |
| `/camera` | GET | Capture JPEG photo (320x240) |
| `/status` | GET | Device status (battery, servo position, WiFi RSSI, uptime) |
| `/home` | GET | Center servos to home position |
| `/touch` | GET | Touch sensor readings (front, middle, back) |
| `/record?seconds=N` | GET | Record audio, returns WAV. Optional: `vad=1` for voice activity detection, `led=0` to hide LED |
| `/stream?port=N` | GET | Stream raw PCM (16kHz mono int16) to host TCP port. Host disconnect = stop |
| `/play?url=<url>` | GET | Stream and play WAV from URL (max 2MB ≈ 62s). Supports lip sync |
| `/speech?text=<text>` | GET | Display subtitle text. Optional: `dur=<ms>` for auto-paging sync |
| `/volume` | GET | Mic volume level (RMS + peak) |

## Face Detection: How It Works

`find_face.py` implements a scan-detect-center pipeline:

1. **Check current position** — take 2 frames (double confirmation to avoid false positives)
2. **If no face** — scan 12 preset servo angles, checking each position
3. **Identity verification** (optional) — compare detected face against a stored embedding using InsightFace ArcFace
4. **Center** — proportional controller adjusts servos to center the face in frame
5. **Output** — JSON result with confidence, position, similarity score

`face_tracker.py` runs continuously with two modes:
- **Lazy** (default): checks every 3-10 seconds, only moves on large displacement. Quiet and unobtrusive.
- **Active**: checks every 0.3 seconds, tight tracking. Responsive but servos are noisy.

## Notes

- **GC0308 camera**: No hardware JPEG encoder. Uses RGB565 → software JPEG conversion. Quality is low (0.3MP) but sufficient for face detection.
- **Mic and speaker share I2S bus**: Half-duplex — cannot record and play simultaneously.
- **macOS users**: Python `socket` may get `Errno 65` connecting to LAN devices. The included `curl_session.py` works around this by using `/usr/bin/curl` subprocess.
- **WiFi sleep is disabled** in the firmware to avoid intermittent connection drops on first request after idle.

## License

MIT
