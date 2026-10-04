#!/usr/bin/env bash
## @file build-bootloader.sh
#
#  Reproducible podman build of the MainlineBoot bootloader applications.
#
#  Outputs (BootApp.efi, TestBootApp.efi, buildinfo.txt) go to the directory
#  given by the OUT environment variable; default is build/bootloader inside
#  this repository.  Deliverable builds pass OUT explicitly:
#    OUT=/home/wyb/Documents/mainline/build/bootloader scripts/build-bootloader.sh
#
#  buildinfo.txt records the full provenance: meta-repo and submodule commit
#  hashes, dirty-vs-base diffs, container image id + pinned base image,
#  toolchain versions inside the container and artifact hashes.
#
#  Usage:
#    scripts/build-bootloader.sh                     # build (RELEASE default)
#    BOOTLOADER_TARGET=DEBUG scripts/build-bootloader.sh
#    REBUILD_IMAGE=1 scripts/build-bootloader.sh     # rebuild builder image
#    OUT=dir scripts/build-bootloader.sh             # output dir
#
#  SPDX-License-Identifier: BSD-3-Clause-Clear
##
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${OUT:-$REPO/build/bootloader}"
IMAGE="${IMAGE:-localhost/mainline-bootloader-builder}"
TAG="${IMAGE_TAG:-v1}"
TARGET="${BOOTLOADER_TARGET:-RELEASE}"

msg() { echo "[build-bootloader] $*"; }

# --- 1. builder image ---------------------------------------------------------
# The image fully captures the toolchain; its id goes into buildinfo.txt.
# EFI builds then run with --network=none, so no network access can leak in.
if [ "${REBUILD_IMAGE:-0}" = 1 ] || ! podman image exists "$IMAGE:$TAG"; then
  msg "building builder image $IMAGE:$TAG"
  podman build -t "$IMAGE:$TAG" "$REPO"
fi
IMAGE_ID="$(podman image inspect "$IMAGE:$TAG" --format '{{.Id}}')"
BASE_IMAGE="$(sed -n 's/^FROM //p' "$REPO/Containerfile" | head -1)"
CONTAINERFILE_SHA="$(sha256sum "$REPO/Containerfile" | cut -d' ' -f1)"

# --- 2. provenance helpers ----------------------------------------------------
repo_section() {  # $1 = section title, $2 = repo path
  local title="$1" repo="$2"
  local commit branch describe
  commit="$(git -C "$repo" rev-parse HEAD 2>/dev/null)" || {
    echo "----- $title -----"
    echo "path     : $repo"
    echo "status   : NOT A GIT REPOSITORY"
    return
  }
  branch="$(git -C "$repo" rev-parse --abbrev-ref HEAD)"
  describe="$(git -C "$repo" describe --always --dirty=-dirty 2>/dev/null || echo "$commit")"
  local -a status=() modified=() untracked=()
  mapfile -t status < <(git -C "$repo" status --porcelain=v1)
  for entry in "${status[@]:-}"; do
    case "$entry" in
      '?? '*) untracked+=("${entry#?? }") ;;
      '') ;;
      *)  modified+=("$entry") ;;
    esac
  done
  echo "----- $title -----"
  echo "path     : $repo"
  echo "branch   : $branch"
  echo "commit   : $commit"
  echo "describe : $describe"
  if git -C "$repo" remote get-url origin >/dev/null 2>&1; then
    echo "origin   : $(git -C "$repo" remote get-url origin)"
  fi
  if [ "${#modified[@]}" -eq 0 ]; then
    echo "status   : clean (tracked tree matches HEAD)"
  else
    echo "status   : DIRTY (${#modified[@]} tracked changes vs base; diff below)"
    printf '#   %s\n' "${modified[@]}"
    echo "diff vs base (git diff HEAD):"
    git -C "$repo" --no-pager diff HEAD || true
  fi
  if [ "${#untracked[@]}" -gt 0 ]; then
    echo "untracked (informational; not inputs of this build):"
    printf '#   %s\n' "${untracked[@]}"
  fi
  if [ -f "$repo/.gitmodules" ]; then
    echo "submodules (git submodule status --recursive; '-' = not checked out):"
    git -C "$repo" submodule status --recursive 2>/dev/null | sed 's/^/    /'
  fi
}

# --- 3. run the container build -------------------------------------------------
# SOURCE_DATE_EPOCH pinned to the meta-repo commit timestamp: __DATE__/__TIME__
# and any other time-derived bytes are then identical across rebuilds.
SDE="$(git -C "$REPO" log -1 --format=%ct HEAD 2>/dev/null || date +%s)"

mkdir -p "$OUT"
rm -f "$OUT/BootApp.efi" "$OUT/TestBootApp.efi" \
      "$OUT/container-fragment.txt" "$OUT/buildinfo.txt"
rm -rf "$OUT/symbols"

msg "building TARGET=$TARGET in $IMAGE:$TAG (SOURCE_DATE_EPOCH=$SDE)"
podman run --rm --network=none \
  -v "$REPO:/src:ro" \
  -v "$OUT:/out" \
  -e SRC=/src -e OUT=/out -e WORK=/work \
  -e BOOTLOADER_TARGET="$TARGET" \
  -e SOURCE_DATE_EPOCH="$SDE" \
  -e TZ=UTC -e LC_ALL=C \
  "$IMAGE:$TAG" \
  bash /src/MainlineBootPkg/build.sh

test -s "$OUT/BootApp.efi"
test -s "$OUT/TestBootApp.efi"
test -s "$OUT/container-fragment.txt"

# --- 4. buildinfo.txt -------------------------------------------------------------
{
  echo "==================================================================="
  echo " Mainline bootloader build info"
  echo "==================================================================="
  echo "generated (UTC)    : $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
  echo "meta repo          : $REPO"
  echo "build target       : $TARGET / CLANG35 / AARCH64"
  echo
  repo_section "meta repository" "$REPO"
  echo
  repo_section "submodule edk2" "$REPO/edk2"
  echo "  (edk2's own nested submodules listed above are outside this build's"
  echo "   dependency closure and are not checked out)"
  echo
  echo "----- build environment -----"
  echo "builder image      : $IMAGE:$TAG"
  echo "builder image id   : $IMAGE_ID"
  echo "builder Dockerfile : Containerfile (sha256 $CONTAINERFILE_SHA)"
  echo "base image         : $BASE_IMAGE"
  echo
  echo "----- toolchain (inside builder container) -----"
  cat "$OUT/container-fragment.txt"
  echo
  echo "----- artifacts -----"
  echo "# symbols/ holds the link-time ELF (.dll, with DWARF - what the .efi's"
  echo "# CodeView debug entry points at) and the linker map for addr->symbol"
  echo "# resolution of on-device crash addresses; the .efi themselves carry"
  echo "# no symbol table (GenFw drops it in the ELF->PE conversion)."
  ( cd "$OUT" && sha256sum BootApp.efi TestBootApp.efi symbols/*.dll symbols/*.map \
      && ls -l BootApp.efi TestBootApp.efi symbols/*.dll symbols/*.map \
      | awk '{printf "%-24s %s bytes\n", $NF, $5}' )
} > "$OUT/buildinfo.txt"
rm -f "$OUT/container-fragment.txt"

msg "done:"
( cd "$OUT" && sha256sum BootApp.efi TestBootApp.efi )
msg "buildinfo written to $OUT/buildinfo.txt"
