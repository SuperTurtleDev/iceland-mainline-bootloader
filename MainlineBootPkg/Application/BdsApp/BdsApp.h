/** @file
  Application-mode BDS core and the canoe platform initialization boundary.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef MAINLINE_BDS_APP_H_
#define MAINLINE_BDS_APP_H_

#include <Uefi.h>
#include <Protocol/DevicePath.h>
#include <Library/UefiBootManagerLib.h>

extern EFI_HANDLE  gBdsImageHandle;

EFI_STATUS
EFIAPI
BdsAppEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  );

// Implemented by platform/canoe. First initializes storage then calls
// BdsDiscoverEspBootOptions before its menu. Called again with BdsMenuRequested
// on failure or any child return. EFI_SUCCESS from a forced menu
// means explicit continue with one finite pass.
EFI_STATUS
PlatformBdsInit (
  VOID
  );

// Requires an assigned option matching its registered Boot####. Also usable
// for inactive options explicitly selected by BootNext or the menu.
// The caller retains ownership of Option and any library-allocated ExitData.
EFI_STATUS
BdsBootLoadOption (
  IN OUT EFI_BOOT_MANAGER_LOAD_OPTION  *Option
  );

EFI_STATUS
BdsReadBootOption (
  IN  UINT16                        Number,
  OUT EFI_BOOT_MANAGER_LOAD_OPTION  *Option
  );

// Preserves BootOrder order; skips malformed/missing entries without rewriting
// variables. Free a non-NULL result with EfiBootManagerFreeLoadOptions().
EFI_BOOT_MANAGER_LOAD_OPTION *
BdsGetBootOptions (
  OUT UINTN  *Count
  );

// Connect devices, then append previously unregistered GPT ESP default loaders.
// Call after the platform has initialized its FAT/storage drivers.
EFI_STATUS
BdsDiscoverEspBootOptions (
  VOID
  );

// One finite pass: cached BootNext, then active CATEGORY_BOOT BootOrder entries.
// A successful returning application stops the pass with EFI_SUCCESS.
EFI_STATUS
BdsRunBootSequence (
  VOID
  );

BOOLEAN
BdsIsSelfDevicePath (
  IN EFI_DEVICE_PATH_PROTOCOL  *Path
  );

// A sticky request for the platform UI, valid for the lifetime of this image.
VOID
BdsRequestMenu (
  VOID
  );

BOOLEAN
BdsMenuRequested (
  VOID
  );

// EFI_NOT_STARTED before any attempt; otherwise the most recent boot result.
EFI_STATUS
BdsGetLastBootStatus (
  VOID
  );

#endif
