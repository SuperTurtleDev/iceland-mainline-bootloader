/** @file
  Canoe console UI and FAT filesystem helpers for application-mode BDS.

  Copyright (c) 2026, contributors to the canoe ABL tree.
  SPDX-License-Identifier: BSD-3-Clause
**/

#ifndef MAINLINE_BDS_MENU_H_
#define MAINLINE_BDS_MENU_H_

#include <Uefi.h>
#include <Protocol/SimpleFileSystem.h>

#define SFB_DESC_CHARS       48
#define SFB_PATH_CHARS       1024
#define SFB_MAX_DIR_ENTRIES  128
#define SFB_VISIBLE_ROWS     12

typedef enum {
  SfbKeyTimeout,
  SfbKeyUp,
  SfbKeyDown,
  SfbKeySelect,
  SfbKeyUnavailable
} SFB_KEY;

typedef enum {
  BdsMenuContinue,
  BdsMenuFastboot,
  BdsMenuUnavailable
} BDS_MENU_ACTION;

BOOLEAN
BdsConsoleAvailable (
  VOID
  );

BDS_MENU_ACTION
BdsRunBootMenu (
  VOID
  );

EFI_STATUS
SfbStartFatStack (
  VOID
  );

// Enumerate standard SFS handles, including USB and the mounted FAT container.
EFI_STATUS
SfbLocateVolumes (
  OUT EFI_HANDLE  **Handles,
  OUT UINTN       *Count
  );

EFI_STATUS
SfbOpenVolumeRoot (
  IN EFI_HANDLE          Volume,
  OUT EFI_FILE_PROTOCOL  **Root
  );

BOOLEAN
SfbFileExists (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CONST CHAR16       *Path
  );

EFI_STATUS
SfbReadFileBytes (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CONST CHAR16       *Path,
  OUT VOID              *Buffer,
  IN UINTN              MaxBytes,
  OUT UINTN             *BytesRead
  );

BOOLEAN
SfbIsEfiDriverFile (
  IN EFI_FILE_PROTOCOL  *Root,
  IN CONST CHAR16       *Path
  );

VOID
SfbConnectAll (
  VOID
  );

VOID
SfbGetVolumeLabel (
  IN EFI_FILE_PROTOCOL  *Root,
  OUT CHAR16            *Out,
  IN UINTN              OutChars
  );

VOID
BdsRunFileBrowser (
  VOID
  );

BOOLEAN
BdsBrowseVolume (
  IN EFI_HANDLE    Volume,
  IN CONST CHAR16  *VolumeLabel,
  IN CONST CHAR16  *BrowseRoot
  );

VOID
SfbShowFastbootMode (
  VOID
  );

VOID
SfbShowEnteringMenu (
  VOID
  );

VOID
SfbShowBootingScreen (
  IN CONST CHAR16  *Name,
  IN BOOLEAN       ClearScreen
  );

VOID
SfbShowEnteringScreen (
  IN CONST CHAR16  *What
  );

VOID
SfbDebounceMenuExit (
  VOID
  );

SFB_KEY
SfbWaitForKey (
  IN UINT32  TimeoutMs
  );

VOID
SfbBeginScreen (
  IN CONST CHAR16  *Title,
  IN CONST CHAR16  *Subtitle OPTIONAL
  );

VOID
SfbEndScreen (
  IN CONST CHAR16  *Footer
  );

VOID
SfbDrawRow (
  IN BOOLEAN       Selected,
  IN CONST CHAR16  *Marker,
  IN CONST CHAR16  *Text
  );

UINTN
SfbWindowStart (
  IN UINTN  Cursor,
  IN UINTN  Count,
  IN UINTN  Rows
  );

VOID
SfbMoveCursor (
  IN OUT UINTN  *Cursor,
  IN UINTN      Count,
  IN SFB_KEY    Key
  );

VOID
SfbReportStatus (
  IN CONST CHAR16  *What,
  IN EFI_STATUS    Status
  );

#endif
