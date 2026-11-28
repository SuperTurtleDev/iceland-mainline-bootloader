/** @file
  Assertions and firmware I/O only. All cryptography belongs to stock edk2.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "SecureBootTest.h"

EFI_GUID gSbTestGuid = { 0x2f5aab30, 0xf7d2, 0x4b41, { 0x9a, 0x4a, 0x79, 0x35, 0xf5, 0x36, 0x13, 0x80 } };

VOID
SbShutdown (VOID)
{
  gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
  CpuDeadLoop ();
}

VOID
SbRequire (IN BOOLEAN Condition, IN CONST CHAR16 *Message)
{
  if (!Condition) {
    Print (L"SBTEST FAIL: %s\n", Message);
    SbShutdown ();
  }
}

VOID
SbStatus (IN EFI_STATUS Status, IN CONST CHAR16 *Message)
{
  if (EFI_ERROR (Status)) {
    Print (L"SBTEST FAIL: %s: %r\n", Message, Status);
    SbShutdown ();
  }
}

VOID *
SbReadFile (IN EFI_HANDLE Fixture, IN CONST CHAR16 *Name, OUT UINTN *Size)
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL               *Root;
  EFI_FILE_PROTOCOL               *File;
  EFI_FILE_INFO                   *Info;
  EFI_STATUS                      Status;
  UINTN                           InfoSize;
  UINTN                           ReadSize;
  VOID                            *Data;

  SbStatus (gBS->HandleProtocol (Fixture, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs), L"fixture filesystem");
  SbStatus (Fs->OpenVolume (Fs, &Root), L"fixture root");
  SbStatus (Root->Open (Root, &File, (CHAR16 *)Name, EFI_FILE_MODE_READ, 0), Name);
  InfoSize = 0;
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, NULL);
  SbRequire ((BOOLEAN)(Status == EFI_BUFFER_TOO_SMALL), L"file info sizing");
  Info = AllocatePool (InfoSize);
  SbRequire ((BOOLEAN)(Info != NULL), L"file info allocation");
  SbStatus (File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info), L"file info");
  SbRequire ((BOOLEAN)(Info->FileSize > 0 && Info->FileSize <= 16 * 1024 * 1024), L"bounded fixture file");
  *Size = (UINTN)Info->FileSize;
  FreePool (Info);
  Data = AllocatePool (*Size);
  SbRequire ((BOOLEAN)(Data != NULL), L"file buffer allocation");
  ReadSize = *Size;
  SbStatus (File->Read (File, &ReadSize, Data), Name);
  SbRequire ((BOOLEAN)(ReadSize == *Size), L"complete fixture read");
  File->Close (File);
  Root->Close (Root);
  return Data;
}

VOID *
SbReadVariable (IN CHAR16 *Name, IN EFI_GUID *Guid, OUT UINTN *Size)
{
  EFI_STATUS Status;
  VOID       *Data;

  *Size = 0;
  Status = gRT->GetVariable (Name, Guid, NULL, Size, NULL);
  SbRequire ((BOOLEAN)(Status == EFI_BUFFER_TOO_SMALL && *Size > 0 && *Size <= 1024 * 1024), Name);
  Data = AllocatePool (*Size);
  SbRequire ((BOOLEAN)(Data != NULL), L"variable allocation");
  SbStatus (gRT->GetVariable (Name, Guid, NULL, Size, Data), Name);
  return Data;
}

VOID
SbCheckVariableFile (IN EFI_HANDLE Fixture, IN CHAR16 *Name, IN EFI_GUID *Guid, IN CONST CHAR16 *FileName)
{
  UINTN  ExpectedSize;
  UINTN  ActualSize;
  UINT32 Attributes;
  VOID   *Expected;
  VOID   *Actual;

  Expected = SbReadFile (Fixture, FileName, &ExpectedSize);
  Actual = SbReadVariable (Name, Guid, &ActualSize);
  SbRequire ((BOOLEAN)(ExpectedSize == ActualSize && CompareMem (Expected, Actual, ExpectedSize) == 0), L"exact authenticated variable readback");
  SbStatus (gRT->GetVariable (Name, Guid, &Attributes, &ActualSize, Actual), Name);
  SbRequire ((BOOLEAN)(Attributes == SB_AUTH_ATTR), L"authenticated variable attributes");
  FreePool (Expected);
  FreePool (Actual);
}

VOID
SbSetAuthFile (IN EFI_HANDLE Fixture, IN CHAR16 *Name, IN EFI_GUID *Guid, IN CONST CHAR16 *FileName, IN EFI_STATUS Expected)
{
  UINTN      Size;
  VOID       *Data;
  EFI_STATUS Status;

  Data = SbReadFile (Fixture, FileName, &Size);
  Status = gRT->SetVariable (Name, Guid, SB_AUTH_ATTR, Size, Data);
  Print (L"SBTEST SET: %s %s -> %r\n", Name, FileName, Status);
  FreePool (Data);
  SbRequire ((BOOLEAN)(Status == Expected), L"authenticated update status");
}

VOID
SbCheckMode (IN UINT8 SetupMode, IN UINT8 SecureBoot)
{
  UINT8  Actual;
  UINTN  Size;

  Size = sizeof (Actual);
  SbStatus (gRT->GetVariable (L"SetupMode", &gEfiGlobalVariableGuid, NULL, &Size, &Actual), L"real SetupMode");
  SbRequire ((BOOLEAN)(Size == sizeof (Actual) && Actual == SetupMode), L"SetupMode value");
  Size = sizeof (Actual);
  SbStatus (gRT->GetVariable (L"SecureBoot", &gEfiGlobalVariableGuid, NULL, &Size, &Actual), L"real SecureBoot");
  SbRequire ((BOOLEAN)(Size == sizeof (Actual) && Actual == SecureBoot), L"SecureBoot value");
  Print (L"SBTEST MODE: SetupMode=%u SecureBoot=%u\n", (UINTN)SetupMode, (UINTN)SecureBoot);
}

VOID
SbCheckNoEnrollment (VOID)
{
  CHAR16     *Names[] = { L"PK", L"KEK", L"db", L"dbx" };
  EFI_GUID   *Guids[] = { &gEfiGlobalVariableGuid, &gEfiGlobalVariableGuid,
                          &gEfiImageSecurityDatabaseGuid, &gEfiImageSecurityDatabaseGuid };
  UINTN      Index;
  UINTN      Size;
  EFI_STATUS Status;

  for (Index = 0; Index < ARRAY_SIZE (Names); Index++) {
    Size = 0;
    Status = gRT->GetVariable (Names[Index], Guids[Index], NULL, &Size, NULL);
    SbRequire ((BOOLEAN)(Status == EFI_NOT_FOUND), L"setup fixture has no enrolled PK, KEK, db or dbx");
  }
}

VOID
SbCheckFirmwareIsolation (IN SB_TEST_STATE *State)
{
  SbRequire ((BOOLEAN)(State->Security->FileAuthenticationState != State->FirmwareStateReject), L"BdsApp replaced cached Security callback");
  SbRequire ((BOOLEAN)(State->Security2->FileAuthentication != State->FirmwareImageReject), L"BdsApp replaced cached Security2 callback");
  SbRequire ((BOOLEAN)(State->ImageCallsBeforeBds > 0 && State->StateCallsBeforeBds > 0), L"both rejecting firmware callbacks exercised before BdsApp");
  SbRequire ((BOOLEAN)(State->FirmwareImageCalls == State->ImageCallsBeforeBds), L"Security2 never calls original firmware rejection after BdsApp starts");
  SbRequire ((BOOLEAN)(State->FirmwareStateCalls == State->StateCallsBeforeBds), L"Security never calls original firmware rejection after BdsApp starts");
  SbRequire ((BOOLEAN)(State->PlatformSecurityCalls == 0), L"QCOM PlatformBds security initialization never called");
}

VOID
SbCheckLegacyIsolation (IN SB_TEST_STATE *State)
{
  EFI_DEVICE_PATH_PROTOCOL *Path;
  EFI_STATUS               Status;

  SbCheckFirmwareIsolation (State);
  Path = FileDevicePath (State->Fixture, L"\\EFI\\TEST\\unsigned.efi");
  SbRequire ((BOOLEAN)(Path != NULL), L"legacy Security test device path");
  Status = State->Security->FileAuthenticationState (State->Security, MAX_UINT32, Path);
  FreePool (Path);
  // Security has no image bytes; policy belongs to the Security2 verifier.
  SbStatus (Status, L"legacy Security permits images without firmware policy");
  if (State->SetupModeFixture) {
    SbStatus (State->Security->FileAuthenticationState (State->Security, MAX_UINT32, NULL), L"SecureBoot=0 legacy Security permits absent device path");
    SbStatus (State->Security2->FileAuthentication (State->Security2, NULL, NULL, 0, FALSE), L"SecureBoot=0 Security2 always permits images");
  }
  SbCheckFirmwareIsolation (State);
  Print (L"SBTEST ISOLATION PASS: Security/Security2 firmware counters unchanged; QCOM platform security calls=0\n");
}

SB_TEST_STATE *
SbGetState (VOID)
{
  SB_TEST_STATE *State;
  UINTN         Size;

  State = NULL;
  Size = sizeof (State);
  SbStatus (gRT->GetVariable (L"State", &gSbTestGuid, NULL, &Size, &State), L"volatile test state");
  SbRequire ((BOOLEAN)(Size == sizeof (State) && State != NULL && State->Magic == SB_TEST_MAGIC), L"test state shape");
  return State;
}

EFI_STATUS
SbLoadImage (IN EFI_HANDLE Parent, IN EFI_HANDLE Fixture, IN CONST CHAR16 *Name, OUT EFI_HANDLE *Image)
{
  EFI_DEVICE_PATH_PROTOCOL *Path;
  EFI_STATUS               Status;

  Path = FileDevicePath (Fixture, Name);
  SbRequire ((BOOLEAN)(Path != NULL), L"image device path");
  *Image = NULL;
  Status = gBS->LoadImage (FALSE, Parent, Path, NULL, 0, Image);
  FreePool (Path);
  return Status;
}
