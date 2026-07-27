# TARCZOWNIX Motor Control System

An ESP32-based controller for three pop-up shooting targets. Each target is driven by a
pair of relays (show / hide) and reports its position through a pair of limit switches.
Configuration and monitoring happen over a self-hosted WiFi access point.

## Features

### Core Functionality
- **3 Targets**: 6 relays organised as 3 show/hide pairs (0↔1, 2↔3, 4↔5)
- **Mutual Exclusion**: energising one relay of a pair forces its partner off, in software
- **Deadtime**: a short gap between dropping one relay and pulling the other
- **Configurable Timeout**: a target that does not reach its limit switch in time latches `ERROR`
- **Persistent Configuration**: settings stored as `/config.json` on LittleFS
- **Modes**: `manual`, `sequence` (randomised training run), `competition` (gunshot-armed timed run)

### Web Interface
Served from LittleFS, updated over a WebSocket at `/ws`:
- Live target states, mode, and competition state
- Show / hide / stop / reset per target, plus global arm / stop / reset
- Settings form bound to `/api/settings`
- Rolling in-memory log and diagnostics view

### Target State Machine
`HIDDEN → MOVING_SHOW → SHOWN → MOVING_HIDE → HIDDEN`, plus:
- `STOPPED` — halted between the two limit switches
- `ERROR` — movement timed out; latched until an explicit `reset`

## API

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/` | Web interface (static files from LittleFS) |
| `GET` | `/api/settings` | Current configuration as JSON |
| `POST` | `/api/settings` | Update configuration; returns the clamped values actually stored |
| `GET` | `/api/diagnostics` | Uptime, mode, relay shadow register, raw/debounced inputs, per-target state |
| `GET` | `/api/logs` | In-memory ring buffer of log lines |
| `POST` | `/api/logs/clear` | Clear the log buffer |
| `GET` | `/mic-status` | Microphone pin, threshold, baseline, last value and peak |
| `WS` | `/ws` | Commands in, telemetry out |

Configuration keys, with the ranges the firmware clamps to:

| Key | Range | Default |
| --- | --- | --- |
| `micThreshold` | 100 – 4095 | 2000 |
| `t1Delay`, `t2Delay`, `t3Delay` | 0 – 60000 ms | 0 / 1000 / 2000 |
| `t1Duration`, `t2Duration`, `t3Duration` | 100 – 60000 ms | 2000 |
| `targetTimeoutMs` | 500 – 120000 ms | 5000 |

### WebSocket protocol

Client → device: `{"target": <0-3>, "cmd": "<command>"}`. Target `0` is the broadcast
address used by the global buttons.

| Command | Effect |
| --- | --- |
| `show` / `hide` / `stop` | Per-target movement (manual mode) |
| `reset` | Clear a latched `ERROR` |
| `arm` / `start` | Re-arm the run in sequence and competition modes |
| `gunshot` | Software gunshot trigger, equivalent to a mic detection |
| `mode:manual` \| `mode:sequence` \| `mode:competition` | Switch mode |

Device → client: `{"type": "status"|"diagnostics"|"logs", "data": ...}`. Telemetry is only
sent while at least one client is connected.

## Hardware Requirements

### Components
- **ESP32 NodeMCU-32S** (main controller)
- **2x PCF8574 I2C Expanders**:
  - Address `0x22` - Input expander (limit switches)
  - Address `0x24` - Relay expander (motor control)
- **6x Relays** capable of switching the motor load (active LOW)
- **6x Limit Switches** (normally open, to GND)
- **3x Motors**
- **Analog microphone** on GPIO36 (ADC1 — ADC2 is unusable while WiFi is on)

### Wiring
```
ESP32 NodeMCU-32S:
- GPIO 4  → SDA (both PCF8574s)
- GPIO 15 → SCL (both PCF8574s)
- GPIO 36 → Microphone analog out
- 3.3V    → VCC (both PCF8574s)
- GND     → GND (both PCF8574s)

PCF8574 (0x22) - Inputs (INPUT_PULLUP, active LOW):
- P0/P1 → Target 1 shown / hidden limit switch
- P2/P3 → Target 2 shown / hidden limit switch
- P4/P5 → Target 3 shown / hidden limit switch

PCF8574 (0x24) - Relays (OUTPUT, active LOW):
- P0/P1 → Target 1 show / hide
- P2/P3 → Target 2 show / hide
- P4/P5 → Target 3 show / hide
```

## Building and Flashing

```sh
pio run                # build firmware
pio run -t upload      # flash firmware
pio run -t uploadfs    # flash the web interface from data/ to LittleFS
pio device monitor     # serial log at 115200 baud
```

`pio run -t uploadfs` is required at least once, and again after any change under `data/`.
The project sets `board_build.filesystem = littlefs`; without it PlatformIO would build a
SPIFFS image that the firmware cannot mount.

### Library Dependencies
- `xreef/PCF8574 library@^2.3.7` - I2C expander control
- `esp32async/ESPAsyncWebServer@^3.7.6` - Asynchronous web server
- `bblanchon/ArduinoJson@^7.0.4` - JSON serialisation

## Operation

### Startup
1. LittleFS is mounted (formatted automatically if blank or corrupt) and settings loaded
2. Both PCF8574 expanders are configured and brought up; all relays are driven to OFF
3. The microphone sampling task starts on core 0
4. WiFi access point **`TARCZOWNIX`** (open, no password) is created at **`192.168.4.1`**
5. A DNS server answers every query with that address, so any URL opens the interface
6. The system starts in `manual` mode

If an expander does not answer, the firmware still comes up and serves the web interface,
but refuses `show`/`hide` so nothing is energised blind. The failure appears in the log.

### Modes
- **manual** — targets respond directly to `show` / `hide` / `stop` / `reset`
- **sequence** — picks a random target, waits a randomised delay derived from that
  target's configured delay, shows it for its configured duration, hides it, repeats
- **competition** — hides all targets and waits for a gunshot; on detection each target is
  shown after its configured delay and hidden after its configured duration. The run ends
  once all three are hidden again; `arm` starts the next run

### Movement
`show`/`hide` drops the opposing relay, waits out the deadtime, then energises the
requested relay. The target stays in `MOVING_*` until its limit switch closes. If that
does not happen within `targetTimeoutMs`, both relays are dropped and the target latches
`ERROR`, which only a `reset` clears.

### Gunshot Detection
The microphone task samples GPIO36 in windows of 128 readings and compares the window's
peak excursion against a slowly tracked baseline. The baseline only adapts on quiet
windows, so a real shot does not drag the reference up with it. Detections are debounced
to 200 ms. Tune `micThreshold` while watching `peak` in `/mic-status`.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Motors don't stop | Limit switch wiring and PCF8574 at `0x22` |
| Relays don't activate | PCF8574 at `0x24`, relay wiring, and the `Relay write failed` log line |
| All inputs read active | SDA/SCL wiring and I2C pull-ups |
| Web interface is blank / "assets not installed" | Run `pio run -t uploadfs` |
| Target stuck in `ERROR` | Increase `targetTimeoutMs`, then press `RESET` |
| Gunshot never detected | Lower `micThreshold`; compare against `peak` in `/mic-status` |

Serial output at 115200 baud carries the same log lines as `/api/logs`.

## Possible Future Work
- OTA firmware updates
- Hardware watchdog
- MQTT integration and cycle/timing analytics
- Access control on the web interface

## License
This project is provided as-is for educational and industrial automation purposes.
