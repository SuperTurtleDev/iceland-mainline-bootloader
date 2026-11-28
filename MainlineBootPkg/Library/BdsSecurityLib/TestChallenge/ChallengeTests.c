/** @file
  Exercise the production challenge API with real edk2 BaseCryptLib/OpenSSL.
  Only runtime/boot services and RNG are fixtures. No production test hooks.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "HostSupport.h"
#include <Guid/ImageAuthentication.h>
#include <Protocol/Rng.h>
#include <Library/BdsSecurityLib.h>
#include <openssl/pkcs7.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#define CHECK(Expression) do { \
  mChecks++; \
  if (!(Expression)) { \
    fprintf (stderr, "FAIL line %d: %s\n", __LINE__, #Expression); \
    exit (1); \
  } \
} while (0)

typedef struct {
  UINT8   *Data;
  UINTN   Size;
} BLOB;

typedef struct {
  EVP_PKEY  *Key;
  X509      *X509;
  BLOB      Der;
} IDENTITY;

STATIC UINTN mChecks;
STATIC BLOB mDb;
STATIC BLOB mDbx;
STATIC EFI_STATUS mReadError;
STATIC UINTN mImageReadinessCalls;
STATIC EFI_STATUS mFastbootStatus;
STATIC BOOLEAN mFastbootEnabled = TRUE;
STATIC BOOLEAN mRngAvailable = TRUE;
STATIC BOOLEAN mRngFailure;
STATIC BOOLEAN mEmptyPresent;
STATIC UINT8 mRngCounter;
STATIC EFI_RUNTIME_SERVICES mRuntime;
STATIC EFI_BOOT_SERVICES mBoot;
EFI_RUNTIME_SERVICES *gRT = &mRuntime;
EFI_BOOT_SERVICES *gBS = &mBoot;
EFI_GUID gEfiImageSecurityDatabaseGuid = EFI_IMAGE_SECURITY_DATABASE_GUID;
EFI_GUID gEfiRngProtocolGuid = EFI_RNG_PROTOCOL_GUID;

VOID *EFIAPI AllocatePool (UINTN Size) { return malloc (Size); }
VOID EFIAPI FreePool (VOID *Buffer) { free (Buffer); }
VOID *EFIAPI CopyMem (VOID *Dst, CONST VOID *Src, UINTN Size) { return memcpy (Dst, Src, Size); }
VOID *EFIAPI ZeroMem (VOID *Dst, UINTN Size) { return memset (Dst, 0, Size); }
INTN EFIAPI CompareMem (CONST VOID *A, CONST VOID *B, UINTN Size) { return memcmp (A, B, Size); }
BOOLEAN EFIAPI CompareGuid (CONST GUID *A, CONST GUID *B) { return (BOOLEAN)(memcmp (A, B, sizeof (*A)) == 0); }
UINT32 EFIAPI WriteUnaligned32 (UINT32 *P, UINT32 V) { memcpy (P, &V, sizeof (V)); return V; }
UINT64 EFIAPI WriteUnaligned64 (UINT64 *P, UINT64 V) { memcpy (P, &V, sizeof (V)); return V; }
VOID FortifyFail (CONST CHAR8 *Name, CONST UINTN Line) { fprintf (stderr, "fortify %s:%llu\n", Name, Line); abort (); }
EFI_STATUS BdsSecurityReady (VOID) { mImageReadinessCalls++; return EFI_WRITE_PROTECTED; }
EFI_STATUS BdsSecurityGetFastbootEnabled (BOOLEAN *Enabled) { *Enabled = mFastbootEnabled; return mFastbootStatus; }

STATIC EFI_STATUS EFIAPI
GetVariable (
  CHAR16 *Name,
  EFI_GUID *Guid,
  UINT32 *Attributes,
  UINTN *Size,
  VOID *Data
  )
{
  BLOB *Blob;

  (void)Attributes;
  CHECK (CompareGuid (Guid, &gEfiImageSecurityDatabaseGuid));
  if (EFI_ERROR (mReadError)) {
    return mReadError;
  }

  Blob = Name[2] == 'x' ? &mDbx : &mDb;
  if (Blob->Size == 0) {
    *Size = 0;
    return mEmptyPresent ? EFI_SUCCESS : EFI_NOT_FOUND;
  }

  if (*Size < Blob->Size) {
    *Size = Blob->Size;
    return EFI_BUFFER_TOO_SMALL;
  }

  memcpy (Data, Blob->Data, Blob->Size);
  *Size = Blob->Size;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
GetRng (EFI_RNG_PROTOCOL *This, EFI_RNG_ALGORITHM *Algorithm, UINTN Size, UINT8 *Data)
{
  UINTN Index;

  (void)This;
  (void)Algorithm;
  if (mRngFailure) {
    return EFI_DEVICE_ERROR;
  }

  mRngCounter++;
  for (Index = 0; Index < Size; Index++) {
    Data[Index] = (UINT8)(mRngCounter + Index);
  }

  return EFI_SUCCESS;
}

STATIC EFI_RNG_PROTOCOL mRng = { NULL, GetRng };

STATIC EFI_STATUS EFIAPI
LocateProtocol (EFI_GUID *Guid, VOID *Registration, VOID **Interface)
{
  (void)Registration;
  if (!mRngAvailable || !CompareGuid (Guid, &gEfiRngProtocolGuid)) {
    return EFI_NOT_FOUND;
  }

  *Interface = &mRng;
  return EFI_SUCCESS;
}

STATIC VOID
FreeBlob (BLOB *Blob)
{
  free (Blob->Data);
  memset (Blob, 0, sizeof (*Blob));
}

STATIC BLOB
MakeList (EFI_GUID Type, CONST VOID *Data, UINTN Size)
{
  BLOB Result;
  EFI_SIGNATURE_LIST List;

  Result.Size = sizeof (List) + sizeof (EFI_GUID) + Size;
  Result.Data = calloc (1, Result.Size);
  CHECK (Result.Data != NULL);
  memset (&List, 0, sizeof (List));
  List.SignatureType = Type;
  List.SignatureListSize = (UINT32)Result.Size;
  List.SignatureSize = (UINT32)(sizeof (EFI_GUID) + Size);
  memcpy (Result.Data, &List, sizeof (List));
  memcpy (Result.Data + sizeof (List) + sizeof (EFI_GUID), Data, Size);
  return Result;
}

STATIC VOID
AppendList (BLOB *Database, BLOB Entry)
{
  UINT8 *Data;

  Data = realloc (Database->Data, Database->Size + Entry.Size);
  CHECK (Data != NULL);
  Database->Data = Data;
  memcpy (Data + Database->Size, Entry.Data, Entry.Size);
  Database->Size += Entry.Size;
  FreeBlob (&Entry);
}

STATIC VOID
AddCertificate (BLOB *Database, IDENTITY *Identity)
{
  EFI_GUID Type = EFI_CERT_X509_GUID;
  AppendList (Database, MakeList (Type, Identity->Der.Data, Identity->Der.Size));
}

STATIC VOID
AddTbsRevocation (IDENTITY *Identity, UINTN DigestSize)
{
  EFI_GUID Type256 = EFI_CERT_X509_SHA256_GUID;
  EFI_GUID Type384 = EFI_CERT_X509_SHA384_GUID;
  EFI_GUID Type512 = EFI_CERT_X509_SHA512_GUID;
  EFI_GUID Type;
  UINT8 *Tbs;
  UINTN TbsSize;
  UINT8 Entry[SHA512_DIGEST_SIZE + sizeof (EFI_TIME)];

  // Deliberately nonzero/future revocation time: no timestamp exception.
  memset (Entry, 0, sizeof (Entry));
  Entry[DigestSize] = 0xFE;
  Entry[DigestSize + 1] = 0x0F;
  CHECK (X509GetTBSCert (Identity->Der.Data, Identity->Der.Size, &Tbs, &TbsSize));
  if (DigestSize == SHA256_DIGEST_SIZE) {
    Type = Type256;
    CHECK (Sha256HashAll (Tbs, TbsSize, Entry));
  } else if (DigestSize == SHA384_DIGEST_SIZE) {
    Type = Type384;
    CHECK (Sha384HashAll (Tbs, TbsSize, Entry));
  } else {
    Type = Type512;
    CHECK (Sha512HashAll (Tbs, TbsSize, Entry));
  }

  AppendList (&mDbx, MakeList (Type, Entry, DigestSize + sizeof (EFI_TIME)));
}

STATIC IDENTITY
MakeIdentity (CONST CHAR8 *Name, IDENTITY *Issuer, BOOLEAN Ec, BOOLEAN Ca)
{
  IDENTITY Result;
  EVP_PKEY_CTX *Context;
  X509_NAME *Subject;
  X509V3_CTX Extensions;
  X509_EXTENSION *Extension;
  UINT8 *Cursor;
  STATIC long Serial = 1;

  memset (&Result, 0, sizeof (Result));
  Context = EVP_PKEY_CTX_new_id (Ec ? EVP_PKEY_EC : EVP_PKEY_RSA, NULL);
  CHECK (Context != NULL);
  CHECK (EVP_PKEY_keygen_init (Context) > 0);
  CHECK ((Ec ? EVP_PKEY_CTX_set_ec_paramgen_curve_nid (Context, NID_X9_62_prime256v1) :
          EVP_PKEY_CTX_set_rsa_keygen_bits (Context, 2048)) > 0);
  CHECK (EVP_PKEY_keygen (Context, &Result.Key) > 0);
  EVP_PKEY_CTX_free (Context);
  Result.X509 = X509_new ();
  CHECK (Result.X509 != NULL);
  CHECK (X509_set_version (Result.X509, 2));
  CHECK (ASN1_INTEGER_set (X509_get_serialNumber (Result.X509), Serial++));
  CHECK (X509_gmtime_adj (X509_getm_notBefore (Result.X509), -60) != NULL);
  CHECK (X509_gmtime_adj (X509_getm_notAfter (Result.X509), 86400) != NULL);
  CHECK (X509_set_pubkey (Result.X509, Result.Key));
  Subject = X509_get_subject_name (Result.X509);
  CHECK (X509_NAME_add_entry_by_txt (Subject, "CN", MBSTRING_ASC,
          (CONST unsigned char *)Name, -1, -1, 0));
  CHECK (X509_set_issuer_name (Result.X509, Issuer ? X509_get_subject_name (Issuer->X509) : Subject));
  X509V3_set_ctx (&Extensions, Issuer ? Issuer->X509 : Result.X509, Result.X509, NULL, NULL, 0);
  Extension = X509V3_EXT_conf_nid (NULL, &Extensions, NID_basic_constraints,
                                 Ca ? "critical,CA:TRUE" : "critical,CA:FALSE");
  CHECK (Extension != NULL);
  CHECK (X509_add_ext (Result.X509, Extension, -1));
  X509_EXTENSION_free (Extension);
  CHECK (X509_sign (Result.X509, Issuer ? Issuer->Key : Result.Key, EVP_sha256 ()) > 0);
  Result.Der.Size = (UINTN)i2d_X509 (Result.X509, NULL);
  Result.Der.Data = malloc (Result.Der.Size);
  CHECK (Result.Der.Data != NULL);
  Cursor = Result.Der.Data;
  CHECK (i2d_X509 (Result.X509, &Cursor) == (int)Result.Der.Size);
  return Result;
}

STATIC BLOB
SignNonce (IDENTITY *Identity, IDENTITY *Extra, BOOLEAN MultiSigner, BOOLEAN Attached, BOOLEAN Raw)
{
  BLOB Result;
  CONST UINT8 *Nonce;
  BIO *Input;
  PKCS7 *Pkcs7;
  STACK_OF(X509) *Certificates;
  UINT8 *Cursor;
  EVP_MD_CTX *DigestContext;
  size_t Size;
  int Flags;

  memset (&Result, 0, sizeof (Result));
  Nonce = BdsSecurityChallenge ();
  CHECK (Nonce != NULL);
  if (Raw) {
    DigestContext = EVP_MD_CTX_new ();
    CHECK (DigestContext != NULL);
    CHECK (EVP_DigestSignInit (DigestContext, NULL, EVP_sha256 (), NULL, Identity->Key) > 0);
    Size = 0;
    CHECK (EVP_DigestSign (DigestContext, NULL, &Size, Nonce, BDS_SECURITY_CHALLENGE_SIZE) > 0);
    Result.Data = malloc (Size);
    CHECK (Result.Data != NULL);
    CHECK (EVP_DigestSign (DigestContext, Result.Data, &Size, Nonce, BDS_SECURITY_CHALLENGE_SIZE) > 0);
    Result.Size = Size;
    EVP_MD_CTX_free (DigestContext);
    return Result;
  }

  Input = BIO_new_mem_buf (Nonce, BDS_SECURITY_CHALLENGE_SIZE);
  CHECK (Input != NULL);
  Certificates = sk_X509_new_null ();
  CHECK (Certificates != NULL);
  if ((Extra != NULL) && !MultiSigner) {
    CHECK (sk_X509_push (Certificates, Extra->X509));
  }

  Flags = PKCS7_BINARY | (Attached ? 0 : PKCS7_DETACHED);
  if (MultiSigner) {
    Pkcs7 = PKCS7_sign (NULL, NULL, NULL, NULL, Flags | PKCS7_PARTIAL);
    CHECK (Pkcs7 != NULL);
    CHECK (PKCS7_sign_add_signer (Pkcs7, Identity->X509, Identity->Key, EVP_sha256 (), 0) != NULL);
    CHECK (PKCS7_sign_add_signer (Pkcs7, Extra->X509, Extra->Key, EVP_sha256 (), 0) != NULL);
    CHECK (PKCS7_final (Pkcs7, Input, Flags));
  } else {
    Pkcs7 = PKCS7_sign (Identity->X509, Identity->Key, Certificates, Input, Flags);
    CHECK (Pkcs7 != NULL);
  }

  Result.Size = (UINTN)i2d_PKCS7 (Pkcs7, NULL);
  Result.Data = malloc (Result.Size);
  CHECK (Result.Data != NULL);
  Cursor = Result.Data;
  CHECK (i2d_PKCS7 (Pkcs7, &Cursor) == (int)Result.Size);
  BIO_free (Input);
  PKCS7_free (Pkcs7);
  sk_X509_free (Certificates);
  return Result;
}

STATIC VOID
ResetPolicy (IDENTITY *Trust)
{
  FreeBlob (&mDb);
  FreeBlob (&mDbx);
  mReadError = EFI_SUCCESS;
  mFastbootStatus = EFI_SUCCESS;
  mFastbootEnabled = TRUE;
  mEmptyPresent = FALSE;
  mRngAvailable = TRUE;
  mRngFailure = FALSE;
  CHECK (!BdsSecurityAuthEnabled ());
  CHECK (!BdsSecurityLocked ());
  CHECK (!BdsSecurityIsPrivileged ());
  if (Trust != NULL) {
    AddCertificate (&mDb, Trust);
    CHECK (BdsSecurityAuthEnabled ());
    CHECK (BdsSecurityLocked ());
  }
}

STATIC VOID
CheckResponse (BLOB *Response, BOOLEAN Success)
{
  EFI_STATUS Status;
  Status = BdsSecurityVerifyChallenge (Response->Data, Response->Size);
  if (Success && EFI_ERROR (Status)) {
    fprintf (stderr, "Unexpected status: %llx\n", Status);
  }
  CHECK ((BOOLEAN)!EFI_ERROR (Status) == Success);
  CHECK (BdsSecurityIsPrivileged () == Success);
  CHECK (BdsSecurityLocked () == (BOOLEAN)!Success);
}

int main (void)
{
  IDENTITY Root;
  IDENTITY Intermediate;
  IDENTITY Leaf;
  IDENTITY Other;
  IDENTITY Ec;
  BLOB Response;
  BLOB Mutated;
  EFI_SIGNATURE_LIST List;
  EFI_GUID HashType = EFI_CERT_SHA256_GUID;
  EFI_GUID UnknownType = { 0 };
  UINT8 Hash[SHA256_DIGEST_SIZE] = { 1 };
  UINT8 Nonce[BDS_SECURITY_CHALLENGE_SIZE];
  UINTN Index;
  UINTN DigestSizes[] = { SHA256_DIGEST_SIZE, SHA384_DIGEST_SIZE, SHA512_DIGEST_SIZE };

  mRuntime.GetVariable = GetVariable;
  mBoot.LocateProtocol = LocateProtocol;
  Root = MakeIdentity ("root", NULL, FALSE, TRUE);
  Intermediate = MakeIdentity ("intermediate", &Root, FALSE, TRUE);
  Leaf = MakeIdentity ("leaf", &Intermediate, FALSE, FALSE);
  Other = MakeIdentity ("other", NULL, FALSE, TRUE);
  Ec = MakeIdentity ("ec", NULL, TRUE, TRUE);

  ResetPolicy (NULL);
  CHECK (BdsSecurityChallenge () == NULL);
  mEmptyPresent = TRUE;
  CHECK (!BdsSecurityAuthEnabled ());
  CHECK (!BdsSecurityLocked ());
  mReadError = EFI_DEVICE_ERROR;
  CHECK (BdsSecurityAuthEnabled ());
  CHECK (BdsSecurityLocked ());
  CHECK (BdsSecurityChallenge () == NULL);
  ResetPolicy (NULL);
  AppendList (&mDb, MakeList (HashType, Hash, sizeof (Hash)));
  CHECK (BdsSecurityAuthEnabled ());
  CHECK (BdsSecurityLocked ());
  CHECK (BdsSecurityChallenge () == NULL);
  ResetPolicy (NULL);
  AddCertificate (&mDbx, &Root);
  CHECK (BdsSecurityAuthEnabled ());
  CHECK (BdsSecurityLocked ());
  CHECK (BdsSecurityChallenge () == NULL);

  ResetPolicy (&Root);
  mRngAvailable = FALSE;
  CHECK (BdsSecurityChallenge () == NULL);
  mRngAvailable = TRUE;
  mRngFailure = TRUE;
  CHECK (BdsSecurityChallenge () == NULL);
  mRngFailure = FALSE;
  memcpy (Nonce, BdsSecurityChallenge (), sizeof (Nonce));
  CHECK (memcmp (Nonce, BdsSecurityChallenge (), sizeof (Nonce)) == 0);
  Response = SignNonce (&Root, NULL, FALSE, FALSE, FALSE);
  CheckResponse (&Response, TRUE);
  CheckResponse (&Response, FALSE);
  FreeBlob (&Response);

  Response = SignNonce (&Root, NULL, FALSE, FALSE, TRUE);
  CheckResponse (&Response, TRUE);
  CheckResponse (&Response, FALSE);
  FreeBlob (&Response);

  ResetPolicy (&Root);
  Response = SignNonce (&Root, NULL, FALSE, FALSE, FALSE);
  CHECK (BdsSecurityVerifyChallenge (NULL, 0) == EFI_INVALID_PARAMETER);
  CheckResponse (&Response, FALSE);
  FreeBlob (&Response);

  ResetPolicy (&Root);
  Response = SignNonce (&Root, NULL, FALSE, FALSE, FALSE);
  CheckResponse (&Response, TRUE);
  AppendList (&mDbx, MakeList (HashType, Hash, sizeof (Hash)));
  CHECK (!BdsSecurityIsPrivileged ());
  CHECK (BdsSecurityLocked ());
  FreeBlob (&Response);
  Response = SignNonce (&Root, NULL, FALSE, FALSE, FALSE);
  CheckResponse (&Response, TRUE);
  mReadError = EFI_DEVICE_ERROR;
  CHECK (BdsSecurityLocked ());
  mReadError = EFI_SUCCESS;
  CHECK (!BdsSecurityIsPrivileged ());
  FreeBlob (&Response);

  ResetPolicy (&Root);
  Response = SignNonce (&Root, NULL, FALSE, FALSE, FALSE);
  AddCertificate (&mDb, &Other);
  CheckResponse (&Response, FALSE);
  FreeBlob (&Response);

  ResetPolicy (&Root);
  Response = SignNonce (&Root, NULL, FALSE, FALSE, FALSE);
  Response.Data[Response.Size - 1] ^= 1;
  CheckResponse (&Response, FALSE);
  FreeBlob (&Response);
  Response = SignNonce (&Other, NULL, FALSE, FALSE, FALSE);
  CheckResponse (&Response, FALSE);
  FreeBlob (&Response);
  Response = SignNonce (&Root, NULL, FALSE, TRUE, FALSE);
  CheckResponse (&Response, FALSE);
  FreeBlob (&Response);
  Response = SignNonce (&Root, &Other, TRUE, FALSE, FALSE);
  CheckResponse (&Response, FALSE);
  FreeBlob (&Response);
  // Both signatures chain to db, so rejection exercises the one-signer rule.
  Response = SignNonce (&Root, &Intermediate, TRUE, FALSE, FALSE);
  CheckResponse (&Response, FALSE);
  FreeBlob (&Response);

  ResetPolicy (&Root);
  AddCertificate (&mDbx, &Root);
  Response = SignNonce (&Root, NULL, FALSE, FALSE, FALSE);
  CheckResponse (&Response, FALSE);
  FreeBlob (&Response);
  Response = SignNonce (&Root, NULL, FALSE, FALSE, TRUE);
  CheckResponse (&Response, FALSE);
  FreeBlob (&Response);

  for (Index = 0; Index < ARRAY_SIZE (DigestSizes); Index++) {
    ResetPolicy (&Root);
    AddTbsRevocation (&Root, DigestSizes[Index]);
    Response = SignNonce (&Root, NULL, FALSE, FALSE, FALSE);
    CheckResponse (&Response, FALSE);
    FreeBlob (&Response);
    Response = SignNonce (&Root, NULL, FALSE, FALSE, TRUE);
    CheckResponse (&Response, FALSE);
    FreeBlob (&Response);
  }

  ResetPolicy (&Root);
  Response = SignNonce (&Leaf, &Intermediate, FALSE, FALSE, FALSE);
  CheckResponse (&Response, TRUE);
  FreeBlob (&Response);
  AddCertificate (&mDbx, &Intermediate);
  Response = SignNonce (&Leaf, &Intermediate, FALSE, FALSE, FALSE);
  CheckResponse (&Response, FALSE);
  FreeBlob (&Response);
  ResetPolicy (&Root);
  AddTbsRevocation (&Intermediate, SHA384_DIGEST_SIZE);
  Response = SignNonce (&Leaf, &Intermediate, FALSE, FALSE, FALSE);
  CheckResponse (&Response, FALSE);
  FreeBlob (&Response);
  ResetPolicy (&Root);
  AddTbsRevocation (&Root, SHA512_DIGEST_SIZE);
  Response = SignNonce (&Leaf, &Intermediate, FALSE, FALSE, FALSE);
  CheckResponse (&Response, FALSE);
  FreeBlob (&Response);
  ResetPolicy (&Intermediate);
  AddCertificate (&mDbx, &Root);
  Response = SignNonce (&Leaf, &Intermediate, FALSE, FALSE, FALSE);
  CheckResponse (&Response, FALSE);
  FreeBlob (&Response);

  ResetPolicy (&Root);
  AddCertificate (&mDbx, &Other);
  Response = SignNonce (&Root, &Other, FALSE, FALSE, FALSE);
  CheckResponse (&Response, FALSE);
  FreeBlob (&Response);
  ResetPolicy (&Root);
  AddTbsRevocation (&Other, SHA256_DIGEST_SIZE);
  Response = SignNonce (&Root, &Other, FALSE, FALSE, FALSE);
  CheckResponse (&Response, FALSE);
  FreeBlob (&Response);

  ResetPolicy (&Ec);
  Response = SignNonce (&Ec, NULL, FALSE, FALSE, FALSE);
  CheckResponse (&Response, TRUE);
  FreeBlob (&Response);
  Response = SignNonce (&Ec, NULL, FALSE, FALSE, TRUE);
  CheckResponse (&Response, FALSE);
  FreeBlob (&Response);

  ResetPolicy (&Root);
  AppendList (&mDbx, MakeList (UnknownType, Hash, sizeof (Hash)));
  CHECK (BdsSecurityLocked ());
  CHECK (BdsSecurityChallenge () == NULL);
  ResetPolicy (&Root);
  memcpy (&List, mDb.Data, sizeof (List));
  List.SignatureHeaderSize = MAX_UINT32;
  memcpy (mDb.Data, &List, sizeof (List));
  CHECK (BdsSecurityLocked ());
  CHECK (BdsSecurityChallenge () == NULL);
  ResetPolicy (&Root);
  mDb.Data[sizeof (EFI_SIGNATURE_LIST) + sizeof (EFI_GUID)] = 0;
  CHECK (BdsSecurityLocked ());
  CHECK (BdsSecurityChallenge () == NULL);
  ResetPolicy (&Root);
  mDb.Size--;
  CHECK (BdsSecurityLocked ());
  CHECK (BdsSecurityChallenge () == NULL);
  ResetPolicy (&Root);
  Response = SignNonce (&Root, NULL, FALSE, FALSE, FALSE);
  CheckResponse (&Response, TRUE);
  CHECK (BdsSecurityIsPrivileged ());
  CHECK (!BdsSecurityLocked ());
  CHECK (mImageReadinessCalls == 0);
  FreeBlob (&Response);

  ResetPolicy (&Root);
  Response = SignNonce (&Root, NULL, FALSE, FALSE, FALSE);
  mFastbootEnabled = FALSE;
  CHECK (!BdsSecurityAuthEnabled ());
  CHECK (!BdsSecurityLocked ());
  CHECK (!BdsSecurityIsPrivileged ());
  CHECK (BdsSecurityChallenge () == NULL);
  mReadError = EFI_DEVICE_ERROR;
  CHECK (!BdsSecurityAuthEnabled ());
  CHECK (!BdsSecurityLocked ());
  mReadError = EFI_SUCCESS;
  mFastbootEnabled = TRUE;
  CHECK (BdsSecurityLocked ());
  CheckResponse (&Response, FALSE);
  FreeBlob (&Response);
  Response = SignNonce (&Root, NULL, FALSE, FALSE, FALSE);
  CheckResponse (&Response, TRUE);
  mFastbootEnabled = FALSE;
  CHECK (!BdsSecurityLocked ());
  CHECK (!BdsSecurityIsPrivileged ());
  mFastbootEnabled = TRUE;
  CHECK (BdsSecurityLocked ());
  FreeBlob (&Response);

  Response = SignNonce (&Root, NULL, FALSE, FALSE, FALSE);
  CheckResponse (&Response, TRUE);
  mFastbootStatus = EFI_SECURITY_VIOLATION;
  CHECK (BdsSecurityAuthEnabled ());
  CHECK (BdsSecurityLocked ());
  CHECK (!BdsSecurityIsPrivileged ());
  CHECK (BdsSecurityChallenge () == NULL);
  mFastbootEnabled = FALSE;
  CHECK (BdsSecurityLocked ());
  mFastbootStatus = EFI_SUCCESS;
  mFastbootEnabled = TRUE;
  CHECK (BdsSecurityLocked ());
  CheckResponse (&Response, FALSE);
  FreeBlob (&Response);

  // Tiny and malformed responses must never reach edk2's unchecked wrapper.
  ResetPolicy (&Root);
  Response = SignNonce (&Root, NULL, FALSE, FALSE, FALSE);
  for (Index = 0; Index < 20; Index++) {
    CHECK (BdsSecurityChallenge () != NULL);
    CHECK (EFI_ERROR (BdsSecurityVerifyChallenge (Response.Data, Index)));
  }
  Mutated.Data = malloc (Response.Size);
  CHECK (Mutated.Data != NULL);
  Mutated.Size = Response.Size;
  for (Index = 0; Index < 19; Index++) {
    memcpy (Mutated.Data, Response.Data, Response.Size);
    Mutated.Data[Index] ^= 0xFF;
    CHECK (BdsSecurityChallenge () != NULL);
    CHECK (EFI_ERROR (BdsSecurityVerifyChallenge (Mutated.Data, Mutated.Size)));
  }
  FreeBlob (&Mutated);
  FreeBlob (&Response);
  ResetPolicy (NULL);

  FreeBlob (&Root.Der); X509_free (Root.X509); EVP_PKEY_free (Root.Key);
  FreeBlob (&Intermediate.Der); X509_free (Intermediate.X509); EVP_PKEY_free (Intermediate.Key);
  FreeBlob (&Leaf.Der); X509_free (Leaf.X509); EVP_PKEY_free (Leaf.Key);
  FreeBlob (&Other.Der); X509_free (Other.X509); EVP_PKEY_free (Other.Key);
  FreeBlob (&Ec.Der); X509_free (Ec.X509); EVP_PKEY_free (Ec.Key);
  CHECK (mImageReadinessCalls == 0);
  printf ("PASS: %llu assertions with production BdsChallenge.c and real edk2 BaseCryptLib/OpenSSL\n", mChecks);
  return 0;
}
