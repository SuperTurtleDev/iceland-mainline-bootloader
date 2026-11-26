/** @file
  UEFI boot-variable management and Canoe image authentication adapter.

  Copyright (c) 2026, contributors to the canoe ABL tree.
  SPDX-License-Identifier: BSD-3-Clause
**/

#ifndef MAINLINE_BDS_ENTRIES_H_
#define MAINLINE_BDS_ENTRIES_H_

#include <Uefi.h>
#include <Library/UefiBootManagerLib.h>

#define BDS_BOOT_VARIABLE_ATTRIBUTES  (EFI_VARIABLE_NON_VOLATILE | \
                                       EFI_VARIABLE_BOOTSERVICE_ACCESS | \
                                       EFI_VARIABLE_RUNTIME_ACCESS)

// Includes valid orphan Boot#### variables. Free with the boot-manager library.
EFI_STATUS
BdsGetAllBootOptions (
  OUT EFI_BOOT_MANAGER_LOAD_OPTION  **Options,
  OUT UINTN                         *Count
  );

EFI_STATUS
BdsReadBootOrder (
  OUT UINT16  **Order,
  OUT UINTN   *Count
  );

EFI_STATUS
BdsWriteBootOrder (
  IN CONST UINT16  *Order,
  IN UINTN         Count
  );

EFI_STATUS
BdsSetBootOrderMembership (
  IN UINT16   Number,
  IN BOOLEAN  Included
  );

EFI_STATUS
BdsSetBootOptionActive (
  IN UINT16   Number,
  IN BOOLEAN  Active
  );

EFI_STATUS
BdsSetBootNext (
  IN UINT16  Number
  );

EFI_STATUS
BdsDeleteBootOption (
  IN UINT16  Number
  );

EFI_STATUS
BdsCreateFileBootOption (
  IN EFI_HANDLE    Volume,
  IN CONST CHAR16  *Path,
  IN CONST CHAR16  *Description
  );

EFI_STATUS
BdsBootFile (
  IN EFI_HANDLE    Volume,
  IN CONST CHAR16  *Path,
  IN CONST CHAR16  *Description
  );

EFI_STATUS
BdsLoadDriver (
  IN EFI_HANDLE    Volume,
  IN CONST CHAR16  *Path
  );

// Temporary, nestable policy scope. Every successful Begin requires an End.
EFI_STATUS
BdsBeginImagePolicy (
  VOID
  );

VOID
BdsEndImagePolicy (
  VOID
  );

#endif
