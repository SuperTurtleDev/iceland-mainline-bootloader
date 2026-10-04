#!/usr/bin/env bash
## @file build.sh
#
#  Container-side build entry for MainlineBootPkg (see MainlineBootPkg.dsc,
#  which references this script).  Runs inside the podman image defined by
#  the meta repository's Containerfile and is invoked by
#  scripts/build-bootloader.sh on the host.
#
#  Produces $OUT/BootApp.efi, $OUT/TestBootApp.efi and a toolchain fragment
#  ($OUT/container-fragment.txt) that the host script merges into
#  buildinfo.txt.
#
#  Environment:
#    SRC                - read-only mount of the bootloader meta-repo root
#    OUT                - writable output directory
#    WORK               - scratch dir (default /work)
#    BOOTLOADER_TARGET  - DEBUG|RELEASE            (default RELEASE)
#    BOOTLOADER_TOOLCHAIN - CLANG35                (default CLANG35)
#    BOOTLOADER_ARCH    - AARCH64                  (default AARCH64)
#    SOURCE_DATE_EPOCH  - pinned by build-bootloader.sh (reproducibility)
#
#  SPDX-License-Identifier: BSD-3-Clause-Clear
##
# edk2's edksetup.sh/BuildEnv reference unset variables by design, so no
# `set -u` here; errexit + explicit tests below carry the error handling.
set -eo pipefail

SRC="${SRC:?meta repo root (read-only mount)}"
OUT="${OUT:?output directory}"
WORK="${WORK:-/work}"
TARGET="${BOOTLOADER_TARGET:-RELEASE}"
TOOLCHAIN="${BOOTLOADER_TOOLCHAIN:-CLANG35}"
ARCH="${BOOTLOADER_ARCH:-AARCH64}"

export TZ=UTC LC_ALL=C
: "${SOURCE_DATE_EPOCH:?must be pinned by build-bootloader.sh}"

# Build from a deterministic scratch copy: the pristine checkout mounted at
# $SRC (including the edk2 submodule) must never be dirtied by the build.
rm -rf "$WORK"
mkdir -p "$WORK" "$OUT"
cp -a "$SRC" "$WORK/src"

EDK2="$WORK/src/edk2"

# --- edk2 build environment --------------------------------------------------
# Classic ABL layout: WORKSPACE = edk2 tree, MainlineBootPkg resolved through
# PACKAGES_PATH (its parent dir, i.e. the scratch copy of the meta repo).
# Build outputs land in $WORKSPACE/Build, i.e. on the scratch copy.
export WORKSPACE="$EDK2"
export PACKAGES_PATH="$WORK/src"
# edksetup.sh references $PYTHON_COMMAND / $EDK_TOOLS_PATH unguarded; they
# must be set first (build.sh also runs under set -u).
export PYTHON_COMMAND=python3
export PYTHONDONTWRITEBYTECODE=1
export EDK_TOOLS_PATH="$EDK2/BaseTools"
cd "$EDK2"
. ./edksetup.sh

# --- host-side BaseTools ------------------------------------------------------
# BUILD_CC carries -std=gnu89: the vintage PCCTS sources (VfrCompile) use
# K&R-style `()` declarations, which gcc >= 15 (default -std=gnu23) rejects.
# -U_FORTIFY_SOURCE + the -Wno-* set: gcc >= 12 false positives (stringop
# family on the vendored brotli, use-after-free/dangling-pointer on the
# 7-Zip and GenFfs/GenSec "fclose(); ptr = NULL" idiom, ...) that the old
# BaseTools code triggers under -Werror.  Disabling the warning class
# (instead of -Wno-error) keeps it order-proof against the later -Werror.
# Putting these in BUILD_CC reaches every sub-makefile, including Pccts's
# own ones that reassign BUILD_CFLAGS (EXTRA_OPTFLAGS would not).
# Host tools only - none of this touches the AARCH64 target compilation.
make -C "$EDK2/BaseTools" -j"$(nproc)" \
     BUILD_CC="gcc -std=gnu89 -U_FORTIFY_SOURCE -Wno-stringop-overflow -Wno-stringop-truncation -Wno-use-after-free -Wno-dangling-pointer -Wno-array-bounds -Wno-format-overflow"

# --- cross toolchain env (mirrors the ABL top-level makefile) -----------------
# The shipped (not template) edk2 Conf/tools_def.txt references
# ENV(CLANG35_BIN), ENV(FUSE_LD), ENV(MAKEPATH) for the CLANG35 toolchain:
#   SLINK/RC/OBJCOPY = ENV(CLANG35_BIN)llvm-ar / llvm-objcopy
#   DLINK2_FLAGS end with -fuse-ld=ENV(FUSE_LD)
export CLANG35_BIN=/usr/bin/
export FUSE_LD=/usr/bin/ld.lld
export MAKEPATH=/usr/bin/
export CLANG_EXTRA_DLINK_FLAGS=
CLANG_MAJOR="$(clang --version | sed -n '1s/.*clang version \([0-9]*\)\..*/\1/p')"
if [ "${CLANG_MAJOR:-0}" -ge 17 ]; then
  # CLANG_NEED_EXTRA_DLINK_FLAGS from the ABL makefile.
  export CLANG_EXTRA_DLINK_FLAGS="-fuse-ld=lld -Wl,--no-relax -Wl,--apply-dynamic-relocs"
fi

# --- build --------------------------------------------------------------------
build -p MainlineBootPkg/MainlineBootPkg.dsc -a "$ARCH" -t "$TOOLCHAIN" -b "$TARGET"

# Module outputs keep their package path, e.g.
# .../AARCH64/MainlineBootPkg/Application/BootApp/BootApp/OUTPUT/BootApp.efi
for APP in BootApp TestBootApp; do
  EFI="$(find "$WORKSPACE/Build/MainlineBoot/${TARGET}_${TOOLCHAIN}/${ARCH}" \
             -type f -name "$APP.efi" -path '*OUTPUT*' | head -1)"
  test -n "$EFI" && test -s "$EFI"
  cp "$EFI" "$OUT/$APP.efi"
done

# --- toolchain fragment for buildinfo.txt --------------------------------------
{
  echo "os                      : $(. /etc/os-release && echo "$PRETTY_NAME")"
  echo "clang                   : $(clang --version | head -1)"
  echo "lld                     : $(ld.lld --version | head -1)"
  echo "llvm-ar                 : $(llvm-ar --version | head -1)"
  echo "gcc (BaseTools, host)   : $(gcc --version | head -1)"
  echo "python3                 : $(python3 --version)"
  echo "make                    : $(make --version | head -1)"
  echo "SOURCE_DATE_EPOCH       : $SOURCE_DATE_EPOCH"
  echo "CLANG35_BIN             : $CLANG35_BIN"
  echo "FUSE_LD                 : $FUSE_LD"
  echo "CLANG_EXTRA_DLINK_FLAGS : $CLANG_EXTRA_DLINK_FLAGS"
  echo "build command           : build -p MainlineBootPkg/MainlineBootPkg.dsc -a $ARCH -t $TOOLCHAIN -b $TARGET"
} > "$OUT/container-fragment.txt"

echo "build.sh: BootApp.efi + TestBootApp.efi -> $OUT"
