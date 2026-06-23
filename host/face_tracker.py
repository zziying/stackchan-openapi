#!/usr/bin/env python3
"""
StackChan face tracker — continuous face following.

Two modes:
  --lazy (default): slow polling, only moves on large displacement, quiet
  --active:         fast polling, tight tracking

Usage:
  python face_tracker.py                           # lazy mode
  python face_tracker.py --active                  # active mode
  python face_tracker.py --host 192.168.0.100      # custom StackChan IP
"""
import cv2
import numpy as np
import time
import argparse
import signal
import sys
import os
import mediapipe as mp
from mediapipe.tasks import python as mp_python
from mediapipe.tasks.python import vision

FRAME_W, FRAME_H = 320, 240
CENTER_X, CENTER_Y = FRAME_W // 2, FRAME_H // 2

MODEL_PATH = "/tmp/blaze_face_short_range.tflite"
MODEL_URL = "https://storage.googleapis.com/mediapipe-models/face_detector/blaze_face_short_range/float16/latest/blaze_face_short_range.tflite"

YAW_MIN, YAW_MAX = -1280, 1280
PITCH_MIN, PITCH_MAX = 0, 900
MIN_CONFIDENCE = 0.5

LAZY_PARAMS = {
    "kp": 0.6,
    "dead_zone": 60,
    "servo_speed": 150,
    "interval": 3.0,
    "settled_interval": 10.0,
    "search_after": 10,
    "search_speed": 200,
    "search_pause": 1.0,
}

ACTIVE_PARAMS = {
    "kp": 0.8,
    "dead_zone": 25,
    "servo_speed": 300,
    "interval": 0.3,
    "settled_interval": 0.3,
    "search_after": 8,
    "search_speed": 400,
    "search_pause": 0.6,
}

SEARCH_POSITIONS = [
    (0, 200),
    (-400, 200),
    (400, 200),
    (-800, 200),
    (800, 200),
    (0, 500),
    (-400, 500),
    (400, 500),
]

running = True


def get_session(host):
    if sys.platform == "darwin":
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from curl_session import CurlSession
        return CurlSession()
    else:
        import requests
        return requests.Session()


def ensure_model():
    if not os.path.exists(MODEL_PATH):
        print("Downloading face detection model...")
        import urllib.request
        urllib.request.urlretrieve(MODEL_URL, MODEL_PATH)
        print("Done.")


def signal_handler(sig, frame):
    global running
    print("\nStopping...")
    running = False


def get_frame(session, base_url):
    r = session.get(f"{base_url}/camera", timeout=5)
    r.raise_for_status()
    return cv2.imdecode(np.frombuffer(r.content, np.uint8), cv2.IMREAD_COLOR)


def detect(detector, img):
    rgb = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
    mp_img = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
    results = detector.detect(mp_img)
    if not results.detections:
        return None
    det = max(results.detections,
              key=lambda d: d.bounding_box.width * d.bounding_box.height)
    if det.categories[0].score < MIN_CONFIDENCE:
        return None
    return det


def get_servo_pos(session, base_url):
    r = session.get(f"{base_url}/status", timeout=3)
    data = r.json()
    return data["yaw"], data["pitch"]


def move_servo(session, base_url, yaw, pitch, speed=200):
    yaw = int(max(YAW_MIN, min(YAW_MAX, yaw)))
    pitch = int(max(PITCH_MIN, min(PITCH_MAX, pitch)))
    session.get(f"{base_url}/servo",
                params={"yaw": yaw, "pitch": pitch, "speed": speed},
                timeout=3)
    return yaw, pitch


def set_face(session, base_url, expr):
    try:
        session.get(f"{base_url}/face", params={"expr": expr}, timeout=2)
    except Exception:
        pass


def search_for_face(detector, session, base_url, P):
    print("[search] scanning for face...", flush=True)
    set_face(session, base_url, "doubt")

    for i, (yaw, pitch) in enumerate(SEARCH_POSITIONS):
        if not running:
            return None
        move_servo(session, base_url, yaw, pitch, speed=P["search_speed"])
        time.sleep(P["search_pause"])

        try:
            img = get_frame(session, base_url)
            det = detect(detector, img)
            if det is not None:
                bbox = det.bounding_box
                cx = bbox.origin_x + bbox.width // 2
                cy = bbox.origin_y + bbox.height // 2
                conf = det.categories[0].score
                print(
                    f"[search] found face at pos {i} "
                    f"(yaw={yaw},pitch={pitch}) "
                    f"face({cx},{cy}) conf={conf:.2f}",
                    flush=True
                )
                set_face(session, base_url, "happy")
                return det
        except Exception as e:
            print(f"[search] error at pos {i}: {e}", flush=True)

    print("[search] no face found in full scan", flush=True)
    set_face(session, base_url, "sleepy")
    return None


def main():
    parser = argparse.ArgumentParser(description="StackChan face tracker")
    parser.add_argument("--host", default=os.environ.get("STACKCHAN_HOST", "192.168.0.162"),
                        help="StackChan IP address (default: $STACKCHAN_HOST or 192.168.0.162)")
    parser.add_argument("--active", action="store_true",
                        help="Active tracking mode (fast, noisy)")
    parser.add_argument("--flip-yaw", action="store_true")
    parser.add_argument("--flip-pitch", action="store_true")
    args = parser.parse_args()

    base_url = f"http://{args.host}"
    session = get_session(args.host)

    P = ACTIVE_PARAMS if args.active else LAZY_PARAMS
    mode = "active" if args.active else "lazy"
    yaw_sign = -1 if not args.flip_yaw else 1
    pitch_sign = -1 if not args.flip_pitch else 1

    signal.signal(signal.SIGINT, signal_handler)
    ensure_model()

    options = vision.FaceDetectorOptions(
        base_options=mp_python.BaseOptions(model_asset_path=MODEL_PATH),
        min_detection_confidence=MIN_CONFIDENCE)
    detector = vision.FaceDetector.create_from_options(options)

    print(f"StackChan face tracker [{mode}]")
    print(f"  host: {base_url}")
    print(f"  interval: {P['interval']}s, settled: {P['settled_interval']}s")
    print(f"  dead zone: {P['dead_zone']}px, speed: {P['servo_speed']}")
    print(f"  Ctrl+C to stop\n")

    set_face(session, base_url, "happy")
    no_face_count = 0
    frame_num = 0
    settled = False

    while running:
        t0 = time.time()
        interval = P["settled_interval"] if settled else P["interval"]

        try:
            if no_face_count >= P["search_after"]:
                det = search_for_face(detector, session, base_url, P)
                no_face_count = 0
                settled = False
                if det is None:
                    time.sleep(5 if mode == "lazy" else 2)
                frame_num += 1
                continue

            img = get_frame(session, base_url)
            det = detect(detector, img)

            if det is not None:
                bbox = det.bounding_box
                face_cx = bbox.origin_x + bbox.width // 2
                face_cy = bbox.origin_y + bbox.height // 2
                conf = det.categories[0].score
                dx = face_cx - CENTER_X
                dy = face_cy - CENTER_Y

                if no_face_count > 3:
                    set_face(session, base_url, "happy")
                no_face_count = 0

                if abs(dx) > P["dead_zone"] or abs(dy) > P["dead_zone"]:
                    settled = False
                    cur_yaw, cur_pitch = get_servo_pos(session, base_url)
                    new_yaw = cur_yaw + yaw_sign * P["kp"] * dx
                    new_pitch = cur_pitch + pitch_sign * P["kp"] * dy
                    actual_yaw, actual_pitch = move_servo(
                        session, base_url, new_yaw, new_pitch, P["servo_speed"])
                    print(
                        f"[{frame_num:04d}] face({face_cx},{face_cy}) "
                        f"err({dx:+d},{dy:+d}) conf={conf:.2f} "
                        f"servo->({actual_yaw},{actual_pitch})",
                        flush=True
                    )
                else:
                    if not settled:
                        print(
                            f"[{frame_num:04d}] settled "
                            f"(next check in {P['settled_interval']}s)",
                            flush=True
                        )
                    settled = True
            else:
                no_face_count += 1
                settled = False
                if no_face_count == P["search_after"]:
                    print(f"[{frame_num:04d}] face lost, will search...",
                          flush=True)

        except Exception as e:
            print(f"[{frame_num:04d}] error: {e}", flush=True)
            time.sleep(1)

        frame_num += 1
        elapsed = time.time() - t0
        sleep_time = max(0, interval - elapsed)
        if sleep_time > 0:
            time.sleep(sleep_time)

    detector.close()
    set_face(session, base_url, "neutral")
    print("Tracker stopped.")


if __name__ == "__main__":
    main()
