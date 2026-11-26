/*
 * Challenge-response authorization state for the SFB security policy.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/SfbSecurity.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/Rng.h>

#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>

#include "SfbSecurityInternal.h"

/* Authorization requires unpredictable entropy. Failure never publishes a
 * challenge derived from clocks, addresses, or uninitialized memory. */
STATIC
EFI_STATUS
FillNonce (
  OUT UINT8  *Nonce
  )
{
  EFI_STATUS        Status;
  EFI_RNG_PROTOCOL  *Rng = NULL;

  ZeroMem (Nonce, SFB_SECURITY_CHALLENGE_SIZE);
  Status = gBS->LocateProtocol (&gEfiRngProtocolGuid, NULL, (VOID **)&Rng);
  if (EFI_ERROR (Status) || (Rng == NULL) || (Rng->GetRNG == NULL)) {
    return EFI_NOT_READY;
  }

  Status = Rng->GetRNG (Rng, NULL, SFB_SECURITY_CHALLENGE_SIZE, Nonce);
  if (EFI_ERROR (Status)) {
    ZeroMem (Nonce, SFB_SECURITY_CHALLENGE_SIZE);
  }

  return Status;
}

STATIC BOOLEAN  mPrivileged = FALSE;
STATIC UINT8    mChallenge[SFB_SECURITY_CHALLENGE_SIZE];
STATIC BOOLEAN  mChallengeValid = FALSE;

BOOLEAN
SfbSecurityLocked (
  VOID
  )
{
  return (BOOLEAN)(SfbSecurityAuthEnabled () && !mPrivileged);
}

BOOLEAN
SfbSecurityIsPrivileged (
  VOID
  )
{
  /* Privilege only ever comes from a successful challenge response - never
   * from the mere absence of class A certificates. */
  return mPrivileged;
}

VOID
SfbSecurityGrantPrivilege (
  VOID
  )
{
  if (EFI_ERROR (SfbCertsAuthPool ()->Status)) {
    return;
  }

  mPrivileged     = TRUE;
  mChallengeValid = FALSE;
  ZeroMem (mChallenge, sizeof (mChallenge));
  DEBUG ((EFI_D_INFO, "SFB-SEC: privilege granted for this boot\n"));
}

VOID
SfbSecurityReset (
  VOID
  )
{
  mPrivileged     = FALSE;
  mChallengeValid = FALSE;
  ZeroMem (mChallenge, sizeof (mChallenge));
  SfbCertsReset ();
}

VOID
SfbSecurityNewChallenge (
  VOID
  )
{
  mChallengeValid = FALSE;
  ZeroMem (mChallenge, sizeof (mChallenge));
}

CONST UINT8 *
SfbSecurityChallenge (
  VOID
  )
{
  if (!mChallengeValid) {
    EFI_STATUS  Status = FillNonce (mChallenge);

    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "SFB-SEC: secure challenge unavailable: %r\n", Status));
      return NULL;
    }

    mChallengeValid = TRUE;
  }

  return mChallenge;
}

/* Verify Signature over SHA-256(Message) with the key of one certificate. */
STATIC
BOOLEAN
VerifyWithCert (
  IN mbedtls_x509_crt  *Crt,
  IN CONST UINT8       *Message,
  IN UINTN             MessageSize,
  IN CONST UINT8       *Signature,
  IN UINTN             SignatureSize
  )
{
  UINT8  Hash[32];
  INT32  MbedErr;

  if (mbedtls_md (
        mbedtls_md_info_from_type (MBEDTLS_MD_SHA256),
        Message,
        MessageSize,
        Hash
        ) != 0)
  {
    return FALSE;
  }

  MbedErr = mbedtls_pk_verify (
              &Crt->pk,
              MBEDTLS_MD_SHA256,
              Hash,
              sizeof (Hash),
              Signature,
              SignatureSize
              );
  return (BOOLEAN)(MbedErr == 0);
}

EFI_STATUS
SfbSecurityVerifyChallenge (
  IN CONST UINT8  *Signature,
  IN UINTN        SignatureSize
  )
{
  CONST SFB_CERT_POOL  *Pool  = SfbCertsAuthPool ();
  CONST UINT8          *Nonce = mChallenge;
  UINTN                Index;
  mbedtls_x509_crt     Crt;
  BOOLEAN              Verified = FALSE;

  /* Only a previously issued nonce may be answered, once. */
  if (!mChallengeValid) {
    return EFI_NOT_READY;
  }

  mChallengeValid = FALSE;

  if ((Signature == NULL) || (SignatureSize == 0) || EFI_ERROR (Pool->Status) || (Pool->Count == 0)) {
    ZeroMem (mChallenge, sizeof (mChallenge));
    return EFI_SECURITY_VIOLATION;
  }

  for (Index = 0; Index < Pool->Count && !Verified; Index++) {
    mbedtls_x509_crt_init (&Crt);
    if (mbedtls_x509_crt_parse_der (
          &Crt,
          Pool->Der[Index],
          Pool->DerSize[Index]
          ) == 0)
    {
      Verified = VerifyWithCert (
                   &Crt,
                   Nonce,
                   SFB_SECURITY_CHALLENGE_SIZE,
                   Signature,
                   SignatureSize
                   );
    }

    mbedtls_x509_crt_free (&Crt);
  }

  ZeroMem ((VOID *)Nonce, SFB_SECURITY_CHALLENGE_SIZE);

  if (Verified) {
    SfbSecurityGrantPrivilege ();
    return EFI_SUCCESS;
  }

  DEBUG ((EFI_D_ERROR, "SFB-SEC: challenge response rejected\n"));
  return EFI_SECURITY_VIOLATION;
}
