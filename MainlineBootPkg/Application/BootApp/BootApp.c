/** @file BootApp.c

  BOOTAA64.EFI — production disk-boot loader for the OnePlus Pad 4
  (Qualcomm SM8850 "kaanapali", project iceland) mainline Linux port.

  The BDS (gbl ABL LinuxLoader with the SuperFb boot menu) launches this
  application unattended from <esp>:\EFI\BOOT\BOOTAA64.EFI via
  LoadImage()/StartImage() (SfbLaunchEntry, SuperFbEntries.c).  An
  application that returns is unloaded by the boot services core and the
  menu comes back — so on any failure we print the reason and RETURN.
  There is deliberately no CpuDeadLoop() here.

  Boot flow:
    0. connect every controller (the SuperFb BDS only connected the ESP
       volume; ABL itself always runs after a full connect pass, which is
       why its fastboot can find partitions by name.  We do the same
       EfiBootManagerConnectAll()-style pass ourselves so the GPT
       partitions without a file system — kernel/dtb/bootcfg/initrd —
       have their BlockIo child handles no matter how we were started)
    1. MainlineBootInit ()
    2. read each payload from its GPT partition BY NAME.  Every payload
       partition holds
           [UINT32 LE payload byte size][payload data]
       written by the flash tool (the prefixed images in out_stage/hdr).
       Only the exact payload is handed on to the loader library.
    3. MainlineBootLoadKernel / LoadDtb / LoadBootcfg / LoadInitrd
       (kernel and dtb are required, bootcfg and initrd are optional)
    4. MainlineBootBoot () — does not return on success

  Partition lookup is a VERBATIM copy of ABL's
  QcomModulePkg/Library/BootLib/LinuxLoaderLib.c GetBlkIOHandles() and
  CompareVolumeLabel() from this tree, together with the BLK_IO_SEL_*
  flags, PartiSelectFilter and HandleInfo from
  QcomModulePkg/Include/Library/LinuxLoaderLib.h.  It is used exactly the
  way ABL's own LoadImageFromPartition() uses it
  (MBR|GPT|NON_REMOVABLE|MATCH_PARTITION_LABEL), which is the same
  mechanism FastbootCmds.c PartitionGetInfo() relies on for flashing by
  name: the partition-record protocol (gEfiPartitionRecordGuid) first,
  the standard EFI_PARTITION_INFO protocol as fallback, then a name
  comparison.  The MainlineBoot* library is left untouched.

  SPDX-License-Identifier: BSD-3-Clause-Clear
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MainlineBootLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Guid/FileSystemInfo.h>
#include <Library/DevicePathLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Uefi/UefiGpt.h>

/* ======================================================================== */
/* =====  begin ABL copy: QcomModulePkg/Include/Library/LinuxLoaderLib.h  == */
/* ======================================================================== */

/* Selection attributes for selecting the BlkIo handles */
#define BLK_IO_SEL_MEDIA_TYPE_REMOVABLE 0x0001
#define BLK_IO_SEL_MEDIA_TYPE_NON_REMOVABLE 0x0002
#define BLK_IO_SEL_PARTITIONED_GPT 0x0004
#define BLK_IO_SEL_PARTITIONED_MBR 0x0008
#define BLK_IO_SEL_MATCH_PARTITION_TYPE_GUID 0x0010
#define BLK_IO_SEL_SELECT_MOUNTED_FILESYSTEM 0x0020
#define BLK_IO_SEL_SELECT_BY_VOLUME_NAME 0x0040

/* Select only the root device handle indicated. Doesn't return
 * any partitions within.
 * Currently this filter applies only for eMMC device, not the external
 * device connected via USB */
#define BLK_IO_SEL_SELECT_ROOT_DEVICE_ONLY 0x0080
/* Select the handle that's on the indicated root device.
 * Currently this filter applies only for eMMC device, not the external
 * device connected via USB */
#define BLK_IO_SEL_MATCH_ROOT_DEVICE 0x0100

/* Select through partition name*/
#define BLK_IO_SEL_MATCH_PARTITION_LABEL 0x0200

/* Do case insensetive string comparisons */
#define BLK_IO_SEL_STRING_CASE_INSENSITIVE 0x0400

/* Partitioning scheme types for selecting the BlkIo handles */
#define PARTITIONED_TYPE_MBR 0x01
#define PARTITIONED_TYPE_GPT 0x02

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) sizeof (a) / sizeof (*a)
#endif

#define MAX_HANDLE_INFO_LIST 128

/* Any data specific to additional attributes can be added here. */
typedef struct {
  EFI_GUID *RootDeviceType; /* GUID Selecting the root device type */
  EFI_GUID *PartitionType;  /* Partition Type to match */
  CHAR8 *VolumeName;        /* Mounted filesystem volume name to match */
  CHAR16 *PartitionLabel;   /* Partition label to match */
} PartiSelectFilter;

/* Output data providing more information about the device handle */
typedef struct {
  /* Handle that has BlkIO protocol installed, returned for all type of filters
   */
  EFI_HANDLE *Handle;

  /* Block IO protocol interface is returned for all type of filters */
  EFI_BLOCK_IO_PROTOCOL *BlkIo;

  /* This HDD dev path is returned only if Matching Partition type is requested
   * It should be noted that the contents of this memory should NOT be changed
   */
  const HARDDRIVE_DEVICE_PATH *PartitionInfo;
} HandleInfo;

/* ======================================================================== */
/* =====  end ABL copy: QcomModulePkg/Include/Library/LinuxLoaderLib.h  ==== */
/* ======================================================================== */

/* ======================================================================== */
/* =====  begin ABL copy: QcomModulePkg/Library/BootLib/LinuxLoaderLib.c  == */
/* =====  (GetBlkIOHandles + CompareVolumeLabel, unmodified)             ==== */
/* ======================================================================== */

/* Volume Label size 11 chars, round off to 16 */
#define VOLUME_LABEL_SIZE 16

/* List of all the filters that need device path protocol in the handle to
 * filter */
#define FILTERS_NEEDING_DEVICEPATH                                             \
  (BLK_IO_SEL_PARTITIONED_MBR | BLK_IO_SEL_PARTITIONED_GPT |                   \
   BLK_IO_SEL_MATCH_PARTITION_TYPE_GUID | BLK_IO_SEL_SELECT_ROOT_DEVICE_ONLY | \
   BLK_IO_SEL_MATCH_ROOT_DEVICE)

/* Returns 0 if the volume label matches otherwise non zero */
STATIC UINTN
CompareVolumeLabel (IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL*   Fs,
                    IN CHAR8*                             ReqVolumeName)
{
  INT32 CmpResult;
  UINT32 j;
  UINT16 VolumeLabel[VOLUME_LABEL_SIZE];
  EFI_FILE_PROTOCOL  *FsVolume = NULL;
  EFI_STATUS         Status;
  UINTN                               Size;
  EFI_FILE_SYSTEM_INFO                *FsInfo;

  // Get information about the volume
  Status = Fs->OpenVolume (Fs, &FsVolume);

  if (Status != EFI_SUCCESS) {
    return 1;
  }

  /* Get the Volume name */
  Size = 0;
  FsInfo = NULL;
  Status = FsVolume->GetInfo (FsVolume, &gEfiFileSystemInfoGuid, &Size, FsInfo);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    FsInfo = AllocateZeroPool (Size);
    Status = FsVolume->GetInfo (FsVolume,
                                &gEfiFileSystemInfoGuid, &Size, FsInfo);
    if (Status != EFI_SUCCESS) {
      FreePool (FsInfo);
      return 1;
    }
  }

  if (FsInfo == NULL) {
    return 1;
  }

  /* Convert the passed in Volume name to Wide char and upper case */
  for (j = 0; (j < VOLUME_LABEL_SIZE - 1) && ReqVolumeName[j]; ++j) {
    VolumeLabel[j] = ReqVolumeName[j];

    if ((VolumeLabel[j] >= 'a') &&
        (VolumeLabel[j] <= 'z')) {
      VolumeLabel[j] -= ('a' - 'A');
    }
  }

  /* Null termination */
  VolumeLabel[j] = 0;

  /* Change any lower chars in volume name to upper
   * (ideally this is not needed) */
  for (j = 0; (j < VOLUME_LABEL_SIZE - 1) && FsInfo->VolumeLabel[j]; ++j) {
    if ((FsInfo->VolumeLabel[j] >= 'a') &&
          (FsInfo->VolumeLabel[j] <= 'z')) {
      FsInfo->VolumeLabel[j] -= ('a' - 'A');
    }
  }

  CmpResult = StrnCmp (FsInfo->VolumeLabel, VolumeLabel, VOLUME_LABEL_SIZE);

  FreePool (FsInfo);
  FsVolume->Close (FsVolume);

  return CmpResult;
}

/**
  Returns a list of BlkIO handles based on required criteria
SelectionAttrib : Bitmask representing the conditions that need
to be met for the handles returned. Based on the
selections filter members should have valid values.
FilterData      : Instance of Partition Select Filter structure that
needs extended data for certain type flags. For example
Partition type and/or Volume name can be specified.
HandleInfoPtr   : Pointer to array of HandleInfo structures in which the
output is returned.
MaxBlkIopCnt    : On input, max number of handle structures the buffer can hold,
On output, the number of handle structures returned.

@retval EFI_SUCCESS if the operation was successful
*/
EFI_STATUS
EFIAPI
GetBlkIOHandles (IN UINT32 SelectionAttrib,
                 IN PartiSelectFilter *FilterData,
                 OUT HandleInfo *HandleInfoPtr,
                 IN OUT UINT32* MaxBlkIopCnt)
{
  EFI_BLOCK_IO_PROTOCOL *BlkIo;
  EFI_HANDLE *BlkIoHandles;
  UINTN BlkIoHandleCount;
  UINTN i;
  UINTN DevicePathDepth;
  HARDDRIVE_DEVICE_PATH *Partition, *PartitionOut;
  EFI_STATUS Status;
  EFI_DEVICE_PATH_PROTOCOL *DevPathInst;
  EFI_DEVICE_PATH_PROTOCOL *TempDevicePath;
  VENDOR_DEVICE_PATH *RootDevicePath;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL     *Fs;
  UINT32 BlkIoCnt = 0;
  EFI_PARTITION_ENTRY *PartEntry;
  EFI_PARTITION_INFO_PROTOCOL  *PartitionInfo;

  if ((MaxBlkIopCnt == NULL) || (HandleInfoPtr == NULL))
    return EFI_INVALID_PARAMETER;

  /* Adjust some defaults first */
  if ((SelectionAttrib & (BLK_IO_SEL_MEDIA_TYPE_REMOVABLE |
                          BLK_IO_SEL_MEDIA_TYPE_NON_REMOVABLE)) == 0)
    SelectionAttrib |=
        (BLK_IO_SEL_MEDIA_TYPE_REMOVABLE | BLK_IO_SEL_MEDIA_TYPE_NON_REMOVABLE);

  if (((BLK_IO_SEL_PARTITIONED_GPT | BLK_IO_SEL_PARTITIONED_MBR) &
       SelectionAttrib) == 0)
    SelectionAttrib |=
        (BLK_IO_SEL_PARTITIONED_GPT | BLK_IO_SEL_PARTITIONED_MBR);

  /* If we need Filesystem handle then search based on that its narrower search
   * than BlkIo */
  if (SelectionAttrib & (BLK_IO_SEL_SELECT_MOUNTED_FILESYSTEM |
                         BLK_IO_SEL_SELECT_BY_VOLUME_NAME)) {
    Status =
        gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid,
                                 NULL, &BlkIoHandleCount, &BlkIoHandles);
  } else {
    Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid,
                                      NULL, &BlkIoHandleCount, &BlkIoHandles);
  }

  if (Status != EFI_SUCCESS) {
    DEBUG (
        (EFI_D_ERROR, "Unable to get Filesystem Handle buffer %r\n", Status));
    return Status;
  }

  /* Loop through to search for the ones we are interested in. */
  for (i = 0; i < BlkIoHandleCount; i++) {

    Status = gBS->HandleProtocol (BlkIoHandles[i], &gEfiBlockIoProtocolGuid,
                                  (VOID **)&BlkIo);
    /* Fv volumes will not support Blk I/O protocol */
    if (Status == EFI_UNSUPPORTED) {
      continue;
    }

    if (Status != EFI_SUCCESS) {
      DEBUG ((EFI_D_ERROR, "Unable to get Filesystem Handle %r\n", Status));
      return Status;
    }

    /* Check if the media type criteria (for removable/not) satisfies */
    if (BlkIo->Media->RemovableMedia) {
      if ((SelectionAttrib & BLK_IO_SEL_MEDIA_TYPE_REMOVABLE) == 0)
        continue;
    } else {
      if ((SelectionAttrib & BLK_IO_SEL_MEDIA_TYPE_NON_REMOVABLE) == 0)
        continue;
    }

    /* Clear the pointer, we can get it if the filter is set */
    PartitionOut = NULL;

    /* Check if partition related criteria satisfies */
    if ((SelectionAttrib & FILTERS_NEEDING_DEVICEPATH) != 0) {
      Status = gBS->HandleProtocol (
          BlkIoHandles[i], &gEfiDevicePathProtocolGuid, (VOID **)&DevPathInst);

      /* If we didn't get the DevicePath Protocol then this handle
       * cannot be used */
      if (EFI_ERROR (Status))
        continue;

      DevicePathDepth = 0;

      /* Get the device path */
      TempDevicePath = DevPathInst;
      RootDevicePath = (VENDOR_DEVICE_PATH *)DevPathInst;
      Partition = (HARDDRIVE_DEVICE_PATH *)TempDevicePath;

      if ((SelectionAttrib & (BLK_IO_SEL_SELECT_ROOT_DEVICE_ONLY |
                              BLK_IO_SEL_MATCH_ROOT_DEVICE)) != 0) {
        if (!FilterData) {
          return EFI_INVALID_PARAMETER;
        }
        /* If this is not the root device that we are looking for, ignore this
         * handle */
        if (RootDevicePath->Header.Type != HARDWARE_DEVICE_PATH ||
            RootDevicePath->Header.SubType != HW_VENDOR_DP ||
            (RootDevicePath->Header.Length[0] |
             (RootDevicePath->Header.Length[1] << 8)) !=
#ifdef AUTO_VIRT_ABL
                (sizeof (VENDOR_DEVICE_PATH) + sizeof (UINT64)) ||
#else
                sizeof (VENDOR_DEVICE_PATH) ||
#endif
            ((FilterData->RootDeviceType != NULL) &&
             (CompareGuid (FilterData->RootDeviceType,
                           &RootDevicePath->Guid) == FALSE)))
          continue;
      }

      /* Locate the last Device Path Node */
      while (!IsDevicePathEnd (TempDevicePath)) {
        DevicePathDepth++;
        Partition = (HARDDRIVE_DEVICE_PATH *)TempDevicePath;
        TempDevicePath = NextDevicePathNode (TempDevicePath);
      }

      /* If we need the handle for root device only and if this is representing
       * a sub partition in the root device then ignore this handle */
      if (SelectionAttrib & BLK_IO_SEL_SELECT_ROOT_DEVICE_ONLY)
        if (DevicePathDepth > 1)
          continue;

      /* Check if the last node is Harddrive Device path that contains the
       * Partition information */
#ifndef AUTO_VIRT_ABL
      if (Partition->Header.Type == MEDIA_DEVICE_PATH &&
          Partition->Header.SubType == MEDIA_HARDDRIVE_DP &&
          (Partition->Header.Length[0] | (Partition->Header.Length[1] << 8)) ==
              sizeof (*Partition)) {
#else
      if (Partition->Header.Type == HARDWARE_DEVICE_PATH &&
          Partition->Header.SubType == HW_VENDOR_DP &&
          (Partition->Header.Length[0] | (Partition->Header.Length[1] << 8)) ==
              (sizeof (VENDOR_DEVICE_PATH) + sizeof (UINT64))) {
#endif

        PartitionOut = Partition;

        if ((SelectionAttrib & BLK_IO_SEL_PARTITIONED_GPT) == 0)
          if (Partition->MBRType == PARTITIONED_TYPE_GPT)
            continue;

        if ((SelectionAttrib & BLK_IO_SEL_PARTITIONED_MBR) == 0)
          if (Partition->MBRType == PARTITIONED_TYPE_MBR)
            continue;

        /* PartitionDxe implementation should return partition type also */
        if ((SelectionAttrib & BLK_IO_SEL_MATCH_PARTITION_TYPE_GUID) != 0) {
          GUID *PartiType;
          VOID *Interface;

          if (!FilterData ||
                FilterData->PartitionType == NULL) {
              return EFI_INVALID_PARAMETER;
          }

          Status = gBS->HandleProtocol (BlkIoHandles[i],
                                        FilterData->PartitionType,
                                        (VOID**)&Interface);
          if (EFI_ERROR (Status)) {
              Status = gBS->HandleProtocol (BlkIoHandles[i],
                              &gEfiPartitionTypeGuid,
                              (VOID **)&PartiType);
              if (EFI_ERROR (Status)) {
                continue;
              }

              if (CompareGuid (PartiType, FilterData->PartitionType) == FALSE) {
                continue;
              }
          }
        }
      }
      /* If we wanted a particular partition and didn't get the HDD DP,
         then this handle is probably not the interested ones */
      else if ((SelectionAttrib & BLK_IO_SEL_MATCH_PARTITION_TYPE_GUID) != 0)
          continue;
    }

    /* Check if the Filesystem related criteria satisfies */
    if ((SelectionAttrib & BLK_IO_SEL_SELECT_MOUNTED_FILESYSTEM) != 0) {
      Status = gBS->HandleProtocol (BlkIoHandles[i],
                               &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
      if (EFI_ERROR (Status)) {
        continue;
      }

      if ((SelectionAttrib & BLK_IO_SEL_SELECT_BY_VOLUME_NAME) != 0) {
        if (!FilterData ||
             FilterData->VolumeName == NULL) {
          return EFI_INVALID_PARAMETER;
        }
        if (CompareVolumeLabel (Fs, FilterData->VolumeName) != 0) {
          continue;
        }
      }
    }

    /* Check if the Partition name related criteria satisfies */
    if ((SelectionAttrib & BLK_IO_SEL_MATCH_PARTITION_LABEL) != 0) {
      Status = gBS->HandleProtocol (BlkIoHandles[i], &gEfiPartitionRecordGuid,
                                    (VOID **)&PartEntry);
      if (Status != EFI_SUCCESS) {
        /* Search for gEfiPartitionInfoProtocolGuid */
        Status = gBS->HandleProtocol (BlkIoHandles[i],
                                    &gEfiPartitionInfoProtocolGuid,
                                    (VOID **)&PartitionInfo);
        if (Status != EFI_SUCCESS) {
          continue;
        }
        else {
          DEBUG ((EFI_D_VERBOSE, "GetBlkIO partition: %s \n\r",
                  PartitionInfo->Info.Gpt.PartitionName));
          if (StrnCmp (PartitionInfo->Info.Gpt.PartitionName,
                     FilterData->PartitionLabel,
                     MAX (StrLen (PartitionInfo->Info.Gpt.PartitionName),
                          StrLen (FilterData->PartitionLabel))))
            continue;
        }
      }
      else {
        if (StrnCmp (PartEntry->PartitionName, FilterData->PartitionLabel,
                     MAX (StrLen (PartEntry->PartitionName),
                          StrLen (FilterData->PartitionLabel))))
          continue;
      }
     }
    /* We came here means, this handle satisfies all the conditions needed,
     * Add it into the list */
    HandleInfoPtr[BlkIoCnt].Handle = BlkIoHandles[i];
    HandleInfoPtr[BlkIoCnt].BlkIo = BlkIo;
    HandleInfoPtr[BlkIoCnt].PartitionInfo = PartitionOut;
    BlkIoCnt++;
    if (BlkIoCnt >= *MaxBlkIopCnt)
      break;
  }

  *MaxBlkIopCnt = BlkIoCnt;

  /* Free the handle buffer */
  if (BlkIoHandles != NULL) {
    FreePool (BlkIoHandles);
    BlkIoHandles = NULL;
  }

  return EFI_SUCCESS;
}

/* ======================================================================== */
/* =====  end ABL copy: QcomModulePkg/Library/BootLib/LinuxLoaderLib.c  ==== */
/* ======================================================================== */

/* ======================================================================== */
/* BootApp                                                                  */
/* ======================================================================== */

#define BA_VERSION_STRING  "0.3"

/* GPT partition names holding the payloads (must match the flash side). */
#define BA_KERNEL_LABEL    L"kernel"
#define BA_DTB_LABEL       L"dtb"
#define BA_BOOTCFG_LABEL   L"bootcfg"
#define BA_INITRD_LABEL    L"initrd"

/* How many matching handles to inspect: two partitions with the same name
 * is a flashing error we refuse (ABL likewise refuses to load
 * nondeterministically: "multiple partitions found"). */
#define BA_PARTITION_MATCHES  2

/* Sanity cap for the size prefix so garbage cannot make us allocate RAM. */
#define BA_MAX_PAYLOAD_BYTES  ((UINT64)1024 * 1024 * 1024)

/* How long an error message stays readable before we return to the menu. */
#define BA_ERROR_DELAY_US     (10 * 1000 * 1000)

/* A MainlineBootLib Load* function. */
typedef
EFI_STATUS
(EFIAPI *BA_LOAD_FN) (
  IN VOID   *Buffer,
  IN UINTN  Size
  );

/* Print a failure and give the user time to read it, then hand the
 * passed status back so BootAppEntry can return it to BDS.  Never loops. */
STATIC
EFI_STATUS
BootAppFail (
  IN CONST CHAR16  *What,
  IN EFI_STATUS     Status
  )
{
  Print (L"[BOOTAPP] ERROR: %s: %r\r\n", What, Status);
  if (gBS != NULL) {
    Print (L"[BOOTAPP] returning to the boot menu in 10 s\r\n");
    gBS->Stall (BA_ERROR_DELAY_US);
  }
  return Status;
}

/* Touch every handle once so bus drivers create all their children — the
 * same thing EfiBootManagerConnectAll() does (MdeModulePkg BmConnect.c)
 * and the state ABL itself runs in after the platform BDS connect pass.
 * Needed because the SuperFb menu may have connected only the ESP volume:
 * partitions without a file system get their BlockIo child handles from
 * PartitionDxe only when it is started on the whole-disk handle. */
STATIC
VOID
BootAppConnectAllControllers (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       Count;
  UINTN       Index;

  Handles = NULL;
  Count = 0;
  Status = gBS->LocateHandleBuffer (AllHandles, NULL, NULL, &Count, &Handles);
  if (EFI_ERROR (Status) || Handles == NULL) {
    DEBUG ((EFI_D_ERROR, "BootApp: LocateHandleBuffer(AllHandles): %r\n",
            Status));
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
  }

  FreePool (Handles);
}

/* Find a partition by GPT label, exactly like ABL LoadImageFromPartition():
 * MBR|GPT, non-removable, match-by-partition-label, then demand exactly one
 * hit. */
STATIC
EFI_STATUS
BootAppFindPartition (
  IN  CONST CHAR16             *Label,
  OUT EFI_BLOCK_IO_PROTOCOL   **BlkIo
  )
{
  EFI_STATUS         Status;
  PartiSelectFilter  Filter;
  HandleInfo         Matches[BA_PARTITION_MATCHES];
  UINT32             MaxHandles;
  UINT32             Attrib;

  Attrib = BLK_IO_SEL_PARTITIONED_MBR |
           BLK_IO_SEL_PARTITIONED_GPT |
           BLK_IO_SEL_MEDIA_TYPE_NON_REMOVABLE |
           BLK_IO_SEL_MATCH_PARTITION_LABEL;

  Filter.RootDeviceType = NULL;
  Filter.PartitionType = NULL;
  Filter.VolumeName = NULL;
  Filter.PartitionLabel = (CHAR16 *)Label;

  MaxHandles = ARRAY_SIZE (Matches);
  ZeroMem (Matches, sizeof (Matches));

  Status = GetBlkIOHandles (Attrib, &Filter, Matches, &MaxHandles);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (MaxHandles == 0) {
    return EFI_NOT_FOUND;
  }
  if (MaxHandles > 1) {
    return EFI_LOAD_ERROR;
  }
  if (Matches[0].BlkIo == NULL || Matches[0].BlkIo->Media == NULL) {
    return EFI_DEVICE_ERROR;
  }

  *BlkIo = Matches[0].BlkIo;
  return EFI_SUCCESS;
}

/* Allocate Size bytes usable as a ReadBlocks target for this device
 * (honours Media->IoAlign).  *PoolBase receives the pool pointer to free;
 * the return value is the aligned buffer. */
STATIC
VOID *
BootAppAllocateAligned (
  IN  EFI_BLOCK_IO_MEDIA  *Media,
  IN  UINTN                Size,
  OUT VOID               **PoolBase
  )
{
  UINTN  Align;
  VOID   *Pool;

  *PoolBase = NULL;

  Align = Media->IoAlign;
  if (Align < 8) {
    Align = 8;
  }

  Pool = AllocatePool (Size + Align);
  if (Pool == NULL) {
    return NULL;
  }

  *PoolBase = Pool;
  return (VOID *)(((UINTN)Pool + Align - 1) & ~((UINTN)Align - 1));
}

/*
  Read one payload partition.  On-disk layout (fixed protocol, written by
  the flash tool — the prefixed images in out_stage/hdr):
      LBA 0:  [UINT32 little-endian payload byte size][payload data ...]

  On success *Payload points PayloadSize bytes into *Stage (the aligned
  read buffer) and *Stage is what to FreePool() afterwards.  The Load*
  functions of MainlineBootLib copy the payload into library-owned memory,
  so the caller frees *Stage as soon as the Load call returned.

  @retval EFI_NOT_FOUND      partition absent (or size prefix zero)
  @retval EFI_BAD_BUFFER_SIZE  size prefix exceeds the partition — flashed
                             without the size prefix?
*/
STATIC
EFI_STATUS
BootAppReadPayload (
  IN  CONST CHAR16             *Label,
  OUT VOID                    **Payload,
  OUT UINTN                    *PayloadSize,
  OUT VOID                    **Stage
  )
{
  EFI_STATUS             Status;
  EFI_BLOCK_IO_PROTOCOL  *BlkIo;
  EFI_BLOCK_IO_MEDIA     *Media;
  UINT64                 PartitionBytes;
  UINTN                  ReadBytes;
  VOID                   *Pool;
  VOID                   *Aligned;
  UINT32                 Size;

  *Payload = NULL;
  *PayloadSize = 0;
  *Stage = NULL;

  Status = BootAppFindPartition (Label, &BlkIo);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Media = BlkIo->Media;
  if (Media->BlockSize == 0) {
    return EFI_DEVICE_ERROR;
  }
  PartitionBytes = (UINT64)(Media->LastBlock + 1) * Media->BlockSize;

  /* First block: the 4-byte size prefix lives at LBA 0. */
  Aligned = BootAppAllocateAligned (Media, Media->BlockSize, &Pool);
  if (Aligned == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = BlkIo->ReadBlocks (BlkIo, Media->MediaId, 0,
                              Media->BlockSize, Aligned);
  if (EFI_ERROR (Status)) {
    Print (L"[BOOTAPP]   ReadBlocks(header) failed: %r\r\n", Status);
    FreePool (Pool);
    return Status;
  }

  Size = ReadUnaligned32 ((CONST UINT32 *)Aligned);
  FreePool (Pool);

  if (Size == 0) {
    /* Empty or never-flashed: indistinguishable from absent. */
    return EFI_NOT_FOUND;
  }
  if ((UINT64)Size + 4 > PartitionBytes) {
    Print (L"[BOOTAPP]   size header 0x%x exceeds partition size 0x%lx - "
           L"was it flashed without the 4-byte size prefix?\r\n",
           Size, PartitionBytes);
    return EFI_BAD_BUFFER_SIZE;
  }
  if ((UINT64)Size > BA_MAX_PAYLOAD_BYTES) {
    Print (L"[BOOTAPP]   size header 0x%x over sanity cap, refusing\r\n", Size);
    return EFI_UNSUPPORTED;
  }

  /* Whole payload: header + data, rounded up to whole blocks. */
  ReadBytes = ((UINTN)Size + 4 + Media->BlockSize - 1) /
              Media->BlockSize * Media->BlockSize;

  Aligned = BootAppAllocateAligned (Media, ReadBytes, &Pool);
  if (Aligned == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = BlkIo->ReadBlocks (BlkIo, Media->MediaId, 0, ReadBytes, Aligned);
  if (EFI_ERROR (Status)) {
    Print (L"[BOOTAPP]   ReadBlocks(0x%lx bytes) failed: %r\r\n",
           (UINT64)ReadBytes, Status);
    FreePool (Pool);
    return Status;
  }

  *Stage = Pool;
  *Payload = (VOID *)((UINT8 *)Aligned + 4);
  *PayloadSize = Size;
  return EFI_SUCCESS;
}

/*
  Read one payload partition and hand exactly the payload bytes to a
  MainlineBootLib Load* function.  Prints its own progress lines.

  @retval EFI_NOT_FOUND  partition absent (caller decides whether that is
                         fatal for this payload)
  @retval other          read or Load error (always fatal for the caller)
*/
STATIC
EFI_STATUS
BootAppLoadPayload (
  IN CONST CHAR16  *Label,
  IN BA_LOAD_FN     LoadFn,
  IN CONST CHAR16  *What
  )
{
  EFI_STATUS  Status;
  VOID        *Payload;
  UINTN       Size;
  VOID        *Stage;

  Print (L"[BOOTAPP] %s: reading '%s' partition...\r\n", What, Label);

  Status = BootAppReadPayload (Label, &Payload, &Size, &Stage);
  if (Status == EFI_NOT_FOUND) {
    Print (L"[BOOTAPP] %s: partition '%s' not found\r\n", What, Label);
    return Status;
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Print (L"[BOOTAPP] %s: %lu bytes, loading\r\n", What, (UINT64)Size);

  Status = LoadFn (Payload, Size);
  FreePool (Stage);

  if (EFI_ERROR (Status)) {
    Print (L"[BOOTAPP] %s: rejected: %r\r\n", What, Status);
  }
  return Status;
}

EFI_STATUS
EFIAPI
BootAppEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *BlkIoHandles;
  UINTN       BlkIoCount;

  /* Not used: the entry point runs with gST/gBS already set up by
   * UefiApplicationEntryPoint. */
  (VOID)ImageHandle;
  (VOID)SystemTable;

  Print (L"\r\n[BOOTAPP] BootApp v%a (built %a %a) - mainline disk boot\r\n",
         BA_VERSION_STRING, __DATE__, __TIME__);

  /* 0. Make sure every controller is connected so all GPT partitions
   *    carry BlockIo handles (see BootAppConnectAllControllers). */
  Print (L"[BOOTAPP] connecting controllers...\r\n");
  BootAppConnectAllControllers ();

  BlkIoHandles = NULL;
  BlkIoCount = 0;
  if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol,
                                           &gEfiBlockIoProtocolGuid, NULL,
                                           &BlkIoCount, &BlkIoHandles))) {
    Print (L"[BOOTAPP] %u block devices online\r\n", (UINT32)BlkIoCount);
    if (BlkIoHandles != NULL) {
      FreePool (BlkIoHandles);
    }
  }

  /* 1. Platform boot parameters (RAM partitions, kernel load region). */
  Print (L"[BOOTAPP] MainlineBootInit...\r\n");
  Status = MainlineBootInit ();
  if (EFI_ERROR (Status)) {
    return BootAppFail (L"MainlineBootInit", Status);
  }

  /* 2. Kernel (arm64 Image, required). */
  Status = BootAppLoadPayload (BA_KERNEL_LABEL, MainlineBootLoadKernel,
                               L"kernel");
  if (EFI_ERROR (Status)) {
    return BootAppFail (L"kernel payload", Status);
  }

  /* 3. DTB (flat device tree, required). */
  Status = BootAppLoadPayload (BA_DTB_LABEL, MainlineBootLoadDtb, L"dtb");
  if (EFI_ERROR (Status)) {
    return BootAppFail (L"dtb payload", Status);
  }

  /* 4. bootcfg (text ini, optional): absent partition or a file without a
   *    "cmdline=" line both just mean "boot without cmdline".  The optional
   *    kernel-base=/kernel-size= keys override the ABL boot window (see
   *    MainlineBootLoadBootcfg). */
  Status = BootAppLoadPayload (BA_BOOTCFG_LABEL, MainlineBootLoadBootcfg,
                               L"bootcfg");
  if (Status == EFI_NOT_FOUND) {
    Print (L"[BOOTAPP] bootcfg is optional - continuing without cmdline\r\n");
  } else if (EFI_ERROR (Status)) {
    return BootAppFail (L"bootcfg payload", Status);
  }

  /* 5. initrd (cpio, optional). */
  Status = BootAppLoadPayload (BA_INITRD_LABEL, MainlineBootLoadInitrd,
                               L"initrd");
  if (Status == EFI_NOT_FOUND) {
    Print (L"[BOOTAPP] initrd is optional - continuing without initrd\r\n");
  } else if (EFI_ERROR (Status)) {
    return BootAppFail (L"initrd payload", Status);
  }

  /* 6. Boot.  MainlineBootBoot() prints its own final [BOOTAPP] lines and
   *    does not return on success (ExitBootServices + jump to the kernel). */
  Print (L"[BOOTAPP] all payloads staged - booting\r\n");
  Status = MainlineBootBoot ();
  return BootAppFail (L"MainlineBootBoot", Status);
}
