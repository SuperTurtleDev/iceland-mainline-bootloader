#!/usr/bin/env python3
"""Test production BIOS Setup policy: locked-menu hiding, mount mode chooser
and fastboot challenge ordering.

Requires Python 3 and a C11 compiler (CC, or cc). The chooser implementation is
extracted from BdsSetup.c; its implementation is never copied here.
Compilation artifacts are temporary. Run this script from any directory.
"""

import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[3]


def production_function(source, name):
    """Extract a file-local function using the source's top-level brace style."""
    match = re.search(r"(?m)^" + re.escape(name) + r" \(", source)
    assert match, f"Function not found: {name}"
    start = source.rfind("\nSTATIC", 0, match.start()) + 1
    assert start > 0, f"Static declaration not found: {name}"
    opening = source.index("\n{", match.end())
    end = source.index("\n}", opening) + 2
    return source[start:end]


def top_function(source, name):
    """Extract a non-static top-level function by name."""
    match = re.search(r"(?m)^" + re.escape(name) + r" \(", source)
    assert match, f"Function not found: {name}"
    start = source.rfind("\n", 0, match.start()) + 1
    opening = source.index("\n{", match.end())
    end = source.index("\n}", opening) + 2
    return source[start:end]


HOST_STUBS = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#define STATIC static
#define IN
#define OUT
#define CONST const
#define VOID void
#define TRUE 1
#define FALSE 0
#define ARRAY_SIZE(Value) (sizeof(Value) / sizeof((Value)[0]))
#define SFB_ADV_ROW_CHARS 80
typedef int BOOLEAN;
typedef int EFI_STATUS;
typedef wchar_t CHAR16;
typedef uintptr_t UINTN;
typedef intptr_t INTN;
static INTN choice;
static int choose_count;
static void UnicodeSPrint(CHAR16 *buffer, UINTN size, const CHAR16 *format, const CHAR16 *target) {
  (void)size; (void)format; (void)target;
  buffer[0] = 0;
}
static INTN SfbAdvChoose(const CHAR16 *title, const CHAR16 *subtitle,
  const CHAR16 (*rows)[SFB_ADV_ROW_CHARS], UINTN count, UINTN cursor) {
  (void)title; (void)subtitle; (void)cursor;
  choose_count++;
  assert(count == 2);
  assert(wcscmp(rows[0], L"Read only") == 0);
  assert(wcscmp(rows[1], L"Read Write") == 0);
  return choice;
}
'''

CHOOSER_TESTS = r'''
int main(void) {
  BOOLEAN read_only;

  choice = 0; read_only = 9; choose_count = 0;
  assert(SfbAdvChooseMountMode(L"test", &read_only));
  assert(read_only == TRUE && choose_count == 1);

  choice = 1; read_only = 9;
  assert(SfbAdvChooseMountMode(L"test", &read_only));
  assert(read_only == FALSE && choose_count == 2);

  choice = -1; read_only = 9;
  assert(!SfbAdvChooseMountMode(L"test", &read_only));
  assert(read_only == 9 && choose_count == 3);

  puts("PASS: mount mode chooser presents Read only/Read Write and honors cancel");
  return 0;
}
'''


def check_call_sites(setup, fastboot, chooser):
    # The mount chooser is a plain RO/RW choice; policy is enforced by hiding
    # the parent rows, not by rejecting the choice.
    assert "BdsSecurityGetFastbootEnabled" not in chooser
    assert "BdsSecurityLocked" not in chooser
    assert "SfbAdvStorageManagementAllowed" not in setup
    assert "Secure Fastboot enabled; storage management disabled" not in setup

    for name, export in (
        ("SfbAdvExportPartition", "BdsUsbDiskCreate ("),
        ("SfbAdvExportLun", "BdsUsbDiskCreatePassThrough ("),
    ):
        body = production_function(setup, name)
        assert body.index("SfbAdvChooseMountMode (") < body.index(export) < body.index("BdsUsbMsdExport ("), name

    sink = production_function(setup, "SfbAdvExportConfigSink")
    assert sink.index("BdsUsbDiskCreate (") < sink.index("BdsUsbMsdExport (")

    # Volumes was a duplicate of the File Browser and is gone.
    assert "SfbRunVolumesMenu" not in setup
    assert "SfbAdvCollectVolumes" not in setup
    assert "SfbAdvVolumeLabel" not in setup
    assert 'L"Volumes >"' not in setup
    assert "grouped volume browser" not in setup

    # Locked mode lists only Enter Fastboot. The privileged rows stay in the
    # static array and are reached only after the row-count gate.
    root = top_function(setup, "BdsRunSetup")
    for row in (
        'L"Enter Fastboot"', 'L"USB Mass Storage >"', 'L"Boot Order >"',
        'L"Boot Entries >"', 'L"File Browser >"', 'L"Refresh Devices"',
        'L"Secure Fastboot >"',
    ):
        assert row in root, row
    assert root.count("BdsSecurityLocked ()") == 1
    assert "RowCount = BdsSecurityLocked () ? 1 : ARRAY_SIZE (Rows)" in root
    assert "RowCount == 1 ? LockHint : NULL" in root
    assert "Secure Fastboot locked" in root
    assert root.index("RowCount = BdsSecurityLocked") < root.index("SfbAdvChoose (")
    assert root.index("RowCount = BdsSecurityLocked") < root.index("SfbRunMassStorageMenu (")
    assert re.search(r'case 1:\s*SfbRunMassStorageMenu \(\);', root)
    assert re.search(r'case 2:\s*SfbRunPriorityMenu \(\);', root)
    assert re.search(r'case 3:\s*SfbRunManagementMenu \(\);', root)
    assert re.search(r'case 4:\s*BdsRunFileBrowser \(\);', root)
    assert re.search(r'case 5:\s*SfbConnectAll \(\);', root)
    assert re.search(r'case 6:\s*BdsRunSecureFastbootMenu \(\);', root)
    assert "case 7:" not in root

    # Fastboot still enforces the challenge lock at the command handlers.
    for name in ("CmdFetch", "CmdFlash", "CmdErase"):
        body = production_function(fastboot, name)
        assert body.count("BdsSecurityLocked ()") == 1, name

    fetch = production_function(fastboot, "CmdFetch")
    flash = production_function(fastboot, "CmdFlash")
    assert fetch.index('"sb_challenge"') < fetch.index("BdsSecurityLocked ()")
    assert flash.index('L"sb_challenge.sig"') < flash.index("BdsSecurityLocked ()")

    oem_vars = production_function(fastboot, "CmdUefiVariable")
    assert oem_vars.count("BdsSecurityLocked ()") == 1
    assert oem_vars.index("BdsSecurityLocked ()") < oem_vars.index("UefiVariableCommandWithData (")
    assert "Fastboot locked; authorize via sb_challenge first" in oem_vars

    assert "BdsSecurityRestrictsStorage" not in setup + fastboot
    assert "BdsSecurityReady" not in chooser
    assert "USB exports are read-only" not in setup
    assert "SecureBootEnable" not in setup
    assert "BdsRunSecureBootMenu" not in setup
    assert 'L"Secure Boot >"' not in setup
    assert 'L"Secure Fastboot >"' in setup
    fastboot_menu = production_function(setup, "BdsRunSecureFastbootMenu")
    assert "BdsSecurityGetFastbootEnabled" in fastboot_menu
    assert "BdsSecuritySetFastbootEnabled" in fastboot_menu
    print("PASS: locked BIOS Setup hides privileged rows and keeps only Enter Fastboot")
    print("PASS: Volumes removed; mount chooser is policy-free; fastboot challenge ordering retained")


def main():
    setup = (ROOT / "MainlineBootPkg/Application/BdsApp/BdsSetup.c").read_text()
    fastboot = (ROOT / "MainlineBootPkg/Library/CanoeFastbootLib/FastbootCmds.c").read_text()
    chooser = production_function(setup, "SfbAdvChooseMountMode")
    with tempfile.TemporaryDirectory(prefix="bds-ui-policy-") as temporary:
        binary = Path(temporary) / "chooser-tests"
        subprocess.run(
            [*shlex.split(os.environ.get("CC", "cc")), "-x", "c", "-std=c11",
             "-Wall", "-Wextra", "-Werror", "-o", str(binary), "-"],
            input=HOST_STUBS + chooser + "\n" + CHOOSER_TESTS,
            text=True, check=True,
        )
        subprocess.run([str(binary)], check=True)
    check_call_sites(setup, fastboot, chooser)


if __name__ == "__main__":
    main()
