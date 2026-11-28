#!/usr/bin/env python3
"""Build only the challenge host tests, using unchanged edk2 crypto sources."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
EDK = ROOT / "edk2"
CRYPT = EDK / "CryptoPkg/Library/BaseCryptLib"
SOURCES = [
    HERE / "ChallengeTests.c",
    HERE.parent / "BdsChallenge.c",
    CRYPT / "Hash/CryptSha256.c",
    CRYPT / "Hash/CryptSha512.c",
    CRYPT / "Pk/CryptRsaBasic.c",
    CRYPT / "Pk/CryptRsaExt.c",
    CRYPT / "Pk/CryptX509.c",
    CRYPT / "Pk/CryptPkcs7VerifyCommon.c",
    CRYPT / "Pk/CryptPkcs7VerifyBase.c",
]
INCLUDES = [
    EDK / "MdePkg/Include",
    EDK / "MdePkg/Include/X64",
    EDK / "CryptoPkg/Include",
    ROOT / "MainlineBootPkg/Include",
    CRYPT,
]
with tempfile.TemporaryDirectory(prefix="build-", dir=HERE) as temporary:
    binary = Path(temporary) / "challenge-tests"
    command = [
        os.environ.get("CC", "cc"), "-O1", "-g", "-fshort-wchar",
        "-D__FORTIFY_SOURCE=1", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-sign-compare",
        "-Wno-deprecated-declarations", "-ffunction-sections", "-fdata-sections",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-include", str(HERE / "HostSupport.h"),
        *["-I" + str(path) for path in INCLUDES],
        *map(str, SOURCES), "-Wl,--gc-sections", "-lcrypto", "-o", str(binary),
    ]
    environment = dict(os.environ)
    environment["TMPDIR"] = temporary
    subprocess.run(command, check=True, env=environment)
    # This older edk2 certificate-list API leaks OpenSSL references internally;
    # retain bounds/UAF/UB checks without mistaking upstream leaks for this code.
    environment.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    subprocess.run([str(binary)], check=True, env=environment)
