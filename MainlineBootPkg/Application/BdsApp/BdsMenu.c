/*
 * Console UI adapted from Canoe for the UEFI Boot Menu.
 *
 * Three keys drive everything: volume up and volume down move the cursor, and
 * power confirms.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "BdsMenu.h"
#include "BdsApp.h"
#include "BdsEntries.h"
#include "BdsSetup.h"

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/SimpleTextIn.h>

#define SFB_ATTR_NORMAL    EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK)
#define SFB_ATTR_SELECTED  EFI_TEXT_ATTR (EFI_BLACK, EFI_LIGHTGRAY)
#define SFB_ATTR_TITLE     EFI_TEXT_ATTR (EFI_WHITE, EFI_BLACK)

BOOLEAN
BdsConsoleAvailable (
  VOID
  )
{
  return (BOOLEAN)((gST != NULL) && (gST->ConIn != NULL) &&
                   (gST->ConOut != NULL) && (gST->ConIn->WaitForKey != NULL));
}

SFB_KEY
SfbWaitForKey (
  IN UINT32  TimeoutMs
  )
{
  EFI_STATUS     Status;
  EFI_EVENT      TimerEvent = NULL;
  EFI_EVENT      WaitList[2];
  UINTN          WaitCount;
  UINTN          EventIndex;
  EFI_INPUT_KEY  Key;
  SFB_KEY        Result = SfbKeyTimeout;

  if (!BdsConsoleAvailable ()) {
    return SfbKeyUnavailable;
  }

  if (TimeoutMs != 0) {
    Status = gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, NULL, NULL, &TimerEvent);
    if (EFI_ERROR (Status)) {
      return SfbKeyUnavailable;
    }

    Status = gBS->SetTimer (TimerEvent, TimerRelative, (UINT64)TimeoutMs * 10000);
    if (EFI_ERROR (Status)) {
      gBS->CloseEvent (TimerEvent);
      return SfbKeyUnavailable;
    }
  }

  WaitCount   = (TimerEvent == NULL) ? 1 : 2;
  WaitList[0] = (TimerEvent == NULL) ? gST->ConIn->WaitForKey : TimerEvent;
  WaitList[1] = gST->ConIn->WaitForKey;
  while (TRUE) {
    Status = gBS->WaitForEvent (WaitCount, WaitList, &EventIndex);
    if (EFI_ERROR (Status)) {
      Result = SfbKeyUnavailable;
      break;
    }

    if ((TimerEvent != NULL) && (EventIndex == 0)) {
      break;
    }

    Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
    if (EFI_ERROR (Status)) {
      if (Status != EFI_NOT_READY) {
        Result = SfbKeyUnavailable;
        break;
      }

      continue;
    }

    // Retain the handset's three-key mapping; power is platform-dependent.
    if (Key.ScanCode == SCAN_UP) {
      Result = SfbKeyUp;
    } else if (Key.ScanCode == SCAN_DOWN) {
      Result = SfbKeyDown;
    } else {
      Result = SfbKeySelect;
    }

    break;
  }

  if (TimerEvent != NULL) {
    gBS->CloseEvent (TimerEvent);
  }

  return Result;
}

/* ---- drawing ------------------------------------------------------------ */

VOID
SfbBeginScreen (
  IN CONST CHAR16  *Title,
  IN CONST CHAR16  *Subtitle
  )
{
  if (!BdsConsoleAvailable ()) {
    return;
  }

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_TITLE);
  gST->ConOut->ClearScreen (gST->ConOut);
  Print (L"%s\r\n", Title);
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
  if (Subtitle != NULL) {
    Print (L"%s\r\n", Subtitle);
  }

  Print (L"\r\n");
}

VOID
SfbEndScreen (
  IN CONST CHAR16  *Footer
  )
{
  if (!BdsConsoleAvailable ()) {
    return;
  }

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
  Print (L"\r\n%s\r\n", Footer);
}

VOID
SfbDrawRow (
  IN BOOLEAN       Selected,
  IN CONST CHAR16  *Marker,
  IN CONST CHAR16  *Text
  )
{
  if (!BdsConsoleAvailable ()) {
    return;
  }

  gST->ConOut->SetAttribute (
                 gST->ConOut,
                 Selected ? SFB_ATTR_SELECTED : SFB_ATTR_NORMAL
                 );
  Print (L"%s %s %s", Selected ? L">" : L" ", Marker, Text);
  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
  Print (L"\r\n");
}

/*
 * First row of the visible window, keeping the cursor inside it. Lists longer
 * than the window scroll rather than overflow the console.
 */
UINTN
SfbWindowStart (
  IN UINTN  Cursor,
  IN UINTN  Count,
  IN UINTN  Rows
  )
{
  if (Count <= Rows) {
    return 0;
  }

  if (Cursor < Rows / 2) {
    return 0;
  }

  if (Cursor > Count - 1 - (Rows - Rows / 2 - 1)) {
    return Count - Rows;
  }

  return Cursor - Rows / 2;
}

VOID
SfbMoveCursor (
  IN OUT UINTN  *Cursor,
  IN UINTN      Count,
  IN SFB_KEY    Key
  )
{
  if (Count == 0) {
    *Cursor = 0;
    return;
  }

  if (Key == SfbKeyUp) {
    *Cursor = (*Cursor == 0) ? Count - 1 : *Cursor - 1;
  } else if (Key == SfbKeyDown) {
    *Cursor = (*Cursor + 1 >= Count) ? 0 : *Cursor + 1;
  }
}

/* Report a failure and hold the screen until the user acknowledges it. */
VOID
SfbReportStatus (
  IN CONST CHAR16  *What,
  IN EFI_STATUS    Status
  )
{
  if (!BdsConsoleAvailable ()) {
    return;
  }

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
  Print (L"\r\n%s: %r\r\n", What, Status);
  Print (L"Press power to continue.\r\n");
  SfbWaitForKey (0);
}

/*
 * Hand the screen over to fastboot. The menu is the last thing that draws
 * before control leaves for the fastboot loop, which prints nothing of its own
 * until a host connects, so without this the user would be staring at a boot
 * menu that no longer responds to anything.
 */
VOID
SfbShowFastbootMode (
  VOID
  )
{
  if (!BdsConsoleAvailable ()) {
    return;
  }

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_TITLE);
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  Print (L"FASTBOOT MODE\r\n");

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
}

/*
 * Clear the menu away and announce the launch. The loaded image prints nothing
 * of its own until it takes over, so without this the boot menu would linger on
 * screen through the load.
 */
VOID
SfbShowBootingScreen (
  IN CONST CHAR16  *Name,
  IN BOOLEAN       ClearScreen
  )
{
  if (!BdsConsoleAvailable ()) {
    return;
  }

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_TITLE);

  /*
   * An unattended default boot must not blank whatever is already on screen
   * (typically the boot splash): only clear when the launch came from the menu,
   * where the menu itself is what needs clearing away.
   */
  if (ClearScreen) {
    gST->ConOut->ClearScreen (gST->ConOut);
  }

  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  Print (L"Booting %s\r\n", (Name != NULL && Name[0] != L'\0') ? Name : L"...");

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
}

/*
 * Announce a power action (Power Off / Restart) and leave the message on
 * screen while the reset takes effect. Neither action returns, so the screen is
 * the last thing the user sees.
 */
STATIC
VOID
SfbShowActionScreen (
  IN CONST CHAR16  *Text
  )
{
  if (!BdsConsoleAvailable ()) {
    return;
  }

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_TITLE);
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  Print (L"%s\r\n", Text);

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);
}

/*
 * Seconds to hold on the "Entering Boot Menu" screen before the menu starts
 * taking input. Long enough that a volume key held from power-on has been
 * released, so it does not immediately move the menu cursor.
 */
#define SFB_ENTER_MENU_DELAY_S  3

VOID
SfbShowEnteringMenu (
  VOID
  )
{
  if (!BdsConsoleAvailable ()) {
    return;
  }

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_TITLE);
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  Print (L"Entering Boot Menu\r\n");

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);

  /* Wait for the key to be released... */
  gBS->Stall (SFB_ENTER_MENU_DELAY_S * 1000 * 1000);

  /* ...then drop anything typed or held during the wait so it does not leak
   * into the menu as a spurious keypress. */
  gST->ConIn->Reset (gST->ConIn, FALSE);
}

/* ---- shared screens ------------------------------------------------------- */

/*
 * Seconds to hold on an "Entering <submenu>" screen before the submenu starts
 * taking input. Short of the root menu's three-second hold on purpose: the
 * parent's confirm keystroke is already consumed, this only covers a key still
 * held or its trailing repeat events.
 */
#define SFB_ENTERING_SCREEN_DELAY_S  1

VOID
SfbShowEnteringScreen (
  IN CONST CHAR16  *What
  )
{
  if (!BdsConsoleAvailable ()) {
    return;
  }

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_TITLE);
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  Print (L"Entering %s\r\n", (What != NULL) ? What : L"...");

  gST->ConOut->SetAttribute (gST->ConOut, SFB_ATTR_NORMAL);

  /* Wait for the key to be released, then drop anything typed or held during
   * the wait so it does not leak into the submenu as a spurious keypress. */
  gBS->Stall (SFB_ENTERING_SCREEN_DELAY_S * 1000 * 1000);
  gST->ConIn->Reset (gST->ConIn, FALSE);
}

/*
 * Debounce a menu exit: the key that confirmed Back would otherwise leak into
 * the parent menu (or its trailing repeats would) and act there immediately.
 * Hold for the entering-screen delay, then drop everything queued.
 */
VOID
SfbDebounceMenuExit (
  VOID
  )
{
  if (!BdsConsoleAvailable ()) {
    return;
  }

  gBS->Stall (SFB_ENTERING_SCREEN_DELAY_S * 1000 * 1000);
  gST->ConIn->Reset (gST->ConIn, FALSE);
}

// Standard load-option descriptions are flat; only visible boot selections
// are shown here. BIOS Setup contains the editable all-Boot#### inventory.
BDS_MENU_ACTION
BdsRunBootMenu (
  VOID
  )
{
  STATIC CONST CHAR16           *Actions[] = {
    L"BIOS Setup >",
    L"Continue Boot",
    L"Power Off",
    L"Restart"
  };
  EFI_BOOT_MANAGER_LOAD_OPTION  *Options;
  UINTN                         OptionCount;
  UINTN                         *Visible;
  UINTN                         VisibleCount;
  UINTN                         Count;
  UINTN                         Cursor;
  UINTN                         Index;
  UINTN                         Start;
  UINTN                         Last;
  UINTN                         Selected;
  SFB_KEY                       Key;
  EFI_STATUS                    Status;
  BDS_MENU_ACTION               Result;
  BOOLEAN                       Rebuild;
  CHAR16                        Text[160];

  if (!BdsConsoleAvailable ()) {
    return BdsMenuUnavailable;
  }

  Options      = NULL;
  Visible      = NULL;
  OptionCount  = 0;
  VisibleCount = 0;
  Cursor       = 0;
  Rebuild      = TRUE;
  Result       = BdsMenuUnavailable;
  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  while (TRUE) {
    if (Rebuild) {
      if (Options != NULL) {
        EfiBootManagerFreeLoadOptions (Options, OptionCount);
      }

      if (Visible != NULL) {
        FreePool (Visible);
        Visible = NULL;
      }

      Options      = BdsGetBootOptions (&OptionCount);
      VisibleCount = 0;
      if (OptionCount != 0) {
        Visible = AllocatePool (OptionCount * sizeof (*Visible));
        if (Visible == NULL) {
          break;
        }
      }

      for (Index = 0; Index < OptionCount; Index++) {
        if (((Options[Index].Attributes & LOAD_OPTION_HIDDEN) == 0) &&
            ((Options[Index].Attributes & LOAD_OPTION_CATEGORY) == LOAD_OPTION_CATEGORY_BOOT) &&
            !BdsIsSelfDevicePath (Options[Index].FilePath))
        {
          Visible[VisibleCount++] = Index;
        }
      }

      Count = VisibleCount + ARRAY_SIZE (Actions);
      if (Cursor >= Count) {
        Cursor = 0;
      }

      Rebuild = FALSE;
    }

    SfbBeginScreen (L"UEFI Boot Menu", L"Choose a boot option, or open BIOS Setup.");
    Start = SfbWindowStart (Cursor, Count, SFB_VISIBLE_ROWS);
    Last  = MIN (Start + SFB_VISIBLE_ROWS, Count);
    for (Index = Start; Index < Last; Index++) {
      if (Index < VisibleCount) {
        Selected = Visible[Index];
        UnicodeSPrint (
          Text,
          sizeof (Text),
          L"Boot%04x  %s%s",
          Options[Selected].OptionNumber,
          Options[Selected].Description,
          (Options[Selected].Attributes & LOAD_OPTION_ACTIVE) ? L"" : L" [disabled]"
          );
        SfbDrawRow ((BOOLEAN)(Index == Cursor), L" ", Text);
      } else {
        SfbDrawRow ((BOOLEAN)(Index == Cursor), L" ", Actions[Index - VisibleCount]);
      }
    }

    if (Last < Count) {
      Print (L"    ... %u more\r\n", (UINT32)(Count - Last));
    }

    Status = BdsGetLastBootStatus ();
    if (EFI_ERROR (Status) && (Status != EFI_NOT_STARTED)) {
      Print (L"\r\nLast boot: %r\r\n", Status);
    }

    SfbEndScreen (L"Vol Up/Down: move   Power: select");
    Key = SfbWaitForKey (0);
    if (Key == SfbKeyUnavailable) {
      break;
    }

    if ((Key == SfbKeyUp) || (Key == SfbKeyDown)) {
      SfbMoveCursor (&Cursor, Count, Key);
      continue;
    }

    if (Key != SfbKeySelect) {
      continue;
    }

    if (Cursor < VisibleCount) {
      Selected = Visible[Cursor];
      SfbShowBootingScreen (Options[Selected].Description, TRUE);
      Status = BdsBootLoadOption (&Options[Selected]);
      if (Options[Selected].ExitData != NULL) {
        FreePool (Options[Selected].ExitData);
        Options[Selected].ExitData = NULL;
      }

      if (EFI_ERROR (Status)) {
        SfbReportStatus (L"Boot failed", Status);
      }

      Rebuild = TRUE;
      continue;
    }

    switch (Cursor - VisibleCount) {
      case 0:
        if (BdsRunSetup ()) {
          Result = BdsMenuFastboot;
          goto Done;
        }

        // Read variables again, without rediscovering explicitly removed options.
        Rebuild = TRUE;
        break;
      case 1:
        SfbDebounceMenuExit ();
        Result = BdsMenuContinue;
        goto Done;
      case 2:
        SfbShowActionScreen (L"Powering off...");
        gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
        SfbReportStatus (L"Power off returned", EFI_DEVICE_ERROR);
        break;
      case 3:
        SfbShowActionScreen (L"Restarting...");
        gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
        SfbReportStatus (L"Restart returned", EFI_DEVICE_ERROR);
        break;
    }
  }

Done:
  if (Options != NULL) {
    EfiBootManagerFreeLoadOptions (Options, OptionCount);
  }

  if (Visible != NULL) {
    FreePool (Visible);
  }

  return Result;
}
