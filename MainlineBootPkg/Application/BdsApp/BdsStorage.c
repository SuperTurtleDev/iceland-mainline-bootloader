/*
 * FAT volume support adapted from Canoe SuperFbFat for application-mode BDS.
 *
 * FAT file systems come from the platform firmware's own FAT driver. The
 * DiskIoDxe and English Unicode Collation support are linked into this
 * application as static libraries: their entry points
 * are invoked here by hand rather than by the DXE dispatcher, then a
 * connection pass binds them to the Block I/O handles the platform published.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "BdsMenu.h"
#include "BdsApp.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Guid/FileInfo.h>
#include <Guid/FileSystemVolumeLabelInfo.h>
#include <IndustryStandard/PeImage.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>

/*
 * Entry points of the statically linked drivers. Declared locally because the
 * headers that carry them are module-private to their own packages.
 */
EFI_STATUS
EFIAPI
InitializeUnicodeCollationEng (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  );

EFI_STATUS
EFIAPI
InitializeDiskIo (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  );

// Driver bindings need their own handle; the BDS application stays resident.
STATIC EFI_GUID  mSfbDriverTagGuid = {
  0x7b41c0de, 0x2f95, 0x4a18,
  { 0x9c,     0x6d,   0x3e,  0x08,0xb7, 0x52, 0xd1, 0x64 }
};

STATIC BOOLEAN  mSfbFatStackStarted = FALSE;

STATIC
EFI_STATUS
SfbCreateDriverHandle (
  OUT EFI_HANDLE  *Handle
  )
{
  *Handle = NULL;
  return gBS->InstallProtocolInterface (
                Handle,
                &mSfbDriverTagGuid,
                EFI_NATIVE_INTERFACE,
                NULL
                );
}

/*
 * Recursively connect every controller in the system.
 *
 * The fastboot-only boot path skips the BDS "connect all" pass, so on this
 * platform whole device stacks are left dispatched-but-unconnected. Most of
 * them do not matter here, but the USB host storage chain does: this platform's
 * firmware carries the Qualcomm USB host bring-up (UsbConfigDxe), the XHCI
 * PCI-emulation shim, XhciDxe, UsbBusDxe and UsbMassStorageDxe, but nothing in
 * the fastboot path ever connects them, so an attached USB drive never appears.
 */
VOID
SfbConnectAll (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles = NULL;
  UINTN       Count    = 0;
  UINTN       Index;
  UINTN       Connected = 0;

  Status = gBS->LocateHandleBuffer (AllHandles, NULL, NULL, &Count, &Handles);
  if (EFI_ERROR (Status) || (Handles == NULL)) {
    DEBUG ((DEBUG_ERROR, "SFB: no handles to connect: %r\n", Status));
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    Status = gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
    if (!EFI_ERROR (Status)) {
      Connected++;
    }
  }

  DEBUG ((
    DEBUG_INFO,
    "SFB: connected %u of %u handles\n",
    (UINT32)Connected,
    (UINT32)Count
    ));

  FreePool (Handles);
}

EFI_STATUS
SfbStartFatStack (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  DiskIoHandle;

  if (mSfbFatStackStarted) {
    SfbConnectAll ();
    return EFI_SUCCESS;
  }

  // Do not retry partially installed libraries on this application handle.
  mSfbFatStackStarted = TRUE;
  Status              = InitializeUnicodeCollationEng (gBdsImageHandle, gST);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BDS: Unicode Collation init failed: %r\n", Status));
    return Status;
  }

  Status = SfbCreateDriverHandle (&DiskIoHandle);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = InitializeDiskIo (DiskIoHandle, gST);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "BDS: Disk I/O driver init failed: %r\n", Status));
    return Status;
  }

  SfbConnectAll ();
  return EFI_SUCCESS;
}

STATIC
UINT16
SfbLe16 (
  IN CONST UINT8  *Sector,
  IN UINTN        Offset
  )
{
  return (UINT16)(Sector[Offset] | ((UINT16)Sector[Offset + 1] << 8));
}

STATIC
UINT32
SfbLe32 (
  IN CONST UINT8  *Sector,
  IN UINTN        Offset
  )
{
  return (UINT32)Sector[Offset] |
         ((UINT32)Sector[Offset + 1] << 8) |
         ((UINT32)Sector[Offset + 2] << 16) |
         ((UINT32)Sector[Offset + 3] << 24);
}

EFI_STATUS
SfbOpenVolumeRoot (
  IN EFI_HANDLE          Volume,
  OUT EFI_FILE_PROTOCOL  **Root
  )
{
  EFI_STATUS                       Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs = NULL;

  *Root = NULL;

  Status = gBS->HandleProtocol (
                  Volume,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&Fs
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return Fs->OpenVolume (Fs, Root);
}

EFI_STATUS
SfbLocateVolumes (
  OUT EFI_HANDLE  **Handles,
  OUT UINTN       *Count
  )
{
  if ((Handles == NULL) || (Count == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Handles = NULL;
  *Count   = 0;
  return gBS->LocateHandleBuffer (
                ByProtocol,
                &gEfiSimpleFileSystemProtocolGuid,
                NULL,
                Count,
                Handles
                );
}

BOOLEAN
SfbFileExists (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CONST CHAR16       *Path
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File = NULL;
  EFI_FILE_INFO      *Info;
  UINTN              InfoSize;
  BOOLEAN            IsFile = FALSE;

  Status = Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status) || (File == NULL)) {
    return FALSE;
  }

  InfoSize = 0;
  Status   = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    Info = AllocateZeroPool (InfoSize);
    if (Info != NULL) {
      Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
      if (!EFI_ERROR (Status)) {
        IsFile = (BOOLEAN)((Info->Attribute & EFI_FILE_DIRECTORY) == 0);
      }

      FreePool (Info);
    }
  }

  File->Close (File);

  return IsFile;
}

EFI_STATUS
SfbReadFileBytes (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CONST CHAR16       *Path,
  OUT VOID              *Buffer,
  IN UINTN              MaxBytes,
  OUT UINTN             *BytesRead
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File    = NULL;
  UINTN              ReadSize = MaxBytes;

  *BytesRead = 0;

  Status = Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status) || (File == NULL)) {
    return EFI_ERROR (Status) ? Status : EFI_NOT_FOUND;
  }

  Status = File->Read (File, &ReadSize, Buffer);
  File->Close (File);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  *BytesRead = ReadSize;

  return EFI_SUCCESS;
}

/*
 * Decide application vs driver from the PE image's subsystem field. The layout
 * checked here is fixed for both PE32 and PE32+: a DOS "MZ" header carries the
 * offset of the PE signature at 0x3C, the COFF header follows the 4-byte
 * signature, and the optional header's 16-bit Subsystem sits 68 bytes into it.
 * Reading a header-sized prefix is enough; anything that does not parse as a PE
 * image is reported as "not a driver" so the caller falls back to app handling.
 */
BOOLEAN
SfbIsEfiDriverFile (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CONST CHAR16       *Path
  )
{
  /*
   * Must be large enough to contain the PE optional header's Subsystem field.
   * EDK2/GCC/CLANG images place the PE signature well past the classic 0x40-ish
   * offset (typically e_lfanew ~= 0xE58 for AARCH64 output), so a 512-byte
   * prefix stops short of Subsystem (which lands near file offset 0xEB4) and the
   * bounds check below would wrongly report every such image as "not a driver".
   * 8 KiB comfortably covers e_lfanew for all images this menu launches.
   */
  UINT8   Header[8192];
  UINTN   Read = 0;
  UINT32  PeOffset;
  UINTN   SubsystemAt;
  UINT16  Subsystem;

  if (EFI_ERROR (SfbReadFileBytes (Root, Path, Header, sizeof (Header), &Read))) {
    return FALSE;
  }

  /* Need at least the DOS header and its e_lfanew field. */
  if (Read < 0x40) {
    return FALSE;
  }

  if (SfbLe16 (Header, 0) != EFI_IMAGE_DOS_SIGNATURE) {
    /* 'MZ' */
    return FALSE;
  }

  PeOffset = SfbLe32 (Header, 0x3C);

  /* Optional header starts after the 4-byte PE signature and 20-byte COFF
   * header; Subsystem is 68 bytes into it. */
  SubsystemAt = (UINTN)PeOffset + 4 + 20 + 68;
  if (SubsystemAt + sizeof (UINT16) > Read) {
    return FALSE;
  }

  if (SfbLe32 (Header, PeOffset) != EFI_IMAGE_NT_SIGNATURE) {
    /* 'PE\0\0' */
    return FALSE;
  }

  Subsystem = SfbLe16 (Header, SubsystemAt);

  return (BOOLEAN)(Subsystem == EFI_IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER ||
                   Subsystem == EFI_IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER);
}

VOID
SfbGetVolumeLabel (
  IN EFI_FILE_PROTOCOL  *Root,
  OUT CHAR16            *Out,
  IN UINTN              OutChars
  )
{
  EFI_STATUS                    Status;
  EFI_FILE_SYSTEM_VOLUME_LABEL  *Label;
  UINTN                         InfoSize;

  if (OutChars == 0) {
    return;
  }

  Out[0] = L'\0';

  InfoSize = 0;
  Status   = Root->GetInfo (
                     Root,
                     &gEfiFileSystemVolumeLabelInfoIdGuid,
                     &InfoSize,
                     NULL
                     );
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return;
  }

  Label = AllocateZeroPool (InfoSize);
  if (Label == NULL) {
    return;
  }

  Status = Root->GetInfo (
                   Root,
                   &gEfiFileSystemVolumeLabelInfoIdGuid,
                   &InfoSize,
                   Label
                   );
  if (!EFI_ERROR (Status)) {
    StrnCpyS (Out, OutChars, Label->VolumeLabel, OutChars - 1);
  }

  FreePool (Label);
}
