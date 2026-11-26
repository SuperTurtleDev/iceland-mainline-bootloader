/*
 * Authenticode SignedData verification for pinned BOOT_CERTS public keys.
 *
 * The upstream mbedtls PKCS#7 helper only accepts detached id-data without
 * signed attributes. Authenticode carries SpcIndirectDataContent and signs
 * its authenticated attributes. Parse that envelope with bounded DER views,
 * then use mbedtls for SHA-256, X.509 parsing, and RSA/ECDSA verification.
 * Embedded certificates and timestamps cannot introduce new trust anchors.
 *
 * Copyright (c) 2026, MainlineBootPkg contributors.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>

#include "SfbSecurityInternal.h"

#define DER_SEQUENCE  0x30
#define DER_SET       0x31
#define DER_OID       0x06
#define DER_INTEGER   0x02
#define DER_OCTETS    0x04
#define DER_CONTEXT0  0xa0
#define DER_CONTEXT1  0xa1

#define OID_SIGNED_DATA   "\x2a\x86\x48\x86\xf7\x0d\x01\x07\x02"
#define OID_SPC_DATA      "\x2b\x06\x01\x04\x01\x82\x37\x02\x01\x04"
#define OID_CONTENT_TYPE  "\x2a\x86\x48\x86\xf7\x0d\x01\x09\x03"
#define OID_MESSAGE_HASH  "\x2a\x86\x48\x86\xf7\x0d\x01\x09\x04"
#define OID_SHA256        "\x60\x86\x48\x01\x65\x03\x04\x02\x01"
#define OID_RSA           "\x2a\x86\x48\x86\xf7\x0d\x01\x01\x01"
#define OID_RSA_SHA256    "\x2a\x86\x48\x86\xf7\x0d\x01\x01\x0b"
#define OID_ECDSA_SHA256  "\x2a\x86\x48\xce\x3d\x04\x03\x02"
#define OID_MATCH(View, Literal) \
  ((View).Size == sizeof (Literal) - 1 && \
   CompareMem ((View).Data, Literal, sizeof (Literal) - 1) == 0)

typedef struct {
  CONST UINT8    *Data;
  UINTN          Size;
} SFB_DER;

/* Consume one DER value. Reject indefinite and non-minimal lengths; no
 * pointer arithmetic is performed until the complete range is bounded. */
STATIC
BOOLEAN
DerTake (
  IN OUT SFB_DER  *Input,
  IN UINT8        Tag,
  OUT SFB_DER     *Value
  )
{
  UINTN  Header = 2;
  UINTN  Length;
  UINTN  Bytes;
  UINTN  Index;

  if ((Input->Size < 2) || (Input->Data[0] != Tag)) {
    return FALSE;
  }

  Length = Input->Data[1];
  if ((Length & 0x80) != 0) {
    Bytes = Length & 0x7f;
    if ((Bytes == 0) || (Bytes > sizeof (UINTN)) || (Bytes > Input->Size - 2) ||
        (Input->Data[2] == 0))
    {
      return FALSE;
    }

    Length = 0;
    for (Index = 0; Index < Bytes; Index++) {
      Length = (Length << 8) | Input->Data[2 + Index];
    }

    if (Length < 128) {
      return FALSE;
    }

    Header += Bytes;
  }

  if (Length > Input->Size - Header) {
    return FALSE;
  }

  Value->Data  = Input->Data + Header;
  Value->Size  = Length;
  Input->Data += Header + Length;
  Input->Size -= Header + Length;
  return TRUE;
}

STATIC
BOOLEAN
DerVersionOne (
  IN OUT SFB_DER  *Input
  )
{
  SFB_DER  Value;

  return (BOOLEAN)(DerTake (Input, DER_INTEGER, &Value) &&
                   Value.Size == 1 && Value.Data[0] == 1);
}

STATIC
BOOLEAN
DerAlgorithm (
  IN OUT SFB_DER  *Input,
  OUT SFB_DER     *Oid
  )
{
  SFB_DER  Sequence;
  SFB_DER  Null;

  if (!DerTake (Input, DER_SEQUENCE, &Sequence) ||
      !DerTake (&Sequence, DER_OID, Oid))
  {
    return FALSE;
  }

  if ((Sequence.Size != 0) && (!DerTake (&Sequence, 0x05, &Null) || (Null.Size != 0))) {
    return FALSE;
  }

  return (BOOLEAN)(Sequence.Size == 0);
}

/* Extract and check the image digest, retaining the SEQUENCE contents as
 * the PKCS#7 signed content (Authenticode excludes the SEQUENCE header). */
STATIC
BOOLEAN
SpcContent (
  IN OUT SFB_DER  *SignedData,
  IN CONST UINT8  ImageHash[32],
  OUT SFB_DER     *Content
  )
{
  SFB_DER  Info;
  SFB_DER  Oid;
  SFB_DER  Explicit;
  SFB_DER  Fields;
  SFB_DER  Data;
  SFB_DER  Digest;
  SFB_DER  Hash;

  if (!DerTake (SignedData, DER_SEQUENCE, &Info) ||
      !DerTake (&Info, DER_OID, &Oid) || !OID_MATCH (Oid, OID_SPC_DATA) ||
      !DerTake (&Info, DER_CONTEXT0, &Explicit) || (Info.Size != 0) ||
      !DerTake (&Explicit, DER_SEQUENCE, Content) || (Explicit.Size != 0))
  {
    return FALSE;
  }

  Fields = *Content;
  if (!DerTake (&Fields, DER_SEQUENCE, &Data) ||
      !DerTake (&Fields, DER_SEQUENCE, &Digest) || (Fields.Size != 0) ||
      !DerAlgorithm (&Digest, &Oid) || !OID_MATCH (Oid, OID_SHA256) ||
      !DerTake (&Digest, DER_OCTETS, &Hash) || (Digest.Size != 0) || (Hash.Size != 32))
  {
    return FALSE;
  }

  return (BOOLEAN)(CompareMem (Hash.Data, ImageHash, 32) == 0);
}

STATIC
BOOLEAN
SignedAttributes (
  IN SFB_DER        Attributes,
  IN CONST SFB_DER  *Content
  )
{
  UINT8    Hash[32];
  BOOLEAN  SeenType = FALSE;
  BOOLEAN  SeenHash = FALSE;

  if (mbedtls_md (
        mbedtls_md_info_from_type (MBEDTLS_MD_SHA256),
        Content->Data,
        Content->Size,
        Hash
        ) != 0)
  {
    return FALSE;
  }

  while (Attributes.Size != 0) {
    SFB_DER  Attribute;
    SFB_DER  Oid;
    SFB_DER  Values;
    SFB_DER  Value;

    if (!DerTake (&Attributes, DER_SEQUENCE, &Attribute) ||
        !DerTake (&Attribute, DER_OID, &Oid) ||
        !DerTake (&Attribute, DER_SET, &Values) || (Attribute.Size != 0))
    {
      return FALSE;
    }

    if (OID_MATCH (Oid, OID_CONTENT_TYPE)) {
      if (SeenType || !DerTake (&Values, DER_OID, &Value) || (Values.Size != 0) ||
          !OID_MATCH (Value, OID_SPC_DATA))
      {
        return FALSE;
      }

      SeenType = TRUE;
    } else if (OID_MATCH (Oid, OID_MESSAGE_HASH)) {
      if (SeenHash || !DerTake (&Values, DER_OCTETS, &Value) || (Values.Size != 0) ||
          (Value.Size != sizeof (Hash)) || (CompareMem (Value.Data, Hash, sizeof (Hash)) != 0))
      {
        return FALSE;
      }

      SeenHash = TRUE;
    }
  }

  return (BOOLEAN)(SeenType && SeenHash);
}

STATIC
BOOLEAN
VerifySigner (
  IN SFB_DER           Signer,
  IN CONST SFB_DER     *Content,
  IN mbedtls_x509_crt  *Cert
  )
{
  SFB_DER               Issuer;
  SFB_DER               Oid;
  SFB_DER               Attributes;
  SFB_DER               Signature;
  SFB_DER               Ignored;
  CONST UINT8           *Encoded;
  UINTN                 EncodedSize;
  UINT8                 Hash[32];
  UINT8                 SetTag = DER_SET;
  BOOLEAN               Ok;
  mbedtls_md_context_t  Md;

  if (!DerVersionOne (&Signer) || !DerTake (&Signer, DER_SEQUENCE, &Issuer) ||
      !DerAlgorithm (&Signer, &Oid) || !OID_MATCH (Oid, OID_SHA256))
  {
    return FALSE;
  }

  Encoded     = Signer.Data;
  EncodedSize = Signer.Size;
  if (!DerTake (&Signer, DER_CONTEXT0, &Attributes) ||
      !SignedAttributes (Attributes, Content))
  {
    return FALSE;
  }

  EncodedSize -= Signer.Size;
  if (!DerAlgorithm (&Signer, &Oid)) {
    return FALSE;
  }

  if (OID_MATCH (Oid, OID_RSA) || OID_MATCH (Oid, OID_RSA_SHA256)) {
    if (!mbedtls_pk_can_do (&Cert->pk, MBEDTLS_PK_RSA)) {
      return FALSE;
    }
  } else if (OID_MATCH (Oid, OID_ECDSA_SHA256)) {
    if (!mbedtls_pk_can_do (&Cert->pk, MBEDTLS_PK_ECDSA)) {
      return FALSE;
    }
  } else {
    return FALSE;
  }

  if (!DerTake (&Signer, DER_OCTETS, &Signature) || (Signature.Size == 0)) {
    return FALSE;
  }

  /* Countersignatures/timestamps do not extend the pinned-key policy. */
  if ((Signer.Size != 0) && !DerTake (&Signer, DER_CONTEXT1, &Ignored)) {
    return FALSE;
  }

  if (Signer.Size != 0) {
    return FALSE;
  }

  mbedtls_md_init (&Md);
  Ok = (BOOLEAN)(mbedtls_md_setup (&Md, mbedtls_md_info_from_type (MBEDTLS_MD_SHA256), 0) == 0 &&
                 mbedtls_md_starts (&Md) == 0 &&
                 mbedtls_md_update (&Md, &SetTag, 1) == 0 &&
                 mbedtls_md_update (&Md, Encoded + 1, EncodedSize - 1) == 0 &&
                 mbedtls_md_finish (&Md, Hash) == 0);
  mbedtls_md_free (&Md);
  return (BOOLEAN)(Ok && mbedtls_pk_verify (
                           &Cert->pk,
                           MBEDTLS_MD_SHA256,
                           Hash,
                           sizeof (Hash),
                           Signature.Data,
                           Signature.Size
                           ) == 0);
}

BOOLEAN
SfbAuthenticodeVerify (
  IN CONST UINT8  *Data,
  IN UINTN        DataSize,
  IN CONST UINT8  *Cert,
  IN UINTN        CertSize,
  IN CONST UINT8  ImageHash[32]
  )
{
  SFB_DER           Input = { Data, DataSize };
  SFB_DER           Outer;
  SFB_DER           Oid;
  SFB_DER           Explicit;
  SFB_DER           SignedData;
  SFB_DER           Algorithms;
  SFB_DER           Content;
  SFB_DER           Ignored;
  SFB_DER           Signers;
  SFB_DER           Signer;
  mbedtls_x509_crt  Trusted;
  BOOLEAN           Verified = FALSE;

  if ((Data == NULL) || (Cert == NULL) || (ImageHash == NULL) ||
      !DerTake (&Input, DER_SEQUENCE, &Outer))
  {
    return FALSE;
  }

  /* WIN_CERTIFICATE may include up to seven zero alignment bytes. */
  if (Input.Size > 7) {
    return FALSE;
  }

  while (Input.Size != 0) {
    if (*Input.Data++ != 0) {
      return FALSE;
    }

    Input.Size--;
  }

  if (!DerTake (&Outer, DER_OID, &Oid) || !OID_MATCH (Oid, OID_SIGNED_DATA) ||
      !DerTake (&Outer, DER_CONTEXT0, &Explicit) || (Outer.Size != 0) ||
      !DerTake (&Explicit, DER_SEQUENCE, &SignedData) || (Explicit.Size != 0) ||
      !DerVersionOne (&SignedData) || !DerTake (&SignedData, DER_SET, &Algorithms) ||
      !DerAlgorithm (&Algorithms, &Oid) || !OID_MATCH (Oid, OID_SHA256) ||
      (Algorithms.Size != 0) || !SpcContent (&SignedData, ImageHash, &Content))
  {
    return FALSE;
  }

  if ((SignedData.Size != 0) && (SignedData.Data[0] == DER_CONTEXT0) &&
      !DerTake (&SignedData, DER_CONTEXT0, &Ignored))
  {
    return FALSE;
  }

  if ((SignedData.Size != 0) && (SignedData.Data[0] == DER_CONTEXT1) &&
      !DerTake (&SignedData, DER_CONTEXT1, &Ignored))
  {
    return FALSE;
  }

  if (!DerTake (&SignedData, DER_SET, &Signers) || (SignedData.Size != 0)) {
    return FALSE;
  }

  mbedtls_x509_crt_init (&Trusted);
  if (mbedtls_x509_crt_parse_der (&Trusted, Cert, CertSize) != 0) {
    goto out;
  }

  while (Signers.Size != 0) {
    if (!DerTake (&Signers, DER_SEQUENCE, &Signer)) {
      Verified = FALSE;
      break;
    }

    if (VerifySigner (Signer, &Content, &Trusted)) {
      Verified = TRUE;
    }
  }

out:
  mbedtls_x509_crt_free (&Trusted);
  return Verified;
}
