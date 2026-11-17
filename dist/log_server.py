from datetime import datetime, timezone
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import json

ROOT = Path(__file__).resolve().parent
LOGS = ROOT / "logs"
LOGS.mkdir(exist_ok=True)
MAX_BYTES = 2 * 1024 * 1024

class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT), **kwargs)

    def do_POST(self):
        if self.path != "/upload":
            self.send_error(404)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self.send_error(400, "Invalid Content-Length")
            return
        if length <= 0 or length > MAX_BYTES:
            self.send_error(413, "Log must be between 1 byte and 2 MiB")
            return
        body = self.rfile.read(length)
        try:
            text = body.decode("utf-8-sig")
        except UnicodeDecodeError:
            self.send_error(400, "Log must be UTF-8 text")
            return
        if not text.strip():
            self.send_error(400, "Log is empty")
            return
        timestamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f")
        name = f"ytm32-log-{timestamp}.txt"
        target = LOGS / name
        target.write_text(text, encoding="utf-8", newline="")
        payload = json.dumps({"ok": True, "file": name}, ensure_ascii=False).encode("utf-8")
        self.send_response(201)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)
        print(f"Saved uploaded log: {target}", flush=True)

    def log_message(self, fmt, *args):
        # Keep request paths and uploaded log contents out of console logs.
        if self.command == "POST":
            print(f"{self.client_address[0]} uploaded a log", flush=True)
        else:
            super().log_message(fmt, *args)

if __name__ == "__main__":
    server = ThreadingHTTPServer(("0.0.0.0", 8000), Handler)
    print("Ytm32 log server listening on port 8000", flush=True)
    server.serve_forever()
