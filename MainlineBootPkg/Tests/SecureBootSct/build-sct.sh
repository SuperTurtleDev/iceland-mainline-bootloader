#!/usr/bin/env bash
# Build the official UEFI SCT SecureBoot tests, harness and EDK2 Shell.
# Use a private, prepared EDK2 workspace: this writes Conf and Build there.
# Source reference: tianocore/edk2-test edk2-test-stable202608.
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -eo pipefail
if [ "$#" -ne 2 ]; then
  printf 'Usage: %s ISOLATED_EDK2_WORKSPACE EDK2_TEST_CHECKOUT\n' "$0" >&2
  exit 2
fi
workspace=$(realpath "$1")
sct_source=$(realpath "$2")
shift 2
expected=ddde0f95aa59a6985132e315e486cdaedfc0280b
[ "$(git -C "$sct_source" rev-parse HEAD)" = "$expected" ] || {
  printf 'Expected official edk2-test-stable202608 commit %s\n' "$expected" >&2
  exit 2
}
[ -z "$(git -C "$sct_source" status --porcelain -- uefi-sct/SctPkg)" ] || {
  printf 'SCT sources must be unmodified.\n' >&2
  exit 2
}
cd "$workspace"
export WORKSPACE=$workspace
export PACKAGES_PATH=$sct_source/uefi-sct
export GCC_AARCH64_PREFIX=${GCC_AARCH64_PREFIX:-aarch64-linux-gnu-}
export PYTHON_COMMAND=python3
source ./edksetup.sh
python3 - "$sct_source" <<'PY'
from pathlib import Path
import sys
p = Path('Conf/tools_def.txt')
s = p.read_text()
# GCC 15 defaults to C23; SCT and EDK2 use pre-C23 C function declarations.
s = s.replace('DEFINE GCC_ALL_CC_FLAGS            = -g ',
              'DEFINE GCC_ALL_CC_FLAGS            = -std=gnu17 -g ')
p.write_text(s)
platform = (Path(sys.argv[1]) / 'uefi-sct/SctPkg/UEFI/UEFI_SCT.dsc').read_text()
# AArch64 va_list/structure copies require the standard compiler intrinsics.
# Only the generated platform configuration changes; SCT sources stay pristine.
platform += '\n[LibraryClasses.AARCH64]\n  NULL|ArmPkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf\n'
Path('SctSecureBoot.dsc').write_text(platform)
PY
base=SctPkg/TestCase/UEFI/EFI/RuntimeServices/SecureBoot/BlackBoxTest
for module in \
  SctPkg/TestInfrastructure/SCT/Framework/Sct.inf \
  SctPkg/TestInfrastructure/SCT/Drivers/StandardTest/StandardTest.inf \
  SctPkg/TestInfrastructure/SCT/Drivers/TestProfile/TestProfile.inf \
  SctPkg/TestInfrastructure/SCT/Drivers/TestRecovery/TestRecovery.inf \
  SctPkg/TestInfrastructure/SCT/Drivers/TestLogging/TestLogging.inf \
  "$base/SecureBootBBTest.inf" \
  "$base/Dependency/Images/SampleAppForSecureBootTest1/SampleAppForSecureBootTest1.inf" \
  "$base/Dependency/Images/SampleAppForSecureBootTest2/SampleAppForSecureBootTest2.inf" \
  "$base/Dependency/Images/SampleAppForSecureBootTest3/SampleAppForSecureBootTest3.inf"
do
  build -p SctSecureBoot.dsc -m "$module" -D ENABLE_SECUREBOOT_TESTS=TRUE \
    -a AARCH64 -t GCC -b RELEASE -n 4
done
build -p ShellPkg/ShellPkg.dsc -m ShellPkg/Application/Shell/Shell.inf \
  -a AARCH64 -t GCC -b RELEASE -n 4
