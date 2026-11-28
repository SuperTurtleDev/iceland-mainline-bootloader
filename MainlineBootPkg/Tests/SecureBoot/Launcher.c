/** @file
  Test-only firmware launcher. Preserve AAVMF variable authentication while
  isolating BdsApp's image verifier from AAVMF's image-verification policy.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "SecureBootTest.h"
#include <Guid/Gpt.h>
#include <Protocol/PartitionInfo.h>

STATIC SB_TEST_STATE mState;
STATIC EFI_GUID mEspGuid = { 0x11111111, 0x2222, 0x4333, { 0x84, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x51 } };

// Test-only stable QCOM PlatformBds prefix. An accidental vendor security call
// must fail even on QEMU, which does not normally expose this protocol.
typedef struct SB_PLATFORM_BDS SB_PLATFORM_BDS;
typedef EFI_STATUS (EFIAPI *SB_PLATFORM_METHOD)(IN SB_PLATFORM_BDS *This);
struct SB_PLATFORM_BDS {
  UINT64              Revision;
  SB_PLATFORM_METHOD  PreConsoleInit;
  SB_PLATFORM_METHOD  PostConsoleInit;
  SB_PLATFORM_METHOD  SetPlatformSecurity;
};

STATIC EFI_GUID mPlatformBdsGuid = {
  0xd874d61a, 0x4b87, 0x7608, { 0xa0, 0x0f, 0x58, 0xad, 0xd7, 0x05, 0x25, 0x30 }
};

STATIC EFI_STATUS EFIAPI
FirmwarePlatformSecurityReject (IN SB_PLATFORM_BDS *This)
{
  mState.PlatformSecurityCalls++;
  SbRequire (FALSE, L"BdsApp called QCOM PlatformBds security initialization");
  return EFI_ACCESS_DENIED;
}

STATIC SB_PLATFORM_BDS mPlatformBds = {
  0x00010000, NULL, NULL, FirmwarePlatformSecurityReject
};

STATIC EFI_STATUS EFIAPI
FirmwareImageReject (
  IN CONST EFI_SECURITY2_ARCH_PROTOCOL *This,
  IN CONST EFI_DEVICE_PATH_PROTOCOL   *DevicePath,
  IN VOID                             *FileBuffer,
  IN UINTN                            FileSize,
  IN BOOLEAN                          BootPolicy
  )
{
  mState.FirmwareImageCalls++;
  return EFI_ACCESS_DENIED;
}

STATIC EFI_STATUS EFIAPI
FirmwareStateReject (
  IN CONST EFI_SECURITY_ARCH_PROTOCOL *This,
  IN UINT32                           AuthenticationStatus,
  IN CONST EFI_DEVICE_PATH_PROTOCOL   *DevicePath
  )
{
  mState.FirmwareStateCalls++;
  return EFI_ACCESS_DENIED;
}

STATIC BOOLEAN
DisabledFixture (IN EFI_HANDLE Fixture)
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL               *Root;
  EFI_FILE_PROTOCOL               *File;
  EFI_STATUS                      Status;

  SbStatus (gBS->HandleProtocol (Fixture, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs), L"fixture mode filesystem");
  SbStatus (Fs->OpenVolume (Fs, &Root), L"fixture mode root");
  Status = Root->Open (Root, &File, L"\\EFI\\TEST\\disabled.flag", EFI_FILE_MODE_READ, 0);
  Root->Close (Root);
  if (Status == EFI_NOT_FOUND) {
    return FALSE;
  }
  SbStatus (Status, L"fixture mode file");
  File->Close (File);
  return TRUE;
}

STATIC EFI_HANDLE
FindFixture (VOID)
{
  EFI_HANDLE                  *Handles;
  EFI_HANDLE                  Found;
  EFI_PARTITION_INFO_PROTOCOL  *Info;
  UINTN                       Count;
  UINTN                       Index;
  EFI_STATUS                  Status;

  SbStatus (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles), L"enumerate fixture filesystems");
  Found = NULL;
  for (Index = 0; Index < Count; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiPartitionInfoProtocolGuid, (VOID **)&Info);
    if (!EFI_ERROR (Status) && Info->Type == PARTITION_TYPE_GPT &&
        CompareGuid (&Info->Info.Gpt.PartitionTypeGUID, &gEfiPartTypeSystemPartGuid) &&
        CompareGuid (&Info->Info.Gpt.UniquePartitionGUID, &mEspGuid)) {
      SbRequire ((BOOLEAN)(Found == NULL), L"unique fixture ESP");
      Found = Handles[Index];
    }
  }
  FreePool (Handles);
  SbRequire ((BOOLEAN)(Found != NULL), L"genuine GPT fixture ESP");
  return Found;
}

STATIC VOID
EnrollOptionalDbx (IN EFI_HANDLE Fixture)
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL               *Root;
  EFI_FILE_PROTOCOL               *File;
  EFI_STATUS                      Status;

  SbStatus (gBS->HandleProtocol (Fixture, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs), L"optional dbx filesystem");
  SbStatus (Fs->OpenVolume (Fs, &Root), L"optional dbx root");
  Status = Root->Open (Root, &File, L"\\EFI\\TEST\\dbx-initial.auth", EFI_FILE_MODE_READ, 0);
  Root->Close (Root);
  if (Status == EFI_NOT_FOUND) {
    return;
  }
  SbStatus (Status, L"optional initial dbx file");
  File->Close (File);
  SbSetAuthFile (Fixture, L"dbx", &gEfiImageSecurityDatabaseGuid, L"\\EFI\\TEST\\dbx-initial.auth", EFI_SUCCESS);
}

STATIC VOID
CheckFirmwareRejects (IN EFI_HANDLE Parent, IN CONST CHAR16 *Name)
{
  EFI_DEVICE_PATH_PROTOCOL *Path;
  EFI_HANDLE               Image;
  EFI_STATUS               Status;
  VOID                     *Buffer;
  UINTN                    Size;
  UINTN                    Before;

  Before = mState.FirmwareImageCalls;
  Status = SbLoadImage (Parent, mState.Fixture, Name, &Image);
  SbRequire ((BOOLEAN)(Status == EFI_ACCESS_DENIED && Image == NULL), L"firmware rejects file LoadImage before BdsApp");
  SbRequire ((BOOLEAN)(mState.FirmwareImageCalls > Before), L"DXE Core called cached firmware Security2 object for file");

  Buffer = SbReadFile (mState.Fixture, Name, &Size);
  Before = mState.FirmwareImageCalls;
  Status = gBS->LoadImage (FALSE, Parent, NULL, Buffer, Size, &Image);
  FreePool (Buffer);
  SbRequire ((BOOLEAN)(Status == EFI_ACCESS_DENIED && Image == NULL), L"firmware rejects pathless buffer LoadImage before BdsApp");
  SbRequire ((BOOLEAN)(mState.FirmwareImageCalls > Before), L"DXE Core called cached firmware Security2 object for buffer");

  Path = FileDevicePath (mState.Fixture, Name);
  SbRequire ((BOOLEAN)(Path != NULL), L"firmware legacy Security device path");
  Before = mState.FirmwareStateCalls;
  Status = mState.Security->FileAuthenticationState (mState.Security, 0, Path);
  FreePool (Path);
  SbRequire ((BOOLEAN)(Status == EFI_ACCESS_DENIED && mState.FirmwareStateCalls == Before + 1), L"firmware legacy Security rejects before BdsApp");
}

EFI_STATUS EFIAPI
LauncherEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_HANDLE    PlatformHandle;
  EFI_STATUS    Status;
  SB_TEST_STATE *State;

  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  Print (L"SBTEST LAUNCHER: real AAVMF authenticated variables, always-rejecting firmware Security/Security2 fixtures\n");
  ZeroMem (&mState, sizeof (mState));
  mState.Magic = SB_TEST_MAGIC;
  mState.Fixture = FindFixture ();
  mState.SetupModeFixture = DisabledFixture (mState.Fixture);
  SbCheckMode (1, 0);
  SbCheckNoEnrollment ();

  // Preload before installing rejecting callbacks. StartImage runs production
  // constructors in actual user mode after enrollment, or untouched setup mode.
  SbStatus (SbLoadImage (ImageHandle, mState.Fixture, L"\\EFI\\TEST\\BdsApp.efi", &mState.BdsImage), L"preload production BdsApp in setup mode");
  if (!mState.SetupModeFixture) {
    SbSetAuthFile (mState.Fixture, L"KEK", &gEfiGlobalVariableGuid, L"\\EFI\\TEST\\KEK.auth", EFI_SUCCESS);
    SbSetAuthFile (mState.Fixture, L"db", &gEfiImageSecurityDatabaseGuid, L"\\EFI\\TEST\\db.auth", EFI_SUCCESS);
    EnrollOptionalDbx (mState.Fixture);
    SbSetAuthFile (mState.Fixture, L"PK", &gEfiGlobalVariableGuid, L"\\EFI\\TEST\\PK.auth", EFI_SUCCESS);
    SbCheckVariableFile (mState.Fixture, L"PK", &gEfiGlobalVariableGuid, L"\\EFI\\TEST\\PK.esl");
    SbCheckVariableFile (mState.Fixture, L"KEK", &gEfiGlobalVariableGuid, L"\\EFI\\TEST\\KEK.esl");
    SbCheckVariableFile (mState.Fixture, L"db", &gEfiImageSecurityDatabaseGuid, L"\\EFI\\TEST\\db.esl");
    SbCheckMode (0, 1);
    Print (L"SBTEST ENROLLMENT PASS: real time-authenticated PK KEK db; exact data and attributes verified\n");
  }

  // DXE Core has cached these protocol objects. Every fixture callback rejects,
  // regardless of image signature, device path or actual SecureBoot value.
  // Production must replace both methods without ever delegating back to them.
  SbStatus (gBS->LocateProtocol (&gEfiSecurity2ArchProtocolGuid, NULL, (VOID **)&mState.Security2), L"firmware Security2 protocol");
  mState.FirmwareImageReject = FirmwareImageReject;
  mState.Security2->FileAuthentication = FirmwareImageReject;
  SbStatus (gBS->LocateProtocol (&gEfiSecurityArchProtocolGuid, NULL, (VOID **)&mState.Security), L"firmware Security protocol");
  mState.FirmwareStateReject = FirmwareStateReject;
  mState.Security->FileAuthenticationState = FirmwareStateReject;
  PlatformHandle = NULL;
  SbStatus (gBS->InstallProtocolInterface (&PlatformHandle, &mPlatformBdsGuid, EFI_NATIVE_INTERFACE, &mPlatformBds), L"install QCOM platform security rejection trap");

  CheckFirmwareRejects (ImageHandle, L"\\EFI\\TEST\\unsigned.efi");
  CheckFirmwareRejects (ImageHandle, L"\\EFI\\BOOT\\BOOTAA64.EFI");
  mState.ImageCallsBeforeBds = mState.FirmwareImageCalls;
  mState.StateCallsBeforeBds = mState.FirmwareStateCalls;
  Print (L"SBTEST FIRMWARE BASELINE: Security=%u Security2=%u PlatformSecurity=%u\n",
         mState.StateCallsBeforeBds, mState.ImageCallsBeforeBds, mState.PlatformSecurityCalls);
  if (mState.SetupModeFixture) {
    SbCheckMode (1, 0);
    SbCheckNoEnrollment ();
    Print (L"SBTEST DISABLED PRE: SecureBoot=0 SetupMode=1; no keys enrolled; firmware Security/Security2 reject images\n");
  } else {
    SbCheckMode (0, 1);
    Print (L"SBTEST ENABLED PRE: SecureBoot=1 SetupMode=0; firmware Security/Security2 reject trusted and unsigned images\n");
  }

  State = &mState;
  SbStatus (gRT->SetVariable (L"State", &gSbTestGuid, SB_VOL_ATTR, sizeof (State), &State), L"arm volatile shared state");
  Print (L"SBTEST ARMED: starting production BdsApp\n");
  Status = gBS->StartImage (mState.BdsImage, NULL, NULL);
  Print (L"SBTEST FAIL: BdsApp returned %r without probe shutdown\n", Status);
  SbShutdown ();
  return EFI_ABORTED;
}
