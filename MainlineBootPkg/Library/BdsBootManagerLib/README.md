# Application boot-option library

This library retains the load-option serialization and standard device-path
expansion code from the vendored `MdeModulePkg/Library/UefiBootManagerLib`.
The original edk2 submodule is unchanged.

The application build includes only:

- Boot-option serialization, bounded validation and variable updates.
- Standard full, file-only, HD and USB device-path expansion; existing FV/LoadFile
  options remain loadable, but they are not automatically registered.
- Connection of existing storage drivers through Boot Services.
- BootCurrent, ReadyToBoot, OptionalData, watchdog and LoadImage/StartImage.

There is no BDS architectural/private protocol, HII, driver-health repair,
firmware UI registration, hotkey service, deferred-image dispatch, DXE dispatch,
console reconstruction, legacy boot, recovery policy, memory budgeting, or
broad automatic boot-option refresh. The caller provides an existing assigned
Boot####; the library does not silently create temporary options or prune bad
BootOrder members. ParentHandle is preserved.

Memory budgeting is a host-firmware responsibility. This older edk2 tree has
`EfiMaxMemoryType == 15`, while current AAVMF uses a newer sentinel in its private
memory-statistics table. The original unbounded accounting walk was reproduced
corrupting an unrelated GPT device path. That accounting code is removed, along
with its HOB dependency, rather than disabled by a reset-only PCD.

BdsApp owns the boot policy: consume BootNext once, try active CATEGORY_BOOT
BootOrder entries in order, and enter Boot Menu after exhausted attempts or a
returning successful child. The BdsApp entry point never returns to ABL. The
platform layer separately supplies the requested canoe facilities.
