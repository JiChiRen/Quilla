#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Quilla QBOOT serial upgrade host (reference implementation; pyserial + stdlib only)

Strictly mirrors the firmware-side protocol (see doc/qboot-app-architecture.md §8):

  Frame format (H->M and M->H identical):
    SOF(0xA5,1B) + CMD(1B) + LEN(2B,LE) + PAYLOAD(LEN) + CRC16(2B,LE)
    CRC covers [CMD][LEN][PAYLOAD]; algorithm = os/lib/crc16.c crc16_data
    (CCITT poly 0x1021, reflected, initial acc=0).

  Commands:
    ENTER 0x01 : payload = app_start(4B LE) + unlock_key(8B)        -> ACK 0x81
    DATA  0x02 : payload = start(4B) + end(4B) + data(<=256B)        -> ACK 0x82
    END   0x03 : payload = total_len(4B) + total_crc16(2B)           -> ACK 0x83
    SIGN  0x04 : payload = signature(64B)                           -> ACK 0x84
    NAK        : CMD = 0xC0|phase (0xC1/0xC2/0xC3/0xC4), payload = reason(1)

  Secure boot (active once a public key is flashed on the device):
    END only replies ACK and does not commit; the APP1 flag is written and the
    MCU reset only after the SIGN frame verifies. The signed object is
    image_len(4B LE) || SHA-256(image), identical to the firmware's
    quilla_qboot_secure_verify_descriptor(). The version is NOT part of the
    signed object: it is carried by the firmware image itself
    (QUILLA_APP_SECURITY_VERSION at a fixed flash address) and checked by QBOOT
    against its anti-rollback floor, so neither the host nor a malicious build
    can forge a lower version. Generate keys offline with quilla_ed25519_sign.py:
        python quilla_ed25519_sign.py keygen --dir keys
    Flash the public key into QBOOT via -DQUILLA_SECURE_BOOT_PUBKEY={...}; keep
    the private key on the production line.

  Entering upgrade mode has two sources (auto-detected by this tool):
    1) Device flag==QIAP (leftover from an interrupted upgrade): prints
       "[QIAP] enter upgrade mode. Send ENTER frame." -> send ENTER directly.
    2) Device reset into QBOOT shell from the APP via 'swqboot': prints the
       "[BOOT] Quilla V..." banner -> this tool sends the text command "qiap\n"
       and waits for "enter upgrade mode".
    Note: before entering upgrade mode (path 2) the port is in shell line-parsing
    mode and binary frames are swallowed, so 'qiap' must be sent first to switch
    to the raw byte stream.

Usage:
    pip install pyserial
    python quilla_host_updater.py COM3 app.bin             # Windows
    python quilla_host_updater.py /dev/ttyUSB0 app.bin     # Linux
    python quilla_host_updater.py COM3 app.bin -b 115200
    python quilla_host_updater.py COM3 app.bin --key keys/private.hex
    python quilla_host_updater.py --self-test              # offline self-check (no serial)

This file is the reference baseline for a production host; a UI can be layered on
top (Qt/C#/Electron, etc.), and the protocol layer (frame codec, session state
machine, CRC, retransmit) can be reused directly.
"""

import sys
import os
import time
import re
import struct
import argparse

# Pure-stdlib Ed25519 implementation in the same directory (quilla_ed25519_sign.py)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from quilla_ed25519_sign import sign_descriptor  # noqa: E402

# ---- Protocol constants (kept in sync with quilla-qiap.h / quilla-qboot-hal.h) ----
SOF = 0xA5

CMD_ENTER = 0x01
CMD_DATA = 0x02
CMD_END = 0x03
CMD_SIGN = 0x04     # payload = signature(64B)

ACK_ENTER = 0x81
ACK_DATA = 0x82
ACK_END = 0x83
ACK_SIGN = 0x84

# NAK phase: ENTER=1, DATA=2, SIGN=4, other=3  -> 0xC1 / 0xC2 / 0xC4 / 0xC3
NAK_ENTER = 0xC1
NAK_DATA = 0xC2
NAK_OTHER = 0xC3
NAK_SIGN = 0xC4

# NAK reason codes (kept in sync with quilla-qiap.h)
REASON = {
    0x01: "ADDR  address out of the APP region",
    0x02: "KEY   unlock key mismatch",
    0x03: "CRC   frame/field CRC error",
    0x04: "ERASE Flash page erase failed",
    0x05: "PROG  Flash program/readback failed",
    0x06: "LEN   total length or total CRC mismatch",
    0x07: "SZ    length field does not match data length",
    0x08: "SIGN  signature did not verify (unsigned or mismatched)",
    0x09: "VER   image has no usable security-version region "
          "(missing .quilla_secver section / magic mismatch)",
    0x0A: "ROLLBACK  firmware security version is below the device floor "
          "(downgrade rejected)",
}

# Partition layout (kept in sync with quilla-qboot-hal.h / arch/cpu/arm/<mcu>/soc.h)
# Physical order is BOOT-META-APP: QBOOT occupies the first 24 KB; the META region
# (0x08006000, 4 KB total, wear-levelled dual slots holding the flag/floor/seal/
# manifest records) sits right after QBOOT; the APP runs from APP_START to the end
# of flash (SOC_FLASH_END). Erase granularity is a page, so only page alignment is
# required.
#
# APP_START is fixed by the META layout (META_BASE + META_SIZE = 0x08007000) and is
# identical across all current MCUs, so it is kept as a default constant; but the
# real programmable ceiling APP_END depends on each MCU's SOC_FLASH_END
# (PY32F040xB = 128 KiB, GD32F502VE = 512 KiB) and must NEVER be hard-coded.
# Before an upgrade the tool asks the device for APP_START/APP_END via the QBOOT
# shell 'pinfo' command and derives APP_SIZE and MAX_IMAGE_SIZE from it -- this
# avoids wrongly rejecting GD32 (an old build hard-coded 102400 B) and keeps the
# host-side pre-check exactly aligned with the device-side ADDR NAK(0x01) bound.
APP_START = 0x08007000          # default/fallback (= META_BASE + META_SIZE, same for all MCUs)

# The following three are instance attributes filled in at run time from the
# device's 'pinfo' output; see Updater.__init__ and fetch_partition(). The module
# level no longer keeps hard-coded APP_END/APP_SIZE/MAX_IMAGE_SIZE.

# unlock key (quilla-qboot-hal.h: QUILLA_QBOOT_UNLOCK_KEY, default "QULLOTA1")
UNLOCK_KEY = bytes([0x51, 0x55, 0x4C, 0x4C, 0x4F, 0x54, 0x41, 0x31])

PAGE_SIZE = 256                 # QUILLA_QBOOT_PAGE_SIZE
# Data bytes per DATA frame: exactly one Flash page.
#
# The firmware USART2 RX is a CIRCULAR-mode DMA over a 1024-byte ring that never
# re-arms, so there is no re-arm window and NO per-frame size cap from the DMA
# (the old 64-byte NORMAL-DMA constraint that forced CHUNK=48 is gone). One page
# per frame is the sweet spot:
#   - frame = SOF(1)+CMD(1)+LEN(2)+payload(8 addr+CHUNK)+CRC(2) = 14 + CHUNK,
#     so a 256-byte chunk is a 270-byte frame, absorbed by the QBOOT 512-byte
#     second-stage ring in one shot;
#   - one frame == one flash page, so flash_write_range commits one
#     erase+program+verify per frame with no cross-frame partial-page staging;
#   - 5.3x fewer frames than the old 48-byte chunk => 5.3x fewer stop-and-wait
#     ACK round-trips, which is the real cost on a USB-serial link.
# The device caps a frame payload at QUILLA_QIAP_MAX_PAYLOAD = PAGE_SIZE + 8
# = 264 = 8 + CHUNK, so 256 is exactly the largest chunk the parser accepts.
CHUNK = 256
MAX_PAYLOAD = PAGE_SIZE + 8     # QUILLA_QIAP_MAX_PAYLOAD (= 264 = 8 addr + CHUNK)

# Communication parameters
ACK_TIMEOUT = 2.0               # per-frame ACK timeout (seconds)
MAX_RETRIES = 3                 # retransmit count on timeout / CRC error

# The SIGN frame needs its own timeout: on receiving SIGN the device runs a full
# Ed25519 verification. Measured on PY32F040xB @8MHz: SHA-256 0.56s + Ed25519
# 20.11s ~= 20.6s, far above a normal ACK timeout. Reusing 2.0s would report
# failure while the device actually succeeds; and retransmitting makes the device
# redo the 20s verify and desyncs the state machine.
SIGN_TIMEOUT = 90.0             # SIGN ACK timeout (seconds); must exceed the device verify time

# Upper bound to wait for the new APP to print its boot version stamp after the
# upgrade reset. After reset the device runs startup secure-boot checks again:
# SEALED mode (default) is SHA-256 + seal compare ~= 0.6 s; SECURE mode runs a
# full Ed25519 verify ~= 20 s every boot. The timeout must be generous enough for
# SECURE.
APP_STAMP_TIMEOUT = 45.0

# Banner fallback wait: see the note in Updater.ensure_upgrade_mode()
DEFAULT_BANNER_TIMEOUT = 15.0


# ---- CRC16 (faithful replica of os/lib/crc16.c crc16_add / crc16_data) ----------
def crc16_add(b, acc):
    """Byte-at-a-time; exactly mirrors the C crc16_add. acc is a 16-bit unsigned."""
    acc ^= b
    acc &= 0xFFFF
    acc = (acc >> 8) | (acc << 8)          # byte swap (within 16 bits)
    acc &= 0xFFFF
    acc ^= (acc & 0xFF00) << 4             # high half truncated by unsigned short
    acc &= 0xFFFF
    acc ^= (acc >> 8) >> 4
    acc &= 0xFFFF
    acc ^= (acc & 0xFF00) >> 5
    acc &= 0xFFFF
    return acc


def crc16(data, acc=0):
    """Mirrors the C crc16_data(data, len, acc)."""
    for b in data:
        acc = crc16_add(b, acc)
    return acc & 0xFFFF


# ---- Frame codec ---------------------------------------------------------------
def build_frame(cmd, payload):
    body = bytes([cmd]) + struct.pack("<H", len(payload)) + payload
    crc = crc16(body)
    return bytes([SOF]) + body + struct.pack("<H", crc)


# ---- Updater ------------------------------------------------------------------
class Updater:
    def __init__(self, port, baud=115200, verbose=True,
                 banner_timeout=DEFAULT_BANNER_TIMEOUT):
        try:
            import serial
        except ImportError:
            raise RuntimeError("pyserial is missing; run: pip install pyserial")
        # Accept a bare number ("7") as a Windows port name ("COM7").
        if isinstance(port, str) and port.isdigit():
            port = "COM" + port
        try:
            self.ser = serial.Serial(port, baud, timeout=0.1,
                                     dsrdtr=False, rtscts=False)
        except serial.SerialException as e:
            msg = str(e)
            if "denied" in msg.lower() or "access" in msg.lower():
                raise RuntimeError(
                    "Cannot open %s: access denied. The port is usually held by "
                    "another serial program (a serial assistant / ComAssistant / "
                    "sscom, etc.).\nClose the other serial tool and re-run this "
                    "script." % port)
            raise
        # Some USB-serial boards reset the MCU on DTR/RTS transitions; force both
        # low to avoid an unintended reset the moment the port opens, which could
        # kick the board out of the "upgrade idle" state into the QBOOT shell and
        # desync the state machine.
        try:
            self.ser.dtr = False
            self.ser.rts = False
        except Exception:
            pass
        self.banner_timeout = banner_timeout
        self.verbose = verbose
        # Partition bounds (fetched at run time from the device 'pinfo' by
        # fetch_partition(); the values here are fallbacks). APP_START is the same
        # for all MCUs; APP_END depends on SOC_FLASH_END and must come from the
        # device.
        self.app_start = APP_START
        self.app_end = 0x0801FFFF          # fallback (PY32F040xB value), overwritten on success
        self.app_size = self.app_end - self.app_start + 1
        self.max_image_size = None         # None = not fetched; host does not enforce a ceiling, defers to device NAK
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()
        time.sleep(0.1)

    def log(self, msg):
        if self.verbose:
            print(msg)

    def close(self):
        """Release the serial port (idempotent; safe to call on any exit path).

        Closing promptly matters: (1) it frees the COM port for a terminal /
        debugger / a re-run of the script; (2) it releases the DTR/RTS control
        lines (some USB-serial boards reset the MCU from them).
        """
        try:
            if getattr(self, "ser", None) is not None:
                self.ser.close()
        except Exception:
            pass

    def _drain_text(self, timeout=2.0):
        """Read the port for a while and return the accumulated bytes (for banner auto-detection)."""
        buf = b""
        t0 = time.time()
        while time.time() - t0 < timeout:
            n = self.ser.in_waiting
            if n:
                buf += self.ser.read(n)
            else:
                time.sleep(0.02)
        return buf

    def _query_line(self, cmd, timeout=2.0):
        """Send one shell command line and read back everything within timeout.

        Used mainly for 'part' fingerprinting (both the QBOOT and APP shells have
        this command) and to expose the board's raw echo for diagnostics. In the
        upgrade-idle state (raw sink has taken over) these bytes are dropped by
        the frame parser with no echo, which is exactly what distinguishes
        "upgrade-idle" from "APP / unresponsive".
        """
        self.ser.reset_input_buffer()
        self.ser.write(cmd)
        return self._drain_text(timeout=timeout)

    def _wait_text(self, needle, timeout=3.0):
        """Read the port for timeout seconds; return True if needle appears."""
        t0 = time.time()
        acc = b""
        while time.time() - t0 < timeout:
            if self.ser.in_waiting:
                acc += self.ser.read(self.ser.in_waiting)
            if needle in acc:
                return True
            time.sleep(0.02)
        return False

    def _recv_frame(self, timeout=ACK_TIMEOUT):
        """Sync to SOF in the serial stream and parse one frame. Non-0xA5 bytes
        (text banner) are skipped. Returns (cmd, payload); None on timeout."""
        t0 = time.time()
        state = 0          # 0=S_SOF 1=S_CMD 2=S_LEN1 3=S_LEN2
                           # 4=S_PAYLOAD 6=S_CRC1 7=S_CRC2
        cmd = 0
        length = 0
        payload = b""
        calc_crc = 0
        crc_lo = 0
        crc_hi = 0
        while time.time() - t0 < timeout:
            if self.ser.in_waiting == 0:
                time.sleep(0.004)
                continue
            b = self.ser.read(1)[0]
            if state == 0:                      # S_SOF
                if b == SOF:
                    state = 1
                    calc_crc = 0
                # else: drop (text)
            elif state == 1:                    # S_CMD
                cmd = b
                calc_crc = crc16(bytes([b]), calc_crc)
                state = 2
            elif state == 2:                    # S_LEN1
                length = b
                calc_crc = crc16(bytes([b]), calc_crc)
                state = 3
            elif state == 3:                    # S_LEN2
                length |= (b << 8)
                calc_crc = crc16(bytes([b]), calc_crc)
                payload = b""
                if length == 0:
                    state = 6
                elif length > MAX_PAYLOAD:
                    state = 0                    # over-long -> drop and wait for the next SOF
                else:
                    state = 4
            elif state == 4:                    # S_PAYLOAD
                payload += bytes([b])
                calc_crc = crc16(bytes([b]), calc_crc)
                if len(payload) >= length:
                    state = 6
            elif state == 6:                    # S_CRC1
                crc_lo = b
                state = 7
            elif state == 7:                    # S_CRC2
                crc_hi = b
                got = (crc_hi << 8) | crc_lo
                state = 0                        # always resync, right or wrong
                if got == calc_crc:
                    return (cmd, payload)
                # CRC error: the device sends a separate NAK(0xC3) frame; keep reading
        return None

    def _send_frame(self, cmd, payload):
        # The device RX is a CIRCULAR DMA over a 1024-byte ring that never
        # re-arms, so no host-side pacing is needed -- write the whole frame at
        # once. (The old 64-byte/12 ms pacing only dodged the NORMAL-DMA re-arm
        # window and just wasted time.) Stop-and-wait per frame keeps the
        # USB-serial adapter from over-buffering.
        self.ser.write(build_frame(cmd, payload))

    def _transact(self, cmd, payload, ack_cmd, retries=MAX_RETRIES,
                  timeout=ACK_TIMEOUT):
        """Send a command and wait for the matching ACK. Returns the payload bytes
        on success; raises on failure.

        timeout can be overridden per frame type: a SIGN frame makes the device
        run a ~20s Ed25519 verify, so SIGN_TIMEOUT must be passed, otherwise a
        success is misreported as a failure.
        """
        last_exc = None
        for attempt in range(1, retries + 1):
            self._send_frame(cmd, payload)
            resp = self._recv_frame(timeout=timeout)
            if resp is None:
                last_exc = TimeoutError(
                    "Timed out waiting for ACK 0x%02X (attempt %d/%d)"
                    % (ack_cmd, attempt, retries))
                continue
            rcmd, rpayload = resp
            if rcmd == ack_cmd:
                return rpayload
            if rcmd in (NAK_ENTER, NAK_DATA, NAK_OTHER, NAK_SIGN):
                reason = rpayload[0] if rpayload else 0
                txt = REASON.get(reason, "unknown(0x%02X)" % reason)
                # CRC error(0x03) is retransmittable; everything else is a
                # parameter/Flash error, so stop immediately.
                if reason == 0x03:
                    last_exc = RuntimeError(
                        "NAK(0x%02X) reason=0x%02X %s (will retransmit)"
                        % (rcmd, reason, txt))
                    continue
                raise RuntimeError(
                    "NAK(0x%02X) reason=0x%02X %s" % (rcmd, reason, txt))
            last_exc = RuntimeError("Unexpected response 0x%02X" % rcmd)
            continue
        raise last_exc

    def fetch_partition(self):
        """Ask the QBOOT shell for the partition layout, overwriting
        self.app_start/end/size.

        Only usable while in the QBOOT interactive shell (not the upgrade raw
        sink). The device echoes `[PART] APP_START=0x... APP_END=0x... APP_SIZE=0x...`
        for the `pinfo` command, which this function parses; the resulting
        max_image_size matches the device-side ADDR NAK(0x01) bound.

        When it cannot be fetched (old firmware without pinfo / already in the
        upgrade raw sink) it returns False and the caller skips the host-side
        pre-check, deferring to the device ADDR NAK -- this is less likely to
        cause harm than hard-coding a PY32F040xB value.
        """
        self.ser.reset_input_buffer()
        out = self._query_line(b"pinfo\n", timeout=2.0)
        txt = out.decode("latin1", errors="replace")
        m_s = re.search(r"APP_START=0x([0-9A-Fa-f]+)", txt)
        m_e = re.search(r"APP_END=0x([0-9A-Fa-f]+)", txt)
        if m_s and m_e:
            self.app_start = int(m_s.group(1), 16)
            self.app_end = int(m_e.group(1), 16)
            self.app_size = self.app_end - self.app_start + 1
            self.max_image_size = self.app_size
            self.log("[*] Got partition layout from device: APP_START=0x%08X APP_END=0x%08X "
                     "APP_SIZE=%d B (0x%X)" % (self.app_start, self.app_end,
                                               self.app_size, self.app_size))
            return True
        self.log("[!] pinfo did not echo the partition (old firmware?): the host "
                 "does not enforce a ceiling; the device ADDR NAK(0x01) enforces "
                 "the real APP-region bound")
        return False

    def ensure_upgrade_mode(self):
        """Auto-detect and enter upgrade mode, then send ENTER in place to set the
        QIAP flag.

        State detection cannot rely on "send a newline and look for a `>` echo":
        the shell does NOT reprint the prompt on an empty line, so both the shell
        state and the upgrade-idle state return an empty probe and cannot be told
        apart.

        This function adapts across four states and echoes the board's raw
        response at each step for on-target diagnosis:
          A. Upgrade mode (banner contains 'enter upgrade mode', leftover
             flag==QIAP): send ENTER directly.
          B. QBOOT shell (part echo contains 'qboot', or banner contains
             '[BOOT]'): send 'qiap' -> prints 'enter upgrade mode' -> ENTER.
          C. APP shell (part echo contains 'app1'): 'swqboot' resets into the
             QBOOT shell -> wait for the '[BOOT]' banner -> 'qiap' -> ENTER.
          D. Upgrade-idle (raw sink has taken over; part/qiap bytes are dropped
             by the frame parser with no echo): send ENTER directly.

        Note: before entering upgrade mode the port is in shell line-parsing mode
        and binary frames are swallowed, so 'qiap' must be sent first to switch to
        the raw byte stream; only then can ENTER/DATA/END frames be sent.
        """
        self.log("[*] Reading startup banner (4s)...")
        banner = self._drain_text(timeout=4.0)
        self.log("[banner] %r" % banner)
        text0 = banner.decode("latin1", errors="replace")
        if "enter upgrade mode" in text0:
            self.log("[*] Device already in upgrade mode (leftover flag==QIAP), sending ENTER directly")
            self.ser.reset_input_buffer()
            self.enter()
            return

        # Use the 'part' fingerprint to identify the partition (both the QBOOT and
        # APP shells have this command; in the upgrade-idle state it is swallowed
        # by the raw sink with no echo). This is the key diagnostic that tells
        # B/C/D apart.
        self.ser.reset_input_buffer()
        part = self._query_line(b"part\n", timeout=2.0)
        self.log("[part] %r" % part)
        part_txt = part.decode("latin1", errors="replace")
        in_qboot = ("qboot" in part_txt) or ("[BOOT]" in text0)

        if not in_qboot:
            # Two distinct non-QBOOT states need DIFFERENT handling:
            #   C) APP shell alive -> `part` echoes (e.g. "app1"). The shell owns
            #      the line scanner, so a binary ENTER frame is counter-productive:
            #      its printable bytes (app_start 0x70='p' + the "QULLOTA1" key)
            #      land in the 128-byte line buffer with NO terminator, and the
            #      later 'swqboot\n' is glued behind that garbage and never matches
            #      a command. So do NOT send ENTER here -- go straight to swqboot.
            #   D) upgrade-idle (a prior QIAP flag is set): the raw sink swallows
            #      `part`, so it comes back empty; only a binary ENTER works.
            if part_txt.strip():
                self.log("[*] APP shell alive (part=%r); skipping binary ENTER "
                         "(it would corrupt the shell line), going to swqboot..." % part)
            else:
                self.log("[*] No shell response (upgrade-idle?); trying ENTER directly...")
                try:
                    self.enter()
                    return
                except Exception as e:
                    self.log("[!] Direct ENTER failed: %s" % e)

            # C: APP shell -> swqboot reset into the QBOOT shell.
            # Flush the line buffer first: anything still in it (a partial command,
            # or the printable leftovers of a failed binary ENTER) has no
            # terminator, so a bare 'swqboot\n' would be appended behind it. A CR
            # terminates/clears that line before the real command goes out.
            self.log("[*] Sending 'swqboot' to reset into QBOOT ...")
            got_boot = False
            for attempt in range(1, 3):
                self.ser.reset_input_buffer()
                self.ser.write(b"\r\n")     # terminate/clear any pending line
                time.sleep(0.1)
                self.ser.write(b"swqboot\n")
                if self._wait_text(b"[BOOT]", timeout=6.0):
                    got_boot = True
                    break
                self.log("[!] swqboot attempt %d: no '[BOOT]' banner "
                         "(reset may not have happened / flag write failed / still in the APP)"
                         % attempt)
            if not got_boot:
                self.log("[!] swqboot did not drop the board into the QBOOT shell; "
                         "type 'swqboot' manually at the APP prompt, then re-run")
            self.ser.reset_input_buffer()

        # We should now be in the QBOOT shell (or the upgrade-idle state). Before
        # entering upgrade mode, fetch the real partition bounds from the SoC
        # (APP_END depends on each MCU's SOC_FLASH_END) for the host-side
        # pre-check ceiling.
        self.fetch_partition()

        # Send 'qiap' -> wait for 'enter upgrade mode', up to 3 attempts, clearing
        # the input buffer before each send.
        for attempt in range(1, 4):
            self.ser.reset_input_buffer()
            self.ser.write(b"qiap\n")
            if self._wait_text(b"enter upgrade mode", timeout=3.0):
                self.log("[*] Switched to upgrade mode (qiap attempt %d), sending ENTER" % attempt)
                self.ser.reset_input_buffer()
                self.enter()
                return
            self.log("[!] qiap attempt %d did not enter upgrade mode" % attempt)

        raise RuntimeError(
            "Failed to enter upgrade mode. Confirm the board is at the QBOOT shell "
            "(send 'qiap' manually, or press reset and re-run), or run 'swqboot' "
            "from the APP first.\nDiagnostics: banner=%r, part=%r" % (banner, part))

    def enter(self):
        payload = struct.pack("<I", self.app_start) + UNLOCK_KEY
        self.log("[*] Sending ENTER (app_start=0x%08X, key=QULLOTA1)..." % self.app_start)
        r = self._transact(CMD_ENTER, payload, ACK_ENTER)
        status = r[0] if r else 0
        if status != 0:
            raise RuntimeError("ENTER returned status=%d" % status)
        self.log("[+] ENTER succeeded (QIAP flag set: recoverable if power is lost mid-upgrade)")

    def send_data(self, data, base=None):
        if base is None:
            base = self.app_start
        total_len = 0
        total_crc = 0
        n = len(data)
        nchunks = (n + CHUNK - 1) // CHUNK
        for i in range(nchunks):
            start_addr = base + i * CHUNK
            chunk = data[i * CHUNK:(i + 1) * CHUNK]
            end_addr = start_addr + len(chunk) - 1
            payload = struct.pack("<II", start_addr, end_addr) + chunk
            r = self._transact(CMD_DATA, payload, ACK_DATA)
            echoed = struct.unpack("<I", r[:4])[0]
            if echoed != start_addr:
                # Structural misalignment: the device lost/mis-parsed a whole
                # frame (second-ring overflow), and resending the same chunk
                # cannot self-heal -- the board must be reset and the run
                # repeated (a re-run re-ENTERS and resets the session). Fail
                # cleanly here instead of pointlessly retrying 5 times.
                raise RuntimeError(
                    "DATA chunk %d echoed a mismatched address (expected 0x%08X, got 0x%08X); "
                    "the upgrade session is misaligned, reset the board and re-run"
                    % (i, start_addr, echoed))
            total_len += len(chunk)
            total_crc = crc16(chunk, total_crc)     # chained accumulation (matches the device)
            pct = (i + 1) * 100 // nchunks
            sys.stdout.write("\r    Flashing %3d%% (%d/%d)" % (pct, i + 1, nchunks))
            sys.stdout.flush()
        print()
        return total_len, total_crc

    def end(self, total_len, total_crc):
        payload = struct.pack("<I", total_len) + struct.pack("<H", total_crc)
        self.log("[*] Sending END (total_len=%d, total_crc=0x%04X)..."
                 % (total_len, total_crc))
        r = self._transact(CMD_END, payload, ACK_END)
        status = r[0] if r else 0
        if status != 0:
            raise RuntimeError("END returned status=%d" % status)
        # Note: if the device already has a verification public key flashed, END
        # only replies ACK and does not commit; the APP1 flag is written and the
        # MCU reset only after the SIGN frame verifies.
        self.log("[+] END succeeded (whether SIGN is still needed depends on the public key flashed on the device)")

    def sign(self, total_len, data, seed):
        """Send the SIGN frame: Ed25519 signature (64B).

        The signed object is image_len(4B LE) || SHA-256(image), identical to the
        firmware's quilla_qboot_secure_verify_descriptor(). The version is carried
        by the firmware image itself and is NOT passed in this frame; QBOOT reads
        it and rejects a downgrade below its floor.
        """
        import hashlib

        digest = hashlib.sha256(data).digest()
        sig = sign_descriptor(seed, total_len, digest)
        payload = sig
        self.log("[*] Sending SIGN (64B signature); device-side verify takes ~20s, "
                 "waiting up to %.0fs..." % SIGN_TIMEOUT)
        # retries=1: one verify already takes ~20s; retransmitting makes the
        # device redo it and desyncs the state machine.
        r = self._transact(CMD_SIGN, payload, ACK_SIGN, retries=1,
                           timeout=SIGN_TIMEOUT)
        status = r[0] if r else 0
        if status != 0:
            raise RuntimeError("SIGN returned status=%d" % status)
        self.log("[+] UPGRADE COMPLETE: signature verified, manifest written, "
                 "APP1 set. The device is now auto-rebooting into the APP "
                 "(QBOOT signaled completion with ACK 0x%02X)." % ACK_SIGN)

    def confirm_app_stamp(self, timeout=APP_STAMP_TIMEOUT):
        """Best-effort post-check that the new APP booted and printed its version
        stamp (`[QAPP] <name> V<ver>`) after the reboot.

        This is NOT the success criterion: the upgrade is already complete when
        QBOOT sends its completion ACK (ACK_SIGN 0x84 keyed, or ACK_END 0x83
        no-key), which it only does after the image is committed, verified, the
        manifest written and flag=APP1 set. Here we merely confirm end-to-end
        that the image actually boots. It never raises -- a missing or slow stamp
        (SEALED re-verifies the seal ~0.6 s; SECURE re-runs Ed25519 ~20 s; some
        builds print no stamp) is a warning, not a failure. Serial data arrives
        in chunks, so the stamp line is only accepted once complete (CR/LF, or
        0.4 s of quiet), never a partial prefix.

        Returns the stamp string on success, or None on timeout.
        """
        needle = b"[QAPP]"
        t0 = time.time()
        acc = b""
        last_tick = -1.0
        last_rx = None
        while time.time() - t0 < timeout:
            if self.ser.in_waiting:
                acc += self.ser.read(self.ser.in_waiting)
                last_rx = time.time()
            i = acc.find(needle)
            if i >= 0:
                rest = acc[i:]
                # Serial data arrives in chunks: only accept the stamp once the
                # line is complete (CR/LF seen after it), otherwise a partial
                # line like "[QAPP] Pr" gets reported as the whole version.
                eol = -1
                for sep in (b"\r", b"\n"):
                    p = rest.find(sep)
                    if p >= 0 and (eol < 0 or p < eol):
                        eol = p
                if eol >= 0:
                    stamp = rest[:eol].decode("ascii", "replace")
                    self.log("[+] Confirmed: new APP is running, boot version stamp: %s" % stamp)
                    return stamp
                acc = acc[i:]           # keep only the stamp line; bound growth
                # Some builds print the stamp with no line ending: accept once
                # the line has been quiet for 0.4 s.
                if last_rx is not None and time.time() - last_rx >= 0.4:
                    stamp = acc.decode("ascii", "replace").rstrip("\r\n")
                    self.log("[+] Confirmed: new APP is running, boot version stamp: %s" % stamp)
                    return stamp
            elapsed = time.time() - t0
            tick = int(elapsed // 5.0)
            if tick != last_tick:
                last_tick = tick
                self.log("[*] Confirming the new APP boot stamp... (waited %.0fs / %.0fs)"
                         % (elapsed, timeout))
            time.sleep(0.02)
        self.log("[!] No '[QAPP]' boot stamp within %.1fs. The flash already "
                 "succeeded (QBOOT committed the image); the board may still be in "
                 "secure verify, or this build prints no stamp. Use --probe if you "
                 "want to double-check." % timeout)
        return None

    def run(self, bin_path, key_path=None):
        with open(bin_path, "rb") as f:
            data = f.read()
        if len(data) == 0:
            raise RuntimeError("Firmware file is empty")

        # First establish the upgrade session (including fetching the real APP-region
        # bounds from the device 'pinfo'), then do the host-side pre-check against
        # that. If it cannot be fetched (old firmware / already in upgrade mode),
        # max_image_size is None, the pre-check is skipped, and the device ADDR
        # NAK(0x01) enforces the real bound -- avoiding a hard-coded PY32F040xB
        # value that would wrongly reject GD32.
        self.ensure_upgrade_mode()
        if self.max_image_size is not None and len(data) > self.max_image_size:
            raise RuntimeError(
                "Firmware is %d bytes, exceeding the programmable ceiling of %d bytes "
                "(APP-region capacity, taken from the device SoC; the last page needs "
                "no manifest reservation -- the manifest lives in the separate META region)"
                % (len(data), self.max_image_size))

        seed = None
        if key_path:
            with open(key_path, "r", encoding="ascii") as f:
                seed = bytes.fromhex(f.read().strip())
            if len(seed) != 32:
                raise RuntimeError("The private-key file should be a 32-byte seed in hex, got %d bytes"
                                   % len(seed))
            self.log("[*] Using signing private key: %s" % key_path)
        else:
            self.log("[!] No --key given: no SIGN frame will be sent. "
                     "If the device has a public key flashed, the upgrade is rejected at SIGN.")

        total_len, total_crc = self.send_data(data)
        self.end(total_len, total_crc)
        if seed is not None:
            # With a public key: END only replies ACK; QBOOT commits (manifest,
            # seal, flag=APP1) on SIGN and acknowledges with ACK_SIGN only AFTER
            # all of that -- that ACK is the completion code.
            self.sign(total_len, data, seed)
        else:
            # Without a public key END itself commits + resets; that ACK is the
            # completion code.
            self.log("[+] UPGRADE COMPLETE: image committed (no signature "
                     "required). The device is now auto-rebooting into the APP.")
        # Best-effort end-to-end confirmation that the committed image boots and
        # prints a version stamp. NOT fatal: the upgrade already completed above.
        self.confirm_app_stamp()


# ---- Offline self-check (no serial dependency) ---------------------------------
def self_test():
    print("[self-test] CRC and frame codec round-trip...")
    # 1) Frame codec round-trip: build_frame, then parsed back by the same
    #    _recv_frame logic.
    for cmd, payload in [(CMD_ENTER, struct.pack("<I", APP_START) + UNLOCK_KEY),
                         (CMD_DATA, struct.pack("<II", APP_START, APP_START + 0xFF) + b"\xAA" * 256),
                         (CMD_END, struct.pack("<I", 1234) + struct.pack("<H", 0xBEEF))]:
        frame = build_frame(cmd, payload)
        assert frame[0] == SOF
        # Replay on an in-memory "serial port": call the parse logic directly.
        state = 0
        rcmd = 0
        length = 0
        rpayload = b""
        calc_crc = 0
        crc_lo = 0
        crc_hi = 0
        ok = False
        for b in frame:
            if state == 0:
                if b == SOF:
                    state = 1
                    calc_crc = 0
            elif state == 1:
                rcmd = b
                calc_crc = crc16(bytes([b]), calc_crc)
                state = 2
            elif state == 2:
                length = b
                calc_crc = crc16(bytes([b]), calc_crc)
                state = 3
            elif state == 3:
                length |= (b << 8)
                calc_crc = crc16(bytes([b]), calc_crc)
                rpayload = b""
                state = 4 if length > 0 else 6
            elif state == 4:
                rpayload += bytes([b])
                calc_crc = crc16(bytes([b]), calc_crc)
                if len(rpayload) >= length:
                    state = 6
            elif state == 6:
                crc_lo = b
                state = 7
            elif state == 7:
                crc_hi = b
                if ((crc_hi << 8) | crc_lo) == calc_crc:
                    ok = True
                break
        assert ok, "frame round-trip CRC check failed cmd=0x%02X" % cmd
        assert rcmd == cmd and rpayload == payload, "frame round-trip content mismatch"
    # 2) Chained CRC accumulation matches the device (reproduction of rx_total_crc)
    d0 = b"\x00" * 100
    d1 = b"\xAB\xCD\xEF" * 10
    assert crc16(d0 + d1) == crc16(d1, crc16(d0)), "chained CRC mismatch"
    print("[self-test] PASS: frame format, CRC and chained accumulation all match the firmware")


def main():
    ap = argparse.ArgumentParser(
        description="Quilla QBOOT serial upgrade host (reference implementation)")
    ap.add_argument("port", nargs="?", help="serial port, e.g. COM3 or /dev/ttyUSB0")
    ap.add_argument("binfile", nargs="?", help="APP firmware .bin (raw binary)")
    ap.add_argument("-b", "--baud", type=int, default=115200, help="baud rate (default 115200)")
    ap.add_argument("-q", "--quiet", action="store_true", help="reduce output")
    ap.add_argument("--self-test", action="store_true", help="offline self-check (no serial)")
    ap.add_argument("--probe", action="store_true",
                    help="only probe the board: read the banner and send part/help to see the echo; no upgrade")
    ap.add_argument("--key", metavar="PATH",
                    help="signing private key (private.hex, 32B seed). "
                         "Required when the device has a public key flashed, so the upgrade sends a SIGN frame")
    ap.add_argument("--banner-timeout", type=float, default=DEFAULT_BANNER_TIMEOUT,
                    metavar="SEC", help="banner fallback wait in seconds (default %.0f)"
                    % DEFAULT_BANNER_TIMEOUT)
    args = ap.parse_args()

    if args.self_test:
        self_test()
        return

    if args.probe:
        up = Updater(args.port, args.baud, verbose=True,
                      banner_timeout=args.banner_timeout)
        try:
            banner = up._drain_text(timeout=4.0)
            print("[probe banner] %r" % banner)
            part = up._query_line(b"part\n", timeout=2.0)
            print("[probe part]   %r" % part)
            help_r = up._query_line(b"help\n", timeout=2.0)
            print("[probe help]   %r" % help_r)
            pinfo = up._query_line(b"pinfo\n", timeout=2.0)
            print("[probe pinfo]  %r" % pinfo)
            print("[probe done] Board state, judged from the above:")
            print("   part contains 'qboot'      -> QBOOT interactive shell ('qiap' upgrades directly)")
            print("   part contains 'app1'       -> APP is running ('swqboot' to reset first)")
            print("   part empty and no echo     -> upgrade-idle (waiting for ENTER) or no serial link")
            print("   pinfo contains '[PART]'    -> APP_START/END/SIZE, to confirm the flash ceiling (GD32=512K)")
        finally:
            up.close()
        return

    if not args.port or not args.binfile:
        ap.error("both port and binfile are required (or use --self-test)")

    up = None
    try:
        up = Updater(args.port, args.baud, verbose=not args.quiet,
                      banner_timeout=args.banner_timeout)
        up.run(args.binfile, key_path=args.key)
    except KeyboardInterrupt:
        # Ctrl+C: cleanly release the port and return to the shell, no traceback.
        print("\n[!] Interrupted by the user (Ctrl+C): serial released, returning to the shell.",
              file=sys.stderr)
        if up is not None:
            up.close()
        sys.exit(130)
    except Exception as e:
        print("[!] Failed: %s" % e, file=sys.stderr)
        if up is not None:
            up.close()
        sys.exit(1)
    finally:
        # Ultimate safety net: whatever happens (success / timeout / error /
        # interrupt), make sure the serial port is released.
        if up is not None:
            up.close()


if __name__ == "__main__":
    main()
