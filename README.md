# mainline bootloader

Bootloader meta repository for the mainline-Linux device port.

Layout:

- `MainlineBootPkg/` — UEFI bootloader applications (`BootApp` = production
  `BOOTAA64.EFI`, `TestBootApp` = USB fastboot test front-end) plus the
  container-side build script (`MainlineBootPkg/build.sh`).
- `edk2/` — git submodule, codelinaro `la/abl/tianocore/edk2`
  (branch `uefi.lnx.6.0.r49-rel`).
- `build.sh` — host entry point; the EFI build runs inside the shared
  provisioned container from `../podman_container/runin.sh`.

## Build

```sh
./build.sh            # -> ../../build/bootloader/{BootApp,TestBootApp}.efi
```

`OUT=<dir>` (or `./build.sh <dir>`) selects the output directory; unset it
defaults to `../../build/bootloader` relative to this repository. Other
knobs: `BOOTLOADER_TARGET=DEBUG`, `BOOTLOADER_CLEAN=1` (wipe `OUT/build`).

Outputs: `BootApp.efi`, `TestBootApp.efi`, `symbols/` (link-time ELF `.dll`
with DWARF + linker maps — the `.efi` carry no symbol table) and
`buildinfo.txt` (meta-repo/submodule/container commit hashes, dirty diffs,
toolchain versions, artifact hashes).

The build runs `--network=none` with `SOURCE_DATE_EPOCH` pinned to the
meta-repo commit timestamp; clean rebuilds (`BOOTLOADER_CLEAN=1`) from the
same tree are byte-identical. Incremental state (source copy, BaseTools,
edk2 Build tree) persists in `OUT/build` for fast iteration; the edk2
submodule commit is stamped and a bump refreshes the source copy.
