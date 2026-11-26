# BdsApp implementation

## Scope

Fork the vendored edk2 BdsDxe boot flow as a UEFI application. Keep standard UEFI
BootNext, BootOrder, Boot#### and ESP default-file discovery. Port only the canoe
BDS image's platform facilities, retaining its FAT configuration container,
EFI-stub memory fixups/protocol completion, certificate policy, browser, USB
storage export and existing fastboot implementation. Exclude Ext4 and the custom
canoe boot-entry format/discovery. Boot Menu is the UEFI boot selector; BIOS Setup
replaces Advanced. New fastboot OEM variable management follows infrastructure
integration and QEMU verification.

## Sequence and ownership

1. Inspect and back up the uncommitted VarTestApp experiment; restore a clean
   tracked baseline. Completed; backup outside repository.
2. Fork BdsApp core and standard boot interfaces. Completed; preserve the original
   edk2 submodule and consume UefiBootManagerLib through package mappings.
3. Port canoe FAT container, memory, security, USB and menu infrastructure.
   Integrate build output, preserve source licenses, apply EDK2 formatting.
4. Implement Boot Order and Boot Entries management over standard variables.
   PlatformBdsInit owns the entry key and OsIndications BOOT_TO_FW_UI (0x1),
   which enters BIOS Setup. Preserve unrelated indication bits.
5. Build AArch64 EFI_APPLICATION and run real QEMU against genuine GPT ESP media.
   Verify discovery, boot, BootNext, BootOrder, bad-entry behavior and menu entry.
6. Add OEM getvar all/Name/guid-Name, setvar guid-Name Value, and rmvar
   Name/guid-Name to the imported fastboot command registry. Creation supports
   typed values and explicit UEFI attributes. Test production parsing and real
   UEFI variable services; document hardware-only USB transport coverage.

Independent work runs in background subagents with disjoint file ownership.
User decisions: original FAT container retained; no Ext4 driver; typed value
prefixes; OsIndications flag is 0x1; variable creation permits a type. No flash,
commit or push is authorized by this implementation task.

## Acceptance

- BdsApp PE subsystem is EFI_APPLICATION; no BDS architectural protocol required.
- No canoe custom boot-entry store or custom filename scanning remains.
- BootNext is consumed before a child launch; eligible BootOrder options retain
  order and OptionalData, malformed records do not assert or silently disappear.
- Boot Menu and BIOS Setup use the standard variables; order membership and entry
  deletion are distinct operations, and orphan Boot#### entries remain manageable.
- FAT configuration and certificate policy retain their previous meaning;
  malformed configured certificates must not silently disable enforcement.
- Installed platform callbacks retain valid executable code for their lifetime.
- Built image is below the 1 MiB reservation preceding the FAT container.
- QEMU proves actual BdsApp startup, genuine GPT ESP discovery and child execution;
  tests use isolated disk/variable files and report failures, not just compilation.
- OEM variable commands use UEFI runtime services, preserve attributes by default,
  reject ambiguous deletion and malformed input, and emit bounded fastboot output.
- Final validation is rerun on the final build; physical Qualcomm USB behavior is
  not claimed as tested by stock QEMU.
