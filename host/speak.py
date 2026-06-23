#!/usr/bin/env python3
"""
StackChan TTS — make StackChan speak with text-to-speech.

Supports two TTS engines:
  - ElevenLabs (high quality, requires API key)
  - edge-tts (free, Microsoft Azure voices)

Flow: TTS → MP3 → WAV (16kHz mono) → HTTP serve → StackChan /play

Usage:
  python speak.py "Hello world"
  python speak.py --expr happy "Nice to meet you"
  python speak.py --edge "Using free TTS"
  python speak.py --host 192.168.0.100 "Custom IP"

Environment / .env file:
  ELEVEN_API_KEY   — ElevenLabs API key
  ELEVEN_VOICE_ID  — ElevenLabs voice ID
  ELEVEN_MODEL     — Model (default: eleven_v3)
"""

import subprocess
import sys
import os
import re
import time
import argparse

AUDIO_PORT = 7071
EDGE_TTS_VOICE = "zh-CN-YunxiNeural"
VOLUME = 0.3


def lan_get(host, path, params=None, timeout=5):
    from urllib.parse import urlencode
    url = f"http://{host}{path}"
    if params:
        url += "?" + urlencode(params)
    if sys.platform == "darwin":
        r = subprocess.run(["/usr/bin/curl", "-s", "--max-time", str(timeout), url],
                           capture_output=True, timeout=timeout + 5)
        if r.returncode != 0:
            raise RuntimeError(f"curl exit {r.returncode} for {path}")
        return r.stdout.decode()
    else:
        import requests
        return requests.get(url, timeout=timeout).text


def load_env(env_path):
    env = {}
    try:
        with open(env_path) as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    k, v = line.split("=", 1)
                    env[k.strip()] = v.strip()
    except FileNotFoundError:
        pass
    return env


def tts_elevenlabs(text, mp3_path, env):
    key, voice = env.get("ELEVEN_API_KEY"), env.get("ELEVEN_VOICE_ID")
    if not key or not voice:
        return False
    import requests
    try:
        resp = requests.post(
            f"https://api.elevenlabs.io/v1/text-to-speech/{voice}",
            headers={"xi-api-key": key, "Content-Type": "application/json"},
            json={"text": text, "model_id": env.get("ELEVEN_MODEL", "eleven_v3")},
            params={"output_format": "mp3_44100_128"},
            timeout=60,
        )
        if resp.status_code != 200:
            print(f"ElevenLabs {resp.status_code}: {resp.text[:200]}", file=sys.stderr)
            return False
        with open(mp3_path, "wb") as f:
            f.write(resp.content)
        return True
    except Exception as e:
        print(f"ElevenLabs failed: {e}", file=sys.stderr)
        return False


def tts_edge(text, mp3_path):
    r = subprocess.run(
        ["edge-tts", "--voice", EDGE_TTS_VOICE, "--text", text, "--write-media", mp3_path],
        capture_output=True, timeout=15
    )
    if r.returncode != 0:
        print(f"edge-tts failed: {r.stderr.decode()}", file=sys.stderr)
        return False
    return True


def convert_to_wav(mp3_path, wav_path):
    if sys.platform == "darwin":
        r = subprocess.run(
            ["/usr/bin/afconvert", "-f", "WAVE", "-d", "LEI16@16000", "-c", "1", mp3_path, wav_path],
            capture_output=True, timeout=10
        )
    else:
        r = subprocess.run(
            ["ffmpeg", "-y", "-i", mp3_path, "-ar", "16000", "-ac", "1", "-f", "wav", wav_path],
            capture_output=True, timeout=10
        )
    if r.returncode != 0:
        print(f"Convert failed: {r.stderr.decode()}", file=sys.stderr)
        return False
    return True


def apply_volume(wav_path, volume):
    if volume >= 1.0:
        return
    import wave, struct
    w = wave.open(wav_path, 'rb')
    params = w.getparams()
    n = w.getnframes()
    frames = w.readframes(n)
    w.close()
    samples = struct.unpack(f'<{n}h', frames)
    scaled = [int(s * volume) for s in samples]
    w = wave.open(wav_path, 'wb')
    w.setparams(params)
    w.writeframes(struct.pack(f'<{n}h', *scaled))
    w.close()


def ensure_audio_server(host_ip, audio_dir):
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.connect((host_ip, AUDIO_PORT))
        s.close()
        return
    except Exception:
        pass

    os.makedirs(audio_dir, exist_ok=True)
    subprocess.Popen(
        ["python3", "-m", "http.server", str(AUDIO_PORT), "--bind", host_ip],
        cwd=audio_dir,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )
    time.sleep(0.5)


def speak(text, stackchan_host, host_ip, expr=None, engine="eleven", env_path=".env"):
    script_dir = os.path.dirname(os.path.abspath(__file__))
    audio_dir = os.path.join(script_dir, "audio")
    os.makedirs(audio_dir, exist_ok=True)
    mp3_path = os.path.join(audio_dir, "speak.mp3")
    wav_path = os.path.join(audio_dir, "speak.wav")

    # Strip audio tags (ElevenLabs-specific) from subtitle text
    subtitle_text = re.sub(r"\[[^\]]+\]", "", text).strip() or text

    # TTS
    ok = False
    if engine == "eleven":
        ok = tts_elevenlabs(text, mp3_path, load_env(env_path))
    if not ok:
        ok = tts_edge(subtitle_text, mp3_path)
    if not ok:
        return False

    # Convert MP3 → WAV (16kHz mono)
    if not convert_to_wav(mp3_path, wav_path):
        return False

    apply_volume(wav_path, VOLUME)

    # Set expression (after audio is ready, so expression syncs with speech)
    if expr:
        try:
            lan_get(stackchan_host, "/face", {"expr": expr})
        except Exception as e:
            print(f"Face set failed: {e}", file=sys.stderr)

    # Show subtitle
    import wave as _wave
    try:
        _w = _wave.open(wav_path, 'rb')
        dur_ms = int(_w.getnframes() / _w.getframerate() * 1000)
        _w.close()
    except Exception:
        dur_ms = len(subtitle_text) * 250
    try:
        lan_get(stackchan_host, "/speech", {"text": subtitle_text, "dur": dur_ms}, timeout=3)
    except Exception:
        pass

    # Serve audio file and tell StackChan to play it
    ensure_audio_server(host_ip, audio_dir)
    url = f"http://{host_ip}:{AUDIO_PORT}/speak.wav"
    try:
        import json
        play_timeout = max(30, dur_ms // 1000 + 15)
        data = json.loads(lan_get(stackchan_host, "/play", {"url": url}, timeout=play_timeout))
        try:
            lan_get(stackchan_host, "/speech", {"text": ""}, timeout=3)
        except Exception:
            pass
        if data.get("ok"):
            return True
        else:
            print(f"Play error: {data}", file=sys.stderr)
            return False
    except Exception as e:
        print(f"Play error: {e}", file=sys.stderr)
        return False


def main():
    parser = argparse.ArgumentParser(description="Make StackChan speak")
    parser.add_argument("text", nargs="+", help="Text to speak")
    parser.add_argument("--host", default=os.environ.get("STACKCHAN_HOST", "192.168.0.162"),
                        help="StackChan IP (default: $STACKCHAN_HOST or 192.168.0.162)")
    parser.add_argument("--host-ip", default=os.environ.get("HOST_IP", ""),
                        help="This computer's LAN IP (for StackChan to fetch audio). "
                             "Auto-detected if not set.")
    parser.add_argument("--expr", default=None,
                        help="Expression while speaking (happy/sad/angry/etc)")
    parser.add_argument("--edge", action="store_true",
                        help="Force edge-tts (free, no API key needed)")
    parser.add_argument("--env", default=".env",
                        help="Path to .env file with API keys (default: .env)")
    args = parser.parse_args()

    host_ip = args.host_ip
    if not host_ip:
        import socket
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.connect(("8.8.8.8", 80))
            host_ip = s.getsockname()[0]
        finally:
            s.close()

    engine = "edge" if args.edge else "eleven"
    text = " ".join(args.text)
    ok = speak(text, args.host, host_ip, args.expr, engine, args.env)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
