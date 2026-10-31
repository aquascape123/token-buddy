"""
Token Buddy - bridge PC -> ESP32 CYD per i limiti di utilizzo Claude.

Legge il token OAuth di Claude Code (~/.claude/.credentials.json), interroga
l'endpoint di utilizzo di Anthropic e serve un JSON compatto in LAN:

    GET http://<ip-pc>:8765/usage
    {"ok":true,"s_pct":12.0,"s_reset":1730300000,"w_pct":40.0,"w_reset":...,
     "now":1730290000,"plan":"max","cc":"working","cc_seq":7,"cc_n":1}

Riceve anche gli eventi degli hook di Claude Code (claude_hook.py):

    POST http://127.0.0.1:8765/hook   <- JSON dell'hook

e ne ricava lo stato "cc": idle, working, done, waiting.

"typing": true se stai scrivendo nella finestra di Claude (app desktop o
claude.ai nel browser). Si guarda solo SE e' stato premuto un tasto, mai
QUALE: e' l'ultimo istante di input che Windows usa per lo screensaver.

Solo libreria standard. Avvio:  python claude_usage_bridge.py
"""
import json
import os
import shutil
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = 8765
POLL_SECONDS = 60
CRED_PATH = os.path.join(os.path.expanduser("~"), ".claude", ".credentials.json")

USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
TOKEN_URLS = [
    "https://console.anthropic.com/v1/oauth/token",
    "https://platform.claude.com/v1/oauth/token",
]
CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e"  # client OAuth pubblico di Claude Code
UA = "claude-code/2.0.0"

state = {"ok": False, "err": "starting..."}
lock = threading.Lock()

# --- stato delle sessioni Claude Code (dagli hook) ---
HOOK_STATE = {
    "UserPromptSubmit": "working",
    "PreToolUse": "working",
    "PostToolUse": "working",
    "Stop": "done",
    "Notification": "waiting",
}
# dopo quanti secondi senza eventi uno stato decade a idle
EXPIRE = {"working": 600, "done": 60, "waiting": 1800}
PRIORITY = {"idle": 0, "done": 1, "working": 2, "waiting": 3}

sessions = {}          # session_id -> (stato, istante)
cc_seq = 0             # cresce a ogni cambiamento che il display deve notare
cc_last = "idle"

# --- "stai scrivendo" nella finestra di Claude ---
TYPING_HOLD = 3.0      # secondi di "typing" dopo l'ultimo tasto
BROWSERS = {"chrome.exe", "msedge.exe", "firefox.exe", "brave.exe", "opera.exe", "vivaldi.exe"}
typing_until = 0.0
key_events = 0         # quante pressioni (giu' e su) sono state viste: solo il numero


def log(msg):
    print(f"[{datetime.now():%H:%M:%S}] {msg}", flush=True)


def http_json(url, method="GET", headers=None, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("User-Agent", UA)
    req.add_header("Accept", "application/json")
    if data is not None:
        req.add_header("Content-Type", "application/json")
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.loads(r.read().decode())


def load_creds():
    with open(CRED_PATH, encoding="utf-8") as f:
        return json.load(f)


def save_creds(creds):
    # backup una tantum, poi scrittura atomica
    bak = CRED_PATH + ".bak"
    if not os.path.exists(bak):
        shutil.copy2(CRED_PATH, bak)
    tmp = CRED_PATH + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(creds, f)
    os.replace(tmp, CRED_PATH)


def refresh_token(creds):
    o = creds["claudeAiOauth"]
    last_err = None
    for url in TOKEN_URLS:
        try:
            r = http_json(url, "POST", body={
                "grant_type": "refresh_token",
                "refresh_token": o["refreshToken"],
                "client_id": CLIENT_ID,
            })
            o["accessToken"] = r["access_token"]
            if r.get("refresh_token"):
                o["refreshToken"] = r["refresh_token"]
            o["expiresAt"] = int((time.time() + r.get("expires_in", 3600)) * 1000)
            save_creds(creds)
            log("token rinnovato")
            return
        except Exception as e:  # prova l'URL successivo
            last_err = e
    raise RuntimeError(f"token refresh failed: {last_err}")


def get_access_token():
    creds = load_creds()
    o = creds["claudeAiOauth"]
    if o.get("expiresAt", 0) / 1000 < time.time() + 120:
        refresh_token(creds)
    return creds["claudeAiOauth"]["accessToken"], o.get("subscriptionType", "")


def iso_to_epoch(s):
    if not s:
        return 0
    return int(datetime.fromisoformat(s.replace("Z", "+00:00")).timestamp())


def fetch_usage():
    token, plan = get_access_token()
    hdr = {"Authorization": f"Bearer {token}", "anthropic-beta": "oauth-2025-04-20"}
    try:
        u = http_json(USAGE_URL, headers=hdr)
    except urllib.error.HTTPError as e:
        if e.code != 401:
            raise
        creds = load_creds()
        refresh_token(creds)
        hdr["Authorization"] = f"Bearer {creds['claudeAiOauth']['accessToken']}"
        u = http_json(USAGE_URL, headers=hdr)

    five = u.get("five_hour") or {}
    week = u.get("seven_day") or {}
    return {
        "ok": True,
        "s_pct": float(five.get("utilization") or 0),
        "s_reset": iso_to_epoch(five.get("resets_at")),
        "w_pct": float(week.get("utilization") or 0),
        "w_reset": iso_to_epoch(week.get("resets_at")),
        "plan": plan,
    }


def poller():
    global state
    while True:
        try:
            s = fetch_usage()
            log(f"sessione {s['s_pct']:.0f}%  settimana {s['w_pct']:.0f}%")
        except Exception as e:
            s = {"ok": False, "err": str(e)[:80]}
            log(f"errore: {e}")
        with lock:
            state = s
        time.sleep(POLL_SECONDS)


def claude_in_foreground(user32, kernel32):
    """True se la finestra attiva e' l'app Claude o claude.ai in un browser."""
    import ctypes
    from ctypes import wintypes
    hwnd = user32.GetForegroundWindow()
    if not hwnd:
        return False
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    exe = ""
    h = kernel32.OpenProcess(0x1000, False, pid.value)   # QUERY_LIMITED_INFORMATION
    if h:
        buf = ctypes.create_unicode_buffer(260)
        size = wintypes.DWORD(260)
        if kernel32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
            exe = os.path.basename(buf.value).lower()
        kernel32.CloseHandle(h)
    if exe == "claude.exe":
        return True
    if exe in BROWSERS:
        title = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(hwnd, title, 256)
        return "claude" in title.value.lower()
    return False


def typing_watcher():
    """Un input senza movimento del mouse, con Claude in primo piano = sta scrivendo."""
    global typing_until, key_events
    import ctypes
    from ctypes import wintypes

    class LASTINPUTINFO(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.UINT), ("dwTime", wintypes.DWORD)]

    user32, kernel32 = ctypes.WinDLL("user32"), ctypes.WinDLL("kernel32")
    # tipi espliciti: su Windows a 64 bit handle e finestre non stanno in un int
    user32.GetForegroundWindow.restype = wintypes.HWND
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.QueryFullProcessImageNameW.argtypes = [
        wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    lii = LASTINPUTINFO(ctypes.sizeof(LASTINPUTINFO), 0)
    pt = wintypes.POINT()
    last_input, last_pos = None, None
    while True:
        try:
            user32.GetLastInputInfo(ctypes.byref(lii))
            user32.GetCursorPos(ctypes.byref(pt))
            pos = (pt.x, pt.y)
            if last_input is not None and lii.dwTime != last_input and pos == last_pos \
                    and claude_in_foreground(user32, kernel32):
                if time.time() > typing_until:
                    log("stai scrivendo a Claude")
                typing_until = time.time() + TYPING_HOLD
                key_events += 1
            last_input, last_pos = lii.dwTime, pos
        except Exception as e:
            log(f"rilevamento scrittura non disponibile: {e}")
            return
        time.sleep(0.04)   # abbastanza spesso da vedere quasi ogni tasto


def on_hook(ev):
    global cc_seq
    name = ev.get("hook_event_name", "")
    sid = ev.get("session_id", "?")
    with lock:
        if name == "SessionEnd":
            sessions.pop(sid, None)
        elif name in HOOK_STATE:
            st = HOOK_STATE[name]
            prev = sessions.get(sid, ("idle", 0))[0]
            sessions[sid] = (st, time.time())
            # "fatto" e "ti aspetta" vanno sempre notati, anche se gia' attivi
            if st in ("done", "waiting") and st != prev:
                cc_seq += 1
            if st != prev:
                log(f"claude code {sid[:8]}: {st}")


def cc_status():
    """Stato complessivo: vince il piu' urgente tra le sessioni attive."""
    global cc_seq, cc_last
    now = time.time()
    best, active = "idle", 0
    for sid, (st, t) in list(sessions.items()):
        if now - t > EXPIRE.get(st, 0):
            del sessions[sid]
            continue
        active += 1
        if PRIORITY[st] > PRIORITY[best]:
            best = st
    if best != cc_last:
        cc_last = best
        cc_seq += 1
    return best, active


class Handler(BaseHTTPRequestHandler):
    # keep-alive: il CYD riusa la stessa connessione invece di aprirne una ogni 2 s
    protocol_version = "HTTP/1.1"
    timeout = 30

    def do_POST(self):
        if self.path.split("?")[0] != "/hook":
            self.send_error(404)
            return
        try:
            n = int(self.headers.get("Content-Length", 0))
            on_hook(json.loads(self.rfile.read(n) or b"{}"))
        except Exception as e:
            log(f"hook non valido: {e}")
        self.send_response(204)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        if self.path.split("?")[0] not in ("/", "/usage"):
            self.send_error(404)
            return
        with lock:
            body = dict(state)
            body["cc"], body["cc_n"] = cc_status()
            body["cc_seq"] = cc_seq
        body["typing"] = time.time() < typing_until
        body["keys"] = key_events
        body["now"] = int(time.time())
        data = json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *a):
        pass


class Server(ThreadingHTTPServer):
    # su Windows il riuso permetterebbe a due bridge di condividere la porta
    allow_reuse_address = False
    # connessioni lasciate a meta' dal WiFi dell'ESP32 non devono riempire la coda
    request_queue_size = 64

    def handle_error(self, request, client_address):
        # il CYD che si riavvia chiude la connessione di colpo: non e' un errore
        import sys
        if isinstance(sys.exc_info()[1], (ConnectionError, TimeoutError)):
            return
        super().handle_error(request, client_address)


BEACON_PORT = 8766


def beacon():
    """Annuncia il bridge in rete ogni 2 s: il CYD lo trova anche se l'IP del PC cambia."""
    import socket
    msg = f"TOKENBUDDY {PORT}".encode()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    while True:
        try:
            sock.sendto(msg, ("255.255.255.255", BEACON_PORT))
        except OSError as e:
            log(f"annuncio in rete fallito: {e}")
        time.sleep(2)


if __name__ == "__main__":
    try:
        server = Server(("0.0.0.0", PORT), Handler)
    except OSError:
        log(f"porta {PORT} gia' in uso: il bridge e' gia' acceso, chiudi questa finestra")
        raise SystemExit(1)
    threading.Thread(target=poller, daemon=True).start()
    threading.Thread(target=beacon, daemon=True).start()
    if os.name == "nt":
        threading.Thread(target=typing_watcher, daemon=True).start()
    log(f"bridge in ascolto su porta {PORT}  ->  http://<ip-di-questo-pc>:{PORT}/usage")
    server.serve_forever()
