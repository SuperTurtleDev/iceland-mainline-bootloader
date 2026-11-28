/** @file
  Test production BdsSecurityLib with its own application state and real db/dbx.
  This covers the authorization library, not the fastboot command transport.
  Deterministic RNG is confined to this fixture so host OpenSSL can sign nonces.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "SecureBootTest.h"
#include <Library/BdsSecurityLib.h>
#include <Library/PrintLib.h>
#include <Protocol/Rng.h>

STATIC UINTN   mGeneration;
STATIC BOOLEAN mRngFailure;

STATIC EFI_STATUS EFIAPI
FixtureGetRng (
  IN EFI_RNG_PROTOCOL *This,
  IN EFI_RNG_ALGORITHM *Algorithm OPTIONAL,
  IN UINTN Length,
  OUT UINT8 *Value
  )
{
  UINTN Index;

  if (mRngFailure) {
    return EFI_DEVICE_ERROR;
  }
  if (Length != BDS_SECURITY_CHALLENGE_SIZE || Value == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  mGeneration++;
  for (Index = 0; Index < Length; Index++) {
    Value[Index] = (UINT8)(Index + mGeneration);
  }
  return EFI_SUCCESS;
}

STATIC EFI_RNG_PROTOCOL mFixtureRng = { NULL, FixtureGetRng };

STATIC VOID
InstallFixtureRng (VOID)
{
  EFI_RNG_PROTOCOL *Rng;
  EFI_HANDLE       Handle;
  EFI_STATUS       Status;

  Status = gBS->LocateProtocol (&gEfiRngProtocolGuid, NULL, (VOID **)&Rng);
  if (Status == EFI_NOT_FOUND) {
    Handle = NULL;
    SbStatus (gBS->InstallProtocolInterface (&Handle, &gEfiRngProtocolGuid, EFI_NATIVE_INTERFACE, &mFixtureRng), L"test-only deterministic RNG");
  } else {
    SbStatus (Status, L"find RNG for test replacement");
    Rng->GetRNG = FixtureGetRng;
  }
}

STATIC VOID
NewChallenge (IN UINTN Generation)
{
  CONST UINT8 *Nonce;
  UINTN       Index;

  Nonce = BdsSecurityChallenge ();
  SbRequire ((BOOLEAN)(Nonce != NULL && mGeneration == Generation), L"challenge RNG generation");
  for (Index = 0; Index < BDS_SECURITY_CHALLENGE_SIZE; Index++) {
    SbRequire ((BOOLEAN)(Nonce[Index] == (UINT8)(Index + Generation)), L"challenge exact signed bytes");
  }
  SbRequire ((BOOLEAN)(BdsSecurityChallenge () == Nonce && mGeneration == Generation), L"pending challenge stable until consumed");
}

STATIC VOID
Response (
  IN SB_TEST_STATE *State,
  IN UINTN        Generation,
  IN CONST CHAR16 *Kind,
  IN BOOLEAN      Accepted
  )
{
  CHAR16     Path[100];
  VOID       *Signature;
  UINTN      Size;
  EFI_STATUS Status;

  UnicodeSPrint (Path, sizeof (Path), L"\\EFI\\TEST\\nonce%u-%s.sig", Generation, Kind);
  Signature = SbReadFile (State->Fixture, Path, &Size);
  Status = BdsSecurityVerifyChallenge (Signature, Size);
  FreePool (Signature);
  Print (L"SBTEST CHALLENGE: nonce=%u response=%s -> %r\n", Generation, Kind, Status);
  SbRequire ((BOOLEAN)(Status == (Accepted ? EFI_SUCCESS : EFI_SECURITY_VIOLATION)), L"challenge response result");
  SbRequire ((BOOLEAN)(BdsSecurityIsPrivileged () == Accepted), L"challenge privilege result");
  SbRequire ((BOOLEAN)(BdsSecurityLocked () != Accepted), L"challenge lock result");
}

STATIC VOID
CheckUnsignedDenied (IN EFI_HANDLE Parent, IN SB_TEST_STATE *State)
{
  EFI_HANDLE Image;
  EFI_STATUS Status;
  UINTN      Executions;

  SbCheckMode (0, 1);
  SbCheckFirmwareIsolation (State);
  Executions = State->Executions;
  Status = SbLoadImage (Parent, State->Fixture, L"\\EFI\\TEST\\unsigned.efi", &Image);
  SbRequire ((BOOLEAN)(Status == EFI_SECURITY_VIOLATION || Status == EFI_ACCESS_DENIED), L"SecureBoot=1 rejects unsigned images regardless of fastboot authorization");
  if (Image != NULL) {
    Status = gBS->StartImage (Image, NULL, NULL);
    SbRequire ((BOOLEAN)(Status == EFI_SECURITY_VIOLATION || Status == EFI_ACCESS_DENIED), L"unsigned resident image cannot start despite fastboot authorization");
    SbStatus (gBS->UnloadImage (Image), L"release untrusted image");
  }
  SbRequire ((BOOLEAN)(State->Executions == Executions), L"fastboot authorization never executes unsigned image");
  SbCheckFirmwareIsolation (State);
}

EFI_STATUS EFIAPI
ChallengeProbeEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  SB_TEST_STATE             *State;
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  UINTN                     Size;
  EFI_STATUS                Status;
  UINT8                     Malformed;

  State = SbGetState ();
  SbStatus (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded), L"challenge loaded image");
  SbRequire ((BOOLEAN)(Loaded->ParentHandle == State->BdsImage), L"challenge parent is BdsApp");
  SbCheckMode (0, 1);
  SbCheckLegacyIsolation (State);
  SbStatus (BdsSecurityInitialize (), L"initialize separate production-library challenge state");
  SbCheckLegacyIsolation (State);
  SbRequire ((BOOLEAN)(!BdsSecurityAuthEnabled () && !BdsSecurityLocked ()), L"secure fastboot defaults disabled with enrolled keys");
  CheckUnsignedDenied (ImageHandle, State);
  SbStatus (BdsSecuritySetFastbootEnabled (TRUE), L"explicitly enable independent secure fastboot");
  SbRequire ((BOOLEAN)(BdsSecurityAuthEnabled () && BdsSecurityLocked () && !BdsSecurityIsPrivileged ()), L"enabled db requires authorization");
  InstallFixtureRng ();
  Print (L"SBTEST CHALLENGE SCOPE: production library in separate probe; deterministic test RNG; real db/dbx\n");
  mRngFailure = TRUE;
  SbRequire ((BOOLEAN)(BdsSecurityChallenge () == NULL && BdsSecurityLocked ()), L"RNG failure stays locked");
  mRngFailure = FALSE;

  NewChallenge (1);
  Response (State, 1, L"raw-good", TRUE);
  Response (State, 1, L"raw-good", FALSE);
  NewChallenge (2);
  Response (State, 2, L"cms-good", TRUE);
  Response (State, 2, L"cms-good", FALSE);
  NewChallenge (3);
  Response (State, 3, L"raw-wrong", FALSE);
  NewChallenge (4);
  Response (State, 4, L"cms-wrong", FALSE);
  NewChallenge (5);
  Malformed = 0x30;
  Status = BdsSecurityVerifyChallenge (&Malformed, sizeof (Malformed));
  SbRequire ((BOOLEAN)(EFI_ERROR (Status) && BdsSecurityLocked () && !BdsSecurityIsPrivileged ()), L"malformed response consumes nonce and remains locked");
  Response (State, 5, L"raw-good", FALSE);
  NewChallenge (6);
  Response (State, 1, L"raw-good", FALSE);
  NewChallenge (7);
  Response (State, 7, L"cms-good", TRUE);
  CheckUnsignedDenied (ImageHandle, State);
  Print (L"SBTEST CHALLENGE SIGNATURES PASS: raw RSA and CMS accepted; wrong key, replay, malformed and RNG failure denied\n");

  SbStatus (BdsSecuritySetFastbootEnabled (FALSE), L"disable secure fastboot independently");
  SbRequire ((BOOLEAN)(!BdsSecurityLocked () && !BdsSecurityIsPrivileged () && BdsSecurityChallenge () == NULL), L"disabled fastboot clears grant and challenge");
  CheckUnsignedDenied (ImageHandle, State);
  SbStatus (BdsSecuritySetFastbootEnabled (TRUE), L"reenable secure fastboot");
  SbRequire ((BOOLEAN)(BdsSecurityLocked () && !BdsSecurityIsPrivileged ()), L"reenabled fastboot cannot recover old grant");
  Print (L"SBTEST CHALLENGE SWITCH PASS: independent default FALSE switch; toggling clears authorization; image verification remains active\n");

  SbSetAuthFile (State->Fixture, L"dbx", &gEfiImageSecurityDatabaseGuid, L"\\EFI\\TEST\\dbx-cert.auth", EFI_SUCCESS);
  SbRequire ((BOOLEAN)(!BdsSecurityIsPrivileged () && BdsSecurityLocked ()), L"dbx update immediately revokes existing grant");
  NewChallenge (8);
  Response (State, 8, L"cms-good", FALSE);
  NewChallenge (9);
  Response (State, 9, L"raw-good", FALSE);
  SbSetAuthFile (State->Fixture, L"dbx", &gEfiImageSecurityDatabaseGuid, L"\\EFI\\TEST\\dbx-tbs.auth", EFI_SUCCESS);
  SbCheckVariableFile (State->Fixture, L"dbx", &gEfiImageSecurityDatabaseGuid, L"\\EFI\\TEST\\dbx-tbs.esl");
  NewChallenge (10);
  Response (State, 10, L"cms-good", FALSE);
  NewChallenge (11);
  Response (State, 11, L"raw-good", FALSE);
  Print (L"SBTEST CHALLENGE REVOCATION PASS: dbx X509 and TBS SHA256 deny raw RSA and CMS; stale grant cleared\n");

  SbSetAuthFile (State->Fixture, L"db", &gEfiImageSecurityDatabaseGuid, L"\\EFI\\TEST\\db-delete.auth", EFI_SUCCESS);
  SbRequire ((BOOLEAN)(!BdsSecurityAuthEnabled () && !BdsSecurityLocked () && BdsSecurityChallenge () == NULL), L"missing db automatically disables secure fastboot");
  SbRequire ((BOOLEAN)(BdsSecuritySetFastbootEnabled (TRUE) == EFI_NOT_READY), L"missing db prevents enabling secure fastboot");
  SbSetAuthFile (State->Fixture, L"dbx", &gEfiImageSecurityDatabaseGuid, L"\\EFI\\TEST\\dbx-delete.auth", EFI_SUCCESS);
  Size = 0;
  Status = gRT->GetVariable (L"db", &gEfiImageSecurityDatabaseGuid, NULL, &Size, NULL);
  SbRequire ((BOOLEAN)(Status == EFI_NOT_FOUND), L"db really absent");
  Size = 0;
  Status = gRT->GetVariable (L"dbx", &gEfiImageSecurityDatabaseGuid, NULL, &Size, NULL);
  SbRequire ((BOOLEAN)(Status == EFI_NOT_FOUND), L"dbx really absent");
  SbRequire ((BOOLEAN)(!BdsSecurityAuthEnabled () && !BdsSecurityLocked () && !BdsSecurityIsPrivileged () && BdsSecurityChallenge () == NULL), L"both databases absent means no certificate authorization");
  CheckUnsignedDenied (ImageHandle, State);
  SbCheckLegacyIsolation (State);
  Print (L"SBTEST CHALLENGE EMPTY PASS: missing db disables switch; both missing have no certificate authorization\n");
  Print (L"SBTEST CHALLENGE PASS: production authorization library with real authenticated variables\n");
  // The local library installed a resident callback. End QEMU without returning
  // or unloading this probe, so that callback never references freed code.
  SbShutdown ();
  return EFI_SUCCESS;
}
