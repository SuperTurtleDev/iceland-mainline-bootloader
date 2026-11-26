/** @file
  Device-path and variable-name helpers for application-mode boot selection.

  Copyright (c) 2011 - 2019, Intel Corporation. All rights reserved.
  (C) Copyright 2015-2018 Hewlett Packard Enterprise Development LP
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "InternalBm.h"

EFI_DEVICE_PATH_PROTOCOL *
BmDelPartMatchInstance (
  IN     EFI_DEVICE_PATH_PROTOCOL  *Multi,
  IN     EFI_DEVICE_PATH_PROTOCOL  *Single
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Instance;
  EFI_DEVICE_PATH_PROTOCOL  *NewDevicePath;
  EFI_DEVICE_PATH_PROTOCOL  *TempNewDevicePath;
  UINTN                     InstanceSize;
  UINTN                     SingleDpSize;

  NewDevicePath     = NULL;
  TempNewDevicePath = NULL;

  if (Multi == NULL || Single == NULL) {
    return Multi;
  }

  Instance        = GetNextDevicePathInstance (&Multi, &InstanceSize);
  SingleDpSize    = GetDevicePathSize (Single) - END_DEVICE_PATH_LENGTH;
  InstanceSize   -= END_DEVICE_PATH_LENGTH;

  while (Instance != NULL) {

    if (CompareMem (Instance, Single, MIN (SingleDpSize, InstanceSize)) != 0) {
      //
      // Append the device path instance which does not match with Single
      //
      TempNewDevicePath = NewDevicePath;
      NewDevicePath = AppendDevicePathInstance (NewDevicePath, Instance);
      if (TempNewDevicePath != NULL) {
        FreePool(TempNewDevicePath);
      }
    }
    FreePool(Instance);
    Instance      = GetNextDevicePathInstance (&Multi, &InstanceSize);
    InstanceSize -= END_DEVICE_PATH_LENGTH;
  }

  return NewDevicePath;
}

BOOLEAN
BmMatchDevicePaths (
  IN  EFI_DEVICE_PATH_PROTOCOL  *Multi,
  IN  EFI_DEVICE_PATH_PROTOCOL  *Single
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePathInst;
  UINTN                     Size;

  if (Multi == NULL || Single  == NULL) {
    return FALSE;
  }

  DevicePath     = Multi;
  DevicePathInst = GetNextDevicePathInstance (&DevicePath, &Size);

  //
  // Search for the match of 'Single' in 'Multi'
  //
  while (DevicePathInst != NULL) {
    //
    // If the single device path is found in multiple device paths,
    // return success
    //
    if (CompareMem (Single, DevicePathInst, Size) == 0) {
      FreePool (DevicePathInst);
      return TRUE;
    }

    FreePool (DevicePathInst);
    DevicePathInst = GetNextDevicePathInstance (&DevicePath, &Size);
  }

  return FALSE;
}

VOID
BmPrintDp (
  EFI_DEVICE_PATH_PROTOCOL            *DevicePath
  )
{
  CHAR16                              *Str;

  Str = ConvertDevicePathToText (DevicePath, FALSE, FALSE);
  DEBUG ((EFI_D_INFO, "%s", Str));
  if (Str != NULL) {
    FreePool (Str);
  }
}

UINTN
BmCharToUint (
  IN CHAR16                           Char
  )
{
  if ((Char >= L'0') && (Char <= L'9')) {
    return (Char - L'0');
  }

  if ((Char >= L'A') && (Char <= L'F')) {
    return (Char - L'A' + 0xA);
  }

  return (UINTN) -1;
}
