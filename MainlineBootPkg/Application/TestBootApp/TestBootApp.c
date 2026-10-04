/** @file TestBootApp.c

  TestBootApp: fastboot "flashless boot" front-end for MainlineBootLib.

    banner -> MainlineBootInit -> fastboot loop -> (continue) ->
    FastbootStop -> MainlineBootBoot (never returns on success)

  If MainlineBootBoot() fails, the error is printed and fastboot is
  restarted so the images can be re-flashed; if the USB stack cannot be
  restarted the app dead-loops (it is the ABL payload, there is nothing to
  return to).

  SPDX-License-Identifier: BSD-3-Clause-Clear
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/MainlineBootLib.h>

#include "TestBootFastboot.h"

STATIC VOID
PrintInfo (
  VOID
  )
{
  MAINLINE_BOOT_INFO  Info;
  UINT64              K, E, R, D;
  UINT32              i;

  if (EFI_ERROR (MainlineBootGetInfo (&Info))) {
    return;
  }
  DEBUG ((EFI_D_INFO, "TestBootApp: state: init=%a kernel=%a(%lu) dtb=%a(%lu) "
          "initrd=%a(%lu) cmdline=%a\n",
          Info.Initialized ? "ok" : "FAILED",
          Info.KernelLoaded ? "yes" : "no", (UINT64)Info.KernelFileSize,
          Info.DtbLoaded ? "yes" : "no", (UINT64)Info.DtbSize,
          Info.InitrdLoaded ? "yes" : "no", (UINT64)Info.InitrdSize,
          Info.CmdLineLoaded ? "yes" : "no"));
  if (Info.CmdLineLoaded && Info.CmdLine != NULL) {
    DEBUG ((EFI_D_INFO, "TestBootApp: cmdline: %a\n", Info.CmdLine));
  }
  for (i = 0; i < Info.NumRamPartitions; i++) {
    DEBUG ((EFI_D_INFO, "TestBootApp: RAM[%u] 0x%016lx + 0x%016lx\n", i,
            Info.RamPartitions[i].Base, Info.RamPartitions[i].Length));
  }
  if (!EFI_ERROR (MainlineBootGetLayout (&K, &E, &R, &D))) {
    DEBUG ((EFI_D_INFO, "TestBootApp: layout kernel 0x%lx dtb 0x%lx initrd 0x%lx "
            "end 0x%lx\n", K, D, R, E));
  }
}

EFI_STATUS
EFIAPI
TestBootAppEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  UINTN       Round = 0;

  DEBUG ((EFI_D_ERROR, "\n"));
  DEBUG ((EFI_D_ERROR, "==============================================\n"));
  DEBUG ((EFI_D_ERROR, " MainlineBoot TestBootApp  (built %a %a)\n", __DATE__, __TIME__));
  DEBUG ((EFI_D_ERROR, " ABL-style mainline Linux flashless boot\n"));
  DEBUG ((EFI_D_ERROR, "==============================================\n"));

  Status = MainlineBootInit ();
  if (EFI_ERROR (Status)) {
    /* keep going: fastboot still lets us inspect the device */
    DEBUG ((EFI_D_ERROR, "TestBootApp: MainlineBootInit failed: %r "
            "(fastboot still available, boot will fail)\n", Status));
  }

  for (;;) {
    Round++;
    Status = FastbootStart ();
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "TestBootApp: cannot start fastboot USB: %r\n", Status));
      if (Round == 1) {
        /* first attempt: give control back (useful when run from a shell) */
        return Status;
      }
      DEBUG ((EFI_D_ERROR, "TestBootApp: halting\n"));
      CpuDeadLoop ();
    }

    DEBUG ((EFI_D_ERROR, "TestBootApp: waiting for fastboot commands "
            "(flash bootcfg/kernel/dtb/initrd, then continue)\n"));

    while (!FastbootContinueRequested ()) {
      Status = FastbootHandleEvents ();
      if (EFI_ERROR (Status) && Status != EFI_ABORTED) {
        DEBUG ((EFI_D_ERROR, "TestBootApp: USB event error %r, continuing\n", Status));
      }
    }

    DEBUG ((EFI_D_ERROR, "TestBootApp: 'continue' received, stopping USB\n"));
    FastbootStop ();
    PrintInfo ();

    Status = MainlineBootBoot ();
    /* only reached on failure */
    DEBUG ((EFI_D_ERROR, "TestBootApp: MainlineBootBoot failed: %r; "
            "restarting fastboot\n", Status));
  }
}
