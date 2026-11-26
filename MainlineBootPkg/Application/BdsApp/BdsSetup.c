/*
 * BIOS Setup adapted from Canoe SuperFbAdvanced: USB mass-storage export of UFS LUNs, partitions and the
 * config store, the grouped volume browser, and boot entry order/deletion.
 *
 *   BIOS Setup
 *    |- USB Mass Storage
 *    |   |- Export a partition   -> LUN picker -> partition picker -> mode
 *    |   |- Export a LUN         -> LUN picker -> mode
 *    |   `- Export Config Store  -> mode
 *    |- Volumes
 *    |- Boot Order
 *    `- Boot Entries
 *
 * A partition is exported behind a RAM-backed fake GPT so the host sees a
 * valid one-partition disk instead of an uninitialized one; the fake tables
 * are write-protected in both mount modes. A whole LUN is exported
 * one-to-one, optionally forced read-only. The mount mode chooser runs before
 * every export.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "BdsSetup.h"
#include "BdsApp.h"
#include "BdsEntries.h"
#include "BdsMenu.h"
#include "BdsUsbDisk.h"
#include "BdsUsbMsd.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PartitionTableUpdate.h>
#include <Library/PrintLib.h>
#include <Library/SfbConfigStore.h>
#include <Library/SfbSecurity.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Guid/Gpt.h>
#include <Uefi/UefiGpt.h>

// Qualcomm publishes the GPT partition record using this protocol GUID.
extern EFI_GUID  gEfiPartitionRecordGuid;

#define SFB_ADV_ROW_CHARS  80

typedef CHAR16 SFB_ADV_ROW[SFB_ADV_ROW_CHARS];
#define SFB_ADV_MAX_LUNS  MAX_LUNS

/* Snapshot of one pickable target, resolved at list build time. The handles
 * behind UFS partitions live in the platform partition driver and survive USB
 * mode switches, so the pointers stay valid across an export session; the
 * lists are nevertheless rebuilt every time a picker opens. */
typedef struct {
  EFI_PARTITION_ENTRY      *Entry; /* GPT record; NULL for the Back row */
  EFI_BLOCK_IO_PROTOCOL    *BlkIo; /* partition child BlkIo (LBA0 = start) */
  UINT64                   Bytes;
  UINT32                   Lun;
} SFB_ADV_PARTITION;

typedef struct {
  EFI_BLOCK_IO_PROTOCOL    *BlkIo; /* whole-LUN BlkIo (LogicalPartition=FALSE) */
  UINT64                   Bytes;
  UINT32                   Lun;
} SFB_ADV_LUN;

/* ---- tiny generic list chooser ------------------------------------------- */

/*
 * Draw Rows[0..Count) plus a trailing "Back" row and service input. Returns
 * the chosen row index, or -1 when the user picked Back (or the list is
 * empty). Scroll handling matches the boot menu.
 */
STATIC
INTN
SfbAdvChoose (
  IN CONST CHAR16  *Title,
  IN CONST CHAR16  *Subtitle,
  IN CONST CHAR16 (*Rows)[SFB_ADV_ROW_CHARS],
  IN UINTN         Count,
  IN UINTN         StartCursor
  )
{
  UINTN    Cursor = (StartCursor < Count) ? StartCursor : 0;
  UINTN    Total  = Count + 1;
  UINTN    Start;
  UINTN    Index;
  UINTN    Last;
  SFB_KEY  Key;

  while (TRUE) {
    SfbBeginScreen (Title, Subtitle);

    Start = SfbWindowStart (Cursor, Total, SFB_VISIBLE_ROWS);
    Last  = Start + SFB_VISIBLE_ROWS;
    if (Last > Total) {
      Last = Total;
    }

    for (Index = Start; Index < Last; Index++) {
      if (Index < Count) {
        SfbDrawRow ((BOOLEAN)(Index == Cursor), L" ", Rows[Index]);
      } else {
        SfbDrawRow ((BOOLEAN)(Index == Cursor), L" ", L"Back");
      }
    }

    if (Last < Total) {
      Print (L"    ... %u more\r\n", (UINT32)(Total - Last));
    }

    SfbEndScreen (L"Vol Up/Down: move   Power: select");

    Key = SfbWaitForKey (0);
    if (Key == SfbKeyUnavailable) {
      return -1;
    }

    if (Key == SfbKeyTimeout) {
      continue;
    }

    if ((Key == SfbKeyUp) || (Key == SfbKeyDown)) {
      SfbMoveCursor (&Cursor, Total, Key);
      continue;
    }

    if (Cursor < Count) {
      return (INTN)Cursor;
    }

    /* Back: debounce the confirming key before the parent redraws. */
    SfbDebounceMenuExit ();
    return -1;
  }
}

/* ---- mount mode chooser -------------------------------------------------- */

/*
 * TRUE = export read-only. Returns FALSE and leaves *ReadOnly untouched when
 * the user backs out.
 */
STATIC
BOOLEAN
SfbAdvChooseMountMode (
  IN CONST CHAR16  *Target,
  OUT BOOLEAN      *ReadOnly
  )
{
  STATIC CONST CHAR16  Rows[2][SFB_ADV_ROW_CHARS] = {
    L"Read only",
    L"Read Write"
  };
  CHAR16               Subtitle[96];
  INTN                 Chosen;

  UnicodeSPrint (Subtitle, sizeof (Subtitle), L"Mount %s how?", Target);
  Chosen = SfbAdvChoose (L"USB Mass Storage", Subtitle, Rows, 2, 0);
  if (Chosen < 0) {
    return FALSE;
  }

  *ReadOnly = (BOOLEAN)(Chosen == 0);
  return TRUE;
}

/* ---- LUN handling --------------------------------------------------------- */

/*
 * The whole-disk BlockIo behind a partition child: truncate the child's
 * device path before its final node and locate the BlockIo handle of what
 * remains, which is the LUN's own disk handle.
 */
STATIC
EFI_STATUS
SfbAdvLunBlkIoFromPartition (
  IN EFI_HANDLE              PartHandle,
  OUT EFI_BLOCK_IO_PROTOCOL  **BlkIo
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *Dp = NULL;
  EFI_DEVICE_PATH_PROTOCOL  *Node;
  EFI_DEVICE_PATH_PROTOCOL  *Last = NULL;
  EFI_DEVICE_PATH_PROTOCOL  *Trunc;
  EFI_DEVICE_PATH_PROTOCOL  *Remaining;
  EFI_HANDLE                Parent       = NULL;
  EFI_BLOCK_IO_PROTOCOL     *ParentBlkIo = NULL;
  UINTN                     Len;

  *BlkIo = NULL;

  Status = gBS->HandleProtocol (
                  PartHandle,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **)&Dp
                  );
  if (EFI_ERROR (Status) || (Dp == NULL)) {
    return EFI_NOT_FOUND;
  }

  Node = Dp;
  while (!IsDevicePathEnd (Node)) {
    Last = Node;
    Node = NextDevicePathNode (Node);
  }

  if ((Last == NULL) || (Last == Dp)) {
    return EFI_NOT_FOUND;
  }

  Len   = (UINTN)((UINT8 *)Last - (UINT8 *)Dp);
  Trunc = AllocatePool (Len + sizeof (EFI_DEVICE_PATH_PROTOCOL));
  if (Trunc == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  CopyMem (Trunc, Dp, Len);
  SetDevicePathEndNode ((UINT8 *)Trunc + Len);

  Remaining = Trunc;
  Status    = gBS->LocateDevicePath (
                     &gEfiBlockIoProtocolGuid,
                     &Remaining,
                     &Parent
                     );
  FreePool (Trunc);
  if (EFI_ERROR (Status) || (Parent == NULL)) {
    return EFI_NOT_FOUND;
  }

  Status = gBS->HandleProtocol (
                  Parent,
                  &gEfiBlockIoProtocolGuid,
                  (VOID **)&ParentBlkIo
                  );
  if (EFI_ERROR (Status) || (ParentBlkIo == NULL) || (ParentBlkIo->Media == NULL) ||
      ParentBlkIo->Media->LogicalPartition)
  {
    return EFI_NOT_FOUND;
  }

  *BlkIo = ParentBlkIo;
  return EFI_SUCCESS;
}

/*
 * Snapshot the visible UFS LUNs: every LUN that carries at least one GPT
 * partition in the platform partition table. Returns the count; Luns must
 * hold at least SFB_ADV_MAX_LUNS entries.
 */
STATIC
UINTN
SfbAdvCollectLuns (
  OUT SFB_ADV_LUN  *Luns
  )
{
  UINTN       Count = 0;
  UINT32      Lun;
  UINT32      MaxLuns;
  UINTN       PartitionIndex;
  EFI_STATUS  Status;

  Status = EnumeratePartitions ();
  if (EFI_ERROR (Status)) {
    SfbReportStatus (L"Cannot enumerate storage", Status);
    return 0;
  }

  UpdatePartitionEntries ();
  MaxLuns = MIN (GetMaxLuns (), SFB_ADV_MAX_LUNS);

  for (Lun = 0; Lun < MaxLuns; Lun++) {
    EFI_BLOCK_IO_PROTOCOL  *BlkIo = NULL;

    if (Ptable[Lun].MaxHandles == 0) {
      continue;
    }

    for (PartitionIndex = 0; PartitionIndex < MIN (Ptable[Lun].MaxHandles, MAX_NUM_PARTITIONS); PartitionIndex++) {
      if ((Ptable[Lun].HandleInfoList[PartitionIndex].Handle != NULL) &&
          !EFI_ERROR (
             SfbAdvLunBlkIoFromPartition (
               Ptable[Lun].HandleInfoList[PartitionIndex].Handle,
               &BlkIo
               )
             ))
      {
        break;
      }

      BlkIo = NULL;
    }

    if ((BlkIo == NULL) || (BlkIo->Media == NULL)) {
      continue;
    }

    Luns[Count].BlkIo = BlkIo;
    Luns[Count].Lun   = Lun;
    Luns[Count].Bytes =
      (BlkIo->Media->LastBlock + 1) * (UINT64)BlkIo->Media->BlockSize;
    Count++;
  }

  return Count;
}

/* ---- export -------------------------------------------------------------- */

/*
 * Run the mode chooser, then export the partition behind a fake-GPT wrapper.
 * The RAM GPT region stays write-protected in both modes.
 */
STATIC
VOID
SfbAdvExportPartition (
  IN CONST SFB_ADV_PARTITION  *Part
  )
{
  BOOLEAN       ReadOnly = TRUE;
  BDS_USB_DISK  *Disk    = NULL;
  CHAR16        Target[48];
  CHAR16        Detail[96];
  UINT64        Bytes;
  EFI_STATUS    Status;

  StrnCpyS (
    Target,
    ARRAY_SIZE (Target),
    Part->Entry->PartitionName,
    ARRAY_SIZE (Part->Entry->PartitionName)
    );
  if (!SfbAdvChooseMountMode (Target, &ReadOnly)) {
    return;
  }

  Bytes = (Part->BlkIo->Media->LastBlock + 1) *
          (UINT64)Part->BlkIo->Media->BlockSize;

  Status = BdsUsbDiskCreate (
             Part->BlkIo,
             0,
             Part->BlkIo->Media->LastBlock + 1,
             &Part->Entry->PartitionTypeGUID,
             Target,
             ReadOnly,
             &Disk
             );
  if (EFI_ERROR (Status)) {
    SfbReportStatus (L"Could not wrap partition", Status);
    return;
  }

  UnicodeSPrint (
    Detail,
    sizeof (Detail),
    L"Partition: %s (%a)",
    Target,
    ReadOnly ? "read-only" : "read-write"
    );
  SfbConfigStoreUnmount ();
  Status = BdsUsbMsdExport (
             &Disk->BlockIo,
             L"USB Mass Storage",
             Detail,
             Bytes
             );
  (VOID)SfbConfigStoreMount ();
  BdsUsbDiskDestroy (Disk);

  if (EFI_ERROR (Status) && (Status != EFI_ABORTED) &&
      (Status != EFI_MEDIA_CHANGED))
  {
    SfbReportStatus (L"Could not start mass storage", Status);
  }
}

/* Run the mode chooser, then export the whole LUN one-to-one. */
STATIC
VOID
SfbAdvExportLun (
  IN CONST SFB_ADV_LUN  *Lun
  )
{
  BOOLEAN       ReadOnly = TRUE;
  BDS_USB_DISK  *Disk    = NULL;
  CHAR16        Target[48];
  CHAR16        Detail[96];
  EFI_STATUS    Status;

  UnicodeSPrint (Target, sizeof (Target), L"LUN %u", Lun->Lun);
  if (!SfbAdvChooseMountMode (Target, &ReadOnly)) {
    return;
  }

  Status = BdsUsbDiskCreatePassThrough (Lun->BlkIo, ReadOnly, &Disk);
  if (EFI_ERROR (Status)) {
    SfbReportStatus (L"Could not wrap LUN", Status);
    return;
  }

  UnicodeSPrint (
    Detail,
    sizeof (Detail),
    L"Whole LUN %u (%a)",
    Lun->Lun,
    ReadOnly ? "read-only" : "read-write"
    );
  SfbConfigStoreUnmount ();
  Status = BdsUsbMsdExport (
             &Disk->BlockIo,
             L"USB Mass Storage",
             Detail,
             Lun->Bytes
             );
  (VOID)SfbConfigStoreMount ();
  BdsUsbDiskDestroy (Disk);

  if (EFI_ERROR (Status) && (Status != EFI_ABORTED) &&
      (Status != EFI_MEDIA_CHANGED))
  {
    SfbReportStatus (L"Could not start mass storage", Status);
  }
}

/* ---- pickers -------------------------------------------------------------- */

/* Snapshot of the partitions on one LUN, resolved from the platform table. */
STATIC
UINTN
SfbAdvCollectPartitions (
  IN UINT32              Lun,
  OUT SFB_ADV_PARTITION  **Out
  )
{
  SFB_ADV_PARTITION    *Parts;
  UINTN                Count = 0;
  UINTN                PartitionIndex;
  EFI_PARTITION_ENTRY  *Entry;
  EFI_STATUS           Status;

  *Out = NULL;
  if ((Lun >= MAX_LUNS) || (Ptable[Lun].MaxHandles == 0)) {
    return 0;
  }

  Parts = AllocateZeroPool (Ptable[Lun].MaxHandles * sizeof (*Parts));
  if (Parts == NULL) {
    return 0;
  }

  for (PartitionIndex = 0; PartitionIndex < MIN (Ptable[Lun].MaxHandles, MAX_NUM_PARTITIONS); PartitionIndex++) {
    Entry  = NULL;
    Status = gBS->HandleProtocol (
                    Ptable[Lun].HandleInfoList[PartitionIndex].Handle,
                    &gEfiPartitionRecordGuid,
                    (VOID **)&Entry
                    );
    if (EFI_ERROR (Status) || (Entry == NULL)) {
      continue;
    }

    if ((Ptable[Lun].HandleInfoList[PartitionIndex].BlkIo == NULL) ||
        (Ptable[Lun].HandleInfoList[PartitionIndex].BlkIo->Media == NULL))
    {
      continue;
    }

    Parts[Count].Entry = Entry;
    Parts[Count].BlkIo = Ptable[Lun].HandleInfoList[PartitionIndex].BlkIo;
    Parts[Count].Lun   = Lun;
    Parts[Count].Bytes =
      (Parts[Count].BlkIo->Media->LastBlock + 1) *
      (UINT64)Parts[Count].BlkIo->Media->BlockSize;
    Count++;
  }

  if (Count == 0) {
    FreePool (Parts);
    return 0;
  }

  *Out = Parts;
  return Count;
}

/* Export-a-partition: LUN picker, then partition picker on that LUN. */
STATIC
VOID
SfbAdvExportPartitionFlow (
  VOID
  )
{
  SFB_ADV_LUN  Luns[SFB_ADV_MAX_LUNS];

  CHAR16 (*Rows)[SFB_ADV_ROW_CHARS];
  UINTN              LunCount;
  UINTN              Index;
  INTN               Chosen;
  UINT32             Lun;
  SFB_ADV_PARTITION  *Parts;
  UINTN              PartCount;

  SfbShowEnteringScreen (L"Export a partition");

  while (TRUE) {
    LunCount = SfbAdvCollectLuns (Luns);
    if (LunCount == 0) {
      SfbReportStatus (L"No storage LUNs found", EFI_NOT_FOUND);
      return;
    }

    Rows = AllocateZeroPool (LunCount * sizeof (*Rows));
    if (Rows == NULL) {
      return;
    }

    for (Index = 0; Index < LunCount; Index++) {
      UnicodeSPrint (
        Rows[Index],
        sizeof (Rows[Index]),
        L"LUN %u (%Lu MiB)",
        Luns[Index].Lun,
        Luns[Index].Bytes / (1024 * 1024)
        );
    }

    Chosen = SfbAdvChoose (
               L"Export a partition",
               L"Pick the LUN holding the partition.",
               Rows,
               LunCount,
               0
               );
    FreePool (Rows);
    if (Chosen < 0) {
      return;
    }

    Lun = Luns[Chosen].Lun;

    /* Partition picker on the chosen LUN. */
    while (TRUE) {
      SfbShowEnteringScreen (L"Partition Picker");

      PartCount = SfbAdvCollectPartitions (Lun, &Parts);
      if (PartCount == 0) {
        SfbReportStatus (L"No partitions on this LUN", EFI_NOT_FOUND);
        break;
      }

      Rows = AllocateZeroPool (PartCount * sizeof (*Rows));
      if (Rows == NULL) {
        FreePool (Parts);
        break;
      }

      for (Index = 0; Index < PartCount; Index++) {
        UnicodeSPrint (
          Rows[Index],
          sizeof (Rows[Index]),
          L"%.36s (%Lu MiB)",
          Parts[Index].Entry->PartitionName,
          Parts[Index].Bytes / (1024 * 1024)
          );
      }

      Chosen = SfbAdvChoose (
                 L"Export a partition",
                 L"Pick the partition to export.",
                 Rows,
                 PartCount,
                 0
                 );
      FreePool (Rows);
      if (Chosen < 0) {
        FreePool (Parts);
        break;
      }

      SfbAdvExportPartition (&Parts[Chosen]);
      FreePool (Parts);
      /* Loop: media state changed during the export; re-list. */
    }

    /* Loop: the LUN list is re-resolved too. */
  }
}

/* Export-a-LUN: LUN picker, then direct export. */
STATIC
VOID
SfbAdvExportLunFlow (
  VOID
  )
{
  SFB_ADV_LUN  Luns[SFB_ADV_MAX_LUNS];

  CHAR16 (*Rows)[SFB_ADV_ROW_CHARS];
  UINTN  LunCount;
  UINTN  Index;
  INTN   Chosen;

  SfbShowEnteringScreen (L"Export a LUN");

  while (TRUE) {
    LunCount = SfbAdvCollectLuns (Luns);
    if (LunCount == 0) {
      SfbReportStatus (L"No storage LUNs found", EFI_NOT_FOUND);
      return;
    }

    Rows = AllocateZeroPool (LunCount * sizeof (*Rows));
    if (Rows == NULL) {
      return;
    }

    for (Index = 0; Index < LunCount; Index++) {
      UnicodeSPrint (
        Rows[Index],
        sizeof (Rows[Index]),
        L"LUN %u (%Lu MiB)",
        Luns[Index].Lun,
        Luns[Index].Bytes / (1024 * 1024)
        );
    }

    Chosen = SfbAdvChoose (
               L"Export a LUN",
               L"Pick the LUN to export whole.",
               Rows,
               LunCount,
               0
               );
    FreePool (Rows);
    if (Chosen < 0) {
      return;
    }

    SfbAdvExportLun (&Luns[Chosen]);
  }
}

/* ---- config store export ------------------------------------------------ */

typedef struct {
  BOOLEAN    ReadOnly;
} SFB_ADV_CONFIG_EXPORT;

/*
 * Export the config container the way a partition is exported: behind a
 * synthesized one-partition GPT whose ESP-typed entry covers exactly the
 * container, so the host mounts it as an ordinary FAT partition.
 */
STATIC
EFI_STATUS
SfbAdvExportConfigSink (
  IN EFI_BLOCK_IO_PROTOCOL  *Container,
  IN VOID                   *Context
  )
{
  SFB_ADV_CONFIG_EXPORT  *Export = Context;
  BDS_USB_DISK           *Disk   = NULL;
  CHAR16                 Detail[96];
  UINT64                 Bytes;
  EFI_STATUS             Status;

  Bytes = (Container->Media->LastBlock + 1) *
          (UINT64)Container->Media->BlockSize;

  Status = BdsUsbDiskCreate (
             Container,
             0,
             Container->Media->LastBlock + 1,
             &gEfiPartTypeSystemPartGuid,
             SFB_CONFIG_PARTITION_NAME,
             Export->ReadOnly,
             &Disk
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  UnicodeSPrint (
    Detail,
    sizeof (Detail),
    L"Config store (%a)",
    Export->ReadOnly ? "read-only" : "read-write"
    );
  Status = BdsUsbMsdExport (
             &Disk->BlockIo,
             L"USB Mass Storage",
             Detail,
             Bytes
             );
  BdsUsbDiskDestroy (Disk);
  return Status;
}

STATIC
VOID
SfbAdvExportConfigFlow (
  VOID
  )
{
  SFB_ADV_CONFIG_EXPORT  Export;
  EFI_STATUS             Status;

  SfbShowEnteringScreen (L"Export Config Store");

  Export.ReadOnly = TRUE;
  if (!SfbAdvChooseMountMode (L"the config store", &Export.ReadOnly)) {
    return;
  }

  Status = SfbConfigStoreExportUsb (SfbAdvExportConfigSink, &Export);
  if (EFI_ERROR (Status) && (Status != EFI_ABORTED) &&
      (Status != EFI_MEDIA_CHANGED))
  {
    SfbReportStatus (L"Could not export the config store", Status);
  }

  // Certificates and MEMCFG are a boot-session snapshot. Configuration
  // changes on the host take effect next boot; no boot-entry file is consulted.
}

// BootOrder is edited as a UINT16 array. Missing or malformed Boot#### values
// remain visible so moving/removing one never drops unrelated membership.
STATIC
SFB_ADV_ROW *
BdsBootOrderRows (
  IN CONST UINT16  *Order,
  IN UINTN         Count
  )
{
  SFB_ADV_ROW                   *Rows;
  EFI_BOOT_MANAGER_LOAD_OPTION  Option;
  EFI_STATUS                    Status;
  CHAR16                        Name[9];
  UINTN                         Index;

  Rows = AllocateZeroPool (Count * sizeof (*Rows));
  if (Rows == NULL) {
    return NULL;
  }

  for (Index = 0; Index < Count; Index++) {
    UnicodeSPrint (Name, sizeof (Name), L"Boot%04X", Order[Index]);
    ZeroMem (&Option, sizeof (Option));
    Status = BdsReadBootOption (Order[Index], &Option);
    UnicodeSPrint (
      Rows[Index],
      sizeof (Rows[Index]),
      L"%s  %s",
      Name,
      EFI_ERROR (Status) ? L"[missing or invalid]" : Option.Description
      );
    if (!EFI_ERROR (Status)) {
      EfiBootManagerFreeLoadOption (&Option);
    }
  }

  return Rows;
}

STATIC
VOID
BdsMoveBootOrder (
  IN OUT UINT16  *Order,
  IN UINTN       Count,
  IN UINTN       Position
  )
{
  SFB_ADV_ROW  *Rows;
  SFB_ADV_ROW  Row;
  UINTN        Start;
  UINTN        Last;
  UINTN        Index;
  UINTN        Next;
  UINT16       Number;
  BOOLEAN      Moved;
  SFB_KEY      Key;
  EFI_STATUS   Status;

  Rows = BdsBootOrderRows (Order, Count);
  if (Rows == NULL) {
    SfbReportStatus (L"Out of memory", EFI_OUT_OF_RESOURCES);
    return;
  }

  Moved = FALSE;
  while (TRUE) {
    SfbBeginScreen (L"Boot Order", L"Moving the marked entry.");
    Start = SfbWindowStart (Position, Count, SFB_VISIBLE_ROWS);
    Last  = MIN (Start + SFB_VISIBLE_ROWS, Count);
    for (Index = Start; Index < Last; Index++) {
      SfbDrawRow ((BOOLEAN)(Index == Position), (Index == Position) ? L"=" : L" ", Rows[Index]);
    }

    SfbEndScreen (L"Vol Up/Down: move entry   Power: save");
    Key  = SfbWaitForKey (0);
    Next = Position;
    if ((Key == SfbKeyUp) && (Position > 0)) {
      Next--;
    } else if ((Key == SfbKeyDown) && (Position + 1 < Count)) {
      Next++;
    } else if (Key == SfbKeySelect) {
      if (Moved) {
        Status = BdsWriteBootOrder (Order, Count);
        if (EFI_ERROR (Status)) {
          SfbReportStatus (L"Could not save BootOrder", Status);
        }
      }

      break;
    } else if (Key == SfbKeyUnavailable) {
      break;
    }

    if (Next != Position) {
      Number          = Order[Position];
      Order[Position] = Order[Next];
      Order[Next]     = Number;
      CopyMem (Row, Rows[Position], sizeof (Row));
      CopyMem (Rows[Position], Rows[Next], sizeof (Row));
      CopyMem (Rows[Next], Row, sizeof (Row));
      Position = Next;
      Moved    = TRUE;
    }
  }

  FreePool (Rows);
  SfbDebounceMenuExit ();
}

STATIC
VOID
SfbRunPriorityMenu (
  VOID
  )
{
  STATIC CONST CHAR16  Actions[2][SFB_ADV_ROW_CHARS] = {
    L"Move in Boot Order",
    L"Remove from Boot Order (keep entry)"
  };
  SFB_ADV_ROW          *Rows;
  UINT16               *Order;
  UINTN                Count;
  UINTN                Cursor;
  INTN                 Chosen;
  INTN                 Action;
  EFI_STATUS           Status;

  SfbShowEnteringScreen (L"Boot Order");
  Cursor = 0;
  while (TRUE) {
    Status = BdsReadBootOrder (&Order, &Count);
    if (EFI_ERROR (Status)) {
      SfbReportStatus (L"Cannot read BootOrder", Status);
      return;
    }

    if (Count == 0) {
      if (Order != NULL) {
        FreePool (Order);
      }

      SfbReportStatus (L"BootOrder is empty; add entries in Boot Entries", EFI_NOT_FOUND);
      return;
    }

    Rows = BdsBootOrderRows (Order, Count);
    if (Rows == NULL) {
      FreePool (Order);
      SfbReportStatus (L"Out of memory", EFI_OUT_OF_RESOURCES);
      return;
    }

    Chosen = SfbAdvChoose (L"Boot Order", L"First entry is tried first.", Rows, Count, Cursor);
    if (Chosen < 0) {
      FreePool (Rows);
      FreePool (Order);
      return;
    }

    Cursor = (UINTN)Chosen;
    Action = SfbAdvChoose (L"Boot Order", Rows[Cursor], Actions, ARRAY_SIZE (Actions), 0);
    FreePool (Rows);
    if (Action == 0) {
      BdsMoveBootOrder (Order, Count, Cursor);
    } else if (Action == 1) {
      Status = BdsSetBootOrderMembership (Order[Cursor], FALSE);
      if (EFI_ERROR (Status)) {
        SfbReportStatus (L"Could not remove BootOrder membership", Status);
      }
    }

    FreePool (Order);
  }
}

STATIC
VOID
BdsBootEntryActions (
  IN EFI_BOOT_MANAGER_LOAD_OPTION  *Option
  )
{
  STATIC CONST CHAR16  Confirm[2][SFB_ADV_ROW_CHARS] = {
    L"Delete Boot#### entry",
    L"Keep entry"
  };
  SFB_ADV_ROW          Rows[5];
  UINT16               *Order;
  UINTN                Count;
  UINTN                Index;
  UINT16               Number;
  BOOLEAN              Included;
  BOOLEAN              Active;
  INTN                 Chosen;
  EFI_STATUS           Status;

  Status = BdsReadBootOrder (&Order, &Count);
  if (EFI_ERROR (Status)) {
    SfbReportStatus (L"Cannot read BootOrder", Status);
    return;
  }

  Number   = (UINT16)Option->OptionNumber;
  Included = FALSE;
  for (Index = 0; Index < Count; Index++) {
    if (Order[Index] == Number) {
      Included = TRUE;
      break;
    }
  }

  if (Order != NULL) {
    FreePool (Order);
  }

  Active = (BOOLEAN)((Option->Attributes & LOAD_OPTION_ACTIVE) != 0);
  StrCpyS (Rows[0], SFB_ADV_ROW_CHARS, Active ? L"Disable entry" : L"Enable entry");
  StrCpyS (Rows[1], SFB_ADV_ROW_CHARS, Included ? L"Remove from Boot Order (keep entry)" : L"Add to Boot Order");
  StrCpyS (Rows[2], SFB_ADV_ROW_CHARS, L"Set BootNext (next restart only)");
  StrCpyS (Rows[3], SFB_ADV_ROW_CHARS, L"Boot now");
  StrCpyS (Rows[4], SFB_ADV_ROW_CHARS, L"Delete entry >");
  Chosen = SfbAdvChoose (L"Boot Entry", Option->Description, Rows, ARRAY_SIZE (Rows), 0);
  switch (Chosen) {
    case 0:
      Status = BdsSetBootOptionActive (Number, (BOOLEAN) !Active);
      break;
    case 1:
      Status = BdsSetBootOrderMembership (Number, (BOOLEAN) !Included);
      break;
    case 2:
      Status = BdsSetBootNext (Number);
      break;
    case 3:
      SfbShowBootingScreen (Option->Description, TRUE);
      Status = BdsBootLoadOption (Option);
      if (Option->ExitData != NULL) {
        FreePool (Option->ExitData);
        Option->ExitData = NULL;
      }

      break;
    case 4:
      if (SfbAdvChoose (L"Delete Boot Entry", Option->Description, Confirm, 2, 1) != 0) {
        return;
      }

      Status = BdsDeleteBootOption (Number);
      break;
    default:
      return;
  }

  SfbReportStatus (EFI_ERROR (Status) ? L"Operation failed" : L"Operation completed", Status);
}

STATIC
VOID
SfbRunManagementMenu (
  VOID
  )
{
  EFI_BOOT_MANAGER_LOAD_OPTION  *Options;
  SFB_ADV_ROW                   *Rows;
  UINTN                         Count;
  UINTN                         Index;
  UINTN                         Cursor;
  INTN                          Chosen;
  EFI_STATUS                    Status;

  SfbShowEnteringScreen (L"Boot Entries");
  Cursor = 0;
  while (TRUE) {
    Status = BdsGetAllBootOptions (&Options, &Count);
    if (EFI_ERROR (Status)) {
      SfbReportStatus (L"Cannot enumerate Boot#### variables", Status);
      return;
    }

    Rows = AllocateZeroPool ((Count + 1) * sizeof (*Rows));
    if (Rows == NULL) {
      if (Options != NULL) {
        EfiBootManagerFreeLoadOptions (Options, Count);
      }

      SfbReportStatus (L"Out of memory", EFI_OUT_OF_RESOURCES);
      return;
    }

    for (Index = 0; Index < Count; Index++) {
      UnicodeSPrint (
        Rows[Index],
        sizeof (Rows[Index]),
        L"Boot%04x  %s%s",
        Options[Index].OptionNumber,
        Options[Index].Description,
        (Options[Index].Attributes & LOAD_OPTION_ACTIVE) ? L"" : L" [disabled]"
        );
    }

    StrCpyS (Rows[Count], SFB_ADV_ROW_CHARS, L"Create entry from file >");
    Chosen = SfbAdvChoose (
               L"Boot Entries",
               L"All valid Boot#### entries, including entries outside BootOrder.",
               Rows,
               Count + 1,
               Cursor
               );
    FreePool (Rows);
    if ((Chosen >= 0) && ((UINTN)Chosen < Count)) {
      Cursor = (UINTN)Chosen;
      BdsBootEntryActions (&Options[Cursor]);
    }

    if (Options != NULL) {
      EfiBootManagerFreeLoadOptions (Options, Count);
    }

    if (Chosen < 0) {
      return;
    }

    if ((UINTN)Chosen == Count) {
      BdsRunFileBrowser ();
    }
  }
}

/* ---- grouped volume browser (BIOS Setup > Volumes) -------------------------- */

/* TRUE for the efisp config container volume. */
STATIC
BOOLEAN
SfbAdvIsConfigVolume (
  IN EFI_HANDLE  Volume
  )
{
  EFI_HANDLE  Config = NULL;

  return (BOOLEAN)(!EFI_ERROR (SfbConfigStoreGetFs (NULL, &Config)) &&
                   Config == Volume);
}

/*
 * The whole-disk handle behind a volume: truncate the volume's device path
 * before its final node and locate the BlockIo handle of what remains.
 */
STATIC
EFI_HANDLE
SfbAdvParentDiskHandle (
  IN EFI_HANDLE  Volume
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *Dp = NULL;
  EFI_DEVICE_PATH_PROTOCOL  *Node;
  EFI_DEVICE_PATH_PROTOCOL  *Last = NULL;
  EFI_DEVICE_PATH_PROTOCOL  *Trunc;
  EFI_DEVICE_PATH_PROTOCOL  *Remaining;
  EFI_HANDLE                Parent = NULL;
  EFI_BLOCK_IO_PROTOCOL     *BlkIo = NULL;
  UINTN                     Len;

  Status = gBS->HandleProtocol (
                  Volume,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **)&Dp
                  );
  if (EFI_ERROR (Status) || (Dp == NULL)) {
    return NULL;
  }

  Node = Dp;
  while (!IsDevicePathEnd (Node)) {
    Last = Node;
    Node = NextDevicePathNode (Node);
  }

  if ((Last == NULL) || (Last == Dp)) {
    return NULL;
  }

  Len   = (UINTN)((UINT8 *)Last - (UINT8 *)Dp);
  Trunc = AllocatePool (Len + sizeof (EFI_DEVICE_PATH_PROTOCOL));
  if (Trunc == NULL) {
    return NULL;
  }

  CopyMem (Trunc, Dp, Len);
  SetDevicePathEndNode ((UINT8 *)Trunc + Len);

  Remaining = Trunc;
  Status    = gBS->LocateDevicePath (&gEfiBlockIoProtocolGuid, &Remaining, &Parent);
  FreePool (Trunc);
  if (EFI_ERROR (Status) || (Parent == NULL)) {
    return NULL;
  }

  if (EFI_ERROR (
        gBS->HandleProtocol (
               Parent,
               &gEfiBlockIoProtocolGuid,
               (VOID **)&BlkIo
               )
        ) ||
      (BlkIo == NULL) || (BlkIo->Media == NULL) ||
      BlkIo->Media->LogicalPartition)
  {
    return NULL;
  }

  return Parent;
}

/* UFS LUN number of a whole-disk handle, or -1 when it is not a UFS LUN. */
STATIC
INTN
SfbAdvLunOfDisk (
  IN EFI_HANDLE  Disk
  )
{
  UINT32  Lun;
  UINT32  MaxLuns = GetMaxLuns ();
  UINTN   PartitionIndex;

  for (Lun = 0; Lun < MaxLuns && Lun < MAX_LUNS; Lun++) {
    for (PartitionIndex = 0; PartitionIndex < MIN (Ptable[Lun].MaxHandles, MAX_NUM_PARTITIONS); PartitionIndex++) {
      if (Ptable[Lun].HandleInfoList[PartitionIndex].Handle == NULL) {
        continue;
      }

      if (SfbAdvParentDiskHandle (Ptable[Lun].HandleInfoList[PartitionIndex].Handle) == Disk) {
        return (INTN)Lun;
      }
    }
  }

  return -1;
}

#define SFB_ADV_GROUPS_MAX  16
/* Volume label of one SFS handle, or an empty string. */
STATIC
VOID
SfbAdvVolumeLabel (
  IN EFI_HANDLE  Volume,
  OUT CHAR16     *Label,
  IN UINTN       LabelChars
  )
{
  EFI_FILE_PROTOCOL  *Root = NULL;

  Label[0] = L'\0';
  if (!EFI_ERROR (SfbOpenVolumeRoot (Volume, &Root)) && (Root != NULL)) {
    SfbGetVolumeLabel (Root, Label, LabelChars);
    Root->Close (Root);
  }
}

/*
 * The grouped volume browser, as a two-level menu: level one picks a storage
 * group - "LUN x" per UFS LUN, "Disk <label>" per other real disk such as a
 * USB stick, "Config Store" for the efisp config container - and level two
 * lists that group's file systems. Selecting a volume browses its root through the
 * ordinary file browser.
 */
#define SFB_ADV_GROUP_VOLS_MAX  32

typedef struct {
  EFI_HANDLE    Key; /* parent disk handle; the volume itself when parentless */
  INTN          Lun; /* UFS LUN, or -1 */
  CHAR16        Title[SFB_ADV_ROW_CHARS];
} SFB_ADV_GROUP;

/*
 * Snapshot the volumes and their groups. Returns the volume count and fills
 * Groups/VolGroups: VolGroups[i] is the Groups[].Key the volume belongs to.
 * The config container belongs to one synthetic group (Key = NULL).
 * Volumes whose parent disk cannot be resolved each form their own group.
 */
STATIC
UINTN
SfbAdvCollectVolumes (
  OUT SFB_ADV_GROUP  *Groups,
  OUT UINTN          *GroupCount,
  IN UINTN           GroupsMax,
  OUT EFI_HANDLE     *Volumes,
  OUT EFI_HANDLE     *VolGroups,
  IN UINTN           VolMax,
  OUT BOOLEAN        *HaveConfig
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *All  = NULL;
  UINTN       Count = 0;
  UINTN       Index;
  UINTN       VolCount = 0;
  UINTN       G        = 0;
  UINTN       GroupIdx;
  UINTN       Move;

  *GroupCount = 0;
  *HaveConfig = FALSE;

  // Standard SFS enumeration includes USB media and the FAT container.
  Status = SfbLocateVolumes (&All, &Count);
  if (EFI_ERROR (Status) || (All == NULL)) {
    return 0;
  }

  for (Index = 0; Index < Count && VolCount < VolMax; Index++) {
    EFI_HANDLE  Key;
    INTN        Lun = -1;
    CHAR16      Label[SFB_DESC_CHARS];

    if (SfbAdvIsConfigVolume (All[Index])) {
      *HaveConfig = TRUE;
      Key         = NULL; /* the synthetic Config Store group */
    } else {
      EFI_HANDLE  Parent = SfbAdvParentDiskHandle (All[Index]);

      Key = (Parent != NULL) ? Parent : All[Index];
      if (Parent != NULL) {
        Lun = SfbAdvLunOfDisk (Parent);
      }
    }

    Volumes[VolCount]   = All[Index];
    VolGroups[VolCount] = Key;

    /* Find or create the group, UFS LUNs in ascending order first, other
     * keys after, the synthetic virtual group kept at the very end. */
    for (GroupIdx = 0; GroupIdx < G; GroupIdx++) {
      if (Groups[GroupIdx].Key == Key) {
        break;
      }
    }

    if (GroupIdx == G) {
      if (G >= GroupsMax) {
        continue;
      }

      if (Key == NULL) {
        GroupIdx = G;   /* virtual group appended last */
      } else if (Lun >= 0) {
        for (GroupIdx = 0; GroupIdx < G; GroupIdx++) {
          if ((Groups[GroupIdx].Key == NULL) ||
              ((Groups[GroupIdx].Lun >= 0) && (Groups[GroupIdx].Lun > Lun)))
          {
            break;
          }
        }

        for (Move = G; Move > GroupIdx; Move--) {
          Groups[Move] = Groups[Move - 1];
        }
      } else {
        GroupIdx = G;
      }

      Groups[GroupIdx].Key = Key;
      Groups[GroupIdx].Lun = Lun;
      SfbAdvVolumeLabel (All[Index], Label, SFB_DESC_CHARS);
      if (Key == NULL) {
        StrCpyS (Groups[GroupIdx].Title, SFB_ADV_ROW_CHARS, L"Config Store");
      } else if (Lun >= 0) {
        UnicodeSPrint (
          Groups[GroupIdx].Title,
          sizeof (Groups[GroupIdx].Title),
          L"LUN %d",
          (INT32)Lun
          );
      } else if (Label[0] != L'\0') {
        UnicodeSPrint (
          Groups[GroupIdx].Title,
          sizeof (Groups[GroupIdx].Title),
          L"Disk %s",
          Label
          );
      } else {
        StrCpyS (Groups[GroupIdx].Title, SFB_ADV_ROW_CHARS, L"Disk");
      }

      G++;
    }

    VolCount++;
  }

  FreePool (All);
  *GroupCount = G;
  return VolCount;
}

STATIC
VOID
SfbRunVolumesMenu (
  VOID
  )
{
  SFB_ADV_GROUP  Groups[SFB_ADV_GROUPS_MAX];
  UINTN          GroupCount = 0;
  EFI_HANDLE     Volumes[SFB_ADV_GROUP_VOLS_MAX];
  EFI_HANDLE     VolGroups[SFB_ADV_GROUP_VOLS_MAX];
  UINTN          VolCount;

  CHAR16 (*Labels)[SFB_ADV_ROW_CHARS];
  CHAR16 (*Rows)[SFB_ADV_ROW_CHARS];
  UINTN       Index;
  UINTN       Count;
  UINTN       Member;
  INTN        Chosen;
  INTN        ChosenVol;
  BOOLEAN     HaveConfig;
  EFI_HANDLE  Volume;
  CHAR16      ChosenLabel[SFB_ADV_ROW_CHARS];

  SfbShowEnteringScreen (L"Volumes");

  while (TRUE) {
    VolCount = SfbAdvCollectVolumes (
                 Groups,
                 &GroupCount,
                 SFB_ADV_GROUPS_MAX,
                 Volumes,
                 VolGroups,
                 SFB_ADV_GROUP_VOLS_MAX,
                 &HaveConfig
                 );
    if ((VolCount == 0) || (GroupCount == 0)) {
      SfbReportStatus (L"No file system volumes found", EFI_NOT_FOUND);
      return;
    }

    Labels = AllocateZeroPool (VolCount * sizeof (*Labels));
    if (Labels == NULL) {
      return;
    }

    for (Index = 0; Index < VolCount; Index++) {
      SfbAdvVolumeLabel (Volumes[Index], Labels[Index], SFB_ADV_ROW_CHARS);
      if (Labels[Index][0] == L'\0') {
        StrCpyS (Labels[Index], SFB_ADV_ROW_CHARS, L"Volume");
      }
    }

    /* Level one: the storage groups. */
    Rows = AllocateZeroPool (GroupCount * sizeof (*Rows));
    if (Rows == NULL) {
      FreePool (Labels);
      return;
    }

    for (Index = 0; Index < GroupCount; Index++) {
      StrCpyS (Rows[Index], SFB_ADV_ROW_CHARS, Groups[Index].Title);
    }

    Chosen = SfbAdvChoose (
               L"Volumes",
               L"Pick a storage group.",
               Rows,
               GroupCount,
               0
               );
    FreePool (Rows);
    if (Chosen < 0) {
      FreePool (Labels);
      return;
    }

    /* Level two: that group's volumes. */
    while (TRUE) {
      Count = 0;
      for (Index = 0; Index < VolCount; Index++) {
        if (VolGroups[Index] == Groups[Chosen].Key) {
          Count++;
        }
      }

      if (Count == 0) {
        SfbReportStatus (L"No volumes in this group", EFI_NOT_FOUND);
        break;
      }

      Rows = AllocateZeroPool (Count * sizeof (*Rows));
      if (Rows == NULL) {
        break;
      }

      Member = 0;
      for (Index = 0; Index < VolCount; Index++) {
        if (VolGroups[Index] == Groups[Chosen].Key) {
          StrCpyS (Rows[Member], SFB_ADV_ROW_CHARS, Labels[Index]);
          Member++;
        }
      }

      ChosenVol = SfbAdvChoose (
                    Groups[Chosen].Title,
                    L"Pick a volume to browse.",
                    Rows,
                    Count,
                    0
                    );
      FreePool (Rows);
      if (ChosenVol < 0) {
        break;   /* back to the group list */
      }

      Member         = 0;
      Volume         = NULL;
      ChosenLabel[0] = L'\0';
      for (Index = 0; Index < VolCount; Index++) {
        if (VolGroups[Index] == Groups[Chosen].Key) {
          if ((INTN)Member == ChosenVol) {
            Volume = Volumes[Index];
            StrCpyS (ChosenLabel, SFB_ADV_ROW_CHARS, Labels[Index]);
            break;
          }

          Member++;
        }
      }

      if ((Volume != NULL) &&
          BdsBrowseVolume (Volume, ChosenLabel, L"\\"))
      {
        /* The browser asked to unwind all the way out. */
        FreePool (Labels);
        SfbDebounceMenuExit ();
        return;
      }

      if (Volume == NULL) {
        break;
      }

      /* Media may have changed while browsing; rebuild this group's list. */
    }

    FreePool (Labels);
    /* Back to level one; groups are re-collected at the top of the loop. */
  }
}

STATIC
VOID
SfbRunMassStorageMenu (
  VOID
  )
{
  STATIC CONST CHAR16  Rows[3][SFB_ADV_ROW_CHARS] = {
    L"Export a partition >",
    L"Export a LUN >",
    L"Export Config Store >"
  };
  INTN                 Chosen;

  SfbShowEnteringScreen (L"USB Mass Storage");

  while (TRUE) {
    Chosen = SfbAdvChoose (
               L"USB Mass Storage",
               L"Expose device storage to the host over USB.",
               Rows,
               3,
               0
               );
    if (Chosen < 0) {
      return;
    }

    if (Chosen == 0) {
      SfbAdvExportPartitionFlow ();
    } else if (Chosen == 1) {
      SfbAdvExportLunFlow ();
    } else {
      SfbAdvExportConfigFlow ();
    }
  }
}

BOOLEAN
BdsRunSetup (
  VOID
  )
{
  STATIC CONST CHAR16  Rows[7][SFB_ADV_ROW_CHARS] = {
    L"Enter Fastboot",
    L"USB Mass Storage >",
    L"Volumes >",
    L"Boot Order >",
    L"Boot Entries >",
    L"File Browser >",
    L"Refresh Devices"
  };
  CONST CHAR16         *LockHint =
    L"Device locked: only fastboot authorization is available here.";
  INTN        Chosen;
  UINTN       RowCount;
  EFI_STATUS  Status;

  /* Locked mode exposes only the fastboot authorization interface; the rest
   * of the menu returns after a successful challenge response this boot. */

  SfbShowEnteringScreen (L"BIOS Setup");

  while (TRUE) {
    RowCount = SfbSecurityLocked () ? 1 : ARRAY_SIZE (Rows);
    Chosen   = SfbAdvChoose (
                 L"BIOS Setup",
                 RowCount == 1 ? LockHint : NULL,
                 Rows,
                 RowCount,
                 0
                 );
    switch (Chosen) {
      case 0:
        /* The caller announces fastboot mode and runs its loop. */
        return TRUE;
      case 1:
        SfbRunMassStorageMenu ();
        break;
      case 2:
        SfbRunVolumesMenu ();
        break;
      case 3:
        SfbRunPriorityMenu ();
        break;
      case 4:
        SfbRunManagementMenu ();
        break;
      case 5:
        BdsRunFileBrowser ();
        break;
      case 6:
        SfbConnectAll ();
        Status = BdsDiscoverEspBootOptions ();
        SfbReportStatus (L"Refresh Devices", Status);
        break;
      default:
        return FALSE;
    }
  }
}
