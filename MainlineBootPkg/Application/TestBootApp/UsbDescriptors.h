/** @file UsbDescriptors.h

  USB descriptors for the fastboot interface (trimmed copy of
  QcomModulePkg/Library/FastbootLib/UsbDescriptors.h).

  SPDX-License-Identifier: BSD-3-Clause-Clear
**/

#ifndef __TESTBOOT_USB_DESCRIPTORS_H__
#define __TESTBOOT_USB_DESCRIPTORS_H__

#include <Uefi.h>
#include <Protocol/EFIUsbDevice.h>
#include <Protocol/EFIUsbEx.h>

#define USBLB_BULK_EP 1
#define ENDPOINT_ADDR(EndpointIndex, Tx)  ((EndpointIndex) | ((Tx) ? 0x80 : 0x00))

/* Google fastboot VID/PID (same as ABL FAST_BOOT_VENDOR / FAST_BOOT_IDPRODUCT) */
#define FAST_BOOT_VENDOR     0x18d1
#define FAST_BOOT_IDPRODUCT  0xD00D

#define USB_STRING_DESC_COUNT 5

extern EFI_USB_DEVICE_QUALIFIER_DESCRIPTOR  DeviceQualifier;
extern EFI_USB_STRING_DESCRIPTOR           *StrDescriptors[USB_STRING_DESC_COUNT];
extern CONST VOID                          *BinaryObjectStorePtr;

/**
  Fill in the serial string descriptor and return pointers to the static
  HS/SS device + configuration descriptors. *Descriptors / *SSDescriptors are
  AllocateZeroPool'd arrays (one entry per configuration) the caller frees.

  @param[in] SerialAscii  NUL terminated ASCII serial number (<= 30 chars).
**/
EFI_STATUS
BuildDefaultDescriptors (
  IN  CONST CHAR8             *SerialAscii,
  OUT USB_DEVICE_DESCRIPTOR  **DevDesc,
  OUT VOID                  **Descriptors,
  OUT USB_DEVICE_DESCRIPTOR  **SSDevDesc,
  OUT VOID                  **SSDescriptors
  );

#endif /* __TESTBOOT_USB_DESCRIPTORS_H__ */
