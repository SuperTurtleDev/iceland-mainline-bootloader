/** @file QemuBoot.c

  QEMU virt test front-end for the MainlineBoot EFISTUB handoff.

  Runs as \EFI\BOOT\BOOTAA64.EFI from a FAT volume (superfloppy) carrying
  KERNEL / DTB / INITRD / CMDLINE files, installs a fake
  EFI_RAMPARTITION_PROTOCOL describing the QEMU virt memory map, then uses
  the public MainlineBootLib API exactly like BootApp does on the device
  (Init -> LoadKernel/LoadDtb/LoadBootcfg/LoadInitrd -> Boot). Booting the
  test kernel to its init is the pass signal ("QEMU-BOOT-OK" + poweroff).

  SPDX-License-Identifier: BSD-3-Clause-Clear
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MainlineBootLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Guid/FileInfo.h>
#include <Guid/Fdt.h>
#include <Protocol/EFIRamPartition.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

/* Raw ConOut marker (UefiLib Print pulled in formatting deps that made
 * output flaky on the QEMU firmware - this mirrors the canary that works) */
#define QBMARK(txt)  gST->ConOut->OutputString (gST->ConOut, L##txt L"\r\n")

/* QEMU virt: RAM at 0x40000000; leave a 64 MB margin for firmware data */
#define QEMU_RAM_BASE       0x40000000ULL
#define QEMU_RAM_LENGTH     0x38000000ULL      /* 896 MB (with -m 1024) */

STATIC RamPartitionEntry  mQemuRamPartitions[1];

STATIC
EFI_STATUS
EFIAPI
QemuGetRamPartitions (
  IN     EFI_RAMPARTITION_PROTOCOL  *This,
  OUT    RamPartitionEntry          *RamPartitions,
  IN OUT UINT32                     *NumPartition
  )
{
  if (NumPartition == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (RamPartitions == NULL) {
    *NumPartition = 1;
    return EFI_BUFFER_TOO_SMALL;
  }
  if (*NumPartition < 1) {
    *NumPartition = 1;
    return EFI_BUFFER_TOO_SMALL;
  }
  mQemuRamPartitions[0].Base = QEMU_RAM_BASE;
  mQemuRamPartitions[0].AvailableLength = QEMU_RAM_LENGTH;
  CopyMem (RamPartitions, mQemuRamPartitions, sizeof (mQemuRamPartitions));
  *NumPartition = 1;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
QemuGetRamPartitionVersion (
  IN  EFI_RAMPARTITION_PROTOCOL  *This,
  OUT UINT32                     *MajorVersion,
  OUT UINT32                     *MinorVersion
  )
{
  *MajorVersion = 1;
  *MinorVersion = 2;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
QemuGetHighestBankBit (
  IN  EFI_RAMPARTITION_PROTOCOL  *This,
  OUT UINT32                     *HighestBankBit
  )
{
  *HighestBankBit = 29;
  return EFI_SUCCESS;
}

STATIC EFI_RAMPARTITION_PROTOCOL  mQemuRamPartition = {
  EFI_RAMPARTITION_PROTOCOL_REVISION,
  QemuGetRamPartitionVersion,
  QemuGetHighestBankBit,
  QemuGetRamPartitions,
  NULL,                                   /* GetMinPasrSize */
  NULL                                    /* GetPreLoadedImageTable */
};

/* ------------------------------------------------------------------------ */
/* payload files from the FAT volume we booted from                          */
/* ------------------------------------------------------------------------ */

STATIC
EFI_STATUS
QemuReadFile (
  IN  EFI_FILE_PROTOCOL  *Root,
  IN  CONST CHAR16       *Name,
  OUT VOID              **Buffer,
  OUT UINTN              *Size
  )
{
  EFI_STATUS       Status;
  EFI_FILE_PROTOCOL *File;
  EFI_FILE_INFO    *Info;
  UINTN            InfoSize = sizeof (EFI_FILE_INFO) + 32;
  VOID             *Buf;

  *Buffer = NULL;
  *Size = 0;

  Status = Root->Open (Root, &File, (CHAR16 *)Name, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Info = AllocateZeroPool (InfoSize);
  if (Info == NULL) {
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR (Status)) {
    FreePool (Info);
    File->Close (File);
    return Status;
  }

  Buf = AllocatePool ((UINTN)Info->FileSize + 1);
  if (Buf == NULL) {
    FreePool (Info);
    File->Close (File);
    return EFI_OUT_OF_RESOURCES;
  }
  *Size = (UINTN)Info->FileSize;
  FreePool (Info);

  Status = File->Read (File, Size, Buf);
  File->Close (File);
  if (EFI_ERROR (Status)) {
    FreePool (Buf);
    return Status;
  }
  ((CHAR8 *)Buf)[*Size] = '\0';
  *Buffer = Buf;
  return EFI_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* entry                                                                     */
/* ------------------------------------------------------------------------ */

EFI_STATUS
EFIAPI
QemuBootMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                       Status;
  EFI_LOADED_IMAGE_PROTOCOL       *LoadedImage;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Sfs;
  EFI_FILE_PROTOCOL               *Root;
  EFI_HANDLE                       Handle = NULL;
  VOID                            *Kernel = NULL;
  VOID                            *Dtb = NULL;
  VOID                            *Initrd = NULL;
  VOID                            *Bootcfg = NULL;
  UINTN                            Size;

  QBMARK ("QemuBoot: entry");

  /* 1. the fake RAM partition table MainlineBootInit() consumes */
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle, &gEfiRamPartitionProtocolGuid, &mQemuRamPartition,
                  NULL);
  if (EFI_ERROR (Status)) {
    QBMARK ("QemuBoot: ram partition protocol FAILED");
    return Status;
  }

  /* 2. the FAT volume we booted from */
  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid,
                                (VOID **)&LoadedImage);
  if (EFI_ERROR (Status)) {
    QBMARK ("QemuBoot: LoadedImage FAILED");
    return Status;
  }
  Status = gBS->HandleProtocol (LoadedImage->DeviceHandle,
                                &gEfiSimpleFileSystemProtocolGuid,
                                (VOID **)&Sfs);
  if (EFI_ERROR (Status)) {
    QBMARK ("QemuBoot: SFS FAILED");
    return Status;
  }
  Status = Sfs->OpenVolume (Sfs, &Root);
  if (EFI_ERROR (Status)) {
    QBMARK ("QemuBoot: OpenVolume FAILED");
    return Status;
  }

  /* 3. full public MainlineBootLib flow, like BootApp on the device */
  Status = MainlineBootInit ();
  if (EFI_ERROR (Status)) {
    QBMARK ("QemuBoot: MainlineBootInit FAILED");
    return Status;
  }

  Status = QemuReadFile (Root, L"KERNEL", &Kernel, &Size);
  if (!EFI_ERROR (Status)) {
    Status = MainlineBootLoadKernel (Kernel, Size);
    FreePool (Kernel);
  }
  if (EFI_ERROR (Status)) {
    QBMARK ("QemuBoot: KERNEL FAILED");
    return Status;
  }

  Status = QemuReadFile (Root, L"DTB", &Dtb, &Size);
  if (EFI_ERROR (Status)) {
    /* fall back to the firmware's own FDT from the configuration table
     * (QEMU edk2 publishes one) - exercises the handoff without our
     * device tree patch */
    UINTN  i;
    Dtb = NULL;
    for (i = 0; i < gST->NumberOfTableEntries; i++) {
      EFI_CONFIGURATION_TABLE  *T = &gST->ConfigurationTable[i];
      if (CompareGuid (&T->VendorGuid, &gFdtTableGuid) && T->VendorTable != NULL) {
        Dtb = T->VendorTable;
        QBMARK ("QemuBoot: using firmware FDT from config table");
        break;
      }
    }
    if (Dtb == NULL) {
      QBMARK ("QemuBoot: no DTB file and no firmware FDT");
      return EFI_NOT_FOUND;
    }
    Status = MainlineBootLoadDtb (Dtb, MAX_ADDRESS);  /* copies via totalsize */
    Dtb = NULL;                                       /* not our allocation */
    Size = 0;
  } else {
    Status = MainlineBootLoadDtb (Dtb, Size);
    FreePool (Dtb);
  }
  if (EFI_ERROR (Status)) {
    QBMARK ("QemuBoot: DTB FAILED");
    return Status;
  }

  /* CMDLINE = a bootcfg text file ("cmdline=..."); optional */
  Status = QemuReadFile (Root, L"CMDLINE", &Bootcfg, &Size);
  if (!EFI_ERROR (Status)) {
    Status = MainlineBootLoadBootcfg (Bootcfg, Size);
    FreePool (Bootcfg);
    if (Status == EFI_NOT_FOUND) {
      QBMARK ("QemuBoot: CMDLINE has no cmdline line");
    } else {
        QBMARK ("QemuBoot: bootcfg loaded");
    }
  } else {
    QBMARK ("QemuBoot: no CMDLINE file");
  }

  /* INITRD is optional too */
  Status = QemuReadFile (Root, L"INITRD", &Initrd, &Size);
  if (!EFI_ERROR (Status)) {
    Status = MainlineBootLoadInitrd (Initrd, Size);
    FreePool (Initrd);
    if (EFI_ERROR (Status)) {
      QBMARK ("QemuBoot: INITRD FAILED");
      return Status;
    }
  } else {
    QBMARK ("QemuBoot: no INITRD");
  }

  /* 4. the code under test: config table + initrd LoadFile2 + LoadImage/
   *    StartImage into the kernel EFI stub. Does not return on success. */
  QBMARK ("QemuBoot: calling MainlineBootBoot");
  Status = MainlineBootBoot ();
  QBMARK ("QemuBoot: MainlineBootBoot RETURNED");
  return Status;
}
