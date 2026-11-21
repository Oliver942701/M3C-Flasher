#!/usr/bin/env python3
"""
Continental (Siemens VDO) M3C, MC9S12X internal flash checksum tool - GUI
(KTM / Husqvarna two-strokes).

Double-click to run (if Python + tkinter is installed), or:
    python3 m3c_checksum_gui.py

No extra packages needed - uses only the Python standard library (tkinter
ships with Python on Windows and macOS; on Linux install "python3-tk" if
it's missing).

Algorithm: CRC-16/MODBUS over the range(s) listed in each FF FF FF FF
descriptor found in the image (verified byte-identical against an existing
commercial tool's checksum correction on real sample files).

Checksum algorithm originally identified by NetworkDir:
https://github.com/NetworkDir/moto-ecu-checksum
"""
import os
import sys
import traceback
import tkinter as tk
from tkinter import filedialog, messagebox

# ---------------------------------------------------------------- core logic

PAGE = 0x4000
FULL_FLASH_SIZE = 0x40000  # true physical flash size of this ECU (256 KB)

_TABLE = None


def _crc_table():
    global _TABLE
    if _TABLE is None:
        t = []
        for n in range(256):
            c = n
            for _ in range(8):
                c = (c >> 1) ^ 0xA001 if (c & 1) else (c >> 1)
            t.append(c)
        _TABLE = t
    return _TABLE


def crc16_modbus(data, ranges):
    table = _crc_table()
    crc = 0xFFFF
    for s, e in ranges:
        for i in range(s, e + 1):
            crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFF]
    return crc


def to_offset(addr, full_size):
    page = (addr >> 16) & 0xFF
    win = addr & 0xFFFF
    if (addr >> 24) or page < 0xE0 or win < 0x8000 or win > 0xBFFF:
        return -1
    pages = full_size // PAGE
    if pages <= 0:
        return -1
    return (page % pages) * PAGE + (win - 0x8000)


def parse_descriptor(data, i, full_size):
    if i < 4 or data[i - 4:i] != b"\xff\xff\xff\xff":
        return None
    count = (data[i + 2] << 8) | data[i + 3]
    if count < 1 or count > 32 or i + 4 + count * 8 > len(data):
        return None
    ranges, addrs = [], []
    for k in range(count):
        p = i + 4 + k * 8
        sa = int.from_bytes(data[p:p + 4], "big")
        ea = int.from_bytes(data[p + 4:p + 8], "big")
        s = to_offset(sa, full_size)
        e = to_offset(ea, full_size)
        if s < 0 or e < s or e >= len(data) or (sa >> 16) != (ea >> 16):
            return None
        if s <= i + 1 and e >= i:
            return None
        ranges.append((s, e))
        addrs.append((sa, ea))
    return {"offset": i, "ranges": ranges, "addrs": addrs}


def find_descriptors(data, full_size=FULL_FLASH_SIZE):
    n = len(data)
    if n < 4 * PAGE or n > full_size or n % PAGE:
        return []
    out = []
    i = 4
    while i + 12 <= n:
        d = parse_descriptor(data, i, full_size)
        if d:
            out.append(d)
        i += 0x100
    return out


def verify(data):
    records = []
    for d in find_descriptors(data):
        stored = (data[d["offset"]] << 8) | data[d["offset"] + 1]
        computed = crc16_modbus(data, d["ranges"])
        records.append({
            "offset": d["offset"], "ranges": d["ranges"], "addrs": d["addrs"],
            "stored": stored, "computed": computed, "ok": stored == computed,
        })
    return records


def recalculate(data):
    """Rewrite every bad checksum record in-place. Returns (changed, records)."""
    records = verify(data)
    if not records:
        raise ValueError("No Continental M3C checksum record found in this file.")
    changed = False
    for rec in records:
        if rec["ok"]:
            continue
        v = crc16_modbus(data, rec["ranges"])
        off = rec["offset"]
        data[off] = v >> 8
        data[off + 1] = v & 0xFF
        changed = True
    final = verify(data)
    for rec in final:
        if not rec["ok"]:
            raise RuntimeError("Self-check failed at offset 0x%X" % rec["offset"])
    return changed, final


# --------------------------------------------------------------------- GUI

BG = "#1e1e24"
PANEL = "#262630"
FG = "#e8e8ec"
MUTED = "#8a8a96"
RED = "#b3261e"
RED_DARK = "#8c1d17"
GREEN = "#1f8a4c"
GREEN_DARK = "#17693b"
ACCENT = "#3a3a46"
ACCENT_HOVER = "#46465a"


class BigButton(tk.Button):
    def __init__(self, master, bg=ACCENT, hover=ACCENT_HOVER, fg=FG, **kw):
        super().__init__(
            master, bg=bg, fg=fg, activebackground=hover, activeforeground=fg,
            relief="flat", bd=0, highlightthickness=0,
            font=("Segoe UI", 14, "bold"), cursor="hand2", **kw
        )
        self._bg, self._hover = bg, hover
        self.bind("<Enter>", lambda e: self.configure(bg=self._hover))
        self.bind("<Leave>", lambda e: self.configure(bg=self._bg))

    def set_colors(self, bg, hover):
        self._bg, self._hover = bg, hover
        self.configure(bg=bg)


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("M3C Checksum")
        self.geometry("560x420")
        self.minsize(480, 380)
        self.configure(bg=BG)

        self.path = None
        self.data = None
        self.dirty = False  # recalculated but not yet saved

        # ---- header
        header = tk.Frame(self, bg=BG)
        header.pack(fill="x", padx=24, pady=(22, 10))
        tk.Label(header, text="M3C Checksum", bg=BG, fg=FG,
                 font=("Segoe UI", 18, "bold")).pack(anchor="w")
        self.file_lbl = tk.Label(header, text="No file selected", bg=BG, fg=MUTED,
                                  font=("Segoe UI", 10), anchor="w", wraplength=500, justify="left")
        self.file_lbl.pack(anchor="w", pady=(2, 0))

        # ---- status panel (big red/green indicator)
        self.status_frame = tk.Frame(self, bg=PANEL, height=110)
        self.status_frame.pack(fill="x", padx=24, pady=10)
        self.status_frame.pack_propagate(False)
        self.status_lbl = tk.Label(self.status_frame, text="—", bg=PANEL, fg=MUTED,
                                    font=("Segoe UI", 22, "bold"))
        self.status_lbl.pack(expand=True)
        self.status_sub = tk.Label(self.status_frame, text="Open a file to check", bg=PANEL, fg=MUTED,
                                    font=("Segoe UI", 10))
        self.status_sub.pack(pady=(0, 10))

        # ---- big buttons
        btns = tk.Frame(self, bg=BG)
        btns.pack(fill="both", expand=True, padx=24, pady=(4, 10))
        btns.columnconfigure((0, 1), weight=1)
        btns.rowconfigure(0, weight=1)
        btns.rowconfigure(1, weight=1)

        self.open_btn = BigButton(btns, text="Open file", command=self.open_file)
        self.open_btn.grid(row=0, column=0, sticky="nsew", padx=(0, 6), pady=(0, 6))

        self.save_btn = BigButton(btns, text="Save", command=self.save_file, state="disabled")
        self.save_btn.grid(row=0, column=1, sticky="nsew", padx=(6, 0), pady=(0, 6))

        self.recalc_btn = BigButton(btns, text="Recalculate CHK", command=self.recalc,
                                     state="disabled")
        self.recalc_btn.grid(row=1, column=0, columnspan=2, sticky="nsew", pady=(6, 0))

        # ---- footer
        tk.Label(self, text="CRC-16/MODBUS · Continental M3C (MC9S12X)", bg=BG, fg=MUTED,
                 font=("Segoe UI", 8)).pack(side="bottom", pady=(0, 10))

    # ---- state helpers -----------------------------------------------

    def set_status(self, ok):
        if ok is None:
            self.status_frame.configure(bg=PANEL)
            self.status_lbl.configure(bg=PANEL, fg=MUTED, text="—")
            self.status_sub.configure(bg=PANEL, fg=MUTED, text="Open a file to check")
        elif ok:
            self.status_frame.configure(bg=GREEN)
            self.status_lbl.configure(bg=GREEN, fg="white", text="CHECKSUM OK")
            self.status_sub.configure(bg=GREEN, fg="white", text=os.path.basename(self.path or ""))
        else:
            self.status_frame.configure(bg=RED)
            self.status_lbl.configure(bg=RED, fg="white", text="CHK not Calculated")
            self.status_sub.configure(bg=RED, fg="white", text="Press 'Recalculate CHK'")

    def refresh_from(self, records):
        bad = [r for r in records if not r["ok"]]
        self.set_status(len(bad) == 0)
        self.recalc_btn.configure(state=("normal" if bad else "disabled"))
        self.save_btn.configure(state=("normal" if (self.dirty or not bad) else "disabled"))

    # ---- actions -------------------------------------------------------

    def open_file(self):
        p = filedialog.askopenfilename(
            title="Select ECU bin file",
            filetypes=[("Binary files", "*.bin"), ("All files", "*.*")],
        )
        if not p:
            return
        try:
            with open(p, "rb") as f:
                data = bytearray(f.read())
            records = verify(data)
            if not records:
                messagebox.showerror("Error", "No checksum record found in this file.")
                return
            self.path = p
            self.data = data
            self.dirty = False
            self.file_lbl.configure(text=p, fg=FG)
            self.refresh_from(records)
        except Exception as e:
            traceback.print_exc()
            messagebox.showerror("Error", str(e))

    def recalc(self):
        if self.data is None:
            return
        try:
            changed, final = recalculate(self.data)
            self.dirty = self.dirty or changed
            self.refresh_from(final)
        except Exception as e:
            traceback.print_exc()
            messagebox.showerror("Error", str(e))

    def save_file(self):
        if self.data is None:
            return
        default_name = "CHK_OK.bin"
        if self.path:
            base, ext = os.path.splitext(os.path.basename(self.path))
            default_name = base + " (CHK_OK)" + (ext or ".bin")
        out_path = filedialog.asksaveasfilename(
            title="Save file",
            initialfile=default_name,
            defaultextension=".bin",
            filetypes=[("Binary files", "*.bin"), ("All files", "*.*")],
        )
        if not out_path:
            return
        try:
            with open(out_path, "wb") as f:
                f.write(bytes(self.data))
            self.dirty = False
            messagebox.showinfo("Saved", "File saved:\n" + out_path)
        except Exception as e:
            traceback.print_exc()
            messagebox.showerror("Error", str(e))


def main():
    try:
        app = App()
        app.mainloop()
    except tk.TclError as e:
        print("Failed to start GUI (tkinter/display missing):", e, file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()