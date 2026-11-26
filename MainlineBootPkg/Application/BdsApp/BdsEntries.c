/** @file
  UEFI boot variables and image launch support adapted from Canoe SuperFbEntries.

  BootOrder membership is independent of Boot#### existence. Descriptions,
  device paths and optional data belong to the standard UEFI load option.

  Copyright (c) 2026, contributors to the canoe ABL tree.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "BdsApp.h"
#include "BdsEntries.h"
#include "BdsMenu.h"

#include <Guid/GlobalVariable.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/SfbSecurity.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/Security.h>
#include <Protocol/Security2.h>

EFI_STATUS
BdsReadBootOrder (
  OUT UINT16  **Order,
  OUT UINTN   *Count
  )
{
  EFI_STATUS  Status;
  UINTN       Size;
  UINTN       Capacity;
  UINTN       Attempt;
  UINT16      *Buffer;

  if ((Order == NULL) || (Count == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Order = NULL;
  *Count = 0;
  for (Attempt = 0; Attempt < 3; Attempt++) {
    Size = 0;
    Status = gRT->GetVariable (L"BootOrder", &gEfiGlobalVariableGuid, NULL, &Size, NULL);
    if ((Status == EFI_NOT_FOUND) || (!EFI_ERROR (Status) && (Size == 0))) {
      return EFI_SUCCESS;
    }

    if (Status != EFI_BUFFER_TOO_SMALL) {
      return EFI_ERROR (Status) ? Status : EFI_COMPROMISED_DATA;
    }

    if ((Size == 0) || ((Size % sizeof (UINT16)) != 0) ||
        (Size > 0x10000 * sizeof (UINT16))) {
      return EFI_COMPROMISED_DATA;
    }

    Capacity = Size;
    Buffer = AllocatePool (Capacity);
    if (Buffer == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    Status = gRT->GetVariable (L"BootOrder", &gEfiGlobalVariableGuid, NULL, &Size, Buffer);
    if (Status == EFI_BUFFER_TOO_SMALL) {
      FreePool (Buffer);
      continue;
    }

    if (EFI_ERROR (Status)) {
      FreePool (Buffer);
      return (Status == EFI_NOT_FOUND) ? EFI_SUCCESS : Status;
    }

    if ((Size > Capacity) || ((Size % sizeof (UINT16)) != 0)) {
      FreePool (Buffer);
      return EFI_COMPROMISED_DATA;
    }

    if (Size == 0) {
      FreePool (Buffer);
      return EFI_SUCCESS;
    }

    *Order = Buffer;
    *Count = Size / sizeof (UINT16);
    return EFI_SUCCESS;
  }

  return EFI_NOT_READY;
}

EFI_STATUS
BdsWriteBootOrder (
  IN CONST UINT16  *Order,
  IN UINTN         Count
  )
{
  EFI_STATUS  Status;

  if (((Order == NULL) && (Count != 0)) || (Count > 0x10000)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = gRT->SetVariable (
                  L"BootOrder",
                  &gEfiGlobalVariableGuid,
                  BDS_BOOT_VARIABLE_ATTRIBUTES,
                  Count * sizeof (UINT16),
                  (VOID *)Order
                  );
  return ((Count == 0) && (Status == EFI_NOT_FOUND)) ? EFI_SUCCESS : Status;
}

EFI_STATUS
BdsGetAllBootOptions (
  OUT EFI_BOOT_MANAGER_LOAD_OPTION  **Options,
  OUT UINTN                         *Count
  )
{
  EFI_STATUS                         Status;
  EFI_GUID                           Guid;
  CHAR16                             *Name;
  CHAR16                             *Larger;
  UINTN                              Capacity;
  UINTN                              Size;
  UINTN                              Used;
  UINTN                              Allocated;
  UINTN                              Index;
  UINTN                              Probe;
  UINT16                             Number;
  EFI_BOOT_MANAGER_LOAD_OPTION_TYPE  Type;
  EFI_BOOT_MANAGER_LOAD_OPTION       Option;
  EFI_BOOT_MANAGER_LOAD_OPTION       *List;
  EFI_BOOT_MANAGER_LOAD_OPTION       *Grown;

  if ((Options == NULL) || (Count == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Options = NULL;
  *Count   = 0;
  Capacity = 128 * sizeof (CHAR16);
  Name     = AllocateZeroPool (Capacity);
  if (Name == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (&Guid, sizeof (Guid));
  List      = NULL;
  Used      = 0;
  Allocated = 0;
  while (TRUE) {
    Size   = Capacity;
    Status = gRT->GetNextVariableName (&Size, Name, &Guid);
    if (Status == EFI_BUFFER_TOO_SMALL) {
      if ((Size <= Capacity) || (Size > SIZE_1MB)) {
        Status = EFI_COMPROMISED_DATA;
        break;
      }

      Larger = ReallocatePool (Capacity, Size, Name);
      if (Larger == NULL) {
        Status = EFI_OUT_OF_RESOURCES;
        break;
      }

      Name     = Larger;
      Capacity = Size;
      continue;
    }

    if (Status == EFI_NOT_FOUND) {
      Status = EFI_SUCCESS;
      break;
    }

    if (EFI_ERROR (Status)) {
      break;
    }

    if ((Size < sizeof (CHAR16)) || (Size > Capacity) ||
        ((Size % sizeof (CHAR16)) != 0) ||
        (StrnLenS (Name, Size / sizeof (CHAR16)) == Size / sizeof (CHAR16))) {
      Status = EFI_COMPROMISED_DATA;
      break;
    }

    if (!CompareGuid (&Guid, &gEfiGlobalVariableGuid) ||
        !EfiBootManagerIsValidLoadOptionVariableName (Name, &Type, &Number) ||
        (Type != LoadOptionTypeBoot))
    {
      continue;
    }

    ZeroMem (&Option, sizeof (Option));
    Status = BdsReadBootOption (Number, &Option);
    if (EFI_ERROR (Status)) {
      if (Status == EFI_OUT_OF_RESOURCES) {
        break;
      }

      continue;
    }

    if (Used == Allocated) {
      Grown = ReallocatePool (
                Allocated * sizeof (*List),
                (Allocated + 16) * sizeof (*List),
                List
                );
      if (Grown == NULL) {
        EfiBootManagerFreeLoadOption (&Option);
        Status = EFI_OUT_OF_RESOURCES;
        break;
      }

      List       = Grown;
      Allocated += 16;
    }

    List[Used++] = Option;
  }

  FreePool (Name);
  if (EFI_ERROR (Status)) {
    if (List != NULL) {
      EfiBootManagerFreeLoadOptions (List, Used);
    }

    return Status;
  }

  // Enumeration order is firmware-specific; show a stable Boot#### order.
  for (Index = 1; Index < Used; Index++) {
    Option = List[Index];
    for (Probe = Index;
         (Probe > 0) && (List[Probe - 1].OptionNumber > Option.OptionNumber);
         Probe--)
    {
      List[Probe] = List[Probe - 1];
    }

    List[Probe] = Option;
  }

  *Options = List;
  *Count   = Used;
  return EFI_SUCCESS;
}

EFI_STATUS
BdsSetBootOrderMembership (
  IN UINT16   Number,
  IN BOOLEAN  Included
  )
{
  EFI_STATUS                    Status;
  EFI_BOOT_MANAGER_LOAD_OPTION  Option;
  UINT16                        *Order;
  UINT16                        *NewOrder;
  UINTN                         Count;
  UINTN                         Index;
  UINTN                         Used;
  BOOLEAN                       Present;

  if (Included) {
    Status = BdsReadBootOption (Number, &Option);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    EfiBootManagerFreeLoadOption (&Option);
  }

  Status = BdsReadBootOrder (&Order, &Count);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  NewOrder = AllocatePool ((Count + 1) * sizeof (*NewOrder));
  if (NewOrder == NULL) {
    if (Order != NULL) {
      FreePool (Order);
    }

    return EFI_OUT_OF_RESOURCES;
  }

  Used    = 0;
  Present = FALSE;
  for (Index = 0; Index < Count; Index++) {
    if (Order[Index] == Number) {
      Present = TRUE;
      if (!Included) {
        continue;
      }
    }

    NewOrder[Used++] = Order[Index];
  }

  if (Included && !Present) {
    NewOrder[Used++] = Number;
  }

  Status = ((Included == Present) && (Used == Count)) ? EFI_SUCCESS :
           BdsWriteBootOrder (NewOrder, Used);
  FreePool (NewOrder);
  if (Order != NULL) {
    FreePool (Order);
  }

  return Status;
}

EFI_STATUS
BdsSetBootOptionActive (
  IN UINT16   Number,
  IN BOOLEAN  Active
  )
{
  EFI_STATUS                    Status;
  EFI_BOOT_MANAGER_LOAD_OPTION  Option;

  Status = BdsReadBootOption (Number, &Option);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Only change ACTIVE. Preserve description, optional data and all other bits.
  if (Active) {
    Option.Attributes |= LOAD_OPTION_ACTIVE;
  } else {
    Option.Attributes &= ~LOAD_OPTION_ACTIVE;
  }

  Status = EfiBootManagerLoadOptionToVariable (&Option);
  EfiBootManagerFreeLoadOption (&Option);
  return Status;
}

EFI_STATUS
BdsSetBootNext (
  IN UINT16  Number
  )
{
  EFI_STATUS                    Status;
  EFI_BOOT_MANAGER_LOAD_OPTION  Option;

  Status = BdsReadBootOption (Number, &Option);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  EfiBootManagerFreeLoadOption (&Option);
  return gRT->SetVariable (
                L"BootNext",
                &gEfiGlobalVariableGuid,
                BDS_BOOT_VARIABLE_ATTRIBUTES,
                sizeof (Number),
                &Number
                );
}

EFI_STATUS
BdsDeleteBootOption (
  IN UINT16  Number
  )
{
  EFI_STATUS  Status;
  UINT16      Next;
  UINTN       Size;
  CHAR16      Name[9];

  // Remove references first. If a later write fails, leave a recoverable orphan.
  Size   = sizeof (Next);
  Status = gRT->GetVariable (L"BootNext", &gEfiGlobalVariableGuid, NULL, &Size, &Next);
  if ((Status != EFI_NOT_FOUND) && EFI_ERROR (Status)) {
    return Status;
  }

  if (!EFI_ERROR (Status)) {
    if (Size != sizeof (Next)) {
      return EFI_COMPROMISED_DATA;
    }

    if (Next == Number) {
      Status = gRT->SetVariable (
                      L"BootNext",
                      &gEfiGlobalVariableGuid,
                      BDS_BOOT_VARIABLE_ATTRIBUTES,
                      0,
                      NULL
                      );
      if (EFI_ERROR (Status)) {
        return Status;
      }
    }
  }

  Status = BdsSetBootOrderMembership (Number, FALSE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  UnicodeSPrint (Name, sizeof (Name), L"Boot%04X", Number);
  return gRT->SetVariable (Name, &gEfiGlobalVariableGuid, BDS_BOOT_VARIABLE_ATTRIBUTES, 0, NULL);
}

STATIC
EFI_STATUS
BdsMakeFileOption (
  IN EFI_HANDLE                     Volume,
  IN CONST CHAR16                   *Path,
  IN CONST CHAR16                   *Description,
  OUT EFI_BOOT_MANAGER_LOAD_OPTION  *Option
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;

  if ((Volume == NULL) || (Path == NULL) || (Description == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  DevicePath = FileDevicePath (Volume, (CHAR16 *)Path);
  if (DevicePath == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  if (BdsIsSelfDevicePath (DevicePath)) {
    FreePool (DevicePath);
    return EFI_ACCESS_DENIED;
  }

  Status = EfiBootManagerInitializeLoadOption (
             Option,
             LoadOptionNumberUnassigned,
             LoadOptionTypeBoot,
             LOAD_OPTION_ACTIVE,
             (CHAR16 *)Description,
             DevicePath,
             NULL,
             0
             );
  FreePool (DevicePath);
  if (!EFI_ERROR (Status) && ((Option->Description == NULL) || (Option->FilePath == NULL))) {
    EfiBootManagerFreeLoadOption (Option);
    Status = EFI_OUT_OF_RESOURCES;
  }

  return Status;
}

EFI_STATUS
BdsCreateFileBootOption (
  IN EFI_HANDLE    Volume,
  IN CONST CHAR16  *Path,
  IN CONST CHAR16  *Description
  )
{
  EFI_STATUS                    Status;
  EFI_STATUS                    CleanupStatus;
  EFI_BOOT_MANAGER_LOAD_OPTION  Option;
  EFI_BOOT_MANAGER_LOAD_OPTION  *Existing;
  UINTN                         Count;
  UINTN                         Index;
  UINTN                         Number;
  UINTN                         Size;
  CHAR16                        Name[9];

  ZeroMem (&Option, sizeof (Option));
  Status = BdsMakeFileOption (Volume, Path, Description, &Option);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = BdsGetAllBootOptions (&Existing, &Count);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  for (Index = 0; Index < Count; Index++) {
    if ((GetDevicePathSize (Existing[Index].FilePath) == GetDevicePathSize (Option.FilePath)) &&
        (CompareMem (Existing[Index].FilePath, Option.FilePath, GetDevicePathSize (Option.FilePath)) == 0) &&
        (Existing[Index].OptionalDataSize == 0))
    {
      Status = BdsSetBootOrderMembership ((UINT16)Existing[Index].OptionNumber, TRUE);
      EfiBootManagerFreeLoadOptions (Existing, Count);
      goto Done;
    }
  }

  if (Existing != NULL) {
    EfiBootManagerFreeLoadOptions (Existing, Count);
  }

  // Probe variables too: malformed Boot#### must never be silently overwritten.
  for (Number = 0; Number < LoadOptionNumberMax; Number++) {
    UnicodeSPrint (Name, sizeof (Name), L"Boot%04X", (UINT16)Number);
    Size   = 0;
    Status = gRT->GetVariable (Name, &gEfiGlobalVariableGuid, NULL, &Size, NULL);
    if (Status == EFI_NOT_FOUND) {
      break;
    }

    if (EFI_ERROR (Status) && (Status != EFI_BUFFER_TOO_SMALL)) {
      goto Done;
    }
  }

  if (Number == LoadOptionNumberMax) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Done;
  }

  Option.OptionNumber = Number;
  Status              = EfiBootManagerLoadOptionToVariable (&Option);
  if (!EFI_ERROR (Status)) {
    Status = BdsSetBootOrderMembership ((UINT16)Number, TRUE);
    if (EFI_ERROR (Status)) {
      CleanupStatus = gRT->SetVariable (Name, &gEfiGlobalVariableGuid, BDS_BOOT_VARIABLE_ATTRIBUTES, 0, NULL);
      if (EFI_ERROR (CleanupStatus)) {
        DEBUG ((DEBUG_ERROR, "BDS: Boot%04x remains orphaned: %r\n", Number, CleanupStatus));
      }
    }
  }

Done:
  EfiBootManagerFreeLoadOption (&Option);
  return Status;
}

STATIC EFI_SECURITY_ARCH_PROTOCOL              *mSecurity;
STATIC EFI_SECURITY2_ARCH_PROTOCOL             *mSecurity2;
STATIC EFI_SECURITY_FILE_AUTHENTICATION_STATE  mOriginalState;
STATIC EFI_SECURITY2_FILE_AUTHENTICATION       mOriginalAuthentication;
STATIC BOOLEAN                                 mEnforceFirmwarePolicy;
STATIC UINTN                                   mPolicyDepth;

STATIC
EFI_STATUS
EFIAPI
BdsAuthenticateState (
  IN CONST EFI_SECURITY_ARCH_PROTOCOL  *This,
  IN UINT32                            AuthenticationStatus,
  IN CONST EFI_DEVICE_PATH_PROTOCOL    *File
  )
{
  if (mEnforceFirmwarePolicy && (mOriginalState != NULL)) {
    return mOriginalState (This, AuthenticationStatus, File);
  }

  // SECURITY2 verifies the exact buffer supplied to LoadImage.
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
BdsAuthenticateImage (
  IN CONST EFI_SECURITY2_ARCH_PROTOCOL  *This,
  IN CONST EFI_DEVICE_PATH_PROTOCOL     *DevicePath,
  IN VOID                               *FileBuffer,
  IN UINTN                              FileSize,
  IN BOOLEAN                            BootPolicy
  )
{
  EFI_STATUS  Status;

  if (mEnforceFirmwarePolicy && (mOriginalAuthentication != NULL)) {
    Status = mOriginalAuthentication (This, DevicePath, FileBuffer, FileSize, BootPolicy);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  if ((FileBuffer == NULL) || (FileSize == 0)) {
    return SfbSecurityVerifyEnabled () ? EFI_SECURITY_VIOLATION : EFI_SUCCESS;
  }

  Status = SfbSecurityVerifyImage (FileBuffer, FileSize);
  return (Status == EFI_UNSUPPORTED) ? EFI_SECURITY_VIOLATION : Status;
}

EFI_STATUS
BdsBeginImagePolicy (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT8       SecureBoot;
  UINTN       Size;

  if (mPolicyDepth != 0) {
    mPolicyDepth++;
    return EFI_SUCCESS;
  }

  mSecurity               = NULL;
  mSecurity2              = NULL;
  mOriginalState          = NULL;
  mOriginalAuthentication = NULL;
  Size                    = sizeof (SecureBoot);
  Status                  = gRT->GetVariable (L"SecureBoot", &gEfiGlobalVariableGuid, NULL, &Size, &SecureBoot);
  mEnforceFirmwarePolicy  = (BOOLEAN)(Status != EFI_NOT_FOUND);
  if (!EFI_ERROR (Status) && (Size == sizeof (SecureBoot)) && (SecureBoot == 0)) {
    mEnforceFirmwarePolicy = FALSE;
  }

  Status = gBS->LocateProtocol (&gEfiSecurity2ArchProtocolGuid, NULL, (VOID **)&mSecurity2);
  if (EFI_ERROR (Status) || (mSecurity2 == NULL)) {
    mSecurity2 = NULL;
    if (SfbSecurityVerifyEnabled ()) {
      return EFI_UNSUPPORTED;
    }
  }

  Status = gBS->LocateProtocol (&gEfiSecurityArchProtocolGuid, NULL, (VOID **)&mSecurity);
  if (EFI_ERROR (Status)) {
    mSecurity = NULL;
  }

  if (mSecurity != NULL) {
    mOriginalState                     = mSecurity->FileAuthenticationState;
    mSecurity->FileAuthenticationState = BdsAuthenticateState;
  }

  if (mSecurity2 != NULL) {
    mOriginalAuthentication        = mSecurity2->FileAuthentication;
    mSecurity2->FileAuthentication = BdsAuthenticateImage;
  }

  mPolicyDepth = 1;
  return EFI_SUCCESS;
}

VOID
BdsEndImagePolicy (
  VOID
  )
{
  if ((mPolicyDepth == 0) || (--mPolicyDepth != 0)) {
    return;
  }

  if (mSecurity != NULL) {
    mSecurity->FileAuthenticationState = mOriginalState;
  }

  if (mSecurity2 != NULL) {
    mSecurity2->FileAuthentication = mOriginalAuthentication;
  }

  mSecurity  = NULL;
  mSecurity2 = NULL;
}

EFI_STATUS
BdsBootFile (
  IN EFI_HANDLE    Volume,
  IN CONST CHAR16  *Path,
  IN CONST CHAR16  *Description
  )
{
  EFI_STATUS                    Status;
  EFI_BOOT_MANAGER_LOAD_OPTION  Option;
  EFI_LOADED_IMAGE_PROTOCOL     *Loaded;
  EFI_HANDLE                    ImageHandle;
  CHAR16                        *ExitData;
  UINTN                         ExitDataSize;

  ZeroMem (&Option, sizeof (Option));
  Status = BdsMakeFileOption (Volume, Path, Description, &Option);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = BdsBeginImagePolicy ();
  if (EFI_ERROR (Status)) {
    EfiBootManagerFreeLoadOption (&Option);
    return Status;
  }

  // Manual browser launch is transient: do not create a Boot#### variable or
  // invoke this old boot manager's variable-normalizing transient fallback.
  SfbShowBootingScreen (Description, TRUE);
  ImageHandle = NULL;
  Status      = gBS->LoadImage (FALSE, gBdsImageHandle, Option.FilePath, NULL, 0, &ImageHandle);
  if (EFI_ERROR (Status)) {
    if (ImageHandle != NULL) {
      gBS->UnloadImage (ImageHandle);
    }
  } else {
    Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
    if (EFI_ERROR (Status) || (Loaded == NULL) || (Loaded->ImageCodeType != EfiLoaderCode)) {
      gBS->UnloadImage (ImageHandle);
      Status = EFI_UNSUPPORTED;
    } else {
      ExitData     = NULL;
      ExitDataSize = 0;
      Status       = gBS->StartImage (ImageHandle, &ExitDataSize, &ExitData);
      if (ExitData != NULL) {
        FreePool (ExitData);
      }
    }
  }

  BdsEndImagePolicy ();
  EfiBootManagerFreeLoadOption (&Option);
  return Status;
}

EFI_STATUS
BdsLoadDriver (
  IN EFI_HANDLE    Volume,
  IN CONST CHAR16  *Path
  )
{
  EFI_STATUS                 Status;
  EFI_DEVICE_PATH_PROTOCOL   *DevicePath;
  EFI_HANDLE                 ImageHandle;
  EFI_LOADED_IMAGE_PROTOCOL  *Loaded;
  CHAR16                     *ExitData;
  UINTN                      ExitDataSize;

  if ((Volume == NULL) || (Path == NULL) || (Path[0] == L'\0')) {
    return EFI_INVALID_PARAMETER;
  }

  DevicePath = FileDevicePath (Volume, (CHAR16 *)Path);
  if (DevicePath == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  if (BdsIsSelfDevicePath (DevicePath)) {
    FreePool (DevicePath);
    return EFI_ACCESS_DENIED;
  }

  Status = BdsBeginImagePolicy ();
  if (EFI_ERROR (Status)) {
    FreePool (DevicePath);
    return Status;
  }

  ImageHandle = NULL;
  Status      = gBS->LoadImage (FALSE, gBdsImageHandle, DevicePath, NULL, 0, &ImageHandle);
  FreePool (DevicePath);
  if (EFI_ERROR (Status)) {
    if (ImageHandle != NULL) {
      gBS->UnloadImage (ImageHandle);
    }

    BdsEndImagePolicy ();
    return Status;
  }

  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (EFI_ERROR (Status) || (Loaded == NULL) ||
      ((Loaded->ImageCodeType != EfiBootServicesCode) &&
       (Loaded->ImageCodeType != EfiRuntimeServicesCode)))
  {
    gBS->UnloadImage (ImageHandle);
    BdsEndImagePolicy ();
    return EFI_UNSUPPORTED;
  }

  ExitData     = NULL;
  ExitDataSize = 0;
  Status       = gBS->StartImage (ImageHandle, &ExitDataSize, &ExitData);
  BdsEndImagePolicy ();
  if (ExitData != NULL) {
    FreePool (ExitData);
  }

  if (!EFI_ERROR (Status)) {
    SfbConnectAll ();
  }

  return Status;
}
