#!/usr/bin/env bash
## @file build.sh
#
#  Container-side build entry for MainlineBootPkg (see MainlineBootPkg.dsc,
#  which references this script).  Runs inside the shared container started
#  through ../podman_container/runin.sh by the meta repo's ./build.sh.
#
#  Produces $OUT/BootApp.efi, $OUT/TestBootApp.efi, $OUT/symbols/ (ELF .dll
#  + linker maps) and a build-flags fragment ($OUT/container-fragment.txt)
#  that the host script merges into buildinfo.txt.
#
#  Incrementality: $WORK persists across runs (bind-mounted by ./build.sh).
#  It holds the source copy refreshed with cp -au plus the edk2 Build tree
#  and the built BaseTools, so make rebuilds only what changed.  A change of
#  the edk2 submodule commit wipes the source copy for a from-scratch sync.
#
#  Environment (defaults match the runin.sh mount layout):
#    SRC                - read-only mount of the bootloader meta repo
#    OUT                - writable output directory
#    WORK               - persistent scratch dir (OUT/build)
#    BOOTLOADER_TARGET  - DEBUG|RELEASE            (default RELEASE)
#    BOOTLOADER_TOOLCHAIN - CLANG35                (default CLANG35)
#    BOOTLOADER_ARCH    - AARCH64                  (default AARCH64)
#    SOURCE_DATE_EPOCH  - pinned by ./build.sh (reproducibility)
#
#  SPDX-License-Identifier: BSD-3-Clause-Clear
##
# edk2's edksetup.sh/BuildEnv reference unset variables by design, so no
# `set -u` here; errexit + explicit tests below carry the error handling.
set -eo pipefail

SRC="${SRC:-/work/src}"
OUT="${OUT:-/work/out}"
WORK="${WORK:-${OUT}/build}"
TARGET="${BOOTLOADER_TARGET:-RELEASE}"
TOOLCHAIN="${BOOTLOADER_TOOLCHAIN:-CLANG35}"
ARCH="${BOOTLOADER_ARCH:-AARCH64}"

export TZ=UTC LC_ALL=C
: "${SOURCE_DATE_EPOCH:?must be pinned by ./build.sh}"

mkdir -p "$WORK" "$OUT"

# --- persistent scratch copy of the sources -----------------------------------
# The pristine meta-repo mounted at $SRC (including the edk2 submodule) must
# never be dirtied; everything is built from the copy below.  cp -au only
# copies files newer than the copy, keeping the tree incremental.  The edk2
# commit is stamped: a submodule bump triggers a full refresh (stale files
# from the old tree would otherwise linger).
EDK2_STAMP="$WORK/src.edk2-stamp"
EDK2_NOW="$(git -C "$SRC/edk2" rev-parse HEAD 2>/dev/null || echo nogit)"
if [ ! -d "$WORK/src" ]; then
    cp -a "$SRC" "$WORK/src"
    printf '%s\n' "$EDK2_NOW" > "$EDK2_STAMP"
elif [ "$(cat "$EDK2_STAMP" 2>/dev/null || true)" != "$EDK2_NOW" ]; then
    echo "build.sh: edk2 commit changed -> refreshing $WORK/src"
    rm -rf "$WORK/src"
    cp -a "$SRC" "$WORK/src"
    printf '%s\n' "$EDK2_NOW" > "$EDK2_STAMP"
else
    cp -au "$SRC/." "$WORK/src/"
fi

EDK2="$WORK/src/edk2"

# The edk2 Build tree bakes SOURCE_DATE_EPOCH-derived bytes (__DATE__ etc.)
# into objects; a stale tree would mix objects from an older pin and diverge
# from a clean rebuild.  Wipe just Build/ when the pin moves -- the source
# copy and the built BaseTools stay, so this costs only the ~8s module build.
SDE_STAMP="$WORK/src.sde-stamp"
if [ "$(cat "$SDE_STAMP" 2>/dev/null || true)" != "$SOURCE_DATE_EPOCH" ]; then
    echo "build.sh: SOURCE_DATE_EPOCH changed -> wiping edk2 Build tree"
    rm -rf "$EDK2/Build"
    printf '%s\n' "$SOURCE_DATE_EPOCH" > "$SDE_STAMP"
fi

# --- edk2 build environment --------------------------------------------------
# Classic ABL layout: WORKSPACE = edk2 tree, MainlineBootPkg resolved through
# PACKAGES_PATH (its parent dir, i.e. the scratch copy of the meta repo).
# Build outputs land in $WORKSPACE/Build, i.e. on the scratch copy.
export WORKSPACE="$EDK2"
export PACKAGES_PATH="$WORK/src"
# edksetup.sh references $PYTHON_COMMAND / $EDK_TOOLS_PATH unguarded; they
# must be set first (build.sh also runs under set -u).
export PYTHON_COMMAND=python3
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
# Incremental: the built tools persist in $WORK/src, later runs are no-ops.
make -C "$EDK2/BaseTools" -j"$(nproc)" \
     BUILD_CC="gcc -std=gnu89 -U_FORTIFY_SOURCE -Wno-stringop-overflow -Wno-stringop-truncation -Wno-use-after-free -Wno-dangling-pointer -Wno-array-bounds -Wno-format-overflow"

# --- cross toolchain env (mirrors the ABL top-level makefile) -----------------
# The shipped (not template) edk2 Conf/tools_def.txt references
# ENV(CLANG35_BIN), ENV(FUSE_LD), ENV(MAKEPATH) for the CLANG35 toolchain:
#   SLINK/RC/OBJCOPY = ENV(CLANG35_BIN)llvm-ar / llvm-objcopy
#   DLINK2_FLAGS end with -fuse-ld=ENV(FUSE_LD)
# The shared container provides the unversioned toolchain names
# (clang/ld.lld/llvm-*) as /usr/local/bin symlinks to the -21 versions;
# make stays at /usr/bin/make (only the toolchain is symlinked).
export CLANG35_BIN=/usr/local/bin/
export FUSE_LD=/usr/local/bin/ld.lld
export MAKEPATH=/usr/bin/
export CLANG_EXTRA_DLINK_FLAGS=
CLANG_MAJOR="$(clang --version | sed -n '1s/.*clang version \([0-9]*\)\..*/\1/p')"
if [ "${CLANG_MAJOR:-0}" -ge 17 ]; then
  # CLANG_NEED_EXTRA_DLINK_FLAGS from the ABL makefile.
  export CLANG_EXTRA_DLINK_FLAGS="-fuse-ld=lld -Wl,--no-relax -Wl,--apply-dynamic-relocs"
fi

# --- build --------------------------------------------------------------------
build -p MainlineBootPkg/MainlineBootPkg.dsc -a "$ARCH" -t "$TOOLCHAIN" -b "$TARGET"

# --- collect artifacts ---------------------------------------------------------
# Module outputs keep their package path, e.g.
# .../AARCH64/MainlineBootPkg/Application/BootApp/BootApp/OUTPUT/BootApp.efi
# The sibling DEBUG/ dir holds the link artifacts: <APP>.dll (ELF + DWARF
# symbols, referenced by the .efi's CodeView debug entry) and <APP>.map.
mkdir -p "$OUT/symbols"
for APP in BootApp TestBootApp; do
  MOD_DIR="$WORKSPACE/Build/MainlineBoot/${TARGET}_${TOOLCHAIN}/${ARCH}/MainlineBootPkg/Application/$APP/$APP"
  EFI="$(find "$MOD_DIR" -type f -name "$APP.efi" -path '*OUTPUT*' | head -1)"
  test -n "$EFI" && test -s "$EFI"
  cp "$EFI" "$OUT/$APP.efi"
  test -s "$MOD_DIR/DEBUG/$APP.dll"
  test -s "$MOD_DIR/DEBUG/$APP.map"
  cp "$MOD_DIR/DEBUG/$APP.dll" "$OUT/symbols/$APP.dll"
  cp "$MOD_DIR/DEBUG/$APP.map" "$OUT/symbols/$APP.map"
done

# --- ESP image (FAT32, Android sparse) -----------------------------------------
# Production deployment: the BDS launches <esp>:\EFI\BOOT\BOOTAA64.EFI
# unattended (see BootApp.c), so the ESP carries exactly BootApp.efi under
# that name -- TestBootApp is an abl/FV test payload, not an ESP app.
# Reproducibility: FAT volume id is fixed from the pinned epoch (mkfs.vfat
# would derive it from the clock) and mcopy keeps the source mtime, which is
# pinned to SOURCE_DATE_EPOCH above.
ESP_MB="${BOOTLOADER_ESP_MB:-1024}"
ESP_RAW="$WORK/esp-fat.img"
rm -f "$ESP_RAW" "$OUT/esp.img"
touch -d "@$SOURCE_DATE_EPOCH" "$OUT/BootApp.efi"
truncate -s "$((ESP_MB * 1024 * 1024))" "$ESP_RAW"
mkfs.vfat -F 32 -n ESP -i "$(printf '0x%08x' "$SOURCE_DATE_EPOCH")" "$ESP_RAW"
mmd  -i "$ESP_RAW" ::/EFI ::/EFI/BOOT
mcopy -i "$ESP_RAW" "$OUT/BootApp.efi" ::/EFI/BOOT/BOOTAA64.EFI
img2simg "$ESP_RAW" "$OUT/esp.img"
test -s "$OUT/esp.img"

# --- build-flags fragment for buildinfo.txt -------------------------------------
{
  echo "SOURCE_DATE_EPOCH       : $SOURCE_DATE_EPOCH"
  echo "CLANG35_BIN             : $CLANG35_BIN"
  echo "FUSE_LD                 : $FUSE_LD"
  echo "CLANG_EXTRA_DLINK_FLAGS : $CLANG_EXTRA_DLINK_FLAGS"
  echo "build command           : build -p MainlineBootPkg/MainlineBootPkg.dsc -a $ARCH -t $TOOLCHAIN -b $TARGET"
  echo "esp image               : FAT32 ${ESP_MB} MiB -> Android sparse (img2simg),"
  echo "                           volume id $(printf '0x%08x' "$SOURCE_DATE_EPOCH"),"
  echo "                           /EFI/BOOT/BOOTAA64.EFI = BootApp.efi"
} > "$OUT/container-fragment.txt"

echo "build.sh: BootApp.efi + TestBootApp.efi -> $OUT"
