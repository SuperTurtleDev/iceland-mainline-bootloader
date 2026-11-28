/** @file
  Exercise production variable commands against real AAVMF runtime services.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "TestCommon.h"
#include <Library/BdsSecurityLib.h>
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
CommandWithData (
  IN CONST CHAR8  *Arguments,
  IN CONST VOID   *Data OPTIONAL,
  IN UINTN        Size,
  IN EFI_STATUS   Expected
  )
{
  CHAR8       Error[61];
  EFI_STATUS  Status;

  mRecords = mDataBytes = 0;
  Status   = UefiVariableCommandWithData (
               "setvar", Arguments, VariableInfo, Error, sizeof (Error), Data, Size
               );
  if (Status != Expected) {
    Print (L"BDSTEST downloaded variable %a: %r, expected %r (%a)\n", Arguments, Status, Expected, Error);
  }

  TestRequire ((BOOLEAN)(Status == Expected), L"downloaded variable command expected status");
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

STATIC VOID
CheckReadOnlyStatus (
  IN CHAR16       *Name,
  IN CONST CHAR8  *AsciiName,
  IN EFI_GUID     *Guid
  )
{
  EFI_STATUS  BeforeStatus;
  EFI_STATUS  AfterStatus;
  UINT32      BeforeAttributes;
  UINT32      AfterAttributes;
  UINTN       BeforeSize;
  UINTN       AfterSize;
  UINT8       Before = 0;
  UINT8       After = 0;
  CHAR8       Qualified[64];
  CHAR8       Arguments[96];

  BeforeSize   = sizeof (Before);
  BeforeStatus = gRT->GetVariable (Name, Guid, &BeforeAttributes, &BeforeSize, &Before);
  TestRequire ((BOOLEAN)(BeforeStatus == EFI_SUCCESS || BeforeStatus == EFI_NOT_FOUND), L"protected variable is valid or absent in fixture");
  AsciiSPrint (Qualified, sizeof (Qualified), "%g-%a", Guid, AsciiName);
  Command ("getvar", Qualified, BeforeStatus);
  AsciiSPrint (Arguments, sizeof (Arguments), "%a u8:0", Qualified);
  Command ("setvar", Arguments, EFI_WRITE_PROTECTED);
  AsciiSPrint (Arguments, sizeof (Arguments), "%a attrs=NV,BS,RT download", Qualified);
  CommandWithData (Arguments, &Before, sizeof (Before), EFI_WRITE_PROTECTED);
  Command ("rmvar", Qualified, EFI_WRITE_PROTECTED);
  Command ("rmvar", AsciiName, BeforeStatus == EFI_NOT_FOUND ? EFI_NOT_FOUND : EFI_WRITE_PROTECTED);
  AfterSize   = sizeof (After);
  AfterStatus = gRT->GetVariable (Name, Guid, &AfterAttributes, &AfterSize, &After);
  TestRequire ((BOOLEAN)(AfterStatus == BeforeStatus), L"status variable existence unchanged");
  if (!EFI_ERROR (BeforeStatus)) {
    TestRequire ((BOOLEAN)(AfterSize == BeforeSize && After == Before && AfterAttributes == BeforeAttributes), L"status value and attributes remain unchanged");
  }
}

STATIC VOID
RunDownloadedVariableTests (
  VOID
  )
{
  UINT8       Payload[] = { 0x00, 0x17, 0xFF, 0x80 };
  UINT8       Appended[] = { 0x00, 0x17, 0xFF, 0x80, 0x00, 0x17, 0xFF, 0x80 };
  UINT8       MalformedAuth = 0;
  EFI_STATUS  FirmwareStatus;
  UINT32      Attributes;
  UINTN       Size;
  CHAR8       FastbootArgs[128];

  TestDelete (L"BdsVarDownload", &mVariableGuid);
  Command ("setvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarDownload download", EFI_INVALID_PARAMETER);
  CommandWithData ("a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarDownload download", NULL, sizeof (Payload), EFI_INVALID_PARAMETER);
  CommandWithData ("a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarDownload download", Payload, 0, EFI_INVALID_PARAMETER);
  CommandWithData ("a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarDownload download", Payload, SIZE_1MB + 1, EFI_BAD_BUFFER_SIZE);
  CommandWithData ("a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarDownload download", Payload, sizeof (Payload), EFI_SUCCESS);
  Check (L"BdsVarDownload", Payload, sizeof (Payload), TEST_NV_ATTR);
  TestRequire ((BOOLEAN)(CompareMem (Payload, Appended, sizeof (Payload)) == 0), L"downloaded payload remains unchanged");

  // Some fixtures do not implement APPEND. Match the real backend response
  // and check that APPEND is an operation, not a persistent attribute.
  TestStatus (gRT->SetVariable (L"BdsVarAppendProbe", &mVariableGuid, TEST_NV_ATTR, sizeof (Payload), Payload), L"create append probe");
  FirmwareStatus = gRT->SetVariable (
                          L"BdsVarAppendProbe", &mVariableGuid,
                          TEST_NV_ATTR | EFI_VARIABLE_APPEND_WRITE, sizeof (Payload), Payload
                          );
  CommandWithData (
    "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarDownload attrs=NV,BS,RT,APPEND download",
    Payload, sizeof (Payload), FirmwareStatus
    );
  if (!EFI_ERROR (FirmwareStatus)) {
    Check (L"BdsVarDownload", Appended, sizeof (Appended), TEST_NV_ATTR);
  } else {
    Check (L"BdsVarDownload", Payload, sizeof (Payload), TEST_NV_ATTR);
  }

  TestDelete (L"BdsVarAppendProbe", &mVariableGuid);
  Command ("setvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarDownload attrs=BS u8:1", EFI_INVALID_PARAMETER);
  CommandWithData ("a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarDownload auth:download", Payload, sizeof (Payload), EFI_INVALID_PARAMETER);
  Command ("rmvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarDownload", EFI_SUCCESS);

  Command ("setvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarAuth attrs=NV,BS,RT,HW u8:1", EFI_INVALID_PARAMETER);
  Command ("setvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarAuth attrs=NV,BS,RT,AW u8:1", EFI_INVALID_PARAMETER);
  Command ("setvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarAuth attrs=NV,RT,AUTH download", EFI_INVALID_PARAMETER);
  Command ("setvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarAuth attrs=NV,BS,RT,AUTH,AUTH download", EFI_INVALID_PARAMETER);
  Command ("setvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarAuth attrs=NV,BS,RT,AUTH u8:1", EFI_INVALID_PARAMETER);
  Command ("setvar", "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarAuth attrs=NV,BS,RT,AUTH auth:download", EFI_INVALID_PARAMETER);

  // A malformed descriptor must reach SetVariable unchanged and its firmware
  // error must propagate. This covers fixtures with and without AUTH support.
  Attributes = TEST_NV_ATTR | EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS;
  FirmwareStatus = gRT->SetVariable (L"BdsVarAuth", &mVariableGuid, Attributes, sizeof (MalformedAuth), &MalformedAuth);
  TestRequire (EFI_ERROR (FirmwareStatus), L"firmware rejects malformed authenticated payload");
  CommandWithData (
    "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarAuth attrs=NV,BS,RT,AUTH auth:download",
    &MalformedAuth, sizeof (MalformedAuth), FirmwareStatus
    );
  CommandWithData (
    "a52e7a10-7259-4bd1-8a64-c340b7c26239-BdsVarAuth attrs=NV,BS,RT,AUTH download",
    &MalformedAuth, sizeof (MalformedAuth), FirmwareStatus
    );
  Size = 0;
  TestRequire ((BOOLEAN)(gRT->GetVariable (L"BdsVarAuth", &mVariableGuid, NULL, &Size, NULL) == EFI_NOT_FOUND), L"malformed auth never creates a variable");

  // The no-secboot fixture may omit these variables. Both creation and
  // mutation are protected without depending on firmware support for AUTH.
  CheckReadOnlyStatus (L"SecureBoot", "SecureBoot", &gEfiGlobalVariableGuid);
  CheckReadOnlyStatus (L"SetupMode", "SetupMode", &gEfiGlobalVariableGuid);
  CheckReadOnlyStatus (L"AuditMode", "AuditMode", &gEfiGlobalVariableGuid);
  CheckReadOnlyStatus (L"DeployedMode", "DeployedMode", &gEfiGlobalVariableGuid);

  CheckReadOnlyStatus (BDS_SECURE_FASTBOOT_ENABLE_NAME, "EnableSecureFastboot", &gMainlineBootVariableGuid);
  AsciiSPrint (FastbootArgs, sizeof (FastbootArgs), "%g-EnableSecureFastboot attrs=NV,BS u8:0", &gMainlineBootVariableGuid);
  Command ("setvar", FastbootArgs, EFI_WRITE_PROTECTED);
  AsciiSPrint (FastbootArgs, sizeof (FastbootArgs), "%g-EnableSecureFastboot attrs=NV,BS,RT u8:0", &gMainlineBootVariableGuid);
  Command ("setvar", FastbootArgs, EFI_WRITE_PROTECTED);
  AsciiSPrint (FastbootArgs, sizeof (FastbootArgs), "%g-EnableSecureFastboot attrs=NV,BS,AUTH,APPEND auth:download", &gMainlineBootVariableGuid);
  CommandWithData (FastbootArgs, Payload, sizeof (Payload), EFI_WRITE_PROTECTED);

  Command ("setvar", "8be4df61-93ca-11d2-aa0d-00e098032b8c-PK attrs=NV,BS,RT hex:01", EFI_INVALID_PARAMETER);
  Command ("setvar", "8be4df61-93ca-11d2-aa0d-00e098032b8c-KEK u8:1", EFI_INVALID_PARAMETER);
  Command ("setvar", "d719b2cb-3d3a-4596-a3bc-dad00e67656f-db u8:1", EFI_INVALID_PARAMETER);
  Command ("setvar", "d719b2cb-3d3a-4596-a3bc-dad00e67656f-dbx u8:1", EFI_INVALID_PARAMETER);
  Command ("setvar", "d719b2cb-3d3a-4596-a3bc-dad00e67656f-dbt u8:1", EFI_INVALID_PARAMETER);
  Command ("rmvar", "8be4df61-93ca-11d2-aa0d-00e098032b8c-PK", EFI_SECURITY_VIOLATION);
  Command ("rmvar", "d719b2cb-3d3a-4596-a3bc-dad00e67656f-db", EFI_SECURITY_VIOLATION);
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
  RunDownloadedVariableTests ();
  Print (L"BDSTEST VARIABLES PASS: types, attributes, ambiguity, bounds, enumeration, downloads, AUTH, read-only status and Secure Fastboot switch\n");
}
