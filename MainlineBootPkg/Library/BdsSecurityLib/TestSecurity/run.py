#!/usr/bin/env python3
"""Test SecureBoot-selected local verification without invoking firmware policy."""
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
EDK = ROOT / "edk2"

# Do not let the removed vendor ABI, callbacks, switch, or protocol publication
# return unnoticed. Runtime fixtures independently reject every unexpected read,
# write and protocol lookup and supply hostile original authentication methods.
production = "\n".join(path.read_text() for path in (
    HERE.parent / "BdsSecurity.c", HERE.parent / "BdsSecurityLib.inf",
    ROOT / "MainlineBootPkg/Include/Library/BdsSecurityLib.h",
))
for forbidden in ("QCOM_PLATFORM_BDS", "d874d61a", "SetPlatformSecurity",
                  "mFirmwareAuthenticate", "SecureBootEnable",
                  "BdsSecurityGetEnabled", "BdsSecuritySetEnabled",
                  "InstallProtocolInterface", "InstallMultipleProtocolInterfaces",
                  "ReinstallProtocolInterface"):
    assert forbidden not in production, f"Forbidden production dependency: {forbidden}"

cases = (
    "enabled", "disabled", "defaults", "disabled-defaults", "fastboot-disabled",
    "setup-one", "setup-missing", "disabled-setup-missing",
    "setup-invalid", "disabled-setup-invalid",
    "secureboot-missing", "secureboot-read-error", "secureboot-invalid-value",
    "secureboot-empty", "secureboot-oversized", "missing-security2",
    "missing-security", "null-security2", "null-security",
    "security2-locate-error", "security-locate-error", "null-original-callbacks",
    "missing-pk", "missing-kek", "missing-db", "malformed-db",
    "fastboot-runtime-attribute", "fastboot-invalid-value", "fastboot-read-error",
    "defaults-write-protected", "fastboot-auto-disable-write-protected",
    "fastboot-set-write-protected",
)
with tempfile.TemporaryDirectory(prefix="bds-security-") as temporary:
    binary = Path(temporary) / "security-tests"
    includes = (EDK / "MdePkg/Include", EDK / "MdePkg/Include/X64",
                EDK / "MdeModulePkg/Include", EDK / "SecurityPkg/Include",
                EDK / "CryptoPkg/Include", ROOT / "MainlineBootPkg/Include")
    subprocess.run([
        os.environ.get("CC", "cc"), "-O1", "-g", "-fshort-wchar", "-DMDEPKG_NDEBUG",
        "-D__FORTIFY_SOURCE=1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-unused-function",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        *["-I" + str(path) for path in includes], str(HERE / "SecurityTests.c"),
        "-o", str(binary),
    ], check=True)
    for case in cases:
        subprocess.run([str(binary), case], check=True)
print(f"PASS: {len(cases)} local security cases with ASan/UBSan")
