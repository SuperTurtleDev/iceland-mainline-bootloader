/** @file
  Standard UEFI boot-option serialization for application-mode boot selection.

  Copyright (c) 2011 - 2019, Intel Corporation. All rights reserved.
  (C) Copyright 2015-2018 Hewlett Packard Enterprise Development LP
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "InternalBm.h"

GLOBAL_REMOVE_IF_UNREFERENCED
  CHAR16 *mBmLoadOptionName[] = {
    L"Driver",
    L"SysPrep",
    L"Boot",
    L"PlatformRecovery"
  };

GLOBAL_REMOVE_IF_UNREFERENCED
  CHAR16 *mBmLoadOptionOrderName[] = {
    EFI_DRIVER_ORDER_VARIABLE_NAME,
    EFI_SYS_PREP_ORDER_VARIABLE_NAME,
    EFI_BOOT_ORDER_VARIABLE_NAME,
    NULL  // PlatformRecovery#### doesn't have associated *Order variable
  };

EFI_STATUS
BmGetFreeOptionNumber (
  IN  EFI_BOOT_MANAGER_LOAD_OPTION_TYPE LoadOptionType,
  OUT UINT16                            *FreeOptionNumber
  )
{

  UINTN         OptionNumber;
  UINTN         Index;
  UINT16        *OptionOrder;
  UINTN         OptionOrderSize;
  UINT16        *BootNext;

  ASSERT (FreeOptionNumber != NULL);
  ASSERT (LoadOptionType == LoadOptionTypeDriver ||
          LoadOptionType == LoadOptionTypeBoot ||
          LoadOptionType == LoadOptionTypeSysPrep);

  GetEfiGlobalVariable2 (mBmLoadOptionOrderName[LoadOptionType], (VOID **) &OptionOrder, &OptionOrderSize);
  if ((OptionOrderSize % sizeof (UINT16)) != 0) {
    if (OptionOrder != NULL) { FreePool (OptionOrder); }
    return EFI_COMPROMISED_DATA;
  }

  BootNext = NULL;
  if (LoadOptionType == LoadOptionTypeBoot) {
    GetEfiGlobalVariable2 (L"BootNext", (VOID**) &BootNext, NULL);
  }

  for (OptionNumber = 0;
       OptionNumber < OptionOrderSize / sizeof (UINT16)
                    + ((BootNext != NULL) ? 1 : 0);
       OptionNumber++
       ) {
    //
    // Search in OptionOrder whether the OptionNumber exists
    //
    for (Index = 0; Index < OptionOrderSize / sizeof (UINT16); Index++) {
      if (OptionNumber == OptionOrder[Index]) {
        break;
      }
    }

    //
    // We didn't find it in the ****Order array and it doesn't equal to BootNext
    // Otherwise, OptionNumber equals to OptionOrderSize / sizeof (UINT16) + 1
    //
    if ((Index == OptionOrderSize / sizeof (UINT16)) &&
        ((BootNext == NULL) || (OptionNumber != *BootNext))
        ) {
      break;
    }
  }
  if (OptionOrder != NULL) {
    FreePool (OptionOrder);
  }

  if (BootNext != NULL) {
    FreePool (BootNext);
  }

  //
  // When BootOrder & BootNext conver all numbers in the range [0 ... 0xffff],
  //   OptionNumber equals to 0x10000 which is not valid.
  //
  ASSERT (OptionNumber <= 0x10000);
  if (OptionNumber == 0x10000) {
    return EFI_OUT_OF_RESOURCES;
  } else {
    *FreeOptionNumber = (UINT16) OptionNumber;
    return EFI_SUCCESS;
  }
}

EFI_STATUS
EFIAPI
EfiBootManagerLoadOptionToVariable (
  IN CONST EFI_BOOT_MANAGER_LOAD_OPTION     *Option
  )
{
  EFI_STATUS                       Status;
  UINTN                            VariableSize;
  UINT8                            *Variable;
  UINT8                            *Ptr;
  CHAR16                           OptionName[BM_OPTION_NAME_LEN];
  CHAR16                           *Description;
  CHAR16                           NullChar;
  UINT32                           VariableAttributes;

  if ((Option == NULL) || (Option->OptionType != LoadOptionTypeBoot) ||
      (Option->OptionNumber == LoadOptionNumberUnassigned) ||
      (Option->FilePath == NULL) ||
      ((UINT32) Option->OptionType >= LoadOptionTypeMax)
     ) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Convert NULL description to empty description
  //
  NullChar    = L'\0';
  Description = Option->Description;
  if (Description == NULL) {
    Description = &NullChar;
  }

  /*
  UINT32                      Attributes;
  UINT16                      FilePathListLength;
  CHAR16                      Description[];
  EFI_DEVICE_PATH_PROTOCOL    FilePathList[];
  UINT8                       OptionalData[];
TODO: FilePathList[] IS:
A packed array of UEFI device paths.  The first element of the
array is a device path that describes the device and location of the
Image for this load option.  The FilePathList[0] is specific
to the device type.  Other device paths may optionally exist in the
FilePathList, but their usage is OSV specific. Each element
in the array is variable length, and ends at the device path end
structure.
  */
  VariableSize = sizeof (Option->Attributes)
               + sizeof (UINT16)
               + StrSize (Description)
               + GetDevicePathSize (Option->FilePath)
               + Option->OptionalDataSize;

  Variable = AllocatePool (VariableSize);
  if (Variable == NULL) { return EFI_OUT_OF_RESOURCES; }

  Ptr             = Variable;
  WriteUnaligned32 ((UINT32 *) Ptr, Option->Attributes);
  Ptr            += sizeof (Option->Attributes);

  WriteUnaligned16 ((UINT16 *) Ptr, (UINT16) GetDevicePathSize (Option->FilePath));
  Ptr            += sizeof (UINT16);

  CopyMem (Ptr, Description, StrSize (Description));
  Ptr            += StrSize (Description);

  CopyMem (Ptr, Option->FilePath, GetDevicePathSize (Option->FilePath));
  Ptr            += GetDevicePathSize (Option->FilePath);

  CopyMem (Ptr, Option->OptionalData, Option->OptionalDataSize);

  UnicodeSPrint (OptionName, sizeof (OptionName), L"%s%04x", mBmLoadOptionName[Option->OptionType], Option->OptionNumber);

  VariableAttributes = EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE;

  Status = gRT->SetVariable (
                  OptionName,
                  &gEfiGlobalVariableGuid,
                  VariableAttributes,
                  VariableSize,
                  Variable
                  );
  FreePool (Variable);

  return Status;
}

EFI_STATUS
BmAddOptionNumberToOrderVariable (
  IN CHAR16               *OptionOrderName,
  IN UINT16               OptionNumber,
  IN UINTN                Position
  )
{
  EFI_STATUS              Status;
  UINTN                   Index;
  UINT16                  *OptionOrder;
  UINT16                  *NewOptionOrder;
  UINTN                   OptionOrderSize;
  //
  // Update the option order variable
  //
  GetEfiGlobalVariable2 (OptionOrderName, (VOID **) &OptionOrder, &OptionOrderSize);
  if ((OptionOrderSize % sizeof (UINT16)) != 0) {
    if (OptionOrder != NULL) { FreePool (OptionOrder); }
    return EFI_COMPROMISED_DATA;
  }

  Status = EFI_SUCCESS;
  for (Index = 0; Index < OptionOrderSize / sizeof (UINT16); Index++) {
    if (OptionOrder[Index] == OptionNumber) {
      Status = EFI_ALREADY_STARTED;
      break;
    }
  }

  if (!EFI_ERROR (Status)) {
    Position       = MIN (Position, OptionOrderSize / sizeof (UINT16));

    NewOptionOrder = AllocatePool (OptionOrderSize + sizeof (UINT16));
    if (NewOptionOrder == NULL) { if (OptionOrder != NULL) FreePool (OptionOrder); return EFI_OUT_OF_RESOURCES; }
    if (OptionOrderSize != 0) {
      CopyMem (NewOptionOrder, OptionOrder, Position * sizeof (UINT16));
      CopyMem (&NewOptionOrder[Position + 1], &OptionOrder[Position], OptionOrderSize - Position * sizeof (UINT16));
    }
    NewOptionOrder[Position] = OptionNumber;

    Status = gRT->SetVariable (
                    OptionOrderName,
                    &gEfiGlobalVariableGuid,
                    EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE,
                    OptionOrderSize + sizeof (UINT16),
                    NewOptionOrder
                    );
    FreePool (NewOptionOrder);
  }

  if (OptionOrder != NULL) {
    FreePool (OptionOrder);
  }

  return Status;
}

EFI_STATUS
EFIAPI
EfiBootManagerAddLoadOptionVariable (
  IN OUT EFI_BOOT_MANAGER_LOAD_OPTION *Option,
  IN     UINTN                        Position
  )
{
  EFI_STATUS                      Status;
  UINT16                          OptionNumber;

  if (Option == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (Option->OptionType != LoadOptionTypeDriver &&
      Option->OptionType != LoadOptionTypeSysPrep &&
      Option->OptionType != LoadOptionTypeBoot
      ) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Get the free option number if the option number is unassigned
  //
  if (Option->OptionNumber == LoadOptionNumberUnassigned) {
    Status = BmGetFreeOptionNumber (Option->OptionType, &OptionNumber);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Option->OptionNumber = OptionNumber;
  }

  if (Option->OptionNumber >= LoadOptionNumberMax) {
    return EFI_INVALID_PARAMETER;
  }

  Status = BmAddOptionNumberToOrderVariable (mBmLoadOptionOrderName[Option->OptionType], (UINT16) Option->OptionNumber, Position);
  if (!EFI_ERROR (Status)) {
    //
    // Save the Boot#### or Driver#### variable
    //
    Status = EfiBootManagerLoadOptionToVariable (Option);
    if (EFI_ERROR (Status)) {
      //
      // Remove the #### from *Order variable when the Driver####/SysPrep####/Boot#### cannot be saved.
      //
      EfiBootManagerDeleteLoadOptionVariable (Option->OptionNumber, Option->OptionType);
    }
  }

  return Status;
}

EFI_STATUS
EFIAPI
EfiBootManagerInitializeLoadOption (
  IN OUT EFI_BOOT_MANAGER_LOAD_OPTION   *Option,
  IN  UINTN                             OptionNumber,
  IN  EFI_BOOT_MANAGER_LOAD_OPTION_TYPE OptionType,
  IN  UINT32                            Attributes,
  IN  CHAR16                            *Description,
  IN  EFI_DEVICE_PATH_PROTOCOL          *FilePath,
  IN  UINT8                             *OptionalData,   OPTIONAL
  IN  UINT32                            OptionalDataSize
  )
{
  if ((Option == NULL) || (Description == NULL) || (FilePath == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (((OptionalData != NULL) && (OptionalDataSize == 0)) ||
      ((OptionalData == NULL) && (OptionalDataSize != 0))) {
    return EFI_INVALID_PARAMETER;
  }

  if ((UINT32) OptionType >= LoadOptionTypeMax) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (Option, sizeof (EFI_BOOT_MANAGER_LOAD_OPTION));
  Option->OptionNumber       = OptionNumber;
  Option->OptionType         = OptionType;
  Option->Attributes         = Attributes;
  Option->Description        = AllocateCopyPool (StrSize (Description), Description);
  Option->FilePath           = DuplicateDevicePath (FilePath);
  if (OptionalData != NULL) {
    Option->OptionalData     = AllocateCopyPool (OptionalDataSize, OptionalData);
    Option->OptionalDataSize = OptionalDataSize;
  }
  if ((Option->Description == NULL) || (Option->FilePath == NULL) ||
      ((OptionalDataSize != 0) && (Option->OptionalData == NULL))) {
    EfiBootManagerFreeLoadOption (Option);
    ZeroMem (Option, sizeof (*Option));
    return EFI_OUT_OF_RESOURCES;
  }

  return EFI_SUCCESS;
}

INTN
EFIAPI
EfiBootManagerFindLoadOption (
  IN CONST EFI_BOOT_MANAGER_LOAD_OPTION *Key,
  IN CONST EFI_BOOT_MANAGER_LOAD_OPTION *Array,
  IN UINTN                              Count
  )
{
  UINTN                             Index;

  for (Index = 0; Index < Count; Index++) {
    if ((Key->OptionType == Array[Index].OptionType) &&
        (Key->Attributes == Array[Index].Attributes) &&
        (StrCmp (Key->Description, Array[Index].Description) == 0) &&
        (CompareMem (Key->FilePath, Array[Index].FilePath, GetDevicePathSize (Key->FilePath)) == 0) &&
        (Key->OptionalDataSize == Array[Index].OptionalDataSize) &&
        (CompareMem (Key->OptionalData, Array[Index].OptionalData, Key->OptionalDataSize) == 0)) {
      return (INTN) Index;
    }
  }

  return -1;
}

EFI_STATUS
EFIAPI
EfiBootManagerDeleteLoadOptionVariable (
  IN UINTN                              OptionNumber,
  IN EFI_BOOT_MANAGER_LOAD_OPTION_TYPE  OptionType
  )
{
  UINT16                            *OptionOrder;
  UINTN                             OptionOrderSize;
  UINTN                             Index;
  CHAR16                            OptionName[BM_OPTION_NAME_LEN];

  if (((UINT32) OptionType >= LoadOptionTypeMax) || (OptionNumber >= LoadOptionNumberMax)) {
    return EFI_INVALID_PARAMETER;
  }

  if (OptionType == LoadOptionTypeDriver || OptionType == LoadOptionTypeSysPrep || OptionType == LoadOptionTypeBoot) {
    //
    // If the associated *Order exists, firstly remove the reference in *Order for
    //  Driver####, SysPrep#### and Boot####.
    //
    GetEfiGlobalVariable2 (mBmLoadOptionOrderName[OptionType], (VOID **) &OptionOrder, &OptionOrderSize);
    if ((OptionOrderSize % sizeof (UINT16)) != 0) {
    if (OptionOrder != NULL) { FreePool (OptionOrder); }
    return EFI_COMPROMISED_DATA;
  }

    for (Index = 0; Index < OptionOrderSize / sizeof (UINT16); Index++) {
      if (OptionOrder[Index] == OptionNumber) {
        OptionOrderSize -= sizeof (UINT16);
        CopyMem (&OptionOrder[Index], &OptionOrder[Index + 1], OptionOrderSize - Index * sizeof (UINT16));
        gRT->SetVariable (
               mBmLoadOptionOrderName[OptionType],
               &gEfiGlobalVariableGuid,
               EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE,
               OptionOrderSize,
               OptionOrder
               );
        break;
      }
    }
    if (OptionOrder != NULL) {
      FreePool (OptionOrder);
    }
  }

  //
  // Remove the Driver####, SysPrep####, Boot#### or PlatformRecovery#### itself.
  //
  UnicodeSPrint (OptionName, sizeof (OptionName), L"%s%04x", mBmLoadOptionName[OptionType], OptionNumber);
  return gRT->SetVariable (
                OptionName,
                &gEfiGlobalVariableGuid,
                0,
                0,
                NULL
                );
}

UINTN
BmGetDevicePathSizeEx (
  IN CONST EFI_DEVICE_PATH_PROTOCOL  *DevicePath,
  IN UINTN                           MaxSize
  )
{
  UINTN  Size;
  UINTN  NodeSize;

  if (DevicePath == NULL) { return 0; }
  Size = 0;
  while ((Size <= MaxSize) && (MaxSize - Size >= sizeof (*DevicePath))) {
    NodeSize = DevicePathNodeLength (DevicePath);
    if ((NodeSize < sizeof (*DevicePath)) || (NodeSize > MaxSize - Size)) { return 0; }
    Size += NodeSize;
    if (IsDevicePathEnd (DevicePath)) {
      return NodeSize == END_DEVICE_PATH_LENGTH ? Size : 0;
    }
    DevicePath = NextDevicePathNode (DevicePath);
  }
  return 0;
}

UINTN
BmStrSizeEx (
  IN      CONST CHAR16              *String,
  IN      UINTN                     MaxStringLen
  )
{
  UINTN                             Length;

  if (String == NULL) { return 0; }
  for (Length = 0; (Length <= MaxStringLen) && (MaxStringLen - Length >= sizeof (CHAR16)); Length += sizeof (CHAR16)) {
    if (ReadUnaligned16 ((CONST UINT16 *)((CONST UINT8 *)String + Length)) == 0) {
      return Length + sizeof (CHAR16);
    }
  }
  return 0;
}

BOOLEAN
BmValidateOption (
  UINT8                     *Variable,
  UINTN                     VariableSize
  )
{
  UINT16                    FilePathSize;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  UINTN                     DescriptionSize;

  if (VariableSize <= sizeof (UINT16) + sizeof (UINT32)) {
    return FALSE;
  }

  //
  // Skip the option attribute
  //
  Variable += sizeof (UINT32);

  //
  // Get the option's device path size
  //
  FilePathSize = ReadUnaligned16 ((UINT16 *) Variable);
  Variable += sizeof (UINT16);

  //
  // Get the option's description string size
  //
  DescriptionSize = BmStrSizeEx ((CHAR16 *) Variable, VariableSize - sizeof (UINT16) - sizeof (UINT32));
  Variable += DescriptionSize;

  //
  // Get the option's device path
  //
  DevicePath = (EFI_DEVICE_PATH_PROTOCOL *) Variable;

  //
  // Validation boot option variable.
  //
  if ((FilePathSize == 0) || (DescriptionSize == 0)) {
    return FALSE;
  }

  if (sizeof (UINT32) + sizeof (UINT16) + DescriptionSize + FilePathSize > VariableSize) {
    return FALSE;
  }

  return (BOOLEAN) (BmGetDevicePathSizeEx (DevicePath, FilePathSize) != 0);
}

BOOLEAN
EFIAPI
EfiBootManagerIsValidLoadOptionVariableName (
  IN CHAR16                             *VariableName,
  OUT EFI_BOOT_MANAGER_LOAD_OPTION_TYPE *OptionType   OPTIONAL,
  OUT UINT16                            *OptionNumber OPTIONAL
  )
{
  UINTN                             VariableNameLen;
  UINTN                             Index;
  UINTN                             Uint;
  EFI_BOOT_MANAGER_LOAD_OPTION_TYPE LocalOptionType;
  UINT16                            LocalOptionNumber;

  if (VariableName == NULL) {
    return FALSE;
  }

  VariableNameLen = StrLen (VariableName);

  //
  // Return FALSE when the variable name length is too small.
  //
  if (VariableNameLen <= 4) {
    return FALSE;
  }

  //
  // Return FALSE when the variable name doesn't start with Driver/SysPrep/Boot/PlatformRecovery.
  //
  for (LocalOptionType = 0; LocalOptionType < ARRAY_SIZE (mBmLoadOptionName); LocalOptionType++) {
    if ((VariableNameLen - 4 == StrLen (mBmLoadOptionName[LocalOptionType])) &&
        (StrnCmp (VariableName, mBmLoadOptionName[LocalOptionType], VariableNameLen - 4) == 0)
        ) {
      break;
    }
  }
  if (LocalOptionType == ARRAY_SIZE (mBmLoadOptionName)) {
    return FALSE;
  }

  //
  // Return FALSE when the last four characters are not hex digits.
  //
  LocalOptionNumber = 0;
  for (Index = VariableNameLen - 4; Index < VariableNameLen; Index++) {
    Uint = BmCharToUint (VariableName[Index]);
    if (Uint == -1) {
      break;
    } else {
      LocalOptionNumber = (UINT16) Uint + LocalOptionNumber * 0x10;
    }
  }
  if (Index != VariableNameLen) {
    return FALSE;
  }

  if (OptionType != NULL) {
    *OptionType = LocalOptionType;
  }

  if (OptionNumber != NULL) {
    *OptionNumber = LocalOptionNumber;
  }

  return TRUE;
}

EFI_STATUS
EFIAPI
EfiBootManagerVariableToLoadOptionEx (
  IN CHAR16                           *VariableName,
  IN EFI_GUID                         *VendorGuid,
  IN OUT EFI_BOOT_MANAGER_LOAD_OPTION *Option
  )
{
  EFI_STATUS                         Status;
  UINT32                             Attribute;
  UINT16                             FilePathSize;
  UINT8                              *Variable;
  UINT8                              *VariablePtr;
  UINTN                              VariableSize;
  EFI_DEVICE_PATH_PROTOCOL           *FilePath;
  UINT8                              *OptionalData;
  UINT32                             OptionalDataSize;
  CHAR16                             *Description;
  EFI_BOOT_MANAGER_LOAD_OPTION_TYPE  OptionType;
  UINT16                             OptionNumber;

  if ((VariableName == NULL) || (Option == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (!EfiBootManagerIsValidLoadOptionVariableName (VariableName, &OptionType, &OptionNumber)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Read the variable
  //
  GetVariable2 (VariableName, VendorGuid, (VOID **) &Variable, &VariableSize);
  if (Variable == NULL) {
    return EFI_NOT_FOUND;
  }

  //
  // Validate *#### variable data.
  //
  if (!BmValidateOption(Variable, VariableSize)) {
    FreePool (Variable);
    return EFI_INVALID_PARAMETER;
  }

  //
  // Get the option attribute
  //
  VariablePtr = Variable;
  Attribute = ReadUnaligned32 ((UINT32 *) VariablePtr);
  VariablePtr += sizeof (UINT32);

  //
  // Get the option's device path size
  //
  FilePathSize = ReadUnaligned16 ((UINT16 *) VariablePtr);
  VariablePtr += sizeof (UINT16);

  //
  // Get the option's description string
  //
  Description = (CHAR16 *) VariablePtr;

  //
  // Get the option's description string size
  //
  VariablePtr += StrSize ((CHAR16 *) VariablePtr);

  //
  // Get the option's device path
  //
  FilePath = (EFI_DEVICE_PATH_PROTOCOL *) VariablePtr;
  VariablePtr += FilePathSize;

  OptionalDataSize = (UINT32) (VariableSize - ((UINTN) VariablePtr - (UINTN) Variable));
  if (OptionalDataSize == 0) {
    OptionalData = NULL;
  } else {
    OptionalData = VariablePtr;
  }

  Status = EfiBootManagerInitializeLoadOption (
             Option,
             OptionNumber,
             OptionType,
             Attribute,
             Description,
             FilePath,
             OptionalData,
             OptionalDataSize
             );
  if (!EFI_ERROR (Status)) {
    CopyGuid (&Option->VendorGuid, VendorGuid);
  }

  FreePool (Variable);
  return Status;
}

EFI_STATUS
EFIAPI
EfiBootManagerVariableToLoadOption (
  IN  CHAR16                          *VariableName,
  IN OUT EFI_BOOT_MANAGER_LOAD_OPTION *Option
  )
{
  return EfiBootManagerVariableToLoadOptionEx (VariableName, &gEfiGlobalVariableGuid, Option);
}

EFI_STATUS
EFIAPI
EfiBootManagerFreeLoadOption (
  IN  EFI_BOOT_MANAGER_LOAD_OPTION  *LoadOption
  )
{
  if (LoadOption == NULL) {
    return EFI_NOT_FOUND;
  }

  if (LoadOption->Description != NULL) {
    FreePool (LoadOption->Description);
  }
  if (LoadOption->FilePath != NULL) {
    FreePool (LoadOption->FilePath);
  }
  if (LoadOption->OptionalData != NULL) {
    FreePool (LoadOption->OptionalData);
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
EfiBootManagerFreeLoadOptions (
  IN  EFI_BOOT_MANAGER_LOAD_OPTION  *Option,
  IN  UINTN                         OptionCount
  )
{
  UINTN   Index;

  if (Option == NULL) {
    return EFI_NOT_FOUND;
  }

  for (Index = 0;Index < OptionCount; Index++) {
    EfiBootManagerFreeLoadOption (&Option[Index]);
  }

  FreePool (Option);

  return EFI_SUCCESS;
}

BOOLEAN
BmIsLoadOptionPeHeaderValid (
  IN EFI_BOOT_MANAGER_LOAD_OPTION_TYPE Type,
  IN VOID                              *FileBuffer,
  IN UINTN                             FileSize
  )
{
  EFI_IMAGE_DOS_HEADER              *DosHeader;
  EFI_IMAGE_OPTIONAL_HEADER_UNION   *PeHeader;
  EFI_IMAGE_OPTIONAL_HEADER32       *OptionalHeader;
  UINT16                            Subsystem;

  if (FileBuffer == NULL || FileSize == 0) {
    return FALSE;
  }

  //
  // Read dos header
  //
  DosHeader = (EFI_IMAGE_DOS_HEADER *) FileBuffer;
  if (FileSize >= sizeof (EFI_IMAGE_DOS_HEADER) &&
      FileSize > DosHeader->e_lfanew && DosHeader->e_magic == EFI_IMAGE_DOS_SIGNATURE
      ) {
    //
    // Read and check PE signature
    //
    PeHeader = (EFI_IMAGE_OPTIONAL_HEADER_UNION *) ((UINT8 *) FileBuffer + DosHeader->e_lfanew);
    if (FileSize >= DosHeader->e_lfanew + sizeof (EFI_IMAGE_OPTIONAL_HEADER_UNION) &&
        PeHeader->Pe32.Signature == EFI_IMAGE_NT_SIGNATURE
        ) {
      //
      // Check PE32 or PE32+ magic, and machine type
      //
      OptionalHeader = (EFI_IMAGE_OPTIONAL_HEADER32 *) &PeHeader->Pe32.OptionalHeader;
      if (OptionalHeader->Magic == EFI_IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
          OptionalHeader->Magic == EFI_IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        //
        // Check the Subsystem:
        //   Driver#### must be of type BootServiceDriver or RuntimeDriver
        //   SysPrep####, Boot####, OsRecovery####, PlatformRecovery#### must be of type Application
        //
        Subsystem = OptionalHeader->Subsystem;
        if ((Type == LoadOptionTypeMax) ||
            (Type == LoadOptionTypeDriver && Subsystem == EFI_IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER) ||
            (Type == LoadOptionTypeDriver && Subsystem == EFI_IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER) ||
            (Type == LoadOptionTypeSysPrep && Subsystem == EFI_IMAGE_SUBSYSTEM_EFI_APPLICATION) ||
            (Type == LoadOptionTypeBoot && Subsystem == EFI_IMAGE_SUBSYSTEM_EFI_APPLICATION) ||
            (Type == LoadOptionTypePlatformRecovery && Subsystem == EFI_IMAGE_SUBSYSTEM_EFI_APPLICATION)
            ) {
          return TRUE;
        }
      }
    }
  }

  return FALSE;
}

VOID *
BmGetNextLoadOptionBuffer (
  IN  EFI_BOOT_MANAGER_LOAD_OPTION_TYPE Type,
  IN  EFI_DEVICE_PATH_PROTOCOL          *FilePath,
  OUT EFI_DEVICE_PATH_PROTOCOL          **FullPath,
  OUT UINTN                             *FileSize
  )
{
  VOID                                  *FileBuffer;
  EFI_DEVICE_PATH_PROTOCOL              *PreFullPath;
  EFI_DEVICE_PATH_PROTOCOL              *CurFullPath;
  UINTN                                 LocalFileSize;
  UINT32                                AuthenticationStatus;
  EFI_DEVICE_PATH_PROTOCOL              *RamDiskDevicePath;

  LocalFileSize = 0;
  FileBuffer  = NULL;
  CurFullPath = *FullPath;
  do {
    PreFullPath = CurFullPath;
    CurFullPath = BmGetNextLoadOptionDevicePath (FilePath, CurFullPath);
    //
    // Only free the full path created *inside* this routine
    //
    if ((PreFullPath != NULL) && (PreFullPath != *FullPath)) {
      FreePool (PreFullPath);
    }
    if (CurFullPath == NULL) {
      break;
    }
    FileBuffer = GetFileBufferByFilePath (TRUE, CurFullPath, &LocalFileSize, &AuthenticationStatus);
    if ((FileBuffer != NULL) && !BmIsLoadOptionPeHeaderValid (Type, FileBuffer, LocalFileSize)) {
      //
      // Free the RAM disk file system if the load option is invalid.
      //
      RamDiskDevicePath = BmGetRamDiskDevicePath (FilePath);
      if (RamDiskDevicePath != NULL) {
        BmDestroyRamDisk (RamDiskDevicePath);
        FreePool (RamDiskDevicePath);
      }

      //
      // Free the invalid load option buffer.
      //
      FreePool (FileBuffer);
      FileBuffer = NULL;
    }
  } while (FileBuffer == NULL);

  if (FileBuffer == NULL) {
    CurFullPath = NULL;
    LocalFileSize = 0;
  }

  DEBUG ((DEBUG_INFO, "[Bds] Expand "));
  BmPrintDp (FilePath);
  DEBUG ((DEBUG_INFO, " -> "));
  BmPrintDp (CurFullPath);
  DEBUG ((DEBUG_INFO, "\n"));

  *FullPath = CurFullPath;
  *FileSize = LocalFileSize;
  return FileBuffer;
}
