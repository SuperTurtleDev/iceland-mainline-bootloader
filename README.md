# mainline bootloader

Bootloader meta repository for the mainline-Linux device port.

Layout:

- `MainlineBootPkg/` — UEFI bootloader applications (`BootApp` = production
  `BOOTAA64.EFI`, `TestBootApp` = USB fastboot test front-end) plus the
  container-side build script (`MainlineBootPkg/build.sh`).
- `edk2/` — git submodule, codelinaro `la/abl/tianocore/edk2`
  (branch `uefi.lnx.6.0.r49-rel`).
- `Containerfile`, `scripts/build-bootloader.sh` — reproducible podman build.

## Build

```sh
OUT=/path/to/output scripts/build-bootloader.sh
# e.g.
OUT=/home/wyb/Documents/mainline/build/bootloader scripts/build-bootloader.sh
```

`BOOTLOADER_TARGET=DEBUG` for a debug build; `REBUILD_IMAGE=1` to rebuild the
builder image. Output goes to `$OUT` (`build/bootloader` inside this repo if
OUT is unset): `BootApp.efi`, `TestBootApp.efi`, `buildinfo.txt`.

The EFI build runs inside `localhost/mainline-bootloader-builder:v1`
(base image pinned by digest in the `Containerfile`), with the network
disabled and `SOURCE_DATE_EPOCH` pinned to the meta-repo commit timestamp,
so rebuilds from the same tree are byte-identical. `buildinfo.txt` records
the meta-repo and submodule commit hashes (plus diffs when dirty), the
container image id and the in-container toolchain versions.
