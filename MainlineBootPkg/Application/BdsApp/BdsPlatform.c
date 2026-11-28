/** @file
  Canoe platform initialization for the UEFI BDS application.

  The FAT container holds MEMCFG and platform configuration. Secure Boot keys
  live in authenticated UEFI variables. Installed drivers and verification/memory
  callbacks refer into this image, which never returns to its ABL caller.

  Copyright (c) 2026, contributors to the canoe ABL tree.
  SPDX-License-Identifier: BSD-3-Clause
**/

#include "BdsApp.h"
#include "BdsMenu.h"
#include "BdsSetup.h"

#include <FastbootLib/FastbootMain.h>
#include <Guid/GlobalVariable.h>
#include <Library/DebugLib.h>
#include <Library/PartitionTableUpdate.h>
#include <Library/SfbConfigStore.h>
#include <Library/SfbMemFixup.h>
#include <Library/BdsSecurityLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/SimpleTextIn.h>

STATIC BOOLEAN  mInitialized;
STATIC BOOLEAN  mSetupRequested;

STATIC
BOOLEAN
BdsWaitForVolumeUp (
  VOID
  )
{
  EFI_STATUS     Status;
  EFI_EVENT      Timer;
  EFI_EVENT      Events[2];
  EFI_INPUT_KEY  Key;
  UINTN          Index;
  BOOLEAN        Detected;

  if ((gST == NULL) || (gST->ConIn == NULL) || (gST->ConIn->WaitForKey == NULL)) {
    return FALSE;
  }

  gST->ConIn->Reset (gST->ConIn, FALSE);
  Status = gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, NULL, NULL, &Timer);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  Detected = FALSE;
  Status   = gBS->SetTimer (Timer, TimerRelative, 10000000);
  if (!EFI_ERROR (Status)) {
    // Timer first: a continuously held non-target key cannot extend the window.
    Events[0] = Timer;
    Events[1] = gST->ConIn->WaitForKey;
    while (TRUE) {
      Status = gBS->WaitForEvent (ARRAY_SIZE (Events), Events, &Index);
      if (EFI_ERROR (Status) || (Index == 0)) {
        break;
      }

      Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
      if (!EFI_ERROR (Status) && (Key.ScanCode == SCAN_UP)) {
        Detected = TRUE;
        break;
      }

      // Power, volume down and unrelated input do not end the one-second scan.
    }
  }

  gBS->CloseEvent (Timer);
  return Detected;
}

STATIC
EFI_STATUS
BdsConsumeOsIndications (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT64      Indications;
  UINTN       Size;

  Size   = sizeof (Indications);
  Status = gRT->GetVariable (
                  L"OsIndications",
                  &gEfiGlobalVariableGuid,
                  NULL,
                  &Size,
                  &Indications
                  );
  if (Status == EFI_NOT_FOUND) {
    return EFI_SUCCESS;
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Size != sizeof (Indications)) {
    return EFI_COMPROMISED_DATA;
  }

  if ((Indications & EFI_OS_INDICATIONS_BOOT_TO_FW_UI) == 0) {
    return EFI_SUCCESS;
  }

  BdsRequestMenu ();
  mSetupRequested = TRUE;
  Indications &= ~((UINT64)EFI_OS_INDICATIONS_BOOT_TO_FW_UI);
  return gRT->SetVariable (
                L"OsIndications",
                &gEfiGlobalVariableGuid,
                EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                EFI_VARIABLE_RUNTIME_ACCESS,
                sizeof (Indications),
                &Indications
                );
}

EFI_STATUS
PlatformBdsInit (
  VOID
  )
{
  EFI_STATUS       Status;
  BDS_MENU_ACTION  Action;

  if (!mInitialized) {
    Status = BdsSecurityInitialize ();
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "BDS: image authentication hooks unavailable: %r\n", Status));
      BdsRequestMenu ();
    }

    if (BdsWaitForVolumeUp ()) {
      BdsRequestMenu ();
    }

    Status = BdsConsumeOsIndications ();
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "BDS: cannot consume OsIndications: %r\n", Status));
    }

    Status = SfbMemFixupInstall ();
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "BDS: memory fix-ups incomplete: %r\n", Status));
    }

    Status = EnumeratePartitions ();
    if (!EFI_ERROR (Status)) {
      UpdatePartitionEntries ();
    } else {
      DEBUG ((DEBUG_ERROR, "BDS: partition enumeration failed: %r\n", Status));
    }

    Status = SfbStartFatStack ();
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "BDS: FAT support initialization failed: %r\n", Status));
    }

    Status = SfbConfigStoreMount ();
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "BDS: FAT configuration container unavailable: %r\n", Status));
    }

    Status = SfbMemFixupLoadConfig ();
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "BDS: MEMCFG load failed: %r\n", Status));
    }

    Status = BdsDiscoverEspBootOptions ();
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "BDS: standard boot discovery failed: %r\n", Status));
    }

    mInitialized = TRUE;
  }

  if (!BdsMenuRequested ()) {
    return EFI_SUCCESS;
  }

  if (!BdsConsoleAvailable ()) {
    return EFI_NOT_READY;
  }

  if (!mSetupRequested) {
    SfbShowEnteringMenu ();
  }

  while (TRUE) {
    if (mSetupRequested) {
      mSetupRequested = FALSE;
      if (BdsRunSetup ()) {
        Action = BdsMenuFastboot;
      } else {
        SfbShowEnteringMenu ();
        continue;
      }
    } else {
      Action = BdsRunBootMenu ();
    }

    if (Action == BdsMenuContinue) {
      return EFI_SUCCESS;
    }

    if (Action == BdsMenuUnavailable) {
      return EFI_NOT_READY;
    }

    SfbShowFastbootMode ();
    SfbConfigStoreUnmount ();
    Status = FastbootInitialize ();
    if (EFI_ERROR (Status)) {
      SfbReportStatus (L"Fastboot returned", Status);
    }

    // The host may have rewritten the container. Boot variables stay separate.
    Status = SfbConfigStoreMount ();
    if (EFI_ERROR (Status)) {
      SfbReportStatus (L"FAT configuration container unavailable", Status);
    }
  }
}
