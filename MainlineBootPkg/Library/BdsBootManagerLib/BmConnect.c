/** @file
  Controller connection for application-mode boot selection.

  Copyright (c) 2011 - 2019, Intel Corporation. All rights reserved.
  (C) Copyright 2015-2018 Hewlett Packard Enterprise Development LP
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "InternalBm.h"

EFI_STATUS
EFIAPI
EfiBootManagerConnectDevicePath (
  IN  EFI_DEVICE_PATH_PROTOCOL  *DevicePathToConnect,
  OUT EFI_HANDLE                *MatchingHandle          OPTIONAL
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *RemainingDevicePath;
  EFI_HANDLE                Handle;
  EFI_HANDLE                PreviousHandle;

  if (DevicePathToConnect == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Start the real work of connect with RemainingDevicePath
  //
  PreviousHandle = NULL;
  do {
    //
    // Find the handle that best matches the Device Path. If it is only a
    // partial match the remaining part of the device path is returned in
    // RemainingDevicePath.
    //
    RemainingDevicePath = DevicePathToConnect;
    Status              = gBS->LocateDevicePath (&gEfiDevicePathProtocolGuid, &RemainingDevicePath, &Handle);
    if (!EFI_ERROR (Status)) {
      if (Handle == PreviousHandle) {
        Status = EFI_NOT_FOUND;

      }


      if (!EFI_ERROR (Status)) {
        PreviousHandle = Handle;
        //
        // Connect all drivers that apply to Handle and RemainingDevicePath,
        // the Recursive flag is FALSE so only one level will be expanded.
        //
        // If ConnectController fails to find a driver, then still give the chance to
        // do dispatch, because partial RemainingDevicePath may be in the new FV
        //
        // 1. If the connect fail, RemainingDevicepath and handle will not
        //    change, so next time will do the dispatch, then dispatch's status
        //    will take effect
        // 2. If the connect success, the RemainingDevicepath and handle will
        //    change, then avoid the dispatch, we have chance to continue the
        //    next connection
        //
        Status = gBS->ConnectController (Handle, NULL, RemainingDevicePath, FALSE);
        if (Status == EFI_NOT_FOUND) {
          Status = EFI_SUCCESS;
        }
        if (MatchingHandle != NULL) {
          *MatchingHandle = Handle;
        }
      }
    }
    //
    // Loop until RemainingDevicePath is an empty device path
    //
  } while (!EFI_ERROR (Status) && !IsDevicePathEnd (RemainingDevicePath));

  ASSERT (EFI_ERROR (Status) || IsDevicePathEnd (RemainingDevicePath));

  return Status;
}

EFI_STATUS
BmConnectUsbShortFormDevicePath (
  IN EFI_DEVICE_PATH_PROTOCOL   *DevicePath
  )
{
  EFI_STATUS                            Status;
  EFI_HANDLE                            *Handles;
  UINTN                                 HandleCount;
  UINTN                                 Index;
  EFI_PCI_IO_PROTOCOL                   *PciIo;
  UINT8                                 Class[3];
  BOOLEAN                               AtLeastOneConnected;

  //
  // Check the passed in parameters
  //
  if (DevicePath == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((DevicePathType (DevicePath) != MESSAGING_DEVICE_PATH) ||
      ((DevicePathSubType (DevicePath) != MSG_USB_CLASS_DP) && (DevicePathSubType (DevicePath) != MSG_USB_WWID_DP))
     ) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Find the usb host controller firstly, then connect with the remaining device path
  //
  AtLeastOneConnected = FALSE;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiPciIoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (!EFI_ERROR (Status)) {
    for (Index = 0; Index < HandleCount; Index++) {
      Status = gBS->HandleProtocol (
                      Handles[Index],
                      &gEfiPciIoProtocolGuid,
                      (VOID **) &PciIo
                      );
      if (!EFI_ERROR (Status)) {
        //
        // Check whether the Pci device is the wanted usb host controller
        //
        Status = PciIo->Pci.Read (PciIo, EfiPciIoWidthUint8, 0x09, 3, &Class);
        if (!EFI_ERROR (Status) &&
            ((PCI_CLASS_SERIAL == Class[2]) && (PCI_CLASS_SERIAL_USB == Class[1]))
           ) {
          Status = gBS->ConnectController (
                          Handles[Index],
                          NULL,
                          DevicePath,
                          FALSE
                          );
          if (!EFI_ERROR(Status)) {
            AtLeastOneConnected = TRUE;
          }
        }
      }
    }

    if (Handles != NULL) {
      FreePool (Handles);
    }
  }

  return AtLeastOneConnected ? EFI_SUCCESS : EFI_NOT_FOUND;
}
