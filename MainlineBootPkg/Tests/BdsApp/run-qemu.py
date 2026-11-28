#!/usr/bin/env python3
"""Bounded real-firmware BdsApp tests; all artifacts are outside the repository.
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
import subprocess
import sys
import tempfile
import time

import pexpect

CASES = (
    "discovery", "bootnext", "order-fallback", "skip-invalid-inactive",
    "bootnext-missing", "esp-scope", "osindications-menu", "nv-discovery", "nv-bootnext",
    "return-menu", "failure-menu",
)
ESP = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
DATA = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
PART_GUIDS = [f"11111111-2222-4333-8444-55555555555{i}" for i in (1, 2, 3)]
SECTORS = 131072  # Each FAT32 partition is 64 MiB.
ANSI = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\)|[()][A-Z0-9])")
FOOTER = "Vol Up/Down: move   Power: select"


def clean(text):
    return ANSI.sub("", text).replace("\r", "")


def digest(path):
    with open(path, "rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


class Runner:
    def __init__(self, args, output):
        self.args = args
        self.output = output
        self.commands = open(output / "commands.txt", "w", encoding="utf-8")
        self.environment = dict(os.environ, LC_ALL="C", TZ="UTC", MTOOLS_SKIP_CHECK="1")

    def record(self, argv, stdin=None):
        self.commands.write(shlex.join(map(str, argv)) + "\n")
        if stdin:
            self.commands.write("# stdin:\n" + "".join("# " + line + "\n" for line in stdin.splitlines()))
        self.commands.flush()

    def command(self, argv, stdin=None):
        argv = list(map(str, argv))
        self.record(argv, stdin)
        result = subprocess.run(argv, input=stdin, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, env=self.environment, timeout=30)
        with open(self.output / "fixture-tools.log", "a", encoding="utf-8") as log:
            log.write(shlex.join(argv) + "\n" + result.stdout)
        if result.returncode:
            raise RuntimeError(f"fixture command failed ({result.returncode}): {shlex.join(argv)}\n{result.stdout}")
        return result.stdout

    def fixture(self, case, directory):
        count = 3 if case == "esp-scope" else 1
        disk = directory / "fixture.gpt"
        with open(disk, "wb") as stream:
            stream.truncate((2048 + count * SECTORS + 2048) * 512)
        table = "label: gpt\nlabel-id: 22222222-3333-4444-8555-666666666666\nunit: sectors\n"
        for index in range(count):
            kind = DATA if index == 2 else ESP
            table += (f"start={2048 + index * SECTORS}, size={SECTORS}, type={kind}, "
                      f"uuid={PART_GUIDS[index]}, name=\"BdsApp fixture {index + 1}\"\n")
        self.command(["sfdisk", "--no-reread", "--no-tell-kernel", disk], table)
        case_file = directory / "case.txt"
        case_file.write_text(case, encoding="ascii")
        for index in range(count):
            partition = directory / f"partition-{index + 1}.fat"
            with open(partition, "wb") as stream:
                stream.truncate(SECTORS * 512)
            self.command(["mkfs.vfat", "--invariant", "-F", "32", "-n", f"BDSFIX{index + 1}", partition])
            offset = (2048 + index * SECTORS) * 512
            with open(disk, "r+b") as image, open(partition, "rb") as source:
                image.seek(offset)
                shutil.copyfileobj(source, image)
            partition.unlink()
            target = f"{disk}@@{offset}"
            self.command(["mmd", "-i", target, "::/EFI", "::/EFI/BOOT", "::/EFI/TEST"])
            payload = self.output / "artifacts/Payload.efi"
            if case == "failure-menu":
                payload = directory / "invalid.efi"
                payload.write_bytes(b"not a PE image\n")
            self.command(["mcopy", "-i", target, payload, "::/EFI/BOOT/BOOTAA64.EFI"])
            if index == 0:
                for source, destination in (
                    ("BdsApp.efi", "BdsApp.efi"), ("Payload.efi", "First.efi"), ("Payload.efi", "Second.efi")
                ):
                    self.command(["mcopy", "-i", target, self.output / "artifacts" / source, f"::/EFI/TEST/{destination}"])
                self.command(["mcopy", "-i", target, case_file, "::/EFI/TEST/case.txt"])
        partition_map = self.command(["sfdisk", "--json", disk])
        (directory / "gpt.json").write_text(partition_map, encoding="utf-8")
        vars_file = directory / "VARS.fd"
        shutil.copyfile(self.args.firmware_vars, vars_file)
        return disk, vars_file

    def qemu_command(self, disk, vars_file):
        return [self.args.qemu, "-machine", "virt", "-accel", "tcg", "-cpu", "cortex-a57",
                "-m", "1024", "-smp", "1", "-nodefaults", "-display", "none",
                "-monitor", "none", "-serial", "stdio", "-nic", "none", "-no-reboot",
                "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={self.args.firmware_code}",
                "-drive", f"if=pflash,format=raw,unit=1,file={vars_file}",
                "-drive", f"if=none,id=fixture,format=raw,file={disk}",
                "-device", "virtio-blk-pci,drive=fixture"]

    def boot(self, case, phase, directory, disk, vars_file):
        argv = self.qemu_command(disk, vars_file)
        self.record(argv)
        log_path = directory / f"phase-{phase}.serial.log"
        actions = []
        started = time.monotonic()
        deadline = started + self.args.timeout
        child = None
        error = None
        try:
            with open(log_path, "w", encoding="utf-8") as log:
                child = pexpect.spawn(argv[0], argv[1:], encoding="utf-8", codec_errors="replace",
                                      env=self.environment, echo=False, dimensions=(40, 120))
                child.logfile_read = log
                child.delaybeforesend = 0.1

                def expect(pattern):
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError("per-boot wall-clock deadline exceeded")
                    child.expect(pattern, timeout=remaining)
                    return clean(child.before + (child.after if isinstance(child.after, str) else ""))

                def screen(key=None, required=None):
                    if key is not None:
                        actions.append({"key": {"\x1b[B": "Down", "\x1b[A": "Up", "\r": "Enter"}[key],
                                        "expect": required or FOOTER})
                        child.send(key)
                    text = expect(re.escape(FOOTER))
                    if required and required not in text:
                        raise AssertionError(f"screen missing {required!r}: {text!r}")
                    return text

                if case == "osindications-menu":
                    # OsIndications bit zero requests BIOS Setup directly.
                    # A complete footer proves debounce ended before key input.
                    text = screen(required="BIOS Setup")
                    if "UEFI Boot Menu" in text:
                        raise AssertionError("OsIndications did not enter BIOS Setup directly")
                    if "Boot Entries >" not in text or "Boot Order >" not in text:
                        raise AssertionError("BIOS Setup lacks boot variable menus")
                    for _ in range(3):
                        screen("\x1b[B")
                    screen("\r", "First entry is tried first.")
                    screen("\r", "Remove from Boot Order (keep entry)")
                    screen("\x1b[A", "Back")
                    screen("\r", "First entry is tried first.")
                    screen("\x1b[A", "Back")
                    screen("\r", "BIOS Setup")
                    for _ in range(4):
                        screen("\x1b[B")
                    screen("\r", "All valid Boot#### entries, including entries outside BootOrder.")
                    screen("\x1b[A", "Back")
                    screen("\r", "BIOS Setup")
                    screen("\x1b[A", "Back")
                    screen("\r", "UEFI Boot Menu")
                    # Root starts on the discovered option. Enter Setup once
                    # from this menu as well, then Continue Boot from the root.
                    screen("\x1b[B", "BIOS Setup >")
                    screen("\r", "BIOS Setup")
                    screen("\x1b[A", "Back")
                    screen("\r", "UEFI Boot Menu")
                    screen("\x1b[B", "Continue Boot")
                    actions.append({"key": "Enter", "expect": f"BDSTEST PASS: {case} phase={phase}"})
                    child.send("\r")
                if case in ("return-menu", "failure-menu"):
                    if case == "return-menu":
                        expect("BDSTEST RETURN: child returns EFI_SUCCESS to BdsApp")
                    else:
                        expect(r"BdsApp: Boot[0-9A-Fa-f]{4} returned Not Found")
                    screen(required="UEFI Boot Menu")
                    # One option, Setup, Continue, Power Off. Remaining in this
                    # screen proves BdsApp did not return to the ABL analogue.
                    for _ in range(3):
                        screen("\x1b[B")
                    actions.append({"key": "Enter", "expect": "QEMU shutdown from Boot Menu"})
                    child.send("\r")
                else:
                    expect(re.escape(f"BDSTEST PASS: {case} phase={phase}"))
                expect(pexpect.EOF)
                child.close()
                if child.exitstatus != 0:
                    raise AssertionError(f"QEMU exit={child.exitstatus}, signal={child.signalstatus}")
        except (pexpect.TIMEOUT, pexpect.EOF, TimeoutError, AssertionError, OSError) as exc:
            error = str(exc)
        finally:
            if child is not None and child.isalive():
                child.terminate(force=True)
            if child is not None:
                child.close(force=True)
        text = clean(log_path.read_text(encoding="utf-8")) if log_path.exists() else ""
        (directory / f"phase-{phase}.clean.log").write_text(text, encoding="utf-8")
        (directory / f"phase-{phase}.keys.json").write_text(json.dumps(actions, indent=2) + "\n", encoding="utf-8")
        assertions = [
            "BDSTEST DIRECT BDS: standard firmware boot manager replaced",
            f"BDSTEST ARMED: {case} phase={phase}",
            "BdsApp: application BDS core starting",
            "BdsApp: self identity: path=yes, file=yes",
            f"BdsApp: discovery found {2 if case == 'esp-scope' else 1} GPT ESP(s), {2 if case == 'esp-scope' else 1} eligible default loader(s): Success",
            "BDSTEST ASSERT: BdsApp-parent BootCurrent=",
            "BDSTEST VARIABLES PASS: types, attributes, ambiguity, bounds, enumeration",
            f"BDSTEST PASS: {case} phase={phase}",
        ]
        if case in ("return-menu", "failure-menu"):
            assertions = assertions[:5] + ["UEFI Boot Menu", "BdsApp: entering platform menu after", "Powering off"]
            if case == "return-menu":
                assertions += ["BDSTEST RETURN: child returns EFI_SUCCESS to BdsApp", "BDSTEST VARIABLES PASS"]
        if case in ("bootnext", "nv-bootnext") and phase == 1:
            assertions += ["BdsApp: BootNext=A902 captured and deleted", "BdsApp: boot BootA902"]
        if case == "bootnext-missing":
            assertions += ["BdsApp: BootNext=A901 captured and deleted", "BdsApp: BootNext BootA901 unavailable:"]
        if case == "order-fallback":
            assertions += ["BdsApp: boot BootA901", "BdsApp: BootA901 returned Not Found", "BdsApp: boot BootA902"]
        if case == "skip-invalid-inactive":
            assertions += ["BdsApp: skipping BootA901: Not Found", "BdsApp: boot BootA904"]
        if case == "esp-scope":
            assertions += ["BDSTEST fixture: second ESP and non-ESP FAT both visible"]
        if case == "osindications-menu":
            assertions += ["UEFI Boot Menu", "BIOS Setup >", "Boot Order", "Boot Entries", "BDSTEST OsIndications: 0x9 -> 0x8"]
        if phase == 2:
            assertions += ["BDSTEST NV VERIFIED: BootOrder/Boot#### unchanged; BootNext absent", "ESP loader already registered"]
        missing = [assertion for assertion in assertions if assertion.lower() not in text.lower()]
        if missing:
            error = (error + "\n" if error else "") + "Missing assertions: " + repr(missing)
        if "BDSTEST FAIL:" in text:
            error = (error + "\n" if error else "") + "Payload/launcher reported FAIL"
        if text.count("BdsApp: application BDS core starting") != 1:
            error = (error + "\n" if error else "") + "Expected exactly one BdsApp startup"
        if case in ("bootnext", "nv-bootnext") and phase == 1 and "BdsApp: boot BootA901".lower() in text.lower():
            error = (error + "\n" if error else "") + "BootOrder ran before BootNext"
        if case == "skip-invalid-inactive" and re.search(r"BdsApp: boot BootA90[123]", text, re.I):
            error = (error + "\n" if error else "") + "Invalid/inactive/APP option executed"
        if case == "order-fallback" and not missing:
            if text.lower().index("bdsapp: boot boota901") > text.lower().index("bdsapp: boot boota902"):
                error = "BootOrder executed out of order"
        return {"phase": phase, "status": "FAIL" if error else "PASS", "error": error,
                "seconds": round(time.monotonic() - started, 2), "assertions": assertions,
                "serial_log": str(log_path), "qemu_argv": argv, "keys": actions}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bds", type=Path, required=True, help="already-built production BdsApp.efi (copied once)")
    parser.add_argument("--test-bin", type=Path, required=True, help="directory containing DirectBoot.efi and Payload.efi")
    parser.add_argument("--output", type=Path, help="new output directory outside the repository; default /tmp/bdsapp-qemu-*")
    parser.add_argument("--cases", nargs="+", choices=CASES, default=list(CASES))
    parser.add_argument("--timeout", type=float, default=90, help="wall-clock seconds per QEMU boot, including menus")
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    parser.add_argument("--firmware-code", type=Path, default=Path("/usr/share/AAVMF/AAVMF_CODE.secboot.fd"))
    parser.add_argument("--firmware-vars", type=Path, default=Path("/usr/share/AAVMF/AAVMF_VARS.fd"))
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    for tool in (args.qemu, "sfdisk", "mkfs.vfat", "mmd", "mcopy"):
        if not shutil.which(tool):
            parser.error(f"required tool missing: {tool}")
    for path in (args.bds, args.test_bin / "DirectBoot.efi", args.test_bin / "Payload.efi", args.firmware_code, args.firmware_vars):
        if not path.is_file():
            parser.error(f"required input missing: {path}")
    repo = Path(__file__).resolve().parents[3]
    if args.output:
        output = args.output.resolve()
        if output == repo or repo in output.parents:
            parser.error("output must be outside the repository")
        output.mkdir(parents=True, exist_ok=False)
    else:
        output = Path(tempfile.mkdtemp(prefix="bdsapp-qemu-"))
    (output / "artifacts").mkdir()
    for source, name in ((args.bds, "BdsApp.efi"), (args.test_bin / "DirectBoot.efi", "DirectBoot.efi"), (args.test_bin / "Payload.efi", "Payload.efi")):
        shutil.copyfile(source, output / "artifacts" / name)
    direct_firmware = output / "AAVMF-direct.fd"
    subprocess.run([sys.executable, str(Path(__file__).with_name("make-direct-firmware.py")),
                    str(args.firmware_code), str(output / "artifacts/DirectBoot.efi"), str(direct_firmware)], check=True)
    args.firmware_code = direct_firmware
    runner = Runner(args, output)
    sources = {str(p): digest(p) for p in sorted(Path(__file__).parent.iterdir()) if p.suffix in (".c", ".h", ".inf", ".dsc", ".sh", ".py")}
    report = {"output": str(output), "invocation": shlex.join([sys.executable, *sys.argv]),
              "artifacts": {p.name: digest(p) for p in (output / "artifacts").iterdir()},
              "firmware": {str(p.resolve()): digest(p) for p in (args.firmware_code, args.firmware_vars)},
              "test_sources_sha256": sources, "qemu_version": runner.command([args.qemu, "--version"]).strip(), "cases": []}
    print(f"Results: {output}", flush=True)
    try:
        for case in args.cases:
            directory = output / case
            directory.mkdir()
            result = {"case": case, "status": "FAIL", "boots": []}
            try:
                disk, vars_file = runner.fixture(case, directory)
                for phase in range(1, 3 if case.startswith("nv-") else 2):
                    boot = runner.boot(case, phase, directory, disk, vars_file)
                    result["boots"].append(boot)
                    print(f"{boot['status']} {case} phase={phase} ({boot['seconds']}s)", flush=True)
                    if boot["status"] != "PASS":
                        break
                result["status"] = "PASS" if all(b["status"] == "PASS" for b in result["boots"]) else "FAIL"
            except (OSError, subprocess.SubprocessError, RuntimeError) as exc:
                result["error"] = str(exc)
                print(f"FAIL {case}: {exc}", flush=True)
            report["cases"].append(result)
            (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    finally:
        runner.commands.close()
    failed = sum(case["status"] != "PASS" for case in report["cases"])
    print(f"{len(report['cases']) - failed}/{len(report['cases'])} cases passed; report {output / 'report.json'}", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
