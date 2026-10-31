# Token Buddy

A pixel desk robot for the **ESP32 CYD 2.8"** (ESP32-2432S028R) and **2.4"** (ESP32-2432S024R) that shows your **Claude Code usage limits**, reacts while Claude works and while you type.

**Install from the browser:** https://aquascape123.github.io/token-buddy/

> Unofficial fan project, not affiliated with or endorsed by Anthropic. The usage numbers come from the same undocumented endpoint Claude Code uses for `/usage`: it may change and stop working without notice.

## What it does

| When | On screen |
|---|---|
| Claude is idle | a pixel face sleeping (breathing, snoring, Z's) with the clock |
| You type in the Claude window | a pixel keyboard: keys light up as you type, little hands press them |
| Claude is working | two ring gauges: 5-hour session and weekly limit, with reset countdowns |
| Claude needs you (permission / question) | eyes bounce, "Claude needs you!", LED blinks yellow |
| Claude is done | big smile, "Done!", LED blinks green |
| You reach 80% or 95% of a limit | red banner and red LED; tap to dismiss |
| A limit resets | green banner "Session available again!" |

- **Themes:** 6 ready-made color themes plus a custom one, chosen from a web page served by the CYD (`http://tokenbuddy.local`) or by holding the screen.
- **Touch:** tap to peek at your limits while it sleeps, hold to switch theme.
- **Sound (optional):** key clicks and a startup chime if you plug an 8 Ω speaker into the `SPEAK` connector.
- **Case:** a 3D-printable pixel robot, one piece plus a snap-on grid cover, no screws, no supports (~35 g PLA).

## What you need

- ESP32 CYD 2.8" (ESP32-2432S028R) or 2.4" (ESP32-2432S024R), with USB cable
- A Windows PC with [Claude Code](https://docs.claude.com/en/docs/claude-code) logged in, and [Python 3](https://www.python.org/downloads/)
- PC and CYD on the same WiFi network

## Install

1. **CYD:** open the [installer page](https://aquascape123.github.io/token-buddy/) in Chrome or Edge, plug in the CYD and click the button for your model (2.8" or 2.4").
2. **WiFi:** join the `TokenBuddy-Setup` network with your phone and choose your WiFi.
3. **PC:** download this repository, open `bridge` and double-click `install_windows.bat`. Allow private-network access if the firewall asks.

The CYD finds the bridge automatically on your network.

To remove the bridge: run `install_windows.bat --uninstall` from a terminal in the `bridge` folder.

## How it works

```
Claude Code ──hooks (curl)──► bridge on your PC ◄──── CYD (polls every 0.5 s over WiFi)
                                   │
                    usage endpoint (Anthropic)
```

- The **bridge** (`bridge/token_buddy_bridge.py`, Python standard library only):
  - reads the Claude Code login on your PC and asks for your usage once a minute; your token never leaves the PC;
  - receives Claude Code hook events (working / done / waiting);
  - detects when you type in the Claude window (only *that* a key was pressed, never *which* key);
  - announces itself on the LAN so the CYD finds it even when the PC's IP changes.
- The **firmware** (`firmware/TokenBuddy`) draws everything, stores your theme and serves the theme page.

## Build the firmware yourself

Needs [arduino-cli](https://arduino.github.io/arduino-cli/) (or the Arduino IDE) with the ESP32 core 2.0.x and these libraries: **TFT_eSPI**, **ArduinoJson 7**, **WiFiManager**, **XPT2046_Touchscreen**.

```
python tools/build_firmware.py              # builds both boards into docs/
python tools/build_firmware.py cyd24 COM5   # builds the 2.4" and uploads it
```

The display settings for each board are passed to the compiler, so you don't need to edit TFT_eSPI's `User_Setup.h`. `firmware/TokenBuddy/User_Setup.h` is the 2.8" setup, kept for building from the Arduino IDE.

## Troubleshooting

- **"Looking for the bridge..."** – is the bridge running? Is the PC on the same network? Allow Python through the Windows firewall on private networks.
- **Black screen, LED on** – you installed the firmware for the other model: install the 2.4" one on a 2.4" board and vice versa.
- **Upside-down screen** – change `setRotation(1)` to `setRotation(3)`.

## License

MIT – see [LICENSE.md](LICENSE.md).

---

# Token Buddy (italiano)

Un robottino pixel da scrivania per il **CYD 2.8"** e **2.4"** che mostra i tuoi **limiti di utilizzo di Claude Code**, reagisce quando Claude lavora e mentre scrivi.

**Installazione dal browser:** https://aquascape123.github.io/token-buddy/

1. **CYD:** apri la pagina di installazione con Chrome o Edge, collega il CYD via USB e clicca il pulsante del tuo modello (2.8" o 2.4").
2. **WiFi:** col telefono collegati alla rete `TokenBuddy-Setup` e scegli il tuo WiFi.
3. **PC:** scarica questo repository, apri la cartella `bridge` e fai doppio clic su `install_windows.bat` (serve Python 3 e Claude Code con login fatto).

Temi colore su `http://tokenbuddy.local`; tieni premuto lo schermo per cambiare tema, toccalo per vedere i limiti mentre dorme.

> Progetto non ufficiale, non affiliato ad Anthropic. I dati di utilizzo arrivano da un servizio non documentato che potrebbe cambiare.
