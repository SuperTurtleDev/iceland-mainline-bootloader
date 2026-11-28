/** @file
  Standard UEFI image policy and db/dbx-backed fastboot authorization.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef BDS_SECURITY_LIB_H_
#define BDS_SECURITY_LIB_H_

#include <Uefi.h>

#define BDS_SECURITY_CHALLENGE_SIZE        32
#define BDS_SECURE_FASTBOOT_ENABLE_NAME    L"EnableSecureFastboot"

extern EFI_GUID  gMainlineBootVariableGuid;

/**
  Initialize security once per application instance. Recursive calls are no-ops;
  BdsSecurityReady reports EFI_NOT_READY until the first invocation completes.
  Later calls return the first result without repeating firmware side effects.
  Read the actual global SecureBoot UINT8 once: 1 selects the locally linked edk2
  image verifier, 0 selects allow-all stubs for this application lifetime.
  Missing or malformed state fails closed. SetupMode is not an input to policy.
  Replace the existing Security/Security2 methods without calling host policy or
  installing protocols. Both standard protocol objects must already exist.
**/
EFI_STATUS
BdsSecurityInitialize (
  VOID
  );

/** Read current SecureBoot and SetupMode for display; do not alter image policy. */
EFI_STATUS
BdsGetSecureBootState (
  OUT BOOLEAN  *Enabled,
  OUT BOOLEAN  *SetupMode
  );

EFI_STATUS
BdsSecurityReady (
  VOID
  );

/**
  Read the independent NV|BS fastboot switch. Missing means disabled.
  An enabled switch is reset to disabled when PK, KEK or db is missing.
**/
EFI_STATUS
BdsSecurityGetFastbootEnabled (
  OUT BOOLEAN  *Enabled
  );

EFI_STATUS
BdsSecuritySetFastbootEnabled (
  IN BOOLEAN  Enabled
  );

BOOLEAN
BdsSecurityAuthEnabled (
  VOID
  );

BOOLEAN
BdsSecurityLocked (
  VOID
  );

BOOLEAN
BdsSecurityIsPrivileged (
  VOID
  );

/** NULL if a cryptographically secure nonce cannot be generated. */
CONST UINT8 *
BdsSecurityChallenge (
  VOID
  );

/** Verify a one-shot response using certificates from db, subject to dbx. */
EFI_STATUS
BdsSecurityVerifyChallenge (
  IN CONST UINT8  *Signature,
  IN UINTN        SignatureSize
  );

#endif
