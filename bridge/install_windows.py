"""
Token Buddy - installatore del bridge per Windows.

Cosa fa:
  1. copia il bridge in %LOCALAPPDATA%\\TokenBuddy
  2. aggiunge gli hook di Claude Code in ~/.claude/settings.json
     (con backup, senza toccare le altre impostazioni, senza duplicati)
  3. lo fa partire da solo all'accensione di Windows (cartella Esecuzione automatica)
  4. lo avvia subito, in background

Uso:  python install_windows.py            installa
      python install_windows.py --uninstall  rimuove tutto
"""
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
APP_DIR = os.path.join(os.environ.get("LOCALAPPDATA", os.path.expanduser("~")), "TokenBuddy")
SETTINGS = os.path.join(os.path.expanduser("~"), ".claude", "settings.json")
STARTUP = os.path.join(os.environ.get("APPDATA", ""), r"Microsoft\Windows\Start Menu\Programs\Startup")
LAUNCHER = os.path.join(STARTUP, "TokenBuddy.vbs")

HOOK_CMD = ('curl -s -m 2 -X POST -H "Content-Type: application/json" '
            '--data-binary @- http://127.0.0.1:8765/hook || exit 0')
HOOK_EVENTS = {"UserPromptSubmit": None, "PreToolUse": "*", "PostToolUse": "*",
               "Stop": None, "Notification": None, "SessionEnd": None}


def is_ours(entry):
    return any("127.0.0.1:8765/hook" in h.get("command", "") or "token_buddy" in h.get("command", "")
               for h in entry.get("hooks", []))


def load_settings():
    if not os.path.exists(SETTINGS):
        return {}
    with open(SETTINGS, encoding="utf-8") as f:
        return json.load(f)


def save_settings(data):
    os.makedirs(os.path.dirname(SETTINGS), exist_ok=True)
    if os.path.exists(SETTINGS) and not os.path.exists(SETTINGS + ".tokenbuddy.bak"):
        shutil.copy2(SETTINGS, SETTINGS + ".tokenbuddy.bak")
    tmp = SETTINGS + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    os.replace(tmp, SETTINGS)


def install_hooks():
    data = load_settings()
    hooks = data.setdefault("hooks", {})
    for event, matcher in HOOK_EVENTS.items():
        entries = [e for e in hooks.get(event, []) if not is_ours(e)]
        entry = {"hooks": [{"type": "command", "command": HOOK_CMD, "timeout": 5}]}
        if matcher:
            entry = {"matcher": matcher, **entry}
        entries.append(entry)
        hooks[event] = entries
    save_settings(data)


def remove_hooks():
    data = load_settings()
    hooks = data.get("hooks", {})
    for event in list(hooks):
        hooks[event] = [e for e in hooks[event] if not is_ours(e)]
        if not hooks[event]:
            del hooks[event]
    if not hooks:
        data.pop("hooks", None)
    save_settings(data)


def pythonw():
    exe = os.path.join(os.path.dirname(sys.executable), "pythonw.exe")
    return exe if os.path.exists(exe) else sys.executable


def install():
    cred = os.path.join(os.path.expanduser("~"), ".claude", ".credentials.json")
    if not os.path.exists(cred):
        print("ATTENZIONE: non trovo il login di Claude Code (~/.claude/.credentials.json).")
        print("Installa Claude Code e fai il login almeno una volta, poi rilancia questo installatore.")
    os.makedirs(APP_DIR, exist_ok=True)
    bridge = os.path.join(APP_DIR, "token_buddy_bridge.py")
    shutil.copy2(os.path.join(HERE, "token_buddy_bridge.py"), bridge)
    install_hooks()
    # avvio nascosto a ogni accensione
    with open(LAUNCHER, "w", encoding="utf-8") as f:
        f.write('CreateObject("WScript.Shell").Run """%s"" ""%s""", 0, False\n' % (pythonw(), bridge))
    subprocess.Popen(["wscript", LAUNCHER])
    print("Token Buddy bridge installato e avviato.")
    print("  file:          ", APP_DIR)
    print("  hook aggiunti: ", SETTINGS)
    print("  avvio automatico:", LAUNCHER)
    print("Se Windows chiede il permesso del firewall, consenti l'accesso sulle reti private.")


def uninstall():
    subprocess.run(["taskkill", "/f", "/fi", "WINDOWTITLE eq token_buddy*"], capture_output=True)
    subprocess.run(["wmic", "process", "where", "commandline like '%token_buddy_bridge.py%'", "delete"],
                   capture_output=True)
    if os.path.exists(LAUNCHER):
        os.remove(LAUNCHER)
    remove_hooks()
    shutil.rmtree(APP_DIR, ignore_errors=True)
    print("Token Buddy bridge rimosso.")


if __name__ == "__main__":
    if os.name != "nt":
        sys.exit("Per ora il bridge e' solo per Windows.")
    uninstall() if "--uninstall" in sys.argv else install()
