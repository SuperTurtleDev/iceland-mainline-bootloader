/** @file UsbDescriptors.c

  USB descriptors for the fastboot interface. Copied from
  QcomModulePkg/Library/FastbootLib/UsbDescriptors.c and FastbootMain.c
  (BinaryObjectStore); the only change is that the serial number is passed
  in by the caller instead of being fetched via BoardSerialNum().

  SPDX-License-Identifier: BSD-3-Clause-Clear
**/

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include "UsbDescriptors.h"

#define MAX_DESC_LEN 62

STATIC
EFI_USB_DEVICE_DESCRIPTOR
DeviceDescriptor = {
    sizeof (EFI_USB_DEVICE_DESCRIPTOR), // uint8  bLength;
    USB_DESC_TYPE_DEVICE,               // uint8  bDescriptorType;
    0x0210,                             // uint16 bcdUSB;
    0x00,                               // uint8  bDeviceClass;
    0x00,                               // uint8  bDeviceSubClass;
    0x00,                               // uint8  bDeviceProtocol;
    64,                                 // uint8  bMaxPacketSize0;
    FAST_BOOT_VENDOR,                   // uint16 idVendor;
    FAST_BOOT_IDPRODUCT,                // uint16 idProduct;
    0x100,                              // uint16 bcdDevice;
    1,                                  // uint8  iManufacturer;
    2,                                  // uint8  iProduct;
    3,                                  // uint8  iSerialNumber;
    1                                   // uint8  bNumConfigurations;
};

STATIC
EFI_USB_DEVICE_DESCRIPTOR
SSDeviceDescriptor = {
    sizeof (EFI_USB_DEVICE_DESCRIPTOR), // uint8  bLength;
    USB_DESC_TYPE_DEVICE,               // uint8  bDescriptorType;
    0x0300,                             // uint16 bcdUSB;
    0x00,                               // uint8  bDeviceClass;
    0x00,                               // uint8  bDeviceSubClass;
    0x00,                               // uint8  bDeviceProtocol;
    9,                                  // uint8  bMaxPacketSize0;
    FAST_BOOT_VENDOR,                   // uint16 idVendor;
    FAST_BOOT_IDPRODUCT,                // uint16 idProduct;
    0x100,                              // uint16 bcdDevice;
    1,                                  // uint8  iManufacturer;
    2,                                  // uint8  iProduct;
    3,                                  // uint8  iSerialNumber;
    1                                   // uint8  bNumConfigurations;
};

EFI_USB_DEVICE_QUALIFIER_DESCRIPTOR
DeviceQualifier = {
    sizeof (EFI_USB_DEVICE_QUALIFIER_DESCRIPTOR), // uint8  bLength;
    USB_DESC_TYPE_DEVICE_QUALIFIER,               // uint8  bDescriptorType;
    0x0200,                                       // uint16 bcdUSB;
    0xff,                                         // uint8  bDeviceClass;
    0xff,                                         // uint8  bDeviceSubClass;
    0xff,                                         // uint8  bDeviceProtocol;
    64,                                           // uint8  bMaxPacketSize0;
    1,                                            // uint8  bNumConfigurations;
    0                                             // uint8  bReserved;
};

#pragma pack(1)
typedef struct _SSCfgDescTree {
  EFI_USB_CONFIG_DESCRIPTOR                 ConfigDescriptor;
  EFI_USB_INTERFACE_DESCRIPTOR              InterfaceDescriptor;
  EFI_USB_ENDPOINT_DESCRIPTOR               EndpointDescriptor0;
  EFI_USB_SS_ENDPOINT_COMPANION_DESCRIPTOR  EndpointCompanionDescriptor0;
  EFI_USB_ENDPOINT_DESCRIPTOR               EndpointDescriptor1;
  EFI_USB_SS_ENDPOINT_COMPANION_DESCRIPTOR  EndpointCompanionDescriptor1;
} SS_CONFIG_DESCRIPTORS;

typedef struct _CfgDescTree {
  EFI_USB_CONFIG_DESCRIPTOR     ConfigDescriptor;
  EFI_USB_INTERFACE_DESCRIPTOR  InterfaceDescriptor;
  EFI_USB_ENDPOINT_DESCRIPTOR   EndpointDescriptor0;
  EFI_USB_ENDPOINT_DESCRIPTOR   EndpointDescriptor1;
} CONFIG_DESCRIPTORS;

typedef struct {
  EFI_USB_BOS_DESCRIPTOR                 BosDescriptor;
  EFI_USB_USB_20_EXTENSION_DESCRIPTOR    Usb2ExtDescriptor;
  EFI_USB_SUPERSPEED_USB_DESCRIPTOR      SsUsbDescriptor;
  EFI_USB_SUPERSPEEDPLUS_USB_DESCRIPTOR  SspUsbDescriptor;
} BOS_DESCRIPTORS;
#pragma pack()

STATIC SS_CONFIG_DESCRIPTORS TotalSSConfigDescriptor = {
    {
        sizeof (EFI_USB_CONFIG_DESCRIPTOR), // uint8  bLength;
        USB_DESC_TYPE_CONFIG,               // uint8  bDescriptorType;
        sizeof (SS_CONFIG_DESCRIPTORS),     // uint16 wTotalLength;
        1,                                  // uint8  bNumInterfaces;
        1,                                  // uint8  bConfigurationValue;
        0,                                  // uint8  iConfiguration;
        0x80,                               // uint8  bmAttributes;
        0x10                                // uint8  bMaxPower;
    },
    {sizeof (EFI_USB_INTERFACE_DESCRIPTOR), // uint8  bLength;
     USB_DESC_TYPE_INTERFACE,               // uint8  bDescriptorType;
     0,                                     // uint8  bInterfaceNumber;
     0,                                     // uint8  bAlternateSetting;
     2,                                     // uint8  bNumEndpoints;
     0xff,                                  // uint8  bInterfaceClass;
     0x42,                                  // uint8  bInterfaceSubClass;
     0x03,                                  // uint8  bInterfaceProtocol;
     4},                                    // uint8  iInterface;
    {
        sizeof (EFI_USB_ENDPOINT_DESCRIPTOR), // uint8  bLength;
        USB_DESC_TYPE_ENDPOINT,               // uint8  bDescriptorType;
        ENDPOINT_ADDR (USBLB_BULK_EP, TRUE),  // uint8  bEndpointAddress;
        USB_ENDPOINT_BULK,                    // uint8  bmAttributes;
        1024, // uint16 wMaxPacketSize; SS=1024, HS=512 , FS=64
        0     // uint8  bInterval;
    },
    {
        sizeof (EFI_USB_SS_ENDPOINT_COMPANION_DESCRIPTOR), // uint8 bLength
        USB_DESC_TYPE_SS_ENDPOINT_COMPANION, // uint8 bDescriptorType
        4, // uint8 bMaxBurst,    0 => max burst 1
        0, // uint8 bmAttributes, 0 => no stream
        0, // uint8 wBytesPerInterval. Does not apply to BULK
    },
    {
        sizeof (EFI_USB_ENDPOINT_DESCRIPTOR), // uint8  bLength;
        USB_DESC_TYPE_ENDPOINT,               // uint8  bDescriptorType;
        ENDPOINT_ADDR (USBLB_BULK_EP, FALSE), // uint8  bEndpointAddress;
        USB_ENDPOINT_BULK,                    // uint8  bmAttributes;
        1024, // uint16 wMaxPacketSize; SS=1024, HS=512 , FS=64
        0     // uint8  bInterval;
    },
    {
        sizeof (EFI_USB_SS_ENDPOINT_COMPANION_DESCRIPTOR), // uint8 bLength
        USB_DESC_TYPE_SS_ENDPOINT_COMPANION, // uint8 bDescriptorType
        4, // uint8 bMaxBurst,    0 => max burst 1
        0, // uint8 bmAttributes, 0 => no stream
        0, // uint8 wBytesPerInterval. Does not apply to BULK
    }};

STATIC CONFIG_DESCRIPTORS TotalConfigDescriptor = {
    {
        sizeof (EFI_USB_CONFIG_DESCRIPTOR), // uint8  bLength;
        USB_DESC_TYPE_CONFIG,               // uint8  bDescriptorType;
        sizeof (CONFIG_DESCRIPTORS),        // uint16 wTotalLength;
        1,                                  // uint8  bNumInterfaces;
        1,                                  // uint8  bConfigurationValue;
        0,                                  // uint8  iConfiguration;
        0x80,                               // uint8  bmAttributes;
        0x50                                // uint8  bMaxPower;
    },
    {sizeof (EFI_USB_INTERFACE_DESCRIPTOR), // uint8  bLength;
     USB_DESC_TYPE_INTERFACE,               // uint8  bDescriptorType;
     0,                                     // uint8  bInterfaceNumber;
     0,                                     // uint8  bAlternateSetting;
     2,                                     // uint8  bNumEndpoints;
     0xff,                                  // uint8  bInterfaceClass;
     0x42,                                  // uint8  bInterfaceSubClass;
     0x03,                                  // uint8  bInterfaceProtocol;
     4},                                    // uint8  iInterface;
    {
        sizeof (EFI_USB_ENDPOINT_DESCRIPTOR), // uint8  bLength;
        USB_DESC_TYPE_ENDPOINT,               // uint8  bDescriptorType;
        ENDPOINT_ADDR (USBLB_BULK_EP, TRUE),  // uint8  bEndpointAddress;
        USB_ENDPOINT_BULK,                    // uint8  bmAttributes;
        512, // uint16 wMaxPacketSize; HS=512 , FS=64
        0    // uint8  bInterval;
    },
    {
        sizeof (EFI_USB_ENDPOINT_DESCRIPTOR), // uint8  bLength;
        USB_DESC_TYPE_ENDPOINT,               // uint8  bDescriptorType;
        ENDPOINT_ADDR (USBLB_BULK_EP, FALSE), // uint8  bEndpointAddress;
        USB_ENDPOINT_BULK,                    // uint8  bmAttributes;
        512, // uint16 wMaxPacketSize; HS=512 , FS=64
        1    // uint8  bInterval;
    },
};

/* Binary Object Store (from FastbootMain.c) */
STATIC CONST BOS_DESCRIPTORS BinaryObjectStore = {
    // BOS Descriptor
    {
        sizeof (EFI_USB_BOS_DESCRIPTOR), // Descriptor Size
        USB_DESC_TYPE_BOS,               // Descriptor Type
        sizeof (BOS_DESCRIPTORS),        // Total Length
        3                                // Number of device capabilities
    },
    // USB2 Extension Desc
    {
        sizeof (EFI_USB_USB_20_EXTENSION_DESCRIPTOR), // Descriptor Size
        USB_DESC_TYPE_DEVICE_CAPABILITY,   // Device Capability Type descriptor
        USB_DEV_CAP_TYPE_USB_20_EXTENSION, // USB 2.0 Extension Capability Type
        0x6                                // Supported device level features
    },
    // Super Speed Device Capability Desc
    {
        sizeof (EFI_USB_SUPERSPEED_USB_DESCRIPTOR), // Descriptor Size
        USB_DESC_TYPE_DEVICE_CAPABILITY, // Device Capability Type descriptor
        USB_DEV_CAP_TYPE_SUPERSPEED_USB, // SuperSpeed Device Capability Type
        0x00,                            // Supported device level features
        0x0E, // Speeds Supported by the device: SS, HS and FS
        0x01, // Functionality support
        0x07, // U1 Device Exit Latency
        0x65  // U2 Device Exit Latency
    },
    // Super Speed Plus Device Capability Desc
    {
        sizeof (EFI_USB_SUPERSPEEDPLUS_USB_DESCRIPTOR), // Descriptor Size
        USB_DESC_TYPE_DEVICE_CAPABILITY, // Device Capability Type descriptor
        USB_DEV_CAP_TYPE_SUPERSPEEDPLUS_USB, //SuperSpeedPlus Device Capability
        0x00, // Reserved
        0x00000001, // Attributes
        0x1100, // Functionality Support
        0x00, // Reserved
        {0x000A4030, 0x000A40B0}, // Sublink Speed Attribute
    }
};

CONST VOID *BinaryObjectStorePtr = &BinaryObjectStore;

STATIC CONST UINT8 Str0Descriptor[4] = {
    sizeof (Str0Descriptor), USB_DESC_TYPE_STRING, 0x09, 0x04 // Langid : US_EN.
};

STATIC CONST UINT8 StrManufacturerDescriptor[14] = {
    sizeof (StrManufacturerDescriptor), USB_DESC_TYPE_STRING,
    'G', 0, 'o', 0, 'o', 0, 'g', 0, 'l', 0, 'e', 0,
};

STATIC UINT8 StrSerialDescriptor[MAX_DESC_LEN];

STATIC CONST UINT8 StrInterfaceDescriptor[18] = {
    sizeof (StrInterfaceDescriptor), USB_DESC_TYPE_STRING,
    'f', 0, 'a', 0, 's', 0, 't', 0, 'b', 0, 'o', 0, 'o', 0, 't', 0,
};

STATIC CONST UINT8 StrProductDescriptor[16] = {
    sizeof (StrProductDescriptor), USB_DESC_TYPE_STRING,
    'A', 0, 'n', 0, 'd', 0, 'r', 0, 'o', 0, 'i', 0, 'd', 0,
};

EFI_USB_STRING_DESCRIPTOR *StrDescriptors[USB_STRING_DESC_COUNT] = {
    (EFI_USB_STRING_DESCRIPTOR *)Str0Descriptor,
    (EFI_USB_STRING_DESCRIPTOR *)StrManufacturerDescriptor,
    (EFI_USB_STRING_DESCRIPTOR *)StrProductDescriptor,
    (EFI_USB_STRING_DESCRIPTOR *)StrSerialDescriptor,
    (EFI_USB_STRING_DESCRIPTOR *)StrInterfaceDescriptor};

EFI_STATUS
BuildDefaultDescriptors (
  IN  CONST CHAR8             *SerialAscii,
  OUT USB_DEVICE_DESCRIPTOR  **DevDesc,
  OUT VOID                  **Descriptors,
  OUT USB_DEVICE_DESCRIPTOR  **SSDevDesc,
  OUT VOID                  **SSDescriptors
  )
{
  UINT8   Index;
  UINT8   NumCfg;
  UINTN   Len;
  UINTN   i;
  VOID  **TempDescs;
  VOID  **TempSSDescs;

  if (SerialAscii == NULL || SerialAscii[0] == '\0') {
    SerialAscii = "unknown";
  }
  Len = AsciiStrLen (SerialAscii);
  /* 2 header bytes + 2 bytes per UTF-16 char must fit MAX_DESC_LEN */
  if (Len * 2 + 2 > MAX_DESC_LEN) {
    Len = (MAX_DESC_LEN - 2) / 2;
  }

  StrSerialDescriptor[0] = (UINT8)(Len * 2 + 2);
  StrSerialDescriptor[1] = USB_DESC_TYPE_STRING;
  for (i = 0; i < Len; i++) {
    StrSerialDescriptor[i * 2 + 2] = SerialAscii[i];
    StrSerialDescriptor[i * 2 + 3] = 0;
  }

  *DevDesc = &DeviceDescriptor;
  *SSDevDesc = &SSDeviceDescriptor;
  NumCfg = DeviceDescriptor.NumConfigurations;

  TempDescs = AllocateZeroPool (NumCfg * sizeof (VOID *));
  if (TempDescs == NULL) {
    DEBUG ((EFI_D_ERROR, "Error Allocating memory for HS config descriptors\n"));
    return EFI_OUT_OF_RESOURCES;
  }
  TempSSDescs = AllocateZeroPool (NumCfg * sizeof (VOID *));
  if (TempSSDescs == NULL) {
    DEBUG ((EFI_D_ERROR, "Error Allocating memory for SS config descriptors\n"));
    FreePool (TempDescs);
    return EFI_OUT_OF_RESOURCES;
  }
  for (Index = 0; Index < NumCfg; Index++) {
    TempDescs[Index] = &TotalConfigDescriptor;
    TempSSDescs[Index] = &TotalSSConfigDescriptor;
  }
  *Descriptors = TempDescs;
  *SSDescriptors = TempSSDescs;
  return EFI_SUCCESS;
}
