/** @file MainlineBootFdt.c

  Device tree fix-ups performed right before jumping to the kernel.

  Ported from QcomModulePkg/Library/BootLib/UpdateDeviceTree.c:
    QueryMemoryCellSize, GetNoMapRegions, SortNoMapRegions,
    CombineNoMapRegions, UpdateRamPartitions, AddMemMap,
    dev_tree_add_mem_infoV64 and the /chosen part of UpdateDeviceTree.
  The pure region arithmetic lives in MainlineBootCore.c (host-testable).

  SPDX-License-Identifier: BSD-3-Clause-Clear
**/

#include "MainlineBootInternal.h"
#include <libfdt.h>
#include <Protocol/EFIRng.h>
#include <Protocol/GraphicsOutput.h>

#define MLB_RNG_SEED_BYTES   64      /* >= 64 bytes as requested */
#define MLB_DEFAULT_CELLS    2

/* Cell helpers and the /reserved-memory no-map walker live in
 * MainlineBootCore.c (MlbFdtGetCells / MlbFdtGetNoMapRegions /
 * MlbReadCells / MlbWriteCells) so the host unit test runs the same code. */

/* ------------------------------------------------------------------------ */
/* /memory                                                                   */
/* ------------------------------------------------------------------------ */

STATIC EFI_STATUS
MlbUpdateMemoryNode (
  IN VOID  *Fdt
  )
{
  EFI_STATUS       Status;
  INT32            MemOff, Ret;
  UINT32           AddrCells, SizeCells, i, Entry, N;
  MLB_MEM_REGION   Parts[MLB_MAX_MEM_REGIONS];
  MLB_MEM_REGION   Final[MLB_MAX_MEM_REGIONS];
  UINT32           NumFinal;
  UINT32          *Buf;

  /* "/memory" also matches "memory@a0000000" (libfdt ignores unit address) */
  MemOff = fdt_path_offset (Fdt, "/memory");
  if (MemOff < 0) {
    DEBUG ((EFI_D_WARN, "MainlineBoot: no /memory node, creating one\n"));
    MemOff = fdt_add_subnode (Fdt, 0, "memory");
    if (MemOff < 0) {
      DEBUG ((EFI_D_ERROR, "MainlineBoot: fdt_add_subnode(memory): %a\n",
              fdt_strerror (MemOff)));
      return EFI_DEVICE_ERROR;
    }
    Ret = fdt_setprop_string (Fdt, MemOff, "device_type", "memory");
    if (Ret < 0) {
      return EFI_DEVICE_ERROR;
    }
  }

  /* cells: root #address-cells/#size-cells, default 2/2 (QueryMemoryCellSize) */
  MlbFdtGetCells (Fdt, 0, MLB_DEFAULT_CELLS, MLB_DEFAULT_CELLS, &AddrCells, &SizeCells);
  if (AddrCells == 0 || AddrCells > 2 || SizeCells == 0 || SizeCells > 2) {
    DEBUG ((EFI_D_ERROR, "MainlineBoot: unsupported root cells %u/%u\n",
            AddrCells, SizeCells));
    return EFI_UNSUPPORTED;
  }
  DEBUG ((EFI_D_INFO, "MainlineBoot: root #address-cells=%u #size-cells=%u\n",
          AddrCells, SizeCells));

  if (gMlb.NumRamPartitions == 0 || gMlb.NumRamPartitions > MLB_MAX_MEM_REGIONS) {
    return EFI_UNSUPPORTED;
  }
  for (i = 0; i < gMlb.NumRamPartitions; i++) {
    Parts[i].Base = gMlb.RamPartitions[i].Base;
    Parts[i].Size = gMlb.RamPartitions[i].AvailableLength;
  }

#ifdef MAINLINE_REMOVE_CARVEOUT
  {
    MLB_MEM_REGION  NoMap[MLB_MAX_MEM_REGIONS];
    UINT32          NumNoMap = 0;

    Status = MlbFdtGetNoMapRegions (Fdt, NoMap, MLB_MAX_MEM_REGIONS, &NumNoMap);
    if (Status == EFI_NOT_FOUND) {
      DEBUG ((EFI_D_INFO, "MainlineBoot: no /reserved-memory node\n"));
    } else if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "MainlineBoot: reading no-map regions failed: %r\n",
              Status));
      return Status;
    }
    DEBUG ((EFI_D_INFO, "MainlineBoot: %u raw no-map region(s) in DT\n", NumNoMap));
    NumNoMap = MlbSortAndCombineRegions (NoMap, NumNoMap);
    DEBUG ((EFI_D_INFO, "MainlineBoot: %u merged no-map carveout(s):\n", NumNoMap));
    for (i = 0; i < NumNoMap; i++) {
      DEBUG ((EFI_D_INFO, "  carveout 0x%016lx - 0x%016lx\n", NoMap[i].Base,
              NoMap[i].Base + NoMap[i].Size));
    }
    Status = MlbRemoveRegions (Parts, gMlb.NumRamPartitions, NoMap, NumNoMap,
                               Final, MLB_MAX_MEM_REGIONS, &NumFinal);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "MainlineBoot: carveout removal failed: %r\n", Status));
      return Status;
    }
  }
#else
  CopyMem (Final, Parts, gMlb.NumRamPartitions * sizeof (MLB_MEM_REGION));
  NumFinal = gMlb.NumRamPartitions;
#endif

  if (NumFinal == 0) {
    DEBUG ((EFI_D_ERROR, "MainlineBoot: no usable RAM left after carveouts\n"));
    return EFI_NOT_FOUND;
  }

  /* Build the whole "reg" property in one go: equivalent to ABL's
   * fdt_setprop_u64 for the first + fdt_appendprop_u64 for the rest. */
  Entry = (AddrCells + SizeCells) * sizeof (UINT32);
  Buf = AllocateZeroPool (NumFinal * Entry);
  if (Buf == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  DEBUG ((EFI_D_INFO, "MainlineBoot: Final RAM partitions (%u):\n", NumFinal));
  N = 0;
  for (i = 0; i < NumFinal; i++) {
    DEBUG ((EFI_D_INFO, "  Add Base: 0x%016lx Available Length: 0x%016lx\n",
            Final[i].Base, Final[i].Size));
    if ((AddrCells == 1 && Final[i].Base >= 0x100000000ULL) ||
        (SizeCells == 1 && Final[i].Size >= 0x100000000ULL)) {
      DEBUG ((EFI_D_ERROR, "MainlineBoot: region does not fit in 32-bit cells\n"));
      FreePool (Buf);
      return EFI_UNSUPPORTED;
    }
    MlbWriteCells (Buf + N, AddrCells, Final[i].Base);
    N += AddrCells;
    MlbWriteCells (Buf + N, SizeCells, Final[i].Size);
    N += SizeCells;
  }

  Ret = fdt_setprop (Fdt, MemOff, "reg", Buf, NumFinal * Entry);
  FreePool (Buf);
  if (Ret < 0) {
    DEBUG ((EFI_D_ERROR, "MainlineBoot: fdt_setprop(memory/reg): %a\n",
            fdt_strerror (Ret)));
    return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* simple-framebuffer (UEFI GOP handoff) — DISABLED                          */
/* msm DRM initializes the panel from scratch; the GOP handoff causes panel */
/* abnormal states during kernel transition. Re-enable if needed for debug.  */
/* ------------------------------------------------------------------------ */
#if 0

/* Locate the Graphics Output Protocol the way ABL's DrawUI does: try the
 * console-out handle first, then a plain LocateProtocol. */
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL *
MlbLocateGop (
  VOID
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop = NULL;

  if (gST != NULL && gST->ConsoleOutHandle != NULL) {
    gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid,
                         (VOID **)&Gop);
    if (Gop != NULL) {
      return Gop;
    }
  }
  gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  return Gop;
}

/* Map GOP pixel information to a simplefb format string. Returns the byte
 * depth in *Bpp, or NULL when the layout has no simplefb equivalent. */
STATIC CONST CHAR8 *
MlbGopSimpleFbFormat (
  IN  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *Info,
  OUT UINT32                                *Bpp
  )
{
  EFI_PIXEL_BITMASK  *M;

  switch (Info->PixelFormat) {
  case PixelRedGreenBlueReserved8BitPerColor:
    *Bpp = 4;
    return "a8b8g8r8";
  case PixelBlueGreenRedReserved8BitPerColor:
    *Bpp = 4;
    return "a8r8g8b8";
  case PixelBitMask:
    M = &Info->PixelInformation;
    if (M->RedMask == 0x0000f800 && M->GreenMask == 0x000007e0 &&
        M->BlueMask == 0x0000001f) {
      *Bpp = 2;
      return "r5g6b5";
    }
    if (M->RedMask == 0x000000ff && M->GreenMask == 0x0000ff00 &&
        M->BlueMask == 0x00ff0000) {
      *Bpp = 4;
      return "a8b8g8r8";
    }
    if (M->RedMask == 0x00ff0000 && M->GreenMask == 0x0000ff00 &&
        M->BlueMask == 0x000000ff) {
      *Bpp = 4;
      return "x8r8g8b8";
    }
    return NULL;
  default:
    return NULL;
  }
}

/* Add a "simple-framebuffer" node under /chosen plus a matching no-map
 * /reserved-memory entry, so the kernel gets an early framebuffer console
 * on the display ABL already lit up (msm DRM takes over later if it probes).
 * Best effort: every failure just skips the node and boots without it. */
STATIC VOID
MlbAddSimpleFramebuffer (
  IN VOID  *Fdt
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL          *Gop;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *Info;
  CONST CHAR8                           *Fmt;
  CHAR8                                  Name[40];
  UINT32                                 Bpp = 0;
  UINT32                                 AddrCells, SizeCells;
  UINT32                                 Reg[4];
  UINT32                                 N;
  UINT64                                 FbBase, FbSize, Stride;
  INT32                                  Rm, Chosen, Node, Ret;

  Gop = MlbLocateGop ();
  if (Gop == NULL || Gop->Mode == NULL || Gop->Mode->Info == NULL) {
    DEBUG ((EFI_D_WARN, "MainlineBoot: no GOP, simplefb skipped\n"));
    return;
  }
  Info   = Gop->Mode->Info;
  FbBase = Gop->Mode->FrameBufferBase;
  FbSize = Gop->Mode->FrameBufferSize;
  Fmt    = MlbGopSimpleFbFormat (Info, &Bpp);
  if (Fmt == NULL || Bpp == 0 || Info->PixelsPerScanLine == 0 ||
      Info->HorizontalResolution == 0 || Info->VerticalResolution == 0 ||
      FbBase == 0 || FbSize == 0) {
    DEBUG ((EFI_D_WARN,
            "MainlineBoot: GOP has no usable linear framebuffer, simplefb skipped\n"));
    return;
  }
  Stride = (UINT64)Info->PixelsPerScanLine * Bpp;

  /* The kernel window [KernelBaseAddr, +KernelSizeReserved) holds Image +
   * initrd + the 2 MB DTB we are about to copy; never hand that out. */
  if (FbBase < gMlb.KernelBaseAddr + gMlb.KernelSizeReserved &&
      gMlb.KernelBaseAddr < FbBase + FbSize) {
    DEBUG ((EFI_D_WARN,
            "MainlineBoot: GOP fb 0x%lx overlaps kernel window, simplefb skipped\n",
            FbBase));
    return;
  }

  DEBUG ((EFI_D_INFO,
          "MainlineBoot: GOP %ux%u stride %lu (%a), fb 0x%lx + 0x%lx\n",
          Info->HorizontalResolution, Info->VerticalResolution, Stride, Fmt,
          FbBase, FbSize));

  /* /reserved-memory: no-map carve-out (also subtracted from /memory when
   * MAINLINE_REMOVE_CARVEOUT is enabled, since this runs before
   * MlbUpdateMemoryNode). */
  Rm = fdt_path_offset (Fdt, "/reserved-memory");
  if (Rm < 0) {
    Rm = fdt_add_subnode (Fdt, 0, "reserved-memory");
    if (Rm < 0) {
      DEBUG ((EFI_D_WARN, "MainlineBoot: cannot add /reserved-memory\n"));
      return;
    }
    fdt_setprop_u32 (Fdt, Rm, "#address-cells", 2);
    fdt_setprop_u32 (Fdt, Rm, "#size-cells", 2);
    fdt_setprop (Fdt, Rm, "ranges", NULL, 0);
    AddrCells = SizeCells = 2;
  } else {
    MlbFdtGetCells (Fdt, Rm, MLB_DEFAULT_CELLS, MLB_DEFAULT_CELLS,
                    &AddrCells, &SizeCells);
  }
  if (AddrCells == 0 || AddrCells > 2 || SizeCells == 0 || SizeCells > 2) {
    DEBUG ((EFI_D_WARN, "MainlineBoot: reserved-memory cells %u/%u\n",
            AddrCells, SizeCells));
    return;
  }

  AsciiSPrint (Name, sizeof (Name), "framebuffer@%lx", FbBase);
  Node = fdt_subnode_offset (Fdt, Rm, Name);
  if (Node < 0) {
    Node = fdt_add_subnode (Fdt, Rm, Name);
  }
  if (Node < 0) {
    DEBUG ((EFI_D_WARN, "MainlineBoot: cannot add %a\n", Name));
    return;
  }
  N = 0;
  MlbWriteCells (Reg + N, AddrCells, FbBase);
  N += AddrCells;
  MlbWriteCells (Reg + N, SizeCells, FbSize);
  N += SizeCells;
  Ret = fdt_setprop (Fdt, Node, "reg", Reg, N * sizeof (UINT32));
  if (Ret == 0) {
    Ret = fdt_setprop (Fdt, Node, "no-map", NULL, 0);
  }
  if (Ret < 0) {
    DEBUG ((EFI_D_WARN, "MainlineBoot: framebuffer reserved-memory: %a\n",
            fdt_strerror (Ret)));
    return;
  }

  /* /chosen: the simple-framebuffer node itself (cells + empty ranges so
   * the kernel resolves reg via of_address_to_resource). */
  Chosen = fdt_path_offset (Fdt, "/chosen");
  if (Chosen < 0) {
    Chosen = fdt_add_subnode (Fdt, 0, "chosen");
    if (Chosen < 0) {
      DEBUG ((EFI_D_WARN, "MainlineBoot: cannot add /chosen\n"));
      return;
    }
  }
  fdt_setprop_u32 (Fdt, Chosen, "#address-cells", 2);
  fdt_setprop_u32 (Fdt, Chosen, "#size-cells", 2);
  fdt_setprop (Fdt, Chosen, "ranges", NULL, 0);

  Node = fdt_subnode_offset (Fdt, Chosen, Name);
  if (Node < 0) {
    Node = fdt_add_subnode (Fdt, Chosen, Name);
  }
  if (Node < 0) {
    DEBUG ((EFI_D_WARN, "MainlineBoot: cannot add chosen/%a\n", Name));
    return;
  }

  N = 0;
  MlbWriteCells (Reg + N, 2, FbBase);
  N += 2;
  MlbWriteCells (Reg + N, 2, FbSize);
  Ret = fdt_setprop (Fdt, Node, "reg", Reg, 4 * sizeof (UINT32));
  if (Ret == 0) {
    Ret = fdt_setprop_string (Fdt, Node, "compatible", "simple-framebuffer");
  }
  if (Ret == 0) {
    Ret = fdt_setprop_u32 (Fdt, Node, "width", Info->HorizontalResolution);
  }
  if (Ret == 0) {
    Ret = fdt_setprop_u32 (Fdt, Node, "height", Info->VerticalResolution);
  }
  if (Ret == 0) {
    Ret = fdt_setprop_u32 (Fdt, Node, "stride", (UINT32)Stride);
  }
  if (Ret == 0) {
    Ret = fdt_setprop_string (Fdt, Node, "format", Fmt);
  }
  if (Ret < 0) {
    DEBUG ((EFI_D_WARN, "MainlineBoot: simplefb node: %a\n", fdt_strerror (Ret)));
    return;
  }

  DEBUG ((EFI_D_INFO, "MainlineBoot: /chosen/%a added (simple-framebuffer)\n", Name));
}
#endif /* simple-framebuffer disabled */

/* ------------------------------------------------------------------------ */
/* random seeds                                                              */
/* ------------------------------------------------------------------------ */

/* UpdateDeviceTree.c GetRandomSeed(), generalised to N bytes. Note that
 * gQcomRngProtocolGuid == gEfiRngProtocolGuid (same GUID value), so this
 * also finds a standard EFI_RNG_PROTOCOL implementation. */
STATIC EFI_STATUS
MlbGetRandom (
  OUT UINT8  *Buf,
  IN  UINTN   Len
  )
{
  EFI_QCOM_RNG_PROTOCOL  *Rng = NULL;
  EFI_STATUS              Status;

  Status = gBS->LocateProtocol (&gQcomRngProtocolGuid, NULL, (VOID **)&Rng);
  if (EFI_ERROR (Status) || Rng == NULL) {
    return EFI_NOT_FOUND;
  }
  Status = Rng->GetRNG (Rng, &gEfiRNGAlgRawGuid, Len, Buf);
  if (EFI_ERROR (Status)) {
    /* some implementations only support their default algorithm */
    Status = Rng->GetRNG (Rng, NULL, Len, Buf);
  }
  return Status;
}

/* ------------------------------------------------------------------------ */
/* /chosen                                                                   */
/* ------------------------------------------------------------------------ */

STATIC EFI_STATUS
MlbUpdateChosen (
  IN VOID         *Fdt,
  IN CONST CHAR8  *CmdLine    OPTIONAL,
  IN UINT64        InitrdAddr,
  IN UINT64        InitrdSize
  )
{
  INT32        Off, Ret, Len;
  UINT64       Seed;
  UINT8        RngSeed[MLB_RNG_SEED_BYTES];
  EFI_STATUS   Status;
  CONST CHAR8 *Existing;

  Off = fdt_path_offset (Fdt, "/chosen");
  if (Off < 0) {
    Off = fdt_add_subnode (Fdt, 0, "chosen");
    if (Off < 0) {
      DEBUG ((EFI_D_ERROR, "MainlineBoot: cannot create /chosen: %a\n",
              fdt_strerror (Off)));
      return EFI_DEVICE_ERROR;
    }
  }

  /* bootargs: overwrite when we have a non-empty cmdline, else keep DT's */
  if (CmdLine != NULL && CmdLine[0] != '\0') {
    Ret = fdt_setprop_string (Fdt, Off, "bootargs", CmdLine);
    if (Ret < 0) {
      DEBUG ((EFI_D_ERROR, "MainlineBoot: cannot set bootargs: %a\n",
              fdt_strerror (Ret)));
      return EFI_LOAD_ERROR;
    }
    DEBUG ((EFI_D_INFO, "MainlineBoot: /chosen/bootargs = \"%a\"\n", CmdLine));
  } else {
    Existing = fdt_getprop (Fdt, Off, "bootargs", &Len);
    DEBUG ((EFI_D_INFO, "MainlineBoot: keeping DT bootargs: \"%a\"\n",
            Existing ? Existing : "(none)"));
  }

  /* initrd */
  if (InitrdSize != 0) {
    Ret = fdt_setprop_u64 (Fdt, Off, "linux,initrd-start", InitrdAddr);
    if (Ret == 0) {
      Ret = fdt_setprop_u64 (Fdt, Off, "linux,initrd-end", InitrdAddr + InitrdSize);
    }
    if (Ret < 0) {
      DEBUG ((EFI_D_ERROR, "MainlineBoot: cannot set linux,initrd-*: %a\n",
              fdt_strerror (Ret)));
      return EFI_LOAD_ERROR;
    }
    DEBUG ((EFI_D_INFO, "MainlineBoot: linux,initrd-start/end = 0x%lx / 0x%lx\n",
            InitrdAddr, InitrdAddr + InitrdSize));
  } else {
    /* stale values from the DT would point at garbage */
    fdt_delprop (Fdt, Off, "linux,initrd-start");
    fdt_delprop (Fdt, Off, "linux,initrd-end");
  }

  /* kaslr-seed */
  Status = MlbGetRandom ((UINT8 *)&Seed, sizeof (Seed));
  if (!EFI_ERROR (Status)) {
    Ret = fdt_setprop_u64 (Fdt, Off, "kaslr-seed", Seed);
    if (Ret < 0) {
      DEBUG ((EFI_D_WARN, "MainlineBoot: cannot set kaslr-seed: %a\n",
              fdt_strerror (Ret)));
    } else {
      DEBUG ((EFI_D_INFO, "MainlineBoot: kaslr-seed added\n"));
    }
  } else {
    DEBUG ((EFI_D_WARN, "MainlineBoot: WARNING: no RNG for kaslr-seed (%r)\n",
            Status));
  }

  /* rng-seed */
  Status = MlbGetRandom (RngSeed, sizeof (RngSeed));
  if (!EFI_ERROR (Status)) {
    Ret = fdt_setprop (Fdt, Off, "rng-seed", RngSeed, sizeof (RngSeed));
    if (Ret < 0) {
      DEBUG ((EFI_D_WARN, "MainlineBoot: cannot set rng-seed: %a\n",
              fdt_strerror (Ret)));
    } else {
      DEBUG ((EFI_D_INFO, "MainlineBoot: rng-seed (%u bytes) added\n",
              (UINT32)sizeof (RngSeed)));
    }
  } else {
    DEBUG ((EFI_D_WARN, "MainlineBoot: WARNING: no RNG for rng-seed (%r)\n",
            Status));
  }

  return EFI_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* entry                                                                     */
/* ------------------------------------------------------------------------ */

EFI_STATUS
MlbUpdateDeviceTree (
  IN VOID          *Fdt,
  IN UINTN          FdtBufferSize,
  IN CONST CHAR8   *CmdLine        OPTIONAL,
  IN UINT64         InitrdAddr,
  IN UINT64         InitrdSize
  )
{
  EFI_STATUS  Status;
  INT32       Ret;

  Ret = fdt_check_header (Fdt);
  if (Ret != 0) {
    DEBUG ((EFI_D_ERROR, "MainlineBoot: bad DTB at load address: %a\n",
            fdt_strerror (Ret)));
    return EFI_NOT_FOUND;
  }

  /* 1. grow to the full 2 MB window (ABL: fdt_open_into with DTB_PAD_SIZE) */
  Ret = fdt_open_into (Fdt, Fdt, (INT32)FdtBufferSize);
  if (Ret != 0) {
    DEBUG ((EFI_D_ERROR, "MainlineBoot: fdt_open_into: %a\n", fdt_strerror (Ret)));
    return EFI_BAD_BUFFER_SIZE;
  }

  /* 1.5 simple-framebuffer: DISABLED — msm DRM initializes the panel from
   *     scratch. Handing off ABL's GOP fb causes panel abnormal states
   *     (white flash → black → recovery) during the kernel transition. */
  /* MlbAddSimpleFramebuffer (Fdt); */

  /* 2. /memory */
  Status = MlbUpdateMemoryNode (Fdt);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* 3. /chosen */
  Status = MlbUpdateChosen (Fdt, CmdLine, InitrdAddr, InitrdSize);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* 4. pack */
  Ret = fdt_pack (Fdt);
  if (Ret != 0) {
    DEBUG ((EFI_D_ERROR, "MainlineBoot: fdt_pack: %a\n", fdt_strerror (Ret)));
    return EFI_DEVICE_ERROR;
  }
  DEBUG ((EFI_D_INFO, "MainlineBoot: DTB updated, final size %u bytes\n",
          fdt_totalsize (Fdt)));
  return EFI_SUCCESS;
}
