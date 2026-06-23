"""Drop-in replacement for requests.Session.get() using curl subprocess.

On macOS, Python's socket module can hit Errno 65 (No route to host) when
connecting to LAN devices due to "Local Network" privacy permissions.
/usr/bin/curl is a system binary exempt from this restriction.

This module is only needed on macOS. On Linux, use requests directly.
"""
import json as _json
import subprocess
from urllib.parse import urlencode


class CurlResponse:
    def __init__(self, content, status_code):
        self.content = content
        self.status_code = status_code

    def raise_for_status(self):
        if self.status_code == 0 or self.status_code >= 400:
            raise IOError(f"HTTP {self.status_code}")

    def json(self):
        return _json.loads(self.content)


class CurlSession:
    def get(self, url, params=None, timeout=5):
        if params:
            url += "?" + urlencode(params)
        t = int(timeout) if timeout else 5
        r = subprocess.run(
            ["/usr/bin/curl", "-4", "-s", "-o", "-", "-w", "%{http_code}",
             "--connect-timeout", str(t), "--max-time", str(t + 10), url],
            capture_output=True, timeout=t + 15)
        if r.returncode != 0:
            raise IOError(r.stderr.decode().strip() or f"curl exit {r.returncode}")
        out = r.stdout
        code = int(out[-3:]) if len(out) >= 3 and out[-3:].isdigit() else 0
        return CurlResponse(out[:-3], code)
