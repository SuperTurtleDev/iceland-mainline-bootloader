/** @file
  Test-only state shared by the firmware launcher and signed probe.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef SECURE_BOOT_TEST_H_
#define SECURE_BOOT_TEST_H_

#include <Uefi.h>
#include <Guid/GlobalVariable.h>
#include <Guid/ImageAuthentication.h>
#include <Guid/FileInfo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/Security.h>
#include <Protocol/Security2.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>

#define SB_NV_ATTR    (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)
#define SB_AUTH_ATTR  (SB_NV_ATTR | EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS)
#define SB_VOL_ATTR   (EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)
#define SB_TEST_MAGIC SIGNATURE_32 ('S', 'B', 'T', 'S')

typedef struct {
  UINT32                             Magic;
  EFI_HANDLE                         BdsImage;
  EFI_HANDLE                         Fixture;
  EFI_SECURITY_ARCH_PROTOCOL         *Security;
  EFI_SECURITY_FILE_AUTHENTICATION_STATE  FirmwareStateReject;
  EFI_SECURITY2_ARCH_PROTOCOL        *Security2;
  EFI_SECURITY2_FILE_AUTHENTICATION   FirmwareImageReject;
  UINTN                              FirmwareImageCalls;
  UINTN                              FirmwareStateCalls;
  UINTN                              ImageCallsBeforeBds;
  UINTN                              StateCallsBeforeBds;
  UINTN                              PlatformSecurityCalls;
  BOOLEAN                            SetupModeFixture;
  UINTN                              Executions;
} SB_TEST_STATE;

extern EFI_GUID  gSbTestGuid;

VOID SbRequire (IN BOOLEAN Condition, IN CONST CHAR16 *Message);
VOID SbStatus (IN EFI_STATUS Status, IN CONST CHAR16 *Message);
VOID SbShutdown (VOID);
VOID *SbReadFile (IN EFI_HANDLE Fixture, IN CONST CHAR16 *Name, OUT UINTN *Size);
VOID *SbReadVariable (IN CHAR16 *Name, IN EFI_GUID *Guid, OUT UINTN *Size);
VOID SbCheckVariableFile (IN EFI_HANDLE Fixture, IN CHAR16 *Name, IN EFI_GUID *Guid, IN CONST CHAR16 *FileName);
VOID SbSetAuthFile (IN EFI_HANDLE Fixture, IN CHAR16 *Name, IN EFI_GUID *Guid, IN CONST CHAR16 *FileName, IN EFI_STATUS Expected);
VOID SbCheckMode (IN UINT8 SetupMode, IN UINT8 SecureBoot);
VOID SbCheckNoEnrollment (VOID);
VOID SbCheckFirmwareIsolation (IN SB_TEST_STATE *State);
VOID SbCheckLegacyIsolation (IN SB_TEST_STATE *State);
SB_TEST_STATE *SbGetState (VOID);
EFI_STATUS SbLoadImage (IN EFI_HANDLE Parent, IN EFI_HANDLE Fixture, IN CONST CHAR16 *Name, OUT EFI_HANDLE *Image);

#endif
