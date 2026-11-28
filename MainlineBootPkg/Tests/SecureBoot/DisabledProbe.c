/** @file
  Confirm disabled BdsApp stubs bypass firmware device-path rejection.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "SecureBootTest.h"

EFI_STATUS
EFIAPI
DisabledProbeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  SB_TEST_STATE              *State;
  EFI_LOADED_IMAGE_PROTOCOL  *Loaded;
  EFI_DEVICE_PATH_PROTOCOL   *Path;
  EFI_HANDLE                 Image;
  VOID                       *Buffer;
  UINTN                      Size;
  UINTN                      Executions;

  State = SbGetState ();
  SbStatus (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded), L"disabled probe loaded image");
  SbRequire ((BOOLEAN)(Loaded->ParentHandle == State->BdsImage && State->SetupModeFixture), L"unsigned probe reached from production BdsApp in setup mode");
  SbCheckMode (1, 0);
  SbCheckNoEnrollment ();
  SbCheckLegacyIsolation (State);
  Executions = State->Executions;
  Path = FileDevicePath (State->Fixture, L"\\EFI\\TEST\\wrong.efi");
  SbRequire ((BOOLEAN)(Path != NULL), L"disabled device path");
  SbStatus (SbLoadImage (ImageHandle, State->Fixture, L"\\EFI\\TEST\\unsigned.efi", &Image), L"disabled unsigned file LoadImage");
  SbStatus (gBS->StartImage (Image, NULL, NULL), L"disabled unsigned file executes");
  Buffer = SbReadFile (State->Fixture, L"\\EFI\\TEST\\wrong.efi", &Size);
  SbStatus (gBS->LoadImage (FALSE, ImageHandle, Path, Buffer, Size, &Image), L"disabled wrong-signed buffer with device path LoadImage");
  FreePool (Buffer);
  SbStatus (gBS->StartImage (Image, NULL, NULL), L"disabled wrong-signed buffer executes");
  FreePool (Path);
  SbRequire ((BOOLEAN)(State->Executions == Executions + 2), L"allow-all stubs execute both images exactly once");
  SbCheckMode (1, 0);
  SbCheckNoEnrollment ();
  SbCheckLegacyIsolation (State);
  Print (L"SBTEST DISABLED PASS: SecureBoot=0; unsigned file and wrong-signed buffer execute; firmware rejection counters unchanged\n");
  SbShutdown ();
  return EFI_SUCCESS;
}
