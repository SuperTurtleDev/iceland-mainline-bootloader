/** @file MainlineBootLib.h

  ABL-style (non EFI-stub) mainline Linux loader.

  Usage:
    MainlineBootInit ();                 // RAM partitions, KernelBaseAddr/Size
    MainlineBootLoadKernel (buf, size);  // arm64 "Image" (uncompressed)
    MainlineBootLoadDtb    (buf, size);  // flattened device tree blob
    MainlineBootLoadInitrd (buf, size);  // optional cpio (newc) / compressed
    MainlineBootLoadBootcfg(buf, size);  // optional "cmdline=..." text file
    MainlineBootBoot ();                 // ExitBootServices + jump, no return

  All Load* functions copy the caller's buffer into library-owned memory, so
  the caller may reuse/free its buffer afterwards. Calling a Load* function
  again replaces the previously loaded item.

  SPDX-License-Identifier: BSD-3-Clause-Clear
**/

#ifndef __MAINLINE_BOOT_LIB_H__
#define __MAINLINE_BOOT_LIB_H__

#include <Uefi.h>

/**
  Reset library state and gather platform boot parameters the same way ABL
  does: EFI_RAMPARTITION_PROTOCOL (RAM partitions from TZ/XBL), BaseMemory
  (lowest partition base) and the L"KernelBaseAddr"/L"KernelSize" UEFI
  variables (gQcomTokenSpaceGuid), falling back to BaseMemory / 0x05600000
  (PcdRamdiskEndAddress) when the variables are absent.

  @retval EFI_SUCCESS     Ready to accept images.
  @retval EFI_NOT_FOUND   EFI_RAMPARTITION_PROTOCOL not available.
**/
EFI_STATUS
EFIAPI
MainlineBootInit (
  VOID
  );

/**
  Load a flattened device tree. The blob must pass fdt_check_header() and
  fdt_totalsize() must fit in Size and in the 2 MB DT window (minus headroom).
**/
EFI_STATUS
EFIAPI
MainlineBootLoadDtb (
  IN VOID   *Buffer,
  IN UINTN  Size
  );

/**
  Load an uncompressed arm64 Linux "Image" (magic "ARM\x64" at offset 56).
  gzip-compressed images (Image.gz) are rejected with EFI_UNSUPPORTED.
**/
EFI_STATUS
EFIAPI
MainlineBootLoadKernel (
  IN VOID   *Buffer,
  IN UINTN  Size
  );

/**
  Load a text boot configuration. Lines are split on '\n' / "\r\n"; empty
  lines and lines starting with '#' are ignored; the value of the last
  "cmdline=" line becomes the kernel command line (max 4095 chars).

  Additional keys (applied even without a "cmdline=" line):
    kernel-base=<val>   boot window base address for this boot
    kernel-size=<val>   boot window size for this boot
  where <val> is 0x-prefixed hex, decimal, or decimal/mixed with a k/K, m/M
  (MiB) or g/G (GiB) suffix, e.g. "0x80000000" or "768M".  Priority:
  bootcfg keys > L"KernelBaseAddr"/L"KernelSize" UEFI variables > the ABL
  fallback (BaseMemory / 0x05600000).  A bootcfg without these keys resets
  the window to the platform default.

  @retval EFI_NOT_FOUND  No "cmdline=" line present.
**/
EFI_STATUS
EFIAPI
MainlineBootLoadBootcfg (
  IN VOID   *Buffer,
  IN UINTN  Size
  );

/**
  Load an initramfs. Accepted: cpio newc/crc ("070701"/"070702"), odc
  ("070707"), or gzip/zstd/xz/lz4/bzip2/lzma/lzo compressed archives (the
  kernel decompresses those itself; a warning is logged). Size == 0 removes a
  previously loaded initrd. Unknown formats return EFI_UNSUPPORTED.
**/
EFI_STATUS
EFIAPI
MainlineBootLoadInitrd (
  IN VOID   *Buffer,
  IN UINTN  Size
  );

/**
  Place kernel/initrd/DTB using the ABL UpdateBootParams layout, patch the
  DTB (/memory from RAM partitions minus no-map carveouts, /chosen bootargs,
  linux,initrd-start/end, kaslr-seed, rng-seed), ExitBootServices, disable
  caches/MMU and jump to the kernel with x0 = DTB.

  Does not return on success.

  @retval EFI_NOT_READY         Kernel or DTB not loaded / Init not called.
  @retval EFI_BUFFER_TOO_SMALL  Kernel region too small for the layout.
  @retval other                 DT update or ExitBootServices failure.
**/
EFI_STATUS
EFIAPI
MainlineBootBoot (
  VOID
  );

/* ------------------------------------------------------------------------
 * Optional read-only queries (for the front-end to print status)
 * ---------------------------------------------------------------------- */

typedef struct {
  UINT64  Base;
  UINT64  Length;
} MAINLINE_BOOT_RAM_PARTITION;

typedef struct {
  BOOLEAN   Initialized;
  UINT64    BaseMemory;
  UINT64    KernelBaseAddr;
  UINT64    KernelSizeReserved;
  BOOLEAN   KernelParamsFromUefiVars;
  BOOLEAN   KernelParamsFromCfg;   /* bootcfg kernel-base=/kernel-size= won */

  BOOLEAN   KernelLoaded;
  UINTN     KernelFileSize;
  UINT64    KernelTextOffset;
  UINT64    KernelImageSize;
  UINT64    KernelFlags;

  BOOLEAN   DtbLoaded;
  UINTN     DtbSize;

  BOOLEAN   InitrdLoaded;
  UINTN     InitrdSize;

  BOOLEAN   CmdLineLoaded;
  CONST CHAR8 *CmdLine;      // NULL if not loaded; may be "" (empty)

  UINT32    NumRamPartitions;
  CONST MAINLINE_BOOT_RAM_PARTITION *RamPartitions;
} MAINLINE_BOOT_INFO;

/**
  Fill *Info with the current library state. Pointers inside remain valid
  until the next Init/Load call.
**/
EFI_STATUS
EFIAPI
MainlineBootGetInfo (
  OUT MAINLINE_BOOT_INFO  *Info
  );

/**
  Compute (without booting) the load addresses that MainlineBootBoot() would
  use with the currently loaded images. Useful for "dry run" printing.

  @retval EFI_NOT_READY         Init not done or kernel not loaded.
  @retval EFI_BUFFER_TOO_SMALL  Layout does not fit the kernel region.
**/
EFI_STATUS
EFIAPI
MainlineBootGetLayout (
  OUT UINT64  *KernelLoadAddr,
  OUT UINT64  *KernelEndAddr,
  OUT UINT64  *RamdiskLoadAddr,
  OUT UINT64  *DeviceTreeLoadAddr
  );

#endif /* __MAINLINE_BOOT_LIB_H__ */
