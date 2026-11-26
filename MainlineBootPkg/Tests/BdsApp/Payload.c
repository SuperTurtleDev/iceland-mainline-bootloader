/** @file
  Independently booted standard EFI/BOOT/BOOTAA64.EFI assertion payload.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "TestCommon.h"

VOID
RunVariableTests (
  VOID
  );

STATIC VOID
CheckDiscoveredOption (
  IN UINT16     Number,
  OUT EFI_GUID  *PartitionGuid
  )
{
  CHAR16                    Name[9];
  UINT8                     *Data;
  UINTN                     Size;
  UINTN                     Offset;
  UINTN                     Length;
  UINTN                     End;
  UINT32                    Attributes;
  EFI_DEVICE_PATH_PROTOCOL  *Node;
  BOOLEAN                   HavePartition;
  BOOLEAN                   HaveFile;
  HARDDRIVE_DEVICE_PATH     *Hd;
  FILEPATH_DEVICE_PATH      *File;

  TestOptionName (Number, Name);
  Data = TestReadVariable (Name, &gEfiGlobalVariableGuid, &Size, &Attributes);
  TestRequire ((BOOLEAN)(Attributes == TEST_NV_ATTR && Size >= 10), L"discovered option NV layout");
  TestRequire ((BOOLEAN)(ReadUnaligned32 ((UINT32 *)Data) == LOAD_OPTION_ACTIVE), L"discovered option active BOOT");
  Offset = 6;
  while (Offset + 2 <= Size && ReadUnaligned16 ((UINT16 *)(Data + Offset)) != 0) {
    Offset += 2;
  }

  TestRequire ((BOOLEAN)(Offset + 2 <= Size), L"option description terminated");
  Offset += 2;
  End     = Offset + ReadUnaligned16 ((UINT16 *)(Data + 4));
  TestRequire ((BOOLEAN)(End == Size), L"discovered path exact size and no optional data");
  HavePartition = FALSE;
  HaveFile      = FALSE;
  while (Offset + sizeof (*Node) <= End) {
    Node   = (EFI_DEVICE_PATH_PROTOCOL *)(Data + Offset);
    Length = DevicePathNodeLength (Node);
    TestRequire ((BOOLEAN)(Length >= sizeof (*Node) && Length <= End - Offset), L"bounded discovered device path");
    if ((DevicePathType (Node) == MEDIA_DEVICE_PATH) && (DevicePathSubType (Node) == MEDIA_HARDDRIVE_DP)) {
      TestRequire ((BOOLEAN)(Length == sizeof (*Hd)), L"hard-drive node size");
      Hd = (HARDDRIVE_DEVICE_PATH *)Node;
      TestRequire ((BOOLEAN)(Hd->MBRType == MBR_TYPE_EFI_PARTITION_TABLE_HEADER && Hd->SignatureType == SIGNATURE_TYPE_GUID), L"discovered path is GPT");
      CopyMem (PartitionGuid, Hd->Signature, sizeof (*PartitionGuid));
      HavePartition = TRUE;
    }

    if ((DevicePathType (Node) == MEDIA_DEVICE_PATH) && (DevicePathSubType (Node) == MEDIA_FILEPATH_DP)) {
      TestRequire (
        (BOOLEAN)(Length >= SIZE_OF_FILEPATH_DEVICE_PATH + sizeof (CHAR16) &&
                  ReadUnaligned16 ((UINT16 *)((UINT8 *)Node + Length - 2)) == 0),
        L"bounded file path"
        );
      File = (FILEPATH_DEVICE_PATH *)Node;
      TestRequire ((BOOLEAN)(StrCmp (File->PathName, L"\\EFI\\BOOT\\BOOTAA64.EFI") == 0), L"standard removable-media path only");
      HaveFile = TRUE;
    }

    Offset += Length;
    if (IsDevicePathEnd (Node)) {
      break;
    }
  }

  TestRequire ((BOOLEAN)(HavePartition && HaveFile && Offset == End), L"discovered GPT file device path");
  FreePool (Data);
}

EFI_STATUS EFIAPI
PayloadEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  TEST_CONTEXT                 *Context;
  UINTN                        Size;
  UINT32                       Attributes;
  EFI_STATUS                   Status;
  EFI_LOADED_IMAGE_PROTOCOL    *Parent;
  UINT16                       Current;
  UINT16                       Next;
  UINT16                       *Order;
  UINTN                        Count;
  UINTN                        Index;
  UINTN                        DiscoveredStart;
  EFI_GUID                     Guid;
  BOOLEAN                      PrimarySeen;
  BOOLEAN                      SecondSeen;
  EFI_LOADED_IMAGE_PROTOCOL    *Loaded;
  EFI_PARTITION_INFO_PROTOCOL  *Partition;
  EFI_DEVICE_PATH_PROTOCOL     *Node;
  CHAR16                       *ExpectedFile;
  UINT64                       Indications;
  TEST_SNAPSHOT                Snapshot;
  TEST_SNAPSHOT                *Persisted;

  gBS->SetWatchdogTimer (0, 0, 0, NULL);
  Context = TestReadVariable (L"Armed", &gTestGuid, &Size, &Attributes);
  TestRequire ((BOOLEAN)(Size == sizeof (*Context) && Context->Magic == TEST_MAGIC && Attributes == TEST_VOL_ATTR), L"volatile BDS-start arm");
  TestStatus (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded), L"payload loaded image");
  TestRequire ((BOOLEAN)(Loaded->ParentHandle != NULL), L"payload has application parent");
  TestStatus (gBS->HandleProtocol (Loaded->ParentHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Parent), L"BdsApp parent image");
  Node = Parent->FilePath;
  while ((Node != NULL) && !IsDevicePathEnd (Node) &&
         !((DevicePathType (Node) == MEDIA_DEVICE_PATH) && (DevicePathSubType (Node) == MEDIA_FILEPATH_DP))) {
    Node = NextDevicePathNode (Node);
  }
  TestRequire ((BOOLEAN)(Node != NULL && !IsDevicePathEnd (Node) &&
               StrCmp (((FILEPATH_DEVICE_PATH *)Node)->PathName, L"\\EFI\\TEST\\BdsApp.efi") == 0), L"payload launched directly by BdsApp");
  Size = sizeof (Current);
  TestStatus (gRT->GetVariable (L"BootCurrent", &gEfiGlobalVariableGuid, NULL, &Size, &Current), L"BootCurrent present");
  TestRequire ((BOOLEAN)(Size == sizeof (Current)), L"BootCurrent UINT16");
  if (Context->ExpectedCurrent != TEST_ANY_BOOT) {
    TestRequire ((BOOLEAN)(Current == Context->ExpectedCurrent), L"BootCurrent selected expected option");
  }

  Size   = sizeof (Next);
  Status = gRT->GetVariable (L"BootNext", &gEfiGlobalVariableGuid, NULL, &Size, &Next);
  TestRequire ((BOOLEAN)(Status == EFI_NOT_FOUND), L"BootNext consumed before payload");
  if (Context->Case == CASE_NEXT_BAD) {
    TestRequire ((BOOLEAN)(Current != TEST_FIRST), L"missing BootNext number never reused for discovery");
  }

  TestStatus (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded), L"payload loaded image");
  Partition = TestPartition (Loaded->DeviceHandle);
  TestRequire ((BOOLEAN)(CompareGuid (&Partition->Info.Gpt.PartitionTypeGUID, &gEfiPartTypeSystemPartGuid)), L"payload loaded from ESP not data partition");
  TestRequire (
    (BOOLEAN)(CompareGuid (&Partition->Info.Gpt.UniquePartitionGUID, &gTestPrimaryEspGuid) ||
              (Context->Case == CASE_SCOPE && CompareGuid (&Partition->Info.Gpt.UniquePartitionGUID, &gTestSecondEspGuid))),
    L"payload from fixture ESP"
    );
  ExpectedFile = L"\\EFI\\BOOT\\BOOTAA64.EFI";
  if (Context->ExpectedCurrent == TEST_SECOND) {
    ExpectedFile = L"\\EFI\\TEST\\Second.efi";
  } else if ((Context->ExpectedCurrent == TEST_FIRST) || (Context->ExpectedCurrent == TEST_FOURTH)) {
    ExpectedFile = L"\\EFI\\TEST\\First.efi";
  }

  Node = Loaded->FilePath;
  while (Node != NULL && !IsDevicePathEnd (Node)) {
    if ((DevicePathType (Node) == MEDIA_DEVICE_PATH) && (DevicePathSubType (Node) == MEDIA_FILEPATH_DP)) {
      TestRequire ((BOOLEAN)(StrCmp (((FILEPATH_DEVICE_PATH *)Node)->PathName, ExpectedFile) == 0), L"payload file matches selected boot option");
      break;
    }

    Node = NextDevicePathNode (Node);
  }

  TestRequire ((BOOLEAN)(Node != NULL && !IsDevicePathEnd (Node)), L"payload file path present");

  Order = TestReadVariable (L"BootOrder", &gEfiGlobalVariableGuid, &Size, &Attributes);
  Count = Size / sizeof (UINT16);
  TestRequire ((BOOLEAN)(Attributes == TEST_NV_ATTR && Size == Context->OrderCount * sizeof (UINT16)), L"BootOrder preserves prefix and appends only ESP loaders");
  for (Index = 0; Index < Context->PrefixCount; Index++) {
    TestRequire ((BOOLEAN)(Order[Index] == Context->Prefix[Index]), L"existing BootOrder prefix unchanged");
  }

  DiscoveredStart = Context->PrefixCount;
  if ((Context->Case == CASE_NV_NEXT) && (Context->Phase == 2)) {
    DiscoveredStart = 1;
  }

  PrimarySeen = FALSE;
  SecondSeen  = FALSE;
  for (Index = DiscoveredStart; Index < Count; Index++) {
    CheckDiscoveredOption (Order[Index], &Guid);
    if (CompareGuid (&Guid, &gTestPrimaryEspGuid)) {
      TestRequire ((BOOLEAN) !PrimarySeen, L"no duplicate primary ESP option");
      PrimarySeen = TRUE;
    } else {
      TestRequire ((BOOLEAN)(Context->Case == CASE_SCOPE && CompareGuid (&Guid, &gTestSecondEspGuid) && !SecondSeen), L"no non-ESP or duplicate discovered option");
      SecondSeen = TRUE;
    }
  }

  TestRequire ((BOOLEAN)(PrimarySeen && (Context->Case != CASE_SCOPE || SecondSeen)), L"all expected ESPs registered");
  if (Context->ExpectedCurrent == TEST_ANY_BOOT) {
    for (Index = 0; Index < Count && Order[Index] != Current; Index++) {
    }

    TestRequire ((BOOLEAN)(Index < Count), L"BootCurrent is a discovered option");
  }

  FreePool (Order);
  if (Context->Case == CASE_MENU) {
    Size = sizeof (Indications);
    TestStatus (gRT->GetVariable (L"OsIndications", &gEfiGlobalVariableGuid, &Attributes, &Size, &Indications), L"remaining OsIndications");
    TestRequire ((BOOLEAN)(Size == sizeof (Indications) && Attributes == TEST_NV_ATTR && Indications == 0x8), L"only OsIndications firmware-UI bit cleared");
    Print (L"BDSTEST OsIndications: 0x9 -> 0x8\n");
  }

  if ((Context->Case == CASE_NV) || (Context->Case == CASE_NV_NEXT)) {
    if (Context->Phase == 1) {
      TestSnapshot (&Snapshot);
      TestStatus (gRT->SetVariable (L"Snapshot", &gTestGuid, TEST_NV_ATTR, sizeof (Snapshot), &Snapshot), L"persist expected boot variables");
    } else {
      Persisted = TestReadVariable (L"Snapshot", &gTestGuid, &Size, NULL);
      TestRequire ((BOOLEAN)(Size == sizeof (*Persisted)), L"snapshot length");
      TestVerifySnapshot (Persisted);
      FreePool (Persisted);
    }
  }

  RunVariableTests ();
  Print (L"BDSTEST ASSERT: BdsApp-parent BootCurrent=%04x BootNext=absent order=%u\n", (UINTN)Current, Context->OrderCount);
  if (Context->Case == CASE_RETURN_MENU) {
    Print (L"BDSTEST RETURN: child returns EFI_SUCCESS to BdsApp\n");
    FreePool (Context);
    return EFI_SUCCESS;
  }
  Print (L"BDSTEST PASS: %a phase=%u\n", Context->Name, Context->Phase);
  TestDelete (L"Armed", &gTestGuid);
  TestShutdown ();
  return EFI_SUCCESS;
}
