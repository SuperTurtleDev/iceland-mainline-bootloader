/** @file
  Assertions deliberately independent of BdsApp and its boot manager library.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "TestCommon.h"

EFI_GUID  gTestGuid = {
  0x51cfd9eb, 0x8488, 0x4b45, { 0xb8, 0x1c, 0x80, 0x18, 0xa5, 0x71, 0x9c, 0x32 }
};
EFI_GUID  gTestPrimaryEspGuid = {
  0x11111111, 0x2222, 0x4333, { 0x84, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x51 }
};
EFI_GUID  gTestSecondEspGuid = {
  0x11111111, 0x2222, 0x4333, { 0x84, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x52 }
};

VOID
TestShutdown (
  VOID
  )
{
  gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
  CpuDeadLoop ();
}

VOID
TestRequire (
  IN BOOLEAN       Condition,
  IN CONST CHAR16  *Message
  )
{
  if (!Condition) {
    Print (L"BDSTEST FAIL: %s\n", Message);
    TestShutdown ();
  }
}

VOID
TestStatus (
  IN EFI_STATUS    Status,
  IN CONST CHAR16  *Message
  )
{
  if (EFI_ERROR (Status)) {
    Print (L"BDSTEST FAIL: %s: %r\n", Message, Status);
    TestShutdown ();
  }
}

VOID *
TestReadVariable (
  IN CHAR16    *Name,
  IN EFI_GUID  *Guid,
  OUT UINTN    *Size,
  OUT UINT32   *Attributes OPTIONAL
  )
{
  VOID        *Data;
  EFI_STATUS  Status;

  *Size  = 0;
  Status = gRT->GetVariable (Name, Guid, Attributes, Size, NULL);
  TestRequire ((BOOLEAN)(Status == EFI_BUFFER_TOO_SMALL && *Size <= SIZE_1MB), Name);
  Data = AllocatePool (*Size);
  TestRequire ((BOOLEAN)(Data != NULL), L"allocate variable");
  TestStatus (gRT->GetVariable (Name, Guid, Attributes, Size, Data), Name);
  return Data;
}

VOID
TestDelete (
  IN CHAR16    *Name,
  IN EFI_GUID  *Guid
  )
{
  EFI_STATUS  Status;

  Status = gRT->SetVariable (Name, Guid, 0, 0, NULL);
  if (Status != EFI_NOT_FOUND) {
    TestStatus (Status, Name);
  }
}

VOID
TestOptionName (
  IN UINT16   Number,
  OUT CHAR16  *Name
  )
{
  UnicodeSPrint (Name, 9 * sizeof (CHAR16), L"Boot%04x", (UINTN)Number);
}

VOID
TestSnapshot (
  OUT TEST_SNAPSHOT  *Snapshot
  )
{
  UINT16  *Order;
  UINTN   Size;
  UINTN   Index;
  UINT32  Attributes;
  VOID    *Data;
  CHAR16  Name[9];

  ZeroMem (Snapshot, sizeof (*Snapshot));
  Snapshot->Magic = TEST_MAGIC;
  Order           = TestReadVariable (L"BootOrder", &gEfiGlobalVariableGuid, &Size, &Attributes);
  TestRequire ((BOOLEAN)(Size % sizeof (UINT16) == 0 && Size <= sizeof (Snapshot->Order)), L"bounded BootOrder");
  TestRequire ((BOOLEAN)(Attributes == TEST_NV_ATTR), L"BootOrder is nonvolatile");
  Snapshot->Count = (UINT16)(Size / sizeof (UINT16));
  CopyMem (Snapshot->Order, Order, Size);
  FreePool (Order);
  for (Index = 0; Index < Snapshot->Count; Index++) {
    TestOptionName (Snapshot->Order[Index], Name);
    Data = TestReadVariable (Name, &gEfiGlobalVariableGuid, &Size, &Attributes);
    TestRequire ((BOOLEAN)(Attributes == TEST_NV_ATTR), L"Boot option is nonvolatile");
    TestStatus (gBS->CalculateCrc32 (Data, Size, &Snapshot->Crc[Index]), L"option CRC");
    FreePool (Data);
  }
}

VOID
TestVerifySnapshot (
  IN TEST_SNAPSHOT  *Expected
  )
{
  TEST_SNAPSHOT  Actual;

  TestSnapshot (&Actual);
  TestRequire ((BOOLEAN)(CompareMem (&Actual, Expected, sizeof (Actual)) == 0), L"NV BootOrder and Boot#### survive unchanged");
}

EFI_PARTITION_INFO_PROTOCOL *
TestPartition (
  IN EFI_HANDLE  Handle
  )
{
  EFI_PARTITION_INFO_PROTOCOL  *Info;

  TestStatus (gBS->HandleProtocol (Handle, &gEfiPartitionInfoProtocolGuid, (VOID **)&Info), L"GPT partition protocol");
  TestRequire ((BOOLEAN)(Info->Type == PARTITION_TYPE_GPT), L"genuine GPT partition");
  return Info;
}
