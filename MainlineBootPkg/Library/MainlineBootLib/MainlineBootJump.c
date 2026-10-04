/** @file MainlineBootJump.c

  ExitBootServices + cache/MMU quiesce + jump. Copied from
  QcomModulePkg/Library/BootLib/ShutdownServices.c (ShutdownUefiBootServices
  and the DISABLE_KERNEL_PROTOCOL flavour of PreparePlatformHardware) and the
  tail of BootLinux(). The WaitForFlashFinished() call is dropped (no
  parallel flashing here).

  IMPORTANT: nothing after MlbShutdownUefiBootServices() succeeds may call a
  boot service, allocate memory or DEBUG().

  SPDX-License-Identifier: BSD-3-Clause-Clear
**/

#include "MainlineBootInternal.h"
#include <Library/ArmLib.h>

typedef VOID (*LINUX_KERNEL) (UINT64 ParametersBase,
                              UINT64 Reserved0,
                              UINT64 Reserved1,
                              UINT64 Reserved2);

EFI_STATUS
MlbShutdownUefiBootServices (
  VOID
  )
{
  EFI_STATUS             Status;
  UINTN                  MemoryMapSize;
  EFI_MEMORY_DESCRIPTOR *MemoryMap;
  UINTN                  MapKey;
  UINTN                  DescriptorSize;
  UINT32                 DescriptorVersion;
  UINTN                  Pages;

  MemoryMap = NULL;
  MemoryMapSize = 0;
  Pages = 0;

  do {
    Status = gBS->GetMemoryMap (&MemoryMapSize, MemoryMap, &MapKey,
                                &DescriptorSize, &DescriptorVersion);
    if (Status == EFI_BUFFER_TOO_SMALL) {

      Pages = EFI_SIZE_TO_PAGES (MemoryMapSize) + 1;
      MemoryMap = AllocatePages (Pages);
      if (!MemoryMap) {
        DEBUG ((EFI_D_ERROR, "Failed to allocate pages for memory map\n"));
        return EFI_OUT_OF_RESOURCES;
      }

      //
      // Get System MemoryMap
      //
      Status = gBS->GetMemoryMap (&MemoryMapSize, MemoryMap, &MapKey,
                                  &DescriptorSize, &DescriptorVersion);
    }

    // Don't do anything between the GetMemoryMap() and ExitBootServices()
    if (!EFI_ERROR (Status)) {
      Status = gBS->ExitBootServices (gImageHandle, MapKey);
      if (EFI_ERROR (Status)) {
        FreePages (MemoryMap, Pages);
        MemoryMap = NULL;
        MemoryMapSize = 0;
      }
    }
  } while (EFI_ERROR (Status));

  return Status;
}

VOID
MlbPreparePlatformHardware (
  VOID
  )
{
  ArmDisableBranchPrediction ();

  /* ArmDisableAllExceptions */
  ArmDisableInterrupts ();
  ArmDisableAsynchronousAbort ();

  ArmCleanInvalidateDataCache ();
  ArmCleanDataCache ();
  ArmInvalidateInstructionCache ();

  ArmDisableDataCache ();
  ArmDisableInstructionCache ();
  ArmDisableMmu ();
  ArmInvalidateTlb ();
}

VOID
MlbJumpToKernel (
  IN UINT64  KernelEntry,
  IN UINT64  DtbAddr
  )
{
  LINUX_KERNEL  LinuxKernel;

  /* arm64 boot protocol: x0 = DTB physical address, x1..x3 = 0 */
  LinuxKernel = (LINUX_KERNEL)(UINTN)KernelEntry;
  LinuxKernel (DtbAddr, 0, 0, 0);

  /* Kernel should never return */
  CpuDeadLoop ();
}
