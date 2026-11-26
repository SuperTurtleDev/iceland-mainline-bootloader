/** @file
  Exercise production variable commands against real AAVMF runtime services.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "TestCommon.h"
#include <Library/UefiVariableCommandsLib.h>

STATIC EFI_GUID  mVariableGuid = {
  0xa52e7a10, 0x7259, 0x4bd1, { 0x8a, 0x64, 0xc3, 0x40, 0xb7, 0xc2, 0x62, 0x39 }
};
STATIC EFI_GUID  mOtherGuid = {
  0xb52e7a10, 0x7259, 0x4bd1, { 0x8a, 0x64, 0xc3, 0x40, 0xb7, 0xc2, 0x62, 0x39 }
};
STATIC UINTN     mRecords;
STATIC UINTN     mDataBytes;

STATIC VOID
VariableInfo (
  IN CONST CHAR8  *Text
  )
{
  UINTN  Length = AsciiStrLen (Text);

  TestRequire ((BOOLEAN)(Length <= 60), L"variable INFO payload fits 60 bytes");
  if (AsciiStrnCmp (Text, "begin guid=", 11) == 0) {
    mRecords++;
  }

  if (AsciiStrnCmp (Text, "hex@", 4) == 0) {
    TestRequire ((BOOLEAN)(Length >= 13 && ((Length - 13) % 2) == 0), L"hex INFO fragment shape");
    mDataBytes += (Length - 13) / 2;
  }
}

STATIC VOID
Command (
  IN CONST CHAR8  *Verb,
  IN CONST CHAR8  *Arguments,
  IN EFI_STATUS   Expected
  )
{
  CHAR8       Error[61];
  EFI_STATUS  Status;

  mRecords = mDataBytes = 0;
  Status   = UefiVariableCommand (Verb, Arguments, VariableInfo, Error, sizeof (Error));
  if (Status != Expected) {
    Print (L"BDSTEST variable %a %a: %r, expected %r (%a)\n", Verb, Arguments, Status, Expected, Error);
  }

  TestRequire ((BOOLEAN)(Status == Expected), L"variable command expected status");
}

STATIC VOID
Check (
  IN CHAR16       *Name,
  IN CONST UINT8  *Expected,
  IN UINTN        Length,
  IN UINT32       ExpectedAttributes
  )
{
  UINT8   *Data;
  UINTN   Size;
  UINT32  Attributes;

  Data = TestReadVariable (Name, &mVariableGuid, &Size, &Attributes);
  TestRequire ((BOOLEAN)(Size == Length && Attributes == ExpectedAttributes && CompareMem (Data, Expected, Length) == 0), L"typed variable bytes and attributes");
  FreePool (Data);
}

VOID
RunVariableTests (
  VOID
  )
{
  CONST CHAR8  *Qualified = "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarTest";
  UINT8        Bytes[80];
  UINTN        Index;
  UINTN        Size;
  UINT32       Attributes;
  CHAR8        Args[512];
  CHAR16       LongName[200];
  CHAR8        LongArgs[512];

  TestDelete (L"BdsVarTest", &mVariableGuid);
  TestDelete (L"BdsVarTest", &mOtherGuid);
  TestDelete (L"BdsVarVolatile", &mVariableGuid);
  Command ("setvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarTest u8:255", EFI_SUCCESS);
  Bytes[0] = 255;
  Check (L"BdsVarTest", Bytes, 1, TEST_NV_ATTR);
  Command ("setvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarTest u16:0x1234", EFI_SUCCESS);
  Bytes[0] = 0x34;
  Bytes[1] = 0x12;
  Check (L"BdsVarTest", Bytes, 2, TEST_NV_ATTR);
  Command ("setvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarTest u32:305419896", EFI_SUCCESS);
  Bytes[0] = 0x78;
  Bytes[1] = 0x56;
  Bytes[2] = 0x34;
  Bytes[3] = 0x12;
  Check (L"BdsVarTest", Bytes, 4, TEST_NV_ATTR);
  Command ("setvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarTest u64:18446744073709551615", EFI_SUCCESS);
  SetMem (Bytes, 8, 255);
  Check (L"BdsVarTest", Bytes, 8, TEST_NV_ATTR);
  Command ("setvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarTest u8:256", EFI_INVALID_PARAMETER);
  Command ("setvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarTest u64:18446744073709551616", EFI_INVALID_PARAMETER);
  Command ("setvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarTest hex:", EFI_INVALID_PARAMETER);
  Command ("setvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarTest hex:001", EFI_INVALID_PARAMETER);
  Command ("setvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarTest hex:0z", EFI_INVALID_PARAMETER);
  Check (L"BdsVarTest", Bytes, 8, TEST_NV_ATTR);
  Command ("setvar", "BdsVarTest u8:1", EFI_INVALID_PARAMETER);
  Command ("setvar", "\"a52e7a10-7259-4bd1-8a64-c340b7c26239-Name With Spaces\" utf8:value", EFI_SUCCESS);
  Command ("getvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-Name With Spaces", EFI_SUCCESS);
  Command ("rmvar", "Name With Spaces", EFI_SUCCESS);
  Command ("setvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarTest utf16:A\xE4\xB8\xAD", EFI_SUCCESS);
  Bytes[0] = 'A';
  Bytes[1] = 0;
  Bytes[2] = 0x2D;
  Bytes[3] = 0x4E;
  Bytes[4] = 0;
  Bytes[5] = 0;
  Check (L"BdsVarTest", Bytes, 6, TEST_NV_ATTR);
  Command ("setvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarTest utf8:hello world", EFI_SUCCESS);
  Check (L"BdsVarTest", (CONST UINT8 *)"hello world", 12, TEST_NV_ATTR);
  Command ("setvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarTest utf16:\xc0\x80", EFI_INVALID_PARAMETER);
  Command ("setvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarVolatile attrs=BS u32:1", EFI_SUCCESS);
  Bytes[0] = 1;
  Bytes[1] = Bytes[2] = Bytes[3] = 0;
  Check (L"BdsVarVolatile", Bytes, 4, EFI_VARIABLE_BOOTSERVICE_ACCESS);
  Command ("setvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarVolatile u32:2", EFI_SUCCESS);
  Bytes[0] = 2;
  Check (L"BdsVarVolatile", Bytes, 4, EFI_VARIABLE_BOOTSERVICE_ACCESS);
  Command ("setvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarVolatile attrs=RT u8:1", EFI_INVALID_PARAMETER);
  Command ("getvar", Qualified, EFI_SUCCESS);
  TestRequire ((BOOLEAN)(mRecords == 1 && mDataBytes == 12), L"qualified getvar framing");
  Command ("getvar", "BdsVarTest", EFI_SUCCESS);
  TestRequire ((BOOLEAN)(mRecords == 1), L"name lookup finds vendor namespace");
  TestStatus (gRT->SetVariable (L"BdsVarTest", &mOtherGuid, TEST_NV_ATTR, 4, Bytes), L"create ambiguous namespace");
  Command ("getvar", "BdsVarTest", EFI_SUCCESS);
  TestRequire ((BOOLEAN)(mRecords == 2), L"getvar returns both namespaces");
  Command ("rmvar", "BdsVarTest", EFI_NO_MAPPING);
  Size = sizeof (Bytes);
  TestStatus (gRT->GetVariable (L"BdsVarTest", &mOtherGuid, &Attributes, &Size, Bytes), L"ambiguous rmvar did not delete");
  Command ("rmvar", "b52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarTest", EFI_SUCCESS);
  Command ("rmvar", "BdsVarTest", EFI_SUCCESS);
  Command ("getvar", Qualified, EFI_NOT_FOUND);
  AsciiSPrint (Args, sizeof (Args), "%a hex:", Qualified);
  for (Index = 0; Index < 80; Index++) {
    AsciiSPrint (Args + AsciiStrLen (Args), sizeof (Args) - AsciiStrLen (Args), "%02x", (UINT8)Index);
    Bytes[Index] = (UINT8)Index;
  }

  Command ("setvar", Args, EFI_SUCCESS);
  Check (L"BdsVarTest", Bytes, 80, TEST_NV_ATTR);
  Command ("getvar", Qualified, EFI_SUCCESS);
  TestRequire ((BOOLEAN)(mDataBytes == 80), L"all data emitted without truncation");
  for (Index = 0; Index < 190; Index++) {
    LongName[Index] = L'Z';
  }

  LongName[190] = 0;
  TestStatus (gRT->SetVariable (LongName, &mVariableGuid, TEST_NV_ATTR, 1, Bytes), L"long variable name");
  AsciiSPrint (LongArgs, sizeof (LongArgs), "a52e7a10-7259-4bd1-8a64-c340b7c26239-");
  for (Index = 0; Index < 190; Index++) {
    LongArgs[37 + Index] = 'Z';
  }

  LongArgs[227] = 0;
  Command ("getvar", "all", EFI_SUCCESS);
  TestRequire ((BOOLEAN)(mRecords > 2), L"all enumeration handles larger name buffer");
  Command ("rmvar", LongArgs, EFI_SUCCESS);
  Command ("rmvar", Qualified, EFI_SUCCESS);
  Command ("rmvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarVolatile", EFI_SUCCESS);
  Print (L"BDSTEST VARIABLES PASS: types, attributes, ambiguity, bounds, enumeration\n");
}
