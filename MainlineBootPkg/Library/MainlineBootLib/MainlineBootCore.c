/** @file MainlineBootCore.c

  Pure algorithms for MainlineBootLib (host-testable, see MainlineBootCore.h).

  SPDX-License-Identifier: BSD-3-Clause-Clear
**/

#include "MainlineBootCore.h"
#ifndef MLB_CORE_NO_FDT
#include <libfdt.h>
#endif

/* ------------------------------------------------------------------------ */
/* Kernel header                                                             */
/* ------------------------------------------------------------------------ */

EFI_STATUS
MlbCheckKernelHeader (
  IN  CONST VOID           *Buffer,
  IN  UINTN                 Size,
  OUT MLB_KERNEL_HDR_INFO  *Info
  )
{
  CONST UINT8            *Bytes = (CONST UINT8 *)Buffer;
  MLB_ARM64_IMAGE_HEADER  Hdr;

  if (Buffer == NULL || Info == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  ZeroMem (Info, sizeof (*Info));

  if (Size >= 2 && Bytes[0] == 0x1f && Bytes[1] == 0x8b) {
    Info->IsGzip = TRUE;
    return EFI_UNSUPPORTED;
  }
  if (Size < MLB_ARM64_IMAGE_HDR_SIZE) {
    return EFI_INVALID_PARAMETER;
  }

  /* Header may be unaligned in the caller's buffer -> copy first. */
  CopyMem (&Hdr, Buffer, sizeof (Hdr));

  if (Hdr.Magic != MLB_ARM64_IMAGE_MAGIC) {
    return EFI_INVALID_PARAMETER;
  }
  /* Flags bit 0: 0 = little endian, 1 = big endian. We only do LE. */
  if (Hdr.ImageSize != 0 && (Hdr.Flags & 1) != 0) {
    return EFI_UNSUPPORTED;
  }

  Info->TextOffset = Hdr.TextOffset;
  Info->ImageSize  = Hdr.ImageSize;
  Info->Flags      = Hdr.Flags;
  return EFI_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* Layout                                                                    */
/* ------------------------------------------------------------------------ */

EFI_STATUS
MlbComputeLayout (
  IN  CONST MLB_LAYOUT_IN  *In,
  OUT MLB_LAYOUT_OUT       *Out
  )
{
  UINT64  KernelSpan;

  if (In == NULL || Out == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  ZeroMem (Out, sizeof (*Out));

  /* ABL UpdateBootParams: KernelLoadAddr += ImageSize ? TextOffset : 0x80000 */
  Out->KernelLoadAddr = In->KernelBaseAddr +
                        (In->ImageSize != 0 ? In->TextOffset
                                            : MLB_KERNEL_64BIT_LOAD_OFFSET);
  Out->KernelEndAddr  = In->KernelBaseAddr + In->KernelSizeReserved;

  /* Overflow / degenerate input guards */
  if (Out->KernelEndAddr < In->KernelBaseAddr ||
      Out->KernelLoadAddr < In->KernelBaseAddr) {
    return EFI_INVALID_PARAMETER;
  }

  /* Ramdisk goes at the top: end - (round_up(size, page) + page) */
  Out->RamdiskLoadAddr = Out->KernelEndAddr -
                         (MLB_ROUND_UP (In->InitrdSize, MLB_PAGE_SIZE) +
                          MLB_PAGE_SIZE);
  if (Out->RamdiskLoadAddr > Out->KernelEndAddr) {
    return EFI_BUFFER_TOO_SMALL;            /* underflow */
  }

  /* DT below the ramdisk: 2 MB window + one page of slack */
  Out->DeviceTreeLoadAddr = Out->RamdiskLoadAddr -
                            (MLB_DT_SIZE_2MB + MLB_PAGE_SIZE);
  if (Out->DeviceTreeLoadAddr > Out->RamdiskLoadAddr) {
    return EFI_BUFFER_TOO_SMALL;            /* underflow */
  }

  if (Out->DeviceTreeLoadAddr <= Out->KernelLoadAddr) {
    return EFI_BUFFER_TOO_SMALL;            /* "Not Enough space left to load kernel image" */
  }

  /* ABL: "DTB header can get corrupted due to runtime kernel size" */
  KernelSpan = In->ImageSize > In->KernelFileSize ? In->ImageSize
                                                  : In->KernelFileSize;
  if (KernelSpan > Out->DeviceTreeLoadAddr - Out->KernelLoadAddr) {
    return EFI_BUFFER_TOO_SMALL;
  }

  return EFI_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* Carveouts                                                                 */
/* ------------------------------------------------------------------------ */

UINT32
MlbSortAndCombineRegions (
  IN OUT MLB_MEM_REGION  *Regions,
  IN     UINT32           Count
  )
{
  UINT32          i, j, n;
  MLB_MEM_REGION  Tmp;

  if (Regions == NULL || Count == 0) {
    return 0;
  }

  /* Drop zero-size entries */
  n = 0;
  for (i = 0; i < Count; i++) {
    if (Regions[i].Size != 0) {
      Regions[n++] = Regions[i];
    }
  }
  Count = n;
  if (Count == 0) {
    return 0;
  }

  /* Insertion sort by Base (Count is small) */
  for (i = 1; i < Count; i++) {
    Tmp = Regions[i];
    j = i;
    while (j > 0 && Regions[j - 1].Base > Tmp.Base) {
      Regions[j] = Regions[j - 1];
      j--;
    }
    Regions[j] = Tmp;
  }

  /* Merge overlapping or adjacent */
  n = 0;
  for (i = 1; i < Count; i++) {
    UINT64 CurEnd = Regions[n].Base + Regions[n].Size;
    UINT64 NxtEnd = Regions[i].Base + Regions[i].Size;
    if (Regions[i].Base <= CurEnd) {
      if (NxtEnd > CurEnd) {
        Regions[n].Size = NxtEnd - Regions[n].Base;
      }
    } else {
      n++;
      Regions[n] = Regions[i];
    }
  }
  return n + 1;
}

EFI_STATUS
MlbRemoveRegions (
  IN  CONST MLB_MEM_REGION  *Parts,
  IN  UINT32                 PartCount,
  IN  CONST MLB_MEM_REGION  *NoMap,
  IN  UINT32                 NoMapCount,
  OUT MLB_MEM_REGION        *Out,
  IN  UINT32                 OutMax,
  OUT UINT32                *OutCount
  )
{
  UINT32  i, j, n;

  if (Parts == NULL || Out == NULL || OutCount == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (NoMapCount != 0 && NoMap == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  n = 0;
  for (i = 0; i < PartCount; i++) {
    UINT64  Cur = Parts[i].Base;
    UINT64  End = Parts[i].Base + Parts[i].Size;

    if (Parts[i].Size == 0) {
      continue;                             /* nothing to describe */
    }

    for (j = 0; j < NoMapCount && Cur < End; j++) {
      UINT64  RS = NoMap[j].Base;
      UINT64  RE = NoMap[j].Base + NoMap[j].Size;

      if (RE <= Cur) {
        continue;                           /* carveout entirely below */
      }
      if (RS >= End) {
        break;                              /* sorted: rest are above */
      }
      if (RS > Cur) {                       /* keep [Cur, RS) */
        if (n >= OutMax) {
          return EFI_OUT_OF_RESOURCES;
        }
        Out[n].Base = Cur;
        Out[n].Size = RS - Cur;
        n++;
      }
      if (RE > Cur) {
        Cur = RE;                           /* skip the carveout */
      }
    }

    if (Cur < End) {                        /* remaining tail */
      if (n >= OutMax) {
        return EFI_OUT_OF_RESOURCES;
      }
      Out[n].Base = Cur;
      Out[n].Size = End - Cur;
      n++;
    }
  }

  *OutCount = n;
  return EFI_SUCCESS;
}

BOOLEAN
MlbRegionContainedIn (
  IN CONST MLB_MEM_REGION  *Regions,
  IN UINT32                 Count,
  IN UINT64                 Base,
  IN UINT64                 Size
  )
{
  UINT32  i;
  UINT64  End = Base + Size;

  if (Regions == NULL || Size == 0 || End < Base) {
    return FALSE;
  }
  for (i = 0; i < Count; i++) {
    UINT64  RE = Regions[i].Base + Regions[i].Size;
    if (Regions[i].Size != 0 && Regions[i].Base <= Base && End <= RE) {
      return TRUE;
    }
  }
  return FALSE;
}

BOOLEAN
MlbRegionsOverlap (
  IN  CONST MLB_MEM_REGION  *Regions,
  IN  UINT32                 Count,
  IN  UINT64                 Base,
  IN  UINT64                 Size,
  OUT MLB_MEM_REGION        *Hit    OPTIONAL
  )
{
  UINT32  i;
  UINT64  End = Base + Size;

  if (Regions == NULL || Size == 0) {
    return FALSE;
  }
  for (i = 0; i < Count; i++) {
    UINT64  RE = Regions[i].Base + Regions[i].Size;
    if (Regions[i].Size != 0 && Regions[i].Base < End && Base < RE) {
      if (Hit != NULL) {
        *Hit = Regions[i];
      }
      return TRUE;
    }
  }
  return FALSE;
}

/* ------------------------------------------------------------------------ */
/* bootcfg                                                                   */
/* ------------------------------------------------------------------------ */

STATIC BOOLEAN
MlbIsSpace (
  IN CHAR8  C
  )
{
  return (C == ' ' || C == '\t' || C == '\r' || C == '\n' || C == '\v' || C == '\f');
}

STATIC VOID
MlbTrim (
  IN OUT CONST CHAR8  **Start,
  IN OUT UINTN         *Len
  )
{
  while (*Len > 0 && MlbIsSpace (**Start)) {
    (*Start)++;
    (*Len)--;
  }
  while (*Len > 0 && MlbIsSpace ((*Start)[*Len - 1])) {
    (*Len)--;
  }
}

EFI_STATUS
MlbParseBootcfg (
  IN  CONST CHAR8               *Text,
  IN  UINTN                      Size,
  OUT CHAR8                     *OutCmdLine,
  IN  UINTN                      OutMax,
  IN  MLB_BOOTCFG_OTHER_KEY_CB   OtherKeyCb   OPTIONAL,
  IN  VOID                      *Context      OPTIONAL
  )
{
  UINTN         Pos = 0;
  BOOLEAN       Found = FALSE;
  BOOLEAN       Truncated = FALSE;
  STATIC CONST CHAR8 CmdKey[] = "cmdline";

  if (Text == NULL || OutCmdLine == NULL || OutMax == 0) {
    return EFI_INVALID_PARAMETER;
  }

  while (Pos < Size) {
    CONST CHAR8  *Line = Text + Pos;
    UINTN         LineLen = 0;
    CONST CHAR8  *Key, *Val;
    UINTN         KeyLen, ValLen, k;

    /* find end of line ('\n'); a NUL also terminates the text */
    while (Pos + LineLen < Size && Line[LineLen] != '\n' && Line[LineLen] != '\0') {
      LineLen++;
    }
    if (Pos + LineLen < Size && Line[LineLen] == '\0') {
      Size = Pos + LineLen;                 /* stop at embedded NUL */
    }
    Pos += LineLen + 1;                     /* skip '\n' */

    Key = Line;
    KeyLen = LineLen;
    MlbTrim (&Key, &KeyLen);                /* handles "\r\n" too */
    if (KeyLen == 0 || Key[0] == '#') {
      continue;
    }

    /* split at first '=' */
    for (k = 0; k < KeyLen && Key[k] != '='; k++) {
    }
    if (k == KeyLen) {
      /* no '=' : report as a key with empty value */
      if (OtherKeyCb != NULL) {
        OtherKeyCb (Key, KeyLen, Key + KeyLen, 0, Context);
      }
      continue;
    }
    Val    = Key + k + 1;
    ValLen = KeyLen - k - 1;
    KeyLen = k;
    MlbTrim (&Key, &KeyLen);
    MlbTrim (&Val, &ValLen);

    if (KeyLen == sizeof (CmdKey) - 1 &&
        MlbMemCmp (Key, CmdKey, KeyLen) == 0) {
      /* last one wins */
      UINTN Copy = ValLen;
      if (Copy > OutMax - 1) {
        Copy = OutMax - 1;
        Truncated = TRUE;
      } else {
        Truncated = FALSE;
      }
      CopyMem (OutCmdLine, Val, Copy);
      OutCmdLine[Copy] = '\0';
      Found = TRUE;
    } else if (OtherKeyCb != NULL) {
      OtherKeyCb (Key, KeyLen, Val, ValLen, Context);
    }
  }

  if (!Found) {
    return EFI_NOT_FOUND;
  }
  return Truncated ? EFI_BUFFER_TOO_SMALL : EFI_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* FDT read-only helpers                                                     */
/* ------------------------------------------------------------------------ */

#ifndef MLB_CORE_NO_FDT

UINT64
MlbReadCells (
  IN CONST UINT32  *P,
  IN UINT32         Cells
  )
{
  UINT64  V = 0;
  UINT32  i;

  for (i = 0; i < Cells; i++) {
    V = (V << 32) | fdt32_to_cpu (P[i]);
  }
  return V;
}

VOID
MlbWriteCells (
  OUT UINT32  *P,
  IN  UINT32   Cells,
  IN  UINT64   V
  )
{
  INT32  i;

  for (i = (INT32)Cells - 1; i >= 0; i--) {
    P[i] = cpu_to_fdt32 ((UINT32)V);
    V >>= 32;
  }
}

VOID
MlbFdtGetCells (
  IN  CONST VOID  *Fdt,
  IN  INT32        NodeOffset,
  IN  UINT32       DefaultAddr,
  IN  UINT32       DefaultSize,
  OUT UINT32      *AddrCells,
  OUT UINT32      *SizeCells
  )
{
  CONST UINT32  *Prop;
  INT32          Len;

  *AddrCells = DefaultAddr;
  *SizeCells = DefaultSize;

  Prop = fdt_getprop (Fdt, NodeOffset, "#address-cells", &Len);
  if (Prop != NULL && Len >= 4) {
    *AddrCells = fdt32_to_cpu (*Prop);
  }
  Prop = fdt_getprop (Fdt, NodeOffset, "#size-cells", &Len);
  if (Prop != NULL && Len >= 4) {
    *SizeCells = fdt32_to_cpu (*Prop);
  }
}

EFI_STATUS
MlbFdtGetNoMapRegions (
  IN  CONST VOID      *Fdt,
  OUT MLB_MEM_REGION  *Regs,
  IN  UINT32           Max,
  OUT UINT32          *Count
  )
{
  INT32          ResOff, Sub, Len;
  UINT32         AddrCells, SizeCells, RootA, RootS, Entry, N = 0;
  CONST UINT32  *Reg;
  CONST CHAR8   *Status;

  *Count = 0;

  ResOff = fdt_path_offset (Fdt, "/reserved-memory");
  if (ResOff < 0) {
    return EFI_NOT_FOUND;
  }

  MlbFdtGetCells (Fdt, 0, 2, 2, &RootA, &RootS);
  MlbFdtGetCells (Fdt, ResOff, RootA, RootS, &AddrCells, &SizeCells);
  if (AddrCells == 0 || AddrCells > 2 || SizeCells == 0 || SizeCells > 2) {
    return EFI_UNSUPPORTED;
  }
  Entry = (AddrCells + SizeCells) * 4;

  for (Sub = fdt_first_subnode (Fdt, ResOff); Sub >= 0;
       Sub = fdt_next_subnode (Fdt, Sub)) {
    if (fdt_get_property (Fdt, Sub, "no-map", &Len) == NULL) {
      continue;
    }
    Status = fdt_getprop (Fdt, Sub, "status", &Len);
    if (Status != NULL && Len >= 8 && MlbMemCmp (Status, "disabled", 8) == 0) {
      continue;
    }
    Reg = fdt_getprop (Fdt, Sub, "reg", &Len);
    if (Reg == NULL || Len <= 0) {
      continue;                            /* dynamic (size=) allocation */
    }
    while (Len >= (INT32)Entry) {
      if (N >= Max) {
        return EFI_OUT_OF_RESOURCES;
      }
      Regs[N].Base = MlbReadCells (Reg, AddrCells);
      Regs[N].Size = MlbReadCells (Reg + AddrCells, SizeCells);
      N++;
      Reg += AddrCells + SizeCells;
      Len -= (INT32)Entry;
    }
  }

  *Count = N;
  return EFI_SUCCESS;
}

#endif /* MLB_CORE_NO_FDT */

/* ------------------------------------------------------------------------ */
/* initrd magic                                                              */
/* ------------------------------------------------------------------------ */

MLB_INITRD_KIND
MlbDetectInitrdKind (
  IN CONST VOID  *Buffer,
  IN UINTN        Size
  )
{
  CONST UINT8  *B = (CONST UINT8 *)Buffer;

  if (B == NULL || Size < 6) {
    return MlbInitrdUnknown;
  }
  if (MlbMemCmp (B, "070701", 6) == 0 || MlbMemCmp (B, "070702", 6) == 0) {
    return MlbInitrdCpioNewc;
  }
  if (MlbMemCmp (B, "070707", 6) == 0) {
    return MlbInitrdCpioOdc;
  }
  if (B[0] == 0x1f && B[1] == 0x8b) {
    return MlbInitrdGzip;
  }
  if (B[0] == 0x28 && B[1] == 0xb5 && B[2] == 0x2f && B[3] == 0xfd) {
    return MlbInitrdZstd;
  }
  if (B[0] == 0xfd && B[1] == '7' && B[2] == 'z' && B[3] == 'X' && B[4] == 'Z' && B[5] == 0x00) {
    return MlbInitrdXz;
  }
  if (B[0] == 0x04 && B[1] == 0x22 && B[2] == 0x4d && B[3] == 0x18) {
    return MlbInitrdLz4;
  }
  if (B[0] == 'B' && B[1] == 'Z' && B[2] == 'h') {
    return MlbInitrdBzip2;
  }
  if (B[0] == 0x5d && B[1] == 0x00 && B[2] == 0x00) {
    return MlbInitrdLzma;
  }
  if (B[0] == 0x89 && B[1] == 'L' && B[2] == 'Z' && B[3] == 'O') {
    return MlbInitrdLzo;
  }
  return MlbInitrdUnknown;
}
