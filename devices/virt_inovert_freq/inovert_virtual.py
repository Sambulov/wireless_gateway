#!/usr/bin/env python3
"""
Virtual Inovert Frequency Converter — Modbus RTU slave (FC3 / FC6 / FC16)

Register groups (Modbus holding registers, 0-based):
  PA   0– 50   Display / monitoring (volatile, mostly read-only per spec)
  PB  100–127  Basic functions          (R/W)
  PC  200–219  Main application params  (R/W)
  PD  300–335  I/O params               (R/W)
  PE  400–433  Auxiliary params         (R/W)
  PF  500–553  PLC params               (R/W)
  PG  600–657  PID params               (R/W)
  PH  700–704  RS-485 comm params       (R/W)

Control via RS-485 in this virtual device:
  Write reg 100 (PB00): set target frequency  (unit = 0.1 Hz, e.g. 500 = 50.0 Hz)
  Write reg 102 (PB02): 0 = stop, 1 = run FWD, 2 = run REV
  Write reg  28        : 0 = stop, 1 = run FWD, 2 = run REV  (direct override)

Requirements:  pip install pyserial

Usage:
    python inovert_virtual.py --port /dev/ttyUSB1
    python inovert_virtual.py --port /dev/ttyUSB1 --address 1 --baud 9600
    python inovert_virtual.py --port /dev/ttyUSB1 --freq 50 --current 5.5 --voltage 380 --poles 4
"""

import argparse
import datetime
import random
import struct
import sys
import threading
import time

import serial


# ── Event logger ──────────────────────────────────────────────────────────────

class EventLogger:
    def __call__(self, kind: str, **kw):
        ts = datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3]
        kv = '  '.join(f'{k}={v}' for k, v in kw.items())
        print(f'[{ts}]  {kind:<22s}  {kv}', flush=True)

_null_log = lambda kind, **kw: None


# ── CRC-16 Modbus ─────────────────────────────────────────────────────────────

def _crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc

def _append_crc(frame: bytes) -> bytes:
    c = _crc16(frame)
    return frame + bytes([c & 0xFF, c >> 8])

def _check_crc(frame: bytes) -> bool:
    if len(frame) < 3:
        return False
    return _crc16(frame[:-2]) == (frame[-2] | (frame[-1] << 8))


# ── Inovert virtual device ────────────────────────────────────────────────────

class Inovert:
    REG_SIZE = 800

    def __init__(self, address: int = 1, on_event=None,
                 rated_freq: float = 50.0,
                 rated_voltage: int = 380,
                 rated_current: float = 10.0,
                 poles: int = 4):
        self.address       = address
        self._regs         = [0] * self.REG_SIZE
        self._log          = on_event or _null_log
        self.lock          = threading.Lock()

        # Motor state
        self._state        = 0      # 0=stopped, 1=FWD, 2=REV
        self._out_freq     = 0.0    # Hz — current output frequency
        self._op_hours     = 0.0    # total operating hours

        # Nominal parameters (can be overridden by writing PB/PC regs)
        self.RATED_FREQ    = rated_freq
        self.RATED_VOLTAGE = rated_voltage
        self.RATED_CURRENT = rated_current
        self.POLES         = poles

        self._t = time.monotonic()
        self._init_defaults()
        self.tick()

    # ── Default register values ──────────────────────────────────────────────

    def _init_defaults(self):
        r = self._regs

        # PA50 — firmware version
        r[50] = 0x0110           # 1.10

        # PB group — basic functions
        r[100] = int(self.RATED_FREQ * 10)   # PB00: target freq 50.0 Hz
        r[101] = 3               # PB01: panel potentiometer source
        r[102] = 0               # PB02: keypad start (0=stop in virtual)
        r[103] = 1               # PB03: STOP button enabled
        r[105] = int(self.RATED_FREQ * 10)   # PB05: max freq
        r[106] = 0               # PB06: min freq 0.0 Hz
        r[107] = 100             # PB07: accel time 10.0 s
        r[108] = 100             # PB08: decel time 10.0 s
        r[109] = self.RATED_VOLTAGE * 10     # PB09: max voltage (0.1V)
        r[110] = int(self.RATED_FREQ * 10)   # PB10: V/F base freq
        r[111] = 0               # PB11: mid voltage 0.0 V
        r[112] = 25              # PB12: mid freq 2.5 Hz
        r[113] = 0               # PB13: min voltage 0.0 V
        r[114] = 12              # PB14: min freq 1.2 Hz
        r[115] = 0               # PB15: carrier freq
        r[117] = 0               # PB17: param init
        r[118] = 0               # PB18: param lock
        r[120] = 0               # PB20: Y-channel source
        r[121] = 0               # PB21: X/Y selection → X
        r[122] = 0               # PB22: Y relative to max
        r[123] = 100             # PB23: Y range 100 %
        r[124] = 0               # PB24: freq correction
        r[125] = 1               # PB25: UP/DOWN base = setpoint
        r[126] = int(self.RATED_FREQ * 10)   # PB26: accel/decel base freq
        r[127] = 0               # PB27: accel/decel mode

        # PC group — main application params
        r[200] = 0               # PC00: normal start
        r[201] = 0               # PC01: decel-to-stop
        r[202] = 5               # PC02: start freq 0.5 Hz
        r[203] = 5               # PC03: stop freq 0.5 Hz
        r[204] = 0               # PC04: DC brake V at start
        r[205] = 0               # PC05: DC brake t at start
        r[206] = 0               # PC06: DC brake V at stop
        r[207] = 0               # PC07: DC brake t at stop
        r[208] = 3               # PC08: boost 3 %
        r[209] = self.RATED_VOLTAGE          # PC09: motor rated V
        r[210] = int(self.RATED_CURRENT * 10) # PC10: motor rated I (0.1A)
        r[211] = 50              # PC11: idle current 50 %
        # synchronous RPM minus ~3 % slip
        sync_rpm = 120.0 * self.RATED_FREQ / self.POLES
        r[212] = int(sync_rpm * 0.97)        # PC12: rated RPM
        r[213] = self.POLES      # PC13: number of poles
        r[215] = int(self.RATED_FREQ * 10)   # PC15: motor rated freq

        # PD group — I/O
        r[300] = 0               # PD00: AVI min V 0.0 V
        r[301] = 100             # PD01: AVI max V 10.0 V
        r[302] = 1               # PD02: AVI filter 0.1 s
        r[303] = 40              # PD03: min current 4.0 mA
        r[304] = 200             # PD04: max current 20.0 mA
        r[305] = 10              # PD05: current filter 1.0 s
        r[310] = 0               # PD10: freq at min AVI = 0.0 Hz
        r[312] = int(self.RATED_FREQ * 10)   # PD12: freq at max AVI
        r[325] = 1               # PD25: relay = "in operation"
        r[329] = 0               # PD29: 2-wire mode 1
        r[330] = 100             # PD30: UP/DOWN step 1.00 Hz/s
        r[331] = 0               # PD31: positive logic

        # PE group — auxiliary
        r[400] = 50              # PE00: jog freq 5.0 Hz
        r[401] = 100; r[402] = 100   # PE01/02: accel/decel 2 = 10 s
        r[403] = 100; r[404] = 100   # PE03/04: accel/decel 3 = 10 s
        r[405] = 100; r[406] = 100   # PE05/06: accel/decel 4 = 10 s
        r[407] = 100             # PE07: counter target
        r[408] = 50              # PE08: counter intermediate
        r[409] = 150             # PE09: OC limit during accel 150 %
        r[410] = 20              # PE10: OL suppression 20 %
        r[411] = 1               # PE11: OV protection enabled
        r[412] = 10              # PE12: V overshoot 10 %
        r[413] = 50              # PE13: V limit 50 %
        r[415] = 100             # PE15: brake duty cycle 100 %
        r[416] = 0               # PE16: power-loss restart disabled
        r[417] = 0               # PE17: no action on power loss
        r[418] = 150             # PE18: freq-search start limit 150 %
        r[419] = 5               # PE19: freq-search time 5 s
        r[420] = 0               # PE20: no fault restart
        r[421] = 10              # PE21: fault delay 1.0 s
        r[423] = 150             # PE23: OC detect level 150 %
        r[424] = 600             # PE24: OC time 60.0 s
        r[425] = 0               # PE25: threshold freq 1
        r[426] = 0               # PE26: threshold freq 2
        r[427] = 100             # PE27: timer 1 = 10.0 s
        r[428] = 200             # PE28: timer 2 = 20.0 s
        r[430] = 50              # PE30: freq-reach hysteresis 5.0 %

        # PG group — PID
        r[604] = 25              # PG04: PID setpoint 2.5
        r[605] = 100             # PG05: PID upper 10.0
        r[607] = 1000            # PG07: P gain 100.0 %
        r[608] = 20              # PG08: I time 2.0 s
        r[610] = 20              # PG10: tolerance 2.0 %
        r[611] = 250             # PG11: sleep freq 25.0 Hz
        r[612] = 10              # PG12: sleep delay 10 s
        r[613] = 900             # PG13: wake feedback 90.0 %
        r[614] = 100             # PG14: feedback display 10.0
        r[615] = 4               # PG15: 4 digits
        r[616] = 2               # PG16: 2 decimal places
        r[617] = 480             # PG17: PID upper freq 48.0 Hz
        r[618] = 200             # PG18: PID lower freq 20.0 Hz
        r[620] = 1               # PG20: deadband 0.1 %
        r[630] = 1000            # PG30: P2 gain 100.0 %
        r[631] = 20              # PG31: I2 time 2.0 s

        # PH group — RS-485
        r[700] = 1               # PH00: 9600 baud (index 1)
        r[701] = 3               # PH01: 8N1 RTU
        r[702] = self.address    # PH02: device address
        r[703] = 0               # PH03: no error action on timeout
        r[704] = 0               # PH04: watchdog timer

    # ── Measurement simulation ────────────────────────────────────────────────

    @staticmethod
    def _jitter(base: float, pct: float = 0.005) -> float:
        return base * (1.0 + random.uniform(-pct, pct))

    def tick(self):
        now = time.monotonic()
        dt  = now - self._t
        self._t = now
        if dt <= 0 or dt > 5:
            dt = 0.1

        # Read config registers
        set_freq   = self._regs[100] / 10.0          # Hz target
        max_freq   = max(0.1, self._regs[105] / 10.0)
        accel_t    = max(0.1, self._regs[107] / 10.0) # s
        decel_t    = max(0.1, self._regs[108] / 10.0) # s
        rated_volt = max(1,   self._regs[209])         # V
        rated_freq = max(0.1, self._regs[215] / 10.0) # Hz
        rated_cur  = self._regs[210] / 10.0           # A
        idle_pct   = self._regs[211] / 100.0          # fraction
        poles      = max(2, self._regs[213])
        rated_rpm  = self._regs[212]

        accel_rate = max_freq / accel_t   # Hz/s
        decel_rate = max_freq / decel_t   # Hz/s

        if self._state in (1, 2):
            # Ramp output frequency toward setpoint
            diff = set_freq - self._out_freq
            if diff > 0:
                self._out_freq = min(set_freq, self._out_freq + accel_rate * dt)
            elif diff < 0:
                self._out_freq = max(set_freq, self._out_freq - decel_rate * dt)
            self._out_freq = max(0.0, min(self._out_freq, max_freq))
            if 0 < dt < 10:
                self._op_hours += dt / 3600.0
        else:
            # Coast/decel to 0
            self._out_freq = max(0.0, self._out_freq - decel_rate * dt)

        f = self._out_freq

        # Output voltage: linear V/Hz curve
        out_v = rated_volt * (f / rated_freq) if f > 0.01 else 0.0
        out_v = min(float(rated_volt), out_v)
        out_v = self._jitter(out_v) if out_v > 0.01 else 0.0

        # Output current: idle + load proportion
        if f > 0.01 and rated_freq > 0:
            load_r = f / rated_freq
            out_i  = rated_cur * (idle_pct + (1.0 - idle_pct) * load_r)
            out_i  = self._jitter(out_i, 0.02)
        else:
            out_i = 0.0

        # RPM: synchronous minus slip
        sync_rpm  = 120.0 * f / poles
        rated_sync = 120.0 * rated_freq / poles
        slip = (rated_sync - rated_rpm) / rated_sync if rated_sync > 0 else 0.03
        slip = max(0.0, min(0.1, slip))
        actual_rpm = sync_rpm * (1.0 - slip) if f > 0.01 else 0.0

        # DC bus voltage (~565 V for 400 V 3-phase grid)
        dc_bus = self._jitter(565.0, 0.005)

        # AVI input: simulate 0–10 V proportional to current setpoint
        avi_v = self._jitter(set_freq / max_freq * 10.0, 0.01) if max_freq > 0 and self._state != 0 else 0.0

        with self.lock:
            r = self._regs
            r[1]  = int(set_freq * 10) if self._state != 0 else 0   # set freq
            r[2]  = int(f * 10)                                       # output freq
            r[3]  = int(out_i * 10)                                   # output current
            r[4]  = int(actual_rpm)                                   # RPM
            r[5]  = int(dc_bus * 10)                                  # DC bus
            r[7]  = 0                                                 # PID feedback
            r[8]  = int(self._op_hours)                               # operating hours
            r[9]  = int(out_v)                                        # output voltage
            r[22] = 1 if self._state != 0 else 0                     # relay "in operation"
            r[23] = int(avi_v * 100)                                  # AVI 0–1000 → 0–10.00 V
            r[27] = 0                                                 # no fault
            r[28] = self._state                                       # current state

        state_str = {0: 'STOP', 1: 'RUN_FWD', 2: 'RUN_REV'}.get(self._state, '?')
        self._log('MEAS_TICK',
                  state=state_str,
                  set=f'{set_freq:.1f}Hz',
                  out=f'{f:.1f}Hz',
                  I=f'{out_i:.2f}A',
                  U=f'{int(out_v)}V',
                  RPM=f'{int(actual_rpm)}',
                  DC=f'{dc_bus:.0f}V')

    # ── Modbus request handlers ───────────────────────────────────────────────

    def fc3(self, start: int, count: int):
        """Read holding registers. Returns (words, None) or (None, error_code)."""
        if count < 1 or count > 125:
            return None, 0x03
        if start < 0 or start + count > self.REG_SIZE:
            return None, 0x02
        with self.lock:
            data = list(self._regs[start: start + count])
        self._log('FC3_READ', start=start, count=count)
        return data, None

    def _write_regs(self, start: int, words: list):
        """Common handler for FC6 and FC16 writes."""
        count = len(words)
        if start < 0 or start + count > self.REG_SIZE:
            self._log('FC_WRITE', start=start, count=count, error='illegal_address')
            return 0x02

        changed = {}
        with self.lock:
            for i, w in enumerate(words):
                addr = start + i
                self._regs[addr] = int(w) & 0xFFFF
                changed[addr] = int(w) & 0xFFFF

        self._on_write(changed)
        self._log('FC_WRITE', start=start, count=count,
                  values=words if len(words) <= 8 else words[:8])
        return None

    def _on_write(self, changed: dict):
        """Side-effects for key register writes."""
        # PB02 (reg 102): start-control — reuse as run command in virtual device
        if 102 in changed:
            v = changed[102]
            if v == 0:
                self._state = 0   # stop
            elif v == 1:
                self._state = 1   # run FWD
            elif v == 2:
                self._state = 2   # run REV

        # Direct write to state register (reg 28) for testing convenience
        if 28 in changed and changed[28] in (0, 1, 2):
            self._state = changed[28]

        # Update device address if PH02 (reg 702) changed
        if 702 in changed:
            new_addr = changed[702]
            if 1 <= new_addr <= 247:
                self.address = new_addr
                self._regs[702] = new_addr

    def reject_reason(self, raw: bytes) -> str:
        if len(raw) < 4:
            return f'too_short({len(raw)}B)'
        if not _check_crc(raw):
            return (f'bad_crc(got={raw[-2]:02x}{raw[-1]:02x}'
                    f'_want={_crc16(raw[:-2]):04x})')
        if raw[0] != self.address:
            return f'wrong_address(got={raw[0]}_want={self.address})'
        return 'ok'

    def handle(self, raw: bytes):
        """Parse raw RTU frame, return response bytes or None."""
        if not _check_crc(raw):
            return None
        if raw[0] != self.address:
            return None

        fc = raw[1]

        if fc == 0x03:                           # Read holding registers
            if len(raw) < 8:
                return None
            start = (raw[2] << 8) | raw[3]
            count = (raw[4] << 8) | raw[5]
            words, err = self.fc3(start, count)
            if err:
                resp = bytes([self.address, fc | 0x80, err])
            else:
                body = b''.join(struct.pack('>H', w) for w in words)
                resp = bytes([self.address, fc, len(body)]) + body

        elif fc == 0x06:                         # Write single register
            if len(raw) < 8:
                return None
            addr  = (raw[2] << 8) | raw[3]
            value = (raw[4] << 8) | raw[5]
            err = self._write_regs(addr, [value])
            if err:
                resp = bytes([self.address, fc | 0x80, err])
            else:
                resp = bytes([self.address, fc,
                              raw[2], raw[3], raw[4], raw[5]])

        elif fc == 0x10:                         # Write multiple registers
            if len(raw) < 9:
                return None
            start      = (raw[2] << 8) | raw[3]
            count      = (raw[4] << 8) | raw[5]
            byte_count = raw[6]
            if len(raw) < 7 + byte_count + 2:
                return None
            words = [struct.unpack('>H', raw[7 + i*2: 9 + i*2])[0]
                     for i in range(count)]
            err = self._write_regs(start, words)
            if err:
                resp = bytes([self.address, fc | 0x80, err])
            else:
                resp = bytes([self.address, fc,
                              raw[2], raw[3], raw[4], raw[5]])

        else:
            resp = bytes([self.address, fc | 0x80, 0x01])   # illegal function

        return _append_crc(resp)


# ── Frame description ─────────────────────────────────────────────────────────

def _describe_frame(frame: bytes) -> str:
    if len(frame) < 4:
        return frame.hex(' ')
    adr, fc = frame[0], frame[1]

    if fc == 0x03:
        if len(frame) == 8:
            start = (frame[2] << 8) | frame[3]
            count = (frame[4] << 8) | frame[5]
            return f'adr={adr} FC3 REQ  reg={start} count={count}'
        else:
            byte_count = frame[2]
            return f'adr={adr} FC3 RESP regs={byte_count // 2} ({byte_count}B)'

    if fc == 0x06:
        if len(frame) == 8:
            reg = (frame[2] << 8) | frame[3]
            val = (frame[4] << 8) | frame[5]
            tag = 'RESP' if frame[0] == frame[0] else 'REQ'   # same format
            return f'adr={adr} FC6 reg={reg} val={val}'
        return f'adr={adr} FC6 [{frame.hex(" ")}]'

    if fc == 0x10:
        if len(frame) == 8:
            start = (frame[2] << 8) | frame[3]
            count = (frame[4] << 8) | frame[5]
            return f'adr={adr} FC16 RESP reg={start} count={count}'
        elif len(frame) >= 9:
            start  = (frame[2] << 8) | frame[3]
            count  = (frame[4] << 8) | frame[5]
            nbytes = frame[6]
            words  = [struct.unpack('>H', frame[7 + i*2: 9 + i*2])[0]
                      for i in range(min(count, nbytes // 2))]
            return f'adr={adr} FC16 REQ  reg={start} count={count} values={words}'

    if fc & 0x80:
        return f'adr={adr} FC{fc & 0x7f}|ERR code={frame[2] if len(frame) > 2 else "?"}'
    return f'adr={adr} FC{fc} [{frame.hex(" ")}]'


# ── Serial RTU server ─────────────────────────────────────────────────────────

class RTUServer:
    def __init__(self, device: Inovert, port: str, baud: int, parity: str,
                 stopbits: int, update_hz: float):
        self.dev      = device
        self.port     = port
        self.baud     = baud
        self.parity   = {'N': serial.PARITY_NONE,
                         'E': serial.PARITY_EVEN,
                         'O': serial.PARITY_ODD}.get(parity, serial.PARITY_NONE)
        self.stopbits = (serial.STOPBITS_ONE if stopbits == 1 else serial.STOPBITS_TWO)
        self.update_hz = update_hz
        self._ser     = None
        self._running = False

    def _frame_gap(self) -> float:
        return max(0.05, 3.5 * 11.0 / self.baud)

    def _read_frame(self) -> bytes:
        self._ser.timeout = 0.5
        first = self._ser.read(1)
        if not first:
            return b''
        self._ser.timeout = self._frame_gap()
        buf = first
        while True:
            chunk = self._ser.read(256)
            if not chunk:
                break
            buf += chunk
        return buf

    def _updater(self):
        interval = 1.0 / max(0.1, self.update_hz)
        while self._running:
            self.dev.tick()
            time.sleep(interval)

    def run(self):
        log = self.dev._log
        d   = self.dev
        print(f"Inovert virtual frequency converter")
        print(f"  Port {self.port}  {self.baud} 8N{1 if self.stopbits == serial.STOPBITS_ONE else 2}"
              f"  Modbus address {d.address}")
        print(f"  Rated: {d.RATED_VOLTAGE} V  {d.RATED_CURRENT} A  {d.RATED_FREQ} Hz  {d.POLES} poles")
        print(f"  Control: write reg 100 = target freq (0.1 Hz units)")
        print(f"           write reg 102 = 0 stop / 1 FWD / 2 REV")
        print("  Press Ctrl+C to stop.\n")

        try:
            self._ser = serial.Serial(
                port=self.port, baudrate=self.baud,
                bytesize=serial.EIGHTBITS, parity=self.parity,
                stopbits=self.stopbits)
        except serial.SerialException as exc:
            sys.exit(f"[ERROR] Cannot open {self.port}: {exc}")

        self._running = True
        threading.Thread(target=self._updater, daemon=True).start()

        rx_n = tx_n = 0
        try:
            while self._running:
                frame = self._read_frame()
                if not frame:
                    continue
                rx_n += 1
                log('SERIAL_RX', n=rx_n, frame=_describe_frame(frame),
                    raw=frame.hex(' '))
                resp = self.dev.handle(frame)
                if resp:
                    self._ser.write(resp)
                    tx_n += 1
                    log('SERIAL_TX', n=tx_n, frame=_describe_frame(resp))
                else:
                    log('SERIAL_IGNORED', n=rx_n,
                        reason=self.dev.reject_reason(frame),
                        raw=frame.hex(' '))
        except KeyboardInterrupt:
            print("\nStopped.")
        finally:
            self._running = False
            self._ser.close()


# ── CLI ────────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(
        description="Virtual Inovert frequency converter (Modbus RTU slave)")
    ap.add_argument("--port",      default="/dev/ttyUSB0",
                    help="Serial port (default: /dev/ttyUSB0)")
    ap.add_argument("--baud",      type=int, default=9600,
                    help="Baud rate (default: 9600)")
    ap.add_argument("--address",   type=int, default=1,
                    help="Modbus slave address 1-247 (default: 1)")
    ap.add_argument("--parity",    default="N", choices=["N", "E", "O"],
                    help="Parity N/E/O (default: N)")
    ap.add_argument("--stopbits",  type=int, default=1, choices=[1, 2],
                    help="Stop bits (default: 1)")
    ap.add_argument("--freq",      type=float, default=50.0,
                    help="Rated frequency Hz (default: 50)")
    ap.add_argument("--voltage",   type=int, default=380,
                    help="Rated motor voltage V (default: 380)")
    ap.add_argument("--current",   type=float, default=10.0,
                    help="Rated motor current A (default: 10)")
    ap.add_argument("--poles",     type=int, default=4,
                    help="Number of motor poles (default: 4)")
    ap.add_argument("--update-hz", type=float, default=2.0,
                    help="Measurement update rate Hz (default: 2)")
    args = ap.parse_args()

    dev = Inovert(
        address       = args.address,
        on_event      = EventLogger(),
        rated_freq    = args.freq,
        rated_voltage = args.voltage,
        rated_current = args.current,
        poles         = args.poles,
    )

    RTUServer(dev, args.port, args.baud, args.parity,
              args.stopbits, args.update_hz).run()


if __name__ == "__main__":
    main()
