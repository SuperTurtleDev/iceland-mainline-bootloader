/** @file
  Exercise production security selection against hostile and reentrant services.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "../TestChallenge/HostSupport.h"
#include <Guid/GlobalVariable.h>
#include <Guid/ImageAuthentication.h>
#include <Protocol/Security.h>
#include <Protocol/Security2.h>
#include <Library/SecurityManagementLib.h>

#define CHECK(Expression) do { \
  mChecks++; \
  if (!(Expression)) { \
    fprintf (stderr, "FAIL line %d: %s\n", __LINE__, #Expression); \
    exit (1); \
  } \
} while (0)

STATIC UINTN mChecks;
STATIC EFI_BOOT_SERVICES mBoot;
STATIC EFI_RUNTIME_SERVICES mRuntime;
EFI_BOOT_SERVICES *gBS = &mBoot;
EFI_RUNTIME_SERVICES *gRT = &mRuntime;
EFI_GUID gEfiGlobalVariableGuid = EFI_GLOBAL_VARIABLE;
EFI_GUID gEfiImageSecurityDatabaseGuid = EFI_IMAGE_SECURITY_DATABASE_GUID;
EFI_GUID gEfiSecurityArchProtocolGuid = EFI_SECURITY_ARCH_PROTOCOL_GUID;
EFI_GUID gEfiSecurity2ArchProtocolGuid = EFI_SECURITY2_ARCH_PROTOCOL_GUID;
EFI_GUID gMainlineBootVariableGuid = { 0xae98d750, 0xef2e, 0x49de, { 0x9a, 0x7d, 0xad, 0x80, 0x8e, 0x1b, 0xe1, 0x27 } };

STATIC UINTN mReads;
STATIC UINTN mSecureReads;
STATIC UINTN mSetupReads;
STATIC UINTN mFastbootReads;
STATIC UINTN mKeyReads;
STATIC UINTN mWrites;
STATIC UINTN mLocates;
STATIC UINTN mPlatformLocates;
STATIC UINTN mPlatformCalls;
STATIC UINTN mFirmwareCalls;
STATIC UINTN mFirmwareStateCalls;
STATIC UINTN mVerifierCalls;
STATIC UINTN mAuthRefreshes;
STATIC UINTN mReentries;
STATIC BOOLEAN mInitializing;
STATIC EFI_STATUS mVerifierStatus;
STATIC UINT8 mSecure = 1;
STATIC UINT8 mSetup;
STATIC UINTN mSecureSize = 1;
STATIC EFI_STATUS mSecureStatus = EFI_SUCCESS;
STATIC EFI_STATUS mSetupStatus = EFI_SUCCESS;
STATIC BOOLEAN mMissingPk;
STATIC BOOLEAN mMissingKek;
STATIC BOOLEAN mMissingDb;
STATIC BOOLEAN mMalformedDb;
STATIC BOOLEAN mFastbootPresent = TRUE;
STATIC UINT8 mFastboot = 1;
STATIC UINT32 mFastbootAttributes = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS;
STATIC EFI_STATUS mFastbootStatus = EFI_SUCCESS;
STATIC BOOLEAN mWritesProtected;
STATIC EFI_SECURITY_ARCH_PROTOCOL mSecurityFixture;
STATIC EFI_SECURITY2_ARCH_PROTOCOL mSecurity2Fixture;
STATIC EFI_STATUS mSecurityLocateStatus = EFI_SUCCESS;
STATIC EFI_STATUS mSecurity2LocateStatus = EFI_SUCCESS;
STATIC BOOLEAN mNullSecurity;
STATIC BOOLEAN mNullSecurity2;
STATIC CONST EFI_DEVICE_PATH_PROTOCOL *mExpectedPath;
STATIC VOID *mExpectedBuffer;
STATIC UINTN mExpectedSize;
STATIC BOOLEAN mExpectedBootPolicy;
STATIC EFI_DEVICE_PATH_PROTOCOL mDevicePath = {
  END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
};
STATIC UINT8 mImage[] = { 1, 2, 3, 4 };

VOID *EFIAPI CopyMem (VOID *Dst, CONST VOID *Src, UINTN Size) { return memcpy (Dst, Src, Size); }
VOID *EFIAPI AllocatePool (UINTN Size) { return malloc (Size); }
VOID EFIAPI FreePool (VOID *Buffer) { free (Buffer); }
VOID FortifyFail (CONST CHAR8 *Name, CONST UINTN Line) { abort (); }
BOOLEAN BdsSecurityAuthEnabled (VOID) { mAuthRefreshes++; return FALSE; }

EFI_STATUS EFIAPI
ExecuteSecurity2Handlers (
  UINT32 Operation, UINT32 AuthenticationStatus,
  CONST EFI_DEVICE_PATH_PROTOCOL *Path, VOID *Buffer, UINTN Size, BOOLEAN BootPolicy
  )
{
  CHECK (Operation == EFI_AUTH_OPERATION_VERIFY_IMAGE);
  CHECK (AuthenticationStatus == 0);
  CHECK (Path == mExpectedPath && Buffer == mExpectedBuffer);
  CHECK (Size == mExpectedSize && BootPolicy == mExpectedBootPolicy);
  mVerifierCalls++;
  return mVerifierStatus;
}

#include "../BdsSecurity.c"

STATIC BOOLEAN
NameIs (CONST CHAR16 *A, CONST CHAR16 *B)
{
  while (*A != 0 && *A == *B) { A++; B++; }
  return *A == *B;
}

STATIC BOOLEAN
GuidIs (CONST EFI_GUID *A, CONST EFI_GUID *B)
{
  return memcmp (A, B, sizeof (*A)) == 0;
}

STATIC VOID
CheckReentry (VOID)
{
  UINTN Reads = mReads;
  UINTN Writes = mWrites;
  UINTN Locates = mLocates;
  UINTN VerifierCalls = mVerifierCalls;

  CHECK (mInitializing);
  CHECK (BdsSecurityInitialize () == EFI_SUCCESS);
  CHECK (BdsSecurityReady () == EFI_NOT_READY);
  if (mSecurity2Fixture.FileAuthentication == BdsAuthenticateImage) {
    CHECK (mSecurity2Fixture.FileAuthentication (&mSecurity2Fixture, &mDevicePath, mImage,
                                                sizeof (mImage), FALSE) == EFI_SECURITY_VIOLATION);
  }
  if (mSecurityFixture.FileAuthenticationState == BdsAuthenticateState) {
    CHECK (mSecurityFixture.FileAuthenticationState (&mSecurityFixture, 0,
                                                     &mDevicePath) == EFI_SECURITY_VIOLATION);
  }
  CHECK (mReads == Reads && mWrites == Writes && mLocates == Locates);
  CHECK (mVerifierCalls == VerifierCalls);
  mReentries++;
}

STATIC EFI_STATUS EFIAPI
GetVariable (CHAR16 *Name, EFI_GUID *Guid, UINT32 *Attributes, UINTN *Size, VOID *Data)
{
  UINT8 Value[2] = { 0, 0 };
  UINTN Required = 1;
  UINT8 Key[sizeof (EFI_SIGNATURE_LIST) + sizeof (EFI_GUID) + 1];
  EFI_SIGNATURE_LIST List;
  CONST VOID *Source = Value;
  UINT32 Attr = EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;

  mReads++;
  if (NameIs (Name, L"SecureBoot")) {
    CHECK (GuidIs (Guid, &gEfiGlobalVariableGuid));
    mSecureReads++;
    if (mInitializing) { CheckReentry (); }
    if (EFI_ERROR (mSecureStatus)) { return mSecureStatus; }
    Value[0] = mSecure;
    Required = mSecureSize;
  } else if (NameIs (Name, L"SetupMode")) {
    CHECK (GuidIs (Guid, &gEfiGlobalVariableGuid));
    mSetupReads++;
    if (EFI_ERROR (mSetupStatus)) { return mSetupStatus; }
    Value[0] = mSetup;
  } else if (NameIs (Name, L"EnableSecureFastboot")) {
    CHECK (GuidIs (Guid, &gMainlineBootVariableGuid));
    mFastbootReads++;
    if (EFI_ERROR (mFastbootStatus)) { return mFastbootStatus; }
    if (!mFastbootPresent) { return EFI_NOT_FOUND; }
    Source = &mFastboot;
    Attr = mFastbootAttributes;
  } else {
    // No other setting or vendor variable may be read by this library.
    CHECK (NameIs (Name, L"PK") || NameIs (Name, L"KEK") || NameIs (Name, L"db"));
    CHECK (GuidIs (Guid, NameIs (Name, L"db") ? &gEfiImageSecurityDatabaseGuid : &gEfiGlobalVariableGuid));
    mKeyReads++;
    if ((NameIs (Name, L"PK") && mMissingPk) ||
        (NameIs (Name, L"KEK") && mMissingKek) ||
        (NameIs (Name, L"db") && mMissingDb)) { return EFI_NOT_FOUND; }
    memset (Key, 0, sizeof (Key));
    memset (&List, 0, sizeof (List));
    List.SignatureType = (EFI_GUID)EFI_CERT_X509_GUID;
    List.SignatureListSize = sizeof (Key);
    List.SignatureSize = sizeof (EFI_GUID) + 1;
    if (mMalformedDb && NameIs (Name, L"db")) { List.SignatureSize = 0; }
    memcpy (Key, &List, sizeof (List));
    Source = Key;
    Required = sizeof (Key);
    Attr = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
           EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS;
  }
  if (Attributes != NULL) { *Attributes = Attr; }
  if (*Size < Required) { *Size = Required; return EFI_BUFFER_TOO_SMALL; }
  *Size = Required;
  if (Required != 0) { CHECK (Data != NULL); memcpy (Data, Source, Required); }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
SetVariable (CHAR16 *Name, EFI_GUID *Guid, UINT32 Attributes, UINTN Size, VOID *Data)
{
  mWrites++;
  // The only writable setting is the independent fastboot control. In
  // particular, neither SecureBoot nor any host mode variable can be written.
  CHECK (NameIs (Name, L"EnableSecureFastboot"));
  CHECK (GuidIs (Guid, &gMainlineBootVariableGuid));
  CHECK (Size == 1 && Data != NULL && *(UINT8 *)Data <= 1);
  CHECK (Attributes == (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS));
  if (mWritesProtected) { return EFI_WRITE_PROTECTED; }
  mFastbootPresent = TRUE;
  mFastboot = *(UINT8 *)Data;
  mFastbootAttributes = Attributes;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
FirmwareAuthenticate (
  CONST EFI_SECURITY2_ARCH_PROTOCOL *This, CONST EFI_DEVICE_PATH_PROTOCOL *Path,
  VOID *Buffer, UINTN Size, BOOLEAN BootPolicy
  )
{
  mFirmwareCalls++;
  return EFI_ACCESS_DENIED;
}

STATIC EFI_STATUS EFIAPI
FirmwareAuthenticateState (
  CONST EFI_SECURITY_ARCH_PROTOCOL *This, UINT32 AuthenticationStatus,
  CONST EFI_DEVICE_PATH_PROTOCOL *Path
  )
{
  mFirmwareStateCalls++;
  return EFI_ACCESS_DENIED;
}

// A deliberately hostile QCOM protocol exists, but must never be located or
// called. Its ABI and GUID are test fixtures only, not production dependencies.
typedef struct FORBIDDEN_PLATFORM_SERVICE FORBIDDEN_PLATFORM_SERVICE;
typedef EFI_STATUS (EFIAPI *FORBIDDEN_PLATFORM_METHOD)(FORBIDDEN_PLATFORM_SERVICE *This);
struct FORBIDDEN_PLATFORM_SERVICE {
  UINT64 Revision;
  FORBIDDEN_PLATFORM_METHOD PreConsoleInit;
  FORBIDDEN_PLATFORM_METHOD PostConsoleInit;
  FORBIDDEN_PLATFORM_METHOD SetPlatformSecurity;
};
STATIC EFI_GUID mForbiddenPlatformGuid = {
  0xd874d61a, 0x4b87, 0x7608, { 0xa0, 0x0f, 0x58, 0xad, 0xd7, 0x05, 0x25, 0x30 }
};
STATIC EFI_STATUS EFIAPI
ForbiddenPlatformSecurity (FORBIDDEN_PLATFORM_SERVICE *This)
{
  mPlatformCalls++;
  return EFI_ACCESS_DENIED;
}
STATIC FORBIDDEN_PLATFORM_SERVICE mPlatform = { 0x10003, NULL, NULL, ForbiddenPlatformSecurity };

STATIC EFI_STATUS EFIAPI
LocateProtocol (EFI_GUID *Guid, VOID *Registration, VOID **Interface)
{
  mLocates++;
  CheckReentry ();
  if (GuidIs (Guid, &mForbiddenPlatformGuid)) {
    mPlatformLocates++;
    *Interface = &mPlatform;
  } else if (GuidIs (Guid, &gEfiSecurityArchProtocolGuid)) {
    if (EFI_ERROR (mSecurityLocateStatus)) { return mSecurityLocateStatus; }
    *Interface = mNullSecurity ? NULL : &mSecurityFixture;
  } else {
    CHECK (GuidIs (Guid, &gEfiSecurity2ArchProtocolGuid));
    if (EFI_ERROR (mSecurity2LocateStatus)) { return mSecurity2LocateStatus; }
    *Interface = mNullSecurity2 ? NULL : &mSecurity2Fixture;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Authenticate (CONST EFI_DEVICE_PATH_PROTOCOL *Path, VOID *Buffer, UINTN Size, BOOLEAN BootPolicy)
{
  mExpectedPath = Path;
  mExpectedBuffer = Buffer;
  mExpectedSize = Size;
  mExpectedBootPolicy = BootPolicy;
  return mSecurity2Fixture.FileAuthentication (&mSecurity2Fixture, Path, Buffer, Size, BootPolicy);
}

STATIC VOID
CheckNoHostPolicy (VOID)
{
  CHECK (mPlatformLocates == 0 && mPlatformCalls == 0);
  CHECK (mFirmwareCalls == 0 && mFirmwareStateCalls == 0);
}

STATIC VOID
CheckRepeatedInitialization (EFI_STATUS Expected)
{
  UINTN Reads = mReads;
  UINTN Writes = mWrites;
  UINTN Locates = mLocates;

  CHECK (BdsSecurityInitialize () == Expected);
  CHECK (BdsSecurityInitialize () == Expected);
  CHECK (BdsSecurityReady () == Expected);
  CHECK (mReads == Reads && mWrites == Writes && mLocates == Locates);
}

STATIC VOID
CheckImagePolicy (BOOLEAN Verify)
{
  UINTN Calls = mVerifierCalls;
  UINTN Reads = mReads;
  UINTN Index;
  CONST EFI_STATUS Results[] = { EFI_SUCCESS, EFI_SECURITY_VIOLATION, EFI_ACCESS_DENIED, EFI_DEVICE_ERROR };

  CHECK (mSecurityFixture.FileAuthenticationState (&mSecurityFixture, MAX_UINT32, &mDevicePath) == EFI_SUCCESS);
  CHECK (mSecurityFixture.FileAuthenticationState (&mSecurityFixture, MAX_UINT32, NULL) == EFI_SUCCESS);
  CHECK (mVerifierCalls == Calls);
  for (Index = 0; Index < sizeof (Results) / sizeof (Results[0]); Index++) {
    mVerifierStatus = Results[Index];
    CHECK (Authenticate (&mDevicePath, mImage, sizeof (mImage), TRUE) == (Verify ? Results[Index] : EFI_SUCCESS));
    CHECK (Authenticate (NULL, mImage, sizeof (mImage), FALSE) == (Verify ? Results[Index] : EFI_SUCCESS));
    if (Verify) { Calls += 2; }
    CHECK (mVerifierCalls == Calls);
  }

  // Security2 distinguishes connection queries from image verification.
  CHECK (Authenticate (&mDevicePath, NULL, 0, FALSE) == EFI_SUCCESS);
  CHECK (Authenticate (&mDevicePath, NULL, sizeof (mImage), FALSE) == (Verify ? EFI_SECURITY_VIOLATION : EFI_SUCCESS));
  CHECK (Authenticate (&mDevicePath, mImage, 0, FALSE) == (Verify ? EFI_SECURITY_VIOLATION : EFI_SUCCESS));
  CHECK (Authenticate (NULL, NULL, 0, FALSE) == (Verify ? EFI_SECURITY_VIOLATION : EFI_SUCCESS));
  CHECK (mVerifierCalls == Calls && mReads == Reads);
  CheckNoHostPolicy ();
}

STATIC VOID
CheckLatchedSelection (BOOLEAN Verify)
{
  UINT8 Secure = mSecure;
  EFI_STATUS SecureStatus = mSecureStatus;
  EFI_STATUS SetupStatus = mSetupStatus;
  UINTN Reads = mReads;

  // The selector must not be re-read by Ready, repeated initialization, or the
  // hooks. The official verifier remains independently responsible for db/dbx.
  mSecure = Verify ? 0 : 1;
  mSecureStatus = EFI_DEVICE_ERROR;
  mSetupStatus = EFI_DEVICE_ERROR;
  CheckRepeatedInitialization (EFI_SUCCESS);
  CheckImagePolicy (Verify);
  CHECK (mReads == Reads);
  mSecure = Secure;
  mSecureStatus = SecureStatus;
  mSetupStatus = SetupStatus;
}

STATIC VOID
CheckHookOwnership (VOID)
{
  UINTN Calls = mVerifierCalls;

  mSecurity2Fixture.FileAuthentication = FirmwareAuthenticate;
  CHECK (BdsSecurityReady () == EFI_SECURITY_VIOLATION);
  CHECK (mSecurityFixture.FileAuthenticationState (&mSecurityFixture, 0, &mDevicePath) == EFI_SECURITY_VIOLATION);
  mSecurity2Fixture.FileAuthentication = BdsAuthenticateImage;
  mSecurityFixture.FileAuthenticationState = FirmwareAuthenticateState;
  CHECK (BdsSecurityReady () == EFI_SECURITY_VIOLATION);
  CHECK (Authenticate (&mDevicePath, mImage, sizeof (mImage), FALSE) == EFI_SECURITY_VIOLATION);
  mSecurityFixture.FileAuthenticationState = BdsAuthenticateState;
  CHECK (BdsSecurityReady () == EFI_SUCCESS);
  CHECK (mVerifierCalls == Calls);
  CheckNoHostPolicy ();
}

STATIC VOID
CheckFastboot (
  EFI_STATUS ExpectedGet, BOOLEAN ExpectedEnabled, EFI_STATUS ExpectedEnable,
  EFI_STATUS ExpectedDisable, EFI_STATUS InitializationStatus
  )
{
  BOOLEAN Enabled = FALSE;
  UINTN SecureReads = mSecureReads;
  UINTN SetupReads = mSetupReads;
  UINTN Refreshes = mAuthRefreshes;
  UINT8 Secure = mSecure;
  EFI_STATUS Status;

  CHECK (BdsSecurityGetFastbootEnabled (NULL) == EFI_INVALID_PARAMETER);
  Status = BdsSecurityGetFastbootEnabled (&Enabled);
  CHECK (Status == ExpectedGet);
  CHECK (Enabled == ExpectedEnabled);
  if (!EFI_ERROR (Status) && !Enabled && mFastbootPresent) { CHECK (mFastboot == 0); }
  CHECK (BdsSecurityReady () == InitializationStatus);
  Status = BdsSecuritySetFastbootEnabled (FALSE);
  CHECK (Status == ExpectedDisable);
  if (!EFI_ERROR (Status)) {
    Refreshes++;
    CHECK (mFastboot == 0 && mFastbootPresent);
  }
  CHECK (mAuthRefreshes == Refreshes);
  Status = BdsSecuritySetFastbootEnabled (TRUE);
  CHECK (Status == ExpectedEnable);
  if (!EFI_ERROR (Status)) {
    Refreshes++;
    CHECK (mFastboot == 1 && mFastbootPresent);
  }
  CHECK (mAuthRefreshes == Refreshes);
  CHECK (mSecure == Secure && mSecureReads == SecureReads && mSetupReads == SetupReads);
  CHECK (BdsSecurityReady () == InitializationStatus);
  CheckNoHostPolicy ();
}

int main (int Argc, char **Argv)
{
  EFI_STATUS Status;
  EFI_STATUS ExpectedInitialization = EFI_SUCCESS;
  EFI_STATUS ExpectedDisplay = EFI_SUCCESS;
  EFI_STATUS ExpectedFastbootGet = EFI_SUCCESS;
  EFI_STATUS ExpectedFastbootEnable = EFI_SUCCESS;
  EFI_STATUS ExpectedFastbootDisable = EFI_SUCCESS;
  BOOLEAN ExpectedFastbootEnabled = TRUE;
  BOOLEAN Enabled;
  BOOLEAN Setup;
  BOOLEAN Verify;
  UINTN Writes;

  CHECK (Argc == 2);
  mRuntime.GetVariable = GetVariable;
  mRuntime.SetVariable = SetVariable;
  mBoot.LocateProtocol = LocateProtocol;
  // Every unconfigured boot service, including protocol installation, is NULL.
  mSecurityFixture.FileAuthenticationState = FirmwareAuthenticateState;
  mSecurity2Fixture.FileAuthentication = FirmwareAuthenticate;

  if (strcmp (Argv[1], "enabled") == 0) {
  } else if (strcmp (Argv[1], "disabled") == 0) {
    mSecure = 0;
  } else if (strcmp (Argv[1], "defaults") == 0 || strcmp (Argv[1], "disabled-defaults") == 0) {
    mFastbootPresent = FALSE;
    ExpectedFastbootEnabled = FALSE;
    if (strcmp (Argv[1], "disabled-defaults") == 0) { mSecure = 0; }
  } else if (strcmp (Argv[1], "fastboot-disabled") == 0) {
    mFastboot = 0;
    ExpectedFastbootEnabled = FALSE;
  } else if (strcmp (Argv[1], "setup-one") == 0) {
    mSetup = 1;
  } else if (strcmp (Argv[1], "setup-missing") == 0 || strcmp (Argv[1], "disabled-setup-missing") == 0) {
    mSetupStatus = ExpectedDisplay = EFI_NOT_FOUND;
    if (strcmp (Argv[1], "disabled-setup-missing") == 0) { mSecure = 0; }
  } else if (strcmp (Argv[1], "setup-invalid") == 0 || strcmp (Argv[1], "disabled-setup-invalid") == 0) {
    mSetup = 2;
    ExpectedDisplay = EFI_COMPROMISED_DATA;
    if (strcmp (Argv[1], "disabled-setup-invalid") == 0) { mSecure = 0; }
  } else if (strcmp (Argv[1], "secureboot-missing") == 0) {
    mSecureStatus = ExpectedInitialization = ExpectedDisplay = EFI_NOT_FOUND;
  } else if (strcmp (Argv[1], "secureboot-read-error") == 0) {
    mSecureStatus = ExpectedInitialization = ExpectedDisplay = EFI_DEVICE_ERROR;
  } else if (strcmp (Argv[1], "secureboot-invalid-value") == 0) {
    mSecure = 2;
    ExpectedInitialization = ExpectedDisplay = EFI_COMPROMISED_DATA;
  } else if (strcmp (Argv[1], "secureboot-empty") == 0) {
    mSecureSize = 0;
    ExpectedInitialization = ExpectedDisplay = EFI_COMPROMISED_DATA;
  } else if (strcmp (Argv[1], "secureboot-oversized") == 0) {
    mSecureSize = 2;
    ExpectedInitialization = ExpectedDisplay = EFI_BUFFER_TOO_SMALL;
  } else if (strcmp (Argv[1], "missing-security2") == 0) {
    mSecurity2LocateStatus = ExpectedInitialization = EFI_NOT_FOUND;
  } else if (strcmp (Argv[1], "missing-security") == 0) {
    mSecurityLocateStatus = ExpectedInitialization = EFI_NOT_FOUND;
  } else if (strcmp (Argv[1], "null-security2") == 0) {
    mNullSecurity2 = TRUE;
    ExpectedInitialization = EFI_UNSUPPORTED;
  } else if (strcmp (Argv[1], "null-security") == 0) {
    mNullSecurity = TRUE;
    ExpectedInitialization = EFI_UNSUPPORTED;
  } else if (strcmp (Argv[1], "security2-locate-error") == 0) {
    mSecurity2LocateStatus = ExpectedInitialization = EFI_DEVICE_ERROR;
  } else if (strcmp (Argv[1], "security-locate-error") == 0) {
    mSecurityLocateStatus = ExpectedInitialization = EFI_DEVICE_ERROR;
  } else if (strcmp (Argv[1], "null-original-callbacks") == 0) {
    mSecurityFixture.FileAuthenticationState = NULL;
    mSecurity2Fixture.FileAuthentication = NULL;
  } else if (strcmp (Argv[1], "missing-pk") == 0) {
    mMissingPk = TRUE;
    mSetup = 1;
    ExpectedFastbootEnabled = FALSE;
    ExpectedFastbootEnable = EFI_NOT_READY;
  } else if (strcmp (Argv[1], "missing-kek") == 0) {
    mMissingKek = TRUE;
    ExpectedFastbootEnabled = FALSE;
    ExpectedFastbootEnable = EFI_NOT_READY;
  } else if (strcmp (Argv[1], "missing-db") == 0) {
    mMissingDb = TRUE;
    ExpectedFastbootEnabled = FALSE;
    ExpectedFastbootEnable = EFI_NOT_READY;
  } else if (strcmp (Argv[1], "malformed-db") == 0) {
    mMalformedDb = TRUE;
    ExpectedFastbootGet = ExpectedFastbootEnable = ExpectedFastbootDisable = EFI_COMPROMISED_DATA;
  } else if (strcmp (Argv[1], "fastboot-runtime-attribute") == 0) {
    mFastbootAttributes |= EFI_VARIABLE_RUNTIME_ACCESS;
    ExpectedFastbootGet = ExpectedFastbootEnable = ExpectedFastbootDisable = EFI_SECURITY_VIOLATION;
  } else if (strcmp (Argv[1], "fastboot-invalid-value") == 0) {
    mFastboot = 2;
    ExpectedFastbootGet = ExpectedFastbootEnable = ExpectedFastbootDisable = EFI_SECURITY_VIOLATION;
  } else if (strcmp (Argv[1], "fastboot-read-error") == 0) {
    mFastbootStatus = ExpectedFastbootGet = ExpectedFastbootEnable = ExpectedFastbootDisable = EFI_DEVICE_ERROR;
  } else if (strcmp (Argv[1], "defaults-write-protected") == 0) {
    mFastbootPresent = FALSE;
    mWritesProtected = TRUE;
    ExpectedFastbootEnabled = FALSE;
    ExpectedFastbootEnable = ExpectedFastbootDisable = EFI_WRITE_PROTECTED;
  } else if (strcmp (Argv[1], "fastboot-auto-disable-write-protected") == 0) {
    mMissingDb = TRUE;
    mWritesProtected = TRUE;
    ExpectedFastbootEnabled = FALSE;
    ExpectedFastbootGet = ExpectedFastbootEnable = ExpectedFastbootDisable = EFI_WRITE_PROTECTED;
  } else if (strcmp (Argv[1], "fastboot-set-write-protected") == 0) {
    mWritesProtected = TRUE;
    ExpectedFastbootEnable = ExpectedFastbootDisable = EFI_WRITE_PROTECTED;
  } else {
    CHECK (FALSE);
  }

  Verify = (BOOLEAN)(mSecure == 1);
  CHECK (BdsSecurityReady () == EFI_NOT_STARTED);
  mInitializing = TRUE;
  Status = BdsSecurityInitialize ();
  mInitializing = FALSE;
  CHECK (Status == ExpectedInitialization);
  CHECK (mSecureReads == 1 && mSetupReads == 0);
  CHECK (mKeyReads == 0 && mFastbootReads == 0 && mWrites == 0 && mAuthRefreshes == 0);
  CHECK (mLocates == 2 && mReentries == 3);
  CheckRepeatedInitialization (ExpectedInitialization);
  CheckNoHostPolicy ();

  if (EFI_ERROR (Status)) {
    // Every available standard interface remains locally hooked and rejecting,
    // including when SecureBoot is absent or Security2 itself is unavailable.
    if (!EFI_ERROR (mSecurity2LocateStatus) && !mNullSecurity2) {
      CHECK (mSecurity2Fixture.FileAuthentication == BdsAuthenticateImage);
      CHECK (Authenticate (&mDevicePath, mImage, sizeof (mImage), FALSE) == EFI_SECURITY_VIOLATION);
      CHECK (Authenticate (NULL, NULL, 0, FALSE) == EFI_SECURITY_VIOLATION);
    }
    if (!EFI_ERROR (mSecurityLocateStatus) && !mNullSecurity) {
      CHECK (mSecurityFixture.FileAuthenticationState == BdsAuthenticateState);
      CHECK (mSecurityFixture.FileAuthenticationState (&mSecurityFixture, 0, &mDevicePath) == EFI_SECURITY_VIOLATION);
    }
    CHECK (mVerifierCalls == 0);
  } else {
    CheckImagePolicy (Verify);
    CheckLatchedSelection (Verify);
    CheckHookOwnership ();
  }

  // The display API tolerates the real-device SetupMode=1, SecureBoot=1 pair.
  Writes = mWrites;
  CHECK (BdsGetSecureBootState (NULL, &Setup) == EFI_INVALID_PARAMETER);
  CHECK (BdsGetSecureBootState (&Enabled, NULL) == EFI_INVALID_PARAMETER);
  CHECK (BdsGetSecureBootState (&Enabled, &Setup) == ExpectedDisplay);
  if (!EFI_ERROR (ExpectedDisplay)) {
    CHECK (Enabled == (BOOLEAN)(mSecure != 0) && Setup == (BOOLEAN)(mSetup != 0));
  }
  CHECK (mWrites == Writes);
  CHECK (BdsSecurityReady () == ExpectedInitialization);

  CheckFastboot (ExpectedFastbootGet, ExpectedFastbootEnabled, ExpectedFastbootEnable,
                 ExpectedFastbootDisable, ExpectedInitialization);
  if (!EFI_ERROR (ExpectedInitialization)) { CheckImagePolicy (Verify); }
  CheckRepeatedInitialization (ExpectedInitialization);
  CheckNoHostPolicy ();
  printf ("PASS: %s (%llu assertions)\n", Argv[1], mChecks);
  return 0;
}
