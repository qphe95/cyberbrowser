#!/usr/bin/env python3
"""
Capture YouTube API JSON responses from a real Chrome session.

Launches Chrome with the DevTools protocol, navigates to a YouTube page,
intercepts network responses, and saves JSON/API response bodies to
youtube_data/captured_responses/.  These files can then be fed to
extract_browser_apis.py to surface APIs referenced in dynamically loaded data.

Usage:
    python scripts/capture_youtube_network.py [URL]

Defaults:
    URL: https://www.youtube.com/watch?v=dQw4w9WgXcQ
"""

import base64
import json
import os
import platform
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path
from urllib.parse import urlparse


DEBUG = "--debug" in sys.argv

DEFAULT_URL = "https://www.youtube.com/watch?v=dQw4w9WgXcQ"
CDP_HOST = "127.0.0.1"
OUTPUT_DIR = Path("youtube_data") / "captured_responses"

# Will be set at runtime to an available port.
CDP_PORT = 0

# CDP request ID counter.
_next_id = 1


def find_free_port() -> int:
    """Return an available TCP port on localhost."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind((CDP_HOST, 0))
    port = s.getsockname()[1]
    s.close()
    return port


def find_chrome():
    """Return the path to Chrome/Chromium or None."""
    system = platform.system()
    candidates = []
    if system == "Windows":
        pf = os.environ.get("PROGRAMFILES", r"C:\Program Files")
        pf_x86 = os.environ.get("PROGRAMFILES(X86)", r"C:\Program Files (x86)")
        candidates = [
            os.path.join(pf, "Google", "Chrome", "Application", "chrome.exe"),
            os.path.join(pf_x86, "Google", "Chrome", "Application", "chrome.exe"),
            os.path.join(os.environ.get("LOCALAPPDATA", ""), "Google", "Chrome", "Application", "chrome.exe"),
            os.path.join(pf, "Chromium", "Application", "chromium.exe"),
        ]
    elif system == "Darwin":
        candidates = [
            "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
            "/Applications/Chromium.app/Contents/MacOS/Chromium",
        ]
    else:
        candidates = [
            "/usr/bin/google-chrome",
            "/usr/bin/chromium",
            "/usr/bin/chromium-browser",
            "/usr/bin/chrome",
        ]
    for c in candidates:
        if os.path.isfile(c):
            return c
    for name in ("google-chrome", "chromium", "chromium-browser", "chrome"):
        for ext in ("", ".exe"):
            for path_dir in os.environ.get("PATH", "").split(os.pathsep):
                full = os.path.join(path_dir, name + ext)
                if os.path.isfile(full):
                    return full
    return None


def get_ws_url():
    """Fetch the WebSocket debugger URL from Chrome's HTTP endpoint."""
    url = f"http://{CDP_HOST}:{CDP_PORT}/json/list"
    with urllib.request.urlopen(url, timeout=10) as resp:
        data = json.loads(resp.read().decode('utf-8'))
    # Prefer the page target, fall back to the first target with a ws URL.
    for entry in data:
        if entry.get('type') == 'page' and 'webSocketDebuggerUrl' in entry:
            return entry['webSocketDebuggerUrl']
    for entry in data:
        if 'webSocketDebuggerUrl' in entry:
            return entry['webSocketDebuggerUrl']
    raise RuntimeError("No WebSocket debugger URL found in Chrome's /json/list")


class SimpleWebSocket:
    """A minimal RFC 6455 WebSocket client using only the Python stdlib."""

    OPCODE_TEXT = 0x1
    OPCODE_BINARY = 0x2
    OPCODE_CLOSE = 0x8
    OPCODE_PING = 0x9
    OPCODE_PONG = 0xA

    def __init__(self, ws_url: str):
        parsed = urlparse(ws_url)
        host = parsed.hostname
        port = parsed.port or 80
        self.sock = socket.create_connection((host, port), timeout=30)
        self._handshake(ws_url, host, port)

    def _handshake(self, ws_url, host, port):
        key = base64.b64encode(os.urandom(16)).decode('ascii')
        path = urlparse(ws_url).path or '/'
        req = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n"
            f"\r\n"
        )
        self.sock.sendall(req.encode('ascii'))
        # Read response headers.
        response = b""
        while b"\r\n\r\n" not in response:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("WebSocket handshake failed")
            response += chunk
        header, _ = response.split(b"\r\n\r\n", 1)
        first_line = header.split(b"\r\n", 1)[0].decode('ascii')
        if "101" not in first_line:
            raise ConnectionError(f"WebSocket handshake failed: {first_line}")

    def _send_frame(self, opcode: int, payload: bytes):
        """Send a masked frame (client -> server)."""
        length = len(payload)
        header = bytearray()
        header.append(0x80 | opcode)  # FIN=1, opcode
        if length < 126:
            header.append(0x80 | length)
        elif length < 65536:
            header.append(0x80 | 126)
            header.extend(struct.pack('>H', length))
        else:
            header.append(0x80 | 127)
            header.extend(struct.pack('>Q', length))
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(bytes(header) + mask + masked)

    def send_text(self, text: str):
        self._send_frame(self.OPCODE_TEXT, text.encode('utf-8'))

    def _recv_exact(self, n: int) -> bytes:
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise ConnectionError("WebSocket connection closed unexpectedly")
            data += chunk
        return data

    def recv_frame(self) -> tuple[int, bytes]:
        """Receive one frame; return (opcode, payload)."""
        header = self._recv_exact(2)
        byte0 = header[0]
        byte1 = header[1]
        opcode = byte0 & 0x0F
        masked = bool(byte1 & 0x80)
        length = byte1 & 0x7F
        if length == 126:
            length = struct.unpack('>H', self._recv_exact(2))[0]
        elif length == 127:
            length = struct.unpack('>Q', self._recv_exact(8))[0]
        if masked:
            mask = self._recv_exact(4)
            payload = self._recv_exact(length)
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        else:
            payload = self._recv_exact(length)
        return opcode, payload

    def recv_text(self) -> str:
        """Receive a text frame (ignoring pings/pongs/close)."""
        while True:
            opcode, payload = self.recv_frame()
            if opcode == self.OPCODE_TEXT:
                return payload.decode('utf-8', errors='replace')
            elif opcode == self.OPCODE_PING:
                self._send_frame(self.OPCODE_PONG, payload)
            elif opcode == self.OPCODE_CLOSE:
                raise ConnectionError("WebSocket closed by peer")

    def close(self):
        try:
            self._send_frame(self.OPCODE_CLOSE, b"")
        except Exception:
            pass
        self.sock.close()


def next_id():
    global _next_id
    cid = _next_id
    _next_id += 1
    return cid


def send_command(ws, method: str, params: dict | None = None):
    cmd = {"id": next_id(), "method": method}
    if params:
        cmd["params"] = params
    ws.send_text(json.dumps(cmd))
    return cmd["id"]


def looks_like_json_response(response: dict) -> bool:
    """Heuristic: is this response worth saving?"""
    mime = response.get('mimeType', '').lower()
    url = response.get('url', '')
    if 'json' in mime:
        return True
    if 'application/protobuf' in mime or 'application/x-protobuf' in mime:
        return False
    if url.endswith('.json'):
        return True
    # youtubei/v1 and similar API endpoints usually return JSON.
    if '/youtubei/' in url or 'pbj=1' in url or 'prettyPrint=false' in url:
        return True
    return False


def sanitize_filename(url: str) -> str:
    """Create a filesystem-safe filename from a URL."""
    parsed = urlparse(url)
    base = parsed.netloc + parsed.path
    base = re.sub(r'[^a-zA-Z0-9_\-./]', '_', base)
    base = base.strip('_').replace('/', '_')[:120]
    return base or "unknown"


def wait_for_chrome(timeout: float = 20.0):
    """Poll Chrome's HTTP endpoint until it's ready."""
    url = f"http://{CDP_HOST}:{CDP_PORT}/json/version"
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(url, timeout=2) as resp:
                if resp.status == 200:
                    return
        except Exception:
            time.sleep(0.25)
    raise RuntimeError(f"Chrome did not become ready on port {CDP_PORT}")


def main():
    global CDP_PORT
    url = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else DEFAULT_URL
    CDP_PORT = find_free_port()
    chrome = find_chrome()
    if not chrome:
        print("ERROR: Could not find Google Chrome/Chromium.", file=sys.stderr)
        sys.exit(1)
    print(f"Using Chrome: {chrome}")

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    user_data_dir = tempfile.mkdtemp(prefix="cb_chrome_capture_")

    chrome_proc = None
    try:
        user_agent = (
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
            "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36"
        )
        cmd = [
            chrome,
            f"--remote-debugging-port={CDP_PORT}",
            f"--user-data-dir={user_data_dir}",
            "--headless=new",
            "--disable-gpu",
            "--no-sandbox",
            "--no-first-run",
            "--no-default-browser-check",
            "--disable-background-networking",
            "--disable-background-timer-throttling",
            "--disable-renderer-backgrounding",
            "--disable-dev-shm-usage",
            "--force-dark-mode",
            f"--user-agent={user_agent}",
            "--window-size=1280,720",
            "--disable-blink-features=AutomationControlled",
            "--enable-features=NetworkService,NetworkServiceInProcess",
        ]
        print("Starting Chrome...")
        chrome_proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        wait_for_chrome(timeout=20.0)
        ws_url = get_ws_url()
        print(f"Connected to CDP: {ws_url}")

        ws = SimpleWebSocket(ws_url)

        # Enable required CDP domains.
        send_command(ws, "Network.enable", {
            "maxTotalBufferSize": 100 * 1024 * 1024,
            "maxResourceBufferSize": 50 * 1024 * 1024,
        })
        send_command(ws, "Page.enable")

        captured_request_ids = set()
        pending_request_ids = {}  # requestId -> response metadata
        saved_count = 0
        page_loaded = False
        last_activity = time.time()

        # Start navigation.
        print(f"Navigating to {url} ...")
        send_command(ws, "Page.navigate", {"url": url})

        # Read events until we're idle for a while after load.
        idle_timeout = 5.0
        overall_timeout = 90.0
        start_time = time.time()

        while time.time() - start_time < overall_timeout:
            try:
                msg = ws.recv_text()
            except socket.timeout:
                if page_loaded and time.time() - last_activity > idle_timeout:
                    break
                continue

            try:
                event = json.loads(msg)
            except json.JSONDecodeError:
                continue

            # Handle response bodies for completed requests.
            method = event.get('method')
            if method == "Network.responseReceived":
                params = event.get('params', {})
                response = params.get('response', {})
                request_id = params.get('requestId')
                if DEBUG:
                    url = response.get('url', '')
                    mime = response.get('mimeType', '')
                    status = response.get('status', 0)
                    print(f"  [RESP] {status} {mime:<40} {url[:100]}")
                if request_id and request_id not in captured_request_ids and looks_like_json_response(response):
                    captured_request_ids.add(request_id)
                    pending_request_ids[request_id] = response
                    last_activity = time.time()

            elif method == "Network.loadingFinished":
                request_id = event.get('params', {}).get('requestId')
                if request_id in pending_request_ids:
                    response = pending_request_ids.pop(request_id)
                    try:
                        get_body_id = send_command(ws, "Network.getResponseBody", {"requestId": request_id})
                        # We'll match the response by id; store pending.
                        pending_request_ids[get_body_id] = response
                    except Exception as e:
                        print(f"  getResponseBody failed for {request_id}: {e}")
                    last_activity = time.time()

            elif method == "Page.loadEventFired":
                page_loaded = True
                print("Page load event fired, waiting for network idle...")
                last_activity = time.time()

            elif 'id' in event:
                if 'error' in event:
                    # Clean up any pending metadata for failed commands.
                    pending_request_ids.pop(event['id'], None)
                    continue
                result = event.get('result', {})
                # Match getResponseBody responses.
                if 'body' in result:
                    body = result['body']
                    if result.get('base64Encoded'):
                        body = base64.b64decode(body).decode('utf-8', errors='ignore')
                    # Find the original response metadata by command id.
                    response_meta = pending_request_ids.pop(event['id'], None)
                    if response_meta:
                        response_url = response_meta.get('url', 'unknown')
                        fname = sanitize_filename(response_url) + f"_{saved_count:04d}.json"
                        out_path = OUTPUT_DIR / fname
                        try:
                            # Pretty-print if valid JSON, else raw.
                            parsed = json.loads(body)
                            out_path.write_text(json.dumps(parsed, ensure_ascii=False, indent=2), encoding='utf-8')
                        except json.JSONDecodeError:
                            out_path.write_text(body, encoding='utf-8', errors='ignore')
                        print(f"  Saved {out_path.name} ({len(body)} bytes)")
                        saved_count += 1
                        last_activity = time.time()

            if page_loaded and time.time() - last_activity > idle_timeout:
                print("Network idle, finishing capture.")
                break

        ws.close()
        print(f"Capture complete. Saved {saved_count} responses to {OUTPUT_DIR}")

    finally:
        if chrome_proc:
            try:
                chrome_proc.terminate()
                chrome_proc.wait(timeout=10)
            except Exception:
                chrome_proc.kill()
        shutil.rmtree(user_data_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
