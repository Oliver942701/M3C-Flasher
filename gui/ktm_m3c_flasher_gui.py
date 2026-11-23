#!/usr/bin/env python3
import tkinter as tk
from tkinter import ttk, filedialog, scrolledtext, messagebox
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


def addr_to_bin_offset(addr):
    if 0x7800 <= addr <= 0x79FF:
        return (addr - 0x7800) * 256
    elif 0x7E00 <= addr <= 0x7FFF:
        return (addr - 0x7E00) * 256 + 0x20000
    return None


class FlasherGUI:
    def __init__(self, root):
        self.root = root
        self.root.title("KTM M3C Flasher")
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
        self._build()
        self._auto_detect()
        # CHANGE: load the SD card contents immediately on startup - the SD
        # card lives on the ESP32, so listing/uploading/deleting files does
        # not need an ECU connection (only WRITE TO ECU does).
        if self.port_var.get():
            self.root.after(300, self._sd_refresh)
        else:
            self.sd_status.config(text='ESP32 not found - pick the port and press Refresh')
        self.port_combo.bind('<<ComboboxSelected>>', self._on_port_changed)

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

        self.btn_diag = tk.Button(top, text='DIAGNOSTICS',
                                   command=self._open_diagnostics,
                                   state='normal',
                                   bg='#6b3fa0', fg='white',
                                   font=('Segoe UI', 10, 'bold'),
                                   relief='flat', padx=12, pady=6)
        self.btn_diag.pack(side='left', padx=(0, 6))

        # Always enabled - also meant for getting out of a stuck operation.
        self.btn_reset = tk.Button(top, text='RESET ESP32',
                                    command=self._do_reset,
                                    bg='#8a6d00', fg='white',
                                    font=('Segoe UI', 10, 'bold'),
                                    relief='flat', padx=12, pady=6,
                                    cursor='hand2')
        self.btn_reset.pack(side='left', padx=(0, 6))

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
        self._build_sd_panel(right)

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

        cfg(self.btn_connect, idle and not self.connected, '#0078d4')
        cfg(self.btn_read, idle and self.connected, '#107c10')
        cfg(self.btn_diag, idle, '#6b3fa0')
        # CHANGE: SD card buttons only need the ESP32 (serial port open +
        # SD card present), NOT an ECU connection. Refresh is always
        # available so the list can be (re)loaded at any time. Only
        # WRITE TO ECU still requires CONNECT.
        esp = self._esp_ready()
        cfg(self.btn_sd_refresh, idle, '#3c3c3c')
        cfg(self.btn_sd_download, idle and esp and bool(self.sd_files), '#107c10')
        cfg(self.btn_sd_delete, idle and esp and bool(self.sd_files), '#c84b2f')
        cfg(self.btn_sd_upload, idle and esp and self.sd_ready, '#0078d4')
        cfg(self.btn_sd_write, idle and self.connected and bool(self.sd_files), '#c84b2f')

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
        for p in serial.tools.list_ports.comports():
            try:
                s = serial.Serial(p.device, BAUD, timeout=0.3)
                time.sleep(0.1)
                s.write(b'PING\n')
                time.sleep(0.2)
                resp = s.read(64).decode('utf-8', errors='ignore')
                s.close()
                if any(x in resp for x in ['PONG', 'CAN_OK', 'READY']):
                    self.port_var.set(p.device)
                    self._log(f'✅ ESP32 found: {p.device}', 'ok')
                    return
            except Exception:
                pass

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
        self._log('\n---------- Connecting to ECU ----------', 'info')
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
                self._log('---------- ECU Connected ----------', 'ok')
                self.connected = True
                self.status_label.config(text='● Connected', fg='#4ec9b0')
                self._set_busy(False)
                threading.Thread(target=self._sd_refresh_thread, daemon=True).start()
                return
            elif line == 'STATUS:CONNECT_FAILED':
                self._log('❌ Connect failed!', 'err')
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
        threading.Thread(target=self._read_thread, args=(False,), daemon=True).start()

    def _read_thread(self, no_handshake=False):
        self.reading = True
        self._set_busy(True)
        self.progress_var.set(0)
        self.progress_label.config(text='0%')
        self.stats_label.config(text='')
        self._log('\n---------- Read Flash Started ----------'
                   + (' (WITHOUT handshake - test)' if no_handshake else ''), 'info')
        self._log('Section 1: 0x7800-0x79FF | Section 2: 0x7E00-0x7FFF', 'info')
        self._log('Saves automatically to the SD card', 'info')
        self._send('READ_NOHS' if no_handshake else 'READ')

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
                self.progress_var.set(100)
                self.progress_label.config(text='100%')
                if errors == 0:
                    self._log(
                        f'✅ Read Done! Read: {done} | Errors: 0 - '
                        f'the whole file is 100% confirmed real ECU content.', 'ok')
                else:
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

    def _open_diagnostics(self):
        if self.op_busy:
            return
        if not self.ser or not self.ser.is_open:
            if not self._open_serial():
                return

        win = tk.Toplevel(self.root)
        win.title('Diagnostics')
        win.geometry('520x480')
        win.configure(bg='#1e1e1e')

        tk.Label(win, text='⚠ These commands have not yet been confirmed '
                            'from a Flex CAN log - best guess, may not work',
                 bg='#1e1e1e', fg='#f0a500', font=('Consolas', 9),
                 wraplength=490, justify='left').pack(anchor='w', padx=10, pady=(10, 6))

        dtc_frame = tk.LabelFrame(win, text='Diagnostic Trouble Codes (DTC)', bg='#1e1e1e',
                                   fg='#cccccc', font=('Consolas', 9))
        dtc_frame.pack(fill='both', expand=True, padx=10, pady=6)

        dtc_list = tk.Listbox(dtc_frame, bg='#0d0d0d', fg='#d4d4d4',
                               font=('Consolas', 10), height=8,
                               selectbackground='#0078d4')
        dtc_list.pack(fill='both', expand=True, padx=6, pady=6)

        dtc_status = tk.Label(dtc_frame, text='', bg='#1e1e1e',
                               fg='#888888', font=('Consolas', 9))
        dtc_status.pack(anchor='w', padx=6)

        def read_dtcs():
            dtc_list.delete(0, 'end')
            dtc_status.config(text='Reading trouble codes...')
            win.update_idletasks()
            self._send('DTC_READ')
            count = 0
            deadline = time.time() + 5
            while time.time() < deadline:
                line = self._read_line(timeout=1.0)
                if line is None:
                    continue
                if line.startswith('DTC_READ_DONE'):
                    break
                if line.startswith('DTC:'):
                    dtc_list.insert('end', line[4:])
                    count += 1
                elif line.startswith('ERR:'):
                    dtc_status.config(text=line)
                    return
            dtc_status.config(text=f'{count} trouble codes found' if count
                               else 'No trouble codes found (or the command does not work)')

        def clear_dtcs():
            def confirm():
                return messagebox.askyesno('Clear trouble codes',
                                            'Clear all trouble codes from the ECU?')
            if not self._ask_on_main(confirm):
                return
            dtc_status.config(text='Clearing...')
            win.update_idletasks()
            self._send('DTC_CLEAR')
            deadline = time.time() + 3
            while time.time() < deadline:
                line = self._read_line(timeout=1.0)
                if line is None:
                    continue
                if line == 'STATUS:DTC_CLEARED':
                    dtc_status.config(text='✅ Trouble codes cleared')
                    dtc_list.delete(0, 'end')
                    return
                if line.startswith('ERR:'):
                    dtc_status.config(text=line)
                    return
            dtc_status.config(text='Timeout - no response received')

        btns = tk.Frame(dtc_frame, bg='#1e1e1e')
        btns.pack(fill='x', padx=6, pady=(0, 6))
        tk.Button(btns, text='Read trouble codes', command=lambda: threading.Thread(
            target=read_dtcs, daemon=True).start(),
            bg='#0078d4', fg='white', relief='flat', padx=8).pack(side='left')
        tk.Button(btns, text='Clear trouble codes', command=lambda: threading.Thread(
            target=clear_dtcs, daemon=True).start(),
            bg='#c84b2f', fg='white', relief='flat', padx=8).pack(side='left', padx=6)

        live_frame = tk.LabelFrame(win, text='Live Data', bg='#1e1e1e',
                                    fg='#cccccc', font=('Consolas', 9))
        live_frame.pack(fill='x', padx=10, pady=6)

        rpm_var = tk.StringVar(value='---')
        temp_var = tk.StringVar(value='---')
        map_var = tk.StringVar(value='---')

        grid = tk.Frame(live_frame, bg='#1e1e1e')
        grid.pack(padx=6, pady=6)
        for i, (label, var) in enumerate([('RPM', rpm_var), ('Temp', temp_var),
                                           ('MAP', map_var)]):
            tk.Label(grid, text=label + ':', bg='#1e1e1e', fg='#888888',
                     font=('Consolas', 11)).grid(row=i, column=0, sticky='w', padx=4, pady=2)
            tk.Label(grid, textvariable=var, bg='#1e1e1e', fg='#4ec9b0',
                     font=('Consolas', 14, 'bold')).grid(row=i, column=1, sticky='w', padx=8)

        live_running = {'on': False}

        def live_poll():
            while live_running['on']:
                self._send('LIVE_READ')
                deadline = time.time() + 1.5
                while time.time() < deadline:
                    line = self._read_line(timeout=0.5)
                    if line is None:
                        continue
                    if line.startswith('LIVE:'):
                        parts = line.split(':')
                        try:
                            rpm_var.set(parts[2])
                            temp_var.set(parts[4])
                            map_var.set(parts[6])
                        except Exception:
                            pass
                        break
                time.sleep(0.3)

        def toggle_live():
            live_running['on'] = not live_running['on']
            if live_running['on']:
                live_btn.config(text='Stop Live Data', bg='#c84b2f')
                threading.Thread(target=live_poll, daemon=True).start()
            else:
                live_btn.config(text='Start Live Data', bg='#107c10')

        live_btn = tk.Button(live_frame, text='Start Live Data',
                              command=toggle_live, bg='#107c10', fg='white',
                              relief='flat', padx=8)
        live_btn.pack(anchor='w', padx=6, pady=(0, 6))

        def on_close():
            live_running['on'] = False
            win.destroy()
        win.protocol('WM_DELETE_WINDOW', on_close)


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