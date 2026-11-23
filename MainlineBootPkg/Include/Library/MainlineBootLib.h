/** @file MainlineBootLib.h

  ABL-style image loading with a Linux EFI stub kernel handoff.

  Usage:
    MainlineBootInit ();                 // RAM partitions, KernelBaseAddr/Size
    MainlineBootLoadKernel (buf, size);  // arm64 "Image" with CONFIG_EFI_STUB
    MainlineBootLoadDtb    (buf, size);  // flattened device tree blob
    MainlineBootLoadInitrd (buf, size);  // optional cpio (newc) / compressed
    MainlineBootLoadBootcfg(buf, size);  // optional "cmdline=..." text file
    MainlineBootBoot ();                 // config table + EFISTUB, no return

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
  Hand the kernel to its EFI stub. The patched DTB (memory map from RAM
  partitions, /chosen bootargs / kaslr-seed / rng-seed) is published in the
  EFI configuration table (gFdtTableGuid), the initrd through a LoadFile2
  protocol on the LINUX_EFI_INITRD_MEDIA_GUID device path, the bootcfg
  cmdline as the kernel image LoadOptions; the kernel Image is then started
  with LoadImage()/StartImage(). The stub does its own kernel relocation,
  initrd load, FDT rebuild, ExitBootServices, cache/MMU quiesce and jump.

  The ABL UpdateBootParams window (KernelBaseAddr/KernelSize) no longer
  receives any image - the stub allocates kernel/initrd/FDT itself - and is
  reported by MainlineBootGetLayout() for diagnostics only.

  The display is blacked out and every EFI_GRAPHICS_OUTPUT_PROTOCOL instance
  is uninstalled right before StartImage(), so the stub finds no display and
  the kernel starts without efifb, exactly like the bare-jump path did; msm
  DRM initialises the panel from scratch. Nothing is printed to the panel
  after that point.

  Does not return on success.

  @retval EFI_NOT_READY         Kernel or DTB not loaded / Init not called.
  @retval EFI_UNSUPPORTED       Kernel Image has no EFI stub (no PE/COFF
                                ARM64 header, i.e. CONFIG_EFI_STUB not built).
  @retval other                 DT update, protocol/table install, or the
                                kernel EFI stub itself failed (any return
                                from the stub is a failure).
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
  Compute (without booting) the ABL UpdateBootParams window layout for the
  currently loaded images. MainlineBootBoot() no longer places anything in
  that window (the kernel EFI stub allocates itself), so this is diagnostics
  only - BootUtil.efi uses it to report the legacy boot window.

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
