#!/usr/bin/env bash
# Build only test helpers, in a separate output tree; never build BdsApp here.
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -eo pipefail
if [[ $# -lt 1 ]]; then
  printf 'Usage: %s /absolute/edk2-workspace [extra build arguments]\n' "$0" >&2
  exit 2
fi
export WORKSPACE="$(realpath "$1")"
shift
BUILD_ARGS=("$@")
set --
TEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export PACKAGES_PATH="$(realpath "$TEST_DIR/../../..")${PACKAGES_PATH:+:$PACKAGES_PATH}"
export EDK_TOOLS_PATH="$WORKSPACE/BaseTools"
export PYTHON_COMMAND=python3
export CLANG35_BIN="${CLANG35_BIN:-/usr/bin/}"
export FUSE_LD="${FUSE_LD:-/usr/bin/ld.lld}"
export MAKEPATH="${MAKEPATH:-/usr/bin/}"
export CLANG_EXTRA_DLINK_FLAGS='-fuse-ld=lld -Wl,--no-relax -Wl,--apply-dynamic-relocs'
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1795590000}"
export TZ=UTC LC_ALL=C
cd "$WORKSPACE"
# edksetup.sh is not nounset-safe. The caller must have prepared BaseTools.
. ./edksetup.sh
build -p MainlineBootPkg/Tests/BdsApp/BdsAppTests.dsc -a AARCH64 -t CLANG35 -b RELEASE -n 4 "${BUILD_ARGS[@]}"
