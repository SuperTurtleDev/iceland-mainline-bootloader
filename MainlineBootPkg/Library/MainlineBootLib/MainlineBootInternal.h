/** @file MainlineBootInternal.h

  Internal state shared between the MainlineBootLib translation units.

  SPDX-License-Identifier: BSD-3-Clause-Clear
**/

#ifndef __MAINLINE_BOOT_INTERNAL_H__
#define __MAINLINE_BOOT_INTERNAL_H__

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/EFIRamPartition.h>

#include <Library/MainlineBootLib.h>
#include "MainlineBootCore.h"

/* Headroom kept free inside the 2 MB DT window for our own additions
 * (memory reg, bootargs, initrd, rng-seed ...). */
#define MLB_DT_HEADROOM   (64 * 1024)

typedef struct {
  BOOLEAN               Initialized;

  /* ---- platform (MainlineBootInit) ---- */
  RamPartitionEntry    *RamPartitions;      /* AllocateZeroPool'd, NumRamPartitions entries */
  UINT32                NumRamPartitions;
  UINT64                BaseMemory;
  UINT64                KernelBaseAddr;
  UINT64                KernelSizeReserved;
  BOOLEAN               KernelParamsFromUefiVars;

  /* ---- images ---- */
  VOID                 *Kernel;
  UINTN                 KernelSize;
  MLB_KERNEL_HDR_INFO   KernelHdr;

  VOID                 *Dtb;
  UINTN                 DtbSize;

  VOID                 *Initrd;
  UINTN                 InitrdSize;

  CHAR8                *CmdLine;           /* NUL terminated, may be "" */
  BOOLEAN               CmdLineLoaded;

  /* mirror for MainlineBootGetInfo (public struct type) */
  MAINLINE_BOOT_RAM_PARTITION *PubRamPartitions;
} MLB_STATE;

extern MLB_STATE  gMlb;

/* MainlineBootFdt.c */
EFI_STATUS
MlbUpdateDeviceTree (
  IN VOID          *Fdt,
  IN UINTN          FdtBufferSize,
  IN CONST CHAR8   *CmdLine        OPTIONAL,
  IN UINT64         InitrdAddr,
  IN UINT64         InitrdSize
  );

/* MainlineBootJump.c */
EFI_STATUS
MlbShutdownUefiBootServices (
  VOID
  );

VOID
MlbPreparePlatformHardware (
  VOID
  );

VOID
MlbJumpToKernel (
  IN UINT64  KernelEntry,
  IN UINT64  DtbAddr
  );

#endif /* __MAINLINE_BOOT_INTERNAL_H__ */
