/** @file TestBootFastboot.h

  Minimal, self-contained USB fastboot implementation for TestBootApp.
  Supports getvar / download / flash:{bootcfg,kernel,dtb,initrd} (RAM only,
  nothing is written to storage) / continue / boot / reboot /
  reboot-bootloader / oem help.

  SPDX-License-Identifier: BSD-3-Clause-Clear
**/

#ifndef __TESTBOOT_FASTBOOT_H__
#define __TESTBOOT_FASTBOOT_H__

#include <Uefi.h>

/**
  Bring up the USB device controller with fastboot descriptors and allocate
  the command and download buffers. Also disables the watchdog.
**/
EFI_STATUS
FastbootStart (
  VOID
  );

/**
  Poll the USB device protocol once and dispatch whatever it reports.
  Call in a tight loop. Returns EFI_ABORTED for a cancelled transfer
  (harmless, keep looping).
**/
EFI_STATUS
FastbootHandleEvents (
  VOID
  );

/** TRUE once "continue" (or a successful "boot") has been acknowledged. */
BOOLEAN
FastbootContinueRequested (
  VOID
  );

/** Free the transfer buffers and stop the USB controller. */
EFI_STATUS
FastbootStop (
  VOID
  );

#endif /* __TESTBOOT_FASTBOOT_H__ */
