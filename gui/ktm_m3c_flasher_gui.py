#!/usr/bin/env python3
import tkinter as tk
from tkinter import ttk, filedialog, scrolledtext, messagebox, simpledialog
import serial
import serial.tools.list_ports
import threading
import time
import os

BAUD = 115200

SECTIONS = [(0x7800, 0x79FF), (0x7E00, 0x7FFF)]
TOTAL_ADDRS = sum(e - s + 1 for s, e in SECTIONS)  # 1024
BIN_SIZE = 262144  # 256KB

WRITEFULL_TOTAL_STEPS = 2306
UPLOAD_CHUNK_SIZE = 256


# ECU connector A pinout (front view), PNG, base64. Embedded so the GUI
# stays a single file. Coloured pins: yellow B1 = CAN-H, blue C1 = CAN-L,
# red A4/B4 = +12 V, black M4 = GND.
WIRING_PNG_B64 = (
    "iVBORw0KGgoAAAANSUhEUgAAAtsAAAFoCAMAAACxGMfcAAADAFBMVEX///8AAABaWlr/8B4eNP/o"
    "HxsUFBQoKCg8PDxQUFBkZGR4eHiMjIygoKC0tLTIyMjc3Nzw8PAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAADbxU3lAAAVGklEQVR42u2d67arKpQGE7tbQBDh/V+2zW2Zs4OoCF6SqjHOn8PG"
    "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"
    "7cUp4O5u15u57XH7u9xucRu3cRu3cRu3cRu3cftIbnMvidvbZoCi3ghZ4LqttvrwwuH26dweWXql"
    "ACUS4s0+vOPZzQndBp5L4jbgNm4DbuM24DZuA27jNuA24DZuA27jNuA2bgNu4zbgNm4DbgNu4zYU"
    "wXchfGTZmCAtbk/RvS3lpd3y9svE8kI+X3uBza/9dCmYa2iQ+Pj7D134hYn3w9w53A7Qvu8vu7zd"
    "/L3fIIIvIDRDe3DxVDm8AhDaXTrefardq6HdxLurS0J7Clp8jhC3q9xua4vbYbdfh2/M7Wi7FsNy"
    "niG3pXq1q+DiqrV+tYvgusMy2n2q3ddNdPPDsqRNcG3WqfYt1ryedLurr7g94vb/PRlzO9o+HCkb"
    "dlvHF8Wevaa2Cbs9e83t4DRfU2tqT7UfwW0vpcDtsm63E24Hl0RfuV78kvXkv9XtSnYSt3H7C902"
    "dedxG7e/0G3b1yO4jdtf6HafMxmj68hc77iN2yd1W98Oeh25cOP2FjkJbpfJAC+OmmTCbTnhttw3"
    "A/xqtz1uH/C5pL7KJyLsdv1qr8PXbfFqvwbdHrqH3Y6397PCRTdfDd2D7jYT7Ulu1+0H5rrGbc8z"
    "97Db74Ma7PJ2O4w2CX7diXZv/ppNd8m++Us7tNsCm0+hFSHG7bTT40kujJW6TM2q55e3w1J8kIic"
    "OojFbbgwfhu3AbdxG3AbtwG3cRtwG7cBtwFwG3AbtwG3cRtwG7cBt3EbcBsAtwG3cRtwG7cBt3Eb"
    "cBu3AbdxG3AbcBu3AbdxG3AbtwG3cRtwG7cBtwG3cRtwG7cBt3EbcBu3AbdxG3AbcBu3AbdxG3Ab"
    "twG3cRtwG7cBtwG3cRtwG7cBt3EbcBu3AbdxG3AbcBu3x/BuHH+uAx37Ku5QG8XtTdyW9Tgn+5Fo"
    "I19FpHpoYhv1uJ3RbZX7Wieu41TncttEvso11W0d2WbtcDvb36yvMvOlwsvIsWu+x+36O93u6qv4"
    "Drer/iztcBu337+y/A63m15Ejdu4/X701Jdct/uiJPP+xO0zu91eS5Tb+7jdFyVX5XEbt193kiVK"
    "kp3c7i/c18bjNm7f1O6Pnai+xu1bDHhVOXcpbq9we9d82/ZX7Vp9kdvVzcVadx63f9ptb5trKbV3"
    "c7uS9e1LyarJgW7qyLETujkROnaaXiudf6N1s9cOUvdHbqKM2vu5XSkR8xF+hVoWM2w3t/u/LdH7"
    "18UWUlXf6HbVVOpH+Wf4i/jV/aAK2rWv2z9L80/5Kxr2CW5/Cf+6zR7BbdwG3MZt3MZt3MZtwG3c"
    "hvGE5MFHTvKA/YPb593d8oH499ncA8Uewu0vqUU+Rr6wh3AbtwG3cRu3cRu3cRtwG7fh842MKJI9"
    "hNvn3d+7jfjEbdwG3AbAbQDcBsBtgPVuK4BTIqfcdhrgnNgLAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AABAbv4fKP1vdnCCytIAAAAASUVORK5CYII="
)

# (pin, colour name, colour, function, connect to)
WIRING_ECU = [
    ('B1', 'yellow', '#fff01e', 'CAN-H', 'CAN transceiver CANH'),
    ('C1', 'blue', '#1e34ff', 'CAN-L', 'CAN transceiver CANL'),
    ('A4', 'red', '#e81f1b', '+12 V', '+12 V supply (also acts as ignition)'),
    ('B4', 'red', '#e81f1b', '+12 V', '+12 V supply (also acts as ignition)'),
    ('M4', 'black', '#000000', 'GND', 'supply GND + ESP32/transceiver GND'),
]

WIRING_ESP32 = [
    ('GPIO 4', 'CAN transceiver TX (CTX)'),
    ('GPIO 5', 'CAN transceiver RX (CRX)'),
    ('3.3 V / GND', 'CAN transceiver VCC / GND'),
    ('GPIO 15', 'SD card CS'),
    ('GPIO 18', 'SD card SCK'),
    ('GPIO 17', 'SD card MISO'),
    ('GPIO 16', 'SD card MOSI'),
]


# =====================================================================
# Fault code descriptions (same list as the Android app). The blink code
# is the most likely match from the owner's manual fault table - not
# confirmed by KTM for every code. "1L 2S" = 1 long + 2 short flashes.
# =====================================================================
DTC_DESCRIPTIONS = {
    'P0335': ('Crankshaft position sensor - circuit fault / no signal', '02 (2S)'),
    'P0336': ('Crankshaft position sensor - signal implausible / irregular', '02 (2S)'),
    'P0337': ('Crankshaft position sensor - signal too low', '02 (2S)'),
    'P0338': ('Crankshaft position sensor - signal too high', '02 (2S)'),
    'P0339': ('Crankshaft position sensor - signal intermittent', '02 (2S)'),
    'P0016': ('Crankshaft position - correlation / synchronization faulty', '02 (2S)'),
    'P0120': ('Throttle position sensor A - circuit fault', '06 (6S)'),
    'P0121': ('Throttle position sensor A - range/performance (adaption failed)', '06 (6S)'),
    'P0122': ('Throttle position sensor A - input signal too low', '06 (6S)'),
    'P0123': ('Throttle position sensor A - input signal too high', '06 (6S)'),
    'P0638': ('Throttle actuator control - range/performance', '06 (6S)'),
    'P0105': ('Crankcase / manifold pressure sensor - circuit fault', '09 (9S)'),
    'P0106': ('Crankcase / manifold pressure sensor - range/performance, difference to ECU value too high', '14 (1L 4S)'),
    'P0107': ('Crankcase / manifold pressure sensor - short circuit to ground (low)', '09 (9S)'),
    'P0108': ('Crankcase / manifold pressure sensor - open / short circuit to plus (high)', '09 (9S)'),
    'P0101': ('Air flow / air mass calculation - range/performance', ''),
    'P0102': ('Air flow / air mass calculation - input too low', ''),
    'P0103': ('Air flow / air mass calculation - input too high', ''),
    'P2227': ('Ambient air pressure sensor - range/performance', '09 (9S)'),
    'P2228': ('Ambient air pressure sensor - short circuit to ground (low)', '09 (9S)'),
    'P2229': ('Ambient air pressure sensor - open / short circuit to plus (high)', '09 (9S)'),
    'P2230': ('Ambient air pressure sensor - signal intermittent', '09 (9S)'),
    'P0069': ('Manifold pressure vs. ambient pressure - correlation', ''),
    'P0070': ('Ambient air temperature sensor - circuit fault', ''),
    'P0071': ('Ambient air temperature sensor - range/performance', ''),
    'P0072': ('Ambient air temperature sensor - input signal too low', ''),
    'P0073': ('Ambient air temperature sensor - input signal too high', ''),
    'P0110': ('Intake air temperature sensor - circuit fault', '13 (1L 3S)'),
    'P0111': ('Intake air temperature sensor - range/performance', '13 (1L 3S)'),
    'P0112': ('Intake air temperature sensor - input signal too low', '13 (1L 3S)'),
    'P0113': ('Intake air temperature sensor - input signal too high', '13 (1L 3S)'),
    'P0115': ('Coolant temperature sensor - circuit fault', '12 (1L 2S)'),
    'P0116': ('Coolant temperature sensor - range/performance', '12 (1L 2S)'),
    'P0117': ('Coolant temperature sensor - input signal too low', '12 (1L 2S)'),
    'P0118': ('Coolant temperature sensor - input signal too high', '12 (1L 2S)'),
    'P0125': ('Coolant temperature too low for closed loop control', ''),
    'P0217': ('Engine overtemperature', ''),
    'P0560': ('System (battery) voltage - circuit fault', '21 (2L 1S)'),
    'P0562': ('Battery voltage - input voltage too low', '21 (2L 1S)'),
    'P0563': ('Battery voltage - input voltage too high', '21 (2L 1S)'),
    'P0200': ('Injector circuit - fault', ''),
    'P0201': ('Injection valve 0, cylinder 1 (low load injector) - circuit fault', '33 (3L 3S)'),
    'P0261': ('Injection valve 0, cylinder 1 - input signal too low', '33 (3L 3S)'),
    'P0262': ('Injection valve 0, cylinder 1 - input signal too high', '33 (3L 3S)'),
    'P0202': ('Injector 2 circuit - likely injection valve 1 (top feed injector)', '34 (3L 4S)'),
    'P0264': ('Injector 2 circuit low - likely injection valve 1 (top feed injector)', '34 (3L 4S)'),
    'P0265': ('Injector 2 circuit high - likely injection valve 1 (top feed injector)', '34 (3L 4S)'),
    'P0351': ('Ignition coil - circuit fault', '37 (3L 7S)'),
    'P2300': ('Ignition coil - primary circuit low', '37 (3L 7S)'),
    'P2301': ('Ignition coil - primary circuit high', '37 (3L 7S)'),
    'P0230': ('Fuel pump - primary circuit fault', '41 (4L 1S)'),
    'P0231': ('Fuel pump - short circuit to ground / open circuit (low)', '41 (4L 1S)'),
    'P0232': ('Fuel pump - open circuit / short circuit to plus (high)', '41 (4L 1S)'),
    'P0627': ('Fuel pump control - circuit open', '41 (4L 1S)'),
    'P0628': ('Fuel pump control - circuit low', '41 (4L 1S)'),
    'P0629': ('Fuel pump control - circuit high', '41 (4L 1S)'),
    'P206C': ('Oil pump circuit (reported on KTM group 2-strokes)', '42 (4L 2S)'),
    'P260C': ('Oil pump circuit (reported on KTM group 2-strokes)', '42 (4L 2S)'),
    'P0505': ('Idle air control system - fault', ''),
    'P0506': ('Idle speed lower than expected', ''),
    'P0507': ('Idle speed higher than expected', ''),
    'P0500': ('Vehicle speed sensor - fault', ''),
    'P0600': ('ECU communication (serial link) - fault', ''),
    'P0601': ('ECU internal memory checksum error', ''),
    'P0603': ('ECU internal keep-alive memory error', ''),
    'P0604': ('ECU internal RAM error', ''),
    'P0605': ('ECU internal ROM error', ''),
    'P0606': ('ECU processor fault', ''),
    'P0650': ('Malfunction indicator lamp (MIL) - circuit fault', ''),
    'U0001': ('CAN bus communication - fault', ''),
    'U0100': ('Lost communication with engine control unit', ''),
}

DTC_GROUPS = [
    ('P00', 'Fuel / air metering and auxiliary emission controls'),
    ('P01', 'Fuel / air metering'),
    ('P02', 'Fuel / air metering (injector circuit)'),
    ('P03', 'Ignition system or misfire'),
    ('P04', 'Auxiliary emission controls'),
    ('P05', 'Vehicle speed, idle control and auxiliary inputs'),
    ('P06', 'ECU and auxiliary outputs'),
    ('P07', 'Transmission'),
    ('P1', 'Manufacturer specific (KTM / Continental)'),
    ('P2', 'Fuel / air metering, auxiliary controls'),
    ('P3', 'Manufacturer specific / reserved'),
    ('U', 'Network / CAN communication'),
    ('C', 'Chassis'),
    ('B', 'Body'),
]


def describe_dtc(code):
    """Returns (description, blink code) for a code like 'P0118'."""
    code = (code or '').upper().strip()
    if code in DTC_DESCRIPTIONS:
        return DTC_DESCRIPTIONS[code]
    for prefix, text in DTC_GROUPS:
        if code.startswith(prefix):
            return (f'Unknown code - group: {text}', '')
    return ('Unknown code', '')


def addr_to_bin_offset(addr):
    if 0x7800 <= addr <= 0x79FF:
        return (addr - 0x7800) * 256
    elif 0x7E00 <= addr <= 0x7FFF:
        return (addr - 0x7E00) * 256 + 0x20000
    return None


class FlasherGUI:
    def __init__(self, root):
        self.root = root
        self.root.title("KTM M3C Flasher + Diagnostics")
        self.root.geometry("1280x760")
        self.root.minsize(1100, 560)
        self.root.configure(bg='#1e1e1e')
        self.ser = None
        self.connected = False
        self.reading = False
        self.op_busy = False
        self.sd_ready = False
        self.sd_files = []
        # Incremented on every ESP32 reset - long-running worker threads
        # compare against the value they started with and stop early, so
        # an operation that was interrupted by a reset does not keep
        # waiting for answers that will never come.
        self.reset_gen = 0
        # Diagnostics mode (firmware 2.0+). Flashing and diagnostics never
        # run at the same time - the ESP32 enforces it too (see
        # firmware/diagnostics.h), the GUI only greys out the buttons.
        self.mode = 'flasher'
        self.fw_version = ''
        self.live_on = False
        self.live_thread = None
        self._build()
        self.root.protocol('WM_DELETE_WINDOW', self._on_close)
        self.port_combo.bind('<<ComboboxSelected>>', self._on_port_changed)
        # Look for the ESP32 in a background thread so the window opens
        # immediately - some COM ports (e.g. Bluetooth serial ports on a
        # laptop) can block for a long time when opened.
        self.port_combo['values'] = [p.device for p in
                                     serial.tools.list_ports.comports()]
        self.sd_status.config(text='Looking for the ESP32...')
        threading.Thread(target=self._auto_detect, daemon=True).start()

    def _build(self):
        style = ttk.Style()
        style.theme_use('clam')
        style.configure('TProgressbar', troughcolor='#2d2d2d',
                        background='#0078d4', borderwidth=0)

        top = tk.Frame(self.root, bg='#2d2d2d', pady=8, padx=10)
        top.pack(fill='x')

        tk.Label(top, text='Port:', bg='#2d2d2d', fg='#cccccc',
                 font=('Consolas', 9)).pack(side='left')
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(top, textvariable=self.port_var,
                                        width=12, font=('Consolas', 9))
        self.port_combo.pack(side='left', padx=(4, 8))
        tk.Button(top, text='↻', command=self._refresh_ports,
                  bg='#3c3c3c', fg='white', font=('Consolas', 10),
                  relief='flat', padx=6, cursor='hand2').pack(side='left', padx=(0, 10))

        self.btn_connect = tk.Button(top, text='CONNECT',
                                      command=self._do_connect,
                                      bg='#0078d4', fg='white',
                                      font=('Segoe UI', 10, 'bold'),
                                      relief='flat', padx=16, pady=4,
                                      cursor='hand2', width=12)
        self.btn_connect.pack(side='left', padx=(0, 6))

        self.btn_read = tk.Button(top, text='READ FLASH',
                                   command=self._do_read,
                                   bg='#555555', fg='#888888',
                                   font=('Segoe UI', 10, 'bold'),
                                   relief='flat', padx=16, pady=4,
                                   cursor='hand2', width=12,
                                   state='disabled')
        self.btn_read.pack(side='left', padx=(0, 6))

        # Mode switch: FLASHER (SD card + write) / DIAGNOSTICS (live data,
        # fault codes). Shown on the right-hand panel.
        mode_frame = tk.Frame(top, bg='#1e1e1e', padx=3, pady=3)
        mode_frame.pack(side='right', padx=(12, 0))   # far right of the bar
        self.btn_mode_flash = tk.Button(mode_frame, text='FLASHER',
                                         command=lambda: self._set_mode('flasher'),
                                         font=('Segoe UI', 9, 'bold'), relief='flat',
                                         padx=12, pady=3, cursor='hand2')
        self.btn_mode_flash.pack(side='left')
        self.btn_mode_diag = tk.Button(mode_frame, text='DIAGNOSTICS',
                                        command=lambda: self._set_mode('diag'),
                                        font=('Segoe UI', 9, 'bold'), relief='flat',
                                        padx=12, pady=3, cursor='hand2')
        self.btn_mode_diag.pack(side='left')

        # Always enabled - also meant for getting out of a stuck operation.
        self.btn_reset = tk.Button(top, text='RESET ESP32',
                                    command=self._do_reset,
                                    bg='#8a6d00', fg='white',
                                    font=('Segoe UI', 10, 'bold'),
                                    relief='flat', padx=12, pady=6,
                                    cursor='hand2')
        self.btn_reset.pack(side='left', padx=(0, 6))

        # Always available - shows the ECU connector pinout and wiring
        self.btn_wiring = tk.Button(top, text='WIRING',
                                     command=self._open_wiring,
                                     bg='#3c3c3c', fg='white',
                                     font=('Segoe UI', 10, 'bold'),
                                     relief='flat', padx=12, pady=6,
                                     cursor='hand2')
        self.btn_wiring.pack(side='left', padx=(0, 6))

        self.status_label = tk.Label(top, text='● Disconnected',
                                      bg='#2d2d2d', fg='#888888',
                                      font=('Consolas', 9, 'bold'))
        self.status_label.pack(side='right', padx=8)

        info_frame = tk.Frame(self.root, bg='#252526', pady=6, padx=10)
        info_frame.pack(fill='x')

        self.vin_var = tk.StringVar(value='---')
        self.cal_var = tk.StringVar(value='---')
        self.date_var = tk.StringVar(value='---')
        self.ver_var = tk.StringVar(value='---')

        for label, var in [
            ('VIN', self.vin_var),
            ('Calibration', self.cal_var),
            ('Date', self.date_var),
            ('Software', self.ver_var),
        ]:
            cell = tk.Frame(info_frame, bg='#252526')
            cell.pack(side='left', padx=(0, 24))
            tk.Label(cell, text=label, bg='#252526', fg='#888888',
                     font=('Consolas', 8)).pack(anchor='w')
            tk.Label(cell, textvariable=var, bg='#252526', fg='#4ec9b0',
                     font=('Consolas', 11, 'bold')).pack(anchor='w')

        pf = tk.Frame(self.root, bg='#1e1e1e', pady=4, padx=10)
        pf.pack(fill='x')
        self.progress_var = tk.DoubleVar()
        self.progress_bar = ttk.Progressbar(pf, variable=self.progress_var,
                                             maximum=100)
        self.progress_bar.pack(side='left', fill='x', expand=True)
        self.progress_label = tk.Label(pf, text='0%', bg='#1e1e1e',
                                        fg='#cccccc', font=('Consolas', 9), width=8)
        self.progress_label.pack(side='right')

        sf = tk.Frame(self.root, bg='#1e1e1e', padx=10)
        sf.pack(fill='x')
        self.stats_label = tk.Label(sf, text='', bg='#1e1e1e',
                                     fg='#888888', font=('Consolas', 9))
        self.stats_label.pack(side='left')

        main_area = tk.Frame(self.root, bg='#1e1e1e')
        main_area.pack(fill='both', expand=True, padx=10, pady=(4, 10))

        left = tk.Frame(main_area, bg='#1e1e1e')
        left.pack(side='left', fill='both', expand=True, padx=(0, 8))

        right = tk.Frame(main_area, bg='#1e1e1e', width=520)
        right.pack(side='right', fill='y')
        right.pack_propagate(False)

        self._build_log_panel(left)
        self.sd_frame = tk.Frame(right, bg='#1e1e1e')
        self.diag_frame = tk.Frame(right, bg='#1e1e1e')
        self._build_sd_panel(self.sd_frame)
        self._build_diag_panel(self.diag_frame)
        self.sd_frame.pack(fill='both', expand=True)
        self._show_mode_buttons()

    def _build_log_panel(self, parent):
        tk.Label(parent, text='Log:', bg='#1e1e1e', fg='#888888',
                 font=('Consolas', 9)).pack(anchor='w')
        self.log_text = scrolledtext.ScrolledText(
            parent, bg='#0d0d0d', fg='#d4d4d4', font=('Consolas', 9),
            borderwidth=0, highlightthickness=1,
            highlightbackground='#3c3c3c', wrap='none', state='disabled')
        self.log_text.pack(fill='both', expand=True)

        self.log_text.tag_configure('tx',   foreground='#9cdcfe')
        self.log_text.tag_configure('rx',   foreground='#4ec9b0')
        self.log_text.tag_configure('step', foreground='#dcdcaa')
        self.log_text.tag_configure('err',  foreground='#f44747')
        self.log_text.tag_configure('ok',   foreground='#4ec9b0')
        self.log_text.tag_configure('data', foreground='#888888')
        self.log_text.tag_configure('info', foreground='#cccccc')

    def _build_sd_panel(self, parent):
        tk.Label(parent, text='SD CARD', bg='#1e1e1e', fg='#cccccc',
                 font=('Segoe UI', 10, 'bold')).pack(anchor='w', pady=(0, 4))

        # CHANGE: the Listbox's own default width is 20 CHARACTERS,
        # regardless of the surrounding Frame's width - this was the
        # actual reason filenames were getting cut off, not the panel
        # width. Set it explicitly wider.
        self.sd_listbox = tk.Listbox(parent, bg='#0d0d0d', fg='#d4d4d4',
                                      font=('Consolas', 9), height=10, width=60,
                                      selectmode='browse',
                                      selectbackground='#0078d4',
                                      exportselection=False)
        self.sd_listbox.pack(fill='x')

        self.sd_status = tk.Label(parent, text='Waiting for connection...',
                                   bg='#1e1e1e', fg='#888888',
                                   font=('Consolas', 8), wraplength=360,
                                   justify='left')
        self.sd_status.pack(anchor='w', pady=(4, 6))

        btns1 = tk.Frame(parent, bg='#1e1e1e')
        btns1.pack(fill='x', pady=(0, 10))
        self.btn_sd_refresh = tk.Button(btns1, text='Refresh',
                                         command=self._sd_refresh,
                                         bg='#3c3c3c', fg='white',
                                         relief='flat', padx=6, state='disabled')
        self.btn_sd_refresh.pack(side='left')
        self.btn_sd_download = tk.Button(btns1, text='Download',
                                          command=self._sd_download,
                                          bg='#107c10', fg='white',
                                          relief='flat', padx=6, state='disabled')
        self.btn_sd_download.pack(side='left', padx=6)
        self.btn_sd_delete = tk.Button(btns1, text='Delete',
                                        command=self._sd_delete,
                                        bg='#c84b2f', fg='white',
                                        relief='flat', padx=6, state='disabled')
        self.btn_sd_delete.pack(side='left')

        tk.Label(parent, text='Upload a new file to the SD card (from this computer):',
                 bg='#1e1e1e', fg='#cccccc', font=('Consolas', 9)).pack(
                     anchor='w', pady=(6, 4))

        self.upload_progress_var = tk.DoubleVar()
        ttk.Progressbar(parent, variable=self.upload_progress_var,
                         maximum=100).pack(fill='x')
        self.upload_status = tk.Label(parent, text='', bg='#1e1e1e',
                                       fg='#888888', font=('Consolas', 8),
                                       wraplength=500, justify='left')
        self.upload_status.pack(anchor='w', pady=(2, 6))

        self.btn_sd_upload = tk.Button(parent, text='Choose a file from this computer and upload to SD',
                                        command=self._sd_upload,
                                        bg='#0078d4', fg='white', relief='flat',
                                        padx=8, pady=4, state='disabled')
        self.btn_sd_upload.pack(fill='x', pady=(0, 12))

        tk.Frame(parent, bg='#3c3c3c', height=1).pack(fill='x', pady=(0, 12))

        tk.Label(parent, text='Write the selected file into the ECU flash:',
                 bg='#1e1e1e', fg='#cccccc', font=('Consolas', 9)).pack(
                     anchor='w', pady=(0, 6))
        self.btn_sd_write = tk.Button(parent, text='WRITE TO ECU',
                                       command=self._sd_write_start,
                                       bg='#c84b2f', fg='white',
                                       font=('Segoe UI', 9, 'bold'),
                                       relief='flat', padx=8, pady=6,
                                       state='disabled')
        self.btn_sd_write.pack(fill='x')

    def _set_busy(self, busy):
        self.op_busy = busy
        idle = not busy

        def cfg(btn, enabled, on_bg, on_fg='white', off_bg='#555555', off_fg='#888888'):
            btn.config(state='normal' if enabled else 'disabled',
                       bg=on_bg if enabled else off_bg,
                       fg=on_fg if enabled else off_fg)

        # Safety lock: no CONNECT / READ / WRITE while live data runs, no
        # diagnostics while the ECU is in flash mode (CONNECT done).
        flash_ok = idle and not self.live_on
        cfg(self.btn_connect, flash_ok and not self.connected, '#0078d4')
        cfg(self.btn_read, flash_ok and self.connected, '#107c10')
        # CHANGE: SD card buttons only need the ESP32 (serial port open +
        # SD card present), NOT an ECU connection. Refresh is always
        # available so the list can be (re)loaded at any time. Only
        # WRITE TO ECU still requires CONNECT.
        esp = self._esp_ready()
        cfg(self.btn_sd_refresh, idle, '#3c3c3c')
        cfg(self.btn_sd_download, idle and esp and bool(self.sd_files), '#107c10')
        cfg(self.btn_sd_delete, idle and esp and bool(self.sd_files), '#c84b2f')
        cfg(self.btn_sd_upload, idle and esp and self.sd_ready, '#0078d4')
        cfg(self.btn_sd_write, flash_ok and self.connected and bool(self.sd_files), '#c84b2f')
        self._update_diag_buttons()

    def _esp_ready(self):
        return bool(self.ser and self.ser.is_open)

    def _on_port_changed(self, _event=None):
        # A different port was picked - drop the old connection and load
        # the SD card list from the newly selected ESP32.
        if self.op_busy:
            return
        try:
            if self.ser and self.ser.is_open:
                self.ser.close()
        except Exception:
            pass
        self.ser = None
        self.connected = False
        self.status_label.config(text='● Disconnected', fg='#888888')
        self._sd_refresh()

    def _refresh_ports(self):
        ports = [p.device for p in serial.tools.list_ports.comports()]
        self.port_combo['values'] = ports
        if ports:
            self.port_combo.set(ports[0])

    def _auto_detect(self):
        # Probe USB serial ports only (ESP32 boards show up as USB devices);
        # Bluetooth / virtual COM ports are skipped because opening them can
        # hang for a long time.
        ports = [p for p in serial.tools.list_ports.comports()
                 if p.vid is not None]
        found = None
        for p in ports:
            try:
                s = serial.Serial(p.device, BAUD, timeout=0.3,
                                  write_timeout=0.5)
                time.sleep(0.1)
                s.write(b'PING\n')
                time.sleep(0.2)
                resp = s.read(64).decode('utf-8', errors='ignore')
                s.close()
                if any(x in resp for x in ['PONG', 'CAN_OK', 'READY']):
                    found = p.device
                    break
            except Exception:
                pass
        self.root.after(0, lambda: self._auto_detect_done(found))

    def _auto_detect_done(self, port):
        if port:
            self.port_var.set(port)
            self._log(f'✅ ESP32 found: {port}', 'ok')
            # Load the SD card contents right away - the SD card lives on
            # the ESP32, so it does not need an ECU connection.
            self.root.after(300, self._sd_refresh)
        else:
            self.sd_status.config(
                text='ESP32 not found - pick the port and press Refresh')

    def _log(self, msg, tag='info'):
        self.log_text.configure(state='normal')
        self.log_text.insert('end', msg + '\n', tag)
        self.log_text.see('end')
        self.log_text.configure(state='disabled')
        self.root.update_idletasks()

    def _open_serial(self):
        port = self.port_var.get()
        if not port:
            self._log('ERR: No port selected!', 'err')
            return False
        try:
            if self.ser and self.ser.is_open:
                self.ser.close()
            self.ser = serial.Serial(port, BAUD, timeout=0.1)
            time.sleep(0.5)
            self.ser.reset_input_buffer()
            return True
        except Exception as e:
            self._log(f'ERR: {e}', 'err')
            return False

    def _ensure_serial(self):
        """Makes sure the serial port is open AND the ESP32 is answering.

        Opening the port can restart the ESP32 (auto-reset circuit on
        the USB adapter), so after opening we wait until it answers PING
        before sending anything else. If the port is already open it is
        reused as-is - this avoids an unnecessary restart."""
        if self._esp_ready():
            return True
        if not self._open_serial():
            return False
        deadline = time.time() + 8
        next_ping = 0
        while time.time() < deadline:
            if time.time() >= next_ping:
                self._send('PING')
                next_ping = time.time() + 0.7
            line = self._read_line(timeout=0.3)
            if line is None:
                continue
            if line == 'STATUS:SD_OK':
                self.sd_ready = True
            elif line == 'STATUS:SD_FAIL':
                self.sd_ready = False
                self._log('⚠ SD card not found', 'err')
            if line == 'PONG':
                # Firmware 2.0+ answers VERSION:x.y right after PONG
                nxt = self._read_line(timeout=0.4)
                if nxt and nxt.startswith('VERSION:'):
                    self.fw_version = nxt[len('VERSION:'):].strip()
                    self.root.after(0, self._update_diag_buttons)
                time.sleep(0.1)
                self.ser.reset_input_buffer()
                return True
        self._log('ERR: ESP32 did not answer on ' + self.port_var.get(), 'err')
        return False

    def _send(self, cmd):
        if self.ser and self.ser.is_open:
            self.ser.write((cmd + '\n').encode())

    def _read_line(self, timeout=2.0):
        start = time.time()
        buf = b''
        while time.time() - start < timeout:
            if self.ser and self.ser.in_waiting:
                c = self.ser.read(1)
                if c == b'\n':
                    return buf.decode('utf-8', errors='ignore').strip()
                buf += c
            else:
                time.sleep(0.005)
        return None

    def _ask_on_main(self, func, *args, **kwargs):
        result = {}
        done = threading.Event()

        def wrapper():
            try:
                result['value'] = func(*args, **kwargs)
            except Exception as e:
                result['error'] = e
            done.set()

        self.root.after(0, wrapper)
        done.wait()
        if 'error' in result:
            raise result['error']
        return result.get('value')

    def _do_connect(self):
        if self.connected or self.op_busy:
            return
        threading.Thread(target=self._connect_thread, daemon=True).start()

    def _connect_thread(self):
        self._set_busy(True)
        self.vin_var.set('---')
        self.cal_var.set('---')
        self.date_var.set('---')
        self.ver_var.set('---')
        self._log('\n---------- Connect Started ----------', 'info')
        connect_start = time.time()
        # CHANGE: reuse the serial port if it is already open (it usually
        # is, since the SD list is loaded on startup) instead of
        # re-opening it, which could restart the ESP32.
        if not self._ensure_serial():
            self._set_busy(False)
            return

        self._send('CONNECT')
        self.status_label.config(text='● Connecting...', fg='#f0a500')

        gen = self.reset_gen
        timeout = time.time() + 90
        while time.time() < timeout and gen == self.reset_gen:
            line = self._read_line(timeout=3.0)
            if line is None:
                continue

            if line.startswith('TX:'):
                self._log(line, 'tx')
            elif line.startswith('RX:'):
                self._log(line, 'rx')
            elif line.startswith('STEP:'):
                self._log(line, 'step')
            elif line.startswith('INFO:VIN:'):
                val = line[len('INFO:VIN:'):]
                self.vin_var.set(val)
                self._log(f'VIN: {val}', 'ok')
            elif line.startswith('INFO:CAL:'):
                val = line[len('INFO:CAL:'):]
                self.cal_var.set(val)
                self._log(f'Calibration: {val}', 'ok')
            elif line.startswith('INFO:DATE:'):
                val = line[len('INFO:DATE:'):]
                self.date_var.set(val)
                self._log(f'Date: {val}', 'ok')
            elif line.startswith('INFO:VER:'):
                val = line[len('INFO:VER:'):]
                self.ver_var.set(val)
                self._log(f'Software version: {val}', 'ok')
            elif line == 'ERR:LOCKED:DIAG':
                self._log('🔒 Flasher locked - diagnostics is running. Stop live data first.', 'err')
                break
            elif line.startswith('ERR:'):
                self._log(line, 'err')
            elif line == 'STATUS:SD_OK':
                self.sd_ready = True
                self._log('✅ SD card found', 'ok')
            elif line == 'STATUS:SD_FAIL':
                self.sd_ready = False
                self._log('⚠ SD card not found', 'err')
                self.sd_status.config(text='SD card not found')
            elif line.startswith('STATUS:WIFI_AP_IP:'):
                self._log(line, 'ok')
            elif line == 'STATUS:WEBSERVER_STARTED':
                self._log(line, 'ok')
            elif line == 'STATUS:CONNECTED':
                # Short summary of the ECU identification, like commercial tools show
                elapsed = int(time.time() - connect_start)
                self._log('', 'info')
                self._log(f'VIN:          {self.vin_var.get()}', 'ok')
                self._log(f'HW:           {self.cal_var.get()}', 'ok')
                self._log(f'HW version:   {self.ver_var.get()}', 'ok')
                self._log(f'Date:         {self.date_var.get()}', 'ok')
                self._log('CPU Type:     MC9S12 (M3C)', 'ok')
                self._log('', 'info')
                self._log(f'---------- Connect Finished ( Time: '
                          f'{elapsed // 3600:02d}:{elapsed % 3600 // 60:02d}:{elapsed % 60:02d} ) '
                          f'----------', 'ok')
                self.connected = True
                self.status_label.config(text='● Connected', fg='#4ec9b0')
                self._set_busy(False)
                threading.Thread(target=self._sd_refresh_thread, daemon=True).start()
                return
            elif line == 'STATUS:CONNECT_FAILED':
                self._log('❌ Connect failed!', 'err')
                elapsed = int(time.time() - connect_start)
                self._log(f'---------- Connect Finished with error ( Time: '
                          f'{elapsed // 3600:02d}:{elapsed % 3600 // 60:02d}:{elapsed % 60:02d} ) '
                          f'----------', 'err')
                self.status_label.config(text='● Error', fg='#f44747')
                break
            elif line:
                self._log(line, 'info')

        self._set_busy(False)

    # =================================================================
    # ESP32 RESET
    #
    # Resets the board through the USB-serial adapter's RTS line (EN pin),
    # exactly like the Arduino IDE / esptool do. This works even if the
    # firmware is stuck in a long operation and not reading Serial.
    # If the board does not come back (some boards have no auto-reset
    # circuit), the "RESET" serial command is sent as a fallback.
    # =================================================================
    def _do_reset(self):
        msg = ('Restart the ESP32?\n\n'
               'Any operation in progress will be stopped and the ECU '
               'connection is lost - you will need to press CONNECT again.')
        if self.op_busy:
            msg += ('\n\nAn operation is running right now. Do NOT reset during '
                    'an ECU write unless the write is already stuck - '
                    'interrupting a write can leave the ECU unable to start '
                    'until it is written again.')
        if not messagebox.askyesno('Reset ESP32', msg):
            return
        self.reset_gen += 1
        threading.Thread(target=self._reset_thread, daemon=True).start()

    def _wait_for_boot(self, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self._read_line(timeout=0.5)
            if line is None:
                continue
            if line.startswith('STATUS:'):
                self._log(line, 'step')
            if line in ('STATUS:READY', 'STATUS:CAN_OK', 'STATUS:CAN_FAIL'):
                return True
        return False

    def _reset_thread(self):
        self._log('\n---------- Resetting ESP32 ----------', 'info')
        self.status_label.config(text='● Resetting...', fg='#f0a500')

        # Forget all ECU/SD state - after a reset nothing is connected
        self.connected = False
        self.reading = False
        self.sd_files = []
        self.sd_listbox.delete(0, 'end')
        self.sd_status.config(text='Waiting for connection...')
        for v in (self.vin_var, self.cal_var, self.date_var, self.ver_var):
            v.set('---')
        self.progress_var.set(0)
        self.progress_label.config(text='0%')
        self.stats_label.config(text='')

        if not (self.ser and self.ser.is_open):
            if not self._open_serial():
                self.status_label.config(text='● Error', fg='#f44747')
                self._set_busy(False)
                return

        booted = False
        try:
            self.ser.reset_input_buffer()
            # Hardware reset: DTR high (IO0 stays high = normal boot),
            # pulse RTS to pull EN low for 100 ms.
            self.ser.dtr = False
            self.ser.rts = True
            time.sleep(0.1)
            self.ser.rts = False
            self._log('Hardware reset sent (RTS/EN)', 'info')
            booted = self._wait_for_boot(5.0)
        except Exception as e:
            self._log(f'Hardware reset not possible: {e}', 'err')

        if not booted:
            self._log('No boot message seen - sending the RESET command instead', 'info')
            try:
                self.ser.reset_input_buffer()
                self._send('RESET')
                booted = self._wait_for_boot(8.0)
            except Exception as e:
                self._log(f'ERR: {e}', 'err')

        if booted:
            self._log('✅ ESP32 restarted - press CONNECT to connect to the ECU again', 'ok')
            self.status_label.config(text='● Disconnected', fg='#888888')
        else:
            self._log('❌ ESP32 did not answer after reset. Check the USB cable '
                      'or press the EN/RST button on the board.', 'err')
            self.status_label.config(text='● Error', fg='#f44747')
        self._set_busy(False)
        if booted:
            # Show the SD card contents again straight away
            time.sleep(0.5)
            self._sd_refresh_thread()

    def _do_read(self):
        if self.reading or self.op_busy:
            return
        # Ask for the dump file name (e.g. the bike owner's name), so reads
        # of different ECUs are easy to tell apart on the SD card.
        name = simpledialog.askstring(
            'Read Flash',
            'File name for the read (e.g. the owner\'s name).\n'
            'Leave empty for flash_dump. ".bin" is added automatically.',
            parent=self.root)
        if name is None:  # Cancel
            return
        threading.Thread(target=self._read_thread, args=(False, name.strip()),
                         daemon=True).start()

    def _read_thread(self, no_handshake=False, name=''):
        self.reading = True
        self._set_busy(True)
        self.progress_var.set(0)
        self.progress_label.config(text='0%')
        self.stats_label.config(text='')
        self._log('\n---------- Read Flash Started ----------'
                   + (' (WITHOUT handshake - test)' if no_handshake else ''), 'info')
        self._log('Section 1: 0x7800-0x79FF | Section 2: 0x7E00-0x7FFF', 'info')
        self._log('Saves automatically to the SD card', 'info')
        cmd = 'READ_NOHS' if no_handshake else 'READ'
        self._send(f'{cmd}:{name}' if name else cmd)

        done = 0
        errors = 0

        gen = self.reset_gen
        timeout = time.time() + 300
        while time.time() < timeout:
            if gen != self.reset_gen:
                self._log('Read stopped - ESP32 was reset', 'err')
                return
            line = self._read_line(timeout=5.0)
            if line is None:
                self._log('ERR: Timeout!', 'err')
                break

            if line.startswith('DATA:'):
                try:
                    parts = line.split(':', 2)
                    addr = int(parts[1], 16)
                    done += 1
                    pct = done / TOTAL_ADDRS * 100
                    self.progress_var.set(min(pct, 100))
                    self.progress_label.config(text=f'{pct:.1f}%')
                    if done % 128 == 0:
                        self._log(f'DATA:0x{addr:04X}: {parts[2][:32]}...', 'data')
                        self.stats_label.config(
                            text=f'Read: {done}/{TOTAL_ADDRS} | '
                                 f'Errors: {errors} | Address: 0x{addr:04X}')
                except Exception as e:
                    self._log(f'Parse ERR: {e}', 'err')

            elif line.startswith('ERR:NO_RESP:'):
                errors += 1
                if errors % 20 == 1:
                    self._log(line, 'err')

            elif line.startswith('ERR:SEND:'):
                errors += 1
                self._log(line, 'err')

            elif line.startswith('STEP:SD_DUMP'):
                self._log(line, 'step')

            elif line.startswith('WARN:INCOMPLETE_READ:'):
                self._log(
                    f'⚠ {line.split(":")[-1]} addresses could not be read - '
                    f'these spots in the file are 0xFF placeholders, NOT '
                    f'confirmed real content!', 'err')

            elif line.startswith('WARN:FAILED_ADDRS:'):
                self._log(f'  Missing addresses: {line[len("WARN:FAILED_ADDRS:"):]}', 'err')

            elif line.startswith('ERR:SD'):
                self._log(line, 'err')

            elif line.startswith('PROGRESS:'):
                # CHANGE: the firmware sends "PROGRESS:<done>/<total>"
                # (slash, not colon) - the old code's split(':')
                # condition was NEVER satisfied, which is why the
                # progress bar never moved during Write.
                try:
                    rest = line[len('PROGRESS:'):]
                    d_str, _t_str = rest.split('/')
                    d = int(d_str)
                    pct = d / TOTAL_ADDRS * 100
                    self.progress_var.set(min(pct, 100))
                    self.progress_label.config(text=f'{pct:.1f}%')
                except Exception:
                    pass

            elif line.startswith('ERR:'):
                self._log(line, 'err')

            elif line.startswith('STATUS:READ_DONE'):
                # FIX: use the firmware's own final numbers
                # ("STATUS:READ_DONE:<read>:ERR:<errors>") - counting lines
                # here missed the case where nothing could be read at all,
                # and then wrongly reported "0 errors, file saved".
                try:
                    parts = line.split(':')
                    done = int(parts[2])
                    errors = int(parts[4])
                except (IndexError, ValueError):
                    pass
                if done == 0:
                    self.progress_var.set(0)
                    self.progress_label.config(text='0%')
                    self._log('❌ Read failed - the ECU did not answer, nothing was '
                              'read and no file was saved.', 'err')
                elif errors == 0:
                    self.progress_var.set(100)
                    self.progress_label.config(text='100%')
                    self._log(
                        f'✅ Read Done! Read: {done} | Errors: 0 - '
                        f'the whole file is 100% confirmed real ECU content.', 'ok')
                    self._log('File saved to the SD card - check the SD panel on the right', 'ok')
                else:
                    self.progress_var.set(100)
                    self.progress_label.config(text='100%')
                    self._log(
                        f'⚠ Read Done! Read: {done} | Errors: {errors} - '
                        f'see above which spots are placeholders.', 'err')
                    self._log('File saved to the SD card - check the SD panel on the right', 'ok')
                break

            elif line:
                self._log(line, 'info')

        self.reading = False

        # CHANGE: if LITERALLY not a single address succeeded (the ECU
        # doesn't respond at all - e.g. the "no response" situation
        # right after a Write), we treat the connection as lost, so
        # the CONNECT button turns green again and the user can retry
        # (after a power cycle).
        if done == 0 and errors > 0:
            self.connected = False
            self.status_label.config(text='● Connection lost', fg='#f44747')
            self._log('⚠ The ECU did not respond to any request - the connection is '
                       'considered lost. Power-cycle it and press CONNECT again.', 'err')

        self._set_busy(False)
        self.stats_label.config(
            text=f'Total: {done}/{TOTAL_ADDRS} addresses | Errors: {errors}')
        if self._esp_ready():
            threading.Thread(target=self._sd_refresh_thread, daemon=True).start()

    def _sd_refresh(self):
        if self.op_busy:
            return
        threading.Thread(target=self._sd_refresh_thread, daemon=True).start()

    def _sd_refresh_thread(self):
        self._set_busy(True)
        self.sd_status.config(text='Connecting to ESP32...')
        if not self._ensure_serial():
            self.sd_status.config(text='ESP32 not found - check the port and press Refresh')
            self._set_busy(False)
            return
        self.sd_status.config(text='Loading file list...')
        self._send('SDLIST')
        self.sd_files = []
        self.sd_listbox.delete(0, 'end')
        deadline = time.time() + 8
        while time.time() < deadline:
            line = self._read_line(timeout=1.0)
            if line is None:
                continue
            if line == 'SDLIST_DONE':
                self.sd_ready = True
                break
            if line.startswith('SDFILE:'):
                parts = line.split(':')
                if len(parts) >= 3:
                    name, size = parts[1], parts[2]
                    self.sd_files.append(name)
                    self.sd_listbox.insert('end', f'{name}  ({size} B)')
            elif line.startswith('ERR:'):
                if line in ('ERR:SD_NOT_READY', 'ERR:SD_LIST_FAIL'):
                    self.sd_ready = False
                    self.sd_status.config(text='SD card not found - insert it and press RESET ESP32')
                else:
                    self.sd_status.config(text=line)
                self._set_busy(False)
                return
        self.sd_status.config(text=f'{len(self.sd_files)} files found')
        self._set_busy(False)

    def _sd_selected_filename(self):
        sel = self.sd_listbox.curselection()
        if not sel:
            return None
        return self.sd_files[sel[0]]

    def _sd_download(self):
        if self.op_busy or not self._esp_ready():
            return
        filename = self._sd_selected_filename()
        if not filename:
            self.sd_status.config(text='Select a file from the list!')
            return
        threading.Thread(target=self._sd_download_thread, args=(filename,), daemon=True).start()

    def _sd_download_thread(self, filename):
        self._set_busy(True)
        self.sd_status.config(text=f'Downloading: {filename}...')
        self._send(f'SDGET:{filename}')

        expected_size = None
        chunks = []
        received = 0
        last_progress = time.time()
        silence_timeout = 20
        got_done = False

        gen = self.reset_gen
        while time.time() - last_progress < silence_timeout:
            if gen != self.reset_gen:
                return
            line = self._read_line(timeout=2.0)
            if line is None:
                continue
            last_progress = time.time()
            if line.startswith('SDSIZE:'):
                expected_size = int(line.split(':')[1])
            elif line.startswith('SDDATA:'):
                hexpart = line[7:]
                chunks.append(bytes.fromhex(hexpart))
                received += len(hexpart) // 2
                if expected_size:
                    pct = received / expected_size * 100
                    self.sd_status.config(
                        text=f'Downloading: {filename} ({received}/{expected_size} B, {pct:.0f}%)')
            elif line == 'SDGET_DONE':
                got_done = True
                break
            elif line.startswith('ERR:'):
                self.sd_status.config(text=line)
                self._set_busy(False)
                return

        if not got_done:
            self.sd_status.config(text=f'ERR: Timeout at {received} bytes')
            self._set_busy(False)
            return

        content = b''.join(chunks)
        if expected_size is not None and len(content) != expected_size:
            self.sd_status.config(
                text=f'⚠ Size mismatch: got {len(content)}, expected {expected_size}')

        def do_save():
            save_path = filedialog.asksaveasfilename(
                title='Save SD file to this computer',
                initialfile=filename.lstrip('/'),
                defaultextension='.bin',
                filetypes=[('Binary', '*.bin'), ('All files', '*.*')])
            if save_path:
                with open(save_path, 'wb') as f:
                    f.write(content)
                self.sd_status.config(text=f'✅ Saved: {save_path} ({len(content)} B)')
            else:
                self.sd_status.config(text='Save cancelled')

        self._ask_on_main(do_save)
        self._set_busy(False)

    def _sd_delete(self):
        if self.op_busy or not self._esp_ready():
            return
        filename = self._sd_selected_filename()
        if not filename:
            self.sd_status.config(text='Select a file from the list!')
            return
        threading.Thread(target=self._sd_delete_thread, args=(filename,), daemon=True).start()

    def _sd_delete_thread(self, filename):
        def confirm():
            return messagebox.askyesno('Delete file', f'Delete {filename} from the SD card?')
        if not self._ask_on_main(confirm):
            return
        self._set_busy(True)
        self._send(f'SDDEL:{filename}')
        deadline = time.time() + 3
        while time.time() < deadline:
            line = self._read_line(timeout=1.0)
            if line is None:
                continue
            if line in ('STATUS:SD_DELETED', 'ERR:SD_DELETE_FAIL'):
                self.sd_status.config(text=line)
                break
        self._set_busy(False)
        self._sd_refresh_thread()

    def _sd_upload(self):
        if self.op_busy or not self._esp_ready():
            return
        threading.Thread(target=self._sd_upload_thread, daemon=True).start()

    def _sd_upload_thread(self):
        local_path = self._ask_on_main(
            filedialog.askopenfilename,
            title='Select a BIN file to upload',
            filetypes=[('Binary', '*.bin'), ('All files', '*.*')])
        if not local_path:
            return

        self._set_busy(True)
        try:
            with open(local_path, 'rb') as lf:
                data = lf.read()
            target_name = os.path.basename(local_path)

            if len(data) != BIN_SIZE:
                self.upload_status.config(
                    text=f'⚠ File size is {len(data)} B, expected {BIN_SIZE} B '
                         f'- writing with this file would later be refused')

            self.upload_status.config(text=f'Starting: {target_name} ({len(data)} B)...')
            self._send(f'WUPLOAD:{target_name}:{len(data)}')
            deadline = time.time() + 5
            ready = False
            while time.time() < deadline:
                line = self._read_line(timeout=1.0)
                if line is None:
                    continue
                if line == 'STATUS:UPLOAD_READY':
                    ready = True
                    break
                if line.startswith('ERR:'):
                    self.upload_status.config(text=line)
                    return
            if not ready:
                self.upload_status.config(text='ERR: ESP32 did not confirm the upload')
                return

            total_chunks = (len(data) + UPLOAD_CHUNK_SIZE - 1) // UPLOAD_CHUNK_SIZE
            for i in range(total_chunks):
                chunk = data[i * UPLOAD_CHUNK_SIZE:(i + 1) * UPLOAD_CHUNK_SIZE]
                self._send(f'WDATA:{chunk.hex().upper()}')
                deadline2 = time.time() + 3
                got_ok = False
                while time.time() < deadline2:
                    line = self._read_line(timeout=1.0)
                    if line is None:
                        continue
                    if line == 'STATUS:CHUNK_OK':
                        got_ok = True
                        break
                    if line.startswith('ERR:'):
                        self.upload_status.config(text=f'{line} (chunk {i+1}/{total_chunks})')
                        return
                if not got_ok:
                    self.upload_status.config(text=f'ERR: Timeout at chunk {i+1}/{total_chunks}')
                    return
                pct = (i + 1) / total_chunks * 100
                self.upload_progress_var.set(pct)
                self.upload_status.config(text=f'Uploading: {i+1}/{total_chunks} ({pct:.1f}%)')

            self._send('WUPLOAD_DONE')
            deadline = time.time() + 5
            while time.time() < deadline:
                line = self._read_line(timeout=1.0)
                if line is None:
                    continue
                if line == 'STATUS:UPLOAD_DONE':
                    self.upload_status.config(text=f'✅ Done: {target_name}')
                    break
                if line.startswith('ERR:'):
                    self.upload_status.config(text=line)
                    break
        finally:
            self._set_busy(False)
            self._sd_refresh_thread()

    def _sd_write_start(self):
        if self.op_busy or not self.connected:
            return
        filename = self._sd_selected_filename()
        if not filename:
            self.sd_status.config(text='Select a file from the list!')
            return
        threading.Thread(target=self._sd_write_thread, args=(filename,), daemon=True).start()

    def _sd_write_thread(self, filename):
        def confirm():
            return messagebox.askyesno(
                'WRITE ECU FLASH',
                f'Write the contents of "{filename}" into the entire ECU flash?\n\n'
                f'This is a 2306-step process (erase + write for both '
                f'sections) and takes several minutes.\n\n'
                f'THIS PERMANENTLY CHANGES THE ECU! Continue?')
        if not self._ask_on_main(confirm):
            return

        self._set_busy(True)
        self.progress_var.set(0)
        self.progress_label.config(text='0%')
        self.status_label.config(text='● Writing...', fg='#f0a500')
        self._log(f'\n---------- Writing {filename} ----------', 'info')

        try:
            self._send(f'WRITEFULL:{filename}')
            gen = self.reset_gen
            deadline = time.time() + 600
            done = False
            ok = False
            while time.time() < deadline:
                if gen != self.reset_gen:
                    self._log('Write monitoring stopped - ESP32 was reset', 'err')
                    return
                line = self._read_line(timeout=5.0)
                if line is None:
                    continue

                if line.startswith('ERR:WRITEFULL_BAD_FILESIZE:'):
                    self._log(line, 'err')
                    self._log('❌ Wrong file size - writing refused', 'err')
                    done = True
                    break
                elif line.startswith('ERR:WRITEFULL_NO_CHECKSUM_RECORDS'):
                    self._log(line, 'err')
                    self._log('❌ No M3C checksum record was found in the file - '
                               'it does not look like a real calibration file. '
                               'Writing refused.', 'err')
                    done = True
                    break
                elif line.startswith('ERR:WRITEFULL_CHECKSUM_SELFCHECK_FAIL'):
                    self._log(line, 'err')
                    self._log('❌ Self-check of the corrected file failed - '
                               'writing refused for safety. The broken copy '
                               'was removed from the SD card.', 'err')
                    done = True
                    break
                elif line.startswith('ERR:WRITEFULL_CHECKSUM_'):
                    self._log(line, 'err')
                    self._log('❌ Could not create the corrected file on the SD '
                               'card - writing refused. Check the SD card '
                               '(free space, card seated properly).', 'err')
                    done = True
                    break
                elif line.startswith('STEP:Checksum:'):
                    # EXAMPLE: "STEP:Checksum:0x6804:stored=0xFF0C:computed=0x13A7:BAD"
                    tag = 'ok' if line.endswith(':OK') else 'err'
                    self._log('  ' + line[len('STEP:Checksum:'):], tag)
                elif line == 'STEP:WriteFull_ChecksumAllOK':
                    self._log('✅ Checksum check: all good, writing the original file.', 'ok')
                elif line == 'STEP:WriteFull_ChecksumBad_Recalculating':
                    self._log('⚠ Checksum is wrong! Computing the correct value and '
                               'creating a new, corrected file on the SD card...', 'err')
                elif line.startswith('STEP:WriteFull_ChecksumFixed:'):
                    fixed_name = line[len('STEP:WriteFull_ChecksumFixed:'):]
                    self._log(f'✅ Corrected file is ready and passed self-check: '
                               f'{fixed_name}', 'ok')
                elif line.startswith('STATUS:WRITEFULL_USING_FIXED_FILE:'):
                    fixed_name = line[len('STATUS:WRITEFULL_USING_FIXED_FILE:'):]
                    self._log(f'ℹ Writing the CORRECTED file to the ECU (not the original): '
                               f'{fixed_name}', 'ok')
                elif line.startswith('STEP:'):
                    self._log(line, 'step')
                elif line.startswith('ERR:'):
                    self._log(line, 'err')
                elif line.startswith('PROGRESS:'):
                    # CHANGE: same fix as for READ - the firmware uses
                    # a slash, not a colon.
                    try:
                        rest = line[len('PROGRESS:'):]
                        d_str, _t_str = rest.split('/')
                        d = int(d_str)
                        pct = d / WRITEFULL_TOTAL_STEPS * 100
                        self.progress_var.set(min(pct, 100))
                        self.progress_label.config(text=f'{pct:.1f}%')
                        self.stats_label.config(
                            text=f'Written: {d}/{WRITEFULL_TOTAL_STEPS}')
                    except Exception:
                        pass
                elif line == 'STATUS:WRITEFULL_DONE':
                    self.progress_var.set(100)
                    self.progress_label.config(text='100%')
                    self._log('✅ Write complete!', 'ok')
                    done = True
                    ok = True
                    break
                elif line == 'STATUS:WRITEFULL_FAILED':
                    self._log('❌ Write failed!', 'err')
                    done = True
                    break

            if not done:
                self._log('ERR: Timeout', 'err')

            self.status_label.config(
                text='● Written' if ok else '● Error',
                fg='#4ec9b0' if ok else '#f44747')
        finally:
            self._set_busy(False)
            # CHANGE: the checksum check can create a new "_CHK_OK"
            # file on the SD card (even if the write itself later
            # fails) - always refresh the list so the user sees it
            # immediately in the panel on the right.
            if self._esp_ready():
                threading.Thread(target=self._sd_refresh_thread, daemon=True).start()

    def _open_wiring(self):
        win = tk.Toplevel(self.root)
        win.title('Wiring diagram')
        win.configure(bg='#1e1e1e')
        win.resizable(False, False)

        tk.Label(win, text='ECU connector A (front view)', bg='#1e1e1e', fg='#cccccc',
                 font=('Segoe UI', 11, 'bold')).pack(anchor='w', padx=12, pady=(12, 6))
        try:
            img = tk.PhotoImage(data=WIRING_PNG_B64)
            lbl = tk.Label(win, image=img, bg='#1e1e1e')
            lbl.image = img  # keep a reference, otherwise Tk drops the image
            lbl.pack(padx=12)
        except Exception as e:
            tk.Label(win, text=f'(image could not be shown: {e})', bg='#1e1e1e',
                     fg='#f44747', font=('Consolas', 9)).pack(padx=12)

        def table(parent, title, rows):
            tk.Label(parent, text=title, bg='#1e1e1e', fg='#cccccc',
                     font=('Segoe UI', 10, 'bold')).pack(anchor='w', padx=12, pady=(12, 4))
            frm = tk.Frame(parent, bg='#1e1e1e')
            frm.pack(anchor='w', padx=12)
            return frm

        frm = table(win, 'ECU side', WIRING_ECU)
        for r, (pin, cname, col, func, to) in enumerate(WIRING_ECU):
            tk.Label(frm, text='  ', bg=col, relief='solid', bd=1).grid(
                row=r, column=0, padx=(0, 8), pady=2)
            tk.Label(frm, text=pin, bg='#1e1e1e', fg='white',
                     font=('Consolas', 10, 'bold'), width=4, anchor='w').grid(row=r, column=1, sticky='w')
            tk.Label(frm, text=f'{func} ({cname})', bg='#1e1e1e', fg='#4ec9b0',
                     font=('Consolas', 10), width=18, anchor='w').grid(row=r, column=2, sticky='w')
            tk.Label(frm, text='->  ' + to, bg='#1e1e1e', fg='#d4d4d4',
                     font=('Consolas', 10), anchor='w').grid(row=r, column=3, sticky='w')

        frm = table(win, 'ESP32 side', WIRING_ESP32)
        for r, (pin, to) in enumerate(WIRING_ESP32):
            tk.Label(frm, text=pin, bg='#1e1e1e', fg='white',
                     font=('Consolas', 10, 'bold'), width=12, anchor='w').grid(row=r, column=0, sticky='w')
            tk.Label(frm, text='->  ' + to, bg='#1e1e1e', fg='#d4d4d4',
                     font=('Consolas', 10), anchor='w').grid(row=r, column=1, sticky='w')

        tk.Label(win, text='⚠ The ESP32/transceiver GND MUST be connected to ECU GND (M4), '
                           'otherwise CAN will not work.\n'
                           '⚠ CAN bus speed is 500 kbit/s. Make sure the connector and cables '
                           'cannot move while writing.',
                 bg='#1e1e1e', fg='#f0a500', font=('Consolas', 9),
                 justify='left', wraplength=700).pack(anchor='w', padx=12, pady=(12, 12))

    # =================================================================
    # MODE SWITCH (FLASHER / DIAGNOSTICS)
    # =================================================================
    def _show_mode_buttons(self):
        diag = self.mode == 'diag'
        self.btn_mode_flash.config(bg='#3c3c3c' if diag else '#ff6a13',
                                   fg='white' if diag else '#1e1e1e')
        self.btn_mode_diag.config(bg='#4ec9b0' if diag else '#3c3c3c',
                                  fg='#1e1e1e' if diag else 'white')

    def _set_mode(self, mode):
        if mode == self.mode:
            return
        if mode == 'flasher' and self.live_on:
            # Leaving diagnostics always stops live data, so the flasher
            # is free again.
            threading.Thread(target=self._live_stop_sync, daemon=True).start()
        self.mode = mode
        if mode == 'diag':
            self.sd_frame.pack_forget()
            self.diag_frame.pack(fill='both', expand=True)
        else:
            self.diag_frame.pack_forget()
            self.sd_frame.pack(fill='both', expand=True)
        self._show_mode_buttons()
        self._update_diag_buttons()

    # =================================================================
    # DIAGNOSTICS PANEL
    #
    # Uses the diagnostics commands of firmware 2.0 (see
    # firmware/diagnostics.h): DIAG_LIVE:1/0, INFO, DTC_READ,
    # DTC_CLEAR:OBD, ECU_RELEASE. Read-only except CLEAR CODES.
    # =================================================================
    LIVE_TILES = [
        ('THR', 'Throttle', '%'), ('CLT', 'Coolant', '°C'), ('LOAD', 'Engine load', '%'),
        ('ADV', 'Ignition adv.', '°'), ('IAT', 'Intake air', '°C'), ('MAP', 'Intake press.', 'kPa'),
        ('STFT', 'Short fuel trim', '%'), ('LTFT', 'Long fuel trim', '%'), ('BARO', 'Baro press.', 'kPa'),
    ]

    def _build_diag_panel(self, parent):
        bg = '#1e1e1e'
        card = '#252526'
        style = ttk.Style()
        style.configure('Diag.Treeview', background='#0d0d0d', fieldbackground='#0d0d0d',
                        foreground='#d4d4d4', rowheight=22, font=('Consolas', 9))
        style.configure('Diag.Treeview.Heading', background='#2d2d2d', foreground='#cccccc',
                        font=('Segoe UI', 9, 'bold'))
        style.map('Diag.Treeview', background=[('selected', '#0078d4')])

        head = tk.Frame(parent, bg=bg)
        head.pack(fill='x')
        tk.Label(head, text='DIAGNOSTICS', bg=bg, fg='#4ec9b0',
                 font=('Segoe UI', 10, 'bold')).pack(side='left')
        self.btn_end_session = tk.Button(head, text='END FLASH SESSION',
                                         command=self._end_flash_session,
                                         bg='#555555', fg='#888888', relief='flat',
                                         font=('Segoe UI', 8, 'bold'), padx=6,
                                         state='disabled')
        self.btn_end_session.pack(side='right')
        self.diag_lock_label = tk.Label(parent, text='', bg=bg, fg='#f0a500',
                                        font=('Consolas', 9), wraplength=500,
                                        justify='left')
        self.diag_lock_label.pack(anchor='w', pady=(2, 4))

        # ---- live data ----
        live = tk.Frame(parent, bg=card, padx=10, pady=8)
        live.pack(fill='x')
        top = tk.Frame(live, bg=card)
        top.pack(fill='x')
        self.live_rpm_var = tk.StringVar(value='–')
        tk.Label(top, textvariable=self.live_rpm_var, bg=card, fg='white',
                 font=('Consolas', 26, 'bold')).pack(side='left')
        tk.Label(top, text=' rpm', bg=card, fg='#888888',
                 font=('Consolas', 11)).pack(side='left', anchor='s', pady=(0, 6))
        self.live_state_label = tk.Label(top, text='stopped', bg=card, fg='#888888',
                                         font=('Consolas', 9))
        self.live_state_label.pack(side='right')

        grid = tk.Frame(live, bg=card)
        grid.pack(fill='x', pady=(4, 0))
        self.live_vars = {}
        for n, (key, label, unit) in enumerate(self.LIVE_TILES):
            cell = tk.Frame(grid, bg=card)
            cell.grid(row=n // 3, column=n % 3, sticky='w', padx=(0, 18), pady=2)
            tk.Label(cell, text=label, bg=card, fg='#888888',
                     font=('Consolas', 8)).pack(anchor='w')
            var = tk.StringVar(value='–')
            self.live_vars[key] = (var, unit)
            tk.Label(cell, textvariable=var, bg=card, fg='#4ec9b0',
                     font=('Consolas', 12, 'bold')).pack(anchor='w')
        self.live_mil_label = tk.Label(live, text='Check-engine light (MIL): –   Run time: –',
                                       bg=card, fg='#cccccc', font=('Consolas', 9))
        self.live_mil_label.pack(anchor='w', pady=(4, 2))
        self.live_faults_label = tk.Label(live, text='', bg=card, fg='#f44747',
                                          font=('Consolas', 9), wraplength=480,
                                          justify='left')
        self.live_faults_label.pack(anchor='w')
        self.btn_live = tk.Button(live, text='START LIVE DATA', command=self._live_toggle,
                                  bg='#4ec9b0', fg='#1e1e1e', relief='flat',
                                  font=('Segoe UI', 9, 'bold'), pady=4)
        self.btn_live.pack(fill='x', pady=(6, 0))

        # ---- fault codes ----
        tk.Label(parent, text='Fault codes', bg=bg, fg='#cccccc',
                 font=('Segoe UI', 9, 'bold')).pack(anchor='w', pady=(10, 2))
        tree_frame = tk.Frame(parent, bg=bg)
        tree_frame.pack(fill='x')
        self.dtc_tree = ttk.Treeview(tree_frame, style='Diag.Treeview', height=5,
                                     columns=('code', 'desc', 'blink', 'state'),
                                     show='headings')
        for col, text, width in (('code', 'Code', 62), ('desc', 'Description', 285),
                                 ('blink', 'Blink', 80), ('state', 'Status', 70)):
            self.dtc_tree.heading(col, text=text, anchor='w')
            self.dtc_tree.column(col, width=width, anchor='w', stretch=(col == 'desc'))
        self.dtc_tree.pack(side='left', fill='x', expand=True)
        sb = ttk.Scrollbar(tree_frame, orient='vertical', command=self.dtc_tree.yview)
        sb.pack(side='right', fill='y')
        self.dtc_tree.configure(yscrollcommand=sb.set)
        self.dtc_status = tk.Label(parent, text='Press READ CODES', bg=bg, fg='#888888',
                                   font=('Consolas', 9), wraplength=500, justify='left')
        self.dtc_status.pack(anchor='w', pady=(2, 4))

        btns = tk.Frame(parent, bg=bg)
        btns.pack(fill='x')
        self.btn_dtc_read = tk.Button(btns, text='READ CODES', command=self._diag_read_codes,
                                      bg='#4ec9b0', fg='#1e1e1e', relief='flat',
                                      font=('Segoe UI', 9, 'bold'), padx=10, pady=3)
        self.btn_dtc_read.pack(side='left')
        self.btn_dtc_clear = tk.Button(btns, text='CLEAR CODES', command=self._diag_clear_codes,
                                       bg='#c84b2f', fg='white', relief='flat',
                                       font=('Segoe UI', 9, 'bold'), padx=10, pady=3)
        self.btn_dtc_clear.pack(side='left', padx=6)
        tk.Button(btns, text='Code list...', command=self._open_code_list,
                  bg='#3c3c3c', fg='white', relief='flat', padx=10,
                  pady=3).pack(side='left')
        # ECU info goes into the VIN / calibration bar at the top
        self.btn_info = tk.Button(btns, text='READ ECU INFO', command=self._diag_read_info,
                                  bg='#4ec9b0', fg='#1e1e1e', relief='flat',
                                  font=('Segoe UI', 9, 'bold'), padx=10, pady=3)
        self.btn_info.pack(side='right')

        # Clicking a code shows its full description (the column is narrow)
        def on_select(_event=None):
            sel = self.dtc_tree.selection()
            if sel:
                code, desc, blink, state = self.dtc_tree.item(sel[0], 'values')
                self.dtc_status.config(
                    text=f'{code}: {desc}' + (f' - blink code {blink}' if blink else '')
                         + f' ({state})', fg='#cccccc')
        self.dtc_tree.bind('<<TreeviewSelect>>', on_select)

    def _update_diag_buttons(self):
        if not hasattr(self, 'btn_live'):
            return
        idle = not self.op_busy

        def cfg(btn, enabled, on_bg, on_fg='white'):
            btn.config(state='normal' if enabled else 'disabled',
                       bg=on_bg if enabled else '#555555',
                       fg=on_fg if enabled else '#888888')

        diag_ok = idle and not self.connected
        cfg(self.btn_live, self.live_on or diag_ok,
            '#c84b2f' if self.live_on else '#4ec9b0',
            'white' if self.live_on else '#1e1e1e')
        self.btn_live.config(text='STOP LIVE DATA' if self.live_on else 'START LIVE DATA')
        cfg(self.btn_dtc_read, diag_ok, '#4ec9b0', '#1e1e1e')
        cfg(self.btn_dtc_clear, diag_ok, '#c84b2f')
        cfg(self.btn_info, diag_ok, '#4ec9b0', '#1e1e1e')
        cfg(self.btn_end_session, idle and self.connected, '#8a6d00')

        if self.connected:
            text = ('🔒 Diagnostics locked: the ECU is in flash mode (CONNECT was done). '
                    'This protects a read/write from being disturbed. When the flash work '
                    'is finished press END FLASH SESSION (after a write: ignition off and '
                    'on first).')
        elif self.live_on:
            text = '🔒 CONNECT / READ / WRITE are locked while live data runs.'
        elif self._esp_ready() and not self.fw_version:
            text = ('⚠ The ESP32 firmware has no diagnostics - upload ktm_m3c_flasher '
                    'version 2.0 or newer.')
        else:
            text = ''
        self.diag_lock_label.config(text=text)

    def _check_diag_firmware(self):
        """Worker thread: True if the ESP32 runs firmware 2.0+ (diagnostics)."""
        if not self.fw_version:
            self._send('PING')
            deadline = time.time() + 1.5
            while time.time() < deadline:
                line = self._read_line(timeout=0.3)
                if line and line.startswith('VERSION:'):
                    self.fw_version = line[len('VERSION:'):].strip()
                    break
        try:
            major = int(self.fw_version.split('.')[0])
        except ValueError:
            major = 0
        if major < 2:
            self._log('⚠ This ESP32 firmware has no diagnostics - upload ktm_m3c_flasher '
                      'version 2.0 or newer.', 'err')
            self.root.after(0, self._update_diag_buttons)
            return False
        return True

    def _diag_op(self, fn):
        """Runs one diagnostics operation in a worker thread. Live data is
        paused meanwhile (only one thread may read the serial port) and
        started again afterwards."""
        if self.op_busy or self.connected:
            return

        def worker():
            was_live = self.live_on
            if was_live:
                self._live_stop_sync()
            self._set_busy(True)
            try:
                if self._ensure_serial() and self._check_diag_firmware():
                    fn()
            except Exception as e:
                self._log(f'ERR: {e}', 'err')
            finally:
                self._set_busy(False)
            if was_live and not self.connected:
                self._live_start_worker()
        threading.Thread(target=worker, daemon=True).start()

    def _read_until(self, done_prefix, on_line, idle_timeout=15.0):
        """Reads lines until one starts with done_prefix. Returns False on
        timeout or when the ESP32 refuses because of the flash lock."""
        gen = self.reset_gen
        while gen == self.reset_gen:
            line = self._read_line(timeout=idle_timeout)
            if line is None:
                self._log('ERR: Timeout - no answer from the ESP32', 'err')
                return False
            if line == 'ERR:LOCKED:FLASH':
                self._log('🔒 Diagnostics locked - the ECU is in flash mode.', 'err')
                return False
            on_line(line)
            if line.startswith(done_prefix):
                return True
        return False

    # ---------------- live data ----------------
    def _live_toggle(self):
        if self.live_on:
            threading.Thread(target=self._live_stop_sync, daemon=True).start()
        else:
            if self.op_busy or self.connected:
                return
            threading.Thread(target=self._live_start_worker, daemon=True).start()

    def _live_start_worker(self):
        self._set_busy(True)
        ok = False
        try:
            if not (self._ensure_serial() and self._check_diag_firmware()):
                return
            self._send('DIAG_LIVE:1')
            deadline = time.time() + 3
            while time.time() < deadline:
                line = self._read_line(timeout=0.5)
                if line is None:
                    continue
                if line == 'STATUS:LIVE_ON':
                    ok = True
                    break
                if line == 'ERR:LOCKED:FLASH':
                    self._log('🔒 Diagnostics locked - the ECU is in flash mode.', 'err')
                    break
            if not ok:
                self._log('ERR: live data did not start', 'err')
        finally:
            if ok:
                self.live_on = True
                self._log('Live data started', 'ok')
            self._set_busy(False)
        if ok:
            self.live_thread = threading.Thread(target=self._live_reader, daemon=True)
            self.live_thread.start()

    def _live_reader(self):
        gen = self.reset_gen
        while self.live_on and gen == self.reset_gen:
            line = self._read_line(timeout=0.5)
            if not line:
                continue
            if line.startswith('LIVE:'):
                self.root.after(0, self._apply_live, line)
            elif line == 'STATUS:LIVE_OFF':
                break
            elif line.startswith('ERR:'):
                self._log(line, 'err')
        if gen != self.reset_gen:
            self.live_on = False
            self.root.after(0, self._live_stopped_ui)

    def _live_stop_sync(self):
        """Stops live data and waits until the reader thread has ended
        (call from a worker thread)."""
        if not self.live_on:
            return
        self.live_on = False
        self._send('DIAG_LIVE:0')
        if self.live_thread and self.live_thread is not threading.current_thread():
            self.live_thread.join(timeout=2)
        deadline = time.time() + 1.0
        while time.time() < deadline:
            line = self._read_line(timeout=0.3)
            if line is None or line == 'STATUS:LIVE_OFF':
                break
        self._log('Live data stopped', 'info')
        self.root.after(0, self._live_stopped_ui)

    def _live_stopped_ui(self):
        self.live_state_label.config(text='stopped', fg='#888888')
        # Old values must not look like live ones
        self.live_rpm_var.set('–')
        for var, _unit in self.live_vars.values():
            var.set('–')
        self.live_mil_label.config(text='Check-engine light (MIL): –   Run time: –', fg='#cccccc')
        self.live_faults_label.config(text='')
        self._set_busy(self.op_busy)

    def _apply_live(self, line):
        if line == 'LIVE:NONE':
            self.live_state_label.config(text='ECU silent - ignition on?', fg='#f0a500')
            return
        self.live_state_label.config(text='● live', fg='#4ec9b0')
        kv = {}
        for part in line[len('LIVE:'):].split(';'):
            if '=' in part:
                k, v = part.split('=', 1)
                kv[k] = v

        def num(key):
            try:
                return float(kv.get(key, '?'))
            except ValueError:
                return None

        rpm = num('RPM')
        self.live_rpm_var.set('–' if rpm is None else f'{rpm:.0f}')
        for key, (var, unit) in self.live_vars.items():
            v = num(key)
            if v is None:
                var.set('–')
            else:
                dec = 1 if key in ('ADV', 'STFT', 'LTFT') else 0
                var.set(f'{v:.{dec}f} {unit}')
        mil = num('MIL')
        dtcn = num('DTCN')
        run = num('RUN')
        mil_txt = '–' if mil is None else ('ON' if mil >= 1 else 'OFF')
        if mil is not None and mil >= 1 and dtcn is not None:
            mil_txt += f' ({dtcn:.0f} code(s) stored)'
        run_txt = '–' if run is None else f'{int(run) // 60}:{int(run) % 60:02d} min'
        self.live_mil_label.config(text=f'Check-engine light (MIL): {mil_txt}   Run time: {run_txt}',
                                   fg='#f44747' if mil and mil >= 1 else '#cccccc')
        # Faults present right now, from the broadcast fault byte (0x6B3).
        # 0x90 is the normal value, bit 0 comes and goes - not a fault.
        faults = []
        try:
            b = int(kv.get('FLT', '?'), 16)
            extra = b & ~0x90 & ~0x01
            if extra & 0x06:
                faults.append('Ambient air pressure / temperature sensor')
            for bit in (3, 5, 6):
                if extra & (1 << bit):
                    faults.append(f'Unknown fault (bit {bit})')
        except ValueError:
            pass
        self.live_faults_label.config(
            text=('Active now: ' + ', '.join(faults)) if faults else '')

    # ---------------- fault codes ----------------
    def _diag_read_codes(self):
        self._diag_op(self._read_codes_worker)

    def _read_codes_worker(self):
        self.root.after(0, lambda: self.dtc_status.config(text='Reading fault codes...', fg='#888888'))
        self._log('\n---------- Read fault codes ----------', 'info')
        self._send('DTC_READ')
        found = {}          # code -> set of methods
        answered = [False]

        def on_line(line):
            if line.startswith('DTC:'):
                p = line.split(':')
                if len(p) >= 4:
                    found.setdefault(p[3], set()).add(p[1])
                self._log(line, 'ok')
            elif line.startswith('DTCCOUNT:'):
                if not line.endswith(':?'):
                    answered[0] = True
                self._log(line, 'data')
            elif line.startswith('DIAG:'):
                self._log(line, 'data')
            elif line.startswith('ERR:'):
                self._log(line, 'err')

        if not self._read_until('DTC_READ_DONE', on_line):
            self.root.after(0, lambda: self.dtc_status.config(text='Read failed - see log', fg='#f44747'))
            return
        rows = []
        for code, methods in found.items():
            desc, blink = describe_dtc(code)
            state = 'PENDING' if methods == {'OBD_07'} else 'STORED'
            rows.append((code, desc, blink, state))
        if not answered[0] and not rows:
            msg, color = 'ECU did not answer - ignition on? cable connected?', '#f44747'
        elif not rows:
            msg, color = '✅ No fault codes stored', '#4ec9b0'
        else:
            msg, color = (f'{len(rows)} fault code(s) found. STORED = in the ECU memory, '
                          f'PENDING = seen once, not confirmed yet.'), '#f44747'
        self._log(msg, 'ok' if color == '#4ec9b0' else 'err')

        def show():
            self.dtc_tree.delete(*self.dtc_tree.get_children())
            for r in rows:
                self.dtc_tree.insert('', 'end', values=r)
            self.dtc_status.config(text=msg, fg=color)
        self.root.after(0, show)

    def _diag_clear_codes(self):
        if self.op_busy or self.connected:
            return
        if not messagebox.askyesno('Clear fault codes',
                                   'Delete all stored fault codes from the ECU memory?\n\n'
                                   'A fault that is still present comes back right away.'):
            return

        def work():
            self._log('\n---------- Clear fault codes ----------', 'info')
            self._send('DTC_CLEAR:OBD')
            result = {}

            def on_line(line):
                if line.startswith('CLEAR:OBD:'):
                    result['r'] = line[len('CLEAR:OBD:'):]
                self._log(line, 'data')

            if not self._read_until('CLEAR:OBD:', on_line):
                return
            if result.get('r') != 'OK':
                self._log(f'❌ Clear failed ({result.get("r")})', 'err')
                self.root.after(0, lambda: self.dtc_status.config(
                    text=f'Clear failed ({result.get("r")})', fg='#f44747'))
                return
            self._log('✅ Fault codes cleared - reading again...', 'ok')
            time.sleep(0.5)
            self._read_codes_worker()
        self._diag_op(work)

    def _open_code_list(self):
        win = tk.Toplevel(self.root)
        win.title('Fault code list')
        win.geometry('720x520')
        win.configure(bg='#1e1e1e')
        q = tk.StringVar()
        row = tk.Frame(win, bg='#1e1e1e')
        row.pack(fill='x', padx=10, pady=8)
        tk.Label(row, text='Search (code, text or blink code):', bg='#1e1e1e',
                 fg='#cccccc', font=('Consolas', 9)).pack(side='left')
        ent = tk.Entry(row, textvariable=q, bg='#3c3c3c', fg='white',
                       insertbackground='white', font=('Consolas', 10))
        ent.pack(side='left', fill='x', expand=True, padx=8)
        tree = ttk.Treeview(win, style='Diag.Treeview', columns=('code', 'desc', 'blink'),
                            show='headings')
        for col, text, width in (('code', 'Code', 70), ('desc', 'Description', 480),
                                 ('blink', 'Blink code', 110)):
            tree.heading(col, text=text, anchor='w')
            tree.column(col, width=width, anchor='w')
        tree.pack(fill='both', expand=True, padx=10, pady=(0, 10))

        def refresh(*_):
            s = q.get().strip().lower()
            tree.delete(*tree.get_children())
            for code in sorted(DTC_DESCRIPTIONS):
                desc, blink = DTC_DESCRIPTIONS[code]
                if not s or s in code.lower() or s in desc.lower() or s in blink.lower():
                    tree.insert('', 'end', values=(code, desc, blink))
        q.trace_add('write', refresh)
        refresh()
        ent.focus_set()

    # ---------------- ECU info / flash session ----------------
    def _diag_read_info(self):
        def work():
            self._log('\n---------- Read ECU info ----------', 'info')
            self._send('INFO')
            no_answer = [False]

            def on_line(line):
                for key, var in (('INFO:VIN:', self.vin_var), ('INFO:CAL:', self.cal_var),
                                 ('INFO:DATE:', self.date_var), ('INFO:VER:', self.ver_var)):
                    if line.startswith(key):
                        val = line[len(key):]
                        self.root.after(0, var.set, val)
                if line == 'ERR:ECU_NO_RESPONSE':
                    no_answer[0] = True
                self._log(line, 'err' if line.startswith('ERR:') else 'ok')

            if self._read_until('INFO_DONE', on_line) and no_answer[0]:
                self._log('❌ ECU not answering - ignition on? cable connected?', 'err')
        self._diag_op(work)

    def _end_flash_session(self):
        if self.op_busy or not self.connected:
            return
        if not messagebox.askyesno(
                'End flash session',
                'Only do this when no read or write is running.\n\n'
                'After a write: switch the ignition OFF, wait 10 seconds and switch it '
                'ON again before using diagnostics.\n\nEnd the flash session now?'):
            return

        def work():
            self._set_busy(True)
            try:
                if not self._ensure_serial():
                    return
                self._send('ECU_RELEASE')
                deadline = time.time() + 3
                while time.time() < deadline:
                    line = self._read_line(timeout=0.5)
                    if line is None:
                        continue
                    if line == 'STATUS:ECU_RELEASED':
                        self.connected = False
                        self.status_label.config(text='● Disconnected', fg='#888888')
                        self._log('Flash session ended - diagnostics can be used', 'ok')
                        break
                    if line == 'ERR:BUSY':
                        self._log('ERR: a read/write is still running', 'err')
                        break
                else:
                    self._log('ERR: no answer - is the firmware version 2.0 or newer?', 'err')
            finally:
                self._set_busy(False)
        threading.Thread(target=work, daemon=True).start()

    def _on_close(self):
        if self.live_on:
            try:
                self._send('DIAG_LIVE:0')
            except Exception:
                pass
            self.live_on = False
        self.root.destroy()

if __name__ == '__main__':
    try:
        root = tk.Tk()
        app = FlasherGUI(root)
        root.mainloop()
    except Exception as e:
        import traceback
        print("ERROR:", e)
        traceback.print_exc()
        input("Press Enter...")