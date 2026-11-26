# BdsApp QEMU integration tests

The runner replaces the standard AAVMF BdsDxe PE image with `DirectBoot.efi`
in a private firmware copy. This small test-only DXE driver connects devices
and the console, prepares test variables, then directly loads
`\EFI\TEST\BdsApp.efi`. It never executes the firmware's standard BootNext,
BootOrder, discovery, UI or hotkey logic. BdsApp itself remains an EFI application
with no BDS architectural or private guard protocol.

## Requirements

- A prepared edk2 workspace with built BaseTools and this repository available
  through PACKAGES_PATH.
- LLVM/Clang 21, QEMU AArch64, sfdisk, mkfs.vfat, mtools, Python 3 and pexpect.
- `/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd` and `AAVMF_VARS.fd`.

No mounts, loop devices, firmware flashing or root privileges are used. Input
firmware is never modified. `make-direct-firmware.py` recognizes the LZMA-guided
volume and original BdsDxe file, replaces its PE section, updates checksums and
compression size, and rejects unfamiliar layouts. Each run records source,
replacement-driver and output-firmware hashes.

## Build and run

From the repository root, with `EDK_WORKSPACE` pointing to the prepared edk2 copy:

```sh
bash MainlineBootPkg/Tests/BdsApp/build-tests.sh "$EDK_WORKSPACE"
python3 MainlineBootPkg/Tests/BdsApp/run-qemu.py \
  --bds "$EDK_WORKSPACE/Build/MainlineBoot/RELEASE_CLANG35/AARCH64/BdsApp.efi" \
  --test-bin "$EDK_WORKSPACE/Build/BdsAppTest/RELEASE_CLANG35/AARCH64" \
  --output /tmp/bdsapp-qemu-results
```

The output directory must be new and outside the repository. Omit `--output` for
a unique temporary directory. `--cases discovery return-menu` selects cases;
`--timeout` limits each boot. `--firmware-code` specifies the original AAVMF input,
not an already-patched firmware. The runner always creates the direct-entry copy.
There is no QEMU `-kernel` launch and no standard firmware BDS participation.

The test build writes only `Build/BdsAppTest` and does not rebuild the production
app. Its modules are the direct-entry driver, a standalone diagnostic launcher,
and an independent assertion payload. All fixture images, variable stores,
executables and logs are outside the repository.

## Assertions

A genuine GPT fixture contains one or two EFI System Partitions. The scope test
also includes a non-ESP FAT32 partition with a valid fallback file. BdsApp is at
`EFI/TEST/BdsApp.efi`; independent payload copies are at `EFI/BOOT/BOOTAA64.EFI`,
`EFI/TEST/First.efi` and `Second.efi`.

The payload checks the volatile fixture arm, its real LoadedImage.ParentHandle
and the parent's BdsApp file path, BootCurrent, consumed BootNext, preserved
BootOrder prefix, standard GPT fallback paths, and absence of duplicate or
non-ESP registrations. The runner requires the direct-BDS marker, exactly one
BdsApp start, expected boot attempts, payload assertions and clean guest shutdown.
A direct firmware fallback cannot satisfy these assertions.

| Case | Required behavior |
| --- | --- |
| discovery | Missing BootOrder: discover one GPT ESP and execute its default loader. |
| bootnext | Inactive category-APP BootNext outside BootOrder executes first; request already deleted. |
| order-fallback | Missing first file fails, then the second ordered entry executes. |
| skip-invalid-inactive | Skip missing, inactive and category-APP order entries; execute active BOOT entry. |
| bootnext-missing | Delete missing BootNext and continue without reusing its reserved number. |
| esp-scope | Register exactly two ESPs, excluding the visible non-ESP FAT partition. |
| osindications-menu | `0x9` enters BIOS Setup directly; actual key navigation opens Boot Order/Entries and Boot Menu; only `0x1` is consumed. |
| nv-discovery | Across two QEMU processes, BootOrder/Boot#### persist and no duplicate is added. |
| nv-bootnext | BootNext deletion persists; next process executes BootOrder. |
| return-menu | A successful child returns; BdsApp stays in Boot Menu and responds to Power Off. |
| failure-menu | Invalid EFI payload fails; BdsApp stays in Boot Menu and responds to Power Off. |

The two persistence cases reuse only their own VARS file. Every other case starts
with an independent VARS copy. UI navigation uses real serial Up/Down/Enter and
synchronizes on complete input footers; no menu functions are mocked.

Every successful assertion payload also invokes the production
`UefiVariableCommandsLib` against AAVMF runtime services. It checks integer
width/overflow, UTF-8/UTF-16 and raw hex encoding, explicit/default/preserved
attributes, namespace ambiguity, qualified/unqualified deletion, malformed input,
long-name enumeration retries and complete <=60-byte INFO fragments. These tests
cover actual runtime variable operations, not physical USB transport.

## Results

Each run saves `report.json`, binary/firmware hashes, exact commands,
`<case>/fixture.gpt`, `VARS.fd`, raw/clean serial logs, and ordered menu-key actions.
Passing tests shut down through firmware; timeout, unexpected return, missing
markers or wrong BootCurrent fail the run. Failure-to-menu tests deliberately do
not require a successful payload marker.

Optional diagnostic builds accept `-D BDS_TEST_DIAGNOSTICS` and a separate
`-D BDS_TEST_OUT=Build/BdsAppTestDiag`. They add preflight LoadImage/UnloadImage
and a ReadyToBoot file-read trace; final acceptance uses the default build.

QEMU does not expose Qualcomm USB-device/MSD, UFS or RAM-partition protocols.
Physical fastboot/MSD transfers and handset-specific EFI-stub memory behavior
require target hardware. The tests never modify production images or flash a device.
