# r00fu_synth — Button Configurator

A desktop tool to **detect** every physical button on the groovebox and
**map** each one to a function (MIDI note / CC / Program / Transport / Mode).
Built for a music producer: the same map drives the standalone groovebox *and*
turns it into a USB-MIDI control surface in Ableton / FL / Reaper / Bitwig.

## Setup
```
pip install pyserial
python r00fu_config.py
```
(Tkinter ships with standard Python on Windows.)

## Workflow
1. Flash `../firmware/r00fu_synth/r00fu_synth.ino` to the ESP32-S3
   (Arduino IDE → ESP32S3 Dev Module → **USB CDC On Boot: Enabled**).
2. Open the GUI, pick the COM port, **Connect**.
3. **Press a physical button** → the matching cell flashes green. That's how you
   find/verify keys.
4. **Learn mode**: tick it, click a cell, press the key you want — selection
   jumps to that key so you can assign it without hunting.
5. Pick a Function in the right panel, set channel/value/label, **Apply**.
6. **Save…** to JSON, and **Push to Device** to send the live map over serial.

## Serial protocol
115200 baud, newline ASCII. Full spec is in the firmware header.
| Dir | Message | Meaning |
|-----|---------|---------|
| ←   | `HELLO r00fu_synth 0.1.0` | device announce |
| ←   | `BTN <idx> <row> <col> <0\|1>` | button edge |
| →   | `MAP <idx> <type> <ch> <val> <label>` | store mapping |
| →   | `IDENTIFY` / `DUMP` | query device |
| ←   | `ACK MAP <idx>` | mapping stored |

`idx = row*8 + col` (0..63). `type`: 0 none, 1 note, 2 cc, 3 program, 4 transport, 5 mode.
