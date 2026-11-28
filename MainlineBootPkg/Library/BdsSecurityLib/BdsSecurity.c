/** @file
  Connect the standard edk2 image-verification handlers to the host LoadImage path.

  Variable authentication and key storage remain with the host runtime services.
  This resident application owns neither a variable backend nor certificate files.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/GlobalVariable.h>
#include <Guid/ImageAuthentication.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BdsSecurityLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/SecurityManagementLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/Security.h>
#include <Protocol/Security2.h>

#define BDS_SETTING_ATTRIBUTES  (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS)

STATIC EFI_SECURITY_ARCH_PROTOCOL        *mSecurity;
STATIC EFI_SECURITY2_ARCH_PROTOCOL       *mSecurity2;
STATIC BOOLEAN                           mImageVerificationEnabled;
STATIC BOOLEAN                           mInitializationStarted;
STATIC BOOLEAN                           mInitializationComplete;
STATIC EFI_STATUS                        mInitializationStatus = EFI_NOT_STARTED;

STATIC
EFI_STATUS
ReadMode (
  IN  CHAR16  *Name,
  OUT UINT8   *Mode
  )
{
  EFI_STATUS  Status;
  UINTN       Size;

  Size   = sizeof (*Mode);
  Status = gRT->GetVariable (Name, &gEfiGlobalVariableGuid, NULL, &Size, Mode);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return ((Size == sizeof (*Mode)) && (*Mode <= 1)) ? EFI_SUCCESS : EFI_COMPROMISED_DATA;
}

STATIC
EFI_STATUS
ReadSetting (
  IN  CHAR16    *Name,
  IN  EFI_GUID  *Guid,
  OUT BOOLEAN   *Enabled
  )
{
  EFI_STATUS  Status;
  UINT8       Value;
  UINTN       Size;
  UINT32      Attributes;

  *Enabled = TRUE;
  Size     = sizeof (Value);
  Status   = gRT->GetVariable (Name, Guid, &Attributes, &Size, &Value);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Size != sizeof (Value)) || (Value > 1) || (Attributes != BDS_SETTING_ATTRIBUTES)) {
    return EFI_SECURITY_VIOLATION;
  }

  *Enabled = (BOOLEAN)(Value != 0);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
HasKeyDatabase (
  IN  CHAR16    *Name,
  IN  EFI_GUID  *Guid,
  OUT BOOLEAN   *Present
  )
{
  EFI_STATUS          Status;
  UINTN               Size;
  UINTN               Capacity;
  UINTN               Offset;
  UINT32              Attributes;
  UINT8               *Data;
  EFI_SIGNATURE_LIST  List;

  *Present = FALSE;
  Size     = 0;
  Status   = gRT->GetVariable (Name, Guid, &Attributes, &Size, NULL);
  if ((Status == EFI_NOT_FOUND) || ((Status == EFI_SUCCESS) && (Size == 0))) {
    return EFI_SUCCESS;
  }

  if (Status != EFI_BUFFER_TOO_SMALL) {
    return EFI_ERROR (Status) ? Status : EFI_COMPROMISED_DATA;
  }

  if ((Size < sizeof (List)) || (Size > 1024 * 1024)) {
    return EFI_COMPROMISED_DATA;
  }

  Capacity = Size;
  Data     = AllocatePool (Size);
  if (Data == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gRT->GetVariable (Name, Guid, &Attributes, &Size, Data);
  if (!EFI_ERROR (Status)) {
    if ((Size == 0) || (Size > Capacity) ||
        (Attributes != (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS |
                        EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS))) {
      Status = EFI_COMPROMISED_DATA;
    } else {
      Offset = 0;
      while (Offset < Size) {
        if (Size - Offset < sizeof (List)) {
          Status = EFI_COMPROMISED_DATA;
          break;
        }

        CopyMem (&List, Data + Offset, sizeof (List));
        if ((List.SignatureListSize < sizeof (List)) ||
            (List.SignatureListSize > Size - Offset) ||
            (List.SignatureHeaderSize > List.SignatureListSize - sizeof (List)) ||
            (List.SignatureSize <= sizeof (EFI_GUID)) ||
            (List.SignatureListSize - sizeof (List) - List.SignatureHeaderSize < List.SignatureSize) ||
            ((List.SignatureListSize - sizeof (List) - List.SignatureHeaderSize) % List.SignatureSize != 0)) {
          Status = EFI_COMPROMISED_DATA;
          break;
        }

        Offset += List.SignatureListSize;
      }

      *Present = (BOOLEAN)!EFI_ERROR (Status);
    }
  }

  FreePool (Data);
  return Status;
}

STATIC
EFI_STATUS
SecurityKeysReady (
  OUT BOOLEAN  *Ready
  )
{
  EFI_STATUS  Status;
  BOOLEAN     Present;

  *Ready = FALSE;
  Status = HasKeyDatabase (L"PK", &gEfiGlobalVariableGuid, &Present);
  if (EFI_ERROR (Status) || !Present) {
    return Status;
  }

  Status = HasKeyDatabase (L"KEK", &gEfiGlobalVariableGuid, &Present);
  if (EFI_ERROR (Status) || !Present) {
    return Status;
  }

  Status = HasKeyDatabase (EFI_IMAGE_SECURITY_DATABASE, &gEfiImageSecurityDatabaseGuid, &Present);
  if (!EFI_ERROR (Status)) {
    *Ready = Present;
  }

  return Status;
}

STATIC
EFI_STATUS
ReadEffectiveSetting (
  IN  CHAR16    *Name,
  IN  EFI_GUID  *Guid,
  OUT BOOLEAN   *Enabled
  )
{
  EFI_STATUS  Status;
  EFI_STATUS  WriteStatus;
  BOOLEAN     Ready;
  UINT8       Disabled;

  Status = ReadSetting (Name, Guid, Enabled);
  if (Status == EFI_NOT_FOUND) {
    *Enabled = FALSE;
    return EFI_SUCCESS;
  }

  if (EFI_ERROR (Status) || !*Enabled) {
    return Status;
  }

  Status = SecurityKeysReady (&Ready);
  if (!EFI_ERROR (Status) && !Ready) {
    *Enabled    = FALSE;
    Disabled    = 0;
    WriteStatus = gRT->SetVariable (Name, Guid, BDS_SETTING_ATTRIBUTES, sizeof (Disabled), &Disabled);
    if (EFI_ERROR (WriteStatus)) {
      return WriteStatus;
    }
  }

  return Status;
}

STATIC
EFI_STATUS
WriteSetting (
  IN CHAR16    *Name,
  IN EFI_GUID  *Guid,
  IN BOOLEAN   Enabled
  )
{
  EFI_STATUS  Status;
  BOOLEAN     Current;
  BOOLEAN     Ready;
  UINT8       Value;

  Status = ReadEffectiveSetting (Name, Guid, &Current);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Enabled) {
    Status = SecurityKeysReady (&Ready);
    if (EFI_ERROR (Status) || !Ready) {
      return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
    }
  }

  Value = Enabled ? 1 : 0;
  return gRT->SetVariable (Name, Guid, BDS_SETTING_ATTRIBUTES, sizeof (Value), &Value);
}

EFI_STATUS
BdsGetSecureBootState (
  OUT BOOLEAN  *Enabled,
  OUT BOOLEAN  *SetupMode
  )
{
  EFI_STATUS  Status;
  UINT8       Secure;
  UINT8       Setup;

  if ((Enabled == NULL) || (SetupMode == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Enabled   = TRUE;
  *SetupMode = FALSE;
  Status     = ReadMode (L"SecureBoot", &Secure);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = ReadMode (L"SetupMode", &Setup);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  *Enabled   = (BOOLEAN)(Secure != 0);
  *SetupMode = (BOOLEAN)(Setup != 0);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
BdsAuthenticateState (
  IN CONST EFI_SECURITY_ARCH_PROTOCOL  *This,
  IN UINT32                            AuthenticationStatus,
  IN CONST EFI_DEVICE_PATH_PROTOCOL    *DevicePath
  )
{
  // PI requires Security2 verification before this FV authentication-state
  // notification. Readiness requires our Security2 hook to remain installed;
  // this legacy interface has no image buffer and must not invoke host policy.
  return EFI_ERROR (BdsSecurityReady ()) ? EFI_SECURITY_VIOLATION : EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
BdsAuthenticateImage (
  IN CONST EFI_SECURITY2_ARCH_PROTOCOL  *This,
  IN CONST EFI_DEVICE_PATH_PROTOCOL     *DevicePath,
  IN VOID                              *FileBuffer,
  IN UINTN                             FileSize,
  IN BOOLEAN                           BootPolicy
  )
{
  if (EFI_ERROR (BdsSecurityReady ())) {
    return EFI_SECURITY_VIOLATION;
  }

  if (!mImageVerificationEnabled) {
    return EFI_SUCCESS;
  }

  // A NULL image buffer is the Security2 connection-policy query, not image
  // authentication. LoadImage supplies the actual buffer to the call below.
  if (FileBuffer == NULL) {
    return ((DevicePath != NULL) && (FileSize == 0)) ? EFI_SUCCESS : EFI_SECURITY_VIOLATION;
  }

  if (FileSize == 0) {
    return EFI_SECURITY_VIOLATION;
  }

  // The official constructor registers in this image's SecurityManagementLib.
  // Execute only its local verification handler; never call host policy.
  return ExecuteSecurity2Handlers (
           EFI_AUTH_OPERATION_VERIFY_IMAGE,
           0,
           DevicePath,
           FileBuffer,
           FileSize,
           BootPolicy
           );
}

STATIC
EFI_STATUS
InitializeSecurity (
  VOID
  )
{
  EFI_STATUS  SecurityStatus;
  EFI_STATUS  Security2Status;
  EFI_STATUS  ModeStatus;
  UINT8       Secure;

  // DXE Core caches these protocol objects. Replace the existing methods in
  // place, without retaining or invoking the firmware's implementations. Hook
  // each available interface even on failure so it rejects subsequent loads.
  Security2Status = gBS->LocateProtocol (&gEfiSecurity2ArchProtocolGuid, NULL, (VOID **)&mSecurity2);
  if (!EFI_ERROR (Security2Status)) {
    if (mSecurity2 == NULL) {
      Security2Status = EFI_UNSUPPORTED;
    } else {
      mSecurity2->FileAuthentication = BdsAuthenticateImage;
    }
  }

  SecurityStatus = gBS->LocateProtocol (&gEfiSecurityArchProtocolGuid, NULL, (VOID **)&mSecurity);
  if (!EFI_ERROR (SecurityStatus)) {
    if (mSecurity == NULL) {
      SecurityStatus = EFI_UNSUPPORTED;
    } else {
      mSecurity->FileAuthenticationState = BdsAuthenticateState;
    }
  }

  // Only the actual global SecureBoot byte selects the application policy.
  // SetupMode and the independent fastboot setting cannot override this choice.
  ModeStatus = ReadMode (L"SecureBoot", &Secure);
  if (EFI_ERROR (ModeStatus)) {
    return ModeStatus;
  }

  if (EFI_ERROR (Security2Status)) {
    return Security2Status;
  }

  if (EFI_ERROR (SecurityStatus)) {
    return SecurityStatus;
  }

  mImageVerificationEnabled = (BOOLEAN)(Secure != 0);
  if (mImageVerificationEnabled) {
    DEBUG ((DEBUG_INFO, "BdsApp: SecureBoot=1, local edk2 image verifier active\n"));
  } else {
    DEBUG ((DEBUG_INFO, "BdsApp: SecureBoot=0, Security/Security2 allow-all stubs active\n"));
  }

  return EFI_SUCCESS;
}

EFI_STATUS
BdsSecurityInitialize (
  VOID
  )
{
  if (mInitializationStarted) {
    return mInitializationComplete ? mInitializationStatus : EFI_SUCCESS;
  }

  // Set the guard before calling any firmware service that can reenter this app.
  mInitializationStarted  = TRUE;
  mInitializationStatus   = EFI_NOT_READY;
  mInitializationStatus   = InitializeSecurity ();
  mInitializationComplete = TRUE;
  return mInitializationStatus;
}

EFI_STATUS
BdsSecurityReady (
  VOID
  )
{
  if (EFI_ERROR (mInitializationStatus)) {
    return mInitializationStatus;
  }

  if (!mInitializationComplete || (mSecurity == NULL) || (mSecurity2 == NULL) ||
      (mSecurity->FileAuthenticationState != BdsAuthenticateState) ||
      (mSecurity2->FileAuthentication != BdsAuthenticateImage)) {
    return EFI_SECURITY_VIOLATION;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
BdsSecurityGetFastbootEnabled (
  OUT BOOLEAN  *Enabled
  )
{
  if (Enabled == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return ReadEffectiveSetting (BDS_SECURE_FASTBOOT_ENABLE_NAME, &gMainlineBootVariableGuid, Enabled);
}

EFI_STATUS
BdsSecuritySetFastbootEnabled (
  IN BOOLEAN  Enabled
  )
{
  EFI_STATUS  Status;

  Status = WriteSetting (BDS_SECURE_FASTBOOT_ENABLE_NAME, &gMainlineBootVariableGuid, Enabled);
  if (!EFI_ERROR (Status)) {
    // Refresh policy immediately so a disable/enable cycle cannot retain a nonce.
    BdsSecurityAuthEnabled ();
  }

  return Status;
}

