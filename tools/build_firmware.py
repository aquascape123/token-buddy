"""
Compila il firmware di Token Buddy per ogni modello di CYD e prepara i file
per la pagina di installazione (docs/).

La configurazione dello schermo (TFT_eSPI) passa al compilatore con -D, cosi'
non serve toccare User_Setup.h della libreria.

Uso:
  python tools/build_firmware.py                 compila tutti i modelli
  python tools/build_firmware.py cyd24 COM11     compila il 2.4" e lo carica su COM11
"""
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKETCH = os.path.join(ROOT, "firmware", "TokenBuddy")
DOCS = os.path.join(ROOT, "docs")
FQBN = "esp32:esp32:esp32"
CLI = shutil.which("arduino-cli") or r"C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
VERSION = "1.1.0"

COMMON = [
    "USER_SETUP_LOADED", "TFT_WIDTH=240", "TFT_HEIGHT=320",
    "TFT_MISO=12", "TFT_MOSI=13", "TFT_SCLK=14", "TFT_CS=15", "TFT_DC=2", "TFT_RST=-1",
    "TFT_BACKLIGHT_ON=HIGH", "USE_HSPI_PORT",
    "LOAD_GLCD", "LOAD_FONT2", "LOAD_FONT4", "LOAD_FONT6", "LOAD_FONT7", "LOAD_FONT8",
    "LOAD_GFXFF", "SMOOTH_FONT",
    "SPI_FREQUENCY=55000000", "SPI_READ_FREQUENCY=20000000", "SPI_TOUCH_FREQUENCY=2500000",
]
BOARDS = {
    "cyd28": {"name": 'CYD 2.8" (ESP32-2432S028R)',
              "defines": ["ILI9341_2_DRIVER", "TFT_INVERSION_ON", "TFT_BL=21"]},
    "cyd24": {"name": 'CYD 2.4" (ESP32-2432S024R)',
              "defines": ["ST7789_DRIVER", "TFT_RGB_ORDER=TFT_BGR", "TFT_INVERSION_OFF",
                          "TFT_BL=27", "TOKENBUDDY_CYD24"]},
}
BOOT_APP0 = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Arduino15", "packages", "esp32",
                         "hardware", "esp32", "2.0.14", "tools", "partitions", "boot_app0.bin")


def build(board, port=None):
    cfg = BOARDS[board]
    flags = " ".join("-D" + d for d in COMMON + cfg["defines"])
    out = os.path.join(ROOT, "build", board)
    cmd = [CLI, "compile", "-b", FQBN, "--build-path", os.path.join(out, "cache"),
           "--output-dir", out,
           "--build-property", "compiler.cpp.extra_flags=" + flags,
           "--build-property", "compiler.c.extra_flags=" + flags, SKETCH]
    if port:
        cmd[2:2] = ["-u", "-p", port]
    print(f"== {cfg['name']}")
    subprocess.run(cmd, check=True)

    dest = os.path.join(DOCS, "firmware", board)
    os.makedirs(dest, exist_ok=True)
    shutil.copy2(os.path.join(out, "TokenBuddy.ino.bootloader.bin"), os.path.join(dest, "bootloader.bin"))
    shutil.copy2(os.path.join(out, "TokenBuddy.ino.partitions.bin"), os.path.join(dest, "partitions.bin"))
    shutil.copy2(os.path.join(out, "TokenBuddy.ino.bin"), os.path.join(dest, "firmware.bin"))
    shutil.copy2(BOOT_APP0, os.path.join(dest, "boot_app0.bin"))
    manifest = {
        "name": f"Token Buddy - {cfg['name']}",
        "version": VERSION,
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": "ESP32", "parts": [
            {"path": f"firmware/{board}/bootloader.bin", "offset": 4096},
            {"path": f"firmware/{board}/partitions.bin", "offset": 32768},
            {"path": f"firmware/{board}/boot_app0.bin", "offset": 57344},
            {"path": f"firmware/{board}/firmware.bin", "offset": 65536},
        ]}],
    }
    with open(os.path.join(DOCS, f"manifest_{board}.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)


if __name__ == "__main__":
    if len(sys.argv) > 1:
        build(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
    else:
        for b in BOARDS:
            build(b)
