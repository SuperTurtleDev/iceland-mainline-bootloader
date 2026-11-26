/** @file
  Internal declarations for standard boot-option parsing and execution.

  Copyright (c) 2004 - 2019, Intel Corporation. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef INTERNAL_BDS_BOOT_MANAGER_H_
#define INTERNAL_BDS_BOOT_MANAGER_H_

#include <Uefi.h>
#include <Pi/PiFirmwareFile.h>
#include <Pi/PiFirmwareVolume.h>
#include <IndustryStandard/Pci.h>
#include <IndustryStandard/PeImage.h>
#include <Protocol/BlockIo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadFile.h>
#include <Protocol/DevicePath.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/PciIo.h>
#include <Protocol/UsbIo.h>
#include <Protocol/RamDisk.h>
#include <Guid/GlobalVariable.h>
#include <Library/PrintLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseLib.h>
#include <Library/DevicePathLib.h>
#include <Library/PeCoffGetEntryPointLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/DxeServicesLib.h>

#define BM_OPTION_NAME_LEN  sizeof ("PlatformRecovery####")
extern CHAR16 *mBmLoadOptionName[];

EFI_STATUS BmGetFreeOptionNumber (IN EFI_BOOT_MANAGER_LOAD_OPTION_TYPE Type, OUT UINT16 *Number);
BOOLEAN BmMatchPartitionDevicePathNode (IN EFI_DEVICE_PATH_PROTOCOL *BlockPath, IN HARDDRIVE_DEVICE_PATH *HardDrive);
EFI_STATUS BmConnectUsbShortFormDevicePath (IN EFI_DEVICE_PATH_PROTOCOL *Path);
BOOLEAN BmMatchDevicePaths (IN EFI_DEVICE_PATH_PROTOCOL *Multi, IN EFI_DEVICE_PATH_PROTOCOL *Single);
EFI_DEVICE_PATH_PROTOCOL *BmDelPartMatchInstance (IN EFI_DEVICE_PATH_PROTOCOL *Multi, IN EFI_DEVICE_PATH_PROTOCOL *Single);
VOID BmPrintDp (IN EFI_DEVICE_PATH_PROTOCOL *Path);
UINTN BmCharToUint (IN CHAR16 Character);
BOOLEAN BmIsAutoCreateBootOption (IN EFI_BOOT_MANAGER_LOAD_OPTION *Option);
EFI_DEVICE_PATH_PROTOCOL *BmExpandLoadFile (IN EFI_HANDLE Handle, IN EFI_DEVICE_PATH_PROTOCOL *Path);
EFI_DEVICE_PATH_PROTOCOL *BmGetRamDiskDevicePath (IN EFI_DEVICE_PATH_PROTOCOL *Path);
VOID BmDestroyRamDisk (IN EFI_DEVICE_PATH_PROTOCOL *Path);
EFI_DEVICE_PATH_PROTOCOL *BmGetNextLoadOptionDevicePath (IN EFI_DEVICE_PATH_PROTOCOL *Path, IN EFI_DEVICE_PATH_PROTOCOL *Previous);
VOID *BmGetNextLoadOptionBuffer (IN EFI_BOOT_MANAGER_LOAD_OPTION_TYPE Type, IN EFI_DEVICE_PATH_PROTOCOL *Path, OUT EFI_DEVICE_PATH_PROTOCOL **FullPath, OUT UINTN *Size);
#endif
