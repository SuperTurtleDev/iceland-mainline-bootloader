#!/usr/bin/env python3
"""Run genuine Secure Boot image/variable tests against an already-built BdsApp.

Requires openssl, sbsign, sbverify, efitools, QEMU, sfdisk, dosfstools and mtools.
Use --efitools-dir for locally extracted tools (no system installation needed).
AAVMF.secboot supplies real authenticated variables; the test BDS installs counted,
always-rejecting Security/Security2 callbacks to isolate BdsApp's standard verifier.
Disabled mode keeps the real unenrolled SetupMode=1, SecureBoot=0 backend state.
This is not coverage of the physical device's TrustZone variable backend.
SPDX-License-Identifier: BSD-2-Clause-Patent
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import uuid

SECTORS = 131072
OWNER = "2f5aab30-f7d2-4b41-9a4a-7935f5361380"
SHA256_GUID = uuid.UUID("c1c41626-504c-4092-aca9-41f936934328").bytes_le
ANSI = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\)|[()][A-Z0-9])")
PROBES = {"images": "SecureBootProbe", "challenge": "ChallengeProbe", "runtime": "RuntimeProbe", "disabled": "DisabledProbe"}
COMMON_ASSERTIONS = (
    "BDSTEST DIRECT BDS: standard firmware boot manager replaced",
    "SBTEST MODE: SetupMode=1 SecureBoot=0",
    "SBTEST FIRMWARE BASELINE: Security=2 Security2=4 PlatformSecurity=0",
    "SBTEST ARMED: starting production BdsApp",
    "BdsApp: application BDS core starting",
    "SBTEST ISOLATION PASS: Security/Security2 firmware counters unchanged; QCOM platform security calls=0",
)
ENABLED_ASSERTIONS = (
    "SBTEST MODE: SetupMode=0 SecureBoot=1",
    "SBTEST ENROLLMENT PASS: real time-authenticated PK KEK db; exact data and attributes verified",
    "SBTEST ENABLED PRE: SecureBoot=1 SetupMode=0; firmware Security/Security2 reject trusted and unsigned images",
    "BdsApp: SecureBoot=1, local edk2 image verifier active",
)
DISABLED_ASSERTIONS = (
    "SBTEST DISABLED PRE: SecureBoot=0 SetupMode=1; no keys enrolled; firmware Security/Security2 reject images",
    "BdsApp: SecureBoot=0, Security/Security2 allow-all stubs active",
    "SBTEST DISABLED PASS: SecureBoot=0; unsigned file and wrong-signed buffer execute; firmware rejection counters unchanged",
)
RUNTIME_ASSERTIONS = (
    "SBTEST RUNTIME EBS: ExitBootServices succeeded",
    "SBTEST RUNTIME PRE: EnableSecureFastboot=0 attributes=NV|BS; no runtime access",
    "SBTEST RUNTIME PASS: OS phase cannot read or write boot-service-only EnableSecureFastboot",
)
CHALLENGE_ASSERTIONS = (
    "SBTEST CHALLENGE SCOPE: production library in separate probe; deterministic test RNG; real db/dbx",
    "SBTEST CHALLENGE SIGNATURES PASS: raw RSA and CMS accepted; wrong key, replay, malformed and RNG failure denied",
    "SBTEST CHALLENGE SWITCH PASS: independent default FALSE switch; toggling clears authorization; image verification remains active",
    "SBTEST CHALLENGE REVOCATION PASS: dbx X509 and TBS SHA256 deny raw RSA and CMS; stale grant cleared",
    "SBTEST CHALLENGE EMPTY PASS: missing db disables switch; both missing have no certificate authorization",
    "SBTEST CHALLENGE PASS: production authorization library with real authenticated variables",
)
ASSERTIONS = COMMON_ASSERTIONS + ENABLED_ASSERTIONS + (
    "SBTEST PROBE: signed default child parent is BdsApp; rejecting firmware callbacks never called",
    "SBTEST IMAGES PASS: trusted executes, unsigned and wrong key denied (file and buffer)",
    "SBTEST AUTH PASS: PK KEK db signed updates accepted; wrong signer and replay rejected",
    "SBTEST DBX PASS: authenticated SHA256 revocation denies previously trusted image",
    "SBTEST PASS: application verifier and real authenticated variable backend",
)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


class Fixture:
    def __init__(self, args, output):
        self.args = args
        self.output = output
        self.environment = dict(os.environ, LC_ALL="C", TZ="UTC", MTOOLS_SKIP_CHECK="1")
        self.artifacts = output / "artifacts"
        self.artifacts.mkdir()
        self.keys = output / "test-keys"
        self.keys.mkdir(mode=0o700)
        self.efi = output / "efi-test"
        self.efi.mkdir()

    def tool(self, name):
        if self.args.efitools_dir and name in ("cert-to-efi-sig-list", "sign-efi-sig-list", "hash-to-efi-sig-list"):
            return str(self.args.efitools_dir / name)
        return name

    def record(self, argv):
        with (self.output / "commands.txt").open("a", encoding="utf-8") as log:
            log.write(shlex.join(list(map(str, argv))) + "\n")

    def command(self, argv, stdin=None, expected=0):
        argv = list(map(str, argv))
        argv[0] = self.tool(argv[0])
        self.record(argv)
        result = subprocess.run(argv, input=stdin, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, env=self.environment, timeout=45)
        with (self.output / "fixture-tools.log").open("a", encoding="utf-8") as log:
            log.write(shlex.join(argv) + "\n" + result.stdout)
        if result.returncode != expected:
            raise RuntimeError(f"unexpected exit {result.returncode} (expected {expected}): {shlex.join(argv)}\n{result.stdout}")
        return result.stdout

    def make_keys(self):
        for name in ("PK", "KEK", "db", "wrong"):
            self.command(["openssl", "req", "-new", "-x509", "-newkey", "rsa:2048", "-sha256",
                          "-nodes", "-subj", f"/CN=BdsApp test {name}/", "-days", "3650",
                          "-keyout", self.keys / f"{name}.key", "-out", self.keys / f"{name}.pem"])
            (self.keys / f"{name}.key").chmod(0o600)
            self.command(["cert-to-efi-sig-list", "-g", OWNER,
                          self.keys / f"{name}.pem", self.efi / f"{name}.esl"])

    def sign_image(self, source, destination, key):
        self.command(["sbsign", "--key", self.keys / f"{key}.key", "--cert", self.keys / f"{key}.pem",
                      "--output", destination, source])
        self.command(["sbverify", "--cert", self.keys / f"{key}.pem", destination])

    def sign_variable(self, name, source, signer, destination, day):
        self.command(["sign-efi-sig-list", "-t", f"2025-01-{day:02d} 00:00:00",
                      "-k", self.keys / f"{signer}.key", "-c", self.keys / f"{signer}.pem",
                      name, self.efi / source, self.efi / destination])

    def challenge_fixtures(self):
        # Signing remains in OpenSSL; the fixture only packages a standard ESL.
        for generation in range(1, 12):
            nonce = self.output / f"nonce{generation}.bin"
            nonce.write_bytes(bytes(index + generation for index in range(32)))
            for key, label in (("db", "good"), ("wrong", "wrong")):
                self.command(["openssl", "dgst", "-sha256", "-sign", self.keys / f"{key}.key",
                              "-out", self.efi / f"nonce{generation}-raw-{label}.sig", nonce])
                self.command(["openssl", "cms", "-sign", "-binary", "-md", "sha256", "-nosmimecap",
                              "-in", nonce, "-signer", self.keys / f"{key}.pem", "-inkey", self.keys / f"{key}.key",
                              "-outform", "DER", "-out", self.efi / f"nonce{generation}-cms-{label}.sig"])
        der_path = self.output / "db.der"
        self.command(["openssl", "x509", "-in", self.keys / "db.pem", "-outform", "DER", "-out", der_path])
        der = der_path.read_bytes()

        def sequence_size(offset):
            # Minimal, bounded DER length extraction to select TBSCertificate.
            # Cryptographic verification and certificate parsing stay in OpenSSL.
            if offset + 2 > len(der) or der[offset] != 0x30:
                raise RuntimeError("invalid generated X509 DER sequence")
            size = der[offset + 1]
            header = 2
            if size & 0x80:
                count = size & 0x7f
                if count not in (1, 2, 3, 4) or offset + 2 + count > len(der):
                    raise RuntimeError("invalid generated X509 DER length")
                size = int.from_bytes(der[offset + 2:offset + 2 + count], "big")
                header += count
            if offset + header + size > len(der):
                raise RuntimeError("truncated generated X509 DER")
            return header, size

        outer_header, _ = sequence_size(0)
        tbs_header, tbs_size = sequence_size(outer_header)
        tbs = der[outer_header:outer_header + tbs_header + tbs_size]
        tbs_hash = hashlib.sha256(tbs).digest()
        x509_sha256 = uuid.UUID("3bd2a492-96c0-4079-b420-fcf98ef103ed").bytes_le
        # EFI_CERT_X509_SHA256 = TBS hash + EFI_TIME (zero => always revoked).
        entry = uuid.UUID(OWNER).bytes_le + tbs_hash + bytes(16)
        (self.efi / "dbx-tbs.esl").write_bytes(x509_sha256 + struct.pack("<III", 28 + len(entry), 0, len(entry)) + entry)
        (self.efi / "empty.esl").write_bytes(b"")
        self.sign_variable("dbx", "db.esl", "KEK", "dbx-cert.auth", 4)
        self.sign_variable("dbx", "dbx-tbs.esl", "KEK", "dbx-tbs.auth", 5)
        self.sign_variable("db", "empty.esl", "KEK", "db-delete.auth", 6)
        self.sign_variable("dbx", "empty.esl", "KEK", "dbx-delete.auth", 6)

    def prepare(self):
        probe = PROBES[self.args.mode] + ".efi"
        for source, name in ((self.args.bds, "BdsApp.efi"),
                             *((self.args.test_bin / name, name) for name in
                               ("SecureBootDirect.efi", probe, "SecureBootReturn.efi"))):
            shutil.copyfile(source, self.artifacts / name)
        shutil.copyfile(self.artifacts / "BdsApp.efi", self.efi / "BdsApp.efi")
        shutil.copyfile(self.artifacts / "SecureBootReturn.efi", self.efi / "unsigned.efi")
        self.make_keys()
        if self.args.mode == "disabled":
            shutil.copyfile(self.artifacts / probe, self.output / "BOOTAA64.EFI")
            (self.efi / "disabled.flag").write_text("disabled\n")
        else:
            self.sign_image(self.artifacts / probe, self.output / "BOOTAA64.EFI", "db")
        self.sign_image(self.artifacts / "SecureBootReturn.efi", self.efi / "good.efi", "db")
        self.sign_image(self.artifacts / "SecureBootReturn.efi", self.efi / "wrong.efi", "wrong")
        self.command(["sbverify", "--cert", self.keys / "db.pem", self.efi / "wrong.efi"], expected=1)
        self.command(["sbverify", "--cert", self.keys / "db.pem", self.efi / "unsigned.efi"], expected=1)
        # efitools computes PE/COFF Authenticode SHA256; a whole-file SHA256 is
        # NOT suitable for dbx. Check signed and unsigned forms have one digest.
        self.command(["hash-to-efi-sig-list", self.efi / "good.efi", self.efi / "dbx.esl"])
        self.command(["hash-to-efi-sig-list", self.efi / "unsigned.efi", self.output / "unsigned-hash.esl"])
        signed_hash = (self.efi / "dbx.esl").read_bytes()
        unsigned_hash = (self.output / "unsigned-hash.esl").read_bytes()
        if (len(signed_hash) != 76 or signed_hash[:16] != SHA256_GUID or
                struct.unpack_from("<III", signed_hash, 16) != (76, 0, 48) or
                signed_hash[44:] != unsigned_hash[44:]):
            raise RuntimeError("expected one identical Authenticode SHA256 ESL for signed/unsigned payload")
        self.sign_variable("PK", "PK.esl", "PK", "PK.auth", 1)
        self.sign_variable("KEK", "KEK.esl", "PK", "KEK.auth", 1)
        self.sign_variable("db", "db.esl", "KEK", "db.auth", 1)
        self.sign_variable("PK", "PK.esl", "PK", "PK-update.auth", 2)
        self.sign_variable("KEK", "KEK.esl", "PK", "KEK-update.auth", 2)
        self.sign_variable("db", "db.esl", "KEK", "db-update.auth", 2)
        # The wrong update is newer and would change db, so rejecting it cannot
        # be explained by a timestamp replay or a no-op payload.
        self.sign_variable("db", "wrong.esl", "wrong", "db-wrong.auth", 3)
        self.sign_variable("dbx", "dbx.esl", "KEK", "dbx.auth", 2)
        self.sign_variable("dbx", "wrong.esl", "wrong", "dbx-wrong.auth", 3)
        if self.args.mode == "challenge":
            self.challenge_fixtures()
        firmware = self.output / "AAVMF-direct-secboot.fd"
        patcher = Path(__file__).resolve().parent.parent / "BdsApp/make-direct-firmware.py"
        self.command([sys.executable, patcher, self.args.firmware_code,
                      self.artifacts / "SecureBootDirect.efi", firmware])
        vars_file = self.output / "VARS.fd"
        shutil.copyfile(self.args.firmware_vars, vars_file)
        disk = self.output / "fixture.gpt"
        with disk.open("wb") as image:
            image.truncate((2048 + SECTORS + 2048) * 512)
        table = ("label: gpt\nlabel-id: 22222222-3333-4444-8555-666666666666\nunit: sectors\n"
                 f"start=2048, size={SECTORS}, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, "
                 'uuid=11111111-2222-4333-8444-555555555551, name="SecureBoot fixture"\n')
        self.command(["sfdisk", "--no-reread", "--no-tell-kernel", disk], table)
        partition = self.output / "partition.fat"
        with partition.open("wb") as image:
            image.truncate(SECTORS * 512)
        self.command(["mkfs.vfat", "--invariant", "-F", "32", "-n", "SBTEST", partition])
        with disk.open("r+b") as image, partition.open("rb") as source:
            image.seek(2048 * 512)
            shutil.copyfileobj(source, image)
        partition.unlink()
        target = f"{disk}@@{2048 * 512}"
        self.command(["mmd", "-i", target, "::/EFI", "::/EFI/BOOT", "::/EFI/TEST"])
        self.command(["mcopy", "-i", target, self.output / "BOOTAA64.EFI", "::/EFI/BOOT/BOOTAA64.EFI"])
        for source in sorted(self.efi.iterdir()):
            self.command(["mcopy", "-i", target, source, f"::/EFI/TEST/{source.name}"])
        (self.output / "gpt.json").write_text(self.command(["sfdisk", "--json", disk]), encoding="utf-8")
        return firmware, vars_file, disk

    def boot(self, firmware, vars_file, disk):
        argv = [self.args.qemu, "-machine", "virt", "-accel", "tcg", "-cpu", "cortex-a57",
                "-m", "1024", "-smp", "1", "-nodefaults", "-display", "none", "-monitor", "none",
                "-serial", "stdio", "-nic", "none", "-no-reboot",
                "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={firmware}",
                "-drive", f"if=pflash,format=raw,unit=1,file={vars_file}",
                "-drive", f"if=none,id=fixture,format=raw,file={disk}",
                "-device", "virtio-blk-pci,drive=fixture"]
        self.record(argv)
        started = time.monotonic()
        error = None
        with (self.output / "serial.log").open("wb") as log:
            try:
                result = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=log,
                                        stderr=subprocess.STDOUT, env=self.environment, timeout=self.args.timeout)
                if result.returncode:
                    error = f"QEMU exited {result.returncode}"
            except subprocess.TimeoutExpired:
                error = f"QEMU exceeded {self.args.timeout}s wall-clock limit"
        text = ANSI.sub("", (self.output / "serial.log").read_text(encoding="utf-8", errors="replace")).replace("\r", "")
        (self.output / "serial.clean.log").write_text(text, encoding="utf-8")
        assertions = {
            "images": ASSERTIONS,
            "challenge": COMMON_ASSERTIONS + ENABLED_ASSERTIONS + CHALLENGE_ASSERTIONS,
            "runtime": COMMON_ASSERTIONS + ENABLED_ASSERTIONS + RUNTIME_ASSERTIONS,
            "disabled": COMMON_ASSERTIONS + DISABLED_ASSERTIONS,
        }[self.args.mode]
        missing = [item for item in assertions if item not in text]
        if missing:
            error = (error + "; " if error else "") + f"{len(missing)} required assertions missing"
        guest_failure = next((line for line in text.splitlines() if "SBTEST FAIL:" in line or "BDSTEST FAIL:" in line), None)
        if guest_failure:
            error = (error + "; " if error else "") + guest_failure
        if text.count("BdsApp: application BDS core starting") != 1:
            error = (error + "; " if error else "") + "expected exactly one production BdsApp startup"
        if self.args.mode == "disabled" and ("SBTEST MODE: SetupMode=0 SecureBoot=1" in text or "SBTEST SET:" in text):
            error = (error + "; " if error else "") + "disabled fixture must remain unenrolled in actual setup mode"
        return {"status": "FAIL" if error else "PASS", "error": error,
                "seconds": round(time.monotonic() - started, 2), "qemu_argv": argv,
                "assertions": list(assertions), "missing_assertions": missing}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bds", required=True, type=Path, help="already-built production BdsApp.efi")
    parser.add_argument("--test-bin", required=True, type=Path, help="directory containing SecureBootDirect/Probe/Return.efi")
    parser.add_argument("--mode", choices=tuple(PROBES), default="images", help="challenge uses its own production library state (-D SB_CHALLENGE_TEST); runtime checks post-EBS EnableSecureFastboot; disabled uses real unenrolled setup mode")
    parser.add_argument("--efitools-dir", type=Path, help="directory containing extracted efitools executables")
    parser.add_argument("--output", type=Path, help="new artifact directory outside repository; default /tmp/secureboot-qemu-*")
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    parser.add_argument("--firmware-code", type=Path, default=Path("/usr/share/AAVMF/AAVMF_CODE.secboot.fd"))
    parser.add_argument("--firmware-vars", type=Path, default=Path("/usr/share/AAVMF/AAVMF_VARS.fd"), help="pristine unenrolled template, copied to private writable VARS")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if args.efitools_dir:
        args.efitools_dir = args.efitools_dir.resolve()
    for tool in (args.qemu, "openssl", "sbsign", "sbverify", "sfdisk", "mkfs.vfat", "mmd", "mcopy",
                 "cert-to-efi-sig-list", "sign-efi-sig-list", "hash-to-efi-sig-list"):
        name = str(args.efitools_dir / tool) if args.efitools_dir and tool.endswith("efi-sig-list") else tool
        if not shutil.which(name):
            parser.error(f"required tool missing: {name}; use --efitools-dir for local efitools")
    probe = PROBES[args.mode]
    inputs = (args.bds, args.firmware_code, args.firmware_vars,
              *(args.test_bin / f"{name}.efi" for name in ("SecureBootDirect", probe, "SecureBootReturn")))
    for path in inputs:
        if not path.is_file():
            parser.error(f"required input missing: {path}")
    repo = Path(__file__).resolve().parents[3]
    if args.output:
        output = args.output.resolve()
        if output == repo or repo in output.parents:
            parser.error("--output must be outside repository")
        output.mkdir(parents=True, exist_ok=False)
    else:
        output = Path(tempfile.mkdtemp(prefix="secureboot-qemu-"))
    fixture = Fixture(args, output)
    source_dir = Path(__file__).resolve().parent
    source_files = [path for path in source_dir.iterdir() if path.is_file()]
    source_files += [source_dir.parent / "BdsApp" / name for name in
                     ("DirectBoot.c", "TestCommon.h", "make-direct-firmware.py")]
    report = {"status": "FAIL", "mode": args.mode, "output": str(output), "invocation": shlex.join([sys.executable, *sys.argv]),
              "challenge_scope": "production library in separate probe; deterministic RNG; not fastboot transport" if args.mode == "challenge" else None,
              "image_verifier": ("production BdsApp allow-all stubs with actual SecureBoot=0"
                                 if args.mode == "disabled" else "production BdsApp local standard edk2 handler with actual SecureBoot=1"),
              "firmware_policy": "always-rejecting Security/Security2 fixtures; no calls after BdsApp starts; QCOM security initialization trapped",
              "secure_boot_state": {"SetupMode": int(args.mode == "disabled"), "SecureBoot": int(args.mode != "disabled")},
              "variable_backend": "real AAVMF authenticated variables, not physical TrustZone secapp",
              "inputs_sha256": {str(path.resolve()): digest(path) for path in inputs},
              "sources_sha256": {str(path): digest(path) for path in sorted(source_files)}}
    print(f"Results: {output}", flush=True)
    try:
        firmware, vars_file, disk = fixture.prepare()
        report["fixture_sha256"] = {str(path.relative_to(output)): digest(path) for path in
                                    (*fixture.artifacts.iterdir(), *fixture.efi.iterdir(), output / "BOOTAA64.EFI", firmware, vars_file, disk)}
        report.update(fixture.boot(firmware, vars_file, disk))
        report["final_vars_sha256"] = digest(vars_file)
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        report["error"] = str(exc)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"{report['status']}: {report.get('error') or args.mode + ' assertions passed'}", flush=True)
    print(f"Report: {output / 'report.json'}", flush=True)
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
