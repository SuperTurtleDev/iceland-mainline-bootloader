/** @file BootUtil.c

  BOOTUTIL.EFI - on-screen probe of the boot environment's initrd capacity.

  Run it from the boot menu (or any UEFI shell) and read the screen:

    1. raw presence/values of the L"KernelBaseAddr" / L"KernelSize" UEFI
       variables (gQcomTokenSpaceGuid) - the same probe MainlineBootInit()
       performs through ABL's QueryBootParams()
    2. the ABL boot window MainlineBootLib derived from them (or the
       0x05600000 = 86 MiB fallback when they are absent)
    3. THE NUMBER: the largest initrd MainlineBootBoot() can place, i.e.
       window - kernel load offset - DTB window (2 MiB) - slack, before any
       kernel is loaded.  A loaded kernel eats into the same window, so the
       real capacity at boot time is this minus the kernel image size.

  SPDX-License-Identifier: BSD-3-Clause-Clear
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MainlineBootLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
/* gQcomTokenSpaceGuid comes from QcomModulePkg.dec via AutoGen */

/* Mirrors MainlineBootCore.h (kept private to the library) */
#define BU_PAGE_SIZE        4096
#define BU_DT_SIZE_2MB      (2 * 1024 * 1024)
#define BU_KERNEL_LOAD_OFF  0x80000    /* KERNEL_64BIT_LOAD_OFFSET, no Image */

STATIC
VOID
BuPrintMiB (
  IN CONST CHAR16  *What,
  IN UINT64        Bytes
  )
{
  Print (L"  %-22s 0x%016lx  %8lu.%03lu MiB\r\n",
         What, Bytes,
         (UINT64)(Bytes >> 20), ((Bytes & ((1 << 20) - 1)) * 1000) >> 20);
}

/* Probe one UINT64 runtime variable the way QueryBootParams() does. */
STATIC
BOOLEAN
BuProbeVar (
  IN  CONST CHAR16  *Name,
  OUT UINT64        *Value
  )
{
  EFI_STATUS  Status;
  UINTN       DataSize = sizeof (*Value);

  Status = gRT->GetVariable ((CHAR16 *)Name, &gQcomTokenSpaceGuid,
                             NULL, &DataSize, Value);
  if (!EFI_ERROR (Status) && DataSize == sizeof (*Value)) {
    Print (L"  %-22s PRESENT = 0x%016lx\r\n", Name, *Value);
    return TRUE;
  }
  Print (L"  %-22s MISSING (%r)\r\n", Name, Status);
  return FALSE;
}

/* Wait up to TimeoutMs for any console key so the numbers stay readable. */
STATIC
VOID
BuWaitKey (
  IN UINTN  TimeoutMs
  )
{
  EFI_INPUT_KEY  Key;
  UINTN          i;

  if (gST->ConIn == NULL) {
    gBS->Stall (TimeoutMs * 1000);
    return;
  }
  gST->ConIn->Reset (gST->ConIn, FALSE);
  for (i = 0; i < TimeoutMs / 50; i++) {
    if (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key) == EFI_SUCCESS) {
      return;
    }
    gBS->Stall (50 * 1000);
  }
}

EFI_STATUS
EFIAPI
BootUtilEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS           Status;
  MAINLINE_BOOT_INFO   Info;
  UINT64               VarBase, VarSize;
  BOOLEAN              HaveBase, HaveSize;
  UINT64               Span, InitrdMax;
  UINT32               i;
  UINT64               RamTotal = 0;

  HaveBase = BuProbeVar (L"KernelBaseAddr", &VarBase);
  HaveSize = BuProbeVar (L"KernelSize", &VarSize);

  Status = MainlineBootInit ();
  if (EFI_ERROR (Status)) {
    Print (L"\r\n[BOOTUTIL] MainlineBootInit failed: %r "
           L"(EFI_RAMPARTITION_PROTOCOL unavailable)\r\n", Status);
    BuWaitKey (15000);
    return Status;
  }
  Status = MainlineBootGetInfo (&Info);
  if (EFI_ERROR (Status)) {
    Print (L"\r\n[BOOTUTIL] MainlineBootGetInfo failed: %r\r\n", Status);
    BuWaitKey (15000);
    return Status;
  }

  Print (L"\r\n[BOOTUTIL] RAM partitions: %u\r\n", Info.NumRamPartitions);
  for (i = 0; i < Info.NumRamPartitions; i++) {
    Print (L"  [%u] base 0x%016lx length 0x%016lx\r\n",
           i, Info.RamPartitions[i].Base, Info.RamPartitions[i].Length);
    RamTotal += Info.RamPartitions[i].Length;
  }
  BuPrintMiB (L"RAM available total", RamTotal);
  BuPrintMiB (L"BaseMemory", Info.BaseMemory);

  Print (L"\r\n[BOOTUTIL] ABL boot window\r\n");
  BuPrintMiB (L"KernelBaseAddr", Info.KernelBaseAddr);
  BuPrintMiB (L"KernelSizeReserved", Info.KernelSizeReserved);
  Print (L"  source: %s\r\n",
         Info.KernelParamsFromCfg
           ? L"bootcfg override (kernel-base=/kernel-size=)"
           : Info.KernelParamsFromUefiVars
               ? L"UEFI variables (KernelBaseAddr/KernelSize)"
               : L"FALLBACK - variables MISSING, ABL default 0x05600000 (86 MiB)");
  Print (L"  (window overridable per boot via the bootcfg partition:\r\n"
         L"   kernel-base=<0x...|decimal|NNNM> / kernel-size=<...>)\r\n");

  if (!HaveBase || !HaveSize) {
    Print (L"\r\n[BOOTUTIL] NOTE: variables missing -> initrd capacity below"
           L"\r\n  is the tiny ABL fallback window, not the physical RAM.\r\n");
  }

  /* MlbComputeLayout() with no kernel loaded:
   *   KernelLoadAddr  = KernelBaseAddr + 0x80000
   *   RamdiskLoadAddr = KernelEndAddr  - (round_up(initrd, 4K) + 4K)
   *   DeviceTreeLoad  = RamdiskLoad    - (2 MiB + 4K), must stay above the
   *                     kernel -> initrd_max = span - 2 MiB - 12 KiB.       */
  Span = Info.KernelSizeReserved > BU_KERNEL_LOAD_OFF
           ? Info.KernelSizeReserved - BU_KERNEL_LOAD_OFF
           : 0;
  InitrdMax = Span > (BU_DT_SIZE_2MB + 12 * BU_PAGE_SIZE)
                ? Span - (BU_DT_SIZE_2MB + 12 * BU_PAGE_SIZE)
                : 0;
  InitrdMax &= ~(UINT64)(BU_PAGE_SIZE - 1);

  Print (L"\r\n=====================================================\r\n");
  Print (L" MAX INITRD CAPACITY (kernel not yet loaded):\r\n");
  Print (L"   %lu bytes = 0x%lx = %lu.%03lu MiB\r\n",
         InitrdMax, InitrdMax,
         (UINT64)(InitrdMax >> 20), ((InitrdMax & ((1 << 20) - 1)) * 1000) >> 20);
  Print (L" A loaded kernel reduces this by its image size.\r\n");
  Print (L"=====================================================\r\n");

  Print (L"\r\n[BOOTUTIL] press any key (auto-exit in 60s)...\r\n");
  BuWaitKey (60000);
  return EFI_SUCCESS;
}
