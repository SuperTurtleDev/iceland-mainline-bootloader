/** @file TestBootFastboot.c

  Self-contained fastboot over EFI_USB_DEVICE_PROTOCOL.

  Modelled on QcomModulePkg/Library/FastbootLib/{FastbootMain.c,
  FastbootCmds.c}: same USB bring-up sequence (InitUsbController event ->
  LocateProtocol -> StartEx -> AllocateTransferBuffer), same endpoint
  conventions (ENDPOINT_IN 0x01 = queue a receive from the host,
  ENDPOINT_OUT 0x81 = send a response to the host), same download state
  machine (DATA response, chunked receives of at most 16 MB to avoid ZLT
  issues). Everything touching partitions, storage, display, battery,
  threads and DeviceInfo has been removed.

  SPDX-License-Identifier: BSD-3-Clause-Clear
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/EFIUsbDevice.h>
#include <Protocol/EFIChipInfo.h>
#include <Library/MainlineBootLib.h>
#include <Protocol/GraphicsOutput.h>

#include "TestBootFastboot.h"
#include "UsbDescriptors.h"

#ifndef PRODUCT_NAME
#define PRODUCT_NAME "iceland"
#endif

/* --- constants (FastbootCmds.h / FastbootMain.h) ------------------------ */
#define ENDPOINT_IN                 0x01   /* queue a receive (host -> device) */
#define ENDPOINT_OUT                0x81   /* send a response (device -> host) */
#define USB_ENDPOINT_DIRECTION_OUT  0
#define USB_ENDPOINT_DIRECTION_IN   1
#define USB_INDEX_TO_EP(index)      ((index) & 0xf)
#define USB_INDEX_TO_EPDIR(index)   ((((index) >> 7) & 0x1) ? USB_ENDPOINT_DIRECTION_IN \
                                                            : USB_ENDPOINT_DIRECTION_OUT)

#define CMD_BUFFER_SIZE             (64 * 1024)     /* rx / tx transfer buffers */
#define MAX_RSP_SIZE                64
#define MAX_FASTBOOT_COMMAND_SIZE   64
#define ASCII_HEX_STRING_MAX_LENGTH 8
#define MAX_XFER_CHUNK              (16 * 1024 * 1024)
#define MIN_DOWNLOAD_BUFFER_SIZE    (64ULL * 1024 * 1024)
#define MAX_DOWNLOAD_BUFFER_SIZE    (1024ULL * 1024 * 1024)
#define FIRST_RX_SIZE               511          /* as ABL HandleUsbEvents() */
#define TX_WAIT_POLLS               200000       /* x 10us = 2s upper bound */

/* Reboot reason plumbing (ShutdownServices.h / EFIResetReason.h) */
#define STR_RESET_PARAM   L"RESET_PARAM"
#define NORMAL_MODE       0x0
#define FASTBOOT_MODE     0x2
typedef struct {
  CHAR16 DataBuffer[12];
  UINT8  Bdata;
} __attribute__ ((packed, aligned (2))) ResetDataType;

typedef enum {
  ExpectCmdState,
  ExpectDataState
} FASTBOOT_STATE;

typedef VOID (*FASTBOOT_CMD_FN) (CONST CHAR8 *Arg, VOID *Data, UINT32 Size);

typedef struct {
  CONST CHAR8      *Prefix;
  FASTBOOT_CMD_FN   Handler;
} FASTBOOT_CMD;

/* --- state -------------------------------------------------------------- */
STATIC EFI_USB_DEVICE_PROTOCOL   *mUsb;
STATIC USB_DEVICE_DESCRIPTOR_SET  mDescSet;
STATIC VOID                      *mRxBuffer;
STATIC VOID                      *mTxBuffer;
STATIC UINT8                     *mDlBuffer;
STATIC UINT64                     mDlBufferSize;

STATIC FASTBOOT_STATE  mState = ExpectCmdState;
STATIC UINT64          mNumDataBytes;       /* expected in this data phase   */
STATIC UINT64          mBytesReceivedSoFar;
STATIC UINT64          mLastDownloadSize;   /* size of last completed download */
STATIC BOOLEAN         mContinue;
STATIC BOOLEAN         mUsbStarted;
STATIC CHAR8           mSerial[MAX_RSP_SIZE];
STATIC CHAR8           mMaxDownloadStr[MAX_RSP_SIZE];

/* ------------------------------------------------------------------------ */
/* responses                                                                 */
/* ------------------------------------------------------------------------ */

STATIC VOID
FastbootAck (
  IN CONST CHAR8  *Code,
  IN CONST CHAR8  *Reason
  )
{
  UINTN  Len;

  if (Reason == NULL) {
    Reason = "";
  }
  AsciiSPrint (mTxBuffer, MAX_RSP_SIZE, "%a%a", Code, Reason);
  Len = AsciiStrLen (mTxBuffer);
  DEBUG ((EFI_D_INFO, "fastboot: -> %a\n", (CHAR8 *)mTxBuffer));
  mUsb->Send (ENDPOINT_OUT, Len, mTxBuffer);
}

STATIC VOID FastbootFail (IN CONST CHAR8 *Reason) { FastbootAck ("FAIL", Reason); }
STATIC VOID FastbootInfo (IN CONST CHAR8 *Info)   { FastbootAck ("INFO", Info);   }
STATIC VOID FastbootOkay (IN CONST CHAR8 *Info)   { FastbootAck ("OKAY", Info);   }

STATIC VOID
FastbootFailStatus (
  IN CONST CHAR8  *What,
  IN EFI_STATUS    Status
  )
{
  CHAR8  Resp[MAX_RSP_SIZE];

  AsciiSPrint (Resp, sizeof (Resp), "%a: %r", What, Status);
  FastbootFail (Resp);
}

/* Block until our pending IN (device->host) transfer on EP1 completes.
 * Same as FastbootCmds.c WaitForTransferComplete(), but bounded. */
STATIC VOID
WaitForTransferComplete (
  VOID
  )
{
  USB_DEVICE_EVENT       Msg;
  USB_DEVICE_EVENT_DATA  Payload;
  UINTN                  PayloadSize;
  UINTN                  Polls;

  for (Polls = 0; Polls < TX_WAIT_POLLS; Polls++) {
    Msg = UsbDeviceEventNoEvent;
    mUsb->HandleEvent (&Msg, &PayloadSize, &Payload);
    if (Msg == UsbDeviceEventTransferNotification &&
        USB_INDEX_TO_EP (Payload.TransferOutcome.EndpointIndex) == 1 &&
        USB_INDEX_TO_EPDIR (Payload.TransferOutcome.EndpointIndex) ==
            USB_ENDPOINT_DIRECTION_IN) {
      return;
    }
    if (Msg == UsbDeviceEventDeviceStateChange &&
        Payload.DeviceState == UsbDeviceStateDisconnected) {
      DEBUG ((EFI_D_WARN, "fastboot: host disconnected while waiting for TX\n"));
      return;
    }
    gBS->Stall (10);
  }
  DEBUG ((EFI_D_WARN, "fastboot: timed out waiting for TX completion\n"));
}

/* ------------------------------------------------------------------------ */
/* helpers                                                                   */
/* ------------------------------------------------------------------------ */

/* FastbootCmds.c GetXfrSize(): remaining bytes, capped to one chunk */
STATIC UINTN
GetXfrSize (
  VOID
  )
{
  UINT64  BytesLeft;

  if (mState == ExpectDataState) {
    BytesLeft = mNumDataBytes - mBytesReceivedSoFar;
    return (BytesLeft < MAX_XFER_CHUNK) ? (UINTN)BytesLeft : MAX_XFER_CHUNK;
  }
  return CMD_BUFFER_SIZE;
}

/* FastbootCmds.c GetMaxAllocatableMemory(): largest EfiConventionalMemory run */
STATIC UINT64
GetMaxAllocatableMemory (
  VOID
  )
{
  EFI_MEMORY_DESCRIPTOR  *MemMap;
  EFI_MEMORY_DESCRIPTOR  *Desc;
  UINTN                   MemMapSize = 0;
  UINTN                   MapKey, DescriptorSize, Index;
  UINT32                  DescriptorVersion;
  UINTN                   MaxFree = 0;
  EFI_STATUS              Status;

  Status = gBS->GetMemoryMap (&MemMapSize, NULL, &MapKey, &DescriptorSize,
                              &DescriptorVersion);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return 0;
  }
  MemMapSize += EFI_PAGE_SIZE;
  MemMap = AllocateZeroPool (MemMapSize);
  if (MemMap == NULL) {
    return 0;
  }
  Status = gBS->GetMemoryMap (&MemMapSize, MemMap, &MapKey, &DescriptorSize,
                              &DescriptorVersion);
  if (EFI_ERROR (Status)) {
    FreePool (MemMap);
    return 0;
  }
  Desc = MemMap;
  for (Index = 0; Index < MemMapSize / DescriptorSize; Index++) {
    if (Desc->Type == EfiConventionalMemory && Desc->NumberOfPages > MaxFree) {
      MaxFree = Desc->NumberOfPages;
    }
    Desc = (EFI_MEMORY_DESCRIPTOR *)((UINTN)Desc + DescriptorSize);
  }
  FreePool (MemMap);
  return EFI_PAGES_TO_SIZE (MaxFree);
}

/* Board.c BoardSerialNum(), UNKNOWN-device branch: ChipInfo serial as hex */
STATIC VOID
GetSerialNumber (
  OUT CHAR8  *Buf,
  IN  UINTN   Len
  )
{
  EFI_STATUS             Status;
  EFI_CHIPINFO_PROTOCOL *ChipInfo = NULL;
  UINT32                 SerialNo = 0;

  Status = gBS->LocateProtocol (&gEfiChipInfoProtocolGuid, NULL, (VOID **)&ChipInfo);
  if (!EFI_ERROR (Status) && ChipInfo != NULL) {
    Status = ChipInfo->GetSerialNumber (ChipInfo, &SerialNo);
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_WARN, "fastboot: no ChipInfo serial number (%r), using 'unknown'\n",
            Status));
    AsciiStrCpyS (Buf, Len, "unknown");
    return;
  }
  AsciiSPrint (Buf, Len, "%x", SerialNo);   /* %x prints lower case */
}

STATIC VOID
RebootDevice (
  IN UINT8  Reason
  )
{
  ResetDataType  ResetData;
  EFI_STATUS     Status;

  /* ShutdownServices.c RebootDevice() */
  ZeroMem (&ResetData, sizeof (ResetData));
  StrnCpyS (ResetData.DataBuffer, ARRAY_SIZE (ResetData.DataBuffer),
            STR_RESET_PARAM, ARRAY_SIZE (STR_RESET_PARAM) - 1);
  ResetData.Bdata = Reason;
  Status = (Reason == NORMAL_MODE) ? EFI_SUCCESS : EFI_INVALID_PARAMETER;
  gRT->ResetSystem (EfiResetCold, Status, sizeof (ResetDataType), &ResetData);
}

/* ------------------------------------------------------------------------ */
/* commands                                                                  */
/* ------------------------------------------------------------------------ */

STATIC VOID
CmdGetVarAll (
  VOID
  );

STATIC BOOLEAN
GetVarValue (
  IN  CONST CHAR8  *Name,
  OUT CHAR8        *Value,
  IN  UINTN         ValueMax
  )
{
  if (AsciiStrCmp (Name, "version") == 0) {
    AsciiStrCpyS (Value, ValueMax, "0.4");
  } else if (AsciiStrCmp (Name, "version-bootloader") == 0) {
    AsciiStrCpyS (Value, ValueMax, "mainlineboot-0.1");
  } else if (AsciiStrCmp (Name, "product") == 0) {
    AsciiStrCpyS (Value, ValueMax, PRODUCT_NAME);
  } else if (AsciiStrCmp (Name, "serialno") == 0) {
    AsciiStrCpyS (Value, ValueMax, mSerial);
  } else if (AsciiStrCmp (Name, "max-download-size") == 0) {
    AsciiStrCpyS (Value, ValueMax, mMaxDownloadStr);
  } else if (AsciiStrCmp (Name, "window") == 0) {
    MAINLINE_BOOT_INFO  W;
    if (!EFI_ERROR (MainlineBootGetInfo (&W))) {
      AsciiSPrint (Value, ValueMax, "0x%lx+0x%lx src=%a",
                  W.KernelBaseAddr, W.KernelSizeReserved,
                  W.KernelParamsFromCfg ? "bootcfg"
                  : W.KernelParamsFromUefiVars ? "uefi-vars" : "fallback");
    } else {
      AsciiStrCpyS (Value, ValueMax, "not-initialized");
    }
  } else if (AsciiStrCmp (Name, "is-userspace") == 0) {
    AsciiStrCpyS (Value, ValueMax, "no");
  } else if (AsciiStrCmp (Name, "kernel") == 0) {
    AsciiStrCpyS (Value, ValueMax, "uefi");
  } else if (AsciiStrCmp (Name, "secure") == 0) {
    AsciiStrCpyS (Value, ValueMax, "no");
  } else if (AsciiStrCmp (Name, "unlocked") == 0) {
    AsciiStrCpyS (Value, ValueMax, "yes");
  } else if (AsciiStrCmp (Name, "slot-count") == 0) {
    AsciiStrCpyS (Value, ValueMax, "0");
  } else if (AsciiStrCmp (Name, "current-slot") == 0) {
    AsciiStrCpyS (Value, ValueMax, "");
  } else if (AsciiStrnCmp (Name, "has-slot:", 9) == 0) {
    AsciiStrCpyS (Value, ValueMax, "no");
  } else if (AsciiStrnCmp (Name, "partition-type:", 15) == 0) {
    AsciiStrCpyS (Value, ValueMax, "raw");
  } else if (AsciiStrnCmp (Name, "partition-size:", 15) == 0) {
    AsciiStrCpyS (Value, ValueMax, "0x40000000");
  } else {
    return FALSE;
  }
  return TRUE;
}

STATIC VOID
CmdGetVar (
  IN CONST CHAR8  *Arg,
  IN VOID         *Data,
  IN UINT32        Size
  )
{
  CHAR8  Value[MAX_RSP_SIZE];

  if (AsciiStrCmp (Arg, "all") == 0) {
    CmdGetVarAll ();
    return;
  }
  if (GetVarValue (Arg, Value, sizeof (Value))) {
    FastbootOkay (Value);
  } else {
    FastbootFail ("GetVar Variable Not found");
  }
}

STATIC VOID
CmdGetVarAll (
  VOID
  )
{
  STATIC CONST CHAR8 *Names[] = {
    "version", "version-bootloader", "product", "serialno", "max-download-size",
    "window", "is-userspace", "kernel", "secure", "unlocked", "slot-count",
    "has-slot:kernel", "partition-type:kernel", "partition-size:kernel",
  };
  CHAR8  Line[MAX_RSP_SIZE];
  CHAR8  Value[MAX_RSP_SIZE];
  UINTN  i;

  for (i = 0; i < ARRAY_SIZE (Names); i++) {
    GetVarValue (Names[i], Value, sizeof (Value));
    AsciiSPrint (Line, sizeof (Line), "%a:%a", Names[i], Value);
    FastbootInfo (Line);
    WaitForTransferComplete ();       /* one outstanding TX at a time */
  }
  FastbootOkay ("");
}

/* FastbootCmds.c CmdDownload() */
STATIC VOID
CmdDownload (
  IN CONST CHAR8  *Arg,
  IN VOID         *Data,
  IN UINT32        Size
  )
{
  CHAR8  Response[13] = "DATA";

  if (AsciiStrLen (Arg) > ASCII_HEX_STRING_MAX_LENGTH || AsciiStrLen (Arg) == 0) {
    FastbootFail ("Invalid argument size");
    return;
  }
  mNumDataBytes = AsciiStrHexToUint64 (Arg);
  if (mNumDataBytes == 0) {
    FastbootFail ("Failed to get the number of bytes to download");
    return;
  }
  if (mNumDataBytes > mDlBufferSize) {
    DEBUG ((EFI_D_ERROR, "fastboot: download %lu > buffer %lu\n", mNumDataBytes,
            mDlBufferSize));
    FastbootFail ("Requested download size is more than max allowed");
    return;
  }

  DEBUG ((EFI_D_INFO, "fastboot: downloading %lu bytes\n", mNumDataBytes));
  AsciiStrnCpyS (Response + 4, sizeof (Response) - 4, Arg, AsciiStrLen (Arg));

  mState = ExpectDataState;
  mBytesReceivedSoFar = 0;
  /* Send "DATA" + 8 hex digits. The TX-complete of this response queues
   * the first data receive into mDlBuffer (see ProcessBulkXfrCompleteTx). */
  AsciiStrCpyS (mTxBuffer, MAX_RSP_SIZE, Response);
  DEBUG ((EFI_D_INFO, "fastboot: -> %a\n", Response));
  mUsb->Send (ENDPOINT_OUT, AsciiStrLen (Response), mTxBuffer);
}

/* flash:<name> -> hand the download buffer to MainlineBootLib */
STATIC VOID
CmdFlash (
  IN CONST CHAR8  *Arg,
  IN VOID         *Data,
  IN UINT32        Size
  )
{
  EFI_STATUS  Status;
  UINTN       Len = (UINTN)mLastDownloadSize;

  if (Len == 0 || Data == NULL) {
    FastbootFail ("no data downloaded");
    return;
  }

  DEBUG ((EFI_D_INFO, "fastboot: flash:%a (%lu bytes, RAM only)\n", Arg,
          (UINT64)Len));

  if (AsciiStrCmp (Arg, "bootcfg") == 0) {
    Status = MainlineBootLoadBootcfg (Data, Len);
  } else if (AsciiStrCmp (Arg, "kernel") == 0) {
    Status = MainlineBootLoadKernel (Data, Len);
  } else if (AsciiStrCmp (Arg, "dtb") == 0) {
    Status = MainlineBootLoadDtb (Data, Len);
  } else if (AsciiStrCmp (Arg, "initrd") == 0 ||
             AsciiStrCmp (Arg, "ramdisk") == 0 ||
             AsciiStrCmp (Arg, "initramfs") == 0) {
    Status = MainlineBootLoadInitrd (Data, Len);
  } else {
    FastbootFail ("unsupported partition (use bootcfg/kernel/dtb/initrd)");
    return;
  }

  if (EFI_ERROR (Status)) {
    FastbootFailStatus (Arg, Status);
    return;
  }
  FastbootOkay ("");
}

STATIC VOID
CmdErase (
  IN CONST CHAR8  *Arg,
  IN VOID         *Data,
  IN UINT32        Size
  )
{
  if (AsciiStrCmp (Arg, "initrd") == 0 || AsciiStrCmp (Arg, "ramdisk") == 0 ||
      AsciiStrCmp (Arg, "initramfs") == 0) {
    MainlineBootLoadInitrd (NULL, 0);     /* drop a previously loaded initrd */
    FastbootOkay ("");
    return;
  }
  FastbootFail ("erase not supported (RAM-only loader)");
}

STATIC BOOLEAN
ReadyToBoot (
  VOID
  )
{
  MAINLINE_BOOT_INFO  Info;

  if (EFI_ERROR (MainlineBootGetInfo (&Info))) {
    return FALSE;
  }
  if (!Info.Initialized) {
    FastbootFail ("MainlineBootInit failed (no RAM partition protocol?)");
    return FALSE;
  }
  if (!Info.KernelLoaded || !Info.DtbLoaded) {
    FastbootFail (Info.KernelLoaded ? "dtb not loaded (fastboot flash dtb ...)"
                                    : "kernel not loaded (fastboot flash kernel ...)");
    return FALSE;
  }
  return TRUE;
}

/* FastbootCmds.c CmdContinue(): OKAY, then leave the loop and boot */
STATIC VOID
CmdContinue (
  IN CONST CHAR8  *Arg,
  IN VOID         *Data,
  IN UINT32        Size
  )
{
  EFI_STATUS  Status;
  UINT64      K, E, R, D;

  if (!ReadyToBoot ()) {
    return;
  }
  Status = MainlineBootGetLayout (&K, &E, &R, &D);
  if (EFI_ERROR (Status)) {
    FastbootFailStatus ("layout", Status);
    return;
  }
  FastbootOkay ("");
  WaitForTransferComplete ();
  mContinue = TRUE;
}

/* "boot": treat the downloaded buffer as the kernel Image, then continue */
STATIC VOID
CmdBoot (
  IN CONST CHAR8  *Arg,
  IN VOID         *Data,
  IN UINT32        Size
  )
{
  EFI_STATUS  Status;

  if (mLastDownloadSize == 0 || Data == NULL) {
    FastbootFail ("no data downloaded");
    return;
  }
  Status = MainlineBootLoadKernel (Data, (UINTN)mLastDownloadSize);
  if (EFI_ERROR (Status)) {
    FastbootFailStatus ("downloaded data is not an arm64 Image", Status);
    return;
  }
  CmdContinue (Arg, Data, Size);
}

STATIC VOID
CmdReboot (
  IN CONST CHAR8  *Arg,
  IN VOID         *Data,
  IN UINT32        Size
  )
{
  DEBUG ((EFI_D_INFO, "fastboot: rebooting the device\n"));
  FastbootOkay ("");
  WaitForTransferComplete ();
  RebootDevice (NORMAL_MODE);
  FastbootFail ("Failed to reboot");
}

STATIC VOID
CmdRebootBootloader (
  IN CONST CHAR8  *Arg,
  IN VOID         *Data,
  IN UINT32        Size
  )
{
  DEBUG ((EFI_D_INFO, "fastboot: rebooting the device into bootloader mode\n"));
  FastbootOkay ("");
  WaitForTransferComplete ();
  RebootDevice (FASTBOOT_MODE);
  FastbootFail ("Failed to reboot");
}

STATIC VOID
CmdOemHelp (
  IN CONST CHAR8  *Arg,
  IN VOID         *Data,
  IN UINT32        Size
  )
{
  STATIC CONST CHAR8 *Lines[] = {
    "MainlineBoot flashless boot:",
    " fastboot flash bootcfg <bootcfg.ini>  (cmdline=...)",
    " fastboot flash kernel  <Image>        (arm64, uncompressed)",
    " fastboot flash dtb     <board.dtb>",
    " fastboot flash initrd  <initramfs.cpio> (optional)",
    " fastboot continue                      (boot)",
    " fastboot oem gop                       (GOP info)",
    " fastboot oem fbfill:RRGGBB             (fill screen)",
    " nothing is written to storage",
  };
  UINTN  i;

  for (i = 0; i < ARRAY_SIZE (Lines); i++) {
    FastbootInfo (Lines[i]);
    WaitForTransferComplete ();
  }
  FastbootOkay ("");
}

STATIC VOID
CmdOemStatus (
  IN CONST CHAR8  *Arg,
  IN VOID         *Data,
  IN UINT32        Size
  )
{
  MAINLINE_BOOT_INFO  Info;
  CHAR8               Line[MAX_RSP_SIZE];
  UINT64              K = 0, E = 0, R = 0, D = 0;
  EFI_STATUS          Status;

  MainlineBootGetInfo (&Info);
  AsciiSPrint (Line, sizeof (Line), "init %a base 0x%lx size 0x%lx",
               Info.Initialized ? "ok" : "FAILED", Info.KernelBaseAddr,
               Info.KernelSizeReserved);
  FastbootInfo (Line); WaitForTransferComplete ();
  AsciiSPrint (Line, sizeof (Line), "kernel %a %lu B img 0x%lx",
               Info.KernelLoaded ? "yes" : "no", (UINT64)Info.KernelFileSize,
               Info.KernelImageSize);
  FastbootInfo (Line); WaitForTransferComplete ();
  AsciiSPrint (Line, sizeof (Line),
               "window 0x%lx+0x%lx src=%a",
               Info.KernelBaseAddr, Info.KernelSizeReserved,
               Info.KernelParamsFromCfg ? "bootcfg"
               : Info.KernelParamsFromUefiVars ? "uefi-vars" : "fallback");
  FastbootInfo (Line); WaitForTransferComplete ();
  AsciiSPrint (Line, sizeof (Line), "dtb %a %lu B  initrd %a %lu B",
               Info.DtbLoaded ? "yes" : "no", (UINT64)Info.DtbSize,
               Info.InitrdLoaded ? "yes" : "no", (UINT64)Info.InitrdSize);
  FastbootInfo (Line); WaitForTransferComplete ();
  AsciiSPrint (Line, sizeof (Line), "cmdline %a", Info.CmdLineLoaded ? "yes" : "no");
  FastbootInfo (Line); WaitForTransferComplete ();
  Status = MainlineBootGetLayout (&K, &E, &R, &D);
  if (!EFI_ERROR (Status)) {
    AsciiSPrint (Line, sizeof (Line), "K 0x%lx D 0x%lx R 0x%lx E 0x%lx", K, D, R, E);
    FastbootInfo (Line); WaitForTransferComplete ();
  }
  FastbootOkay ("");
}

/* --- diagnostics --------------------------------------------------------- */

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL *
MlbDiagLocateGop (
  VOID
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop = NULL;

  if (gST != NULL && gST->ConsoleOutHandle != NULL) {
    gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid,
                         (VOID **)&Gop);
  }
  if (Gop == NULL) {
    gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  }
  return Gop;
}

STATIC VOID
CmdOemGop (
  IN CONST CHAR8  *Arg,
  IN VOID         *Data,
  IN UINT32        Size
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL          *Gop;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *Info;
  CHAR8                                  Line[MAX_RSP_SIZE];

  Gop = MlbDiagLocateGop ();
  if (Gop == NULL || Gop->Mode == NULL || Gop->Mode->Info == NULL) {
    FastbootFail ("no GOP (display off?)");
    return;
  }
  Info = Gop->Mode->Info;

  AsciiSPrint (Line, sizeof (Line), "mode %u/%u res %ux%u ppsl %u",
               (UINT32)Gop->Mode->Mode, (UINT32)Gop->Mode->MaxMode,
               Info->HorizontalResolution, Info->VerticalResolution,
               Info->PixelsPerScanLine);
  FastbootInfo (Line); WaitForTransferComplete ();

  AsciiSPrint (Line, sizeof (Line), "fmt %d r=%lx g=%lx b=%lx",
               (INT32)Info->PixelFormat, Info->PixelInformation.RedMask,
               Info->PixelInformation.GreenMask, Info->PixelInformation.BlueMask);
  FastbootInfo (Line); WaitForTransferComplete ();

  AsciiSPrint (Line, sizeof (Line), "fb 0x%lx size 0x%lx",
               Gop->Mode->FrameBufferBase, Gop->Mode->FrameBufferSize);
  FastbootInfo (Line); WaitForTransferComplete ();

  FastbootOkay ("");
}

STATIC VOID
CmdOemFbFill (
  IN CONST CHAR8  *Arg,
  IN VOID         *Data,
  IN UINT32        Size
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL    *Gop;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL    Fill;
  EFI_STATUS                       Status;
  UINT32                           C = 0xFF0000;   /* default: red */
  UINTN                            i;

  if (Arg != NULL && AsciiStrLen (Arg) >= 6) {
    C = 0;
    for (i = 0; i < 6; i++) {
      CHAR8  Ch = Arg[i];
      UINT32 Nib;

      if (Ch >= '0' && Ch <= '9') {
        Nib = (UINT32)(Ch - '0');
      } else if (Ch >= 'a' && Ch <= 'f') {
        Nib = (UINT32)(Ch - 'a' + 10);
      } else if (Ch >= 'A' && Ch <= 'F') {
        Nib = (UINT32)(Ch - 'A' + 10);
      } else {
        FastbootFail ("bad RRGGBB hex");
        return;
      }
      C = (C << 4) | Nib;
    }
  }
  Fill.Reserved = 0;
  Fill.Red      = (UINT8)(C >> 16);
  Fill.Green    = (UINT8)(C >> 8);
  Fill.Blue     = (UINT8)C;

  Gop = MlbDiagLocateGop ();
  if (Gop == NULL || Gop->Mode == NULL || Gop->Mode->Info == NULL) {
    FastbootFail ("no GOP (display off?)");
    return;
  }

  Status = Gop->Blt (Gop, &Fill, EfiBltVideoFill, 0, 0, 0, 0,
                     Gop->Mode->Info->HorizontalResolution,
                     Gop->Mode->Info->VerticalResolution, 0);
  if (EFI_ERROR (Status)) {
    FastbootFailStatus ("Blt failed", Status);
    return;
  }
  FastbootOkay ("screen filled, watch it");
}

/* Order matters: longer prefixes that share a stem come first. */
STATIC CONST FASTBOOT_CMD mCmdList[] = {
  { "getvar:",           CmdGetVar },
  { "download:",         CmdDownload },
  { "flash:",            CmdFlash },
  { "erase:",            CmdErase },
  { "continue",          CmdContinue },
  { "boot",              CmdBoot },
  { "reboot-bootloader", CmdRebootBootloader },
  { "reboot",            CmdReboot },
  { "oem help",          CmdOemHelp },
  { "oem status",        CmdOemStatus },
  { "oem gop",           CmdOemGop },
  { "oem fbfill:",       CmdOemFbFill },
};

/* FastbootCmds.c AcceptCmd() */
STATIC VOID
AcceptCmd (
  IN UINT64  Size,
  IN CHAR8  *Data
  )
{
  UINTN  i, PrefixLen;

  if (Data == NULL) {
    FastbootFail ("Invalid input command");
    return;
  }
  if (Size > MAX_FASTBOOT_COMMAND_SIZE) {
    Size = MAX_FASTBOOT_COMMAND_SIZE;
  }
  Data[Size] = '\0';                 /* rx buffer is far larger than 65 bytes */

  DEBUG ((EFI_D_INFO, "fastboot: <- %a\n", Data));

  for (i = 0; i < ARRAY_SIZE (mCmdList); i++) {
    PrefixLen = AsciiStrLen (mCmdList[i].Prefix);
    if (AsciiStrnCmp (Data, mCmdList[i].Prefix, PrefixLen) != 0) {
      continue;
    }
    mCmdList[i].Handler (Data + PrefixLen, mDlBuffer, (UINT32)mLastDownloadSize);
    return;
  }
  FastbootFail ("unknown command");
}

/* FastbootCmds.c AcceptData() */
STATIC VOID
AcceptData (
  IN UINT64  Size,
  IN VOID   *Data
  )
{
  UINT64  RemainingBytes = mNumDataBytes - mBytesReceivedSoFar;

  if (Size > RemainingBytes) {
    Size = RemainingBytes;           /* ignore surplus */
  }
  mBytesReceivedSoFar += Size;

  if (mBytesReceivedSoFar == mNumDataBytes) {
    DEBUG ((EFI_D_INFO, "fastboot: download finished (%lu bytes)\n", mNumDataBytes));
    mLastDownloadSize = mNumDataBytes;
    mState = ExpectCmdState;
    FastbootOkay ("");
  } else {
    mUsb->Send (ENDPOINT_IN, GetXfrSize (),
                (VOID *)((UINT8 *)Data + mBytesReceivedSoFar));
  }
}

STATIC VOID
DataReady (
  IN UINT64  Size,
  IN VOID   *Data
  )
{
  if (mState == ExpectCmdState) {
    AcceptCmd (Size, (CHAR8 *)Data);
  } else {
    AcceptData (Size, Data);
  }
}

/* ------------------------------------------------------------------------ */
/* USB event handling (FastbootMain.c)                                       */
/* ------------------------------------------------------------------------ */

STATIC EFI_STATUS
ProcessBulkXfrCompleteRx (
  IN USB_DEVICE_TRANSFER_OUTCOME  *Uto
  )
{
  switch (Uto->Status) {
  case UsbDeviceTransferStatusCompleteOK:
    if (mState == ExpectDataState) {
      DataReady (Uto->BytesCompleted, mDlBuffer);
    } else {
      DataReady (Uto->BytesCompleted, mRxBuffer);
    }
    return EFI_SUCCESS;
  case UsbDeviceTransferStatusCancelled:
    DEBUG ((EFI_D_ERROR, "fastboot: bulk in XFR aborted\n"));
    return EFI_ABORTED;
  default:
    return EFI_DEVICE_ERROR;
  }
}

STATIC EFI_STATUS
ProcessBulkXfrCompleteTx (
  IN USB_DEVICE_TRANSFER_OUTCOME  *Uto
  )
{
  switch (Uto->Status) {
  case UsbDeviceTransferStatusCompleteOK:
    /* Response went out: queue the next receive (data chunk or command) */
    if (mState == ExpectDataState) {
      return mUsb->Send (ENDPOINT_IN, GetXfrSize (), mDlBuffer + mBytesReceivedSoFar);
    }
    return mUsb->Send (ENDPOINT_IN, GetXfrSize (), mRxBuffer);
  case UsbDeviceTransferStatusCancelled:
    DEBUG ((EFI_D_ERROR, "fastboot: bulk out xfr aborted\n"));
    return EFI_ABORTED;
  default:
    DEBUG ((EFI_D_ERROR, "fastboot: unhandled transfer status %d\n", Uto->Status));
    return EFI_DEVICE_ERROR;
  }
}

EFI_STATUS
FastbootHandleEvents (
  VOID
  )
{
  EFI_STATUS             Status = EFI_SUCCESS;
  USB_DEVICE_EVENT       Msg = UsbDeviceEventNoEvent;
  USB_DEVICE_EVENT_DATA  Payload;
  UINTN                  PayloadSize;

  if (!mUsbStarted) {
    return EFI_NOT_STARTED;
  }

  mUsb->HandleEvent (&Msg, &PayloadSize, &Payload);

  if (Msg == UsbDeviceEventDeviceStateChange) {
    if (Payload.DeviceState == UsbDeviceStateConnected) {
      DEBUG ((EFI_D_INFO, "fastboot: device connected\n"));
      mState = ExpectCmdState;
      Status = mUsb->Send (ENDPOINT_IN, FIRST_RX_SIZE, mRxBuffer);
    } else if (Payload.DeviceState == UsbDeviceStateDisconnected) {
      DEBUG ((EFI_D_INFO, "fastboot: device disconnected\n"));
    }
  } else if (Msg == UsbDeviceEventTransferNotification) {
    if (USB_INDEX_TO_EP (Payload.TransferOutcome.EndpointIndex) == 1) {
      if (USB_INDEX_TO_EPDIR (Payload.TransferOutcome.EndpointIndex) ==
          USB_ENDPOINT_DIRECTION_OUT) {
        Status = ProcessBulkXfrCompleteRx (&Payload.TransferOutcome);
      } else {
        Status = ProcessBulkXfrCompleteTx (&Payload.TransferOutcome);
      }
      if (EFI_ERROR (Status) && Status != EFI_ABORTED) {
        DEBUG ((EFI_D_ERROR, "fastboot: transfer error %r, check USB connection\n",
                Status));
      }
    }
  }
  return Status;
}

BOOLEAN
FastbootContinueRequested (
  VOID
  )
{
  return mContinue;
}

/* ------------------------------------------------------------------------ */
/* start / stop                                                              */
/* ------------------------------------------------------------------------ */

STATIC VOID
DummyNotify (
  IN EFI_EVENT  Event,
  IN VOID      *Context
  )
{
}

STATIC EFI_STATUS
AllocateDownloadBuffer (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT64      Size;

  Size = GetMaxAllocatableMemory ();
  DEBUG ((EFI_D_INFO, "fastboot: largest free block %lu MB\n", Size >> 20));
  if (Size == 0) {
    return EFI_OUT_OF_RESOURCES;
  }
  Size = (Size * 3) / 4;
  Size &= ~(UINT64)(EFI_PAGE_SIZE - 1);
  if (Size > MAX_DOWNLOAD_BUFFER_SIZE) {
    Size = MAX_DOWNLOAD_BUFFER_SIZE;
  }

  for (;;) {
    if (Size < MIN_DOWNLOAD_BUFFER_SIZE) {
      DEBUG ((EFI_D_ERROR, "fastboot: cannot allocate even %lu MB download buffer\n",
              MIN_DOWNLOAD_BUFFER_SIZE >> 20));
      return EFI_OUT_OF_RESOURCES;
    }
    Status = mUsb->AllocateTransferBuffer ((UINTN)Size, (VOID **)&mDlBuffer);
    if (!EFI_ERROR (Status) && mDlBuffer != NULL) {
      break;
    }
    DEBUG ((EFI_D_WARN, "fastboot: %lu MB transfer buffer failed (%r), halving\n",
            Size >> 20, Status));
    Size /= 2;
    Size &= ~(UINT64)(EFI_PAGE_SIZE - 1);
  }

  mDlBufferSize = Size;
  AsciiSPrint (mMaxDownloadStr, sizeof (mMaxDownloadStr), "0x%x", (UINT32)Size);
  DEBUG ((EFI_D_INFO, "fastboot: download buffer %lu MB at 0x%p\n", Size >> 20,
          mDlBuffer));
  return EFI_SUCCESS;
}

EFI_STATUS
FastbootStart (
  VOID
  )
{
  EFI_STATUS              Status;
  EFI_EVENT               UsbConfigEvt;
  USB_DEVICE_DESCRIPTOR  *DevDesc;
  USB_DEVICE_DESCRIPTOR  *SSDevDesc;
  VOID                   *Descriptors;
  VOID                   *SSDescriptors;
  EFI_GUID                InitUsbControllerGuid = {
      0x1c0cffce, 0xfc8d, 0x4e44, {0x8c, 0x78, 0x9c, 0x9e, 0x5b, 0x53, 0x0d, 0x36}};

  mContinue = FALSE;
  mState = ExpectCmdState;
  mNumDataBytes = 0;
  mBytesReceivedSoFar = 0;
  mLastDownloadSize = 0;

  /* FastbootCmdsInit(): disable watchdog */
  Status = gBS->SetWatchdogTimer (0, 0x10000, 0, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_WARN, "fastboot: couldn't disable watchdog timer: %r\n", Status));
  }

  /* FastbootUsbDeviceStart(): kick the USB controller init event group */
  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, DummyNotify, NULL,
                               &InitUsbControllerGuid, &UsbConfigEvt);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "fastboot: Usb controller init event not signaled: %r\n",
            Status));
    return Status;
  }
  gBS->SignalEvent (UsbConfigEvt);
  gBS->CloseEvent (UsbConfigEvt);

  Status = gBS->LocateProtocol (&gEfiUsbDeviceProtocolGuid, NULL, (VOID **)&mUsb);
  if (EFI_ERROR (Status) || mUsb == NULL) {
    DEBUG ((EFI_D_ERROR, "fastboot: couldn't find USB device protocol: %r\n", Status));
    return EFI_NOT_FOUND;
  }

  GetSerialNumber (mSerial, sizeof (mSerial));
  DEBUG ((EFI_D_INFO, "fastboot: serialno %a product %a\n", mSerial, PRODUCT_NAME));

  Status = BuildDefaultDescriptors (mSerial, &DevDesc, &Descriptors, &SSDevDesc,
                                    &SSDescriptors);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&mDescSet, sizeof (mDescSet));
  mDescSet.DeviceDescriptor = DevDesc;
  mDescSet.Descriptors = Descriptors;
  mDescSet.SSDeviceDescriptor = SSDevDesc;
  mDescSet.SSDescriptors = SSDescriptors;
  mDescSet.DeviceQualifierDescriptor = &DeviceQualifier;
  mDescSet.BinaryDeviceOjectStore = (VOID *)BinaryObjectStorePtr;
  mDescSet.StringDescriptorCount = USB_STRING_DESC_COUNT;
  mDescSet.StringDescritors = StrDescriptors;

  Status = mUsb->StartEx (&mDescSet);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "fastboot: error starting the usb device: %r\n", Status));
    Status = EFI_NOT_STARTED;
    goto Fail;                       /* frees the descriptor arrays */
  }
  mUsbStarted = TRUE;

  Status = mUsb->AllocateTransferBuffer (CMD_BUFFER_SIZE, &mRxBuffer);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "fastboot: cannot allocate RX buffer\n"));
    goto Fail;
  }
  Status = mUsb->AllocateTransferBuffer (CMD_BUFFER_SIZE, &mTxBuffer);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "fastboot: cannot allocate TX buffer\n"));
    goto Fail;
  }
  Status = AllocateDownloadBuffer ();
  if (EFI_ERROR (Status)) {
    goto Fail;
  }

  DEBUG ((EFI_D_INFO, "fastboot: processing commands (max-download-size %a)\n",
          mMaxDownloadStr));
  return EFI_SUCCESS;

Fail:
  FastbootStop ();
  return Status;
}

EFI_STATUS
FastbootStop (
  VOID
  )
{
  EFI_STATUS  Status = EFI_SUCCESS;

  if (mUsb == NULL) {
    return EFI_NOT_STARTED;
  }
  if (mTxBuffer != NULL) {
    mUsb->FreeTransferBuffer (mTxBuffer);
    mTxBuffer = NULL;
  }
  if (mRxBuffer != NULL) {
    mUsb->FreeTransferBuffer (mRxBuffer);
    mRxBuffer = NULL;
  }
  if (mDlBuffer != NULL) {
    mUsb->FreeTransferBuffer (mDlBuffer);
    mDlBuffer = NULL;
    mDlBufferSize = 0;
  }
  if (mDescSet.Descriptors != NULL) {
    FreePool (mDescSet.Descriptors);
    mDescSet.Descriptors = NULL;
  }
  if (mDescSet.SSDescriptors != NULL) {
    FreePool (mDescSet.SSDescriptors);
    mDescSet.SSDescriptors = NULL;
  }
  if (mUsbStarted) {
    Status = mUsb->Stop ();
    mUsbStarted = FALSE;
    DEBUG ((EFI_D_INFO, "fastboot: USB stopped (%r)\n", Status));
  }
  return Status;
}
