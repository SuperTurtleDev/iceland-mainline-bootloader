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
#include <libfdt.h>

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
  /* Platform default = what Init decided (UEFI vars, else ABL fallback).
   * A bootcfg kernel-base=/kernel-size= override replaces the effective
   * window for this boot; every LoadBootcfg first resets to these. */
  gMlb.PlatKernelBaseAddr = gMlb.KernelBaseAddr;
  gMlb.PlatKernelSizeReserved = gMlb.KernelSizeReserved;
  gMlb.KernelParamsFromCfg = FALSE;

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

/* Parse a bootcfg numeric value: 0x-prefixed hex, decimal, with optional
 * k/K (1024), m/M (MiB), g/G (GiB) suffix, e.g. "0x80000000", "768M". */
STATIC BOOLEAN
MlbParseU64Value (
  IN  CONST CHAR8  *Val,
  IN  UINTN        ValLen,
  OUT UINT64       *Out
  )
{
  CHAR8  V[24];
  UINTN  N = ValLen;
  UINT64 Mult = 1;
  UINT64 Num;

  if (N == 0 || N >= sizeof (V)) {
    return FALSE;
  }
  if (Val[N - 1] == 'k' || Val[N - 1] == 'K') { Mult = 1024ULL;              N--; }
  else if (Val[N - 1] == 'm' || Val[N - 1] == 'M') { Mult = 1024ULL * 1024;      N--; }
  else if (Val[N - 1] == 'g' || Val[N - 1] == 'G') { Mult = 1024ULL * 1024 * 1024; N--; }
  if (N == 0) {
    return FALSE;
  }
  CopyMem (V, Val, N);
  V[N] = '\0';
  if (N > 2 && V[0] == '0' && (V[1] == 'x' || V[1] == 'X')) {
    Num = AsciiStrHexToUint64 (V);
  } else {
    Num = AsciiStrDecimalToUint64 (V);
  }
  /* both parsers return 0 on garbage */
  if (Num == 0) {
    return FALSE;
  }
  *Out = Num * Mult;
  return TRUE;
}

/* bootcfg "other" keys: kernel-base= / kernel-size= override the ABL boot
 * window for this boot (priority: bootcfg > UEFI vars > ABL fallback). */
STATIC VOID
MlbBootcfgOtherKey (
  IN CONST CHAR8  *Key,
  IN UINTN         KeyLen,
  IN CONST CHAR8  *Value,
  IN UINTN         ValueLen,
  IN VOID         *Context
  )
{
  UINT64  Num;

  if (KeyLen == 11 && MlbMemCmp (Key, "kernel-base", 11) == 0) {
    if (!MlbParseU64Value (Value, ValueLen, &Num)) {
      DEBUG ((EFI_D_ERROR,
              "MainlineBoot: bootcfg: bad kernel-base value (ignored)\n"));
      return;
    }
    gMlb.KernelBaseAddr = Num;
    gMlb.KernelParamsFromCfg = TRUE;
    DEBUG ((EFI_D_INFO, "MainlineBoot: bootcfg: kernel-base = 0x%lx\n", Num));
  } else if (KeyLen == 11 && MlbMemCmp (Key, "kernel-size", 11) == 0) {
    if (!MlbParseU64Value (Value, ValueLen, &Num)) {
      DEBUG ((EFI_D_ERROR,
              "MainlineBoot: bootcfg: bad kernel-size value (ignored)\n"));
      return;
    }
    gMlb.KernelSizeReserved = Num;
    gMlb.KernelParamsFromCfg = TRUE;
    DEBUG ((EFI_D_INFO, "MainlineBoot: bootcfg: kernel-size = 0x%lx\n", Num));
  } else {
    CHAR8  K[32];
    UINTN  N = KeyLen < sizeof (K) - 1 ? KeyLen : sizeof (K) - 1;

    CopyMem (K, Key, N);
    K[N] = '\0';
    DEBUG ((EFI_D_INFO,
            "MainlineBoot: bootcfg: ignoring key '%a' (%lu byte value)\n",
            K, (UINT64)ValueLen));
  }
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

  /* Re-apply from the platform defaults: a bootcfg without kernel-base=/
   * kernel-size= keys must not inherit a previous file's override.  The
   * keys fire through MlbBootcfgOtherKey() during MlbParseBootcfg() below,
   * so they take effect even when the file has no 'cmdline=' line. */
  gMlb.KernelBaseAddr = gMlb.PlatKernelBaseAddr;
  gMlb.KernelSizeReserved = gMlb.PlatKernelSizeReserved;
  gMlb.KernelParamsFromCfg = FALSE;

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
  Info->KernelParamsFromCfg      = gMlb.KernelParamsFromCfg;

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

EFI_STATUS
EFIAPI
MainlineBootBoot (
  VOID
  )
{
  EFI_STATUS  Status;
  VOID       *DtbBuf;

  if (!gMlb.Initialized) {
    DEBUG ((EFI_D_ERROR, "MainlineBoot: MainlineBootInit() not done\n"));
    return EFI_NOT_READY;
  }
  if (gMlb.Kernel == NULL || gMlb.Dtb == NULL) {
    DEBUG ((EFI_D_ERROR, "MainlineBoot: kernel%a and dtb%a must be loaded\n",
            gMlb.Kernel ? "" : " (missing)", gMlb.Dtb ? "" : " (missing)"));
    return EFI_NOT_READY;
  }

  /* --- 0. the kernel must carry an EFI stub (PE/COFF ARM64) --- */
  if (!MlbKernelHasEfiStub (gMlb.Kernel, gMlb.KernelSize)) {
    DEBUG ((EFI_D_ERROR,
            "MainlineBoot: kernel Image has no EFI stub (CONFIG_EFI_STUB not "
            "built?); the bare-jump path is gone, refusing to boot\n"));
    return EFI_UNSUPPORTED;
  }

  /* --- 1. stage the patched DTB in pool memory ---
   * The EFI stub allocates kernel / initrd / its FDT copy itself through
   * the boot services, so nothing goes into the ABL kernel window anymore
   * and the MlbLayout() window is no longer load-bearing
   * (MainlineBootGetLayout still reports it for diagnostics).
   * The DTB only has to stay put until the stub copies it, which pool
   * (EfiLoaderData) memory guarantees - raw window memory could be handed
   * out to those stub allocations first. The 2 MB staging buffer keeps
   * MlbUpdateDeviceTree's growth headroom identical. initrd goes through
   * the LoadFile2 protocol, so the DT chosen initrd properties are just
   * cleared (MlbUpdateChosen deletes stale ones on size 0). */
  /* ACPIReclaim pages, not pool: the closed firmware may scrub loader/
   * boot-services memory after ExitBootServices; EfiACPIReclaimMemory is
   * the loader->OS handoff type that survives for the stub to read. */
  {
    EFI_STATUS           PgStatus;
    EFI_PHYSICAL_ADDRESS DtbAddr = 0;

    PgStatus = gBS->AllocatePages (AllocateAnyPages, EfiACPIReclaimMemory,
                                   EFI_SIZE_TO_PAGES (MLB_DT_SIZE_2MB),
                                   &DtbAddr);
    if (EFI_ERROR (PgStatus)) {
      return PgStatus;
    }
    DtbBuf = (VOID *)(UINTN)DtbAddr;
  }
  CopyMem (DtbBuf, gMlb.Dtb, gMlb.DtbSize);

  Status = MlbUpdateDeviceTree (DtbBuf, MLB_DT_SIZE_2MB,
                                gMlb.CmdLineLoaded ? gMlb.CmdLine : NULL,
                                0, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "MainlineBoot: device tree update failed: %r\n",
            Status));
    return Status;        /* ACPIReclaim staging stays, harmless */
  }
  /* screen stays quiet on the success path (debug port only) */
  DEBUG ((EFI_D_INFO,
          "MainlineBoot: dtb updated: kernel %lu, initrd %lu, cmdline %a\n",
          (UINT64)gMlb.KernelSize, (UINT64)gMlb.InitrdSize,
          gMlb.CmdLineLoaded ? "yes" : "no"));

  /* --- 2. publish DTB + initrd for the stub (configuration table) --- */
  Status = MlbInstallFdtConfigTable (DtbBuf);
  if (EFI_ERROR (Status)) {
    return Status;                    /* DtbBuf leaked on purpose: the table entry must stay valid */
  }
  Status = MlbInstallInitrdMedia ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* --- 3. turn off display before kernel handoff ---
   * The panel must not be left in ABL's state during the kernel transition.
   * msm DRM initializes it from scratch after boot. This eliminates the
   * white-flash / panel-abnormal-state sequence. The GOP itself is removed
   * right before StartImage() (MlbDetachGop) so the kernel gets no efifb. */
  {
    EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop = MlbLocateGop ();

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

  /* --- 4. start the kernel EFI stub ---
   * ExitBootServices, the cache/MMU quiesce and the kernel jump are the
   * stub's job now; MlbBootKernelEfiStub() returns only on failure (when
   * it does, boot services are still alive, so BootApp can report and
   * return to the boot menu). */
  Status = MlbBootKernelEfiStub (gMlb.CmdLineLoaded ? gMlb.CmdLine : NULL);
  return Status;
}
