#!/usr/bin/env python3
"""
One-shot face finder for StackChan.
Scans servo positions until a face is found, centers on it, then exits.
Uses MediaPipe for detection + InsightFace ArcFace for identity verification.
Exit code 0 = found, 1 = not found.

Usage:
  python find_face.py                          # detect any face
  python find_face.py --face-db face.pkl       # detect + verify identity
  python find_face.py --host 192.168.0.100     # custom StackChan IP
"""
import os, sys, warnings, logging

os.environ["GLOG_minloglevel"] = "3"
os.environ["TF_CPP_MIN_LOG_LEVEL"] = "3"
os.environ["ONNXRUNTIME_LOG_LEVEL"] = "3"
warnings.filterwarnings("ignore")
logging.disable(logging.CRITICAL)

_saved_stdout = os.dup(1)
_saved_stderr = os.dup(2)
_devnull_fd = os.open(os.devnull, os.O_WRONLY)
os.dup2(_devnull_fd, 1)
os.dup2(_devnull_fd, 2)

import cv2
import numpy as np
import time
import json
import pickle
import argparse
import onnxruntime as ort
ort.set_default_logger_severity(3)
import mediapipe as mp
from mediapipe.tasks import python as mp_python
from mediapipe.tasks.python import vision

FRAME_W, FRAME_H = 320, 240
CENTER_X, CENTER_Y = FRAME_W // 2, FRAME_H // 2

MODEL_PATH = "/tmp/blaze_face_short_range.tflite"
MODEL_URL = "https://storage.googleapis.com/mediapipe-models/face_detector/blaze_face_short_range/float16/latest/blaze_face_short_range.tflite"

MIN_CONFIDENCE = 0.5
MIN_FACE_SIZE = 60
KP = 1.5
DEAD_ZONE = 30
CENTER_STEPS = 15
IDENTITY_THRESHOLD = 0.5

SEARCH_POSITIONS = [
    (0, 200),
    (-400, 200),
    (400, 200),
    (0, 400),
    (-400, 400),
    (400, 400),
    (-800, 200),
    (800, 200),
    (-800, 400),
    (800, 400),
    (0, 0),
    (0, 600),
]

face_recognizer = None
target_embedding = None


def get_session(host):
    """Return an HTTP session. Uses curl subprocess on macOS (avoids Errno 65
    local network permission issue), falls back to requests elsewhere."""
    if sys.platform == "darwin":
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from curl_session import CurlSession
        return CurlSession()
    else:
        import requests
        return requests.Session()


def ensure_model():
    if not os.path.exists(MODEL_PATH):
        import urllib.request
        urllib.request.urlretrieve(MODEL_URL, MODEL_PATH)


def load_face_db(face_db_path):
    global face_recognizer, target_embedding
    if not face_db_path or not os.path.exists(face_db_path):
        return
    with open(face_db_path, "rb") as f:
        data = pickle.load(f)
    target_embedding = data["embedding"]
    from insightface.app import FaceAnalysis
    face_recognizer = FaceAnalysis(name="buffalo_l",
                                   providers=["CPUExecutionProvider"])
    face_recognizer.prepare(ctx_id=0, det_size=(320, 320))


def output(data):
    os.write(_saved_stdout, (json.dumps(data) + "\n").encode())


def verify_identity(img):
    if face_recognizer is None or target_embedding is None:
        return True, 1.0
    faces = face_recognizer.get(img)
    if not faces:
        return False, 0.0
    face = max(faces, key=lambda f: (f.bbox[2] - f.bbox[0]) * (f.bbox[3] - f.bbox[1]))
    sim = np.dot(face.normed_embedding, target_embedding)
    return sim >= IDENTITY_THRESHOLD, float(sim)


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
    bbox = det.bounding_box
    if bbox.width < MIN_FACE_SIZE or bbox.height < MIN_FACE_SIZE:
        return None
    return det


def confirm_detection(detector, session, base_url):
    img1 = get_frame(session, base_url)
    det1 = detect(detector, img1)
    if det1 is None:
        return None, None
    time.sleep(0.3)
    img2 = get_frame(session, base_url)
    det2 = detect(detector, img2)
    if det2 is None:
        return None, None
    is_target, sim = verify_identity(img2)
    if not is_target:
        return None, sim
    return det2, sim


def move_servo(session, base_url, yaw, pitch, speed=300):
    yaw = int(max(-1280, min(1280, yaw)))
    pitch = int(max(0, min(900, pitch)))
    session.get(f"{base_url}/servo",
                params={"yaw": yaw, "pitch": pitch, "speed": speed},
                timeout=3)
    return yaw, pitch


def get_servo_pos(session, base_url):
    r = session.get(f"{base_url}/status", timeout=3)
    data = r.json()
    return data["yaw"], data["pitch"]


def set_face(session, base_url, expr):
    try:
        session.get(f"{base_url}/face", params={"expr": expr}, timeout=2)
    except Exception:
        pass


def center_on_face(detector, session, base_url):
    missed = 0
    for i in range(CENTER_STEPS):
        try:
            img = get_frame(session, base_url)
            det = detect(detector, img)
            if det is None:
                missed += 1
                if missed >= 5:
                    return False
                time.sleep(0.3)
                continue
            missed = 0
            bbox = det.bounding_box
            cx = bbox.origin_x + bbox.width // 2
            cy = bbox.origin_y + bbox.height // 2
            dx = cx - CENTER_X
            dy = cy - CENTER_Y
            if abs(dx) <= DEAD_ZONE and abs(dy) <= DEAD_ZONE:
                return True
            cur_yaw, cur_pitch = get_servo_pos(session, base_url)
            move_servo(session, base_url, cur_yaw + KP * dx, cur_pitch - KP * dy)
            time.sleep(0.5)
        except Exception:
            missed += 1
            if missed >= 5:
                return False
    return True


def save_photo(session, base_url, path):
    try:
        img = get_frame(session, base_url)
        cv2.imwrite(path, img)
    except Exception:
        pass


def main():
    parser = argparse.ArgumentParser(description="StackChan face finder")
    parser.add_argument("--host", default=os.environ.get("STACKCHAN_HOST", "192.168.0.162"),
                        help="StackChan IP address (default: $STACKCHAN_HOST or 192.168.0.162)")
    parser.add_argument("--face-db", default=None,
                        help="Path to face embedding .pkl for identity verification")
    parser.add_argument("--save-photo", default=None,
                        help="Save photo to this path when face is found")
    args = parser.parse_args()

    base_url = f"http://{args.host}"
    session = get_session(args.host)

    ensure_model()
    load_face_db(args.face_db)

    options = vision.FaceDetectorOptions(
        base_options=mp_python.BaseOptions(model_asset_path=MODEL_PATH),
        min_detection_confidence=MIN_CONFIDENCE)
    detector = vision.FaceDetector.create_from_options(options)

    has_recognition = face_recognizer is not None

    # Check current position first
    try:
        det, sim = confirm_detection(detector, session, base_url)
        if det is not None:
            conf = det.categories[0].score
            set_face(session, base_url, "happy")
            centered = center_on_face(detector, session, base_url)
            if args.save_photo:
                save_photo(session, base_url, args.save_photo)
            detector.close()
            result = {"found": True, "search": False,
                      "confidence": round(conf, 2),
                      "centered": centered}
            if has_recognition:
                result["similarity"] = round(sim, 3)
            output(result)
            return 0
    except Exception:
        pass

    # Search through positions
    set_face(session, base_url, "doubt")
    for yaw, pitch in SEARCH_POSITIONS:
        move_servo(session, base_url, yaw, pitch, speed=400)
        time.sleep(0.6)
        try:
            det, sim = confirm_detection(detector, session, base_url)
            if det is not None:
                conf = det.categories[0].score
                set_face(session, base_url, "happy")
                centered = center_on_face(detector, session, base_url)
                if args.save_photo:
                    save_photo(session, base_url, args.save_photo)
                detector.close()
                result = {"found": True, "search": True,
                          "position": [yaw, pitch],
                          "confidence": round(conf, 2),
                          "centered": centered}
                if has_recognition:
                    result["similarity"] = round(sim, 3)
                output(result)
                return 0
        except Exception:
            continue

    # Not found
    set_face(session, base_url, "sleepy")
    move_servo(session, base_url, 0, 200, speed=300)
    detector.close()
    output({"found": False})
    return 1


if __name__ == "__main__":
    sys.exit(main())
