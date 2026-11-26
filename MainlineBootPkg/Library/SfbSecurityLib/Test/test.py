#!/usr/bin/env python3
"""Build host tests; create real sbsign vectors in an isolated temporary directory.
Copyright (c) 2026, MainlineBootPkg contributors.
SPDX-License-Identifier: BSD-3-Clause
"""
from pathlib import Path
import hashlib
import os
import shlex
import shutil
import struct
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
LIB = HERE.parent
PACKAGE = LIB.parent.parent
MBED = LIB.parent / "MbedTlsLib"


def run(command):
    subprocess.run([str(arg) for arg in command], check=True)


def image():
    # PE32+ with two section headers deliberately reversed relative to raw data.
    # The independent digest below follows the Microsoft Authenticode byte ranges.
    data = bytearray(1536)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", data, 0x84, 0xAA64, 2, 0, 0, 0, 240, 0x2022)
    opt = 0x98
    struct.pack_into("<H", data, opt, 0x20B)
    struct.pack_into("<II", data, opt + 16, 0x1000, 0x1000)
    struct.pack_into("<Q", data, opt + 24, 0x400000)
    struct.pack_into("<II", data, opt + 32, 0x1000, 512)
    struct.pack_into("<II", data, opt + 56, 0x3000, 512)
    struct.pack_into("<H", data, opt + 68, 10)
    struct.pack_into("<I", data, opt + 108, 16)
    section = opt + 240
    for index, (name, rva, offset) in enumerate([(b".data", 0x2000, 1024), (b".text", 0x1000, 512)]):
        start = section + index * 40
        data[start:start + len(name)] = name
        struct.pack_into("<IIII", data, start + 8, 512, rva, 512, offset)
        struct.pack_into("<I", data, start + 36, 0x60000020)
    data[512:] = bytes((index * 17 + 3) & 255 for index in range(1024))
    digest = hashlib.sha256(data[:opt + 64] + data[opt + 68:opt + 144] + data[opt + 152:512]
                            + data[512:1024] + data[1024:1536]).digest()
    return data, digest


with tempfile.TemporaryDirectory(prefix="sfb-security-test-") as directory:
    work = Path(directory)
    includes = work / "include"
    for name in ["Uefi.h", "Library/BaseLib.h", "Library/BaseMemoryLib.h", "Library/DebugLib.h",
                 "Library/UefiBootServicesTableLib.h", "Protocol/Rng.h", "Library/UefiLib.h",
                 "Library/MemoryAllocationLib.h", "Guid/FileInfo.h", "Protocol/BlockIo.h",
                 "Protocol/SimpleFileSystem.h"]:
        target = includes / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text('#include "Host.h"\n')
    # Use the exact firmware crypto configuration, with host C library headers.
    (includes / "mbedtls").mkdir()
    shutil.copy2(MBED / "Include/mbedtls/mbedtls_config.h", includes / "mbedtls/mbedtls_config.h")
    source_section = (MBED / "MbedTlsLib.inf").read_text().split("[Sources]", 1)[1].split("[Packages]", 1)[0]
    sources = [MBED / line.strip() for line in source_section.splitlines()
               if line.strip().startswith("mbedtls/library/") and line.strip().endswith(".c")]
    cc = shlex.split(os.environ.get("CC", "cc"))
    flags = shlex.split(os.environ.get("CFLAGS", "-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"))
    run(cc + flags + ["-std=c11", "-fshort-wchar", "-Wall", "-Wextra", "-Wno-unused-parameter",
                     "-I" + str(includes), "-I" + str(HERE), "-I" + str(PACKAGE / "Include"),
                     "-I" + str(MBED / "mbedtls/include"), "-I" + str(MBED / "mbedtls/library"),
                     HERE / "SecurityTest.c", LIB / "SfbSecurity.c", LIB / "SfbAuthenticode.c",
                     *sources, "-o", work / "SecurityTest"])
    for name, key_options in [("rsa", ["-newkey", "rsa:2048"]),
                              ("ecdsa", ["-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256"]),
                              ("wrong", ["-newkey", "rsa:2048"])]:
        run(["openssl", "req", "-x509", *key_options, "-keyout", work / (name + ".key"),
             "-out", work / (name + ".pem"), "-nodes", "-subj", "/CN=SfbTest-" + name,
             "-days", "1", "-sha256"])
        run(["openssl", "x509", "-in", work / (name + ".pem"), "-outform", "DER",
             "-out", work / (name + ".der")])
    data, digest = image()
    (work / "unsigned.efi").write_bytes(data)
    (work / "digest.bin").write_bytes(digest)
    (work / "nonce.bin").write_bytes(bytes([1]) * 32)
    for name in ["rsa", "ecdsa"]:
        print("Testing " + name.upper() + " signatures", flush=True)
        run(["sbsign", "--key", work / (name + ".key"), "--cert", work / (name + ".pem"),
             "--output", work / "signed.efi", work / "unsigned.efi"])
        run(["sbverify", "--cert", work / (name + ".pem"), work / "signed.efi"])
        run(["openssl", "dgst", "-sha256", "-sign", work / (name + ".key"), "-out", work / "response.bin",
             work / "nonce.bin"])
        run([work / "SecurityTest", work / "signed.efi", work / (name + ".der"), work / "wrong.der",
             work / "digest.bin", work / "response.bin"])
