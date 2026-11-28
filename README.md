# mainline bootloader

Bootloader meta repository for the mainline-Linux device port.

Layout:

- `MainlineBootPkg/` — UEFI applications: `BdsApp` is the boot manager and
  firmware-volume payload; `BootApp` is the Linux loader installed on the ESP as
  `BOOTAA64.EFI`; `TestBootApp` is the RAM-only USB test front-end. The package
  also contains the container-side build script (`MainlineBootPkg/build.sh`).
- `edk2/` — git submodule, codelinaro `la/abl/tianocore/edk2`
  (branch `uefi.lnx.6.0.r49-rel`).
- `build.sh` — host entry point; the EFI build runs inside the shared
  provisioned container from `../podman_container/runin.sh`.

## Build

```sh
git -C edk2 submodule update --init CryptoPkg/Library/OpensslLib/openssl
./build.sh            # -> ../../build/bootloader/{BdsApp,BootApp,TestBootApp}.efi
```

`OUT=<dir>` (or `./build.sh <dir>`) selects the output directory; unset it
defaults to `../../build/bootloader` relative to this repository. Other
knobs: `BOOTLOADER_TARGET=DEBUG`, `BOOTLOADER_CLEAN=1` (wipe `OUT/build`).

Outputs: `BdsApp.efi`, `BootApp.efi`, `TestBootApp.efi`, `BootUtil.efi` (on-screen probe
of KernelBaseAddr/KernelSize variables, the ABL boot window and the max
initrd capacity - run it from the boot menu), `esp.img` (FAT32, 1 GiB,
Android sparse image carrying `/EFI/BOOT/BOOTAA64.EFI` = BootApp.efi, for
fastboot flashing to the ESP partition), `symbols/` (link-time ELF `.dll`
with DWARF + linker maps — the `.efi` carry no symbol table) and
`buildinfo.txt` (meta-repo/submodule/container commit hashes, dirty diffs,
toolchain versions, artifact hashes).

The build runs `--network=none` with `SOURCE_DATE_EPOCH` pinned to the
meta-repo commit timestamp; clean rebuilds (`BOOTLOADER_CLEAN=1`) from the
same tree are byte-identical. Incremental state (source copy, BaseTools,
edk2 Build tree) persists in `OUT/build` for fast iteration; the edk2
submodule commit is stamped and a bump refreshes the source copy.

## BdsApp

`BdsApp` is an application-mode fork of edk2's BdsDxe. It consumes `BootNext`
once, then processes active boot-category entries in `BootOrder`. It uses
`UefiBootManagerLib` for load-option decoding, default-file device-path expansion,
`BootCurrent`, ReadyToBoot, and image execution. Automatic discovery examines
standard GPT EFI System Partitions and their architecture-default loader
(`\\EFI\\BOOT\\BOOTAA64.EFI` on AArch64). It does not read canoe's `ENTRIES.CFG`
or use a partition's name as proof that it is an ESP. Existing options keep their
order; malformed options are skipped rather than silently deleted.

Hold Volume Up during the one-second entry window to open **UEFI Boot Menu**.
`PlatformBdsInit()` also consumes `OsIndications` bit `0x1` (`BOOT_TO_FW_UI`),
clearing that bit while preserving unrelated requests and entering **BIOS Setup**
directly. Boot Menu offers standard boot options and a BIOS Setup entry. Setup contains **Boot Order**, **Boot Entries**,
the file browser, USB mass-storage export and fastboot. Boot Order membership is
separate from Boot#### existence; removing membership does not delete the option.
Boot Entries includes orphan options, active-state control, one-shot BootNext,
explicit deletion and creation from a selected file.

The canoe BDS platform services are retained: its FAT configuration container,
USB partition/LUN/container export, memory-map fixups and EFI memory-attribute
protocol support. Image verification uses edk2 DxeImageVerificationLib and
BaseCryptLib; authenticated key storage remains with firmware/TZ. Fastboot
challenge authorization uses the standard db/dbx databases. The Ext4 filesystem
driver is not included. Configuration still lives in the FAT container beginning
1 MiB into the GPT partition named `efisp`; that name selects configuration only,
not boot discovery. The leading MiB is reserved for the raw BDS EFI image, and the
build rejects a BdsApp image that would overlap the container. An absent optional
container allows boot without its configuration; a known configuration or
certificate read error must not silently turn a configured policy off.

BdsApp is launched by ABL and its entry point never returns. Exhausted boot
attempts and returning boot applications lead back to Boot Menu. There is no
private active-instance protocol or BDS architectural-protocol dependency.
Install BdsApp in the BDS image slot; the generated ESP retains BootApp as its
fallback Linux loader. The build does not flash a device.

Boot-manager progress and diagnostics use `DEBUG()` only, exactly like BootApp.
The ABL report-status-code handler records that stream in the LOGFS partition, so
an automatic boot leaves the console silent; the console carries the platform UI
and the fatal case where no menu remains.

The package-local boot library is restricted to boot-option operations. It does
not run DXE dispatch, driver-health repair, firmware console/UI/hotkey setup,
recovery policy or host memory-budget adjustment. Platform initialization stays
with QcomBds/ABL; canoe's explicitly requested facilities live in BdsPlatform.

## Security settings

BdsApp reads the firmware's actual global `SecureBoot` value at startup and
replaces the methods in the existing Security/Security2 protocol objects before
connecting devices. `SecureBoot=1` selects this application's standard edk2 image
verifier; `SecureBoot=0` selects an always-pass stub. Missing or malformed status
fails closed. `SetupMode` does not override the selected policy. The legacy
Security notification is handled locally after Security2 image verification.
Neither QCOM's security initializer nor either original firmware authentication
callback is invoked. The host variable services remain responsible for
PK/KEK/db/dbx authentication and persistence.

There is no Secure Boot toggle in BdsApp and it does not read or write
`SecureBootEnable`. `BdsSecurityInitialize()` is application-local and runs once;
recursive and repeated calls do not reinstall hooks or execute platform policy.
The local dispatcher selects its path from the startup status; the linked stock
edk2 verification library retains its standard variable checks.

BIOS Setup retains **Secure Fastboot** through `EnableSecureFastboot`, GUID
`ae98d750-ef2e-49de-9a7d-ad808e1be127`. It is UINT8 `NV|BS` without runtime access.
Missing means FALSE; reading an absent switch does not create it. PK, KEK and db
must be provisioned before it can be enabled; missing key databases reset an
enabled setting to FALSE. Read errors or malformed data are reported rather
than treated as missing keys.

Secure Fastboot is independent of image-verifier readiness. When OFF, Setup
storage tools offer their normal read-only/read-write choice. When ON, USB mass
storage, volume storage tools and config-container exports are unavailable,
including read-only exports. Fastboot flash/erase/fetch retain their separate
one-shot db/dbx challenge authorization. A successful fastboot challenge does
not enable the Setup storage tools or bypass EFI image verification.

## Fastboot UEFI variables

These OEM commands use UEFI runtime services and the existing canoe authorization
gate. Plain `fastboot getvar` remains the ordinary fastboot device-information
interface.

```sh
fastboot oem getvar all
fastboot oem getvar BootOrder
fastboot oem getvar 8be4df61-93ca-11d2-aa0d-00e098032b8c-BootOrder
fastboot oem setvar a52e7a10-7259-4bd1-8a64-c340b7c26239-Example u32:42
fastboot oem setvar a52e7a10-7259-4bd1-8a64-c340b7c26239-Volatile attrs=BS,RT utf16:hello
fastboot oem rmvar Example
fastboot oem rmvar a52e7a10-7259-4bd1-8a64-c340b7c26239-Volatile
```

Values require one of `hex:`, `utf8:`, `utf16:`, `u8:`, `u16:`, `u32:` or `u64:`.
Integers accept decimal or `0x` hexadecimal and are stored little-endian. Strings
include a terminating NUL. `hex:` is an even-length, nonempty sequence of raw
bytes. Empty writes are rejected; deletion is explicit through `rmvar`.

New variables default to `NV,BS,RT`. `attrs=BS`, `attrs=BS,RT`, `attrs=NV,BS` and
`attrs=NV,BS,RT` select volatile/nonvolatile and access properties. Existing
attributes are preserved when omitted. Changing existing attributes is subject
to the firmware's normal rules. `auth:download` submits the completed fastboot
download buffer unchanged to the firmware for authenticated updates; `attrs=AUTH`
selects time-based authentication and `APPEND` selects append-write. Standard
PK/KEK/db/dbx names default to authenticated attributes. Signed deletion uses an
authenticated empty payload, not `rmvar`. Runtime access requires boot-service
access. Generic commands cannot write/remove EnableSecureFastboot; use its
dedicated setup control. SecureBoot and the other firmware mode/status variables
remain protected from generic modification.

`getvar Name` returns every matching GUID. `rmvar Name` requires exactly one match;
otherwise specify `guid-Name`. Names with spaces are accepted by get/remove;
set requires literal quotes around the qualified name, e.g.
`fastboot oem setvar '\"guid-Name With Spaces\"' utf8:value`.

Each read result uses bounded INFO records: `begin guid=...`, `name16:` UTF-16
code units, readable `name:` UTF-8 fragments, attributes/size, `hex@OFFSET:` byte
chunks, then `end`. Concatenate the hex chunks by offset to obtain a value usable
as `hex:`. No INFO payload exceeds 60 bytes. Device command input is bounded at
4096 bytes and is rejected rather than truncated; host-client limits may differ.

## Validation

See [QEMU test instructions](MainlineBootPkg/Tests/BdsApp/README.md). The test runner
replaces QEMU's standard BDS with a fixed-entry test driver before running
BdsApp, so the outer firmware never consumes BootNext or scans boot options.
The suite covers ESP discovery, boot ordering, persistence, setup navigation,
return/failure-to-menu behavior and production variable command parsing against
real UEFI runtime services. Qualcomm USB transfer and board-specific memory
protocol behavior require hardware; stock QEMU does not provide those protocols.
