/** @file
  Shared assertions for the external BdsApp QEMU launcher and boot payload.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef BDS_APP_TEST_COMMON_H_
#define BDS_APP_TEST_COMMON_H_

#include <Uefi.h>
#include <Guid/GlobalVariable.h>
#include <Guid/Gpt.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#define TEST_NV_ATTR    (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)
#define TEST_VOL_ATTR   (EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)
#define TEST_MAGIC      SIGNATURE_32 ('B', 'D', 'S', 'T')
#define TEST_FIRST      0xA901
#define TEST_SECOND     0xA902
#define TEST_THIRD      0xA903
#define TEST_FOURTH     0xA904
#define TEST_ANY_BOOT   0xFFFF
#define TEST_MAX_ORDER  16

#define CASE_DISCOVERY  1
#define CASE_NEXT       2
#define CASE_FALLBACK   3
#define CASE_SKIP       4
#define CASE_NEXT_BAD   5
#define CASE_SCOPE      6
#define CASE_MENU       7
#define CASE_NV         8
#define CASE_NV_NEXT    9
#define CASE_RETURN_MENU 10
#define CASE_FAILURE_MENU 11

typedef struct {
  UINT32    Magic;
  UINT32    Case;
  UINT32    Phase;
  CHAR8     Name[32];
  UINT16    ExpectedCurrent;
  UINT16    PrefixCount;
  UINT16    Prefix[8];
  UINT16    OrderCount;
} TEST_CONTEXT;

typedef struct {
  UINT32    Magic;
  UINT16    Count;
  UINT16    Order[TEST_MAX_ORDER];
  UINT32    Crc[TEST_MAX_ORDER];
} TEST_SNAPSHOT;

extern EFI_GUID  gTestGuid;
extern EFI_GUID  gTestPrimaryEspGuid;
extern EFI_GUID  gTestSecondEspGuid;

VOID
TestRequire (
  IN BOOLEAN       Condition,
  IN CONST CHAR16  *Message
  );

VOID
TestStatus (
  IN EFI_STATUS    Status,
  IN CONST CHAR16  *Message
  );

VOID
TestShutdown (
  VOID
  );

VOID *
TestReadVariable (
  IN CHAR16    *Name,
  IN EFI_GUID  *Guid,
  OUT UINTN    *Size,
  OUT UINT32   *Attributes OPTIONAL
  );

VOID
TestDelete (
  IN CHAR16    *Name,
  IN EFI_GUID  *Guid
  );

VOID
TestOptionName (
  IN UINT16   Number,
  OUT CHAR16  *Name
  );

VOID
TestSnapshot (
  OUT TEST_SNAPSHOT  *Snapshot
  );

VOID
TestVerifySnapshot (
  IN TEST_SNAPSHOT  *Expected
  );

EFI_PARTITION_INFO_PROTOCOL *
TestPartition (
  IN EFI_HANDLE  Handle
  );

#endif
