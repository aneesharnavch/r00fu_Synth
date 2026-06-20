# r00fu_synth — Setup Guide (zero to sound)

This is the single guide a person follows to take a bare **ESP32-S3-DevKitC-1** and
the r00fu_synth hardware (8x8 button matrix, 3x analog muxes for 25 knobs + 15
sliders, a PCM5102A I2S DAC, and 5-pin DIN MIDI) and turn it into a working
**groovebox + USB-MIDI controller + standalone polyphonic synth**.

Follow the sections **in order**. Commands are copy-paste ready for Windows.

---

## 1. What this is + the libraries it leans on

r00fu_synth is firmware for a portable music workstation built on the ESP32-S3
(dual core, 240 MHz). It is five instruments in one box, selectable as **modes**:

| Mode | id | What the 8x8 grid does |
|------|----|------------------------|
| Drum        | 0 | drum trigger pads (per-button map) |
| StepSeq     | 1 | 64-step sequencer: rows select track/page/pattern, then edit steps |
| Keyboard    | 2 | notes (per-button map) |
| DAW         | 3 | clip-launch / control surface — buttons, knobs, faders all leave over USB-MIDI |
| Performance | 4 | momentary live FX (filter sweep on held pads) |

The whole point of the project is to **lean on mature open-source libraries**
instead of reinventing them. The firmware pulls these in via PlatformIO
(`platformio.ini` → `lib_deps`):

| Library | Why it is here |
|---------|----------------|
| **FortySevenEffects MIDI Library** | Parses/serializes 5-pin **DIN MIDI** on UART1. |
| **Adafruit TinyUSB** | The native USB stack — exposes a **composite USB-MIDI + USB-CDC** device so the same cable is both a DAW MIDI port and the GUI's serial console. |
| **ResponsiveAnalogRead** | Smooths the 25 knobs + 15 sliders read through the muxes (kills ADC jitter). |
| **ArduinoJson** | Reads/writes presets and settings as JSON on LittleFS. |
| **Adafruit NeoPixel** | Drives the on-board WS2812 status LED (per-mode colour + activity blink). |

The synth/effects DSP is adapted from **ML_SynthTools / AcidBox** patterns but is
self-contained, so it compiles without that library. (`platformio.ini` has a
commented `marcel-licence/ML_SynthTools` line if you ever want to swap engines.)

---

## 2. Prerequisites (Windows)

You need Python (for PlatformIO CLI and the GUI), then PlatformIO itself.

**PlatformIO CLI** — the simplest path is via pip:

```powershell
python -m pip install --upgrade platformio
pio --version
```

If `pio` is not found afterward, add Python's `Scripts` folder to your PATH, or
use the full path `%USERPROFILE%\.platformio\penv\Scripts\pio.exe`.

**PlatformIO IDE (VS Code extension)** — recommended for editing/monitoring:

1. Install **VS Code**.
2. Extensions panel (`Ctrl+Shift+X`) → search **PlatformIO IDE** → Install.
3. Reload. A PlatformIO toolbar appears at the bottom. Open this folder
   (`firmware/r00fu_synth`) and it auto-detects the project.

The CH343/native-USB drivers are built into Windows 11. (Only the *UART* port,
section 3, may pop a "CP210x / CH340" driver request — you generally won't use it.)

---

## 3. The TWO USB-C ports — plug into "USB", not "UART"

The DevKitC-1 has **two USB-C connectors side by side**:

- **UART** — goes through a USB-to-serial bridge chip. This is the classic
  "always works for flashing" port, but it is **not** a real USB device — it
  cannot be a USB-MIDI port, and it shares the bridge with our DIN MIDI UART pins.
- **USB** — the ESP32-S3's **native USB** peripheral. **Use this one.**

**Plug into the port silkscreened `USB`.** This firmware is built with
`-DARDUINO_USB_MODE=0` (TinyUSB) and `-DARDUINO_USB_CDC_ON_BOOT=1`, so the native
port enumerates as a **composite device**: a USB-MIDI port for your DAW *and* a
USB-CDC serial port for the GUI/console, simultaneously. The UART port can't do
that, and the DIN MIDI lines (`GPIO43/44`) are physically the bridge's
`U0TX/U0RX` — keeping the bridge idle avoids contention. See `config.h` for the
exact note.

**First-time flashing / "download mode".** On a fresh board the native USB device
may not be enumerated yet, so the uploader can't auto-reset it into the
bootloader. If an upload hangs at *"Connecting…"*, put the board in download mode
by hand:

1. Press and **hold `BOOT`**.
2. **Tap `RESET`** (a.k.a. `EN`) once.
3. **Release `BOOT`**.

The chip is now in ROM download mode. Run the upload again. After the first
successful flash, subsequent uploads usually auto-reset and you won't need this.

---

## 4. First-time flash

From this directory (`firmware/r00fu_synth`). Do these **in this order** — the
filesystem image first, then the firmware.

**A) Upload the LittleFS data partition (presets/settings):**

```powershell
pio run -t uploadfs
```

This builds the `data/` folder into a LittleFS image and writes it to the flash
partition reserved in `default_16MB.csv`. The `data/` folder is currently empty,
which is fine — the firmware **formats LittleFS on first boot** and writes a
factory mapping + default settings itself. Running `uploadfs` once guarantees the
partition exists and is clean. (If your module is the 8 MB "N8" variant, switch
`board_build.partitions = default_8MB.csv` in `platformio.ini` first.)

**B) Upload the firmware:**

```powershell
pio run -t upload
```

`pio run` builds first (downloading the `lib_deps` on the first run — give it a
minute), then flashes.

**Selecting the COM port.** PlatformIO auto-detects in most cases. To be explicit:

```powershell
pio device list
```

Find the port for the **native USB** device (e.g. `COM7`), then pin it in
`platformio.ini` under `[env:esp32-s3-devkitc-1]`:

```ini
upload_port  = COM7
monitor_port = COM7
```

**What success looks like.** Open the serial monitor:

```powershell
pio device monitor -b 115200
```

On boot the device prints (this is the GUI protocol, section 7):

```
HELLO r00fu_synth 0.2.0
LOG r00fu_synth boot complete
```

Pressing a physical button prints `BTN <index> <row> <col> 1` on press and
`... 0` on release. The on-board RGB LED lights in the current mode's colour
(amber = Drum at default). If a PCM5102A and headphones/amp are wired to
`BCK=40 / LRCK=41 / DOUT=42`, playing a mapped note makes sound.

---

## 5. PSRAM-off decision (and how to turn it back on)

**Why PSRAM is OFF by default.** The three analog-mux select lines `S0/S1/S2`
reuse **GPIO 35/36/37**, which on this module are the **octal-PSRAM bus**. You
can't have both. We ship with **PSRAM disabled** (`-DBOARD_HAS_PSRAM=0`) so the
wiring you already have works unchanged. The synth (8 voices + effects) fits
comfortably in internal SRAM, so nothing is lost for normal use.

**If you ever want PSRAM** (e.g. for samples/looper stretch goals), you must move
those three select lines off the PSRAM pins — a **3-jumper rewire**:

| Mux select | Move FROM | Move TO |
|------------|-----------|---------|
| S0 | GPIO 35 | **GPIO 39** |
| S1 | GPIO 36 | **GPIO 47** |
| S2 | GPIO 37 | **GPIO 48** |

(`S3` on GPIO 38 stays put. Note GPIO 48 is the status LED's default pin — relocate
the LED too, or accept the conflict, when you go down this road.)

Then update `MUX_SEL_PINS[]` in `include/config.h` to `{39, 47, 48, 38}`, and
build/flash the PSRAM environment, which re-enables octal PSRAM:

```powershell
pio run -e esp32-s3-devkitc-1-psram -t upload
```

That env (`platformio.ini`) flips `-DBOARD_HAS_PSRAM=1` and sets
`board_build.arduino.memory_type = qio_opi`.

---

## 6. How to USE it

### Configure buttons with the PC GUI

The GUI (`gui/r00fu_config.py`) lights up an on-screen 8x8 grid live as you press
keys, and lets you assign a function to each button, then push it to the device.

```powershell
python -m pip install pyserial
python ../../gui/r00fu_config.py
```

1. **Port** dropdown → pick the device's COM port → **Connect**. The status bar
   shows `Device: HELLO r00fu_synth 0.2.0`.
2. **Press a physical button** — the matching on-screen cell flashes green. This
   is how you find/verify every key. (Tick *Learn mode* to auto-select the cell
   you press.)
3. **Click a cell**, choose a **Function** (Note / CC / Program / Transport /
   Mode / None), set channel + value + label, **Apply**.
4. **Push to Device** sends the whole map over serial; the firmware acts on it
   standalone *and* persists it to LittleFS.
5. **Save…/Load…** stores the map as JSON for later.

### Use as a USB-MIDI controller in a DAW

Plug the **USB** port into your computer. The composite device exposes a
**USB-MIDI port** named for the board. In Ableton/FL/Reaper/Bitwig, enable it as a
MIDI input. Now:

- In **DAW mode**, the 64 buttons, 25 knobs, and 15 sliders all emit MIDI that
  the host can learn (knobs → CC 16.., faders → CC 0.., buttons → your mapped
  notes/CC). Knobs/faders are always sent out USB in this mode.
- In other modes, any button you mapped to a Note/CC/Program also mirrors out
  USB-MIDI, so the box doubles as a controller while it plays its own synth.

### DIN MIDI in/out

Wire 5-pin DIN through the standard opto-isolated input (6N138 / H11L1) on
`GPIO44` (**MIDI IN**) and a current-limited output on `GPIO43` (**MIDI OUT**),
both at 31250 baud on UART1. The box:

- plays incoming DIN notes on its synth and can **slave to incoming MIDI clock**,
- sequences external gear and can act as **clock master** out both ports,
- transparently bridges **USB ↔ DIN** without echo loops (a message that arrived
  on one port is never sent back out that same port).

---

## 7. Serial protocol + the 5 modes

The GUI link rides on **USB-CDC `Serial` at 115200 baud**, newline-terminated
ASCII. It is a separate USB interface from USB-MIDI — they never mix.
(`src/drivers/config_protocol.cpp`.)

**Device → PC**

| Line | Meaning |
|------|---------|
| `HELLO r00fu_synth <fwver>` | sent on boot and in reply to `IDENTIFY` |
| `BTN <index> <row> <col> <0\|1>` | button edge (1 = press, 0 = release) |
| `ACK MAP <index>` | a mapping was stored |
| `MAP <index> <type> <ch> <val> <label>` | one row emitted by `DUMP` |
| `LOG <text>` | human-readable debug |

**PC → Device**

| Line | Meaning |
|------|---------|
| `IDENTIFY` | ask the device to announce itself |
| `MAP <index> <type> <ch> <val> <label...>` | store a mapping (rest of line = label; `-` = none) |
| `DUMP` | echo every stored mapping back as `MAP` lines |

`index = row * 8 + col` (0..63). `type` is the **MapType** (`include/midi_map.h`),
which must stay in lock-step with the GUI's `TYPES` dict:

| type | MapType | `value` means |
|------|---------|---------------|
| 0 | NONE | (no action) |
| 1 | NOTE | MIDI note 0..127 |
| 2 | CC | CC number 0..127 (momentary: 127 on press, 0 on release) |
| 3 | PROGRAM | program 0..127 |
| 4 | TRANSPORT | 0 Play · 1 Stop · 2 Record · 3 Continue · 4 TapTempo |
| 5 | MODE | 0 Drum · 1 StepSeq · 2 Keyboard · 3 DAW · 4 Performance |

The five **modes** are the table in section 1; map a button to MapType **5** to
switch between them on the device.

---

## 8. Troubleshooting

**No serial port shows up.**
- Make sure you're on the **USB** (native) port, not **UART**.
- Re-run `pio device list`. If still nothing, the device may not have enumerated —
  enter download mode (hold `BOOT`, tap `RESET`, release `BOOT`) and re-flash;
  the firmware brings USB-CDC up in `setup()`.
- Close any other program (the GUI, another monitor) holding the port — only one
  can own it at a time.

**Board won't enter / stay in download mode.**
- Sequence matters: **hold `BOOT` → tap `RESET`/`EN` → release `BOOT`**, *then*
  start the upload.
- A flaky USB-C cable is the #1 cause — many cables are charge-only with no data
  lines. Swap it. Try a different USB port / avoid hubs.

**No audio.**
- The PCM5102A needs **`SCK` tied to GND** — this firmware runs the DAC in
  PLL/software mode with no MCLK pin (`pcm5102.cpp`). A floating SCK = silence.
- Check `BCK=GPIO40`, `LRCK=GPIO41`, `DOUT=GPIO42`, plus `VIN`/`GND` and the
  PCM5102A's `FLT/DEMP/XSMT/FMT` config pins per its board.
- Confirm a button is actually **mapped to a Note** and you're in a synth mode
  (Drum/Keyboard) — DAW mode sends MIDI out rather than driving the local synth.

**MIDI not received (DIN).**
- DIN IN must go through an **opto-isolator** (6N138/H11L1) into `GPIO44`; a bare
  DIN socket won't work and can be unsafe.
- Check baud is 31250 and the IN/OUT sockets aren't swapped.
- USB-MIDI not seen by the DAW? You're probably on the **UART** port — move to
  **USB**.

**Buttons ghosting / phantom presses / inverted.**
- The matrix needs a **per-key diode** for N-key rollover. If keys read inverted
  or trigger neighbours, your diodes are reversed. The fix is a one-liner: swap
  the ROW/COL roles (drive columns, read rows) — flip `ROW_PINS`/`COL_PINS` in
  `include/config.h`. The scan drives one row LOW and reads columns with pull-ups.

---

## 9. Build order / phase status

The firmware follows the spec's phase plan (`instructions.txt`). Boot order and
task placement are real and final (`src/main.cpp`, `src/system/tasks.cpp`): all
scanners, MIDI, sequencer, and the UI dispatcher are pinned to **CORE_IO (0)**;
the audio render loop is the sole task on **CORE_AUDIO (1)** at the highest
priority, and the only place that touches DSP. Hardware scanners only
`event_post()`; the dispatcher routes; musical actions reach the synth through an
RT-safe core0→core1 command queue (`src/audio/synth.h`).

| Phase | Area | Status |
|-------|------|--------|
| 1 | Button matrix (8x8, 1 ms scan, debounce) | **functional** |
| 2 | Analog muxes (knobs/sliders, smoothing) | **functional** |
| 3 | USB-MIDI (TinyUSB composite) | **functional** |
| 4 | DIN MIDI (UART1, 47Effects lib) | **functional** |
| 5 | Sequencer (8 track / 64 step, clock, swing) | **functional** |
| 6 | Synth engine (8-voice poly) | **functional, self-contained DSP** |
| 7 | Effects (delay/reverb/chorus/bitcrusher) | **functional, tunable** |
| 8 | Presets/settings (LittleFS + ArduinoJson) | **functional** |
| 9 | Performance FX (momentary filter sweep) | **basic/scaffolded** |
| 10 | Microphone (reactive FX, looper, vocoder) | **not implemented** |

GUI config protocol (HELLO/BTN/MAP/IDENTIFY/DUMP) is **functional** and matches
`gui/r00fu_config.py` exactly. Stretch goals (SD card, sampling, wavetables,
euclidean sequencer, USB firmware update) are **not started**.
