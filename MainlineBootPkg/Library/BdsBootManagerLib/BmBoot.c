/** @file
  Standard load-option device-path expansion for application-mode boot selection.

  Copyright (c) 2011 - 2019, Intel Corporation. All rights reserved.
  (C) Copyright 2015-2018 Hewlett Packard Enterprise Development LP
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "InternalBm.h"

EFI_RAM_DISK_PROTOCOL *mRamDisk = NULL;
EFI_GUID mBmHardDriveBootVariableGuid = { 0xfab7e9e1, 0x39dd, 0x4f2b, { 0x84, 0x08, 0xe2, 0x0e, 0x90, 0x6c, 0xb6, 0xde } };
EFI_GUID mBmAutoCreateBootOptionGuid  = { 0x8108ac4e, 0x9f11, 0x4d59, { 0x85, 0x0e, 0xe2, 0x1a, 0x52, 0x2c, 0x59, 0xb2 } };

BOOLEAN
BmIsAutoCreateBootOption (
  EFI_BOOT_MANAGER_LOAD_OPTION    *BootOption
  )
{
  if ((BootOption->OptionalDataSize == sizeof (EFI_GUID)) &&
      CompareGuid ((EFI_GUID *) BootOption->OptionalData, &mBmAutoCreateBootOptionGuid)
      ) {
    return TRUE;
  } else {
    return FALSE;
  }
}

EFI_DEVICE_PATH_PROTOCOL *
BmAdjustFvFilePath (
  IN EFI_DEVICE_PATH_PROTOCOL      *FilePath
  )
{
  EFI_STATUS                    Status;
  UINTN                         Index;
  EFI_DEVICE_PATH_PROTOCOL      *FvFileNode;
  EFI_HANDLE                    FvHandle;
  EFI_LOADED_IMAGE_PROTOCOL     *LoadedImage;
  UINTN                         FvHandleCount;
  EFI_HANDLE                    *FvHandles;
  EFI_DEVICE_PATH_PROTOCOL      *NewDevicePath;
  EFI_DEVICE_PATH_PROTOCOL      *FullPath;

  //
  // Get the file buffer by using the exactly FilePath.
  //
  FvFileNode = FilePath;
  Status = gBS->LocateDevicePath (&gEfiFirmwareVolume2ProtocolGuid, &FvFileNode, &FvHandle);
  if (!EFI_ERROR (Status)) {
    return DuplicateDevicePath (FilePath);
  }

  //
  // Only wide match other FVs if it's a memory mapped FV file path.
  //
  if ((DevicePathType (FilePath) != HARDWARE_DEVICE_PATH) || (DevicePathSubType (FilePath) != HW_MEMMAP_DP)) {
    return NULL;
  }

  FvFileNode = NextDevicePathNode (FilePath);

  //
  // Firstly find the FV file in current FV
  //
  gBS->HandleProtocol (
         gImageHandle,
         &gEfiLoadedImageProtocolGuid,
         (VOID **) &LoadedImage
         );
  NewDevicePath = AppendDevicePathNode (DevicePathFromHandle (LoadedImage->DeviceHandle), FvFileNode);
  FullPath = BmAdjustFvFilePath (NewDevicePath);
  FreePool (NewDevicePath);
  if (FullPath != NULL) {
    return FullPath;
  }

  //
  // Secondly find the FV file in all other FVs
  //
  gBS->LocateHandleBuffer (
         ByProtocol,
         &gEfiFirmwareVolume2ProtocolGuid,
         NULL,
         &FvHandleCount,
         &FvHandles
         );
  for (Index = 0; Index < FvHandleCount; Index++) {
    if (FvHandles[Index] == LoadedImage->DeviceHandle) {
      //
      // Skip current FV, it was handed in first step.
      //
      continue;
    }
    NewDevicePath = AppendDevicePathNode (DevicePathFromHandle (FvHandles[Index]), FvFileNode);
    FullPath = BmAdjustFvFilePath (NewDevicePath);
    FreePool (NewDevicePath);
    if (FullPath != NULL) {
      break;
    }
  }

  if (FvHandles != NULL) {
    FreePool (FvHandles);
  }
  return FullPath;
}

BOOLEAN
BmIsFvFilePath (
  IN EFI_DEVICE_PATH_PROTOCOL    *DevicePath
  )
{
  EFI_STATUS                     Status;
  EFI_HANDLE                     Handle;
  EFI_DEVICE_PATH_PROTOCOL       *Node;

  Node = DevicePath;
  Status = gBS->LocateDevicePath (&gEfiFirmwareVolume2ProtocolGuid, &Node, &Handle);
  if (!EFI_ERROR (Status)) {
    return TRUE;
  }

  if ((DevicePathType (DevicePath) == HARDWARE_DEVICE_PATH) && (DevicePathSubType (DevicePath) == HW_MEMMAP_DP)) {
    DevicePath = NextDevicePathNode (DevicePath);
    if ((DevicePathType (DevicePath) == MEDIA_DEVICE_PATH) && (DevicePathSubType (DevicePath) == MEDIA_PIWG_FW_FILE_DP)) {
      return IsDevicePathEnd (NextDevicePathNode (DevicePath));
    }
  }
  return FALSE;
}

BOOLEAN
BmMatchUsbClass (
  IN EFI_USB_IO_PROTOCOL        *UsbIo,
  IN USB_CLASS_DEVICE_PATH      *UsbClass
  )
{
  EFI_STATUS                    Status;
  EFI_USB_DEVICE_DESCRIPTOR     DevDesc;
  EFI_USB_INTERFACE_DESCRIPTOR  IfDesc;
  UINT8                         DeviceClass;
  UINT8                         DeviceSubClass;
  UINT8                         DeviceProtocol;

  if ((DevicePathType (UsbClass) != MESSAGING_DEVICE_PATH) ||
      (DevicePathSubType (UsbClass) != MSG_USB_CLASS_DP)){
    return FALSE;
  }

  //
  // Check Vendor Id and Product Id.
  //
  Status = UsbIo->UsbGetDeviceDescriptor (UsbIo, &DevDesc);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  if ((UsbClass->VendorId != 0xffff) &&
      (UsbClass->VendorId != DevDesc.IdVendor)) {
    return FALSE;
  }

  if ((UsbClass->ProductId != 0xffff) &&
      (UsbClass->ProductId != DevDesc.IdProduct)) {
    return FALSE;
  }

  DeviceClass    = DevDesc.DeviceClass;
  DeviceSubClass = DevDesc.DeviceSubClass;
  DeviceProtocol = DevDesc.DeviceProtocol;
  if (DeviceClass == 0) {
    //
    // If Class in Device Descriptor is set to 0, use the Class, SubClass and
    // Protocol in Interface Descriptor instead.
    //
    Status = UsbIo->UsbGetInterfaceDescriptor (UsbIo, &IfDesc);
    if (EFI_ERROR (Status)) {
      return FALSE;
    }

    DeviceClass    = IfDesc.InterfaceClass;
    DeviceSubClass = IfDesc.InterfaceSubClass;
    DeviceProtocol = IfDesc.InterfaceProtocol;
  }

  //
  // Check Class, SubClass and Protocol.
  //
  if ((UsbClass->DeviceClass != 0xff) &&
      (UsbClass->DeviceClass != DeviceClass)) {
    return FALSE;
  }

  if ((UsbClass->DeviceSubClass != 0xff) &&
      (UsbClass->DeviceSubClass != DeviceSubClass)) {
    return FALSE;
  }

  if ((UsbClass->DeviceProtocol != 0xff) &&
      (UsbClass->DeviceProtocol != DeviceProtocol)) {
    return FALSE;
  }

  return TRUE;
}

BOOLEAN
BmMatchUsbWwid (
  IN EFI_USB_IO_PROTOCOL        *UsbIo,
  IN USB_WWID_DEVICE_PATH       *UsbWwid
  )
{
  EFI_STATUS                   Status;
  EFI_USB_DEVICE_DESCRIPTOR    DevDesc;
  EFI_USB_INTERFACE_DESCRIPTOR IfDesc;
  UINT16                       *LangIdTable;
  UINT16                       TableSize;
  UINT16                       Index;
  CHAR16                       *CompareStr;
  UINTN                        CompareLen;
  CHAR16                       *SerialNumberStr;
  UINTN                        Length;

  if ((DevicePathType (UsbWwid) != MESSAGING_DEVICE_PATH) ||
      (DevicePathSubType (UsbWwid) != MSG_USB_WWID_DP)) {
    return FALSE;
  }

  //
  // Check Vendor Id and Product Id.
  //
  Status = UsbIo->UsbGetDeviceDescriptor (UsbIo, &DevDesc);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }
  if ((DevDesc.IdVendor != UsbWwid->VendorId) ||
      (DevDesc.IdProduct != UsbWwid->ProductId)) {
    return FALSE;
  }

  //
  // Check Interface Number.
  //
  Status = UsbIo->UsbGetInterfaceDescriptor (UsbIo, &IfDesc);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }
  if (IfDesc.InterfaceNumber != UsbWwid->InterfaceNumber) {
    return FALSE;
  }

  //
  // Check Serial Number.
  //
  if (DevDesc.StrSerialNumber == 0) {
    return FALSE;
  }

  //
  // Get all supported languages.
  //
  TableSize = 0;
  LangIdTable = NULL;
  Status = UsbIo->UsbGetSupportedLanguages (UsbIo, &LangIdTable, &TableSize);
  if (EFI_ERROR (Status) || (TableSize == 0) || (LangIdTable == NULL)) {
    return FALSE;
  }

  //
  // Serial number in USB WWID device path is the last 64-or-less UTF-16 characters.
  //
  CompareStr = (CHAR16 *) (UINTN) (UsbWwid + 1);
  CompareLen = (DevicePathNodeLength (UsbWwid) - sizeof (USB_WWID_DEVICE_PATH)) / sizeof (CHAR16);
  if (CompareStr[CompareLen - 1] == L'\0') {
    CompareLen--;
  }

  //
  // Compare serial number in each supported language.
  //
  for (Index = 0; Index < TableSize / sizeof (UINT16); Index++) {
    SerialNumberStr = NULL;
    Status = UsbIo->UsbGetStringDescriptor (
                      UsbIo,
                      LangIdTable[Index],
                      DevDesc.StrSerialNumber,
                      &SerialNumberStr
                      );
    if (EFI_ERROR (Status) || (SerialNumberStr == NULL)) {
      continue;
    }

    Length = StrLen (SerialNumberStr);
    if ((Length >= CompareLen) &&
        (CompareMem (SerialNumberStr + Length - CompareLen, CompareStr, CompareLen * sizeof (CHAR16)) == 0)) {
      FreePool (SerialNumberStr);
      return TRUE;
    }

    FreePool (SerialNumberStr);
  }

  return FALSE;
}

EFI_HANDLE *
BmFindUsbDevice (
  IN  EFI_DEVICE_PATH_PROTOCOL  *DevicePath,
  IN  UINTN                     ParentDevicePathSize,
  OUT UINTN                     *UsbIoHandleCount
  )
{
  EFI_STATUS                Status;
  EFI_HANDLE                *UsbIoHandles;
  EFI_DEVICE_PATH_PROTOCOL  *UsbIoDevicePath;
  EFI_USB_IO_PROTOCOL       *UsbIo;
  UINTN                     Index;
  BOOLEAN                   Matched;

  ASSERT (UsbIoHandleCount != NULL);

  //
  // Get all UsbIo Handles.
  //
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiUsbIoProtocolGuid,
                  NULL,
                  UsbIoHandleCount,
                  &UsbIoHandles
                  );
  if (EFI_ERROR (Status)) {
    *UsbIoHandleCount = 0;
    UsbIoHandles      = NULL;
  }

  for (Index = 0; Index < *UsbIoHandleCount; ) {
    //
    // Get the Usb IO interface.
    //
    Status = gBS->HandleProtocol(
                    UsbIoHandles[Index],
                    &gEfiUsbIoProtocolGuid,
                    (VOID **) &UsbIo
                    );
    UsbIoDevicePath = DevicePathFromHandle (UsbIoHandles[Index]);
    Matched         = FALSE;
    if (!EFI_ERROR (Status) && (UsbIoDevicePath != NULL)) {

      //
      // Compare starting part of UsbIoHandle's device path with ParentDevicePath.
      //
      if (CompareMem (UsbIoDevicePath, DevicePath, ParentDevicePathSize) == 0) {
        if (BmMatchUsbClass (UsbIo, (USB_CLASS_DEVICE_PATH *) ((UINTN) DevicePath + ParentDevicePathSize)) ||
            BmMatchUsbWwid (UsbIo, (USB_WWID_DEVICE_PATH *) ((UINTN) DevicePath + ParentDevicePathSize))) {
          Matched = TRUE;
        }
      }
    }

    if (!Matched) {
      (*UsbIoHandleCount) --;
      CopyMem (&UsbIoHandles[Index], &UsbIoHandles[Index + 1], (*UsbIoHandleCount - Index) * sizeof (EFI_HANDLE));
    } else {
      Index++;
    }
  }

  return UsbIoHandles;
}

EFI_DEVICE_PATH_PROTOCOL *
BmExpandUsbDevicePath (
  IN  EFI_DEVICE_PATH_PROTOCOL  *FilePath,
  IN  EFI_DEVICE_PATH_PROTOCOL  *FullPath,
  IN  EFI_DEVICE_PATH_PROTOCOL  *ShortformNode
  )
{
  UINTN                             ParentDevicePathSize;
  EFI_DEVICE_PATH_PROTOCOL          *RemainingDevicePath;
  EFI_DEVICE_PATH_PROTOCOL          *NextFullPath;
  EFI_HANDLE                        *Handles;
  UINTN                             HandleCount;
  UINTN                             Index;
  BOOLEAN                           GetNext;

  NextFullPath = NULL;
  GetNext = (BOOLEAN)(FullPath == NULL);
  ParentDevicePathSize = (UINTN) ShortformNode - (UINTN) FilePath;
  RemainingDevicePath = NextDevicePathNode (ShortformNode);
  Handles = BmFindUsbDevice (FilePath, ParentDevicePathSize, &HandleCount);

  for (Index = 0; Index < HandleCount; Index++) {
    FilePath = AppendDevicePath (DevicePathFromHandle (Handles[Index]), RemainingDevicePath);
    if (FilePath == NULL) {
      //
      // Out of memory.
      //
      continue;
    }
    NextFullPath = BmGetNextLoadOptionDevicePath (FilePath, NULL);
    FreePool (FilePath);
    if (NextFullPath == NULL) {
      //
      // No BlockIo or SimpleFileSystem under FilePath.
      //
      continue;
    }
    if (GetNext) {
      break;
    } else {
      GetNext = (BOOLEAN)(CompareMem (NextFullPath, FullPath, GetDevicePathSize (NextFullPath)) == 0);
      FreePool (NextFullPath);
      NextFullPath = NULL;
    }
  }

  if (Handles != NULL) {
    FreePool (Handles);
  }

  return NextFullPath;
}

EFI_DEVICE_PATH_PROTOCOL *
BmExpandFileDevicePath (
  IN  EFI_DEVICE_PATH_PROTOCOL    *FilePath,
  IN  EFI_DEVICE_PATH_PROTOCOL    *FullPath
  )
{
  EFI_STATUS                      Status;
  UINTN                           Index;
  UINTN                           HandleCount;
  EFI_HANDLE                      *Handles;
  EFI_BLOCK_IO_PROTOCOL           *BlockIo;
  UINTN                           MediaType;
  EFI_DEVICE_PATH_PROTOCOL        *NextFullPath;
  BOOLEAN                         GetNext;

  EfiBootManagerConnectAll ();
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    HandleCount = 0;
    Handles = NULL;
  }

  GetNext = (BOOLEAN)(FullPath == NULL);
  NextFullPath = NULL;
  //
  // Enumerate all removable media devices followed by all fixed media devices,
  //   followed by media devices which don't layer on block io.
  //
  for (MediaType = 0; MediaType < 3; MediaType++) {
    for (Index = 0; Index < HandleCount; Index++) {
      Status = gBS->HandleProtocol (Handles[Index], &gEfiBlockIoProtocolGuid, (VOID *) &BlockIo);
      if (EFI_ERROR (Status)) {
        BlockIo = NULL;
      }
      if ((MediaType == 0 && BlockIo != NULL && BlockIo->Media->RemovableMedia) ||
          (MediaType == 1 && BlockIo != NULL && !BlockIo->Media->RemovableMedia) ||
          (MediaType == 2 && BlockIo == NULL)
          ) {
        NextFullPath = AppendDevicePath (DevicePathFromHandle (Handles[Index]), FilePath);
        if (GetNext) {
          break;
        } else {
          GetNext = (BOOLEAN)(CompareMem (NextFullPath, FullPath, GetDevicePathSize (NextFullPath)) == 0);
          FreePool (NextFullPath);
          NextFullPath = NULL;
        }
      }
    }
    if (NextFullPath != NULL) {
      break;
    }
  }

  if (Handles != NULL) {
    FreePool (Handles);
  }

  return NextFullPath;
}

EFI_DEVICE_PATH_PROTOCOL *
BmExpandUriDevicePath (
  IN  EFI_DEVICE_PATH_PROTOCOL    *FilePath,
  IN  EFI_DEVICE_PATH_PROTOCOL    *FullPath
  )
{
  EFI_STATUS                      Status;
  UINTN                           Index;
  UINTN                           HandleCount;
  EFI_HANDLE                      *Handles;
  EFI_DEVICE_PATH_PROTOCOL        *NextFullPath;
  EFI_DEVICE_PATH_PROTOCOL        *RamDiskDevicePath;
  BOOLEAN                         GetNext;

  EfiBootManagerConnectAll ();
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiLoadFileProtocolGuid, NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    HandleCount = 0;
    Handles = NULL;
  }

  NextFullPath = NULL;
  GetNext = (BOOLEAN)(FullPath == NULL);
  for (Index = 0; Index < HandleCount; Index++) {
    NextFullPath = BmExpandLoadFile (Handles[Index], FilePath);

    if (NextFullPath == NULL) {
      continue;
    }

    if (GetNext) {
      break;
    } else {
      GetNext = (BOOLEAN)(CompareMem (NextFullPath, FullPath, GetDevicePathSize (NextFullPath)) == 0);
      //
      // Free the resource occupied by the RAM disk.
      //
      RamDiskDevicePath = BmGetRamDiskDevicePath (NextFullPath);
      if (RamDiskDevicePath != NULL) {
        BmDestroyRamDisk (RamDiskDevicePath);
        FreePool (RamDiskDevicePath);
      }
      FreePool (NextFullPath);
      NextFullPath = NULL;
    }
  }

  if (Handles != NULL) {
    FreePool (Handles);
  }

  return NextFullPath;
}

VOID
BmCachePartitionDevicePath (
  IN OUT EFI_DEVICE_PATH_PROTOCOL **CachedDevicePath,
  IN EFI_DEVICE_PATH_PROTOCOL     *DevicePath
  )
{
  EFI_DEVICE_PATH_PROTOCOL        *TempDevicePath;
  UINTN                           Count;

  if (BmMatchDevicePaths (*CachedDevicePath, DevicePath)) {
    TempDevicePath = *CachedDevicePath;
    *CachedDevicePath = BmDelPartMatchInstance (*CachedDevicePath, DevicePath);
    FreePool (TempDevicePath);
  }

  if (*CachedDevicePath == NULL) {
    *CachedDevicePath = DuplicateDevicePath (DevicePath);
    return;
  }

  TempDevicePath = *CachedDevicePath;
  *CachedDevicePath = AppendDevicePathInstance (DevicePath, *CachedDevicePath);
  if (TempDevicePath != NULL) {
    FreePool (TempDevicePath);
  }

  //
  // Here limit the device path instance number to 12, which is max number for a system support 3 IDE controller
  // If the user try to boot many OS in different HDs or partitions, in theory, the 'HDDP' variable maybe become larger and larger.
  //
  Count = 0;
  TempDevicePath = *CachedDevicePath;
  while (!IsDevicePathEnd (TempDevicePath)) {
    TempDevicePath = NextDevicePathNode (TempDevicePath);
    //
    // Parse one instance
    //
    while (!IsDevicePathEndType (TempDevicePath)) {
      TempDevicePath = NextDevicePathNode (TempDevicePath);
    }
    Count++;
    //
    // If the CachedDevicePath variable contain too much instance, only remain 12 instances.
    //
    if (Count == 12) {
      SetDevicePathEndNode (TempDevicePath);
      break;
    }
  }
}

EFI_DEVICE_PATH_PROTOCOL *
BmExpandPartitionDevicePath (
  IN  EFI_DEVICE_PATH_PROTOCOL  *FilePath
  )
{
  EFI_STATUS                Status;
  UINTN                     BlockIoHandleCount;
  EFI_HANDLE                *BlockIoBuffer;
  EFI_DEVICE_PATH_PROTOCOL  *BlockIoDevicePath;
  UINTN                     Index;
  EFI_DEVICE_PATH_PROTOCOL  *CachedDevicePath;
  EFI_DEVICE_PATH_PROTOCOL  *TempNewDevicePath;
  EFI_DEVICE_PATH_PROTOCOL  *TempDevicePath;
  EFI_DEVICE_PATH_PROTOCOL  *FullPath;
  UINTN                     CachedDevicePathSize;
  BOOLEAN                   NeedAdjust;
  EFI_DEVICE_PATH_PROTOCOL  *Instance;
  UINTN                     Size;

  //
  // Check if there is prestore 'HDDP' variable.
  // If exist, search the front path which point to partition node in the variable instants.
  // If fail to find or 'HDDP' not exist, reconnect all and search in all system
  //
  GetVariable2 (L"HDDP", &mBmHardDriveBootVariableGuid, (VOID **) &CachedDevicePath, &CachedDevicePathSize);

  //
  // Delete the invalid 'HDDP' variable.
  //
  if ((CachedDevicePath != NULL) && !IsDevicePathValid (CachedDevicePath, CachedDevicePathSize)) {
    FreePool (CachedDevicePath);
    CachedDevicePath = NULL;
    Status = gRT->SetVariable (
                    L"HDDP",
                    &mBmHardDriveBootVariableGuid,
                    0,
                    0,
                    NULL
                    );
    ASSERT_EFI_ERROR (Status);
  }

  FullPath = NULL;
  if (CachedDevicePath != NULL) {
    TempNewDevicePath = CachedDevicePath;
    NeedAdjust = FALSE;
    do {
      //
      // Check every instance of the variable
      // First, check whether the instance contain the partition node, which is needed for distinguishing  multi
      // partial partition boot option. Second, check whether the instance could be connected.
      //
      Instance  = GetNextDevicePathInstance (&TempNewDevicePath, &Size);
      if (BmMatchPartitionDevicePathNode (Instance, (HARDDRIVE_DEVICE_PATH *) FilePath)) {
        //
        // Connect the device path instance, the device path point to hard drive media device path node
        // e.g. ACPI() /PCI()/ATA()/Partition()
        //
        Status = EfiBootManagerConnectDevicePath (Instance, NULL);
        if (!EFI_ERROR (Status)) {
          TempDevicePath = AppendDevicePath (Instance, NextDevicePathNode (FilePath));
          //
          // TempDevicePath = ACPI()/PCI()/ATA()/Partition()
          // or             = ACPI()/PCI()/ATA()/Partition()/.../A.EFI
          //
          // When TempDevicePath = ACPI()/PCI()/ATA()/Partition(),
          // it may expand to two potienal full paths (nested partition, rarely happen):
          //   1. ACPI()/PCI()/ATA()/Partition()/Partition(A1)/EFI/BootX64.EFI
          //   2. ACPI()/PCI()/ATA()/Partition()/Partition(A2)/EFI/BootX64.EFI
          // For simplicity, only #1 is returned.
          //
          FullPath = BmGetNextLoadOptionDevicePath (TempDevicePath, NULL);
          FreePool (TempDevicePath);

          if (FullPath != NULL) {
            //
            // Adjust the 'HDDP' instances sequence if the matched one is not first one.
            //
            if (NeedAdjust) {
              BmCachePartitionDevicePath (&CachedDevicePath, Instance);
              //
              // Save the matching Device Path so we don't need to do a connect all next time
              // Failing to save only impacts performance next time expanding the short-form device path
              //
              Status = gRT->SetVariable (
                L"HDDP",
                &mBmHardDriveBootVariableGuid,
                EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_NON_VOLATILE,
                GetDevicePathSize (CachedDevicePath),
                CachedDevicePath
                );
            }

            FreePool (Instance);
            FreePool (CachedDevicePath);
            return FullPath;
          }
        }
      }
      //
      // Come here means the first instance is not matched
      //
      NeedAdjust = TRUE;
      FreePool(Instance);
    } while (TempNewDevicePath != NULL);
  }

  //
  // If we get here we fail to find or 'HDDP' not exist, and now we need
  // to search all devices in the system for a matched partition
  //
  EfiBootManagerConnectAll ();
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &BlockIoHandleCount, &BlockIoBuffer);
  if (EFI_ERROR (Status)) {
    BlockIoHandleCount = 0;
    BlockIoBuffer      = NULL;
  }
  //
  // Loop through all the device handles that support the BLOCK_IO Protocol
  //
  for (Index = 0; Index < BlockIoHandleCount; Index++) {
    BlockIoDevicePath = DevicePathFromHandle (BlockIoBuffer[Index]);
    if (BlockIoDevicePath == NULL) {
      continue;
    }

    if (BmMatchPartitionDevicePathNode (BlockIoDevicePath, (HARDDRIVE_DEVICE_PATH *) FilePath)) {
      //
      // Find the matched partition device path
      //
      TempDevicePath = AppendDevicePath (BlockIoDevicePath, NextDevicePathNode (FilePath));
      FullPath = BmGetNextLoadOptionDevicePath (TempDevicePath, NULL);
      FreePool (TempDevicePath);

      if (FullPath != NULL) {
        BmCachePartitionDevicePath (&CachedDevicePath, BlockIoDevicePath);

        //
        // Save the matching Device Path so we don't need to do a connect all next time
        // Failing to save only impacts performance next time expanding the short-form device path
        //
        Status = gRT->SetVariable (
                        L"HDDP",
                        &mBmHardDriveBootVariableGuid,
                        EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_NON_VOLATILE,
                        GetDevicePathSize (CachedDevicePath),
                        CachedDevicePath
                        );

        break;
      }
    }
  }

  if (CachedDevicePath != NULL) {
    FreePool (CachedDevicePath);
  }
  if (BlockIoBuffer != NULL) {
    FreePool (BlockIoBuffer);
  }
  return FullPath;
}

EFI_DEVICE_PATH_PROTOCOL *
BmExpandMediaDevicePath (
  IN  EFI_DEVICE_PATH_PROTOCOL        *DevicePath,
  IN  EFI_DEVICE_PATH_PROTOCOL        *FullPath
  )
{
  EFI_STATUS                          Status;
  EFI_HANDLE                          Handle;
  EFI_BLOCK_IO_PROTOCOL               *BlockIo;
  VOID                                *Buffer;
  EFI_DEVICE_PATH_PROTOCOL            *TempDevicePath;
  EFI_DEVICE_PATH_PROTOCOL            *NextFullPath;
  UINTN                               Size;
  UINTN                               TempSize;
  EFI_HANDLE                          *SimpleFileSystemHandles;
  UINTN                               NumberSimpleFileSystemHandles;
  UINTN                               Index;
  BOOLEAN                             GetNext;

  GetNext = (BOOLEAN)(FullPath == NULL);
  //
  // Check whether the device is connected
  //
  TempDevicePath = DevicePath;
  Status = gBS->LocateDevicePath (&gEfiSimpleFileSystemProtocolGuid, &TempDevicePath, &Handle);
  if (!EFI_ERROR (Status)) {
    ASSERT (IsDevicePathEnd (TempDevicePath));

    NextFullPath = FileDevicePath (Handle, EFI_REMOVABLE_MEDIA_FILE_NAME);
    //
    // For device path pointing to simple file system, it only expands to one full path.
    //
    if (GetNext) {
      return NextFullPath;
    } else {
      FreePool (NextFullPath);
      return NULL;
    }
  }

  Status = gBS->LocateDevicePath (&gEfiBlockIoProtocolGuid, &TempDevicePath, &Handle);
  ASSERT_EFI_ERROR (Status);

  //
  // For device boot option only pointing to the removable device handle,
  // should make sure all its children handles (its child partion or media handles)
  // are created and connected.
  //
  gBS->ConnectController (Handle, NULL, NULL, TRUE);

  //
  // Issue a dummy read to the device to check for media change.
  // When the removable media is changed, any Block IO read/write will
  // cause the BlockIo protocol be reinstalled and EFI_MEDIA_CHANGED is
  // returned. After the Block IO protocol is reinstalled, subsequent
  // Block IO read/write will success.
  //
  Status = gBS->HandleProtocol (Handle, &gEfiBlockIoProtocolGuid, (VOID **) &BlockIo);
  ASSERT_EFI_ERROR (Status);
  if (EFI_ERROR (Status)) {
    return NULL;
  }
  Buffer = AllocatePool (BlockIo->Media->BlockSize);
  if (Buffer != NULL) {
    BlockIo->ReadBlocks (
      BlockIo,
      BlockIo->Media->MediaId,
      0,
      BlockIo->Media->BlockSize,
      Buffer
      );
    FreePool (Buffer);
  }

  //
  // Detect the the default boot file from removable Media
  //
  NextFullPath = NULL;
  Size = GetDevicePathSize (DevicePath) - END_DEVICE_PATH_LENGTH;
  gBS->LocateHandleBuffer (
         ByProtocol,
         &gEfiSimpleFileSystemProtocolGuid,
         NULL,
         &NumberSimpleFileSystemHandles,
         &SimpleFileSystemHandles
         );
  for (Index = 0; Index < NumberSimpleFileSystemHandles; Index++) {
    //
    // Get the device path size of SimpleFileSystem handle
    //
    TempDevicePath = DevicePathFromHandle (SimpleFileSystemHandles[Index]);
    TempSize = GetDevicePathSize (TempDevicePath) - END_DEVICE_PATH_LENGTH;
    //
    // Check whether the device path of boot option is part of the SimpleFileSystem handle's device path
    //
    if ((Size <= TempSize) && (CompareMem (TempDevicePath, DevicePath, Size) == 0)) {
      NextFullPath = FileDevicePath (SimpleFileSystemHandles[Index], EFI_REMOVABLE_MEDIA_FILE_NAME);
      if (GetNext) {
        break;
      } else {
        GetNext = (BOOLEAN)(CompareMem (NextFullPath, FullPath, GetDevicePathSize (NextFullPath)) == 0);
        FreePool (NextFullPath);
        NextFullPath = NULL;
      }
    }
  }

  if (SimpleFileSystemHandles != NULL) {
    FreePool (SimpleFileSystemHandles);
  }

  return NextFullPath;
}

BOOLEAN
BmMatchHttpBootDevicePath (
  IN EFI_DEVICE_PATH_PROTOCOL *Left,
  IN EFI_DEVICE_PATH_PROTOCOL *Right
  )
{
  for (;  !IsDevicePathEnd (Left) && !IsDevicePathEnd (Right)
       ;  Left = NextDevicePathNode (Left), Right = NextDevicePathNode (Right)
       ) {
    if (CompareMem (Left, Right, DevicePathNodeLength (Left)) != 0) {
      if ((DevicePathType (Left) != MESSAGING_DEVICE_PATH) || (DevicePathType (Right) != MESSAGING_DEVICE_PATH)) {
        return FALSE;
      }

      if (DevicePathSubType (Left) == MSG_DNS_DP) {
        Left = NextDevicePathNode (Left);
      }

      if (DevicePathSubType (Right) == MSG_DNS_DP) {
        Right = NextDevicePathNode (Right);
      }

      if (((DevicePathSubType (Left) != MSG_IPv4_DP) || (DevicePathSubType (Right) != MSG_IPv4_DP)) &&
          ((DevicePathSubType (Left) != MSG_IPv6_DP) || (DevicePathSubType (Right) != MSG_IPv6_DP)) &&
          ((DevicePathSubType (Left) != MSG_URI_DP)  || (DevicePathSubType (Right) != MSG_URI_DP))
          ) {
        return FALSE;
      }
    }
  }
  return (BOOLEAN) (IsDevicePathEnd (Left) && IsDevicePathEnd (Right));
}

EFI_DEVICE_PATH_PROTOCOL *
BmExpandNetworkFileSystem (
  IN  EFI_HANDLE                      LoadFileHandle,
  OUT EFI_HANDLE                      *RamDiskHandle
  )
{
  EFI_STATUS                      Status;
  EFI_HANDLE                      Handle;
  EFI_HANDLE                      *Handles;
  UINTN                           HandleCount;
  UINTN                           Index;
  EFI_DEVICE_PATH_PROTOCOL        *Node;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiBlockIoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    Handles = NULL;
    HandleCount = 0;
  }

  Handle = NULL;
  for (Index = 0; Index < HandleCount; Index++) {
    Node = DevicePathFromHandle (Handles[Index]);
    Status = gBS->LocateDevicePath (&gEfiLoadFileProtocolGuid, &Node, &Handle);
    if (!EFI_ERROR (Status) &&
        (Handle == LoadFileHandle) &&
        (DevicePathType (Node) == MEDIA_DEVICE_PATH) && (DevicePathSubType (Node) == MEDIA_RAM_DISK_DP)) {
      //
      // Find the BlockIo instance populated from the LoadFile.
      //
      Handle = Handles[Index];
      break;
    }
  }

  if (Handles != NULL) {
    FreePool (Handles);
  }

  if (Index == HandleCount) {
    Handle = NULL;
  }

  *RamDiskHandle = Handle;

  if (Handle != NULL) {
    //
    // Re-use BmExpandMediaDevicePath() to get the full device path of load option.
    // But assume only one SimpleFileSystem can be found under the BlockIo.
    //
    return BmExpandMediaDevicePath (DevicePathFromHandle (Handle), NULL);
  } else {
    return NULL;
  }
}

EFI_DEVICE_PATH_PROTOCOL *
BmGetRamDiskDevicePath (
  IN EFI_DEVICE_PATH_PROTOCOL *FilePath
  )
{
  EFI_STATUS                  Status;
  EFI_DEVICE_PATH_PROTOCOL    *RamDiskDevicePath;
  EFI_DEVICE_PATH_PROTOCOL    *Node;
  EFI_HANDLE                  Handle;

  Node = FilePath;
  Status = gBS->LocateDevicePath (&gEfiLoadFileProtocolGuid, &Node, &Handle);
  if (!EFI_ERROR (Status) &&
      (DevicePathType (Node) == MEDIA_DEVICE_PATH) &&
      (DevicePathSubType (Node) == MEDIA_RAM_DISK_DP)
      ) {

    //
    // Construct the device path pointing to RAM Disk
    //
    Node = NextDevicePathNode (Node);
    RamDiskDevicePath = DuplicateDevicePath (FilePath);
    ASSERT (RamDiskDevicePath != NULL);
    SetDevicePathEndNode ((VOID *) ((UINTN) RamDiskDevicePath + ((UINTN) Node - (UINTN) FilePath)));
    return RamDiskDevicePath;
  }

  return NULL;
}

VOID *
BmGetRamDiskMemoryInfo (
  IN EFI_DEVICE_PATH_PROTOCOL *RamDiskDevicePath,
  OUT UINTN                   *RamDiskSizeInPages
  )
{

  EFI_STATUS                  Status;
  EFI_HANDLE                  Handle;
  UINT64                      StartingAddr;
  UINT64                      EndingAddr;

  ASSERT (RamDiskDevicePath != NULL);

  *RamDiskSizeInPages = 0;

  //
  // Get the buffer occupied by RAM Disk.
  //
  Status = gBS->LocateDevicePath (&gEfiLoadFileProtocolGuid, &RamDiskDevicePath, &Handle);
  ASSERT_EFI_ERROR (Status);
  ASSERT ((DevicePathType (RamDiskDevicePath) == MEDIA_DEVICE_PATH) &&
          (DevicePathSubType (RamDiskDevicePath) == MEDIA_RAM_DISK_DP));
  StartingAddr = ReadUnaligned64 ((UINT64 *) ((MEDIA_RAM_DISK_DEVICE_PATH *) RamDiskDevicePath)->StartingAddr);
  EndingAddr   = ReadUnaligned64 ((UINT64 *) ((MEDIA_RAM_DISK_DEVICE_PATH *) RamDiskDevicePath)->EndingAddr);
  *RamDiskSizeInPages = EFI_SIZE_TO_PAGES ((UINTN) (EndingAddr - StartingAddr + 1));
  return (VOID *) (UINTN) StartingAddr;
}

VOID
BmDestroyRamDisk (
  IN EFI_DEVICE_PATH_PROTOCOL *RamDiskDevicePath
  )
{
  EFI_STATUS                  Status;
  VOID                        *RamDiskBuffer;
  UINTN                       RamDiskSizeInPages;

  ASSERT (RamDiskDevicePath != NULL);

  RamDiskBuffer = BmGetRamDiskMemoryInfo (RamDiskDevicePath, &RamDiskSizeInPages);

  //
  // Destroy RAM Disk.
  //
  if (mRamDisk == NULL) {
    Status = gBS->LocateProtocol (&gEfiRamDiskProtocolGuid, NULL, (VOID *) &mRamDisk);
    ASSERT_EFI_ERROR (Status);
  }
  Status = mRamDisk->Unregister (RamDiskDevicePath);
  ASSERT_EFI_ERROR (Status);
  FreePages (RamDiskBuffer, RamDiskSizeInPages);
}

EFI_DEVICE_PATH_PROTOCOL *
BmExpandLoadFile (
  IN  EFI_HANDLE                      LoadFileHandle,
  IN  EFI_DEVICE_PATH_PROTOCOL        *FilePath
  )
{
  EFI_STATUS                          Status;
  EFI_LOAD_FILE_PROTOCOL              *LoadFile;
  VOID                                *FileBuffer;
  EFI_HANDLE                          RamDiskHandle;
  UINTN                               BufferSize;
  EFI_DEVICE_PATH_PROTOCOL            *FullPath;

  Status = gBS->OpenProtocol (
                  LoadFileHandle,
                  &gEfiLoadFileProtocolGuid,
                  (VOID **) &LoadFile,
                  gImageHandle,
                  NULL,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  ASSERT_EFI_ERROR (Status);

  FileBuffer = NULL;
  BufferSize = 0;
  Status = LoadFile->LoadFile (LoadFile, FilePath, TRUE, &BufferSize, FileBuffer);
  if ((Status != EFI_WARN_FILE_SYSTEM) && (Status != EFI_BUFFER_TOO_SMALL)) {
    return NULL;
  }

  if (Status == EFI_BUFFER_TOO_SMALL) {
    //
    // The load option buffer is directly returned by LoadFile.
    //
    return DuplicateDevicePath (DevicePathFromHandle (LoadFileHandle));
  }

  //
  // The load option resides in a RAM disk.
  //
  FileBuffer = AllocateReservedPages (EFI_SIZE_TO_PAGES (BufferSize));
  if (FileBuffer == NULL) {
    DEBUG_CODE (
      EFI_DEVICE_PATH *LoadFilePath;
      CHAR16          *LoadFileText;
      CHAR16          *FileText;

      LoadFilePath = DevicePathFromHandle (LoadFileHandle);
      if (LoadFilePath == NULL) {
        LoadFileText = NULL;
      } else {
        LoadFileText = ConvertDevicePathToText (LoadFilePath, FALSE, FALSE);
      }
      FileText = ConvertDevicePathToText (FilePath, FALSE, FALSE);

      DEBUG ((
        DEBUG_ERROR,
        "%a:%a: failed to allocate reserved pages: "
        "BufferSize=%Lu LoadFile=\"%s\" FilePath=\"%s\"\n",
        gEfiCallerBaseName,
        __FUNCTION__,
        (UINT64)BufferSize,
        LoadFileText,
        FileText
        ));

      if (FileText != NULL) {
        FreePool (FileText);
      }
      if (LoadFileText != NULL) {
        FreePool (LoadFileText);
      }
      );
    return NULL;
  }

  Status = LoadFile->LoadFile (LoadFile, FilePath, TRUE, &BufferSize, FileBuffer);
  if (EFI_ERROR (Status)) {
    FreePages (FileBuffer, EFI_SIZE_TO_PAGES (BufferSize));
    return NULL;
  }

  FullPath = BmExpandNetworkFileSystem (LoadFileHandle, &RamDiskHandle);
  if (FullPath == NULL) {
    //
    // Free the memory occupied by the RAM disk if there is no BlockIo or SimpleFileSystem instance.
    //
    BmDestroyRamDisk (DevicePathFromHandle (RamDiskHandle));
  }

  return FullPath;
}

EFI_DEVICE_PATH_PROTOCOL *
BmExpandLoadFiles (
  IN  EFI_DEVICE_PATH_PROTOCOL        *FilePath
  )
{
  EFI_STATUS                      Status;
  EFI_HANDLE                      Handle;
  EFI_HANDLE                      *Handles;
  UINTN                           HandleCount;
  UINTN                           Index;
  EFI_DEVICE_PATH_PROTOCOL        *Node;

  //
  // Get file buffer from load file instance.
  //
  Node = FilePath;
  Status = gBS->LocateDevicePath (&gEfiLoadFileProtocolGuid, &Node, &Handle);
  if (!EFI_ERROR (Status) && IsDevicePathEnd (Node)) {
    //
    // When wide match happens, pass full device path to LoadFile (),
    // otherwise, pass remaining device path to LoadFile ().
    //
    FilePath = Node;
  } else {
    Handle = NULL;
    //
    // Use wide match algorithm to find one when
    //  cannot find a LoadFile instance to exactly match the FilePath
    //
    Status = gBS->LocateHandleBuffer (
                    ByProtocol,
                    &gEfiLoadFileProtocolGuid,
                    NULL,
                    &HandleCount,
                    &Handles
                    );
    if (EFI_ERROR (Status)) {
      Handles = NULL;
      HandleCount = 0;
    }
    for (Index = 0; Index < HandleCount; Index++) {
      if (BmMatchHttpBootDevicePath (DevicePathFromHandle (Handles[Index]), FilePath)) {
        Handle = Handles[Index];
        break;
      }
    }
    if (Handles != NULL) {
      FreePool (Handles);
    }
  }

  if (Handle == NULL) {
    return NULL;
  }

  return BmExpandLoadFile (Handle, FilePath);
}

VOID *
EFIAPI
EfiBootManagerGetLoadOptionBuffer (
  IN  EFI_DEVICE_PATH_PROTOCOL          *FilePath,
  OUT EFI_DEVICE_PATH_PROTOCOL          **FullPath,
  OUT UINTN                             *FileSize
  )
{
  *FullPath = NULL;

  EfiBootManagerConnectDevicePath (FilePath, NULL);
  return BmGetNextLoadOptionBuffer (LoadOptionTypeMax, FilePath, FullPath, FileSize);
}

EFI_DEVICE_PATH_PROTOCOL *
BmGetNextLoadOptionDevicePath (
  IN  EFI_DEVICE_PATH_PROTOCOL          *FilePath,
  IN  EFI_DEVICE_PATH_PROTOCOL          *FullPath
  )
{
  EFI_HANDLE                      Handle;
  EFI_DEVICE_PATH_PROTOCOL        *Node;
  EFI_STATUS                      Status;

  ASSERT (FilePath != NULL);

  //
  // Boot from media device by adding a default file name \EFI\BOOT\BOOT{machine type short-name}.EFI
  //
  Node = FilePath;
  Status = gBS->LocateDevicePath (&gEfiSimpleFileSystemProtocolGuid, &Node, &Handle);
  if (EFI_ERROR (Status)) {
    Status = gBS->LocateDevicePath (&gEfiBlockIoProtocolGuid, &Node, &Handle);
  }

  if (!EFI_ERROR (Status) && IsDevicePathEnd (Node)) {
    return BmExpandMediaDevicePath (FilePath, FullPath);
  }

  //
  // Expand the short-form device path to full device path
  //
  if ((DevicePathType (FilePath) == MEDIA_DEVICE_PATH) &&
      (DevicePathSubType (FilePath) == MEDIA_HARDDRIVE_DP)) {
    //
    // Expand the Harddrive device path
    //
    if (FullPath == NULL) {
      return BmExpandPartitionDevicePath (FilePath);
    } else {
      return NULL;
    }
  } else if ((DevicePathType (FilePath) == MEDIA_DEVICE_PATH) &&
             (DevicePathSubType (FilePath) == MEDIA_FILEPATH_DP)) {
    //
    // Expand the File-path device path
    //
    return BmExpandFileDevicePath (FilePath, FullPath);
  } else if ((DevicePathType (FilePath) == MESSAGING_DEVICE_PATH) &&
             (DevicePathSubType (FilePath) == MSG_URI_DP)) {
    //
    // Expand the URI device path
    //
    return BmExpandUriDevicePath (FilePath, FullPath);
  } else {
    Node = FilePath;
    Status = gBS->LocateDevicePath (&gEfiUsbIoProtocolGuid, &Node, &Handle);
    if (EFI_ERROR (Status)) {
      //
      // Only expand the USB WWID/Class device path
      // when FilePath doesn't point to a physical UsbIo controller.
      // Otherwise, infinite recursion will happen.
      //
      for (Node = FilePath; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
        if ((DevicePathType (Node) == MESSAGING_DEVICE_PATH) &&
            ((DevicePathSubType (Node) == MSG_USB_CLASS_DP) || (DevicePathSubType (Node) == MSG_USB_WWID_DP))) {
          break;
        }
      }

      //
      // Expand the USB WWID/Class device path
      //
      if (!IsDevicePathEnd (Node)) {
        if (FilePath == Node) {
          //
          // Boot Option device path starts with USB Class or USB WWID device path.
          // For Boot Option device path which doesn't begin with the USB Class or
          // USB WWID device path, it's not needed to connect again here.
          //
          BmConnectUsbShortFormDevicePath (FilePath);
        }
        return BmExpandUsbDevicePath (FilePath, FullPath, Node);
      }
    }
  }

  //
  // For the below cases, FilePath only expands to one Full path.
  // So just handle the case when FullPath == NULL.
  //
  if (FullPath != NULL) {
    return NULL;
  }

  //
  // Load option resides in FV.
  //
  if (BmIsFvFilePath (FilePath)) {
    return BmAdjustFvFilePath (FilePath);
  }

  //
  // Load option resides in Simple File System.
  //
  Node   = FilePath;
  Status = gBS->LocateDevicePath (&gEfiSimpleFileSystemProtocolGuid, &Node, &Handle);
  if (!EFI_ERROR (Status)) {
    return DuplicateDevicePath (FilePath);
  }

  //
  // Last chance to try: Load option may be loaded through LoadFile.
  //
  return BmExpandLoadFiles (FilePath);
}

BOOLEAN
BmMatchPartitionDevicePathNode (
  IN  EFI_DEVICE_PATH_PROTOCOL   *BlockIoDevicePath,
  IN  HARDDRIVE_DEVICE_PATH      *HardDriveDevicePath
  )
{
  HARDDRIVE_DEVICE_PATH     *Node;

  if ((BlockIoDevicePath == NULL) || (HardDriveDevicePath == NULL)) {
    return FALSE;
  }

  //
  // Match all the partition device path nodes including the nested partition nodes
  //
  while (!IsDevicePathEnd (BlockIoDevicePath)) {
    if ((DevicePathType (BlockIoDevicePath) == MEDIA_DEVICE_PATH) &&
        (DevicePathSubType (BlockIoDevicePath) == MEDIA_HARDDRIVE_DP)
        ) {
      //
      // See if the harddrive device path in blockio matches the orig Hard Drive Node
      //
      Node = (HARDDRIVE_DEVICE_PATH *) BlockIoDevicePath;

      //
      // Match Signature and PartitionNumber.
      // Unused bytes in Signature are initiaized with zeros.
      //
      if ((Node->PartitionNumber == HardDriveDevicePath->PartitionNumber) &&
          (Node->MBRType == HardDriveDevicePath->MBRType) &&
          (Node->SignatureType == HardDriveDevicePath->SignatureType) &&
          (CompareMem (Node->Signature, HardDriveDevicePath->Signature, sizeof (Node->Signature)) == 0)) {
        return TRUE;
      }
    }

    BlockIoDevicePath = NextDevicePathNode (BlockIoDevicePath);
  }

  return FALSE;
}

EFI_DEVICE_PATH_PROTOCOL *
EFIAPI
EfiBootManagerGetNextLoadOptionDevicePath (
  IN  EFI_DEVICE_PATH_PROTOCOL          *FilePath,
  IN  EFI_DEVICE_PATH_PROTOCOL          *FullPath
  )
{
  return BmGetNextLoadOptionDevicePath(FilePath, FullPath);
}
