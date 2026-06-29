#!/usr/bin/env python3
"""
StackChan listen engine — stream audio from StackChan, detect end of speech, transcribe.

Flow: bind TCP port → kick firmware /stream → receive PCM in real-time →
      WebRTC VAD + RMS energy gate detects end of utterance → Whisper transcribes

Exit codes: 0 = transcribed (text on stdout), 1 = no speech, 2 = error

Usage:
  python listen.py --host 192.168.0.162
  python listen.py --host 192.168.0.162 --no-speech-timeout 10 --max-secs 60
  python listen.py --host 192.168.0.162 --whisper-model /path/to/ggml-large-v3-turbo.bin
"""

import subprocess, time, os, struct, math, sys, wave, socket, collections, argparse
import webrtcvad

GAIN = 15

# ---- VAD parameters (20ms frames @16kHz) ----
VAD_MODE = 3          # 0-3, higher = stricter (less likely to flag noise as speech)
START_FRAMES = 3      # 3 consecutive voiced frames (60ms) = speech started
END_FRAMES = 45       # 45 consecutive silent frames (0.9s) after speech = done
PREROLL_FRAMES = 15   # capture 300ms before speech start to avoid clipping
# RMS energy gate: WebRTC VAD alone flags amplified background noise as speech.
# Both VAD and energy must pass to count as voiced.
# Calibrated: background noise ~2000-2800 RMS, human speech ~6700-17500 RMS
VAD_RMS_MIN = 3500
# Mic startup spike (~0.5s of rms~10000 "pop"), discard these frames
STARTUP_SKIP_FRAMES = 25

MIN_RMS = 500

# Common Whisper hallucinations on silence/noise
JUNK = ["谢谢观看", "谢谢收看", "字幕", "感谢观看",
        "请不吝点赞", "订阅", "小铃铛", "下期再见",
        "优优独播", "YoYo", "Television", "thank you", "subscribe",
        "请订阅", "感谢大家", "欢迎收看", "敬请期待"]

# Simplified Chinese conversion for common traditional characters
T2S = str.maketrans(
    "聽說話嗎這個們點時間愛認識開關機書寫問題覺無門醬優獨播劇場歡學習從來後對請過還會種為現東動買賣區義課",
    "听说话吗这个们点时间爱认识开关机书写问题觉无门酱优独播剧场欢学习从来后对请过还会种为现东动买卖区义课")


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", file=sys.stderr, flush=True)


def lan_curl(url, timeout):
    """HTTP GET via curl subprocess (macOS local network permission workaround)."""
    if sys.platform == "darwin":
        return subprocess.Popen(
            ["/usr/bin/curl", "-s", "--max-time", str(timeout), url],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:
        import requests
        return subprocess.Popen(
            ["curl", "-s", "--max-time", str(timeout), url],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def record_stream(host, no_speech_timeout, max_secs):
    """Stream audio from StackChan, return (status, samples). Status: ok/nospeech/error."""
    base_url = f"http://{host}"
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", 0))
    port = srv.getsockname()[1]
    srv.listen(1)
    srv.settimeout(5)

    kicker = lan_curl(f"{base_url}/stream?port={port}", max_secs + 15)

    try:
        conn, _ = srv.accept()
    except socket.timeout:
        log("StackChan didn't connect (offline or firmware too old?)")
        srv.close()
        kicker.kill()
        return ("error", None)
    conn.settimeout(2)
    log(f"Streaming on :{port} (speak freely)")

    vad = webrtcvad.Vad(VAD_MODE)
    FRAME = 320  # 20ms @16kHz
    frame_bytes = FRAME * 2

    buf = b""
    samples = []
    preroll = collections.deque(maxlen=PREROLL_FRAMES)
    started = False
    voiced_run = 0
    silent_run = 0
    skipped = 0
    status = "error"
    t0 = time.time()
    meter_rms_max = 0.0
    meter_voiced = 0
    meter_frames = 0

    try:
        while True:
            if time.time() - t0 > max_secs:
                status = "ok" if started else "nospeech"
                log("Max length reached")
                break
            try:
                data = conn.recv(4096)
            except socket.timeout:
                status = "ok" if started else "error"
                log("recv timeout")
                break
            if not data:
                status = "ok" if started else "error"
                log("Closed by StackChan")
                break
            buf += data
            done = False
            while len(buf) >= frame_bytes:
                raw = buf[:frame_bytes]
                buf = buf[frame_bytes:]
                s = struct.unpack(f'<{FRAME}h', raw)
                if skipped < STARTUP_SKIP_FRAMES:
                    skipped += 1
                    continue
                amp = [max(-32767, min(32767, x * GAIN)) for x in s]
                fb = struct.pack(f'<{FRAME}h', *amp)
                frame_rms = math.sqrt(sum(x * x for x in amp) / FRAME)
                voiced = frame_rms > VAD_RMS_MIN and vad.is_speech(fb, 16000)

                meter_rms_max = max(meter_rms_max, frame_rms)
                meter_voiced += 1 if voiced else 0
                meter_frames += 1
                if meter_frames >= 50:
                    log(f"  meter: rms_max={meter_rms_max:.0f} voiced={meter_voiced}/50"
                        f" {'STARTED' if started else 'waiting'} silent_run={silent_run}")
                    meter_rms_max = 0.0
                    meter_voiced = 0
                    meter_frames = 0

                if not started:
                    preroll.append(amp)
                    voiced_run = voiced_run + 1 if voiced else 0
                    if voiced_run >= START_FRAMES:
                        started = True
                        for p in preroll:
                            samples.extend(p)
                        log("Speech started")
                else:
                    samples.extend(amp)
                    silent_run = silent_run + 1 if not voiced else 0
                    if silent_run >= END_FRAMES:
                        status = "ok"
                        log(f"End of utterance ({len(samples)/16000:.1f}s)")
                        done = True
                        break
            if done:
                break
            if not started and time.time() - t0 > no_speech_timeout:
                status = "nospeech"
                log("No speech, giving up")
                break
    finally:
        try: conn.close()
        except: pass
        try: srv.close()
        except: pass
        try: kicker.wait(timeout=5)
        except Exception: kicker.kill()

    if status == "ok" and samples:
        cut = (END_FRAMES - 10) * FRAME
        if len(samples) > cut + FRAME * 25:
            samples = samples[:-cut]
        return ("ok", samples)
    return (status, None)


def transcribe(samples, whisper_model, language, audio_dir, prompt):
    os.makedirs(audio_dir, exist_ok=True)
    path = os.path.join(audio_dir, "listen_last.wav")
    w = wave.open(path, 'wb')
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000)
    w.writeframes(struct.pack(f'<{len(samples)}h', *samples))
    w.close()

    rms = math.sqrt(sum(s * s for s in samples) / len(samples))
    if rms < MIN_RMS:
        log(f"Too quiet (RMS {rms:.0f})")
        return ""

    cmd = ["whisper-cli", "-m", whisper_model, "-l", language, "-f", path,
           "--no-timestamps", "-t", "4"]
    if prompt:
        cmd += ["--prompt", prompt]

    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    text = "".join(l.strip() for l in r.stdout.split("\n")
        if l.strip() and not any(l.strip().startswith(p)
        for p in ["whisper_", "ggml_", "load_", "main:", "system_info"])).strip()

    text = text.translate(T2S).strip().strip("。，！？.!?, ")
    if not text or len(text) < 2:
        return ""
    if any(h in text for h in JUNK):
        log(f"Filtered hallucination: '{text}'")
        return ""
    return text


def main():
    ap = argparse.ArgumentParser(description="StackChan speech-to-text")
    ap.add_argument("--host", default=os.environ.get("STACKCHAN_HOST", "192.168.0.162"),
                    help="StackChan IP (default: $STACKCHAN_HOST or 192.168.0.162)")
    ap.add_argument("--no-speech-timeout", type=float, default=8.0,
                    help="Give up if no speech within N seconds (default: 8)")
    ap.add_argument("--max-secs", type=float, default=110.0,
                    help="Max recording length in seconds (default: 110)")
    ap.add_argument("--whisper-model", default=os.environ.get("WHISPER_MODEL", ""),
                    help="Path to Whisper GGML model file")
    ap.add_argument("--language", default="zh", help="Language code for Whisper (default: zh)")
    ap.add_argument("--prompt", default="", help="Whisper prompt for better recognition")
    ap.add_argument("--audio-dir", default=os.path.join(os.path.dirname(__file__), "audio"),
                    help="Directory to save audio files")
    args = ap.parse_args()

    if not args.whisper_model:
        default_path = os.path.expanduser("~/models/whisper/ggml-large-v3-turbo.bin")
        if os.path.exists(default_path):
            args.whisper_model = default_path
        else:
            print("Error: --whisper-model required (or set WHISPER_MODEL env var)", file=sys.stderr)
            sys.exit(2)

    status, samples = record_stream(args.host, args.no_speech_timeout, args.max_secs)
    if status == "nospeech":
        sys.exit(1)
    if status != "ok":
        sys.exit(2)
    if len(samples) / 16000 < 0.5:
        log("Too short")
        sys.exit(1)

    text = transcribe(samples, args.whisper_model, args.language, args.audio_dir, args.prompt)
    if not text:
        sys.exit(1)
    print(text)
    sys.exit(0)


if __name__ == "__main__":
    main()
