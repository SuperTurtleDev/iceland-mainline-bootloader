/** @file MainlineBootCore.h

  Pure (no UEFI services, no logging) algorithms used by MainlineBootLib:
    - arm64 Image header validation
    - ABL UpdateBootParams address layout
    - /reserved-memory no-map carveout removal from RAM partitions
    - bootcfg "cmdline=" parsing

  The file compiles both inside EDK2 and on a host with -DMLB_HOST_TEST
  (see MainlineBootPkg/Test/).

  SPDX-License-Identifier: BSD-3-Clause-Clear
**/

#ifndef __MAINLINE_BOOT_CORE_H__
#define __MAINLINE_BOOT_CORE_H__

#ifdef MLB_HOST_TEST
  #include <stdint.h>
  #include <stddef.h>
  #include <string.h>
  typedef uint8_t   UINT8;
  typedef uint16_t  UINT16;
  typedef uint32_t  UINT32;
  typedef uint64_t  UINT64;
  typedef int32_t   INT32;
  typedef size_t    UINTN;
  typedef char      CHAR8;
  typedef int       BOOLEAN;
  typedef UINTN     EFI_STATUS;
  #define TRUE  1
  #define FALSE 0
  #define IN
  #define OUT
  #define OPTIONAL
  #define CONST const
  #define STATIC static
  #define VOID  void
  #define MAX_BIT               (1ULL << 63)
  #define ENCODE_ERROR(a)       ((EFI_STATUS)(MAX_BIT | (a)))
  #define EFI_SUCCESS           0
  #define EFI_INVALID_PARAMETER ENCODE_ERROR (2)
  #define EFI_UNSUPPORTED       ENCODE_ERROR (3)
  #define EFI_BUFFER_TOO_SMALL  ENCODE_ERROR (5)
  #define EFI_NOT_READY         ENCODE_ERROR (6)
  #define EFI_OUT_OF_RESOURCES  ENCODE_ERROR (9)
  #define EFI_NOT_FOUND         ENCODE_ERROR (14)
  #define EFI_ERROR(s)          (((int64_t)(s)) < 0)
  #define CopyMem(d, s, n)      memcpy ((d), (s), (n))
  #define ZeroMem(d, n)         memset ((d), 0, (n))
  #define MlbMemCmp(a, b, n)    memcmp ((a), (b), (n))
#else
  #include <Uefi.h>
  #include <Library/BaseLib.h>
  #include <Library/BaseMemoryLib.h>
  #define MlbMemCmp(a, b, n)    CompareMem ((a), (b), (n))
#endif

/* ---- constants shared with ABL (BootLinux.h) ---------------------------- */
#define MLB_PAGE_SIZE               4096
#define MLB_DT_SIZE_2MB             (2 * 1024 * 1024)
#define MLB_KERNEL_64BIT_LOAD_OFFSET 0x80000        /* KERNEL_64BIT_LOAD_OFFSET */
#define MLB_FALLBACK_KERNEL_SIZE    0x05600000      /* PcdRamdiskEndAddress     */
#define MLB_ARM64_IMAGE_MAGIC       0x644d5241      /* "ARM\x64" little endian  */
#define MLB_ARM64_IMAGE_MAGIC_OFF   56
#define MLB_ARM64_IMAGE_HDR_SIZE    64
#define MLB_MAX_CMDLINE_LEN         4096            /* including NUL            */
#define MLB_MAX_MEM_REGIONS         64
#define MLB_ROUND_UP(x, a)          ((((UINT64)(x)) + ((a) - 1)) & ~((UINT64)(a) - 1))

/* ---- arm64 Image header (Documentation/arch/arm64/booting.rst) ---------- */
#pragma pack(1)
typedef struct {
  UINT32  Code0;        /* Executable code (may be "MZ" when EFI stub built) */
  UINT32  Code1;
  UINT64  TextOffset;   /* Image load offset from 2MB aligned base */
  UINT64  ImageSize;    /* Effective Image size (incl. BSS), 0 on old kernels */
  UINT64  Flags;        /* bit0: 1 = big endian; bits1-2: page size; bit3: phys placement */
  UINT64  Res2;
  UINT64  Res3;
  UINT64  Res4;
  UINT32  Magic;        /* 0x644d5241 */
  UINT32  Res5;         /* PE/COFF header offset */
} MLB_ARM64_IMAGE_HEADER;
#pragma pack()

typedef struct {
  UINT64  TextOffset;
  UINT64  ImageSize;
  UINT64  Flags;
  BOOLEAN IsGzip;       /* set when buffer starts with 1f 8b */
} MLB_KERNEL_HDR_INFO;

/**
  Validate an arm64 Image header.
  @retval EFI_SUCCESS            Valid; Info filled.
  @retval EFI_UNSUPPORTED        gzip magic (Image.gz) or big-endian kernel.
  @retval EFI_INVALID_PARAMETER  Too small / bad magic.
**/
EFI_STATUS
MlbCheckKernelHeader (
  IN  CONST VOID           *Buffer,
  IN  UINTN                 Size,
  OUT MLB_KERNEL_HDR_INFO  *Info
  );

/* ---- address layout (ABL UpdateBootParams) ----------------------------- */
typedef struct {
  UINT64  KernelBaseAddr;       /* L"KernelBaseAddr" or BaseMemory          */
  UINT64  KernelSizeReserved;   /* L"KernelSize" or 0x05600000              */
  UINT64  TextOffset;           /* from Image header                        */
  UINT64  ImageSize;            /* from Image header, 0 for old kernels     */
  UINT64  KernelFileSize;       /* bytes actually loaded                    */
  UINT64  InitrdSize;           /* 0 if none                                */
} MLB_LAYOUT_IN;

typedef struct {
  UINT64  KernelLoadAddr;
  UINT64  KernelEndAddr;
  UINT64  RamdiskLoadAddr;
  UINT64  DeviceTreeLoadAddr;
} MLB_LAYOUT_OUT;

/**
  KernelLoadAddr     = KernelBaseAddr + (ImageSize ? TextOffset : 0x80000)
  KernelEndAddr      = KernelBaseAddr + KernelSizeReserved
  RamdiskLoadAddr    = KernelEndAddr - (ROUND_UP(InitrdSize, 4K) + 4K)
  DeviceTreeLoadAddr = RamdiskLoadAddr - (2MB + 4K)
  @retval EFI_BUFFER_TOO_SMALL  DT <= kernel start, or kernel would overrun DT.
**/
EFI_STATUS
MlbComputeLayout (
  IN  CONST MLB_LAYOUT_IN  *In,
  OUT MLB_LAYOUT_OUT       *Out
  );

/* ---- carveout removal --------------------------------------------------- */
typedef struct {
  UINT64  Base;
  UINT64  Size;
} MLB_MEM_REGION;

/**
  Sort regions by Base (in place) and merge overlapping / adjacent ones.
  Zero-size regions are dropped. Returns the number of merged regions.
**/
UINT32
MlbSortAndCombineRegions (
  IN OUT MLB_MEM_REGION  *Regions,
  IN     UINT32           Count
  );

/**
  Subtract the (sorted, merged) NoMap regions from every RAM partition.
  Handles a carveout at the head, in the middle, at the tail of a partition,
  covering the whole partition, or spanning several partitions. Zero-length
  input partitions are dropped.

  @param[out] Out     Receives up to OutMax regions.
  @param[out] OutCount Number of regions written.
  @retval EFI_OUT_OF_RESOURCES  OutMax too small.
**/
EFI_STATUS
MlbRemoveRegions (
  IN  CONST MLB_MEM_REGION  *Parts,
  IN  UINT32                 PartCount,
  IN  CONST MLB_MEM_REGION  *NoMap,
  IN  UINT32                 NoMapCount,
  OUT MLB_MEM_REGION        *Out,
  IN  UINT32                 OutMax,
  OUT UINT32                *OutCount
  );

/**
  TRUE when [Base, Base+Size) lies entirely inside one of the Regions.
**/
BOOLEAN
MlbRegionContainedIn (
  IN CONST MLB_MEM_REGION  *Regions,
  IN UINT32                 Count,
  IN UINT64                 Base,
  IN UINT64                 Size
  );

/**
  TRUE when [Base, Base+Size) intersects any of the Regions. On TRUE and
  Hit != NULL, *Hit receives the first intersecting region.
**/
BOOLEAN
MlbRegionsOverlap (
  IN  CONST MLB_MEM_REGION  *Regions,
  IN  UINT32                 Count,
  IN  UINT64                 Base,
  IN  UINT64                 Size,
  OUT MLB_MEM_REGION        *Hit    OPTIONAL
  );

/* ---- bootcfg parsing ---------------------------------------------------- */

/** Callback for keys other than "cmdline" (may be NULL). Key/Value are not
    NUL terminated; lengths are given. */
typedef VOID (*MLB_BOOTCFG_OTHER_KEY_CB) (
  IN CONST CHAR8 *Key, IN UINTN KeyLen,
  IN CONST CHAR8 *Value, IN UINTN ValueLen,
  IN VOID *Context
  );

/**
  Parse Text[0..Size) as "key=value" lines. Empty lines and '#' lines are
  skipped; whitespace around key and value is trimmed. The LAST "cmdline="
  line wins. OutCmdLine receives a NUL terminated copy (truncated to
  OutMax-1 chars; a truncation returns EFI_BUFFER_TOO_SMALL).

  @retval EFI_NOT_FOUND  No "cmdline" key found (OutCmdLine untouched).
**/
EFI_STATUS
MlbParseBootcfg (
  IN  CONST CHAR8               *Text,
  IN  UINTN                      Size,
  OUT CHAR8                     *OutCmdLine,
  IN  UINTN                      OutMax,
  IN  MLB_BOOTCFG_OTHER_KEY_CB   OtherKeyCb   OPTIONAL,
  IN  VOID                      *Context      OPTIONAL
  );

/* ---- read-only FDT helpers (libfdt: EmbeddedPkg on target, system on host) */
#ifndef MLB_CORE_NO_FDT

/** Root #address-cells/#size-cells, defaulting to 2/2 when absent. */
VOID
MlbFdtGetCells (
  IN  CONST VOID  *Fdt,
  IN  INT32        NodeOffset,
  IN  UINT32       DefaultAddr,
  IN  UINT32       DefaultSize,
  OUT UINT32      *AddrCells,
  OUT UINT32      *SizeCells
  );

/**
  Collect every "reg" range of /reserved-memory children carrying a "no-map"
  property (skipping status = "disabled"), using the reserved-memory node's
  own cells (fallback: root cells). Mirrors UpdateDeviceTree.c GetNoMapRegions.

  @retval EFI_NOT_FOUND         No /reserved-memory node.
  @retval EFI_UNSUPPORTED       Cells outside 1..2.
  @retval EFI_OUT_OF_RESOURCES  More than Max regions.
**/
EFI_STATUS
MlbFdtGetNoMapRegions (
  IN  CONST VOID      *Fdt,
  OUT MLB_MEM_REGION  *Regs,
  IN  UINT32           Max,
  OUT UINT32          *Count
  );

/** Big-endian cell (un)packing helpers used for "reg" properties. */
UINT64
MlbReadCells (
  IN CONST UINT32  *P,
  IN UINT32         Cells
  );

VOID
MlbWriteCells (
  OUT UINT32  *P,
  IN  UINT32   Cells,
  IN  UINT64   V
  );

#endif /* MLB_CORE_NO_FDT */

/* ---- initrd magic ------------------------------------------------------- */
typedef enum {
  MlbInitrdUnknown = 0,
  MlbInitrdCpioNewc,      /* 070701 / 070702 */
  MlbInitrdCpioOdc,       /* 070707 */
  MlbInitrdGzip,
  MlbInitrdZstd,
  MlbInitrdXz,
  MlbInitrdLz4,
  MlbInitrdBzip2,
  MlbInitrdLzma,
  MlbInitrdLzo
} MLB_INITRD_KIND;

MLB_INITRD_KIND
MlbDetectInitrdKind (
  IN CONST VOID  *Buffer,
  IN UINTN        Size
  );

#endif /* __MAINLINE_BOOT_CORE_H__ */
