#!/usr/bin/env python3
"""Run official UEFI SCT SecureBoot tests through production BdsApp.

Uses the test-only SecureBootDirect launcher (with optional dbx-initial.auth
support), real AAVMF authenticated variables, and a signed Shell fallback.
The original firmware BDS is replaced. No test assertions are implemented here;
pass/fail counts come from official SCT Data/TestCase.ini and its native logs.
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
import time

ANSI = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1b\\)|[()][A-Z0-9])")
REVISION = "ddde0f95aa59a6985132e315e486cdaedfc0280b"
EXPECTED_ASSERTIONS = {"VariableAttributes": 12, "VariableUpdates": 10, "ImageLoading": 20}
# Upstream reuses assertion GUID D2073163 for both unsigned and signed KEK
# updates (VariableUpdatesBBTest.c:297 and :373). SCT deduplicates by GUID in
# TestCase.ini and CSV, but the native case log records both executions.
EXPECTED_UNIQUE_ASSERTIONS = {"VariableAttributes": 12, "VariableUpdates": 9, "ImageLoading": 20}
CASES = set(EXPECTED_ASSERTIONS)
REQUIRED = (
    "BDSTEST DIRECT BDS: standard firmware boot manager replaced",
    "SBTEST MODE: SetupMode=1 SecureBoot=0",
    "SBTEST MODE: SetupMode=0 SecureBoot=1",
    "SBTEST ENABLED PRE: SecureBoot=1 SetupMode=0; firmware Security/Security2 reject trusted and unsigned images",
    "SBTEST ARMED: starting production BdsApp",
    "BdsApp: application BDS core starting",
    "BdsApp: SecureBoot=1, local edk2 image verifier active",
    "SCT_OFFICIAL_BEGIN edk2-test-stable202608",
    "SCT_OFFICIAL_EXECUTION_RETURNED",
    "SCT_OFFICIAL_REPORT_RETURNED",
    "SCT_OFFICIAL_END",
)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def text_file(path):
    data = path.read_bytes()
    encoding = "utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-16-le" if b"\0" in data[:64] else "utf-8"
    return data.decode(encoding, errors="replace")


def read_counts(path):
    result = {}
    for section in text_file(path).split("[Test Case]")[1:]:
        fields = {}
        for line in section.splitlines():
            if "=" in line and not line.lstrip().startswith(("#", ";")):
                key, value = line.split("=", 1)
                fields[key.strip()] = value.strip()
        name = fields.get("Name")
        if name in CASES:
            result[name] = {key.lower(): int(fields[key], 0) for key in ("Passes", "Warnings", "Failures")}
            result[name]["guid"] = fields["Guid"]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bds", required=True, type=Path)
    parser.add_argument("--direct", required=True, type=Path)
    parser.add_argument("--overlay", required=True, type=Path, help="esp directory from prepare-overlay.py")
    parser.add_argument("--output", required=True, type=Path, help="new directory outside repository")
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    parser.add_argument("--firmware-code", type=Path, default=Path("/usr/share/AAVMF/AAVMF_CODE.secboot.fd"))
    parser.add_argument("--firmware-vars", type=Path, default=Path("/usr/share/AAVMF/AAVMF_VARS.fd"))
    args = parser.parse_args()
    for name in ("bds", "direct", "overlay", "output", "firmware_code", "firmware_vars"):
        setattr(args, name, getattr(args, name).resolve())
    repo = Path(__file__).resolve().parents[3]
    if args.output == repo or repo in args.output.parents:
        parser.error("output must be outside repository")
    if args.timeout <= 0:
        parser.error("timeout must be positive")
    for file in (args.bds, args.direct, args.firmware_code, args.firmware_vars,
                 args.overlay / "startup.nsh", args.overlay / "Sct/Test/SecureBootBBTest.efi"):
        if not file.is_file():
            parser.error(f"input missing: {file}")
    manifest_path = args.overlay.parent / "manifest.json"
    if not manifest_path.is_file():
        parser.error("overlay requires the manifest from prepare-overlay.py")
    manifest_data = json.loads(manifest_path.read_text())
    if manifest_data.get("revision") != REVISION or manifest_data.get("source_modified") is not False:
        parser.error("overlay must contain unmodified official edk2-test-stable202608 tests")
    for filename, expected in manifest_data["esp_files_sha256"].items():
        path = (args.overlay / filename).resolve()
        if args.overlay not in path.parents or not path.is_file() or digest(path) != expected:
            parser.error(f"overlay integrity check failed: {filename}")
    args.output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, LC_ALL="C", TZ="UTC", MTOOLS_SKIP_CHECK="1")

    def record(argv):
        with (args.output / "commands.txt").open("a") as stream:
            stream.write(shlex.join(list(map(str, argv))) + "\n")

    def run(argv, stdin=None):
        argv = list(map(str, argv))
        record(argv)
        result = subprocess.run(argv, input=stdin, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, timeout=60)
        with (args.output / "tools.log").open("a") as stream:
            stream.write(shlex.join(argv) + "\n" + result.stdout)
        result.check_returncode()
        return result.stdout

    inputs = (args.bds, args.direct, args.firmware_code, args.firmware_vars)
    report = {"suite": "official UEFI SCT SecureBootBBTest", "status": "ERROR", "output": str(args.output),
              "invocation": shlex.join([sys.executable, *sys.argv]),
              "inputs_sha256": {str(p): digest(p) for p in inputs},
              "image_verifier": "production BdsApp; test-only firmware Security2 bypass",
              "variable_backend": "real AAVMF authenticated VariableDxe; not physical secapp",
              "cases": {}}
    try:
        esp = args.output / "esp-input"
        shutil.copytree(args.overlay, esp)
        shutil.copyfile(args.bds, esp / "EFI/TEST/BdsApp.efi")
        manifest = args.overlay.parent / "manifest.json"
        if manifest.is_file():
            shutil.copyfile(manifest, args.output / "suite-manifest.json")
        direct = args.output / "SecureBootDirect.efi"
        shutil.copyfile(args.direct, direct)
        firmware = args.output / "AAVMF-direct-secboot.fd"
        patcher = Path(__file__).resolve().parent.parent / "BdsApp/make-direct-firmware.py"
        run([sys.executable, patcher, args.firmware_code, direct, firmware])
        vars_file = args.output / "VARS.fd"
        shutil.copyfile(args.firmware_vars, vars_file)
        disk = args.output / "fixture.gpt"
        sectors = 131072
        with disk.open("wb") as stream:
            stream.truncate((2048 + sectors + 2048) * 512)
        table = ("label: gpt\nlabel-id: 22222222-3333-4444-8555-666666666666\nunit: sectors\n"
                 f"start=2048, size={sectors}, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, "
                 'uuid=11111111-2222-4333-8444-555555555551, name="Official SCT fixture"\n')
        run(["sfdisk", "--no-reread", "--no-tell-kernel", disk], table)
        partition = args.output / "partition.fat"
        with partition.open("wb") as stream:
            stream.truncate(sectors * 512)
        run(["mkfs.vfat", "--invariant", "-F", "32", "-n", "SCTTEST", partition])
        with disk.open("r+b") as stream, partition.open("rb") as source:
            stream.seek(2048 * 512)
            shutil.copyfileobj(source, stream)
        partition.unlink()
        target = f"{disk}@@1048576"
        for child in sorted(esp.iterdir()):
            run(["mcopy", "-s", "-i", target, child, "::/"])
        report["firmware_sha256"] = digest(firmware)
        report["initial_vars_sha256"] = digest(vars_file)
        report["initial_disk_sha256"] = digest(disk)
        argv = [args.qemu, "-machine", "virt", "-accel", "tcg", "-cpu", "cortex-a57", "-m", "1024", "-smp", "1",
                "-nodefaults", "-display", "none", "-monitor", "none", "-serial", "stdio", "-nic", "none", "-no-reboot",
                "-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={firmware}",
                "-drive", f"if=pflash,format=raw,unit=1,file={vars_file}",
                "-drive", f"if=none,id=fixture,format=raw,file={disk}", "-device", "virtio-blk-pci,drive=fixture"]
        report["qemu_argv"] = argv
        record(argv)
        started = time.monotonic()
        errors = []
        with (args.output / "serial.log").open("wb") as stream:
            try:
                result = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=stream, stderr=subprocess.STDOUT, env=env, timeout=args.timeout)
                if result.returncode:
                    errors.append(f"QEMU exited {result.returncode}")
            except subprocess.TimeoutExpired:
                errors.append(f"QEMU exceeded {args.timeout}s")
        report["seconds"] = round(time.monotonic() - started, 2)
        serial = ANSI.sub("", (args.output / "serial.log").read_text(errors="replace")).replace("\r", "")
        (args.output / "serial.clean.log").write_text(serial)
        errors.extend(f"missing marker: {marker}" for marker in REQUIRED if marker not in serial)
        for phase in ("EXECUTION", "REPORT"):
            found = re.search(rf"SCT_OFFICIAL_{phase}_RETURNED\s+(\S+)", serial)
            if found is not None:
                try:
                    if int(found.group(1), 16) != 0:
                        errors.append(f"official SCT {phase.lower()} returned {found.group(1)}")
                except ValueError:
                    errors.append(f"unreadable SCT {phase.lower()} return status: {found.group(1)}")
        if serial.count("BdsApp: application BDS core starting") != 1:
            errors.append("expected exactly one BdsApp startup")
        if "SBTEST FAIL:" in serial or "BDSTEST FAIL:" in serial:
            errors.append("test launcher failed")
        results = args.output / "sct-results"
        results.mkdir()
        run(["mcopy", "-s", "-i", target, "::/Sct", results])
        result_path = results / "Sct/Data/TestCase.ini"
        if result_path.is_file():
            report["cases"] = read_counts(result_path)
        else:
            errors.append("SCT did not write Data/TestCase.ini")
        counts = report["cases"]
        if set(counts) != CASES:
            errors.append("not all three official SecureBoot cases were recorded")
        for name, values in counts.items():
            if any(values[key] == 0xFFFFFFFF for key in ("passes", "warnings", "failures")):
                errors.append(f"{name} did not finish")
            elif values["passes"] + values["warnings"] + values["failures"] != EXPECTED_UNIQUE_ASSERTIONS[name]:
                errors.append(f"{name} did not record all {EXPECTED_UNIQUE_ASSERTIONS[name]} unique official assertions")
        returned = []
        executions = {}
        for log in (results / "Sct/Log").rglob("*.log"):
            native = text_file(log)
            returned.extend(re.findall(r"Returned Status Code:\s*([^\r\n]+)", native))
            name = log.name.split("_", 1)[0]
            if name in CASES:
                matches = re.findall(r"Passes\.+\s*(\d+)\s+Warnings\.+\s*(\d+)\s+Errors\.+\s*(\d+)", native)
                if len(matches) != 1:
                    errors.append(f"{name} native assertion totals missing or ambiguous")
                else:
                    passed, warned, failed = map(int, matches[0])
                    executions[name] = {"passes": passed, "warnings": warned, "failures": failed}
                    if passed + warned + failed != EXPECTED_ASSERTIONS[name]:
                        errors.append(f"{name} did not execute all {EXPECTED_ASSERTIONS[name]} official assertions")
        report["assertion_executions"] = executions
        if set(executions) != CASES:
            errors.append("native totals are missing for one or more official cases")
        report["test_return_statuses"] = returned
        if len(returned) != 3 or any(value.strip() != "Success" for value in returned):
            errors.append("official case completion logs are missing or report an error status")
        if not (results / "Sct/Report/SecureBoot.csv").is_file():
            errors.append("official SCT report was not generated")
        report["final_vars_sha256"] = digest(vars_file)
        report["execution_errors"] = errors
        report["status"] = "ERROR" if errors else "FAIL" if any(c["failures"] for c in counts.values()) else "PASS"
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        report["error"] = str(exc)
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"status": report["status"], "cases": report["cases"], "report": str(args.output / "report.json")}, indent=2))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
