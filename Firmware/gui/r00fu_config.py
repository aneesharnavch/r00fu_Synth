#!/usr/bin/env python3
"""
r00fu_synth — Button Configurator GUI
=====================================
A bench/producer tool for the r00fu_synth groovebox.

What it does:
  * Connects to the ESP32-S3 over USB serial.
  * Lights up the on-screen 8x8 grid LIVE when you press a physical button
    (so you can find/verify every key).
  * Click a cell -> assign a FUNCTION to that button:
        Note  / CC / Program Change / Transport / Mode switch / None
  * Save / Load the whole mapping as JSON.
  * "Push to Device" sends the mapping to the firmware over serial so the
    groovebox acts on it standalone — and because notes/CCs are MIDI, the
    same map makes it a USB-MIDI control surface in any DAW.

Run:
    pip install pyserial
    python r00fu_config.py

Serial protocol is documented in firmware/r00fu_synth/r00fu_synth.ino
"""

import json
import queue
import threading
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    serial = None

BAUD = 115200
GRID = 8

# ---- Function model -------------------------------------------------------
# Keep these ids in lock-step with MapType in the firmware .ino
TYPES = {
    "none":      0,
    "note":      1,
    "cc":        2,
    "program":   3,
    "transport": 4,
    "mode":      5,
}
TYPE_NAMES = {v: k for k, v in TYPES.items()}

TRANSPORT = ["Play", "Stop", "Record", "Continue", "TapTempo"]
MODES = ["Drum", "StepSeq", "Keyboard", "DAW", "Performance"]

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def note_name(n):
    return f"{NOTE_NAMES[n % 12]}{n // 12 - 1}"


def default_button(index):
    row, col = divmod(index, GRID)
    return {
        "index": index, "row": row, "col": col,
        "type": "none", "channel": 1, "value": 0, "label": "",
    }


# ===========================================================================
# Serial worker — runs in its own thread, talks to the main thread via queues
# ===========================================================================
class SerialLink:
    def __init__(self, rx_queue):
        self.rx = rx_queue          # lines from device -> GUI
        self.ser = None
        self.thread = None
        self.running = False

    @staticmethod
    def ports():
        if serial is None:
            return []
        return [p.device for p in serial.tools.list_ports.comports()]

    def open(self, port):
        self.close()
        self.ser = serial.Serial(port, BAUD, timeout=0.1)
        self.running = True
        self.thread = threading.Thread(target=self._read_loop, daemon=True)
        self.thread.start()
        self.send("IDENTIFY")

    def _read_loop(self):
        buf = b""
        while self.running and self.ser:
            try:
                data = self.ser.read(256)
            except Exception as e:
                self.rx.put(("error", str(e)))
                break
            if data:
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    text = line.decode("utf-8", "replace").strip()
                    if text:
                        self.rx.put(("line", text))

    def send(self, text):
        if self.ser and self.ser.is_open:
            try:
                self.ser.write((text + "\n").encode())
            except Exception as e:
                self.rx.put(("error", str(e)))

    def close(self):
        self.running = False
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None


# ===========================================================================
# Main application
# ===========================================================================
class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("r00fu_synth — Button Configurator")
        self.configure(bg="#1b1d23")
        self.geometry("980x640")

        self.buttons = [default_button(i) for i in range(GRID * GRID)]
        self.cells = {}                 # index -> tk widget
        self.selected = None
        self.rx_queue = queue.Queue()
        self.link = SerialLink(self.rx_queue)
        self.pressed = set()            # currently-held indices (live)
        self.learn_mode = tk.BooleanVar(value=False)

        self._build_topbar()
        self._build_body()
        self._build_statusbar()
        self.refresh_grid()

        self.after(30, self._pump)
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    # ---- UI construction --------------------------------------------------
    def _build_topbar(self):
        bar = tk.Frame(self, bg="#23262e")
        bar.pack(fill="x", side="top")

        tk.Label(bar, text="Port", bg="#23262e", fg="#cfd3dc").pack(side="left", padx=(10, 4), pady=8)
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(bar, textvariable=self.port_var, width=16, values=self.link.ports())
        self.port_combo.pack(side="left")
        tk.Button(bar, text="⟳", command=self._refresh_ports, width=3).pack(side="left", padx=4)

        self.connect_btn = tk.Button(bar, text="Connect", command=self._toggle_connect, width=10)
        self.connect_btn.pack(side="left", padx=6)

        ttk.Checkbutton(bar, text="Learn mode (click cell, then press the key)",
                        variable=self.learn_mode).pack(side="left", padx=12)

        tk.Button(bar, text="Push to Device", command=self.push_to_device).pack(side="right", padx=6, pady=6)
        tk.Button(bar, text="Load…", command=self.load_json).pack(side="right", padx=2)
        tk.Button(bar, text="Save…", command=self.save_json).pack(side="right", padx=2)

    def _build_body(self):
        body = tk.Frame(self, bg="#1b1d23")
        body.pack(fill="both", expand=True)

        # 8x8 grid
        grid_frame = tk.Frame(body, bg="#1b1d23")
        grid_frame.pack(side="left", padx=16, pady=16)
        for r in range(GRID):
            for c in range(GRID):
                idx = r * GRID + c
                cell = tk.Label(grid_frame, width=7, height=3, bd=2, relief="raised",
                                bg="#2b2f3a", fg="#e6e8ee", font=("Consolas", 9))
                cell.grid(row=r, column=c, padx=3, pady=3)
                cell.bind("<Button-1>", lambda e, i=idx: self.select(i))
                self.cells[idx] = cell

        # Inspector
        insp = tk.Frame(body, bg="#23262e", width=320)
        insp.pack(side="right", fill="y", padx=(0, 12), pady=16)
        insp.pack_propagate(False)

        self.insp_title = tk.Label(insp, text="No button selected", bg="#23262e",
                                   fg="#9aa0ae", font=("Segoe UI", 12, "bold"))
        self.insp_title.pack(pady=(14, 10))

        form = tk.Frame(insp, bg="#23262e")
        form.pack(fill="x", padx=16)

        tk.Label(form, text="Function", bg="#23262e", fg="#cfd3dc").grid(row=0, column=0, sticky="w", pady=4)
        self.type_var = tk.StringVar(value="none")
        self.type_combo = ttk.Combobox(form, textvariable=self.type_var, state="readonly",
                                       values=list(TYPES.keys()), width=16)
        self.type_combo.grid(row=0, column=1, pady=4)
        self.type_combo.bind("<<ComboboxSelected>>", lambda e: self._on_type_change())

        tk.Label(form, text="Channel", bg="#23262e", fg="#cfd3dc").grid(row=1, column=0, sticky="w", pady=4)
        self.chan_var = tk.IntVar(value=1)
        self.chan_spin = tk.Spinbox(form, from_=1, to=16, textvariable=self.chan_var, width=16,
                                    command=self._apply_inspector)
        self.chan_spin.grid(row=1, column=1, pady=4)

        self.value_label = tk.Label(form, text="Value", bg="#23262e", fg="#cfd3dc")
        self.value_label.grid(row=2, column=0, sticky="w", pady=4)
        self.value_var = tk.StringVar()
        # value can be a number spin OR a dropdown depending on type
        self.value_spin = tk.Spinbox(form, from_=0, to=127, width=16,
                                     command=self._apply_inspector)
        self.value_combo = ttk.Combobox(form, state="readonly", width=16)
        self.value_combo.bind("<<ComboboxSelected>>", lambda e: self._apply_inspector())
        self.value_spin.grid(row=2, column=1, pady=4)

        self.value_hint = tk.Label(form, text="", bg="#23262e", fg="#7d8392", font=("Segoe UI", 8))
        self.value_hint.grid(row=3, column=1, sticky="w")

        tk.Label(form, text="Label", bg="#23262e", fg="#cfd3dc").grid(row=4, column=0, sticky="w", pady=4)
        self.label_var = tk.StringVar()
        self.label_entry = tk.Entry(form, textvariable=self.label_var, width=18)
        self.label_entry.grid(row=4, column=1, pady=4)
        self.label_entry.bind("<KeyRelease>", lambda e: self._apply_inspector())

        tk.Button(insp, text="Apply", command=self._apply_inspector).pack(pady=10)
        tk.Button(insp, text="Clear button", command=self._clear_selected).pack()

        # bind spinbox manual typing
        for w in (self.chan_spin, self.value_spin):
            w.bind("<KeyRelease>", lambda e: self._apply_inspector())

    def _build_statusbar(self):
        self.status = tk.Label(self, text="Disconnected.", bg="#15171c", fg="#8b94a3",
                               anchor="w", font=("Consolas", 9))
        self.status.pack(fill="x", side="bottom")

    # ---- Serial connection ------------------------------------------------
    def _refresh_ports(self):
        self.port_combo["values"] = self.link.ports()

    def _toggle_connect(self):
        if self.link.ser:
            self.link.close()
            self.connect_btn.config(text="Connect")
            self.set_status("Disconnected.")
            return
        if serial is None:
            messagebox.showerror("pyserial missing", "Run:  pip install pyserial")
            return
        port = self.port_var.get().strip()
        if not port:
            messagebox.showinfo("No port", "Pick a serial port first (⟳ to refresh).")
            return
        try:
            self.link.open(port)
            self.connect_btn.config(text="Disconnect")
            self.set_status(f"Connected to {port} @ {BAUD}.")
        except Exception as e:
            messagebox.showerror("Connect failed", str(e))

    def _pump(self):
        """Drain serial events on the Tk thread."""
        try:
            while True:
                kind, payload = self.rx_queue.get_nowait()
                if kind == "line":
                    self._handle_line(payload)
                elif kind == "error":
                    self.set_status(f"Serial error: {payload}")
        except queue.Empty:
            pass
        self.after(30, self._pump)

    def _handle_line(self, text):
        parts = text.split()
        if not parts:
            return
        if parts[0] == "BTN" and len(parts) >= 5:
            idx, _r, _c, state = (int(parts[1]), int(parts[2]), int(parts[3]), int(parts[4]))
            self._on_physical_button(idx, state)
        elif parts[0] == "HELLO":
            self.set_status("Device: " + text)
        elif parts[0] == "ACK":
            self.set_status("Device " + text)
        # (LOG / MAP echoes simply land in status)
        else:
            self.set_status(text)

    def _on_physical_button(self, idx, state):
        if not (0 <= idx < GRID * GRID):
            return
        if state:
            self.pressed.add(idx)
            if self.learn_mode.get() and self.selected is not None and self.selected != idx:
                # Move selection to the key the user just pressed
                self.select(idx)
        else:
            self.pressed.discard(idx)
        self.paint_cell(idx)

    # ---- Grid / selection -------------------------------------------------
    def select(self, idx):
        self.selected = idx
        b = self.buttons[idx]
        r, c = b["row"], b["col"]
        self.insp_title.config(text=f"Button {idx}   (row {r}, col {c})", fg="#e6e8ee")
        self.type_var.set(b["type"])
        self.chan_var.set(b["channel"])
        self.label_var.set(b["label"])
        self._on_type_change(load_value=b["value"])
        self.refresh_grid()

    def _on_type_change(self, load_value=None):
        t = self.type_var.get()
        self.value_spin.grid_remove()
        self.value_combo.grid_remove()
        self.value_hint.config(text="")

        if t == "note":
            self.value_spin.config(from_=0, to=127)
            self.value_spin.grid(row=2, column=1, pady=4)
            if load_value is not None:
                self.value_spin.delete(0, "end"); self.value_spin.insert(0, load_value)
            self.value_hint.config(text=f"MIDI note  ({note_name(int(self._spin_val()))})")
        elif t == "cc":
            self.value_spin.config(from_=0, to=127)
            self.value_spin.grid(row=2, column=1, pady=4)
            if load_value is not None:
                self.value_spin.delete(0, "end"); self.value_spin.insert(0, load_value)
            self.value_hint.config(text="CC number 0–127")
        elif t == "program":
            self.value_spin.config(from_=0, to=127)
            self.value_spin.grid(row=2, column=1, pady=4)
            if load_value is not None:
                self.value_spin.delete(0, "end"); self.value_spin.insert(0, load_value)
            self.value_hint.config(text="Program 0–127")
        elif t == "transport":
            self.value_combo.config(values=TRANSPORT)
            self.value_combo.grid(row=2, column=1, pady=4)
            self.value_combo.set(TRANSPORT[load_value] if load_value is not None and load_value < len(TRANSPORT) else TRANSPORT[0])
        elif t == "mode":
            self.value_combo.config(values=MODES)
            self.value_combo.grid(row=2, column=1, pady=4)
            self.value_combo.set(MODES[load_value] if load_value is not None and load_value < len(MODES) else MODES[0])
        else:  # none
            self.value_hint.config(text="(no action)")

        self._apply_inspector()

    def _spin_val(self):
        try:
            return int(self.value_spin.get())
        except (ValueError, tk.TclError):
            return 0

    def _current_value(self):
        t = self.type_var.get()
        if t in ("note", "cc", "program"):
            return self._spin_val()
        if t == "transport":
            v = self.value_combo.get()
            return TRANSPORT.index(v) if v in TRANSPORT else 0
        if t == "mode":
            v = self.value_combo.get()
            return MODES.index(v) if v in MODES else 0
        return 0

    def _apply_inspector(self):
        if self.selected is None:
            return
        b = self.buttons[self.selected]
        b["type"] = self.type_var.get()
        try:
            b["channel"] = int(self.chan_var.get())
        except (ValueError, tk.TclError):
            b["channel"] = 1
        b["value"] = self._current_value()
        b["label"] = self.label_var.get().strip()
        if b["type"] == "note":
            self.value_hint.config(text=f"MIDI note  ({note_name(b['value'])})")
        self.paint_cell(self.selected)

    def _clear_selected(self):
        if self.selected is None:
            return
        self.buttons[self.selected] = default_button(self.selected)
        self.select(self.selected)

    # ---- Painting ---------------------------------------------------------
    def short_desc(self, b):
        t = b["type"]
        if t == "note":
            return note_name(b["value"])
        if t == "cc":
            return f"CC{b['value']}"
        if t == "program":
            return f"PG{b['value']}"
        if t == "transport":
            return TRANSPORT[b["value"]] if b["value"] < len(TRANSPORT) else "?"
        if t == "mode":
            return MODES[b["value"]] if b["value"] < len(MODES) else "?"
        return ""

    def paint_cell(self, idx):
        cell = self.cells[idx]
        b = self.buttons[idx]
        desc = self.short_desc(b)
        text = f"{idx}\n{b['label'] or desc}" if (b["label"] or desc) else f"{idx}"
        cell.config(text=text)

        if idx in self.pressed:
            cell.config(bg="#4ade80", fg="#0b1410", relief="sunken")   # live press = green
        elif idx == self.selected:
            cell.config(bg="#3b82f6", fg="#ffffff", relief="ridge")    # selected = blue
        elif b["type"] != "none":
            cell.config(bg="#3a4150", fg="#e6e8ee", relief="raised")   # assigned
        else:
            cell.config(bg="#2b2f3a", fg="#7d8392", relief="raised")   # empty

    def refresh_grid(self):
        for i in range(GRID * GRID):
            self.paint_cell(i)

    # ---- Persistence / device ---------------------------------------------
    def as_config(self):
        return {"version": 1, "device": "r00fu_synth", "buttons": self.buttons}

    def save_json(self):
        path = filedialog.asksaveasfilename(defaultextension=".json",
                                            filetypes=[("JSON", "*.json")],
                                            initialfile="r00fu_mapping.json")
        if not path:
            return
        with open(path, "w", encoding="utf-8") as f:
            json.dump(self.as_config(), f, indent=2)
        self.set_status(f"Saved {path}")

    def load_json(self):
        path = filedialog.askopenfilename(filetypes=[("JSON", "*.json")])
        if not path:
            return
        try:
            with open(path, "r", encoding="utf-8") as f:
                cfg = json.load(f)
            loaded = {b["index"]: b for b in cfg.get("buttons", [])}
            for i in range(GRID * GRID):
                self.buttons[i] = loaded.get(i, default_button(i))
            self.selected = None
            self.insp_title.config(text="No button selected", fg="#9aa0ae")
            self.refresh_grid()
            self.set_status(f"Loaded {path}")
        except Exception as e:
            messagebox.showerror("Load failed", str(e))

    def push_to_device(self):
        if not self.link.ser:
            messagebox.showinfo("Not connected", "Connect to the device first.")
            return
        n = 0
        for b in self.buttons:
            t = TYPES.get(b["type"], 0)
            label = b["label"] or "-"
            self.link.send(f"MAP {b['index']} {t} {b['channel']} {b['value']} {label}")
            n += 1
        self.set_status(f"Pushed {n} mappings to device.")

    # ---- misc -------------------------------------------------------------
    def set_status(self, text):
        self.status.config(text=text)

    def _on_close(self):
        self.link.close()
        self.destroy()


if __name__ == "__main__":
    App().mainloop()
