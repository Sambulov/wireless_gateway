#!/usr/bin/env python3
"""
Virtual SUTO S111 Three-Phase Power Meter — Modbus RTU slave

Implements FC3 (read holding registers) and FC16 (write multiple registers).
FC16 writes are restricted to config-instruction registers 300-423 per spec.
Writing register 300 triggers instruction processing (see section 1.7).

Requirements:  pip install pyserial

Usage:
    python s111_virtual.py --port /dev/ttyUSB0
    python s111_virtual.py --port COM3 --address 1 --baud 115200
    python s111_virtual.py --port /dev/ttyUSB0 --voltage 230 --current 25 --pf 0.85
"""

import argparse
import datetime
import math
import random
import struct
import sys
import threading
import time

import serial


# ── Event logger ───────────────────────────────────────────────────────────────

class EventLogger:
    def __call__(self, kind: str, **kw):
        ts = datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3]
        kv = '  '.join(f'{k}={v}' for k, v in kw.items())
        print(f'[{ts}]  {kind:<20s}  {kv}', flush=True)

_null_log = lambda kind, **kw: None


# ── CRC-16 Modbus ─────────────────────────────────────────────────────────────

def _crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def _append_crc(frame: bytes) -> bytes:
    c = _crc16(frame)
    return frame + bytes([c & 0xFF, c >> 8])   # low byte first


def _check_crc(frame: bytes) -> bool:
    if len(frame) < 3:
        return False
    return _crc16(frame[:-2]) == (frame[-2] | (frame[-1] << 8))


# ── Encoding helpers ──────────────────────────────────────────────────────────

def _f32(v: float) -> list:
    """Float32 → [hi_word, lo_word], big-endian word order (IEEE 754 ABCD)."""
    b = struct.pack('>f', float(v))
    return [struct.unpack('>H', b[0:2])[0], struct.unpack('>H', b[2:4])[0]]


def _u32(v: int) -> list:
    """UInt32 → [hi_word, lo_word]."""
    v = int(v) & 0xFFFFFFFF
    return [v >> 16, v & 0xFFFF]


def _i64(v: int) -> list:
    """Int64 → [w3, w2, w1, w0], big-endian word order."""
    v = int(v) & 0xFFFFFFFFFFFFFFFF
    return [(v >> 48) & 0xFFFF, (v >> 32) & 0xFFFF,
            (v >> 16) & 0xFFFF,  v        & 0xFFFF]


def _dt(dt: datetime.datetime) -> list:
    """DateTime → 4 registers per spec §1.8.
    Reg+0: Year          Reg+1: Month(hi) | Day(lo)
    Reg+2: Hour(hi) | Min(lo)  Reg+3: Second
    """
    return [dt.year,
            (dt.month << 8) | dt.day,
            (dt.hour  << 8) | dt.minute,
            dt.second]


def _utf8(text: str, n_regs: int) -> list:
    """UTF-8 string → n_regs × UInt16 registers, big-endian byte pairs."""
    b = text.encode('utf-8')[:n_regs * 2].ljust(n_regs * 2, b'\x00')
    return [(b[i * 2] << 8) | b[i * 2 + 1] for i in range(n_regs)]


# ── Register bank ─────────────────────────────────────────────────────────────

class _RegBank:
    SIZE = 9000

    def __init__(self):
        self._r = [0] * self.SIZE

    # low-level
    def set(self, addr: int, words: list):
        for i, w in enumerate(words):
            a = addr + i
            if 0 <= a < self.SIZE:
                self._r[a] = int(w) & 0xFFFF

    def get(self, addr: int, count: int) -> list:
        return [self._r[a] if 0 <= a < self.SIZE else 0
                for a in range(addr, addr + count)]

    # typed writers
    def f32(self, addr, v):  self.set(addr, _f32(v))
    def u32(self, addr, v):  self.set(addr, _u32(v))
    def i64(self, addr, v):  self.set(addr, _i64(v))
    def dt (self, addr, v):  self.set(addr, _dt(v))


# ── S111 device state ─────────────────────────────────────────────────────────

class S111:
    # default grid parameters
    VOLTAGE = 220.0   # V (phase-to-neutral)
    CURRENT = 10.0    # A (nominal load)
    PF      = 0.92    # power factor
    FREQ    = 50.0    # Hz

    _BAUD_MAP = {0: 2400, 1: 4800, 2: 9600, 3: 19200,
                 4: 38400, 5: 57600, 6: 115200}

    def __init__(self, address: int = 1, on_event=None):
        self.address   = address
        self.rb        = _RegBank()
        self.lock      = threading.Lock()
        self._log      = on_event or _null_log

        # energy accumulators (Wh, VARh, VAh) per phase
        self._wh   = [0.0, 0.0, 0.0]
        self._varh = [0.0, 0.0, 0.0]
        self._vah  = [0.0, 0.0, 0.0]
        self._t    = time.monotonic()

        # instruction scratch (regs 300-423)
        self._instr = [0] * 124

        self._init_static()
        self.tick()

    # ── static / config registers ──────────────────────────────────────────

    def _init_static(self):
        rb = self.rb

        # §1.9.1  Equipment parameters
        rb.set(60, _utf8("S111", 10))          # Device model
        rb.u32(70, 20250001)                   # Serial No.
        rb.set(72, [0x0201])                   # App version 2.1
        rb.dt (75, datetime.datetime.now())    # Date and time

        # §1.9.2  Communication parameters
        rb.set(80, [1])   # slave address
        rb.set(81, [2])   # baud 9600
        rb.set(82, [0])   # parity: none
        rb.set(83, [0])   # stop: 1 bit

        # §1.9.3  Phase sequence — all correct
        rb.set(220, [0])

        # §1.9.4  Config instruction result regs
        rb.set(424, [0])
        rb.set(425, [0])

        # §1.9.5  Power system defaults
        rb.set(500, [0])          # 3P4W_4CT
        rb.set(501, [50])         # 50 Hz
        rb.set(502, [220])        # 220 V nominal
        rb.u32(503, 10000)        # VT ratio × 10000 = 1.0
        rb.u32(505, 10000)        # CT ratio × 10000 = 1.0

        # Phase ABC CT (VCT, 100 A primary)
        rb.set(510, [1])          # VCT access
        rb.u32(511, 1000)         # Rogowski coil primary 1000 A
        rb.u32(513, 3333)         # Rogowski coil secondary 33.33 mV
        rb.u32(515, 1000)         # Rogowski nominal 1000 A
        rb.u32(517, 100)          # VCT primary 100 A
        rb.u32(519, 2500)         # VCT secondary 25.00 mV
        rb.u32(521, 100)          # VCT nominal 100 A

        # Phase N CT (same)
        rb.set(530, [1]);  rb.u32(531, 1000); rb.u32(533, 3333)
        rb.u32(535, 1000); rb.u32(537, 100);  rb.u32(539, 2500); rb.u32(541, 100)

        # §1.9.6  Current direction — forward
        rb.set(550, [0]); rb.set(551, [0]); rb.set(552, [0])

        # §1.9.7  Current channel defaults
        rb.set(553, [0]); rb.set(554, [1]); rb.set(555, [2])

        # §1.9.8  Zero drift suppression 1.00%
        rb.set(600, [100]); rb.set(601, [100])

        # §1.9.9  Tariff defaults (manual, tariff 1)
        rb.set(800, [0]); rb.set(801, [0]); rb.set(802, [0])

        # §1.9.13  Demand config
        rb.set(3000, [0])   # sliding type
        rb.set(3001, [15])  # 15-min interval
        rb.dt(3002, datetime.datetime.now())

    # ── measurement simulation ─────────────────────────────────────────────

    @staticmethod
    def _jitter(base: float, pct: float = 0.005) -> float:
        return base * (1.0 + random.uniform(-pct, pct))

    def tick(self):
        """Recompute all measurement registers. Called from background thread."""
        now = time.monotonic()
        dt  = now - self._t
        self._t = now

        j = self._jitter
        V, I, PF, F = self.VOLTAGE, self.CURRENT, self.PF, self.FREQ

        # Phase voltages (V)
        ua, ub, uc = j(V), j(V), j(V)
        u_avg = (ua + ub + uc) / 3.0
        u0    = abs(ua - ub) * 0.002   # tiny zero-sequence

        # Phase currents (A)
        ia, ib, ic = j(I), j(I), j(I)
        i_avg = (ia + ib + ic) / 3.0
        i_n   = j(I * 0.015)           # small neutral

        # Line voltages (V)
        rt3 = math.sqrt(3)
        uab, ubc, uca = j(V * rt3), j(V * rt3), j(V * rt3)
        ul_avg = (uab + ubc + uca) / 3.0

        # Per-phase PF and angles
        pf_a, pf_b, pf_c = j(PF, 0.01), j(PF, 0.01), j(PF, 0.01)
        phi = [math.acos(max(-1.0, min(1.0, p))) for p in (pf_a, pf_b, pf_c)]

        # Powers
        pa, pb, pc = ua*ia*pf_a/1000, ub*ib*pf_b/1000, uc*ic*pf_c/1000
        p_tot = pa + pb + pc

        qa, qb, qc = (ua*ia*math.sin(phi[0])/1000,
                      ub*ib*math.sin(phi[1])/1000,
                      uc*ic*math.sin(phi[2])/1000)
        q_tot = qa + qb + qc

        sa, sb, sc = ua*ia/1000, ub*ib/1000, uc*ic/1000
        s_tot = sa + sb + sc

        pf_tot = p_tot / s_tot if s_tot > 0 else 0.0
        freq   = j(F, 0.001)

        # Energy accumulation
        if 0 < dt < 10:
            for i, (pw, qw, sw) in enumerate(zip([pa, pb, pc],
                                                  [qa, qb, qc],
                                                  [sa, sb, sc])):
                self._wh[i]   += pw * 1000.0 * dt / 3600.0
                self._varh[i] += qw * 1000.0 * dt / 3600.0
                self._vah[i]  += sw * 1000.0 * dt / 3600.0

        tw = sum(self._wh); tq = sum(self._varh); tv = sum(self._vah)

        # Write all to register bank
        rb = self.rb
        with self.lock:
            # Date/time (reg 75)
            rb.dt(75, datetime.datetime.now())

            # §1.9.10  Currents
            rb.f32(1000, ia);  rb.f32(1002, ib);  rb.f32(1004, ic)
            rb.f32(1006, i_avg);  rb.f32(1008, i_n)

            # Phase voltages
            rb.f32(1010, ua); rb.f32(1012, ub); rb.f32(1014, uc)
            rb.f32(1016, u_avg); rb.f32(1018, u0)

            # Line voltages
            rb.f32(1020, uab); rb.f32(1022, ubc); rb.f32(1024, uca)
            rb.f32(1026, ul_avg)

            # Active power (kW)
            rb.f32(1028, pa); rb.f32(1030, pb); rb.f32(1032, pc)
            rb.f32(1034, p_tot)

            # Reactive power (kVAR)
            rb.f32(1036, qa); rb.f32(1038, qb); rb.f32(1040, qc)
            rb.f32(1042, q_tot)

            # Apparent power (kVA)
            rb.f32(1044, sa); rb.f32(1046, sb); rb.f32(1048, sc)
            rb.f32(1050, s_tot)

            # Power factor
            rb.f32(1052, pf_a); rb.f32(1054, pf_b); rb.f32(1056, pf_c)
            rb.f32(1058, pf_tot)

            # Fundamental harmonic PF (≈ PF for sinusoidal simulation)
            rb.f32(1060, pf_a); rb.f32(1062, pf_b); rb.f32(1064, pf_c)
            rb.f32(1066, pf_tot)

            # Frequency
            rb.f32(1068, freq); rb.f32(1070, freq)
            rb.f32(1072, freq); rb.f32(1074, freq)

            # §1.9.11  Energy Int64 (Wh / VARh / VAh)
            for i in range(3):
                rb.i64(2500 + i*4, int(self._wh[i]))
            rb.i64(2512, int(tw))
            # Reverse active (0 — load only)
            for a in [2516, 2520, 2524, 2528]: rb.i64(a, 0)

            for i in range(3):
                rb.i64(2532 + i*4, int(self._varh[i]))
            rb.i64(2544, int(tq))
            for a in [2548, 2552, 2556, 2560]: rb.i64(a, 0)

            for i in range(3):
                rb.i64(2564 + i*4, int(self._vah[i]))
            rb.i64(2576, int(tv))

            # Energy UInt32 (kWh / kVARh / kVAh)
            for i in range(3):
                rb.u32(2600 + i*2, max(0, int(self._wh[i]  / 1000)))
            rb.u32(2606, max(0, int(tw / 1000)))
            for a in [2608, 2610, 2612, 2614]: rb.u32(a, 0)

            for i in range(3):
                rb.u32(2616 + i*2, max(0, int(self._varh[i] / 1000)))
            rb.u32(2622, max(0, int(tq / 1000)))
            for a in [2624, 2626, 2628, 2630]: rb.u32(a, 0)

            for i in range(3):
                rb.u32(2632 + i*2, max(0, int(self._vah[i]  / 1000)))
            rb.u32(2638, max(0, int(tv / 1000)))

            # Tariff energy (all in tariff 1 for simplicity)
            rb.i64(2700, int(tw)); rb.u32(2750, max(0, int(tw / 1000)))
            for off in [4, 8, 12, 16, 20]:
                rb.i64(2700 + off, 0); rb.u32(2750 + off//2, 0)

            # §1.9.13  Demand (current power = current demand)
            for addr, val in [(3020, pa), (3022, pa),  # phase A active
                              (3028, pb), (3030, pb),  # phase B active
                              (3036, pc), (3038, pc),  # phase C active
                              (3044, p_tot), (3046, p_tot),  # total active
                              (3052, qa), (3054, qa),
                              (3060, qb), (3062, qb),
                              (3068, qc), (3070, qc),
                              (3076, q_tot), (3078, q_tot),
                              (3084, sa), (3086, sa),
                              (3092, sb), (3094, sb),
                              (3100, sc), (3102, sc),
                              (3108, s_tot), (3110, s_tot)]:
                rb.f32(addr, val)

            # Demand peak timestamps = now
            for addr in [3024, 3032, 3040, 3048, 3056, 3064, 3072,
                         3080, 3088, 3096, 3104, 3112]:
                rb.dt(addr, datetime.datetime.now())

            # §1.9.14  Current harmonics (~3.2 % THD, mostly 3rd harmonic)
            for base in [4000, 4002, 4004]: rb.f32(base, j(3.2, 0.05))   # total
            for base in [4006, 4008, 4010]: rb.f32(base, j(2.8, 0.05))   # odd
            for base in [4012, 4014, 4016]: rb.f32(base, j(0.9, 0.05))   # even
            # 1st harmonic value ≈ fundamental current
            rb.f32(4018, ia); rb.f32(4020, ib); rb.f32(4022, ic)
            # 3rd harmonic value (≈ 3% of fundamental)
            rb.f32(4024, ia*0.03); rb.f32(4026, ib*0.03); rb.f32(4028, ic*0.03)
            # Populate 2nd–50th harmonic percentage (4024-4316, stride 6 per harmonic)
            for h in range(2, 51):
                pct = max(0.0, 3.0 / h + random.gauss(0, 0.1))
                for ph in range(3):
                    rb.f32(4018 + (h-1)*6 + ph*2, j(pct, 0.1))
            # 2nd–50th harmonic value
            for h in range(2, 51):
                for ph, cur in enumerate([ia, ib, ic]):
                    rb.f32(4400 + (h-1)*6 + ph*2, cur * 3.0/h/100)

            # §1.9.14  Voltage harmonics (~1.5 % THD)
            for base in [5000, 5002, 5004]: rb.f32(base, j(1.5, 0.05))
            for base in [5006, 5008, 5010]: rb.f32(base, j(1.2, 0.05))
            for base in [5012, 5014, 5016]: rb.f32(base, j(0.5, 0.05))
            rb.f32(5018, ua); rb.f32(5020, ub); rb.f32(5022, uc)
            for h in range(2, 51):
                pct = max(0.0, 1.5 / h + random.gauss(0, 0.05))
                for ph in range(3):
                    rb.f32(5018 + (h-1)*6 + ph*2, j(pct, 0.1))
            for h in range(2, 51):
                for ph, vol in enumerate([ua, ub, uc]):
                    rb.f32(5400 + (h-1)*6 + ph*2, vol * 1.5/h/100)

            # §1.9.15  Max / min (tracked as ±5% of current reading)
            for addr, val in [(6000, ia), (6002, ib), (6004, ic),
                              (6006, i_avg), (6008, i_n)]:
                rb.f32(addr, val * 1.05)
            for addr, val in [(6010, ia), (6012, ib), (6014, ic),
                              (6016, i_avg), (6018, i_n)]:
                rb.f32(addr, val * 0.95)

            for addr, val in [(6020, ua), (6022, ub), (6024, uc), (6026, u_avg)]:
                rb.f32(addr, val * 1.02)
            for addr, val in [(6030, ua), (6032, ub), (6034, uc), (6036, u_avg)]:
                rb.f32(addr, val * 0.98)
            for addr, val in [(6040, uab), (6042, ubc), (6044, uca), (6046, ul_avg)]:
                rb.f32(addr, val * 1.02)
            for addr, val in [(6050, uab), (6052, ubc), (6054, uca), (6056, ul_avg)]:
                rb.f32(addr, val * 0.98)

            for addr, val in [(6060, pa), (6062, pb), (6064, pc), (6066, p_tot)]:
                rb.f32(addr, val * 1.05)
            for addr, val in [(6070, pa), (6072, pb), (6074, pc), (6076, p_tot)]:
                rb.f32(addr, val * 0.95)
            for addr, val in [(6080, qa), (6082, qb), (6084, qc), (6086, q_tot)]:
                rb.f32(addr, val * 1.05)
            for addr, val in [(6090, qa), (6092, qb), (6094, qc), (6096, q_tot)]:
                rb.f32(addr, val * 0.95)
            for addr, val in [(6100, sa), (6102, sb), (6104, sc), (6106, s_tot)]:
                rb.f32(addr, val * 1.05)
            for addr, val in [(6110, sa), (6112, sb), (6114, sc), (6116, s_tot)]:
                rb.f32(addr, val * 0.95)

            # §1.9.16  Unbalancedness (<1 % for balanced simulation)
            rb.f32(7000, j(0.50, 0.1));  rb.f32(7002, j(0.30, 0.1))
            rb.f32(7004, j(0.40, 0.1));  rb.f32(7006, j(0.20, 0.1))

            # §1.9.17  K-factor (~1.05 for slight harmonic distortion)
            rb.f32(8000, j(1.05, 0.01))
            rb.f32(8002, j(1.05, 0.01))
            rb.f32(8004, j(1.05, 0.01))

            # §1.9.18  Voltage/current angles (120° apart for balanced 3-phase)
            rb.f32(8100,   0.0);  rb.f32(8102, 120.0);  rb.f32(8104, 240.0)
            ang_a = math.degrees(phi[0])
            rb.f32(8110, -ang_a); rb.f32(8112, -ang_a); rb.f32(8114, -ang_a)

        self._log('MEAS_TICK',
                  Ia=f'{ia:.3f}A',    Ib=f'{ib:.3f}A',    Ic=f'{ic:.3f}A',
                  Ua=f'{ua:.2f}V',    Ub=f'{ub:.2f}V',    Uc=f'{uc:.2f}V',
                  P=f'{p_tot:.4f}kW', Q=f'{q_tot:.4f}kVAR',
                  PF=f'{pf_tot:.4f}', F=f'{freq:.3f}Hz')

    # ── config instruction processor ──────────────────────────────────────────

    def _process_instr(self):
        """Called after a FC16 write that includes register 300."""
        code   = self._instr[0]
        params = self._instr[1:]
        rb     = self.rb

        rb.set(424, [code])   # echo last instruction code
        result = 0            # 0 = success

        if code == 1001:      # System parameter setting
            # params: wiring(1), freq(1), nom_V(1), VT_hi(1), VT_lo(1), CT_hi(1), CT_lo(1)
            if len(params) < 7:
                result = 82
            elif params[0] > 5:
                result = 81
            else:
                rb.set(500, [params[0]])
                rb.set(501, [params[1]])
                rb.set(502, [params[2]])
                rb.u32(503, (params[3] << 16) | params[4])
                rb.u32(505, (params[5] << 16) | params[6])
                if params[2] > 0:
                    self.VOLTAGE = float(params[2])
                if params[1] in (50, 60):
                    self.FREQ = float(params[1])

        elif code == 1010:    # Current direction A/B/C
            if len(params) < 3:
                result = 82
            else:
                for i in range(3):
                    if params[i] not in (0, 1):
                        result = 81; break
                    rb.set(550 + i, [params[i]])

        elif code == 1011:    # Current channel A/B/C
            if len(params) < 3:
                result = 82
            else:
                for i in range(3):
                    if params[i] > 2:
                        result = 81; break
                    rb.set(553 + i, [params[i]])

        elif code == 1020:    # Zero drift suppression
            if len(params) < 2:
                result = 82
            elif params[0] > 1000 or params[1] > 1000:
                result = 81
            else:
                rb.set(600, [params[0]]); rb.set(601, [params[1]])

        elif code == 1060:    # Demand parameters
            if len(params) < 2:
                result = 82
            else:
                rb.set(3000, [params[0] & 1])
                rb.set(3001, [params[1]])

        elif code == 1070:    # Tariff mode
            if len(params) < 1:
                result = 82
            else:
                rb.set(801, [params[0] & 1])

        elif code == 1071:    # Manual tariff selection
            if len(params) < 1:
                result = 82
            elif params[0] > 5:
                result = 81
            else:
                rb.set(802, [params[0]])

        elif code == 1200:    # Device time setting
            # params: year, month, day, hour, minute, second (each 1 reg)
            if len(params) < 6:
                result = 82
            else:
                try:
                    dt = datetime.datetime(
                        params[0], max(1, params[1] & 0xFF),
                        max(1, params[2] & 0xFF),
                        params[3] & 0xFF, params[4] & 0xFF,
                        min(59, params[5] & 0xFF))
                    rb.dt(75, dt)
                except (ValueError, OverflowError):
                    result = 81

        elif code == 1210:    # Communication parameter setting
            # params: address, baud_index, parity, stop_bit
            if len(params) < 4:
                result = 82
            elif not (1 <= params[0] <= 247):
                result = 81
            elif params[1] > 6:
                result = 81
            elif params[2] > 2:
                result = 81
            elif params[3] not in (1, 2):
                result = 81
            else:
                rb.set(80, [params[0]])
                rb.set(81, [params[1]])
                rb.set(82, [params[2]])
                rb.set(83, [params[3] - 1])   # 1→0, 2→1
                self.address = params[0]       # live update

        elif code == 1301:    # Reset
            if len(params) < 1:
                result = 82
            else:
                t = params[0]
                if t == 1:    # reset max/min (no-op: already live)
                    pass
                elif t == 2:  # reset demand max
                    pass
                elif t == 3:  # reset tariff energy
                    pass
                elif t == 4:  # reset energy
                    self._wh = [0.0, 0.0, 0.0]
                    self._varh = [0.0, 0.0, 0.0]
                    self._vah  = [0.0, 0.0, 0.0]
                elif t == 5:  # reset ALL
                    self._wh = [0.0, 0.0, 0.0]
                    self._varh = [0.0, 0.0, 0.0]
                    self._vah  = [0.0, 0.0, 0.0]
                else:
                    result = 81

        else:
            result = 80   # invalid instruction code

        rb.set(425, [result])
        self._log('INSTR_EXEC',
                  code=code, params=self._instr[1:len(params)+1],
                  result=result, ok=(result == 0))

    # ── Modbus request handlers ───────────────────────────────────────────────

    def fc3(self, start: int, count: int):
        """Returns (words_list, None) or (None, error_code)."""
        if count < 1 or count > 125:
            return None, 0x03   # illegal data value
        if start + count > _RegBank.SIZE:
            return None, 0x02   # illegal data address
        with self.lock:
            data = self.rb.get(start, count)
        self._log('FC3_READ', start=start, count=count)
        return data, None

    def fc16(self, start: int, words: list):
        """Returns None on success or error_code."""
        count = len(words)
        # Only config instruction regs 300-423 are writable via FC16
        if start < 300 or (start + count) > 424:
            self._log('FC16_WRITE', start=start, count=count,
                      values=words, error='illegal_address')
            return 0x02
        self._log('FC16_WRITE', start=start, count=count, values=words)
        # Store words
        for i, w in enumerate(words):
            idx = start - 300 + i
            if 0 <= idx < 124:
                self._instr[idx] = w
        # If reg 300 was covered, process the instruction
        if start <= 300 < start + count:
            with self.lock:
                self._process_instr()
        # Also mirror into register bank for readback
        with self.lock:
            self.rb.set(start, words)
        return None

    def reject_reason(self, raw: bytes) -> str:
        """Return a short string explaining why handle() would return None."""
        if len(raw) < 4:
            return f'too_short({len(raw)}B)'
        if not _check_crc(raw):
            return f'bad_crc(got={raw[-2]:02x}{raw[-1]:02x}' \
                   f'_want={_crc16(raw[:-2]):04x})'
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

        if fc == 0x03:
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

        elif fc == 0x10:
            if len(raw) < 9:
                return None
            start      = (raw[2] << 8) | raw[3]
            count      = (raw[4] << 8) | raw[5]
            byte_count = raw[6]
            if len(raw) < 7 + byte_count + 2:
                return None
            words = [struct.unpack('>H', raw[7 + i*2: 9 + i*2])[0]
                     for i in range(count)]
            err = self.fc16(start, words)
            if err:
                resp = bytes([self.address, fc | 0x80, err])
            else:
                resp = bytes([self.address, fc,
                              raw[2], raw[3],   # echo start address
                              raw[4], raw[5]])  # echo count

        else:
            resp = bytes([self.address, fc | 0x80, 0x01])   # illegal function

        return _append_crc(resp)


# ── Serial server ──────────────────────────────────────────────────────────────

def _describe_frame(frame: bytes) -> str:
    """Human-readable summary of a raw Modbus RTU frame.

    FC3 request  = 8 bytes  (addr fc reg_hi reg_lo cnt_hi cnt_lo crc crc)
    FC3 response = 3 + N*2 + 2 bytes  (addr fc byte_cnt data... crc crc)
    FC16 request = 9+ bytes (addr fc reg_hi reg_lo cnt_hi cnt_lo byte_cnt data... crc crc)
    FC16 response= 8 bytes  (addr fc reg_hi reg_lo cnt_hi cnt_lo crc crc)
    """
    if len(frame) < 4:
        return frame.hex(' ')
    adr, fc = frame[0], frame[1]

    if fc == 0x03:
        if len(frame) == 8:                          # FC3 request
            start = (frame[2] << 8) | frame[3]
            count = (frame[4] << 8) | frame[5]
            return f'adr={adr} FC3 REQ  reg={start} count={count}'
        else:                                        # FC3 response
            byte_count = frame[2]
            return f'adr={adr} FC3 RESP regs={byte_count // 2} ({byte_count}B)'

    if fc == 0x10:
        if len(frame) == 8:                          # FC16 response (echo)
            start = (frame[2] << 8) | frame[3]
            count = (frame[4] << 8) | frame[5]
            return f'adr={adr} FC16 RESP reg={start} count={count}'
        elif len(frame) >= 9:                        # FC16 request
            start  = (frame[2] << 8) | frame[3]
            count  = (frame[4] << 8) | frame[5]
            nbytes = frame[6]
            words  = [struct.unpack('>H', frame[7+i*2:9+i*2])[0]
                      for i in range(min(count, nbytes // 2))]
            return f'adr={adr} FC16 REQ  reg={start} count={count} values={words}'

    if fc & 0x80:
        return f'adr={adr} FC{fc & 0x7f}|ERR code={frame[2] if len(frame) > 2 else "?"}'
    return f'adr={adr} FC{fc} [{frame.hex(" ")}]'


class RTUServer:
    def __init__(self, device: S111, port: str, baud: int, parity: str,
                 stopbits: int, update_hz: float):
        self.dev       = device
        self.port      = port
        self.baud      = baud
        self.parity    = {'N': serial.PARITY_NONE,
                          'E': serial.PARITY_EVEN,
                          'O': serial.PARITY_ODD}.get(parity, serial.PARITY_NONE)
        self.stopbits  = (serial.STOPBITS_ONE if stopbits == 1
                          else serial.STOPBITS_TWO)
        self.update_hz = update_hz
        self._ser      = None
        self._running  = False

    def _frame_gap(self) -> float:
        """Inter-byte silence timeout.

        Modbus spec says 3.5 char times, but USB-serial adapters add latency
        (OS buffering, USB frame cadence ~1 ms). Use 50 ms as a floor so a
        complete frame is always captured in one call, even on slow USB paths.
        """
        return max(0.05, 3.5 * 11.0 / self.baud)

    def _read_frame(self) -> bytes:
        self._ser.timeout = 0.5          # wait up to 500 ms for first byte
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
        print(f"SUTO S111 virtual device")
        print(f"  Port {self.port}  {self.baud} 8N{1 if self.stopbits == serial.STOPBITS_ONE else 2}"
              f"  Modbus address {self.dev.address}")
        print(f"  Voltage {self.dev.VOLTAGE} V  Current {self.dev.CURRENT} A  PF {self.dev.PF}")
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
        description="Virtual SUTO S111 three-phase power meter (Modbus RTU slave)")
    ap.add_argument("--port",     default="/dev/ttyUSB0",
                    help="Serial port (default: /dev/ttyUSB0)")
    ap.add_argument("--baud",     type=int, default=115200,
                    help="Baud rate: 1200-115200 (default: 115200)")
    ap.add_argument("--address",  type=int, default=1,
                    help="Modbus slave address 1-247 (default: 1)")
    ap.add_argument("--parity",   default="N", choices=["N", "E", "O"],
                    help="Parity N/E/O (default: N)")
    ap.add_argument("--stopbits", type=int, default=1, choices=[1, 2],
                    help="Stop bits (default: 1)")
    ap.add_argument("--voltage",  type=float, default=220.0,
                    help="Nominal phase-to-neutral voltage V (default: 220)")
    ap.add_argument("--current",  type=float, default=10.0,
                    help="Nominal phase current A (default: 10)")
    ap.add_argument("--pf",       type=float, default=0.92,
                    help="Power factor 0..1 (default: 0.92)")
    ap.add_argument("--freq",     type=float, default=50.0,
                    help="Grid frequency Hz (default: 50)")
    ap.add_argument("--update-hz", type=float, default=1.0,
                    help="Measurement update rate Hz (default: 1)")
    args = ap.parse_args()

    dev = S111(address=args.address, on_event=EventLogger())
    dev.VOLTAGE = args.voltage
    dev.CURRENT = args.current
    dev.PF      = args.pf
    dev.FREQ    = args.freq
    dev.tick()   # re-initialise with new values

    RTUServer(dev, args.port, args.baud, args.parity,
              args.stopbits, args.update_hz).run()


if __name__ == "__main__":
    main()
