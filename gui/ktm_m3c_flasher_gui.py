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
        self.root.title("KTM Husqvarna GasGas M3C Flasher")
        self.root.geometry("1280x760")
        self.root.minsize(1100, 560)
        self.root.configure(bg='#1e1e1e')
        self.ser = None
        self.connected = False
        self.reading = False
        self.op_busy = False
        self.sd_ready = False
        self.sd_files = []
        self._build()
        self._auto_detect()

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
        tk.Button(top, text='\u21bb', command=self._refresh_ports,
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

        self.btn_diag = tk.Button(top, text='DIAGNOSTIKA',
                                   command=self._open_diagnostics,
                                   state='normal',
                                   bg='#6b3fa0', fg='white',
                                   font=('Segoe UI', 10, 'bold'),
                                   relief='flat', padx=12, pady=6)
        self.btn_diag.pack(side='left', padx=(0, 6))

        self.status_label = tk.Label(top, text='\u25cf Not connected',
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
            ('Firmware', self.ver_var),
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

        # NOTE: Listbox has its own default width of 20 CHARACTERS, regardless
        # of the surrounding Frame's width - this was the real reason file
        # names were getting cut off, not the panel width. Set it explicitly wider.
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

        tk.Label(parent, text='Upload a new file to SD (from computer):',
                 bg='#1e1e1e', fg='#cccccc', font=('Consolas', 9)).pack(
                     anchor='w', pady=(6, 4))

        self.upload_progress_var = tk.DoubleVar()
        ttk.Progressbar(parent, variable=self.upload_progress_var,
                         maximum=100).pack(fill='x')
        self.upload_status = tk.Label(parent, text='', bg='#1e1e1e',
                                       fg='#888888', font=('Consolas', 8),
                                       wraplength=500, justify='left')
        self.upload_status.pack(anchor='w', pady=(2, 6))

        self.btn_sd_upload = tk.Button(parent, text='Choose file from computer and upload to SD',
                                        command=self._sd_upload,
                                        bg='#0078d4', fg='white', relief='flat',
                                        padx=8, pady=4, state='disabled')
        self.btn_sd_upload.pack(fill='x', pady=(0, 12))

        tk.Frame(parent, bg='#3c3c3c', height=1).pack(fill='x', pady=(0, 12))

        tk.Label(parent, text='Write the selected file\'s content to the ECU flash:',
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
        cfg(self.btn_sd_refresh, idle and self.connected, '#3c3c3c')
        cfg(self.btn_sd_download, idle and self.connected and bool(self.sd_files), '#107c10')
        cfg(self.btn_sd_delete, idle and self.connected and bool(self.sd_files), '#c84b2f')
        cfg(self.btn_sd_upload, idle and self.connected, '#0078d4')
        cfg(self.btn_sd_write, idle and self.connected and bool(self.sd_files), '#c84b2f')

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
                    self._log(f'\u2705 ESP32 leitud: {p.device}', 'ok')
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
            self._log('ERR: Port valimata!', 'err')
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
        if not self._open_serial():
            self._set_busy(False)
            return

        self._send('CONNECT')
        self.status_label.config(text='\u25cf Connecting...', fg='#f0a500')

        timeout = time.time() + 90
        while time.time() < timeout:
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
                self._log(f'Firmware version: {val}', 'ok')
            elif line.startswith('ERR:'):
                self._log(line, 'err')
            elif line == 'STATUS:SD_OK':
                self.sd_ready = True
                self._log('\u2705 SD card found', 'ok')
            elif line == 'STATUS:SD_FAIL':
                self.sd_ready = False
                self._log('\u26a0 SD card not found', 'err')
                self.sd_status.config(text='SD card not found')
            elif line.startswith('STATUS:WIFI_AP_IP:'):
                self._log(line, 'ok')
            elif line == 'STATUS:WEBSERVER_STARTED':
                self._log(line, 'ok')
            elif line == 'STATUS:CONNECTED':
                self._log('---------- ECU Connected ----------', 'ok')
                self.connected = True
                self.status_label.config(text='\u25cf Connected', fg='#4ec9b0')
                self._set_busy(False)
                if self.sd_ready:
                    threading.Thread(target=self._sd_refresh_thread, daemon=True).start()
                return
            elif line == 'STATUS:CONNECT_FAILED':
                self._log('\u274c Connect failed!', 'err')
                self.status_label.config(text='\u25cf Error', fg='#f44747')
                break
            elif line:
                self._log(line, 'info')

        self._set_busy(False)

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
                   + (' (WITHOUT handshake - experiment)' if no_handshake else ''), 'info')
        self._log('Section 1: 0x7800-0x79FF | Section 2: 0x7E00-0x7FFF', 'info')
        self._log('Salvestub automaatselt SD-kaardile', 'info')
        self._send('READ_NOHS' if no_handshake else 'READ')

        done = 0
        errors = 0

        timeout = time.time() + 300
        while time.time() < timeout:
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
                            text=f'Loetud: {done}/{TOTAL_ADDRS} | '
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
                    f'\u26a0 {line.split(":")[-1]} addresses were not read - '
                    f'these spots in the file are 0xFF placeholders, NOT '
                    f'confirmed real content!', 'err')

            elif line.startswith('WARN:FAILED_ADDRS:'):
                self._log(f'  Missing addresses: {line[len("WARN:FAILED_ADDRS:"):]}', 'err')

            elif line.startswith('ERR:SD'):
                self._log(line, 'err')

            elif line.startswith('PROGRESS:'):
                # NOTE: firmware sends "PROGRESS:<done>/<total>" (slash,
                # not colon) - the old code's split(':') condition never
                # matched, which is why the progress bar didn't move during Write.
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
                        f'\u2705 Read Done! Loetud: {done} | Vead: 0 - '
                        f'the whole file is 100% confirmed real ECU content.', 'ok')
                else:
                    self._log(
                        f'\u26a0 Read Done! Loetud: {done} | Vead: {errors} - '
                        f'see above which spots are placeholders.', 'err')
                self._log('File saved to SD card - see the SD panel on the right', 'ok')
                break

            elif line:
                self._log(line, 'info')

        self.reading = False

        # NOTE: if LITERALLY not a single address succeeded
        # (ECU not responding at all - e.g. the post-Write "no response" situation),
        # we consider the connection lost, so the CONNECT button turns green again
        # and the user can try again (after a power cycle).
        if done == 0 and errors > 0:
            self.connected = False
            self.status_label.config(text='● Connection lost', fg='#f44747')
            self._log('⚠ ECU did not respond to any request - the connection is considered '
                       'lost. Power-cycle the ECU and press CONNECT again.', 'err')

        self._set_busy(False)
        self.stats_label.config(
            text=f'Total: {done}/{TOTAL_ADDRS} addresses | Errors: {errors}')
        if self.connected:
            threading.Thread(target=self._sd_refresh_thread, daemon=True).start()

    def _sd_refresh(self):
        if self.op_busy or not self.connected:
            return
        threading.Thread(target=self._sd_refresh_thread, daemon=True).start()

    def _sd_refresh_thread(self):
        self._set_busy(True)
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
                break
            if line.startswith('SDFILE:'):
                parts = line.split(':')
                if len(parts) >= 3:
                    name, size = parts[1], parts[2]
                    self.sd_files.append(name)
                    self.sd_listbox.insert('end', f'{name}  ({size} B)')
            elif line.startswith('ERR:'):
                self.sd_status.config(text=line)
                self._set_busy(False)
                return
        self.sd_status.config(text=f'{len(self.sd_files)} file(s) found')
        self._set_busy(False)

    def _sd_selected_filename(self):
        sel = self.sd_listbox.curselection()
        if not sel:
            return None
        return self.sd_files[sel[0]]

    def _sd_download(self):
        if self.op_busy or not self.connected:
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

        while time.time() - last_progress < silence_timeout:
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
                text=f'\u26a0 Suurus ei klapi: sai {len(content)}, oodati {expected_size}')

        def do_save():
            save_path = filedialog.asksaveasfilename(
                title='Save SD file to computer',
                initialfile=filename.lstrip('/'),
                defaultextension='.bin',
                filetypes=[('Binary', '*.bin'), ('All', '*.*')])
            if save_path:
                with open(save_path, 'wb') as f:
                    f.write(content)
                self.sd_status.config(text=f'\u2705 Saved: {save_path} ({len(content)} B)')
            else:
                self.sd_status.config(text='Save cancelled')

        self._ask_on_main(do_save)
        self._set_busy(False)

    def _sd_delete(self):
        if self.op_busy or not self.connected:
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
        if self.op_busy or not self.connected:
            return
        threading.Thread(target=self._sd_upload_thread, daemon=True).start()

    def _sd_upload_thread(self):
        local_path = self._ask_on_main(
            filedialog.askopenfilename,
            title='Choose BIN file to upload',
            filetypes=[('Binary', '*.bin'), ('All', '*.*')])
        if not local_path:
            return

        self._set_busy(True)
        try:
            with open(local_path, 'rb') as lf:
                data = lf.read()
            target_name = os.path.basename(local_path)

            if len(data) != BIN_SIZE:
                self.upload_status.config(
                    text=f'\u26a0 File size {len(data)} B, expected {BIN_SIZE} B '
                         f'- writing would later be refused with this file')

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
                self.upload_status.config(text='ERR: ESP32 did not acknowledge the upload')
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
                    self.upload_status.config(text=f'\u2705 Done: {target_name}')
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
                f'Write the contents of "{filename}" to the entire ECU flash?\n\n'
                f'This is a 2306-step process (erase + write for both '
                f'sections) and takes several minutes.\n\n'
                f'THIS PERMANENTLY MODIFIES THE ECU! Continue?')
        if not self._ask_on_main(confirm):
            return

        self._set_busy(True)
        self.progress_var.set(0)
        self.progress_label.config(text='0%')
        self.status_label.config(text='\u25cf Writing...', fg='#f0a500')
        self._log(f'\n---------- Writing {filename} ----------', 'info')

        try:
            self._send(f'WRITEFULL:{filename}')
            deadline = time.time() + 600
            done = False
            ok = False
            while time.time() < deadline:
                line = self._read_line(timeout=5.0)
                if line is None:
                    continue

                if line.startswith('ERR:WRITEFULL_BAD_FILESIZE:'):
                    self._log(line, 'err')
                    self._log('\u274c Wrong file size - write refused', 'err')
                    done = True
                    break
                elif line.startswith('STEP:'):
                    self._log(line, 'step')
                elif line.startswith('ERR:'):
                    self._log(line, 'err')
                elif line.startswith('PROGRESS:'):
                    # NOTE: same fix as for READ - firmware
                    # kasutab kaldkriipsu, mitte koolonit.
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
                    self._log('\u2705 Write complete!', 'ok')
                    done = True
                    ok = True
                    break
                elif line == 'STATUS:WRITEFULL_FAILED':
                    self._log('\u274c Write failed!', 'err')
                    done = True
                    break

            if not done:
                self._log('ERR: Timeout', 'err')

            self.status_label.config(
                text='\u25cf Written' if ok else '\u25cf Error',
                fg='#4ec9b0' if ok else '#f44747')
        finally:
            self._set_busy(False)

    def _open_diagnostics(self):
        if self.op_busy:
            return
        if not self.ser or not self.ser.is_open:
            if not self._open_serial():
                return

        win = tk.Toplevel(self.root)
        win.title('Diagnostika')
        win.geometry('520x480')
        win.configure(bg='#1e1e1e')

        tk.Label(win, text='\u26a0 These commands are not yet confirmed from a CAN log - '
                            'best guess, may not work',
                 bg='#1e1e1e', fg='#f0a500', font=('Consolas', 9),
                 wraplength=490, justify='left').pack(anchor='w', padx=10, pady=(10, 6))

        dtc_frame = tk.LabelFrame(win, text='Veakoodid (DTC)', bg='#1e1e1e',
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
            dtc_status.config(text='Reading fault codes...')
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
            dtc_status.config(text=f'{count} veakoodi leitud' if count
                               else 'No fault codes found (or command not working)')

        def clear_dtcs():
            def confirm():
                return messagebox.askyesno('Clear fault codes',
                                            'Clear all fault codes from the ECU?')
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
                    dtc_status.config(text='\u2705 Fault codes cleared')
                    dtc_list.delete(0, 'end')
                    return
                if line.startswith('ERR:'):
                    dtc_status.config(text=line)
                    return
            dtc_status.config(text='Timeout - no response received')

        btns = tk.Frame(dtc_frame, bg='#1e1e1e')
        btns.pack(fill='x', padx=6, pady=(0, 6))
        tk.Button(btns, text='Loe veakoodid', command=lambda: threading.Thread(
            target=read_dtcs, daemon=True).start(),
            bg='#0078d4', fg='white', relief='flat', padx=8).pack(side='left')
        tk.Button(btns, text='Clear fault codes', command=lambda: threading.Thread(
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
        input("Press Enter to exit...")
