/** @file
  Signed application reached only through production BdsApp. Exercise real
  LoadImage/StartImage and authenticated SetVariable; never call a verifier stub.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "SecureBootTest.h"

STATIC VOID
CheckImage (
  IN EFI_HANDLE    Parent,
  IN SB_TEST_STATE *State,
  IN CONST CHAR16  *Name,
  IN BOOLEAN      Allowed,
  IN BOOLEAN      FromBuffer
  )
{
  EFI_HANDLE                Image;
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *Path;
  VOID                      *Buffer;
  UINTN                     Size;
  UINTN                     Executions;

  SbCheckFirmwareIsolation (State);
  Executions = State->Executions;
  Image = NULL;
  if (FromBuffer) {
    Buffer = SbReadFile (State->Fixture, Name, &Size);
    Path = FileDevicePath (State->Fixture, Name);
    SbRequire ((BOOLEAN)(Path != NULL), L"buffer load device path");
    Status = gBS->LoadImage (FALSE, Parent, Path, Buffer, Size, &Image);
    FreePool (Path);
    FreePool (Buffer);
  } else {
    Status = SbLoadImage (Parent, State->Fixture, Name, &Image);
  }
  Print (L"SBTEST IMAGE: %s source=%s -> %r\n", Name, FromBuffer ? L"buffer" : L"file", Status);
  SbCheckFirmwareIsolation (State);
  if (Allowed) {
    SbStatus (Status, L"trusted signature accepted by local verifier despite rejecting firmware");
    SbStatus (gBS->StartImage (Image, NULL, NULL), L"trusted payload StartImage");
    SbRequire ((BOOLEAN)(State->Executions == Executions + 1), L"trusted payload executed exactly once");
    // An application returning from StartImage has already been unloaded.
  } else {
    SbRequire ((BOOLEAN)(Status == EFI_SECURITY_VIOLATION || Status == EFI_ACCESS_DENIED), L"untrusted image denied with security status");
    if (Image != NULL) {
      // SECURITY_VIOLATION may return a resident but untrusted image. Verify
      // StartImage cannot execute it, then release its handle.
      Status = gBS->StartImage (Image, NULL, NULL);
      SbRequire ((BOOLEAN)(Status == EFI_SECURITY_VIOLATION || Status == EFI_ACCESS_DENIED), L"untrusted resident image cannot start");
      SbStatus (gBS->UnloadImage (Image), L"unload rejected image");
    }
    SbRequire ((BOOLEAN)(State->Executions == Executions), L"denied payload did not execute");
  }
  SbCheckFirmwareIsolation (State);
}

EFI_STATUS EFIAPI
SecureBootProbeEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  SB_TEST_STATE             *State;
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  UINTN                     Mode;

  State = SbGetState ();
  SbStatus (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded), L"probe loaded image");
  SbRequire ((BOOLEAN)(Loaded->ParentHandle == State->BdsImage), L"probe was started by production BdsApp");
  SbRequire ((BOOLEAN)(!State->SetupModeFixture), L"image verification fixture enrolled actual SecureBoot");
  SbCheckMode (0, 1);
  SbCheckLegacyIsolation (State);
  Print (L"SBTEST PROBE: signed default child parent is BdsApp; rejecting firmware callbacks never called\n");

  for (Mode = 0; Mode < 2; Mode++) {
    CheckImage (ImageHandle, State, L"\\EFI\\TEST\\good.efi", TRUE, (BOOLEAN)Mode);
    CheckImage (ImageHandle, State, L"\\EFI\\TEST\\unsigned.efi", FALSE, (BOOLEAN)Mode);
    CheckImage (ImageHandle, State, L"\\EFI\\TEST\\wrong.efi", FALSE, (BOOLEAN)Mode);
  }
  Print (L"SBTEST IMAGES PASS: trusted executes, unsigned and wrong key denied (file and buffer)\n");

  SbSetAuthFile (State->Fixture, L"PK", &gEfiGlobalVariableGuid, L"\\EFI\\TEST\\PK-update.auth", EFI_SUCCESS);
  SbSetAuthFile (State->Fixture, L"PK", &gEfiGlobalVariableGuid, L"\\EFI\\TEST\\PK-update.auth", EFI_SECURITY_VIOLATION);
  SbCheckVariableFile (State->Fixture, L"PK", &gEfiGlobalVariableGuid, L"\\EFI\\TEST\\PK.esl");
  SbSetAuthFile (State->Fixture, L"KEK", &gEfiGlobalVariableGuid, L"\\EFI\\TEST\\KEK-update.auth", EFI_SUCCESS);
  SbSetAuthFile (State->Fixture, L"KEK", &gEfiGlobalVariableGuid, L"\\EFI\\TEST\\KEK-update.auth", EFI_SECURITY_VIOLATION);
  SbCheckVariableFile (State->Fixture, L"KEK", &gEfiGlobalVariableGuid, L"\\EFI\\TEST\\KEK.esl");
  SbSetAuthFile (State->Fixture, L"db", &gEfiImageSecurityDatabaseGuid, L"\\EFI\\TEST\\db-update.auth", EFI_SUCCESS);
  SbSetAuthFile (State->Fixture, L"db", &gEfiImageSecurityDatabaseGuid, L"\\EFI\\TEST\\db-wrong.auth", EFI_SECURITY_VIOLATION);
  SbSetAuthFile (State->Fixture, L"db", &gEfiImageSecurityDatabaseGuid, L"\\EFI\\TEST\\db-update.auth", EFI_SECURITY_VIOLATION);
  SbCheckVariableFile (State->Fixture, L"db", &gEfiImageSecurityDatabaseGuid, L"\\EFI\\TEST\\db.esl");
  CheckImage (ImageHandle, State, L"\\EFI\\TEST\\good.efi", TRUE, FALSE);
  Print (L"SBTEST AUTH PASS: PK KEK db signed updates accepted; wrong signer and replay rejected\n");

  SbSetAuthFile (State->Fixture, L"dbx", &gEfiImageSecurityDatabaseGuid, L"\\EFI\\TEST\\dbx.auth", EFI_SUCCESS);
  SbSetAuthFile (State->Fixture, L"dbx", &gEfiImageSecurityDatabaseGuid, L"\\EFI\\TEST\\dbx-wrong.auth", EFI_SECURITY_VIOLATION);
  SbSetAuthFile (State->Fixture, L"dbx", &gEfiImageSecurityDatabaseGuid, L"\\EFI\\TEST\\dbx.auth", EFI_SECURITY_VIOLATION);
  SbCheckVariableFile (State->Fixture, L"dbx", &gEfiImageSecurityDatabaseGuid, L"\\EFI\\TEST\\dbx.esl");
  CheckImage (ImageHandle, State, L"\\EFI\\TEST\\good.efi", FALSE, FALSE);
  CheckImage (ImageHandle, State, L"\\EFI\\TEST\\good.efi", FALSE, TRUE);
  SbCheckMode (0, 1);
  Print (L"SBTEST DBX PASS: authenticated SHA256 revocation denies previously trusted image\n");
  SbCheckLegacyIsolation (State);
  Print (L"SBTEST PASS: application verifier and real authenticated variable backend\n");
  SbShutdown ();
  return EFI_SUCCESS;
}
