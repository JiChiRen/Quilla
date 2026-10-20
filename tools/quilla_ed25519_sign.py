#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Quilla firmware signing key utility (RFC 8032 Ed25519).

Pure standard library: no third-party crypto, so it can run anywhere the
host upgrader runs. Signing only -- the device verifies, never signs.

The arithmetic is the textbook Edwards curve implementation (a = -1,
d = -121665/121666) over GF(2^255-19); it is checked against the RFC 8032
section 7.1 vectors by --self-test, which must pass before the module is
trusted to sign a production image.

Usage:
    python quilla_ed25519_sign.py keygen [--dir DIR]
    python quilla_ed25519_sign.py pubkey  --key private.hex
    python quilla_ed25519_sign.py --self-test

keygen writes:
    DIR/private.hex   32-byte seed, hex          (KEEP SECRET)
    DIR/public.hex    32-byte public key, hex     (bake into the QBOOT)
and prints the exact -DQUILLA_SECURE_BOOT_PUBKEY=... macro for the build.
"""

import argparse
import hashlib
import os
import sys

# ---- curve constants (RFC 8032 section 5.1) --------------------------------
P = 2 ** 255 - 19
L = 2 ** 252 + 27742317777372353535851937790883648493
D = (-121665 * pow(121666, P - 2, P)) % P

# Base point (Ed25519)
BY = (4 * pow(5, P - 2, P)) % P
BX = None  # derived below


def _recover_x(y, sign=0):
    """Recover x from y on -x^2 + y^2 = 1 + d*x^2*y^2."""
    y2 = pow(y, 2, P)
    num = (y2 - 1) % P
    den = (D * y2 + 1) % P
    x2 = (num * pow(den, P - 2, P)) % P

    x = pow(x2, (P + 3) // 8, P)
    if pow(x, 2, P) != x2:
        x = (x * pow(2, (P - 1) // 4, P)) % P
    if pow(x, 2, P) != x2:
        raise ValueError("point not on curve")
    if (x & 1) != sign:
        x = (-x) % P
    return x


BX = _recover_x(BY, 0)
B = (BX, BY)


# ---- group law -------------------------------------------------------------
def point_add(p1, p2):
    x1, y1 = p1
    x2, y2 = p2
    # Complete addition law for a = -1, no exceptional cases.
    k = (D * x1 * x2 * y1 * y2) % P
    x3 = ((x1 * y2 + x2 * y1) * pow(1 + k, P - 2, P)) % P
    y3 = ((y1 * y2 + x1 * x2) * pow(1 - k, P - 2, P)) % P
    return (x3, y3)


def point_double(p):
    return point_add(p, p)


def scalarmult(k, p):
    q = (0, 1)                       # identity
    while k > 0:
        if k & 1:
            q = point_add(q, p)
        p = point_double(p)
        k >>= 1
    return q


# ---- encodings -------------------------------------------------------------
def encode_point(p):
    x, y = p
    out = bytearray(y.to_bytes(32, "little"))
    if x & 1:
        out[31] |= 0x80
    return bytes(out)


def sha512(b):
    return hashlib.sha512(b).digest()


# ---- key handling ----------------------------------------------------------
def pubkey_from_seed(seed):
    if len(seed) != 32:
        raise ValueError("seed must be 32 bytes")
    h = sha512(seed)
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return encode_point(scalarmult(a, B))


def sign(seed, msg):
    """RFC 8032 section 5.1.6. Returns the 64-byte signature."""
    if len(seed) != 32:
        raise ValueError("seed must be 32 bytes")
    h = sha512(seed)
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    prefix = h[32:]

    A = encode_point(scalarmult(a, B))
    r = int.from_bytes(sha512(prefix + msg), "little") % L
    R = encode_point(scalarmult(r, B))
    k = int.from_bytes(sha512(R + A + msg), "little") % L
    S = (r + k * a) % L
    return R + S.to_bytes(32, "little")


def verify(sig, msg, pk):
    """RFC 8032 section 5.1.7 (cofactored check omitted: basic equation)."""
    if len(sig) != 64 or len(pk) != 32:
        return False
    S = int.from_bytes(sig[32:], "little")
    if S >= L:
        return False
    try:
        A = _decode_point(pk)
        R = _decode_point(sig[:32])
    except ValueError:
        return False
    k = int.from_bytes(sha512(sig[:32] + pk + msg), "little") % L
    lhs = scalarmult(S, B)
    rhs = point_add(scalarmult(k, A), R)
    return lhs == rhs


def _decode_point(s):
    if len(s) != 32:
        raise ValueError("bad point length")
    y = int.from_bytes(s, "little") & ((1 << 255) - 1)
    x = _recover_x(y, (s[31] >> 7) & 1)
    return (x, y)


# ---- signing the firmware descriptor --------------------------------------
def sign_descriptor(seed, image_len, image_sha256):
    """
    Sign the descriptor the QBOOT checks:

        image_len (4B LE) || SHA-256(image) (32B)

    The version is NOT part of the signed object: it is owned by QBOOT (an
    internal anti-rollback counter assigned at install), never supplied by the
    host and never taken from the firmware image, so a malicious build cannot
    forge a lower one. Returns the 64-byte signature.
    """
    descriptor = (image_len.to_bytes(4, "little") +
                  image_sha256)
    return sign(seed, descriptor)


# ---- self test against RFC 8032 section 7.1 -------------------------------
def self_test():
    vectors = [
        # (secret, public, message, signature)
        ("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
         "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
         "",
         "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555"
         "fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"),
        ("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
         "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
         "72",
         "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da0"
         "85ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"),
        ("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
         "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
         "af82",
         "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac1"
         "8ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"),
    ]
    ok = True
    for i, (sk_hex, pk_hex, msg_hex, sig_hex) in enumerate(vectors, 1):
        sk = bytes.fromhex(sk_hex)
        want_pk = bytes.fromhex(pk_hex)
        want_sig = bytes.fromhex(sig_hex)
        msg = bytes.fromhex(msg_hex)

        got_pk = pubkey_from_seed(sk)
        if got_pk != want_pk:
            print("FAIL TEST %d: public key mismatch" % i)
            ok = False
            continue
        got_sig = sign(sk, msg)
        if got_sig != want_sig:
            print("FAIL TEST %d: signature mismatch" % i)
            ok = False
            continue
        if not verify(want_sig, msg, want_pk):
            print("FAIL TEST %d: verify() rejected a good signature" % i)
            ok = False
            continue
        print("PASS RFC 8032 TEST %d" % i)

    # Negative: tampered message must not verify
    sk = bytes.fromhex(vectors[1][0])
    pk = bytes.fromhex(vectors[1][1])
    bad = sign(sk, b"other")
    if verify(bad, b"different", pk):
        print("FAIL: tampered message accepted")
        ok = False
    else:
        print("PASS tampered message rejected")

    print("\nself-test: %s" % ("OK" if ok else "FAILED"))
    return 0 if ok else 1


# ---- CLI -------------------------------------------------------------------
def cmd_keygen(args):
    d = args.dir
    os.makedirs(d, exist_ok=True)
    seed = os.urandom(32)
    pk = pubkey_from_seed(seed)

    sk_path = os.path.join(d, "private.hex")
    pk_path = os.path.join(d, "public.hex")
    with open(sk_path, "w", encoding="ascii") as f:
        f.write(seed.hex() + "\n")
    with open(pk_path, "w", encoding="ascii") as f:
        f.write(pk.hex() + "\n")

    print("wrote %s   (KEEP SECRET - 32-byte seed)" % sk_path)
    print("wrote %s" % pk_path)
    print()
    print("Bake the public key into the QBOOT build:")
    print("  -DQUILLA_SECURE_BOOT_PUBKEY={%s}"
          % ",".join("0x%02x" % b for b in pk))
    return 0


def cmd_pubkey(args):
    with open(args.key, "r", encoding="ascii") as f:
        seed = bytes.fromhex(f.read().strip())
    pk = pubkey_from_seed(seed)
    print(pk.hex())
    print()
    print("  -DQUILLA_SECURE_BOOT_PUBKEY={%s}"
          % ",".join("0x%02x" % b for b in pk))
    return 0


def main():
    ap = argparse.ArgumentParser(description="Quilla Ed25519 signing key tool")
    sub = ap.add_subparsers(dest="cmd")

    g = sub.add_parser("keygen", help="generate a keypair")
    g.add_argument("--dir", default="keys", help="output directory")
    g.set_defaults(func=cmd_keygen)

    p = sub.add_parser("pubkey", help="print the public key for a private key")
    p.add_argument("--key", required=True, help="private.hex path")
    p.set_defaults(func=cmd_pubkey)

    ap.add_argument("--self-test", action="store_true",
                    help="check against RFC 8032 vectors and exit")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if hasattr(args, "func"):
        return args.func(args)
    ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
