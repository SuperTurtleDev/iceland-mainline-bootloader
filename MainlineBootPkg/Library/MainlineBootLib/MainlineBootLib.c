/** @file MainlineBootLib.c

  ABL-style mainline Linux loader: Init / Load* / Boot.

  Reference behaviour copied from QcomModulePkg/Library/BootLib:
    Board.c        GetRamPartitions / BaseMem
    BootLinux.c    QueryBootParams / UpdateBootParams / LoadAddrAndDTUpdate /
                   BootLinux tail
  without the boot.img / partition / AVB / DeviceInfo machinery.

  SPDX-License-Identifier: BSD-3-Clause-Clear
**/

#include "MainlineBootInternal.h"
#include <Library/UefiLib.h>
#include <libfdt.h>
#include <Protocol/GraphicsOutput.h>

MLB_STATE  gMlb;

/* gQcomTokenSpaceGuid comes from QcomModulePkg.dec via AutoGen */

/* ------------------------------------------------------------------------ */
/* helpers                                                                   */
/* ------------------------------------------------------------------------ */

STATIC VOID
MlbFreeImages (
  VOID
  )
{
  if (gMlb.Kernel != NULL) {
    FreePool (gMlb.Kernel);
  }
  if (gMlb.Dtb != NULL) {
    FreePool (gMlb.Dtb);
  }
  if (gMlb.Initrd != NULL) {
    FreePool (gMlb.Initrd);
  }
  if (gMlb.CmdLine != NULL) {
    FreePool (gMlb.CmdLine);
  }
  gMlb.Kernel = NULL;
  gMlb.KernelSize = 0;
  ZeroMem (&gMlb.KernelHdr, sizeof (gMlb.KernelHdr));
  gMlb.Dtb = NULL;
  gMlb.DtbSize = 0;
  gMlb.Initrd = NULL;
  gMlb.InitrdSize = 0;
  gMlb.CmdLine = NULL;
  gMlb.CmdLineLoaded = FALSE;
}

STATIC VOID
MlbFreePlatform (
  VOID
  )
{
  if (gMlb.RamPartitions != NULL) {
    FreePool (gMlb.RamPartitions);
  }
  if (gMlb.PubRamPartitions != NULL) {
    FreePool (gMlb.PubRamPartitions);
  }
  gMlb.RamPartitions = NULL;
  gMlb.PubRamPartitions = NULL;
  gMlb.NumRamPartitions = 0;
  gMlb.BaseMemory = 0;
  gMlb.KernelBaseAddr = 0;
  gMlb.KernelSizeReserved = 0;
  gMlb.KernelParamsFromUefiVars = FALSE;
  gMlb.Initialized = FALSE;
}

/* Copy of Board.c GetRamPartitions(): size query, then full read. */
STATIC EFI_STATUS
MlbGetRamPartitions (
  OUT RamPartitionEntry  **RamPartitions,
  OUT UINT32              *NumPartitions
  )
{
  EFI_STATUS                  Status;
  EFI_RAMPARTITION_PROTOCOL  *RamPartProt = NULL;
  UINT32                      Num = 0;

  *RamPartitions = NULL;
  *NumPartitions = 0;

  Status = gBS->LocateProtocol (&gEfiRamPartitionProtocolGuid, NULL,
                                (VOID **)&RamPartProt);
  if (EFI_ERROR (Status) || RamPartProt == NULL) {
    DEBUG ((EFI_D_ERROR,
            "MainlineBoot: Locate EFI_RAMPARTITION_PROTOCOL failed: %r\n",
            Status));
    return EFI_NOT_FOUND;
  }

  Status = RamPartProt->GetRamPartitions (RamPartProt, NULL, &Num);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    DEBUG ((EFI_D_ERROR,
            "MainlineBoot: GetRamPartitions size query returned %r\n", Status));
    return EFI_PROTOCOL_ERROR;
  }
  if (Num == 0) {
    return EFI_NOT_FOUND;
  }

  *RamPartitions = AllocateZeroPool (Num * sizeof (RamPartitionEntry));
  if (*RamPartitions == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = RamPartProt->GetRamPartitions (RamPartProt, *RamPartitions, &Num);
  if (EFI_ERROR (Status) || Num < 1) {
    DEBUG ((EFI_D_ERROR, "MainlineBoot: Failed to get RAM partitions: %r\n",
            Status));
    FreePool (*RamPartitions);
    *RamPartitions = NULL;
    return EFI_NOT_FOUND;
  }

  *NumPartitions = Num;
  return EFI_SUCCESS;
}

/* Copy of BootLinux.c QueryBootParams(). */
STATIC BOOLEAN
MlbQueryBootParams (
  OUT UINT64  *KernelLoadAddr,
  OUT UINT64  *KernelSizeReserved
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  SizeStatus;
  UINTN       DataSize;

  DataSize = sizeof (*KernelLoadAddr);
  Status = gRT->GetVariable ((CHAR16 *)L"KernelBaseAddr", &gQcomTokenSpaceGuid,
                             NULL, &DataSize, KernelLoadAddr);

  DataSize = sizeof (*KernelSizeReserved);
  SizeStatus = gRT->GetVariable ((CHAR16 *)L"KernelSize", &gQcomTokenSpaceGuid,
                                 NULL, &DataSize, KernelSizeReserved);

  return (Status == EFI_SUCCESS && SizeStatus == EFI_SUCCESS);
}

/* Replace a library-owned buffer with a copy of the caller's data. */
STATIC EFI_STATUS
MlbStoreCopy (
  IN OUT VOID   **Dest,
  IN OUT UINTN   *DestSize,
  IN     VOID    *Src,
  IN     UINTN    Size
  )
{
  VOID  *New;

  New = AllocatePool (Size);
  if (New == NULL) {
    DEBUG ((EFI_D_ERROR, "MainlineBoot: out of memory (%lu bytes)\n",
            (UINT64)Size));
    return EFI_OUT_OF_RESOURCES;
  }
  CopyMem (New, Src, Size);

  if (*Dest != NULL) {
    FreePool (*Dest);
  }
  *Dest = New;
  *DestSize = Size;
  return EFI_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* MainlineBootInit                                                          */
/* ------------------------------------------------------------------------ */

EFI_STATUS
EFIAPI
MainlineBootInit (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      i;
  UINT64      KernelBase = 0;
  UINT64      KernelSize = 0;

  MlbFreeImages ();
  MlbFreePlatform ();

  /* --- RAM partitions (what ABL calls "from TZ") --- */
  Status = MlbGetRamPartitions (&gMlb.RamPartitions, &gMlb.NumRamPartitions);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  gMlb.PubRamPartitions = AllocateZeroPool (gMlb.NumRamPartitions *
                                            sizeof (MAINLINE_BOOT_RAM_PARTITION));
  if (gMlb.PubRamPartitions == NULL) {
    MlbFreePlatform ();
    return EFI_OUT_OF_RESOURCES;
  }

  /* --- BaseMemory = smallest Base (Board.c BaseMem) --- */
  gMlb.BaseMemory = gMlb.RamPartitions[0].Base;
  DEBUG ((EFI_D_INFO, "MainlineBoot: RAM partitions (%u):\n",
          gMlb.NumRamPartitions));
  for (i = 0; i < gMlb.NumRamPartitions; i++) {
    DEBUG ((EFI_D_INFO, "  [%2u] Base 0x%016lx Length 0x%016lx\n", i,
            gMlb.RamPartitions[i].Base, gMlb.RamPartitions[i].AvailableLength));
    if (gMlb.RamPartitions[i].Base < gMlb.BaseMemory) {
      gMlb.BaseMemory = gMlb.RamPartitions[i].Base;
    }
    gMlb.PubRamPartitions[i].Base   = gMlb.RamPartitions[i].Base;
    gMlb.PubRamPartitions[i].Length = gMlb.RamPartitions[i].AvailableLength;
  }
  DEBUG ((EFI_D_INFO, "MainlineBoot: Memory Base Address: 0x%lx\n",
          gMlb.BaseMemory));

  /* --- kernel region (BootLinux.c QueryBootParams / fallback) --- */
  if (MlbQueryBootParams (&KernelBase, &KernelSize)) {
    gMlb.KernelBaseAddr = KernelBase;
    gMlb.KernelSizeReserved = KernelSize;
    gMlb.KernelParamsFromUefiVars = TRUE;
    DEBUG ((EFI_D_INFO,
            "MainlineBoot: KernelBaseAddr 0x%lx KernelSize 0x%lx (UEFI vars)\n",
            KernelBase, KernelSize));
  } else {
    gMlb.KernelBaseAddr = gMlb.BaseMemory;
    gMlb.KernelSizeReserved = MLB_FALLBACK_KERNEL_SIZE;
    gMlb.KernelParamsFromUefiVars = FALSE;
    DEBUG ((EFI_D_WARN,
            "MainlineBoot: WARNING: KernelBaseAddr/KernelSize variables not "
            "found, falling back to BaseMemory 0x%lx / 0x%x (ABL default)\n",
            gMlb.KernelBaseAddr, MLB_FALLBACK_KERNEL_SIZE));
  }

  gMlb.Initialized = TRUE;
  return EFI_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* Load*                                                                     */
/* ------------------------------------------------------------------------ */

EFI_STATUS
EFIAPI
MainlineBootLoadKernel (
  IN VOID   *Buffer,
  IN UINTN  Size
  )
{
  EFI_STATUS           Status;
  MLB_KERNEL_HDR_INFO  Hdr;

  if (Buffer == NULL || Size == 0) {
    return EFI_INVALID_PARAMETER;
  }

  Status = MlbCheckKernelHeader (Buffer, Size, &Hdr);
  if (EFI_ERROR (Status)) {
    if (Hdr.IsGzip) {
      DEBUG ((EFI_D_ERROR,
              "MainlineBoot: kernel is gzip compressed (Image.gz); only "
              "uncompressed arm64 'Image' is supported\n"));
    } else {
      DEBUG ((EFI_D_ERROR,
              "MainlineBoot: not an arm64 Image (size %lu, bad magic at "
              "offset 56 or big-endian flag)\n", (UINT64)Size));
    }
    return Status;
  }

  Status = MlbStoreCopy (&gMlb.Kernel, &gMlb.KernelSize, Buffer, Size);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  gMlb.KernelHdr = Hdr;

  DEBUG ((EFI_D_INFO,
          "MainlineBoot: kernel loaded: %lu bytes, text_offset 0x%lx, "
          "image_size 0x%lx, flags 0x%lx%a\n",
          (UINT64)Size, Hdr.TextOffset, Hdr.ImageSize, Hdr.Flags,
          Hdr.ImageSize == 0 ? " (legacy: image_size 0, using 0x80000 offset)"
                             : ""));
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
MainlineBootLoadDtb (
  IN VOID   *Buffer,
  IN UINTN  Size
  )
{
  EFI_STATUS  Status;
  INT32       Ret;
  UINT32      TotalSize;

  if (Buffer == NULL || Size < sizeof (struct fdt_header)) {
    return EFI_INVALID_PARAMETER;
  }

  Ret = fdt_check_header (Buffer);
  if (Ret != 0) {
    DEBUG ((EFI_D_ERROR, "MainlineBoot: invalid DTB header (%a)\n",
            fdt_strerror (Ret)));
    return EFI_INVALID_PARAMETER;
  }

  TotalSize = fdt_totalsize (Buffer);
  if (TotalSize > Size) {
    DEBUG ((EFI_D_ERROR,
            "MainlineBoot: DTB totalsize %u > buffer %lu\n", TotalSize,
            (UINT64)Size));
    return EFI_INVALID_PARAMETER;
  }
  if (TotalSize > MLB_DT_SIZE_2MB - MLB_DT_HEADROOM) {
    DEBUG ((EFI_D_ERROR,
            "MainlineBoot: DTB too large (%u), limit is %u bytes\n", TotalSize,
            MLB_DT_SIZE_2MB - MLB_DT_HEADROOM));
    return EFI_BAD_BUFFER_SIZE;
  }

  Status = MlbStoreCopy (&gMlb.Dtb, &gMlb.DtbSize, Buffer, TotalSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((EFI_D_INFO, "MainlineBoot: DTB loaded: %u bytes (buffer %lu)\n",
          TotalSize, (UINT64)Size));
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
MainlineBootLoadInitrd (
  IN VOID   *Buffer,
  IN UINTN  Size
  )
{
  EFI_STATUS       Status;
  MLB_INITRD_KIND  Kind;
  CONST CHAR8     *KindName = "?";

  if (Size == 0) {
    /* explicit "no initrd" */
    if (gMlb.Initrd != NULL) {
      FreePool (gMlb.Initrd);
    }
    gMlb.Initrd = NULL;
    gMlb.InitrdSize = 0;
    DEBUG ((EFI_D_INFO, "MainlineBoot: initrd cleared\n"));
    return EFI_SUCCESS;
  }
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Kind = MlbDetectInitrdKind (Buffer, Size);
  switch (Kind) {
  case MlbInitrdCpioNewc: KindName = "cpio newc/crc"; break;
  case MlbInitrdCpioOdc:  KindName = "cpio odc";      break;
  case MlbInitrdGzip:     KindName = "gzip";          break;
  case MlbInitrdZstd:     KindName = "zstd";          break;
  case MlbInitrdXz:       KindName = "xz";            break;
  case MlbInitrdLz4:      KindName = "lz4";           break;
  case MlbInitrdBzip2:    KindName = "bzip2";         break;
  case MlbInitrdLzma:     KindName = "lzma";          break;
  case MlbInitrdLzo:      KindName = "lzo";           break;
  default:
    DEBUG ((EFI_D_ERROR,
            "MainlineBoot: initrd: unrecognised format (first bytes "
            "%02x %02x %02x %02x %02x %02x)\n",
            ((UINT8 *)Buffer)[0], ((UINT8 *)Buffer)[1], ((UINT8 *)Buffer)[2],
            ((UINT8 *)Buffer)[3], ((UINT8 *)Buffer)[4], ((UINT8 *)Buffer)[5]));
    return EFI_UNSUPPORTED;
  }

  if (Kind != MlbInitrdCpioNewc && Kind != MlbInitrdCpioOdc) {
    DEBUG ((EFI_D_WARN,
            "MainlineBoot: WARNING: initrd is %a compressed; the kernel must "
            "have the matching decompressor built in\n", KindName));
  }

  Status = MlbStoreCopy (&gMlb.Initrd, &gMlb.InitrdSize, Buffer, Size);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((EFI_D_INFO, "MainlineBoot: initrd loaded: %lu bytes (%a)\n",
          (UINT64)Size, KindName));
  return EFI_SUCCESS;
}

STATIC VOID
MlbBootcfgOtherKey (
  IN CONST CHAR8  *Key,
  IN UINTN         KeyLen,
  IN CONST CHAR8  *Value,
  IN UINTN         ValueLen,
  IN VOID         *Context
  )
{
  CHAR8  K[32];
  UINTN  N = KeyLen < sizeof (K) - 1 ? KeyLen : sizeof (K) - 1;

  CopyMem (K, Key, N);
  K[N] = '\0';
  DEBUG ((EFI_D_INFO, "MainlineBoot: bootcfg: ignoring key '%a' (%lu byte value)\n",
          K, (UINT64)ValueLen));
}

EFI_STATUS
EFIAPI
MainlineBootLoadBootcfg (
  IN VOID   *Buffer,
  IN UINTN  Size
  )
{
  EFI_STATUS  Status;
  CHAR8      *CmdLine;

  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  CmdLine = AllocateZeroPool (MLB_MAX_CMDLINE_LEN);
  if (CmdLine == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = MlbParseBootcfg ((CONST CHAR8 *)Buffer, Size, CmdLine,
                            MLB_MAX_CMDLINE_LEN, MlbBootcfgOtherKey, NULL);
  if (Status == EFI_NOT_FOUND) {
    DEBUG ((EFI_D_ERROR, "MainlineBoot: bootcfg has no 'cmdline=' line\n"));
    FreePool (CmdLine);
    return EFI_NOT_FOUND;
  }
  if (Status == EFI_BUFFER_TOO_SMALL) {
    DEBUG ((EFI_D_WARN,
            "MainlineBoot: WARNING: cmdline truncated to %u chars\n",
            MLB_MAX_CMDLINE_LEN - 1));
    Status = EFI_SUCCESS;
  }
  if (EFI_ERROR (Status)) {
    FreePool (CmdLine);
    return Status;
  }

  if (gMlb.CmdLine != NULL) {
    FreePool (gMlb.CmdLine);
  }
  gMlb.CmdLine = CmdLine;
  gMlb.CmdLineLoaded = TRUE;

  DEBUG ((EFI_D_INFO, "MainlineBoot: cmdline (%u chars): \"%a\"\n",
          (UINT32)AsciiStrLen (CmdLine), CmdLine));
  if (CmdLine[0] == '\0') {
    DEBUG ((EFI_D_INFO,
            "MainlineBoot: empty cmdline -> existing /chosen/bootargs kept\n"));
  }
  return EFI_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* Queries                                                                   */
/* ------------------------------------------------------------------------ */

EFI_STATUS
EFIAPI
MainlineBootGetInfo (
  OUT MAINLINE_BOOT_INFO  *Info
  )
{
  if (Info == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  ZeroMem (Info, sizeof (*Info));

  Info->Initialized              = gMlb.Initialized;
  Info->BaseMemory               = gMlb.BaseMemory;
  Info->KernelBaseAddr           = gMlb.KernelBaseAddr;
  Info->KernelSizeReserved       = gMlb.KernelSizeReserved;
  Info->KernelParamsFromUefiVars = gMlb.KernelParamsFromUefiVars;

  Info->KernelLoaded     = (gMlb.Kernel != NULL);
  Info->KernelFileSize   = gMlb.KernelSize;
  Info->KernelTextOffset = gMlb.KernelHdr.TextOffset;
  Info->KernelImageSize  = gMlb.KernelHdr.ImageSize;
  Info->KernelFlags      = gMlb.KernelHdr.Flags;

  Info->DtbLoaded  = (gMlb.Dtb != NULL);
  Info->DtbSize    = gMlb.DtbSize;

  Info->InitrdLoaded = (gMlb.Initrd != NULL);
  Info->InitrdSize   = gMlb.InitrdSize;

  Info->CmdLineLoaded = gMlb.CmdLineLoaded;
  Info->CmdLine       = gMlb.CmdLine;

  Info->NumRamPartitions = gMlb.NumRamPartitions;
  Info->RamPartitions    = gMlb.PubRamPartitions;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
MlbLayout (
  OUT MLB_LAYOUT_OUT  *Out
  )
{
  MLB_LAYOUT_IN  In;

  if (!gMlb.Initialized || gMlb.Kernel == NULL) {
    return EFI_NOT_READY;
  }
  In.KernelBaseAddr     = gMlb.KernelBaseAddr;
  In.KernelSizeReserved = gMlb.KernelSizeReserved;
  In.TextOffset         = gMlb.KernelHdr.TextOffset;
  In.ImageSize          = gMlb.KernelHdr.ImageSize;
  In.KernelFileSize     = gMlb.KernelSize;
  In.InitrdSize         = gMlb.InitrdSize;
  return MlbComputeLayout (&In, Out);
}

EFI_STATUS
EFIAPI
MainlineBootGetLayout (
  OUT UINT64  *KernelLoadAddr,
  OUT UINT64  *KernelEndAddr,
  OUT UINT64  *RamdiskLoadAddr,
  OUT UINT64  *DeviceTreeLoadAddr
  )
{
  EFI_STATUS      Status;
  MLB_LAYOUT_OUT  L;

  Status = MlbLayout (&L);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (KernelLoadAddr)     *KernelLoadAddr     = L.KernelLoadAddr;
  if (KernelEndAddr)      *KernelEndAddr      = L.KernelEndAddr;
  if (RamdiskLoadAddr)    *RamdiskLoadAddr    = L.RamdiskLoadAddr;
  if (DeviceTreeLoadAddr) *DeviceTreeLoadAddr = L.DeviceTreeLoadAddr;
  return EFI_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* MainlineBootBoot                                                          */
/* ------------------------------------------------------------------------ */

/**
  Sanity check of the kernel window [KernelBaseAddr, KernelEndAddr) before
  anything is copied into it. ABL does not do this (it trusts the UEFI
  variables), but it protects against the ABL fallback (BaseMemory +
  0x05600000) on SoCs where the bottom of DDR is hypervisor / TZ memory:
  writing there faults or is silently blocked by stage-2.

    - not inside any TZ RAM partition          -> error (always)
    - overlaps a /reserved-memory no-map region -> error when the window came
                                                  from the fallback, warning
                                                  when it came from UEFI vars
**/
STATIC EFI_STATUS
MlbCheckKernelWindow (
  IN CONST MLB_LAYOUT_OUT  *L
  )
{
  MLB_MEM_REGION  Parts[MLB_MAX_MEM_REGIONS];
  MLB_MEM_REGION  NoMap[MLB_MAX_MEM_REGIONS];
  MLB_MEM_REGION  Hit;
  UINT32          i, NumNoMap = 0;
  UINT64          Base = gMlb.KernelBaseAddr;
  UINT64          Size = L->KernelEndAddr - gMlb.KernelBaseAddr;
  EFI_STATUS      Status;

  if (gMlb.NumRamPartitions > MLB_MAX_MEM_REGIONS) {
    return EFI_UNSUPPORTED;
  }
  for (i = 0; i < gMlb.NumRamPartitions; i++) {
    Parts[i].Base = gMlb.RamPartitions[i].Base;
    Parts[i].Size = gMlb.RamPartitions[i].AvailableLength;
  }

  if (!MlbRegionContainedIn (Parts, gMlb.NumRamPartitions, Base, Size)) {
    DEBUG ((EFI_D_ERROR,
            "MainlineBoot: ERROR: kernel window 0x%lx-0x%lx is not inside any "
            "TZ RAM partition, refusing to boot\n", Base, Base + Size));
    return EFI_UNSUPPORTED;
  }

  Status = MlbFdtGetNoMapRegions (gMlb.Dtb, NoMap, MLB_MAX_MEM_REGIONS, &NumNoMap);
  if (EFI_ERROR (Status)) {
    return EFI_SUCCESS;                      /* no /reserved-memory: nothing to check */
  }
  NumNoMap = MlbSortAndCombineRegions (NoMap, NumNoMap);

  if (MlbRegionsOverlap (NoMap, NumNoMap, Base, Size, &Hit)) {
    if (!gMlb.KernelParamsFromUefiVars) {
      DEBUG ((EFI_D_ERROR,
              "MainlineBoot: ERROR: fallback kernel window 0x%lx-0x%lx overlaps "
              "DT no-map region 0x%lx-0x%lx (firmware memory); KernelBaseAddr/"
              "KernelSize UEFI variables are required on this platform\n",
              Base, Base + Size, Hit.Base, Hit.Base + Hit.Size));
      return EFI_UNSUPPORTED;
    }
    DEBUG ((EFI_D_WARN,
            "MainlineBoot: WARNING: kernel window 0x%lx-0x%lx (UEFI vars) "
            "overlaps DT no-map region 0x%lx-0x%lx; continuing like ABL\n",
            Base, Base + Size, Hit.Base, Hit.Base + Hit.Size));
  }
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
MainlineBootBoot (
  VOID
  )
{
  EFI_STATUS      Status;
  MLB_LAYOUT_OUT  L;
  UINT64          InitrdAddr = 0;

  if (!gMlb.Initialized) {
    DEBUG ((EFI_D_ERROR, "MainlineBoot: MainlineBootInit() not done\n"));
    return EFI_NOT_READY;
  }
  if (gMlb.Kernel == NULL || gMlb.Dtb == NULL) {
    DEBUG ((EFI_D_ERROR, "MainlineBoot: kernel%a and dtb%a must be loaded\n",
            gMlb.Kernel ? "" : " (missing)", gMlb.Dtb ? "" : " (missing)"));
    return EFI_NOT_READY;
  }

  /* --- address layout (ABL UpdateBootParams) --- */
  Status = MlbLayout (&L);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR,
            "MainlineBoot: layout failed (%r): kernel region 0x%lx+0x%lx "
            "cannot hold Image (%lu bytes, image_size 0x%lx) + initrd "
            "(%lu bytes) + 2MB DT\n",
            Status, gMlb.KernelBaseAddr, gMlb.KernelSizeReserved,
            (UINT64)gMlb.KernelSize, gMlb.KernelHdr.ImageSize,
            (UINT64)gMlb.InitrdSize));
    return Status;
  }

  if ((L.KernelLoadAddr - gMlb.KernelHdr.TextOffset) & (SIZE_2MB - 1)) {
    DEBUG ((EFI_D_WARN,
            "MainlineBoot: WARNING: kernel base 0x%lx is not 2MB aligned "
            "(arm64 boot protocol requires text_offset from a 2MB aligned base)\n",
            L.KernelLoadAddr - gMlb.KernelHdr.TextOffset));
  }

  DEBUG ((EFI_D_INFO, "MainlineBoot: layout:\n"));
  DEBUG ((EFI_D_INFO, "  KernelBaseAddr      0x%016lx\n", gMlb.KernelBaseAddr));
  DEBUG ((EFI_D_INFO, "  KernelLoadAddr      0x%016lx (+0x%lx text_offset, %lu bytes, image_size 0x%lx)\n",
          L.KernelLoadAddr, L.KernelLoadAddr - gMlb.KernelBaseAddr,
          (UINT64)gMlb.KernelSize, gMlb.KernelHdr.ImageSize));
  DEBUG ((EFI_D_INFO, "  DeviceTreeLoadAddr  0x%016lx (%lu bytes, 2MB window)\n",
          L.DeviceTreeLoadAddr, (UINT64)gMlb.DtbSize));
  DEBUG ((EFI_D_INFO, "  RamdiskLoadAddr     0x%016lx (%lu bytes)\n",
          L.RamdiskLoadAddr, (UINT64)gMlb.InitrdSize));
  DEBUG ((EFI_D_INFO, "  KernelEndAddr       0x%016lx\n", L.KernelEndAddr));

  Status = MlbCheckKernelWindow (&L);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* --- 1. copy images into place (ABL LoadAddrAndDTUpdate / BootLinux) --- */
  DEBUG ((EFI_D_INFO, "MainlineBoot: copying kernel...\n"));
  gBS->CopyMem ((VOID *)(UINTN)L.KernelLoadAddr, gMlb.Kernel, gMlb.KernelSize);

  if (gMlb.Initrd != NULL && gMlb.InitrdSize != 0) {
    DEBUG ((EFI_D_INFO, "MainlineBoot: copying initrd...\n"));
    gBS->CopyMem ((VOID *)(UINTN)L.RamdiskLoadAddr, gMlb.Initrd, gMlb.InitrdSize);
    InitrdAddr = L.RamdiskLoadAddr;
  }

  DEBUG ((EFI_D_INFO, "MainlineBoot: copying dtb...\n"));
  gBS->CopyMem ((VOID *)(UINTN)L.DeviceTreeLoadAddr, gMlb.Dtb, gMlb.DtbSize);

  /* --- 2..4. update the device tree in place --- */
  Status = MlbUpdateDeviceTree ((VOID *)(UINTN)L.DeviceTreeLoadAddr,
                                MLB_DT_SIZE_2MB,
                                gMlb.CmdLineLoaded ? gMlb.CmdLine : NULL,
                                InitrdAddr, gMlb.InitrdSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "MainlineBoot: device tree update failed: %r\n",
            Status));
    return Status;
  }
  Print (L"[BOOTAPP] dtb updated: kernel %lu, initrd %lu, cmdline %a\r\n",
         (UINT64)gMlb.KernelSize, (UINT64)gMlb.InitrdSize,
         gMlb.CmdLineLoaded ? "yes" : "no");

  /* --- 5. turn off display before kernel handoff ---
   * The panel must not be left in ABL's state during the kernel transition.
   * msm DRM initializes it from scratch after boot. This eliminates the
   * white-flash / panel-abnormal-state sequence. */
  {
    EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop = NULL;

    if (gST != NULL && gST->ConsoleOutHandle != NULL) {
      gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid,
                           (VOID **)&Gop);
    }
    if (Gop == NULL) {
      gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
    }
    if (Gop != NULL && Gop->Mode != NULL && Gop->Mode->Info != NULL) {
      /* Black out the framebuffer — panel shows nothing during kernel boot */
      gBS->SetMem ((VOID *)(UINTN)Gop->Mode->FrameBufferBase,
                   (UINTN)Gop->Mode->FrameBufferSize, 0);
      Gop->Blt (Gop, &(EFI_GRAPHICS_OUTPUT_BLT_PIXEL){0, 0, 0, 0},
                EfiBltVideoFill, 0, 0, 0, 0,
                Gop->Mode->Info->HorizontalResolution,
                Gop->Mode->Info->VerticalResolution, 0);
      DEBUG ((EFI_D_INFO, "MainlineBoot: display blacked out for kernel handoff\n"));
    }
  }

  /* --- 6. final addresses, ExitBootServices, hardware quiesce --- */
  DEBUG ((EFI_D_INFO,
          "MainlineBoot: jumping to kernel: entry 0x%lx, x0 (dtb) 0x%lx%a\n",
          L.KernelLoadAddr, L.DeviceTreeLoadAddr,
          InitrdAddr ? "" : " (no initrd)"));
  DEBUG ((EFI_D_INFO, "MainlineBoot: shutting down UEFI boot services\n"));
  Print (L"[BOOTAPP] entry 0x%lx, dtb 0x%lx, initrd 0x%lx\r\n"
         L"[BOOTAPP] shutting down boot services...\r\n",
         L.KernelLoadAddr, L.DeviceTreeLoadAddr, InitrdAddr);

  Status = MlbShutdownUefiBootServices ();
  if (EFI_ERROR (Status)) {
    /* Boot services may still be alive here (ExitBootServices failed). */
    DEBUG ((EFI_D_ERROR,
            "MainlineBoot: ERROR: ExitBootServices failed: %r\n", Status));
    return Status;
  }

  /* ===== No boot services / DEBUG from here on ===== */
  MlbPreparePlatformHardware ();

  /* --- 6. jump --- */
  MlbJumpToKernel (L.KernelLoadAddr, L.DeviceTreeLoadAddr);

  /* not reached */
  CpuDeadLoop ();
  return EFI_NOT_STARTED;
}
