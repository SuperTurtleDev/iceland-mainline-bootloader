/** @file
  Test firmware BDS: initialize console and enter BdsApp through one fixed path.
  No standard firmware BootOrder, BootNext, hotkeys, menus or auto discovery run.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "TestCommon.h"
#include <Protocol/Bds.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>
#include <Guid/EventGroup.h>

EFI_STATUS EFIAPI
LauncherEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  );

STATIC EFI_HANDLE  mDriverImage;

STATIC VOID
ConnectDevices (
  VOID
  )
{
  EFI_HANDLE  *Handles;
  UINTN       Count;
  UINTN       Index;
  UINTN       Pass;

  for (Pass = 0; Pass < 2; Pass++) {
    if (!EFI_ERROR (gBS->LocateHandleBuffer (AllHandles, NULL, NULL, &Count, &Handles))) {
      for (Index = 0; Index < Count; Index++) {
        gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
      }

      FreePool (Handles);
    }
  }
}

STATIC VOID
ConnectConsole (
  VOID
  )
{
  EFI_HANDLE                       *Handles;
  EFI_SIMPLE_TEXT_INPUT_PROTOCOL   *Input;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *Output;
  UINTN                            Count;
  UINTN                            Index;
  UINT32                           Crc;

  if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleTextOutProtocolGuid, NULL, &Count, &Handles))) {
    for (Index = 0; Index < Count; Index++) {
      if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiSimpleTextInProtocolGuid, (VOID **)&Input))) {
        continue;
      }

      if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiSimpleTextOutProtocolGuid, (VOID **)&Output))) {
        continue;
      }

      gST->ConIn               = Input;
      gST->ConOut              = Output;
      gST->StdErr              = Output;
      gST->ConsoleInHandle     = Handles[Index];
      gST->ConsoleOutHandle    = Handles[Index];
      gST->StandardErrorHandle = Handles[Index];
      break;
    }

    FreePool (Handles);
  }

  gST->Hdr.CRC32 = 0;
  if (!EFI_ERROR (gBS->CalculateCrc32 (gST, gST->Hdr.HeaderSize, &Crc))) {
    gST->Hdr.CRC32 = Crc;
  }
}

STATIC VOID EFIAPI
DirectBdsEntry (
  IN EFI_BDS_ARCH_PROTOCOL  *This
  )
{
  EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);
  ConnectDevices ();
  ConnectConsole ();
  Print (L"BDSTEST DIRECT BDS: standard firmware boot manager replaced\n");
  LauncherEntry (mDriverImage, gST);
  CpuDeadLoop ();
}

STATIC EFI_BDS_ARCH_PROTOCOL  mDirectBds = { DirectBdsEntry };

EFI_STATUS EFIAPI
DirectBootEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HANDLE  Handle;

  mDriverImage = ImageHandle;
  Handle       = NULL;
  return gBS->InstallProtocolInterface (&Handle, &gEfiBdsArchProtocolGuid, EFI_NATIVE_INTERFACE, &mDirectBds);
}
