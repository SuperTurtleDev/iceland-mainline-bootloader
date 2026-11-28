/** @file
  Application-mode fork of MdeModulePkg/Universal/BdsDxe/BdsEntry.c.

  Retains the BootNext/BootOrder portion of BdsEntry and BootBootOptions, with
  one finite pass and a platform UI boundary. The hosting firmware owns DXE,
  architectural protocols, console policy, variable locks and recovery policy.

  Boot-manager progress and diagnostics go through DEBUG(), which the ABL's
  report-status-code handler records in the LOGFS partition.  The console is
  reserved for the platform UI and for the fatal case where no menu is left.

  Copyright (c) 2004 - 2019, Intel Corporation. All rights reserved.<BR>
  (C) Copyright 2016-2019 Hewlett Packard Enterprise Development LP<BR>
  (C) Copyright 2015 Hewlett-Packard Development Company, L.P.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "BdsApp.h"
#include "BdsEntries.h"

#include <Guid/GlobalVariable.h>
#include <Guid/Gpt.h>
#include <Protocol/BlockIo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BdsSecurityLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#define BDS_MAX_VARIABLE_SIZE  SIZE_1MB

EFI_HANDLE  gBdsImageHandle;

STATIC EFI_DEVICE_PATH_PROTOCOL  *mSelfDevicePath;
STATIC EFI_FILE_PROTOCOL         *mSelfFile;
STATIC UINT64                    mSelfFileSize;
STATIC BOOLEAN                   mMenuRequested;
STATIC BOOLEAN                   mBootNextCaptured;
STATIC BOOLEAN                   mBootNextPending;
STATIC BOOLEAN                   mBootNextReserved;
STATIC UINT16                    mBootNext;
STATIC BOOLEAN                   mSequenceRunning;
STATIC EFI_STATUS                mBootNextStatus    = EFI_NOT_STARTED;
STATIC EFI_STATUS                mLastBootStatus    = EFI_NOT_STARTED;
STATIC EFI_STATUS                mEnumerationStatus = EFI_SUCCESS;

VOID
BdsRequestMenu (
  VOID
  )
{
  mMenuRequested = TRUE;
}

BOOLEAN
BdsMenuRequested (
  VOID
  )
{
  return mMenuRequested;
}

EFI_STATUS
BdsGetLastBootStatus (
  VOID
  )
{
  return mLastBootStatus;
}

// Do not use GetVariable2 here: bound allocations and handle a changing size
// without assertions. In particular, never let malformed BootOrder drive an
// unbounded allocation or silently turn a failed read into an empty order.
STATIC
EFI_STATUS
BdsReadVariable (
  IN  CHAR16  *Name,
  OUT VOID    **Data,
  OUT UINTN   *Size
  )
{
  EFI_STATUS  Status;
  UINTN       Required;
  UINTN       Capacity;
  UINTN       Attempt;
  VOID        *Buffer;

  *Data = NULL;
  *Size = 0;
  for (Attempt = 0; Attempt < 2; Attempt++) {
    Required = 0;
    Status   = gRT->GetVariable (Name, &gEfiGlobalVariableGuid, NULL, &Required, NULL);
    if (Status != EFI_BUFFER_TOO_SMALL) {
      return EFI_ERROR (Status) ? Status : EFI_COMPROMISED_DATA;
    }

    if ((Required == 0) || (Required > BDS_MAX_VARIABLE_SIZE)) {
      return EFI_COMPROMISED_DATA;
    }

    Capacity = Required;
    Buffer   = AllocatePool (Capacity);
    if (Buffer == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    Status = gRT->GetVariable (Name, &gEfiGlobalVariableGuid, NULL, &Required, Buffer);
    if (!EFI_ERROR (Status) && (Required != 0) && (Required <= Capacity)) {
      *Data = Buffer;
      *Size = Required;
      return EFI_SUCCESS;
    }

    FreePool (Buffer);
    if (Status != EFI_BUFFER_TOO_SMALL) {
      return EFI_ERROR (Status) ? Status : EFI_COMPROMISED_DATA;
    }
  }

  return EFI_NOT_READY;
}

STATIC
EFI_STATUS
BdsReadCoreBootOrder (
  OUT UINT16  **Order,
  OUT UINTN   *Count
  )
{
  EFI_STATUS  Status;
  UINTN       Size;

  *Count = 0;
  Status = BdsReadVariable (L"BootOrder", (VOID **)Order, &Size);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (((Size % sizeof (UINT16)) != 0) ||
      (Size / sizeof (UINT16) > LoadOptionNumberMax))
  {
    FreePool (*Order);
    *Order = NULL;
    return EFI_COMPROMISED_DATA;
  }

  *Count = Size / sizeof (UINT16);
  return EFI_SUCCESS;
}

// Validate within the serialized variable before calling this revision's boot
// manager parser: its description scan can otherwise read past malformed data.
STATIC
BOOLEAN
BdsValidDevicePath (
  IN EFI_DEVICE_PATH_PROTOCOL  *Path,
  IN UINTN                     Size
  )
{
  UINTN                     Offset;
  UINTN                     Length;
  EFI_DEVICE_PATH_PROTOCOL  *Node;

  for (Offset = 0; (Offset <= Size) && (Size - Offset >= sizeof (*Node)); Offset += Length) {
    Node   = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)Path + Offset);
    Length = DevicePathNodeLength (Node);
    if ((Length < sizeof (*Node)) || (Length > Size - Offset)) {
      return FALSE;
    }

    if (DevicePathType (Node) == END_DEVICE_PATH_TYPE) {
      if (Length != END_DEVICE_PATH_LENGTH) {
        return FALSE;
      }

      if (IsDevicePathEnd (Node)) {
        return TRUE;
      }

      if (DevicePathSubType (Node) != END_INSTANCE_DEVICE_PATH_SUBTYPE) {
        return FALSE;
      }
    } else if (DevicePathType (Node) == MEDIA_DEVICE_PATH) {
      if ((DevicePathSubType (Node) == MEDIA_HARDDRIVE_DP) &&
          (Length != sizeof (HARDDRIVE_DEVICE_PATH)))
      {
        return FALSE;
      }

      if ((DevicePathSubType (Node) == MEDIA_FILEPATH_DP) &&
          ((Length < SIZE_OF_FILEPATH_DEVICE_PATH + sizeof (CHAR16)) ||
           ((Length & 1) != 0) ||
           (ReadUnaligned16 ((UINT16 *)((UINT8 *)Node + Length - sizeof (CHAR16))) != 0)))
      {
        return FALSE;
      }
    } else if (DevicePathType (Node) == MESSAGING_DEVICE_PATH) {
      if ((DevicePathSubType (Node) == MSG_USB_CLASS_DP) &&
          (Length != sizeof (USB_CLASS_DEVICE_PATH)))
      {
        return FALSE;
      }

      if ((DevicePathSubType (Node) == MSG_USB_WWID_DP) &&
          ((Length < sizeof (USB_WWID_DEVICE_PATH) + sizeof (CHAR16)) || ((Length & 1) != 0)))
      {
        return FALSE;
      }
    }
  }

  return FALSE;
}

EFI_STATUS
BdsReadBootOption (
  IN  UINT16                        Number,
  OUT EFI_BOOT_MANAGER_LOAD_OPTION  *Option
  )
{
  CHAR16                    Name[9];
  EFI_STATUS                Status;
  UINT8                     *Data;
  UINTN                     Size;
  UINTN                     Offset;
  UINT16                    PathSize;
  EFI_DEVICE_PATH_PROTOCOL  *Path;

  ZeroMem (Option, sizeof (*Option));
  UnicodeSPrint (Name, sizeof (Name), L"Boot%04x", (UINTN)Number);
  Status = BdsReadVariable (Name, (VOID **)&Data, &Size);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = EFI_COMPROMISED_DATA;
  if (Size < sizeof (UINT32) + sizeof (UINT16) + sizeof (CHAR16)) {
    goto Done;
  }

  PathSize = ReadUnaligned16 ((UINT16 *)(Data + sizeof (UINT32)));
  Offset   = sizeof (UINT32) + sizeof (UINT16);
  while ((Size - Offset >= sizeof (CHAR16)) &&
         (ReadUnaligned16 ((UINT16 *)(Data + Offset)) != 0))
  {
    Offset += sizeof (CHAR16);
  }

  if (Size - Offset < sizeof (CHAR16)) {
    goto Done;
  }

  Offset += sizeof (CHAR16);
  if ((PathSize < END_DEVICE_PATH_LENGTH) || (PathSize > Size - Offset)) {
    goto Done;
  }

  Path = (EFI_DEVICE_PATH_PROTOCOL *)(Data + Offset);
  if (!BdsValidDevicePath (Path, PathSize) || IsDevicePathEnd (Path)) {
    goto Done;
  }

  // Use the standard decoder after bounded validation, keeping the library's
  // load-option layout, ownership and vendor GUID semantics.
  Status = EfiBootManagerVariableToLoadOption (Name, Option);
  if (!EFI_ERROR (Status) &&
      ((Option->Description == NULL) || (Option->FilePath == NULL) ||
       ((Option->OptionalDataSize != 0) && (Option->OptionalData == NULL))))
  {
    EfiBootManagerFreeLoadOption (Option);
    ZeroMem (Option, sizeof (*Option));
    Status = EFI_OUT_OF_RESOURCES;
  }

Done:
  FreePool (Data);
  return Status;
}

EFI_BOOT_MANAGER_LOAD_OPTION *
BdsGetBootOptions (
  OUT UINTN  *Count
  )
{
  EFI_STATUS                    Status;
  UINT16                        *Order;
  UINTN                         OrderCount;
  UINTN                         Index;
  EFI_BOOT_MANAGER_LOAD_OPTION  *Options;

  mEnumerationStatus = EFI_INVALID_PARAMETER;
  if (Count == NULL) {
    return NULL;
  }

  *Count             = 0;
  Status             = BdsReadCoreBootOrder (&Order, &OrderCount);
  mEnumerationStatus = Status;
  if (EFI_ERROR (Status)) {
    if (Status != EFI_NOT_FOUND) {
      DEBUG ((DEBUG_ERROR, "BdsApp: ignoring invalid/unreadable BootOrder: %r\n", Status));
    }

    return NULL;
  }

  Options = AllocateZeroPool (OrderCount * sizeof (*Options));
  if (Options == NULL) {
    FreePool (Order);
    mEnumerationStatus = EFI_OUT_OF_RESOURCES;
    return NULL;
  }

  for (Index = 0; Index < OrderCount; Index++) {
    Status = BdsReadBootOption (Order[Index], &Options[*Count]);
    if (!EFI_ERROR (Status)) {
      (*Count)++;
    } else {
      DEBUG ((DEBUG_WARN, "BdsApp: skipping Boot%04x: %r\n", (UINTN)Order[Index], Status));
      if ((Status == EFI_OUT_OF_RESOURCES) || (Status == EFI_NOT_READY)) {
        mEnumerationStatus = Status;
      }
    }
  }

  FreePool (Order);
  if (*Count == 0) {
    FreePool (Options);
    return NULL;
  }

  return Options;
}

// Snapshot and delete once, before platform initialization or any boot. A new
// BootNext written by platform code is intentionally reserved for the next run.
STATIC
EFI_STATUS
BdsCaptureBootNext (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  DeleteStatus;
  UINTN       Size;
  UINT16      Number;

  if (mBootNextCaptured) {
    return mBootNextStatus;
  }

  mBootNextCaptured = TRUE;
  Size              = sizeof (Number);
  Status            = gRT->GetVariable (L"BootNext", &gEfiGlobalVariableGuid, NULL, &Size, &Number);
  if (Status == EFI_NOT_FOUND) {
    mBootNextStatus = EFI_SUCCESS;
    return EFI_SUCCESS;
  }

  if (!EFI_ERROR (Status) && (Size == sizeof (Number))) {
    mBootNext         = Number;
    mBootNextReserved = TRUE;
  }

  DeleteStatus = gRT->SetVariable (L"BootNext", &gEfiGlobalVariableGuid, 0, 0, NULL);
  if (EFI_ERROR (DeleteStatus) && (DeleteStatus != EFI_NOT_FOUND)) {
    DEBUG ((DEBUG_ERROR, "BdsApp: cannot consume BootNext: %r; automatic boot disabled\n", DeleteStatus));
    mBootNextStatus = DeleteStatus;
    mLastBootStatus = DeleteStatus;
    return DeleteStatus;
  }

  mBootNextPending = mBootNextReserved;
  if (mBootNextPending) {
    DEBUG ((DEBUG_INFO, "BdsApp: BootNext=%04x captured and deleted\n", (UINTN)mBootNext));
  } else {
    DEBUG ((DEBUG_WARN, "BdsApp: malformed/unreadable BootNext discarded (%r, %u bytes)\n", Status, Size));
  }

  mBootNextStatus = EFI_SUCCESS;
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
BdsEqualDevicePaths (
  IN EFI_DEVICE_PATH_PROTOCOL  *Left,
  IN EFI_DEVICE_PATH_PROTOCOL  *Right
  )
{
  UINTN  Size;

  if ((Left == NULL) || (Right == NULL)) {
    return FALSE;
  }

  Size = GetDevicePathSize (Left);
  return (BOOLEAN)((Size == GetDevicePathSize (Right)) && (CompareMem (Left, Right, Size) == 0));
}

STATIC
BOOLEAN
BdsDirectSelfPath (
  IN EFI_DEVICE_PATH_PROTOCOL  *Path
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *Remaining;
  EFI_FILE_PROTOCOL         *File;
  UINT8                     SelfBuffer[4096];
  UINT8                     OtherBuffer[4096];
  UINTN                     SelfSize;
  UINTN                     OtherSize;
  UINT64                    OtherFileSize;
  BOOLEAN                   Same;

  if (BdsEqualDevicePaths (Path, mSelfDevicePath)) {
    return TRUE;
  }

  if (mSelfFile == NULL) {
    return FALSE;
  }

  Remaining = Path;
  Status    = EfiOpenFileByDevicePath (&Remaining, &File, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  OtherFileSize = 0;
  Status        = File->SetPosition (File, MAX_UINT64);
  if (!EFI_ERROR (Status)) {
    Status = File->GetPosition (File, &OtherFileSize);
  }

  if (EFI_ERROR (Status) || (OtherFileSize != mSelfFileSize)) {
    File->Close (File);
    return FALSE;
  }

  Status = File->SetPosition (File, 0);
  if (EFI_ERROR (Status)) {
    File->Close (File);
    return FALSE;
  }

  Same   = FALSE;
  Status = mSelfFile->SetPosition (mSelfFile, 0);
  if (!EFI_ERROR (Status)) {
    for ( ; ;) {
      SelfSize  = sizeof (SelfBuffer);
      OtherSize = sizeof (OtherBuffer);
      Status    = mSelfFile->Read (mSelfFile, &SelfSize, SelfBuffer);
      if (EFI_ERROR (Status)) {
        break;
      }

      Status = File->Read (File, &OtherSize, OtherBuffer);
      if (EFI_ERROR (Status) || (SelfSize != OtherSize) ||
          (CompareMem (SelfBuffer, OtherBuffer, SelfSize) != 0))
      {
        break;
      }

      if (SelfSize == 0) {
        Same = TRUE;
        break;
      }
    }
  }

  File->Close (File);
  return Same;
}

BOOLEAN
BdsIsSelfDevicePath (
  IN EFI_DEVICE_PATH_PROTOCOL  *Path
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *FullPath;
  EFI_DEVICE_PATH_PROTOCOL  *Previous;
  EFI_DEVICE_PATH_PROTOCOL  *Node;
  EFI_HANDLE                Handle;
  EFI_STATUS                Status;
  BOOLEAN                   LocalPath;
  BOOLEAN                   Same;
  UINTN                     Iteration;

  if ((Path == NULL) || !BdsValidDevicePath (Path, MAX_UINT16)) {
    return FALSE;
  }

  if (BdsDirectSelfPath (Path)) {
    return TRUE;
  }

  // Resolve local short-form/device-only options just as BootManagerLib does.
  // Do not trigger LoadFile/network discovery merely to test image identity.
  Node      = Path;
  Status    = gBS->LocateDevicePath (&gEfiSimpleFileSystemProtocolGuid, &Node, &Handle);
  LocalPath = (BOOLEAN)(!EFI_ERROR (Status) && IsDevicePathEnd (Node));
  if ((DevicePathType (Path) == MEDIA_DEVICE_PATH) &&
      ((DevicePathSubType (Path) == MEDIA_HARDDRIVE_DP) ||
       (DevicePathSubType (Path) == MEDIA_FILEPATH_DP)))
  {
    LocalPath = TRUE;
  }

  if ((DevicePathType (Path) == MESSAGING_DEVICE_PATH) &&
      ((DevicePathSubType (Path) == MSG_USB_CLASS_DP) ||
       (DevicePathSubType (Path) == MSG_USB_WWID_DP)))
  {
    LocalPath = TRUE;
  }

  if (!LocalPath) {
    Node      = Path;
    Status    = gBS->LocateDevicePath (&gEfiBlockIoProtocolGuid, &Node, &Handle);
    LocalPath = (BOOLEAN)(!EFI_ERROR (Status) && IsDevicePathEnd (Node));
  }

  if (!LocalPath) {
    return FALSE;
  }

  Previous = NULL;
  Same     = FALSE;
  for (Iteration = 0; Iteration < LoadOptionNumberMax; Iteration++) {
    FullPath = EfiBootManagerGetNextLoadOptionDevicePath (Path, Previous);
    if (Previous != NULL) {
      FreePool (Previous);
    }

    Previous = FullPath;
    if (FullPath == NULL) {
      break;
    }

    if (BdsDirectSelfPath (FullPath)) {
      Same = TRUE;
      break;
    }
  }

  if (Previous != NULL) {
    FreePool (Previous);
  }

  return Same;
}

STATIC
HARDDRIVE_DEVICE_PATH *
BdsPartitionNode (
  IN EFI_DEVICE_PATH_PROTOCOL  *Path
  )
{
  HARDDRIVE_DEVICE_PATH  *Partition;

  Partition = NULL;
  if (Path == NULL) {
    return NULL;
  }

  for ( ; !IsDevicePathEnd (Path); Path = NextDevicePathNode (Path)) {
    if ((DevicePathType (Path) == MEDIA_DEVICE_PATH) &&
        (DevicePathSubType (Path) == MEDIA_HARDDRIVE_DP) &&
        (DevicePathNodeLength (Path) == sizeof (*Partition)))
    {
      Partition = (HARDDRIVE_DEVICE_PATH *)Path;
    }
  }

  return Partition;
}

STATIC
BOOLEAN
BdsIsEsp (
  IN EFI_HANDLE  Handle
  )
{
  EFI_STATUS                   Status;
  EFI_PARTITION_INFO_PROTOCOL  *Info;
  HARDDRIVE_DEVICE_PATH        *Partition;
  VOID                         *Marker;

  Status = gBS->HandleProtocol (Handle, &gEfiPartitionInfoProtocolGuid, (VOID **)&Info);
  if (!EFI_ERROR (Status) && (Info != NULL)) {
    return (BOOLEAN)((Info->Type == PARTITION_TYPE_GPT) &&
                     CompareGuid (&Info->Info.Gpt.PartitionTypeGUID, &gEfiPartTypeSystemPartGuid));
  }

  // Older PartitionDxe also installs the GPT type GUID as a marker protocol.
  // A GPT unique-partition GUID in HD() alone does NOT establish ESP type.
  Partition = BdsPartitionNode (DevicePathFromHandle (Handle));
  if ((Partition == NULL) ||
      (Partition->MBRType != MBR_TYPE_EFI_PARTITION_TABLE_HEADER) ||
      (Partition->SignatureType != SIGNATURE_TYPE_GUID))
  {
    return FALSE;
  }

  Status = gBS->HandleProtocol (Handle, &gEfiPartTypeSystemPartGuid, &Marker);
  return (BOOLEAN) !EFI_ERROR (Status);
}

STATIC
BOOLEAN
BdsEqualFileNames (
  IN CHAR16  *Left,
  IN CHAR16  *Right
  )
{
  CHAR16  A;
  CHAR16  B;

  do {
    A = *Left++;
    B = *Right++;
    if ((A >= L'a') && (A <= L'z')) {
      A -= L'a' - L'A';
    }

    if ((B >= L'a') && (B <= L'z')) {
      B -= L'a' - L'A';
    }

    if (A == L'/') {
      A = L'\\';
    }

    if (B == L'/') {
      B = L'\\';
    }

    if (A != B) {
      return FALSE;
    }
  } while (A != L'\0');

  return TRUE;
}

STATIC
BOOLEAN
BdsSameEspTarget (
  IN EFI_DEVICE_PATH_PROTOCOL  *Left,
  IN EFI_DEVICE_PATH_PROTOCOL  *Right
  )
{
  HARDDRIVE_DEVICE_PATH     *LeftHd;
  HARDDRIVE_DEVICE_PATH     *RightHd;
  EFI_DEVICE_PATH_PROTOCOL  *LeftFile;
  EFI_DEVICE_PATH_PROTOCOL  *RightFile;
  CHAR16                    *LeftName;
  CHAR16                    *RightName;

  if (BdsEqualDevicePaths (Left, Right)) {
    return TRUE;
  }

  LeftHd  = BdsPartitionNode (Left);
  RightHd = BdsPartitionNode (Right);
  if ((LeftHd == NULL) || (RightHd == NULL) ||
      (LeftHd->SignatureType != SIGNATURE_TYPE_GUID) ||
      (RightHd->SignatureType != SIGNATURE_TYPE_GUID) ||
      (CompareMem (LeftHd->Signature, RightHd->Signature, sizeof (EFI_GUID)) != 0))
  {
    return FALSE;
  }

  LeftFile  = NextDevicePathNode (&LeftHd->Header);
  RightFile = NextDevicePathNode (&RightHd->Header);
  LeftName  = EFI_REMOVABLE_MEDIA_FILE_NAME;
  RightName = EFI_REMOVABLE_MEDIA_FILE_NAME;
  if (!IsDevicePathEnd (LeftFile)) {
    if ((DevicePathType (LeftFile) != MEDIA_DEVICE_PATH) ||
        (DevicePathSubType (LeftFile) != MEDIA_FILEPATH_DP) ||
        !IsDevicePathEnd (NextDevicePathNode (LeftFile)))
    {
      return FALSE;
    }

    LeftName = ((FILEPATH_DEVICE_PATH *)LeftFile)->PathName;
  }

  if (!IsDevicePathEnd (RightFile)) {
    if ((DevicePathType (RightFile) != MEDIA_DEVICE_PATH) ||
        (DevicePathSubType (RightFile) != MEDIA_FILEPATH_DP) ||
        !IsDevicePathEnd (NextDevicePathNode (RightFile)))
    {
      return FALSE;
    }

    RightName = ((FILEPATH_DEVICE_PATH *)RightFile)->PathName;
  }

  return BdsEqualFileNames (LeftName, RightName);
}

// Unlike this older library's free-number helper, also preserve orphan Boot####
// variables and the consumed BootNext number. A failed BootNext must never turn
// into an unrelated newly discovered loader occupying the same number.
STATIC
EFI_STATUS
BdsFreeOptionNumber (
  IN  UINT16  *Order,
  IN  UINTN   Count,
  OUT UINT16  *Number
  )
{
  UINTN       Candidate;
  UINTN       Index;
  UINTN       Size;
  UINT16      NextBoot;
  BOOLEAN     HaveNextBoot;
  CHAR16      Name[9];
  EFI_STATUS  Status;

  Size         = sizeof (NextBoot);
  Status       = gRT->GetVariable (L"BootNext", &gEfiGlobalVariableGuid, NULL, &Size, &NextBoot);
  HaveNextBoot = (BOOLEAN)(!EFI_ERROR (Status) && (Size == sizeof (NextBoot)));
  for (Candidate = 0; Candidate < LoadOptionNumberMax; Candidate++) {
    if ((mBootNextReserved && (Candidate == mBootNext)) ||
        (HaveNextBoot && (Candidate == NextBoot)))
    {
      continue;
    }

    for (Index = 0; Index < Count; Index++) {
      if (Order[Index] == Candidate) {
        break;
      }
    }

    if (Index != Count) {
      continue;
    }

    UnicodeSPrint (Name, sizeof (Name), L"Boot%04x", Candidate);
    Size   = 0;
    Status = gRT->GetVariable (Name, &gEfiGlobalVariableGuid, NULL, &Size, NULL);
    if (Status == EFI_NOT_FOUND) {
      *Number = (UINT16)Candidate;
      return EFI_SUCCESS;
    }

    if (EFI_ERROR (Status) && (Status != EFI_BUFFER_TOO_SMALL)) {
      return Status;
    }
  }

  return EFI_OUT_OF_RESOURCES;
}

STATIC
EFI_STATUS
BdsRegisterEspOption (
  IN EFI_DEVICE_PATH_PROTOCOL  *Path
  )
{
  EFI_STATUS                    Status;
  EFI_BOOT_MANAGER_LOAD_OPTION  *Options;
  EFI_BOOT_MANAGER_LOAD_OPTION  Option;
  UINTN                         Count;
  UINTN                         Index;
  UINT16                        *Order;
  UINTN                         OrderCount;
  UINT16                        Number;

  Options = BdsGetBootOptions (&Count);
  Status  = mEnumerationStatus;
  if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) {
    if (Options != NULL) {
      EfiBootManagerFreeLoadOptions (Options, Count);
    }

    return Status;
  }

  for (Index = 0; Index < Count; Index++) {
    if (BdsSameEspTarget (Options[Index].FilePath, Path)) {
      DEBUG ((DEBUG_INFO, "BdsApp: ESP loader already registered as Boot%04x\n", Options[Index].OptionNumber));
      EfiBootManagerFreeLoadOptions (Options, Count);
      return EFI_SUCCESS;
    }
  }

  if (Options != NULL) {
    EfiBootManagerFreeLoadOptions (Options, Count);
  }

  Status = BdsReadCoreBootOrder (&Order, &OrderCount);
  if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) {
    return Status;
  }

  Status = BdsFreeOptionNumber (Order, OrderCount, &Number);
  if (Order != NULL) {
    FreePool (Order);
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = EfiBootManagerInitializeLoadOption (
             &Option,
             Number,
             LoadOptionTypeBoot,
             LOAD_OPTION_ACTIVE | LOAD_OPTION_CATEGORY_BOOT,
             L"UEFI ESP default loader",
             Path,
             NULL,
             0
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Option.Description == NULL) || (Option.FilePath == NULL)) {
    EfiBootManagerFreeLoadOption (&Option);
    return EFI_OUT_OF_RESOURCES;
  }

  // Append, never sort, delete or refresh the user's existing options/order.
  Status = EfiBootManagerAddLoadOptionVariable (&Option, MAX_UINTN);
  DEBUG ((DEBUG_INFO, "BdsApp: append ESP Boot%04x: %r\n", Option.OptionNumber, Status));
  EfiBootManagerFreeLoadOption (&Option);
  return Status;
}

STATIC
EFI_STATUS
BdsConnectDevices (
  VOID
  );

EFI_STATUS
BdsDiscoverEspBootOptions (
  VOID
  )
{
  EFI_STATUS                Status;
  EFI_STATUS                Result;
  EFI_HANDLE                *Handles;
  UINTN                     HandleCount;
  UINTN                     Index;
  UINTN                     EspCount;
  UINTN                     LoaderCount;
  EFI_FILE_PROTOCOL         *File;
  EFI_DEVICE_PATH_PROTOCOL  *Path;
  EFI_DEVICE_PATH_PROTOCOL  *Remaining;
  UINT64                    FileSize;

  Status = BdsConnectDevices ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BdsApp: device connection failed: %r\n", Status));
  }

  // Storage may have become available only during platform initialization.
  if ((mSelfFile == NULL) && (mSelfDevicePath != NULL)) {
    Remaining = mSelfDevicePath;
    Status    = EfiOpenFileByDevicePath (&Remaining, &mSelfFile, EFI_FILE_MODE_READ, 0);
    if (!EFI_ERROR (Status)) {
      Status = mSelfFile->SetPosition (mSelfFile, MAX_UINT64);
      if (!EFI_ERROR (Status)) {
        Status = mSelfFile->GetPosition (mSelfFile, &mSelfFileSize);
      }

      if (EFI_ERROR (Status)) {
        mSelfFile->Close (mSelfFile);
        mSelfFile = NULL;
      }
    } else {
      mSelfFile = NULL;
    }
  }

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BdsApp: no filesystem handles: %r\n", Status));
    return Status;
  }

  Result      = EFI_NOT_FOUND;
  EspCount    = 0;
  LoaderCount = 0;
  for (Index = 0; Index < HandleCount; Index++) {
    if (!BdsIsEsp (Handles[Index])) {
      continue;
    }

    EspCount++;
    DEBUG ((DEBUG_INFO, "BdsApp: GPT ESP %u, checking %s\n", EspCount, EFI_REMOVABLE_MEDIA_FILE_NAME));
    Remaining = DevicePathFromHandle (Handles[Index]);
    if (Remaining == NULL) {
      continue;
    }

    // Reuse BmExpandMediaDevicePath via the public boot-manager API. The ESP
    // type check is the only discovery filter: no custom names, directory
    // scanning, shell/FV enumeration or LoadFile/network auto entries.
    Path = EfiBootManagerGetNextLoadOptionDevicePath (Remaining, NULL);
    if (Path == NULL) {
      Result = EFI_OUT_OF_RESOURCES;
      break;
    }

    Remaining = Path;
    Status    = EfiOpenFileByDevicePath (&Remaining, &File, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "BdsApp: ESP default loader: %r\n", Status));
      FreePool (Path);
      continue;
    }

    // Directories do not support seeking to EOF; reject them and empty files.
    FileSize = 0;
    Status   = File->SetPosition (File, MAX_UINT64);
    if (!EFI_ERROR (Status)) {
      Status = File->GetPosition (File, &FileSize);
    }

    File->Close (File);
    if (EFI_ERROR (Status) || (FileSize == 0)) {
      DEBUG ((DEBUG_WARN, "BdsApp: ESP default path is not a nonempty file\n"));
      FreePool (Path);
      continue;
    }

    if (BdsIsSelfDevicePath (Path)) {
      DEBUG ((DEBUG_INFO, "BdsApp: ESP default loader is BdsApp; skipping recursion\n"));
      FreePool (Path);
      continue;
    }

    LoaderCount++;
    Status = BdsRegisterEspOption (Path);
    FreePool (Path);
    if ((Result == EFI_NOT_FOUND) || EFI_ERROR (Status)) {
      Result = Status;
    }
  }

  FreePool (Handles);
  DEBUG ((DEBUG_INFO, "BdsApp: discovery found %u GPT ESP(s), %u eligible default loader(s): %r\n", EspCount, LoaderCount, Result));
  return Result;
}

STATIC
BOOLEAN
BdsEqualOptions (
  IN EFI_BOOT_MANAGER_LOAD_OPTION  *Left,
  IN EFI_BOOT_MANAGER_LOAD_OPTION  *Right
  )
{
  return (BOOLEAN)((Left->Attributes == Right->Attributes) &&
                   (StrCmp (Left->Description, Right->Description) == 0) &&
                   BdsEqualDevicePaths (Left->FilePath, Right->FilePath) &&
                   (Left->OptionalDataSize == Right->OptionalDataSize) &&
                   ((Left->OptionalDataSize == 0) ||
                    (CompareMem (Left->OptionalData, Right->OptionalData, Left->OptionalDataSize) == 0)));
}

EFI_STATUS
BdsBootLoadOption (
  IN OUT EFI_BOOT_MANAGER_LOAD_OPTION  *Option
  )
{
  EFI_STATUS                    Status;
  EFI_BOOT_MANAGER_LOAD_OPTION  Current;

  Status = BdsCaptureBootNext ();
  if (EFI_ERROR (Status)) {
    mLastBootStatus = Status;
    return Status;
  }

  if ((Option == NULL) || (Option->OptionType != LoadOptionTypeBoot) ||
      (Option->OptionNumber >= LoadOptionNumberMax) ||
      (Option->Description == NULL) || (Option->FilePath == NULL) ||
      ((Option->OptionalDataSize != 0) && (Option->OptionalData == NULL)))
  {
    mLastBootStatus = EFI_INVALID_PARAMETER;
    return EFI_INVALID_PARAMETER;
  }

  // An exact registered option prevents EfiBootManagerBoot's internal fallback
  // to GetLoadOptions(), which in this library revision prunes bad BootOrder
  // entries and can assert when every referenced option is malformed.
  Status = BdsReadBootOption ((UINT16)Option->OptionNumber, &Current);
  if (!EFI_ERROR (Status)) {
    if (!BdsEqualOptions (Option, &Current)) {
      Status = EFI_MEDIA_CHANGED;
    } else if (BdsIsSelfDevicePath (Current.FilePath)) {
      DEBUG ((DEBUG_WARN, "BdsApp: skipping self Boot%04x (%s)\n", Current.OptionNumber, Current.Description));
      Status = EFI_ACCESS_DENIED;
    } else {
      DEBUG ((DEBUG_INFO, "BdsApp: boot Boot%04x %s (attributes 0x%x)\n", Current.OptionNumber, Current.Description, Current.Attributes));
      Status = BdsSecurityReady ();
      if (!EFI_ERROR (Status)) {
        EfiBootManagerBoot (&Current);
        Status = Current.Status;
      }

      if (Option->ExitData != NULL) {
        FreePool (Option->ExitData);
      }

      Option->ExitData     = Current.ExitData;
      Option->ExitDataSize = Current.ExitDataSize;
      DEBUG ((DEBUG_INFO, "BdsApp: Boot%04x returned %r\n", Current.OptionNumber, Status));
    }

    EfiBootManagerFreeLoadOption (&Current);
  }

  Option->Status  = Status;
  mLastBootStatus = Status;
  return Status;
}

STATIC
VOID
BdsFreeExitData (
  IN OUT EFI_BOOT_MANAGER_LOAD_OPTION  *Option
  )
{
  if (Option->ExitData != NULL) {
    FreePool (Option->ExitData);
    Option->ExitData     = NULL;
    Option->ExitDataSize = 0;
  }
}

EFI_STATUS
BdsRunBootSequence (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_STATUS                    Result;
  EFI_BOOT_MANAGER_LOAD_OPTION  Next;
  EFI_BOOT_MANAGER_LOAD_OPTION  *Options;
  UINTN                         Count;
  UINTN                         Index;

  if (mSequenceRunning) {
    return EFI_ALREADY_STARTED;
  }

  Status = BdsCaptureBootNext ();
  if (EFI_ERROR (Status)) {
    mLastBootStatus = Status;
    return Status;
  }

  if (BdsMenuRequested ()) {
    DEBUG ((DEBUG_INFO, "BdsApp: platform requested menu; automatic boot deferred\n"));
    return EFI_ABORTED;
  }

  mSequenceRunning = TRUE;
  Result           = EFI_NOT_FOUND;
  if (mBootNextPending) {
    mBootNextPending = FALSE;
    Status           = BdsReadBootOption (mBootNext, &Next);
    if (!EFI_ERROR (Status)) {
      // BootNext explicitly selects an option, even if it is inactive or APP.
      Result = BdsBootLoadOption (&Next);
      BdsFreeExitData (&Next);
      EfiBootManagerFreeLoadOption (&Next);
      if ((Result == EFI_SUCCESS) || BdsMenuRequested ()) {
        goto Done;
      }
    } else {
      Result = Status;
      DEBUG ((DEBUG_WARN, "BdsApp: BootNext Boot%04x unavailable: %r\n", (UINTN)mBootNext, Status));
    }
  }

  Options = BdsGetBootOptions (&Count);
  if ((Count == 0) && EFI_ERROR (mEnumerationStatus) && (mEnumerationStatus != EFI_NOT_FOUND)) {
    Result = mEnumerationStatus;
  }

  for (Index = 0; Index < Count; Index++) {
    if (((Options[Index].Attributes & LOAD_OPTION_ACTIVE) == 0) ||
        ((Options[Index].Attributes & LOAD_OPTION_CATEGORY) != LOAD_OPTION_CATEGORY_BOOT))
    {
      continue;
    }

    Result = BdsBootLoadOption (&Options[Index]);
    BdsFreeExitData (&Options[Index]);
    if ((Result == EFI_SUCCESS) || BdsMenuRequested ()) {
      break;
    }
  }

  if (Options != NULL) {
    EfiBootManagerFreeLoadOptions (Options, Count);
  }

Done:
  mLastBootStatus  = Result;
  mSequenceRunning = FALSE;
  if (EFI_ERROR (Result)) {
    BdsRequestMenu ();
  }

  DEBUG ((DEBUG_INFO, "BdsApp: finite boot sequence returned %r\n", Result));
  return Result;
}

// Connect existing storage drivers without running the firmware DXE dispatcher.
STATIC
EFI_STATUS
BdsConnectDevices (
  VOID
  )
{
  EFI_HANDLE  *Handles;
  UINTN       Count;
  UINTN       Index;
  EFI_STATUS  Status;

  Status = gBS->LocateHandleBuffer (AllHandles, NULL, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((DEBUG_INFO, "BdsApp: connecting %u existing handles\n", Count));
  for (Index = 0; Index < Count; Index++) {
    gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
  }

  FreePool (Handles);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
BdsAppEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                 Status;
  EFI_STATUS                 BootNextStatus;
  EFI_STATUS                 MenuStatus;
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;
  EFI_DEVICE_PATH_PROTOCOL   *Path;

  gBdsImageHandle = ImageHandle;
  DEBUG ((DEBUG_INFO, "BdsApp: application BDS core starting\n"));

  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  BootNextStatus = BdsCaptureBootNext ();
  Status         = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageDevicePathProtocolGuid, (VOID **)&Path);
  if (!EFI_ERROR (Status) && (Path != NULL)) {
    mSelfDevicePath = DuplicateDevicePath (Path);
  }

  if (mSelfDevicePath == NULL) {
    Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&LoadedImage);
    if (!EFI_ERROR (Status) && (LoadedImage->DeviceHandle != NULL) && (LoadedImage->FilePath != NULL)) {
      Path = DevicePathFromHandle (LoadedImage->DeviceHandle);
      if (Path != NULL) {
        mSelfDevicePath = AppendDevicePath (Path, LoadedImage->FilePath);
      }
    }
  }

  if (mSelfDevicePath != NULL) {
    Path   = mSelfDevicePath;
    Status = EfiOpenFileByDevicePath (&Path, &mSelfFile, EFI_FILE_MODE_READ, 0);
    if (!EFI_ERROR (Status)) {
      Status = mSelfFile->SetPosition (mSelfFile, MAX_UINT64);
      if (!EFI_ERROR (Status)) {
        Status = mSelfFile->GetPosition (mSelfFile, &mSelfFileSize);
      }

      if (EFI_ERROR (Status)) {
        mSelfFile->Close (mSelfFile);
        mSelfFile = NULL;
      }
    } else {
      mSelfFile = NULL;
    }
  }

  DEBUG ((DEBUG_INFO, "BdsApp: self identity: path=%s, file=%s\n", (mSelfDevicePath != NULL) ? L"yes" : L"no", (mSelfFile != NULL) ? L"yes" : L"no"));

  // Platform initialization opens the key window, installs storage/FAT support
  // and calls BdsDiscoverEspBootOptions before presenting its menu.
  DEBUG ((DEBUG_INFO, "BdsApp: initializing platform\n"));
  Status = PlatformBdsInit ();
  if (EFI_ERROR (Status)) {
    mLastBootStatus = Status;
    DEBUG ((DEBUG_ERROR, "BdsApp: platform initialization returned %r\n", Status));
  } else if (EFI_ERROR (BootNextStatus)) {
    Status          = BootNextStatus;
    mLastBootStatus = Status;
  } else {
    // A successful menu return means the user explicitly selected continue.
    mMenuRequested = FALSE;
    Status         = BdsRunBootSequence ();
  }

  for (;;) {
    BdsRequestMenu ();
    DEBUG ((DEBUG_INFO, "BdsApp: entering platform menu after %r\n", Status));
    MenuStatus = PlatformBdsInit ();
    if (EFI_ERROR (MenuStatus) || EFI_ERROR (BootNextStatus)) {
      Status          = EFI_ERROR (MenuStatus) ? MenuStatus : BootNextStatus;
      mLastBootStatus = Status;
      // ABL treats any return as a boot failure. Keep callbacks alive even
      // when the console is unavailable, without spinning on an error.
      Print (L"BdsApp: platform menu failed: %r\n", Status);
      DEBUG ((DEBUG_ERROR, "BdsApp: platform menu failed: %r\n", Status));
      gBS->Stall (1000000);
      continue;
    }

    mMenuRequested = FALSE;
    Status         = BdsRunBootSequence ();
  }
}
