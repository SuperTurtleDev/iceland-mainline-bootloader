/** @file
  QEMU fw_cfg entry point. Seed variables after the host BDS has selected us.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "TestCommon.h"

#ifdef BDS_TEST_DIAGNOSTICS
  #include <PiDxe.h>
  #include <Guid/FileInfo.h>
  #include <Guid/EventGroup.h>
  #include <Library/DxeServicesLib.h>

STATIC VOID EFIAPI
DiagnoseRead (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Path;
  EFI_DEVICE_PATH_PROTOCOL  *Remaining;
  EFI_FILE_PROTOCOL         *File;
  EFI_FILE_INFO             *Info;
  EFI_STATUS                Status;
  UINTN                     Size;
  UINT8                     Header[4096];
  UINTN                     Pe;
  UINT16                    Current;
  CHAR16                    Name[9];
  CHAR16                    *Text;
  UINT8                     *Option;
  UINTN                     Offset;
  UINT32                    Auth;
  VOID                      *Buffer;

  Path = FileDevicePath ((EFI_HANDLE)Context, L"\\EFI\\BOOT\\BOOTAA64.EFI");
  if (Event != NULL) {
    Size = sizeof (Current);
    TestStatus (gRT->GetVariable (L"BootCurrent", &gEfiGlobalVariableGuid, NULL, &Size, &Current), L"diagnostic BootCurrent");
    TestOptionName (Current, Name);
    Option = TestReadVariable (Name, &gEfiGlobalVariableGuid, &Size, NULL);
    Offset = 6;
    while (Offset + 2 <= Size && ReadUnaligned16 ((UINT16 *)(Option + Offset)) != 0) {
      Offset += 2;
    }

    Offset += 2;
    TestRequire ((BOOLEAN)(Offset + ReadUnaligned16 ((UINT16 *)(Option + 4)) <= Size), L"diagnostic load option length");
    FreePool (Path);
    Path = DuplicateDevicePath ((EFI_DEVICE_PATH_PROTOCOL *)(Option + Offset));
    FreePool (Option);
    Print (L"BDSTEST DIAG: ReadyToBoot BootCurrent=%04x\n", (UINTN)Current);
  }

  Text = ConvertDevicePathToText (Path, FALSE, FALSE);
  Print (L"BDSTEST DIAG: exact path %s\n", Text);
  FreePool (Text);
  Size   = 0;
  Buffer = GetFileBufferByFilePath (TRUE, Path, &Size, &Auth);
  Print (L"BDSTEST DIAG: GetFileBufferByFilePath buffer=%p size=%u auth=%x\n", Buffer, Size, Auth);
  if (Buffer != NULL) {
    FreePool (Buffer);
  }

  Remaining = Path;
  Status    = EfiOpenFileByDevicePath (&Remaining, &File, EFI_FILE_MODE_READ, 0);
  Print (L"BDSTEST DIAG: open payload %r\n", Status);
  if (!EFI_ERROR (Status)) {
    Size   = 0;
    Status = File->GetInfo (File, &gEfiFileInfoGuid, &Size, NULL);
    Print (L"BDSTEST DIAG: GetInfo sizing %r size=%u\n", Status, Size);
    if (Status == EFI_BUFFER_TOO_SMALL) {
      Info   = AllocatePool (Size);
      Status = File->GetInfo (File, &gEfiFileInfoGuid, &Size, Info);
      Print (L"BDSTEST DIAG: GetInfo read %r file-size=%lu attrs=%lx\n", Status, Info->FileSize, Info->Attribute);
      FreePool (Info);
    }

    Size   = sizeof (Header);
    Status = File->Read (File, &Size, Header);
    Pe     = ReadUnaligned32 ((UINT32 *)(Header + 0x3c));
    Print (L"BDSTEST DIAG: read %r bytes=%u MZ=%x PE-offset=%x\n", Status, Size, ReadUnaligned16 ((UINT16 *)Header), Pe);
    if (Pe + 94 <= Size) {
      Print (L"BDSTEST DIAG: signature=%x magic=%x subsystem=%u\n", ReadUnaligned32 ((UINT32 *)(Header + Pe)), ReadUnaligned16 ((UINT16 *)(Header + Pe + 24)), ReadUnaligned16 ((UINT16 *)(Header + Pe + 92)));
    }

    File->Close (File);
  }

  FreePool (Path);
}

#endif

STATIC EFI_HANDLE
FindFixture (
  VOID
  )
{
  EFI_HANDLE                   *Handles;
  EFI_HANDLE                   Found;
  EFI_PARTITION_INFO_PROTOCOL  *Info;
  UINTN                        Count;
  UINTN                        Index;
  EFI_STATUS                   Status;

  TestStatus (gBS->LocateHandleBuffer (AllHandles, NULL, NULL, &Count, &Handles), L"enumerate controllers");
  for (Index = 0; Index < Count; Index++) {
    gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
  }

  FreePool (Handles);
  TestStatus (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles), L"enumerate filesystems");
  Found = NULL;
  for (Index = 0; Index < Count; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiPartitionInfoProtocolGuid, (VOID **)&Info);
    if (!EFI_ERROR (Status) && (Info->Type == PARTITION_TYPE_GPT) &&
        CompareGuid (&Info->Info.Gpt.PartitionTypeGUID, &gEfiPartTypeSystemPartGuid) &&
        CompareGuid (&Info->Info.Gpt.UniquePartitionGUID, &gTestPrimaryEspGuid))
    {
      TestRequire ((BOOLEAN)(Found == NULL), L"unique fixture ESP");
      Found = Handles[Index];
    }
  }

  FreePool (Handles);
  TestRequire ((BOOLEAN)(Found != NULL), L"fixture GPT ESP located");
  Print (L"BDSTEST fixture: genuine GPT ESP GUID matched\n");
  return Found;
}

STATIC VOID
ReadCase (
  IN EFI_HANDLE     Fixture,
  OUT TEST_CONTEXT  *Context
  )
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *File;
  UINTN                            Size;
  UINTN                            Index;
  STATIC CONST CHAR8               *Names[] = {
    "discovery",        "bootnext",  "order-fallback",     "skip-invalid-inactive",
    "bootnext-missing", "esp-scope", "osindications-menu", "nv-discovery", "nv-bootnext",
    "return-menu", "failure-menu"
  };

  ZeroMem (Context, sizeof (*Context));
  Context->Magic           = TEST_MAGIC;
  Context->ExpectedCurrent = TEST_ANY_BOOT;
  TestStatus (gBS->HandleProtocol (Fixture, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs), L"fixture filesystem");
  TestStatus (Fs->OpenVolume (Fs, &Root), L"open fixture root");
  TestStatus (Root->Open (Root, &File, L"\\EFI\\TEST\\case.txt", EFI_FILE_MODE_READ, 0), L"open test case");
  Size = sizeof (Context->Name) - 1;
  TestStatus (File->Read (File, &Size, Context->Name), L"read test case");
  File->Close (File);
  Root->Close (Root);
  for (Index = 0; Index < ARRAY_SIZE (Names); Index++) {
    if (AsciiStrCmp (Context->Name, Names[Index]) == 0) {
      Context->Case = (UINT32)Index + 1;
      break;
    }
  }

  TestRequire ((BOOLEAN)(Context->Case != 0), L"known test case");
}

STATIC VOID
CheckScopeFixture (
  VOID
  )
{
  EFI_HANDLE                   *Handles;
  EFI_PARTITION_INFO_PROTOCOL  *Info;
  EFI_GUID                     DataGuid = {
    0x11111111, 0x2222, 0x4333, { 0x84, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x53 }
  };
  EFI_STATUS                   Status;
  UINTN                        Count;
  UINTN                        Index;
  BOOLEAN                      Second;
  BOOLEAN                      NonEsp;

  Second = FALSE;
  NonEsp = FALSE;
  TestStatus (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles), L"scope fixture filesystems");
  for (Index = 0; Index < Count; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiPartitionInfoProtocolGuid, (VOID **)&Info);
    if (EFI_ERROR (Status) || (Info->Type != PARTITION_TYPE_GPT)) {
      continue;
    }

    if (CompareGuid (&Info->Info.Gpt.UniquePartitionGUID, &gTestSecondEspGuid)) {
      TestRequire (CompareGuid (&Info->Info.Gpt.PartitionTypeGUID, &gEfiPartTypeSystemPartGuid), L"second fixture is ESP");
      Second = TRUE;
    }

    if (CompareGuid (&Info->Info.Gpt.UniquePartitionGUID, &DataGuid)) {
      TestRequire ((BOOLEAN) !CompareGuid (&Info->Info.Gpt.PartitionTypeGUID, &gEfiPartTypeSystemPartGuid), L"data fixture is not ESP");
      NonEsp = TRUE;
    }
  }

  FreePool (Handles);
  TestRequire ((BOOLEAN)(Second && NonEsp), L"second ESP and non-ESP FAT both visible to firmware");
  Print (L"BDSTEST fixture: second ESP and non-ESP FAT both visible\n");
}

STATIC VOID
WriteOption (
  IN EFI_HANDLE  Fixture,
  IN UINT16      Number,
  IN UINT32      Attributes,
  IN CHAR16      *FileName
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Path;
  CHAR16                    Name[9];
  CHAR16                    Description[48];
  UINT8                     *Data;
  UINTN                     DescriptionSize;
  UINTN                     PathSize;
  UINTN                     Size;

  Path = FileDevicePath (Fixture, FileName);
  TestRequire ((BOOLEAN)(Path != NULL), L"build option device path");
  UnicodeSPrint (Description, sizeof (Description), L"BdsApp QEMU Boot%04x", (UINTN)Number);
  DescriptionSize = StrSize (Description);
  PathSize        = GetDevicePathSize (Path);
  TestRequire ((BOOLEAN)(PathSize <= MAX_UINT16), L"bounded option path");
  Size = sizeof (UINT32) + sizeof (UINT16) + DescriptionSize + PathSize;
  Data = AllocateZeroPool (Size);
  TestRequire ((BOOLEAN)(Data != NULL), L"allocate load option");
  WriteUnaligned32 ((UINT32 *)Data, Attributes);
  WriteUnaligned16 ((UINT16 *)(Data + sizeof (UINT32)), (UINT16)PathSize);
  CopyMem (Data + 6, Description, DescriptionSize);
  CopyMem (Data + 6 + DescriptionSize, Path, PathSize);
  TestOptionName (Number, Name);
  TestStatus (gRT->SetVariable (Name, &gEfiGlobalVariableGuid, TEST_NV_ATTR, Size, Data), Name);
  FreePool (Data);
  FreePool (Path);
}

STATIC VOID
Seed (
  IN EFI_HANDLE        Fixture,
  IN OUT TEST_CONTEXT  *Context
  )
{
  UINTN          Index;
  UINTN          Size;
  UINT16         Next;
  UINT64         Indications;
  CHAR16         Name[9];
  EFI_STATUS     Status;
  TEST_SNAPSHOT  Snapshot;

  Context->Phase = 1;
  if ((Context->Case == CASE_NV) || (Context->Case == CASE_NV_NEXT)) {
    Size   = sizeof (Snapshot);
    Status = gRT->GetVariable (L"Snapshot", &gTestGuid, NULL, &Size, &Snapshot);
    if (Status != EFI_NOT_FOUND) {
      TestStatus (Status, L"read persistent snapshot");
      TestRequire ((BOOLEAN)(Size == sizeof (Snapshot) && Snapshot.Magic == TEST_MAGIC), L"persistent snapshot shape");
      TestVerifySnapshot (&Snapshot);
      Size   = 0;
      Status = gRT->GetVariable (L"BootNext", &gEfiGlobalVariableGuid, NULL, &Size, NULL);
      TestRequire ((BOOLEAN)(Status == EFI_NOT_FOUND), L"BootNext deletion persisted across QEMU restart");
      Context->Phase           = 2;
      Context->OrderCount      = Snapshot.Count;
      Context->ExpectedCurrent = Snapshot.Order[0];
      Print (L"BDSTEST NV VERIFIED: BootOrder/Boot#### unchanged; BootNext absent\n");
      return;
    }
  }

  TestDelete (L"BootNext", &gEfiGlobalVariableGuid);
  TestDelete (L"BootOrder", &gEfiGlobalVariableGuid);
  TestDelete (L"BootCurrent", &gEfiGlobalVariableGuid);
  TestDelete (L"OsIndications", &gEfiGlobalVariableGuid);
  for (Index = TEST_FIRST; Index <= TEST_FOURTH; Index++) {
    TestOptionName ((UINT16)Index, Name);
    TestDelete (Name, &gEfiGlobalVariableGuid);
  }

  Context->OrderCount = 1;
  switch (Context->Case) {
    case CASE_NEXT:
    case CASE_NV_NEXT:
      WriteOption (Fixture, TEST_FIRST, LOAD_OPTION_ACTIVE, L"\\EFI\\TEST\\First.efi");
      // BootNext must honor both an inactive option and category APP.
      WriteOption (Fixture, TEST_SECOND, LOAD_OPTION_CATEGORY_APP, L"\\EFI\\TEST\\Second.efi");
      Context->Prefix[0]       = TEST_FIRST;
      Context->PrefixCount     = 1;
      Context->ExpectedCurrent = TEST_SECOND;
      Context->OrderCount      = 2;
      Next                     = TEST_SECOND;
      TestStatus (gRT->SetVariable (L"BootNext", &gEfiGlobalVariableGuid, TEST_NV_ATTR, sizeof (Next), &Next), L"seed BootNext");
      break;
    case CASE_FALLBACK:
      WriteOption (Fixture, TEST_FIRST, LOAD_OPTION_ACTIVE, L"\\EFI\\TEST\\Missing.efi");
      WriteOption (Fixture, TEST_SECOND, LOAD_OPTION_ACTIVE, L"\\EFI\\TEST\\Second.efi");
      Context->Prefix[0]       = TEST_FIRST;
      Context->Prefix[1]       = TEST_SECOND;
      Context->PrefixCount     = 2;
      Context->ExpectedCurrent = TEST_SECOND;
      Context->OrderCount      = 3;
      break;
    case CASE_SKIP:
      // Missing BootA901 is invalid. Inactive and APP entries point to a real
      // payload so incorrectly executing either is detected by BootCurrent.
      WriteOption (Fixture, TEST_SECOND, 0, L"\\EFI\\TEST\\Second.efi");
      WriteOption (Fixture, TEST_THIRD, LOAD_OPTION_ACTIVE | LOAD_OPTION_CATEGORY_APP, L"\\EFI\\TEST\\Second.efi");
      WriteOption (Fixture, TEST_FOURTH, LOAD_OPTION_ACTIVE, L"\\EFI\\TEST\\First.efi");
      for (Index = 0; Index < 4; Index++) {
        Context->Prefix[Index] = (UINT16)(TEST_FIRST + Index);
      }

      Context->PrefixCount     = 4;
      Context->ExpectedCurrent = TEST_FOURTH;
      Context->OrderCount      = 5;
      break;
    case CASE_NEXT_BAD:
      Next = TEST_FIRST;
      TestStatus (gRT->SetVariable (L"BootNext", &gEfiGlobalVariableGuid, TEST_NV_ATTR, sizeof (Next), &Next), L"seed missing BootNext");
      break;
    case CASE_SCOPE:
      Context->OrderCount = 2;
      break;
    case CASE_MENU:
      // Bit zero requests firmware UI. Bit three must survive consumption.
      Indications = 0x9;
      TestStatus (gRT->SetVariable (L"OsIndications", &gEfiGlobalVariableGuid, TEST_NV_ATTR, sizeof (Indications), &Indications), L"seed OsIndications");
      break;
    default:
      break;
  }

  if (Context->PrefixCount != 0) {
    TestStatus (
      gRT->SetVariable (
             L"BootOrder",
             &gEfiGlobalVariableGuid,
             TEST_NV_ATTR,
             Context->PrefixCount * sizeof (UINT16),
             Context->Prefix
             ),
      L"seed BootOrder"
      );
  }
}

EFI_STATUS EFIAPI
LauncherEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HANDLE                Fixture;
  EFI_HANDLE                BdsImage;
  EFI_DEVICE_PATH_PROTOCOL  *Path;
  TEST_CONTEXT              Context;
  EFI_STATUS                Status;

  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  Print (L"BDSTEST launcher starting\n");
  Fixture = FindFixture ();
  ReadCase (Fixture, &Context);
  if (Context.Case == CASE_SCOPE) {
    CheckScopeFixture ();
  }

  Seed (Fixture, &Context);
 #ifdef BDS_TEST_DIAGNOSTICS
  {
    EFI_EVENT   DiagnosticEvent;
    EFI_HANDLE  Probe;

    DiagnoseRead (NULL, Fixture);
    Path   = FileDevicePath (Fixture, L"\\EFI\\BOOT\\BOOTAA64.EFI");
    Status = gBS->LoadImage (TRUE, ImageHandle, Path, NULL, 0, &Probe);
    Print (L"BDSTEST DIAG: direct LoadImage only, no StartImage: %r\n", Status);
    if (!EFI_ERROR (Status)) {
      gBS->UnloadImage (Probe);
    }

    FreePool (Path);
    TestStatus (
      gBS->CreateEventEx (
             EVT_NOTIFY_SIGNAL,
             TPL_CALLBACK,
             DiagnoseRead,
             Fixture,
             &gEfiEventReadyToBootGuid,
             &DiagnosticEvent
             ),
      L"diagnostic event"
      );
  }
 #endif
  // Volatile arm cannot survive reboot. An outer-firmware fallback into the
  // payload therefore fails, including after the NV verification restart.
  TestStatus (gRT->SetVariable (L"Armed", &gTestGuid, TEST_VOL_ATTR, sizeof (Context), &Context), L"arm BDS invocation");
  Print (L"BDSTEST ARMED: %a phase=%u\n", Context.Name, Context.Phase);
  Path = FileDevicePath (Fixture, L"\\EFI\\TEST\\BdsApp.efi");
  TestRequire ((BOOLEAN)(Path != NULL), L"BdsApp device path");
  TestStatus (gBS->LoadImage (FALSE, ImageHandle, Path, NULL, 0, &BdsImage), L"load BdsApp");
  FreePool (Path);
  Status = gBS->StartImage (BdsImage, NULL, NULL);
  Print (L"BDSTEST FAIL: BdsApp returned %r without payload shutdown\n", Status);
  TestShutdown ();
  return EFI_ABORTED;
}
