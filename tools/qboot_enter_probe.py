#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright 2026 JiChiRen Info. Tech.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# This file is part of Quilla.

"""
CR-0055 on-target probe: send a single 16-byte ENTER frame with a FORCED
image security version, bypassing the host-side SEC VER pre-check.

Purpose: the regular updater skips the transfer locally when image_secver <
device FW_SECVER (CR-0053), so the device-side ENTER gate (NAK 0x0A) can never
be observed through the normal tool. This probe drives the same state machine
(banner -> part fingerprint -> swqboot/qiap) but forces the secver value, so
the rejection happens on the device, before any DATA is written.

Typical verification step (CR-0055 #3): on a board whose floor is 2,

    python qboot_enter_probe.py COM7 --secver 1

expected: NAK 0xC4 reason=0x0A (ROLLBACK), no DATA sent, and after a reset the
original APP boots normally (the QIAP flag is only set when ENTER is ACCEPTED).

If the ENTER is ACCEPTED (forced secver >= floor), the board stays in
upgrade-idle (flag=QIAP): power-cycle it and run a full upgrade with the
regular updater to restore a known-good state.

This is a verification aid, not part of the normal update flow.
"""

import argparse
import sys

from quilla_host_updater import Updater


def main():
    ap = argparse.ArgumentParser(
        description="Send one ENTER frame with a forced SEC VER (device-side "
                    "rollback gate test, CR-0055)")
    ap.add_argument("port", help="serial port, e.g. COM7 or 7")
    ap.add_argument("--secver", type=int, default=1,
                    help="forced image security version in the ENTER frame "
                         "(default: 1)")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    u = Updater(args.port, baud=args.baud)
    u.image_secver = args.secver   # forced; no image file is involved
    # The CR-0053 host-side pre-check runs INSIDE ensure_upgrade_mode() and
    # would reject the forced downgrade locally (swapp back to APP) before a
    # single frame is sent -- the exact path this probe must NOT take, since
    # its whole purpose is to observe the DEVICE-side gate. Disable it
    # explicitly; the device NAK is the outcome under test.
    u._precheck_secver = lambda: True
    try:
        ok = u.ensure_upgrade_mode()
    except RuntimeError as e:
        u.log("[!] ENTER rejected: %s" % e)
        u.log("[*] No DATA was written and the QIAP flag was not set: "
              "reset the board (power cycle / reset button) and the "
              "original APP boots normally.")
        return 1
    except (TimeoutError, OSError) as e:
        u.log("[!] Transport error: %s" % e)
        return 2
    if not ok:
        # Should not happen with the host pre-check disabled; never claim
        # success without an ACK.
        u.log("[!] ensure_upgrade_mode returned False (upgrade skipped "
              "before ENTER); nothing was flashed")
        return 1
    u.log("[+] ENTER ACCEPTED (forced secver=%u >= device floor): the board "
          "is now in upgrade-idle (flag=QIAP). Nothing was flashed. To "
          "restore: power-cycle and run a full upgrade with the regular "
          "updater (any image with secver >= floor)." % args.secver)
    return 0


if __name__ == "__main__":
    sys.exit(main())
