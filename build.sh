#!/usr/bin/env bash
# Host-side entry point for the MainlineBoot bootloader build.
#
# The EFI build runs inside the shared provisioned container through
# ../podman_container/runin.sh; buildinfo generation runs on the host.
#
# Incrementality -- intermediate state (source copy, built BaseTools,
# edk2 Build tree) persists in OUT/build and is refreshed with cp -au, so
# make rebuilds only what changed; the edk2 submodule commit is stamped and
# a mismatch wipes OUT/build/src for a from-scratch refresh.  A fully clean
# rebuild is BOOTLOADER_CLEAN=1 (rm -rf OUT/build) -- artifacts from a clean
# rebuild are the reproducibility reference; incremental state can mix
# objects compiled under an older SOURCE_DATE_EPOCH.
#
# Usage: ./build.sh [OUT_DIR]      (or OUT=... ./build.sh)
#        BOOTLOADER_TARGET=DEBUG ./build.sh
#        BOOTLOADER_CLEAN=1 ./build.sh
set -euo pipefail

SCRIPTDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
META="${SCRIPTDIR}"
OUT="${OUT:-${SCRIPTDIR}/../../build/bootloader}"
# podman (SecureJoin) rejects paths that still contain '..' components
mkdir -p "${OUT}"
OUT="$(cd "${OUT}" && pwd)"
RUNIN="${META}/../podman_container/runin.sh"
DATA="${OUT}/podman-data"

log() { printf '[build.sh] %s\n' "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) OUT="$1" ;;
    esac
    shift
done

[ -x "${RUNIN}" ] || die "runin.sh not found at ${RUNIN}"
command -v podman >/dev/null 2>&1 || die "podman not found"

TARGET="${BOOTLOADER_TARGET:-RELEASE}"

if [ "${BOOTLOADER_CLEAN:-0}" = 1 ]; then
    log "BOOTLOADER_CLEAN=1: wiping ${OUT}/build"
    rm -rf "${OUT}/build"
fi

# SOURCE_DATE_EPOCH pinned to the meta-repo commit timestamp: __DATE__/__TIME__
# and any other time-derived bytes are then identical across rebuilds.
SDE="$(git -C "${META}" log -1 --format=%ct HEAD 2>/dev/null || date +%s)"
if [ -d "${OUT}/build/src" ]; then INCREMENTAL=yes; else INCREMENTAL=no; fi

mkdir -p "${OUT}"
rm -f "${OUT}/BootApp.efi" "${OUT}/TestBootApp.efi" \
      "${OUT}/container-fragment.txt" "${OUT}/buildinfo.txt"
rm -rf "${OUT}/symbols"

log "building TARGET=${TARGET} (SOURCE_DATE_EPOCH=${SDE}, incremental=${INCREMENTAL})"
# --network=none rides in EXTRA_MOUNTS (runin.sh word-splits it before the
# --rootfs positional): the EFI build itself must never touch the network.
CONTAINER_DATA="${DATA}" \
EXTRA_MOUNTS="--network=none -v ${META}:/work/src:ro -v ${OUT}:/work/out" \
    bash "${RUNIN}" \
    env SRC=/work/src OUT=/work/out WORK=/work/out/build \
        BOOTLOADER_TARGET="${TARGET}" \
        SOURCE_DATE_EPOCH="${SDE}" TZ=UTC LC_ALL=C \
        bash /work/src/MainlineBootPkg/build.sh 2>&1 | tee "${OUT}/build.log"

test -s "${OUT}/BootApp.efi"
test -s "${OUT}/TestBootApp.efi"
test -s "${OUT}/container-fragment.txt"

# --- provenance helpers --------------------------------------------------------
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

# --- buildinfo.txt ---------------------------------------------------------------
{
  echo "==================================================================="
  echo " Mainline bootloader build info"
  echo "==================================================================="
  echo "generated (UTC)    : $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
  echo "meta repo          : ${META}"
  echo "build target       : ${TARGET} / CLANG35 / AARCH64"
  echo "incremental        : ${INCREMENTAL} (clean rebuild = BOOTLOADER_CLEAN=1;"
  echo "                    rm -rf OUT/build is the reproducibility reference)"
  echo
  repo_section "meta repository" "${META}"
  echo
  repo_section "submodule edk2" "${META}/edk2"
  echo "  (edk2's own nested submodules listed above are outside this build's"
  echo "   dependency closure and are not checked out)"
  echo
  repo_section "build container (src/podman_container)" "${META}/../podman_container"
  if [ -f "${DATA}/commit" ]; then
    echo "container data id  : $(cat "${DATA}/commit")"
    echo "container data at  : ${DATA}"
  fi
  if [ -f "${DATA}/build-env.txt" ]; then
    echo "container /opt/build-env.txt:"
    sed 's/^/    /' "${DATA}/build-env.txt"
  fi
  echo
  echo "----- build flags (inside container) -----"
  cat "${OUT}/container-fragment.txt"
  echo
  echo "----- artifacts -----"
  echo "# symbols/ holds the link-time ELF (.dll, with DWARF - what the .efi's"
  echo "# CodeView debug entry points at) and the linker map for addr->symbol"
  echo "# resolution of on-device crash addresses; the .efi themselves carry"
  echo "# no symbol table (GenFw drops it in the ELF->PE conversion)."
  ( cd "${OUT}" && sha256sum BootApp.efi TestBootApp.efi symbols/*.dll symbols/*.map \
      && ls -l BootApp.efi TestBootApp.efi symbols/*.dll symbols/*.map \
      | awk '{printf "%-24s %s bytes\n", $NF, $5}' )
} > "${OUT}/buildinfo.txt"
rm -f "${OUT}/container-fragment.txt"

log "done:"
( cd "${OUT}" && sha256sum BootApp.efi TestBootApp.efi )
log "buildinfo written to ${OUT}/buildinfo.txt"
