/** @file
  Execute a registered UEFI boot option within an application.

  Copyright (c) 2011 - 2019, Intel Corporation. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "InternalBm.h"

VOID
EFIAPI
EfiBootManagerBoot (
  IN EFI_BOOT_MANAGER_LOAD_OPTION *Option
  )
{
  EFI_STATUS Status;
  EFI_HANDLE Image;
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  EFI_DEVICE_PATH_PROTOCOL *RamDisk;
  VOID *Buffer;
  UINTN Size;
  UINT16 Number;
  BOOLEAN Watchdog;

  if (Option == NULL) { return; }
  Option->Status = EFI_INVALID_PARAMETER;
  if ((Option->OptionType != LoadOptionTypeBoot) || (Option->FilePath == NULL) ||
      (Option->OptionNumber >= LoadOptionNumberMax)) { return; }
  if (DevicePathType (Option->FilePath) == BBS_DEVICE_PATH) {
    Option->Status = EFI_UNSUPPORTED;
    return;
  }
  Number = (UINT16)Option->OptionNumber;
  Status = gRT->SetVariable (L"BootCurrent", &gEfiGlobalVariableGuid,
                            EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                            sizeof (Number), &Number);
  if (EFI_ERROR (Status)) { Option->Status = Status; return; }

  EfiSignalEventReadyToBoot ();
  Image = NULL;
  Path = RamDisk = NULL;
  Buffer = NULL;
  Watchdog = FALSE;
  EfiBootManagerConnectDevicePath (Option->FilePath, NULL);
  Buffer = BmGetNextLoadOptionBuffer (LoadOptionTypeBoot, Option->FilePath, &Path, &Size);
  if (Buffer == NULL) { Status = EFI_NOT_FOUND; goto Done; }
  RamDisk = BmGetRamDiskDevicePath (Path);
  Status = gBS->LoadImage (TRUE, gImageHandle, Path, Buffer, Size, &Image);
  if (EFI_ERROR (Status)) {
    if ((Status == EFI_SECURITY_VIOLATION) && (Image != NULL)) { gBS->UnloadImage (Image); }
    goto Done;
  }
  Status = gBS->HandleProtocol (Image, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (EFI_ERROR (Status)) { gBS->UnloadImage (Image); goto Done; }
  if (!BmIsAutoCreateBootOption (Option)) {
    Loaded->LoadOptions = Option->OptionalData;
    Loaded->LoadOptionsSize = Option->OptionalDataSize;
  }
  gBS->SetWatchdogTimer (300, 0, 0, NULL);
  Watchdog = TRUE;
  Status = gBS->StartImage (Image, &Option->ExitDataSize, &Option->ExitData);
Done:
  if (Watchdog) { gBS->SetWatchdogTimer (0, 0, 0, NULL); }
  if (Buffer != NULL) { FreePool (Buffer); }
  if (Path != NULL) { FreePool (Path); }
  if (RamDisk != NULL) { BmDestroyRamDisk (RamDisk); FreePool (RamDisk); }
  Option->Status = Status;
  gRT->SetVariable (L"BootCurrent", &gEfiGlobalVariableGuid, 0, 0, NULL);
}

VOID
EFIAPI
EfiBootManagerConnectAll (
  VOID
  )
{
  EFI_HANDLE *Handles;
  UINTN Count;
  UINTN Index;

  if (EFI_ERROR (gBS->LocateHandleBuffer (AllHandles, NULL, NULL, &Count, &Handles))) { return; }
  for (Index = 0; Index < Count; Index++) {
    gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
  }
  FreePool (Handles);
}
