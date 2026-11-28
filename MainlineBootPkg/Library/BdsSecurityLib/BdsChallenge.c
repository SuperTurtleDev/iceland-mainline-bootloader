/** @file
  Fastboot challenge authorization using the standard UEFI db and dbx databases.

  The signed message is exactly BDS_SECURITY_CHALLENGE_SIZE unpredictable bytes
  obtained from EFI_RNG_PROTOCOL. There is no fixed nonce, string conversion, or
  prefix in the signed message. A nonce belongs only to this fastboot exchange;
  every verification attempt consumes it. Image verification is independent.

  The primary response is a complete DER PKCS#7 ContentInfo containing detached
  SignedData with exactly one signer. Include the signer and intermediate X.509
  certificates. This edk2 Pkcs7GetCertificatesList supports only one signer, so
  multiple signers are deliberately rejected. All embedded certificates, even
  unchained ones, and every successful db anchor are subject to dbx revocation.
  EFI_CERT_X509_SHA256/384/512 revocations apply regardless of their timestamps:
  a fresh nonce has no historic signing-time exception. Plain image hashes in
  dbx do not describe nonce signers. Unknown or raw-key dbx types fail closed.

  Legacy raw RSA PKCS#1 v1.5 signatures of SHA256(nonce) remain supported for
  2048..8192-bit keys pinned by X.509 certificates in db. Raw ECDSA is unsupported;
  use PKCS#7 for ECDSA. All crypto and certificate decoding use BaseCryptLib.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/ImageAuthentication.h>
#include <Protocol/Rng.h>
#include <Library/BaseCryptLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BdsSecurityLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#define BDS_DATABASE_MAX_SIZE        (1024 * 1024)
#define BDS_DATABASE_MAX_SIGNATURES  4096
#define BDS_CERTIFICATE_MAX_SIZE     (16 * 1024)
#define BDS_RESPONSE_MAX_SIZE        (64 * 1024)
#define BDS_RSA_MIN_SIZE             256
#define BDS_RSA_MAX_SIZE             1024

typedef enum {
  BdsImageHash,
  BdsX509Certificate,
  BdsX509Sha256,
  BdsX509Sha384,
  BdsX509Sha512,
  BdsUnsupportedSignature
} BDS_SIGNATURE_KIND;

typedef struct {
  EFI_GUID            Guid;
  UINTN               DataSize;
  BDS_SIGNATURE_KIND  Kind;
} BDS_SIGNATURE_TYPE;

STATIC CONST BDS_SIGNATURE_TYPE  mSignatureTypes[] = {
  { EFI_CERT_X509_GUID,         0,                            BdsX509Certificate },
  { EFI_CERT_X509_SHA256_GUID,  sizeof (EFI_CERT_X509_SHA256), BdsX509Sha256 },
  { EFI_CERT_X509_SHA384_GUID,  sizeof (EFI_CERT_X509_SHA384), BdsX509Sha384 },
  { EFI_CERT_X509_SHA512_GUID,  sizeof (EFI_CERT_X509_SHA512), BdsX509Sha512 },
  { EFI_CERT_SHA1_GUID,         20,                           BdsImageHash },
  { EFI_CERT_SHA224_GUID,       28,                           BdsImageHash },
  { EFI_CERT_SHA256_GUID,       SHA256_DIGEST_SIZE,           BdsImageHash },
  { EFI_CERT_SHA384_GUID,       SHA384_DIGEST_SIZE,           BdsImageHash },
  { EFI_CERT_SHA512_GUID,       SHA512_DIGEST_SIZE,           BdsImageHash },
  { EFI_CERT_RSA2048_GUID,      256,                          BdsUnsupportedSignature },
  { EFI_CERT_RSA2048_SHA256_GUID, 256,                        BdsUnsupportedSignature },
  { EFI_CERT_RSA2048_SHA1_GUID, 256,                          BdsUnsupportedSignature }
};

typedef struct {
  UINT8    *Data;
  UINTN    Size;
  BOOLEAN  HasCertificates;
} BDS_SIGNATURE_DATABASE;

typedef struct {
  CONST BDS_SIGNATURE_DATABASE  *Database;
  UINTN                         ListOffset;
  UINTN                         EntryOffset;
  EFI_SIGNATURE_LIST            List;
} BDS_DATABASE_CURSOR;

STATIC UINT8    mChallenge[BDS_SECURITY_CHALLENGE_SIZE];
STATIC UINT8    mDatabaseFingerprint[SHA256_DIGEST_SIZE];
STATIC BOOLEAN  mFingerprintValid;
STATIC BOOLEAN  mChallengeValid;
STATIC BOOLEAN  mPrivileged;
STATIC BOOLEAN  mAuthRequired = TRUE;

STATIC
VOID
InvalidateChallenge (
  VOID
  )
{
  mChallengeValid = FALSE;
  ZeroMem (mChallenge, sizeof (mChallenge));
}

STATIC
VOID
InvalidateAuthorization (
  VOID
  )
{
  InvalidateChallenge ();
  mPrivileged = FALSE;
}

STATIC
CONST BDS_SIGNATURE_TYPE *
GetSignatureType (
  IN CONST EFI_GUID  *Guid
  )
{
  UINTN  Index;

  for (Index = 0; Index < ARRAY_SIZE (mSignatureTypes); Index++) {
    if (CompareGuid (Guid, &mSignatureTypes[Index].Guid)) {
      return &mSignatureTypes[Index];
    }
  }

  return NULL;
}

STATIC
VOID
FreeDatabase (
  IN OUT BDS_SIGNATURE_DATABASE  *Database
  )
{
  if (Database->Data != NULL) {
    FreePool (Database->Data);
  }

  ZeroMem (Database, sizeof (*Database));
}

/** Read one bounded snapshot. A size/read race fails closed for this decision. */
STATIC
EFI_STATUS
ReadDatabase (
  IN CHAR16                     *Name,
  OUT BDS_SIGNATURE_DATABASE    *Database
  )
{
  EFI_STATUS  Status;
  UINTN       Size;
  UINTN       Capacity;

  if ((gRT == NULL) || (gRT->GetVariable == NULL)) {
    return EFI_NOT_READY;
  }

  Size = 0;
  Status = gRT->GetVariable (Name, &gEfiImageSecurityDatabaseGuid, NULL, &Size, NULL);
  if ((Status == EFI_NOT_FOUND) || ((Status == EFI_SUCCESS) && (Size == 0))) {
    return EFI_SUCCESS;
  }

  if (Status != EFI_BUFFER_TOO_SMALL) {
    return EFI_ERROR (Status) ? Status : EFI_COMPROMISED_DATA;
  }

  if ((Size == 0) || (Size > BDS_DATABASE_MAX_SIZE)) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Database->Data = AllocatePool (Size);
  if (Database->Data == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Capacity = Size;
  Status = gRT->GetVariable (
                  Name,
                  &gEfiImageSecurityDatabaseGuid,
                  NULL,
                  &Size,
                  Database->Data
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Status != EFI_SUCCESS) || (Size > Capacity)) {
    return EFI_COMPROMISED_DATA;
  }

  Database->Size = Size;
  return EFI_SUCCESS;
}

/** Validate complete ESL framing and every certificate before using a database. */
STATIC
EFI_STATUS
ValidateDatabase (
  IN OUT BDS_SIGNATURE_DATABASE  *Database,
  IN BOOLEAN                    Revocations
  )
{
  EFI_SIGNATURE_LIST         List;
  CONST BDS_SIGNATURE_TYPE   *Type;
  UINTN                      Offset;
  UINTN                      EntryOffset;
  UINTN                      EntriesSize;
  UINTN                      DataSize;
  UINTN                      SignatureCount;
  UINT8                      *Certificate;

  Offset = 0;
  SignatureCount = 0;
  while (Offset < Database->Size) {
    if (Database->Size - Offset < sizeof (List)) {
      return EFI_COMPROMISED_DATA;
    }

    CopyMem (&List, Database->Data + Offset, sizeof (List));
    if ((List.SignatureListSize < sizeof (List)) ||
        (List.SignatureListSize > Database->Size - Offset) ||
        (List.SignatureHeaderSize > List.SignatureListSize - sizeof (List)) ||
        (List.SignatureSize <= sizeof (EFI_GUID))) {
      return EFI_COMPROMISED_DATA;
    }

    EntriesSize = List.SignatureListSize - sizeof (List) - List.SignatureHeaderSize;
    if ((EntriesSize == 0) || ((EntriesSize % List.SignatureSize) != 0)) {
      return EFI_COMPROMISED_DATA;
    }

    SignatureCount += EntriesSize / List.SignatureSize;
    if (SignatureCount > BDS_DATABASE_MAX_SIGNATURES) {
      return EFI_BAD_BUFFER_SIZE;
    }

    Type = GetSignatureType (&List.SignatureType);
    DataSize = List.SignatureSize - sizeof (EFI_GUID);
    if (Type != NULL) {
      if ((List.SignatureHeaderSize != 0) ||
          ((Type->DataSize != 0) && (DataSize != Type->DataSize))) {
        return EFI_COMPROMISED_DATA;
      }
    }

    if (Revocations &&
        ((Type == NULL) || (Type->Kind == BdsUnsupportedSignature))) {
      return EFI_UNSUPPORTED;
    }

    if ((Type != NULL) && (Type->Kind == BdsX509Certificate)) {
      if (DataSize > BDS_CERTIFICATE_MAX_SIZE) {
        return EFI_BAD_BUFFER_SIZE;
      }

      EntryOffset = sizeof (List) + List.SignatureHeaderSize;
      while (EntryOffset < List.SignatureListSize) {
        Certificate = NULL;
        if (!X509ConstructCertificate (
               Database->Data + Offset + EntryOffset + sizeof (EFI_GUID),
               DataSize,
               &Certificate
               )) {
          return EFI_COMPROMISED_DATA;
        }

        X509Free (Certificate);
        Database->HasCertificates = TRUE;
        EntryOffset += List.SignatureSize;
      }
    }

    Offset += List.SignatureListSize;
  }

  return EFI_SUCCESS;
}

/** Iterate only databases which have passed ValidateDatabase. */
STATIC
BOOLEAN
NextSignature (
  IN OUT BDS_DATABASE_CURSOR    *Cursor,
  OUT CONST BDS_SIGNATURE_TYPE  **Type,
  OUT CONST UINT8               **Data,
  OUT UINTN                     *DataSize
  )
{
  if (Cursor->EntryOffset >= Cursor->List.SignatureListSize) {
    if (Cursor->ListOffset == Cursor->Database->Size) {
      return FALSE;
    }

    CopyMem (
      &Cursor->List,
      Cursor->Database->Data + Cursor->ListOffset,
      sizeof (Cursor->List)
      );
    Cursor->ListOffset += Cursor->List.SignatureListSize;
    Cursor->EntryOffset = sizeof (Cursor->List) + Cursor->List.SignatureHeaderSize;
  }

  *Type = GetSignatureType (&Cursor->List.SignatureType);
  *Data = Cursor->Database->Data + Cursor->ListOffset - Cursor->List.SignatureListSize +
          Cursor->EntryOffset + sizeof (EFI_GUID);
  *DataSize = Cursor->List.SignatureSize - sizeof (EFI_GUID);
  Cursor->EntryOffset += Cursor->List.SignatureSize;
  return TRUE;
}

/**
  Refresh on EVERY public authorization decision. Frame the two database hashes
  with their lengths, so moving bytes between db and dbx cannot preserve policy.
  Missing and zero-length variables intentionally have the same empty policy.
**/
STATIC
EFI_STATUS
RefreshAuthorization (
  OUT BDS_SIGNATURE_DATABASE  *Db,
  OUT BDS_SIGNATURE_DATABASE  *Dbx
  )
{
  EFI_STATUS  Status;
  UINT8       Frame[2 * sizeof (UINT64) + 2 * SHA256_DIGEST_SIZE];
  UINT8       Fingerprint[SHA256_DIGEST_SIZE];
  BOOLEAN     Enabled;

  ZeroMem (Db, sizeof (*Db));
  ZeroMem (Dbx, sizeof (*Dbx));
  Status = BdsSecurityGetFastbootEnabled (&Enabled);
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  if (!Enabled) {
    InvalidateAuthorization ();
    mFingerprintValid = FALSE;
    mAuthRequired = FALSE;
    return EFI_SUCCESS;
  }

  Status = ReadDatabase (EFI_IMAGE_SECURITY_DATABASE, Db);
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  Status = ReadDatabase (EFI_IMAGE_SECURITY_DATABASE1, Dbx);
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  Status = ValidateDatabase (Db, FALSE);
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  Status = ValidateDatabase (Dbx, TRUE);
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  WriteUnaligned64 ((UINT64 *)Frame, (UINT64)Db->Size);
  WriteUnaligned64 ((UINT64 *)(Frame + sizeof (UINT64)), (UINT64)Dbx->Size);
  if (!Sha256HashAll (Db->Data, Db->Size, Frame + 2 * sizeof (UINT64)) ||
      !Sha256HashAll (Dbx->Data, Dbx->Size, Frame + 2 * sizeof (UINT64) + SHA256_DIGEST_SIZE) ||
      !Sha256HashAll (Frame, sizeof (Frame), Fingerprint)) {
    Status = EFI_DEVICE_ERROR;
    goto Error;
  }

  if (!mFingerprintValid ||
      (CompareMem (Fingerprint, mDatabaseFingerprint, sizeof (Fingerprint)) != 0)) {
    InvalidateAuthorization ();
  }

  CopyMem (mDatabaseFingerprint, Fingerprint, sizeof (Fingerprint));
  mFingerprintValid = TRUE;
  mAuthRequired = (BOOLEAN)((Db->Size != 0) || (Dbx->Size != 0));
  return EFI_SUCCESS;

Error:
  InvalidateAuthorization ();
  mFingerprintValid = FALSE;
  mAuthRequired = TRUE;
  FreeDatabase (Db);
  FreeDatabase (Dbx);
  return Status;
}

STATIC
EFI_STATUS
RefreshAuthorizationState (
  VOID
  )
{
  EFI_STATUS              Status;
  BDS_SIGNATURE_DATABASE  Db;
  BDS_SIGNATURE_DATABASE  Dbx;

  Status = RefreshAuthorization (&Db, &Dbx);
  FreeDatabase (&Db);
  FreeDatabase (&Dbx);
  return Status;
}

BOOLEAN
BdsSecurityAuthEnabled (
  VOID
  )
{
  return (BOOLEAN)(EFI_ERROR (RefreshAuthorizationState ()) || mAuthRequired);
}

BOOLEAN
BdsSecurityLocked (
  VOID
  )
{
  return (BOOLEAN)(EFI_ERROR (RefreshAuthorizationState ()) ||
                   (mAuthRequired && !mPrivileged));
}

BOOLEAN
BdsSecurityIsPrivileged (
  VOID
  )
{
  return (BOOLEAN)(!EFI_ERROR (RefreshAuthorizationState ()) && mPrivileged);
}

CONST UINT8 *
BdsSecurityChallenge (
  VOID
  )
{
  EFI_STATUS              Status;
  EFI_RNG_PROTOCOL        *Rng;
  BDS_SIGNATURE_DATABASE  Db;
  BDS_SIGNATURE_DATABASE  Dbx;
  BOOLEAN                 CanAuthorize;

  Status = RefreshAuthorization (&Db, &Dbx);
  CanAuthorize = Db.HasCertificates;
  FreeDatabase (&Db);
  FreeDatabase (&Dbx);
  if (EFI_ERROR (Status) || !CanAuthorize) {
    InvalidateChallenge ();
    return NULL;
  }

  if (mChallengeValid) {
    return mChallenge;
  }

  if ((gBS == NULL) || (gBS->LocateProtocol == NULL)) {
    return NULL;
  }

  Rng = NULL;
  Status = gBS->LocateProtocol (&gEfiRngProtocolGuid, NULL, (VOID **)&Rng);
  if (EFI_ERROR (Status) || (Rng == NULL) || (Rng->GetRNG == NULL)) {
    return NULL;
  }

  Status = Rng->GetRNG (Rng, NULL, sizeof (mChallenge), mChallenge);
  if (Status != EFI_SUCCESS) {
    InvalidateChallenge ();
    return NULL;
  }

  mChallengeValid = TRUE;
  return mChallenge;
}

/** Check a certificate against every certificate-related dbx entry. */
STATIC
EFI_STATUS
CheckCertificateRevocation (
  IN CONST BDS_SIGNATURE_DATABASE  *Dbx,
  IN CONST UINT8                  *Certificate,
  IN UINTN                        CertificateSize
  )
{
  BDS_DATABASE_CURSOR       Cursor;
  CONST BDS_SIGNATURE_TYPE  *Type;
  CONST UINT8               *Data;
  UINTN                     DataSize;
  UINT8                     *Tbs;
  UINTN                     TbsSize;
  UINT8                     Digest[SHA512_DIGEST_SIZE];
  UINTN                     DigestSize;
  BOOLEAN                   Hashed;

  ZeroMem (&Cursor, sizeof (Cursor));
  Cursor.Database = Dbx;
  Tbs = NULL;
  TbsSize = 0;
  while (NextSignature (&Cursor, &Type, &Data, &DataSize)) {
    if (Type->Kind == BdsImageHash) {
      continue;
    }

    if (Type->Kind == BdsX509Certificate) {
      if (((CertificateSize == DataSize) &&
           (CompareMem (Certificate, Data, DataSize) == 0)) ||
          X509VerifyCert (Certificate, CertificateSize, Data, DataSize)) {
        return EFI_SECURITY_VIOLATION;
      }

      continue;
    }

    if (Tbs == NULL) {
      if (!X509GetTBSCert (Certificate, CertificateSize, &Tbs, &TbsSize) ||
          ((UINTN)Tbs < (UINTN)Certificate) ||
          ((UINTN)Tbs - (UINTN)Certificate > CertificateSize) ||
          (TbsSize == 0) ||
          (TbsSize > CertificateSize - ((UINTN)Tbs - (UINTN)Certificate))) {
        return EFI_COMPROMISED_DATA;
      }
    }

    switch (Type->Kind) {
      case BdsX509Sha256:
        DigestSize = SHA256_DIGEST_SIZE;
        Hashed = Sha256HashAll (Tbs, TbsSize, Digest);
        break;
      case BdsX509Sha384:
        DigestSize = SHA384_DIGEST_SIZE;
        Hashed = Sha384HashAll (Tbs, TbsSize, Digest);
        break;
      case BdsX509Sha512:
        DigestSize = SHA512_DIGEST_SIZE;
        Hashed = Sha512HashAll (Tbs, TbsSize, Digest);
        break;
      default:
        return EFI_UNSUPPORTED;
    }

    if (!Hashed) {
      return EFI_DEVICE_ERROR;
    }

    if (CompareMem (Digest, Data, DigestSize) == 0) {
      return EFI_SECURITY_VIOLATION;
    }
  }

  return EFI_SUCCESS;
}

/** Validate BaseCryptLib's counted EFI_CERT_STACK, not ASN.1 data. */
STATIC
EFI_STATUS
CheckCertificateStack (
  IN CONST BDS_SIGNATURE_DATABASE  *Dbx,
  IN CONST UINT8                  *Stack,
  IN UINTN                        StackSize
  )
{
  EFI_STATUS  Status;
  UINTN       Offset;
  UINTN       Index;
  UINT32      CertificateSize;

  if ((Stack == NULL) || (StackSize == 0) || (StackSize > BDS_RESPONSE_MAX_SIZE)) {
    return EFI_COMPROMISED_DATA;
  }

  Offset = sizeof (UINT8);
  for (Index = 0; Index < Stack[0]; Index++) {
    if (StackSize - Offset < sizeof (CertificateSize)) {
      return EFI_COMPROMISED_DATA;
    }

    CopyMem (&CertificateSize, Stack + Offset, sizeof (CertificateSize));
    Offset += sizeof (CertificateSize);
    if ((CertificateSize == 0) || (CertificateSize > BDS_CERTIFICATE_MAX_SIZE) ||
        (CertificateSize > StackSize - Offset)) {
      return EFI_COMPROMISED_DATA;
    }

    Status = CheckCertificateRevocation (Dbx, Stack + Offset, CertificateSize);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Offset += CertificateSize;
  }

  return (Offset == StackSize) ? EFI_SUCCESS : EFI_COMPROMISED_DATA;
}

/**
  Guard the fixed ContentInfo envelope recognized by this edk2 WrapPkcs7Data.
  That implementation reads through byte 16 without a minimum-size check, and
  Pkcs7GetCertificatesList cannot safely release its synthetic wrapping buffer.
  Require complete, conventionally encoded DER ContentInfo, never bare
  SignedData. This fixed framing check does not decode certificates or ASN.1.
**/
STATIC
BOOLEAN
IsPkcs7Response (
  IN CONST UINT8  *Signature,
  IN UINTN        SignatureSize
  )
{
  STATIC CONST UINT8  ContentInfoPrefix[] = {
    0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x02, 0xA0, 0x82
  };

  return (BOOLEAN)((SignatureSize >= 19) &&
                   (Signature[0] == 0x30) && (Signature[1] == 0x82) &&
                   (CompareMem (Signature + 4, ContentInfoPrefix, sizeof (ContentInfoPrefix)) == 0) &&
                   ((((UINTN)Signature[2] << 8) | Signature[3]) == SignatureSize - 4) &&
                   ((((UINTN)Signature[17] << 8) | Signature[18]) == SignatureSize - 19));
}

STATIC
EFI_STATUS
VerifyPkcs7Response (
  IN CONST BDS_SIGNATURE_DATABASE  *Db,
  IN CONST BDS_SIGNATURE_DATABASE  *Dbx,
  IN CONST UINT8                  *Signature,
  IN UINTN                        SignatureSize,
  IN CONST UINT8                  *Nonce
  )
{
  EFI_STATUS                Status;
  BDS_DATABASE_CURSOR       Cursor;
  CONST BDS_SIGNATURE_TYPE  *Type;
  CONST UINT8               *Certificate;
  UINTN                     CertificateSize;
  UINT8                     *Chain;
  UINTN                     ChainSize;
  UINT8                     *Unchained;
  UINTN                     UnchainedSize;
  VOID                      *Content;
  UINTN                     ContentSize;
  BOOLEAN                   Allowed;

  Chain = NULL;
  ChainSize = 0;
  Unchained = NULL;
  UnchainedSize = 0;
  Content = NULL;
  ContentSize = 0;
  Status = EFI_SECURITY_VIOLATION;

  // A revoked root may be omitted from the embedded certificates altogether.
  ZeroMem (&Cursor, sizeof (Cursor));
  Cursor.Database = Dbx;
  while (NextSignature (&Cursor, &Type, &Certificate, &CertificateSize)) {
    if ((Type->Kind == BdsX509Certificate) &&
        Pkcs7Verify (Signature, SignatureSize, Certificate, CertificateSize,
          Nonce, BDS_SECURITY_CHALLENGE_SIZE)) {
      Status = EFI_SECURITY_VIOLATION;
      goto Done;
    }
  }

  Allowed = FALSE;
  ZeroMem (&Cursor, sizeof (Cursor));
  Cursor.Database = Db;
  while (NextSignature (&Cursor, &Type, &Certificate, &CertificateSize)) {
    if ((Type == NULL) || (Type->Kind != BdsX509Certificate)) {
      continue;
    }

    if (Pkcs7Verify (Signature, SignatureSize, Certificate, CertificateSize,
          Nonce, BDS_SECURITY_CHALLENGE_SIZE)) {
      Status = CheckCertificateRevocation (Dbx, Certificate, CertificateSize);
      if (EFI_ERROR (Status)) {
        goto Done;
      }

      // Do not let another db anchor mask a revoked successful anchor.
      Allowed = TRUE;
    }
  }

  Status = EFI_SECURITY_VIOLATION;
  if (!Allowed ||
      !Pkcs7GetAttachedContent (Signature, SignatureSize, &Content, &ContentSize) ||
      (Content != NULL) || (ContentSize != 0)) {
    goto Done;
  }

  // This older edk2 enumerator leaks some internal X509 references. Invoke it
  // only after verifying the response against db; untrusted inputs cannot
  // consume those resources. No privilege is granted until every certificate
  // has also passed the following revocation checks.
  if (!Pkcs7GetCertificatesList (
         Signature,
         SignatureSize,
         &Chain,
         &ChainSize,
         &Unchained,
         &UnchainedSize
         )) {
    goto Done;
  }

  if ((Chain == NULL) || (ChainSize == 0) || (Chain[0] == 0)) {
    goto Done;
  }

  Status = CheckCertificateStack (Dbx, Chain, ChainSize);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if ((Unchained != NULL) || (UnchainedSize != 0)) {
    Status = CheckCertificateStack (Dbx, Unchained, UnchainedSize);
  }

Done:
  if (Content != NULL) {
    FreePool (Content);
  }

  Pkcs7FreeSigners (Chain);
  Pkcs7FreeSigners (Unchained);
  return Status;
}

STATIC
BOOLEAN
VerifyRsaSignature (
  IN CONST UINT8  *Certificate,
  IN UINTN        CertificateSize,
  IN CONST UINT8  *Signature,
  IN UINTN        SignatureSize,
  IN CONST UINT8  *Digest
  )
{
  VOID     *Rsa;
  UINTN    ModulusSize;
  BOOLEAN  Valid;

  Rsa = NULL;
  if (!RsaGetPublicKeyFromX509 (Certificate, CertificateSize, &Rsa)) {
    return FALSE;
  }

  ModulusSize = BDS_RSA_MAX_SIZE;
  Valid = (BOOLEAN)(RsaGetKey (Rsa, RsaKeyN, NULL, &ModulusSize) &&
                    (ModulusSize == SignatureSize) &&
                    RsaPkcs1Verify (Rsa, Digest, SHA256_DIGEST_SIZE, Signature, SignatureSize));
  RsaFree (Rsa);
  return Valid;
}

STATIC
EFI_STATUS
VerifyRawRsaResponse (
  IN CONST BDS_SIGNATURE_DATABASE  *Db,
  IN CONST BDS_SIGNATURE_DATABASE  *Dbx,
  IN CONST UINT8                  *Signature,
  IN UINTN                        SignatureSize,
  IN CONST UINT8                  *Nonce
  )
{
  EFI_STATUS                Status;
  BDS_DATABASE_CURSOR       Cursor;
  CONST BDS_SIGNATURE_TYPE  *Type;
  CONST UINT8               *Certificate;
  UINTN                     CertificateSize;
  UINT8                     Digest[SHA256_DIGEST_SIZE];
  BOOLEAN                   Allowed;

  if ((SignatureSize < BDS_RSA_MIN_SIZE) || (SignatureSize > BDS_RSA_MAX_SIZE)) {
    return EFI_SECURITY_VIOLATION;
  }

  if (!Sha256HashAll (Nonce, BDS_SECURITY_CHALLENGE_SIZE, Digest)) {
    return EFI_DEVICE_ERROR;
  }

  // A different certificate for the same RSA key must not bypass dbx.
  ZeroMem (&Cursor, sizeof (Cursor));
  Cursor.Database = Dbx;
  while (NextSignature (&Cursor, &Type, &Certificate, &CertificateSize)) {
    if ((Type->Kind == BdsX509Certificate) &&
        VerifyRsaSignature (Certificate, CertificateSize, Signature, SignatureSize, Digest)) {
      return EFI_SECURITY_VIOLATION;
    }
  }

  Allowed = FALSE;
  ZeroMem (&Cursor, sizeof (Cursor));
  Cursor.Database = Db;
  while (NextSignature (&Cursor, &Type, &Certificate, &CertificateSize)) {
    if ((Type == NULL) || (Type->Kind != BdsX509Certificate)) {
      continue;
    }

    if (VerifyRsaSignature (Certificate, CertificateSize, Signature, SignatureSize, Digest)) {
      Status = CheckCertificateRevocation (Dbx, Certificate, CertificateSize);
      if (EFI_ERROR (Status)) {
        return Status;
      }

      Allowed = TRUE;
    }
  }

  return Allowed ? EFI_SUCCESS : EFI_SECURITY_VIOLATION;
}

EFI_STATUS
BdsSecurityVerifyChallenge (
  IN CONST UINT8  *Signature,
  IN UINTN        SignatureSize
  )
{
  EFI_STATUS              Status;
  BDS_SIGNATURE_DATABASE  Db;
  BDS_SIGNATURE_DATABASE  Dbx;
  UINT8                   Nonce[BDS_SECURITY_CHALLENGE_SIZE];
  UINT8                   Fingerprint[SHA256_DIGEST_SIZE];
  BOOLEAN                 HadChallenge;

  Status = RefreshAuthorization (&Db, &Dbx);
  HadChallenge = mChallengeValid;
  CopyMem (Nonce, mChallenge, sizeof (Nonce));
  CopyMem (Fingerprint, mDatabaseFingerprint, sizeof (Fingerprint));
  // Consume even malformed responses and revoke any previous grant on failure.
  InvalidateAuthorization ();
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if ((Signature == NULL) || (SignatureSize == 0)) {
    Status = EFI_INVALID_PARAMETER;
    goto Done;
  }

  if ((SignatureSize > BDS_RESPONSE_MAX_SIZE) || !HadChallenge || !Db.HasCertificates) {
    Status = EFI_SECURITY_VIOLATION;
    goto Done;
  }

  if (IsPkcs7Response (Signature, SignatureSize)) {
    Status = VerifyPkcs7Response (&Db, &Dbx, Signature, SignatureSize, Nonce);
  } else {
    Status = VerifyRawRsaResponse (&Db, &Dbx, Signature, SignatureSize, Nonce);
  }

Done:
  ZeroMem (Nonce, sizeof (Nonce));
  FreeDatabase (&Db);
  FreeDatabase (&Dbx);
  if (!EFI_ERROR (Status)) {
    // Recheck after crypto as well as before it: never grant for stale policy.
    Status = RefreshAuthorizationState ();
    if (!EFI_ERROR (Status)) {
      if (CompareMem (Fingerprint, mDatabaseFingerprint, sizeof (Fingerprint)) != 0) {
        Status = EFI_SECURITY_VIOLATION;
      } else {
        mPrivileged = TRUE;
      }
    }
  }

  return Status;
}
