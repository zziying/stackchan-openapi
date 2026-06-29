#!/usr/bin/env python3
"""
Listens for touch events from StackChan and prints them.

StackChan firmware sends POST /touch with JSON {"event":"touch","type":"..."}
when the user interacts with the touch sensors or shakes the device.

Touch types:
  tap         — double tap on head
  pet         — swipe forward (petting)
  pet_reverse — swipe backward
  shake       — shaking the device
  screen_tap  — tap on the screen

Usage:
  python touch_listener.py                    # listen on port 7070
  python touch_listener.py --port 7070
  python touch_listener.py --command 'echo "touched: {type}"'
"""
from http.server import HTTPServer, BaseHTTPRequestHandler
import json
import subprocess
import time
import argparse
import shlex

DEBOUNCE_SECONDS = 5
_last_event = {}

REACTIONS = {
    "tap": "tapped!",
    "pet": "petted!",
    "pet_reverse": "reverse petted!",
    "shake": "shaken!",
    "screen_tap": "screen tapped!",
}

_command_template = None


class TouchHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(length)
        try:
            data = json.loads(body)
            touch_type = data.get('type', 'unknown')
            now = time.strftime('%H:%M:%S')

            t = time.monotonic()
            last = _last_event.get(touch_type, 0)
            if t - last < DEBOUNCE_SECONDS:
                print(f"[{now}] {touch_type} (debounced)", flush=True)
            else:
                _last_event[touch_type] = t
                label = REACTIONS.get(touch_type, f"unknown touch: {touch_type}")
                print(f"[{now}] {label}", flush=True)

                if _command_template:
                    cmd = _command_template.replace("{type}", touch_type)
                    subprocess.Popen(cmd, shell=True,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except Exception as e:
            print(f"[error] {e}", flush=True)

        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(b'{"ok":true}')

    def log_message(self, format, *args):
        pass


def main():
    global _command_template
    parser = argparse.ArgumentParser(description="StackChan touch event listener")
    parser.add_argument("--port", type=int, default=7070, help="Listen port (default: 7070)")
    parser.add_argument("--command", default=None,
                        help="Shell command to run on touch. {type} is replaced with touch type.")
    args = parser.parse_args()

    _command_template = args.command

    server = HTTPServer(('0.0.0.0', args.port), TouchHandler)
    print(f"Touch listener on :{args.port}", flush=True)
    print(f"  Events: {', '.join(REACTIONS.keys())}", flush=True)
    if _command_template:
        print(f"  Command: {_command_template}", flush=True)
    server.serve_forever()


if __name__ == '__main__':
    main()
