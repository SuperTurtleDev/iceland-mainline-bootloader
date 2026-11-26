/*
 * Certificate loading for the SFB security policy: X.509 DER files under
 * AUTH_CERTS and BOOT_CERTS in the config container.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/SfbConfigStore.h>
#include <Library/SfbSecurity.h>
#include <Library/UefiLib.h>
#include <Guid/FileInfo.h>
#include <Protocol/SimpleFileSystem.h>

#include <mbedtls/x509_crt.h>

#include "SfbSecurityInternal.h"

STATIC SFB_CERT_POOL  mAuthCerts = { { NULL }, { 0 }, 0, EFI_NOT_READY };
STATIC SFB_CERT_POOL  mBootCerts = { { NULL }, { 0 }, 0, EFI_NOT_READY };

#define SFB_CERT_MAX_BYTES   SIZE_64KB
#define SFB_CERT_INFO_BYTES  (SIZE_OF_EFI_FILE_INFO + 512)

/* TRUE when Name ends with the .cer extension (case-insensitive). */
STATIC
BOOLEAN
CertFileName (
  IN CONST CHAR16  *Name
  )
{
  UINTN  Len    = StrLen (Name);
  UINTN  ExtLen = StrLen (SFB_SECURITY_CERT_EXT);
  UINTN  Index;

  if (Len <= ExtLen) {
    return FALSE;
  }

  for (Index = 0; Index < ExtLen; Index++) {
    CHAR16  Ch = Name[Len - ExtLen + Index];

    if ((Ch >= L'A') && (Ch <= L'Z')) {
      Ch = (CHAR16)(Ch - L'A' + L'a');
    }

    if (Ch != SFB_SECURITY_CERT_EXT[Index]) {
      return FALSE;
    }
  }

  return TRUE;
}

/* Append one certificate to Pool when it parses as X.509 DER. Takes ownership
 * of Der only on success. */
STATIC
EFI_STATUS
CertPoolAppend (
  IN OUT SFB_CERT_POOL  *Pool,
  IN UINT8              *Der,
  IN UINTN              DerSize
  )
{
  mbedtls_x509_crt  Crt;
  INT32             MbedErr;

  if (Pool->Count >= SFB_SECURITY_MAX_CERTS) {
    DEBUG ((EFI_D_ERROR, "SFB-SEC: cert pool full, ignoring extra cert\n"));
    return EFI_OUT_OF_RESOURCES;
  }

  mbedtls_x509_crt_init (&Crt);
  MbedErr = mbedtls_x509_crt_parse_der (&Crt, Der, DerSize);
  mbedtls_x509_crt_free (&Crt);
  if (MbedErr != 0) {
    DEBUG ((
      EFI_D_ERROR,
      "SFB-SEC: skipping unparsable cert (-0x%x)\n",
      -MbedErr
      ));
    return EFI_INVALID_PARAMETER;
  }

  Pool->Der[Pool->Count]     = Der;
  Pool->DerSize[Pool->Count] = DerSize;
  Pool->Count++;
  return EFI_SUCCESS;
}

/* Enumerate DirPath (relative to the container root) and load every .cer file
 * into Pool. A missing directory is an empty pool, not an error. */
STATIC
EFI_STATUS
LoadCertDir (
  IN OUT SFB_CERT_POOL  *Pool,
  IN CONST CHAR16       *DirPath,
  IN EFI_FILE_PROTOCOL  *Root
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *Dir     = NULL;
  UINT8              *InfoBuf = NULL;
  EFI_FILE_INFO      *Info;
  UINTN              InfoSize;
  EFI_FILE_PROTOCOL  *File = NULL;
  UINT8              *Der  = NULL;
  UINTN              DerSize;
  UINTN              ReadSize;
  UINTN              NameChars;
  UINTN              Index;

  Status = Root->Open (Root, &Dir, (CHAR16 *)DirPath, EFI_FILE_MODE_READ, 0);
  if (Status == EFI_NOT_FOUND) {
    return EFI_SUCCESS;
  }

  if (EFI_ERROR (Status) || (Dir == NULL)) {
    return EFI_SECURITY_VIOLATION;
  }

  InfoBuf = AllocatePool (SFB_CERT_INFO_BYTES);
  if (InfoBuf == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto out;
  }

  Status = Dir->SetPosition (Dir, 0);
  if (EFI_ERROR (Status)) {
    goto out;
  }

  while (TRUE) {
    InfoSize = SFB_CERT_INFO_BYTES;
    Status   = Dir->Read (Dir, &InfoSize, InfoBuf);
    if (EFI_ERROR (Status) || (InfoSize == 0)) {
      break;
    }

    Info = (EFI_FILE_INFO *)InfoBuf;
    if ((InfoSize < SIZE_OF_EFI_FILE_INFO + sizeof (CHAR16)) ||
        (InfoSize > SFB_CERT_INFO_BYTES) || (Info->Size != InfoSize))
    {
      Status = EFI_VOLUME_CORRUPTED;
      break;
    }

    NameChars = (InfoSize - SIZE_OF_EFI_FILE_INFO) / sizeof (CHAR16);
    for (Index = 0; Index < NameChars && Info->FileName[Index] != 0; Index++) {
    }

    if (Index == NameChars) {
      Status = EFI_VOLUME_CORRUPTED;
      break;
    }

    if (((Info->Attribute & EFI_FILE_DIRECTORY) != 0) ||
        !CertFileName (Info->FileName))
    {
      continue;
    }

    /* Preserve the original maximum-key policy, but never partially read a
     * key and interpret the failure as an empty certificate class. */
    if (Pool->Count == SFB_SECURITY_MAX_CERTS) {
      DEBUG ((EFI_D_WARN, "SFB-SEC: extra certificate ignored\n"));
      continue;
    }

    if ((Info->FileSize == 0) || (Info->FileSize > SFB_CERT_MAX_BYTES)) {
      Status = EFI_SECURITY_VIOLATION;
      break;
    }

    Status = Dir->Open (Dir, &File, Info->FileName, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR (Status) || (File == NULL)) {
      Status = EFI_SECURITY_VIOLATION;
      break;
    }

    DerSize = (UINTN)Info->FileSize;
    Der     = AllocatePool (DerSize);
    if (Der == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      break;
    }

    ReadSize = DerSize;
    Status   = File->Read (File, &ReadSize, Der);
    if (EFI_ERROR (Status) || (ReadSize != DerSize)) {
      Status = EFI_SECURITY_VIOLATION;
      break;
    }

    Status = CertPoolAppend (Pool, Der, DerSize);
    if (EFI_ERROR (Status)) {
      break;
    }

    Der = NULL;                            /* owned by the pool */
    File->Close (File);
    File = NULL;
  }

out:
  if (Der != NULL) {
    FreePool (Der);
  }

  if (File != NULL) {
    File->Close (File);
  }

  if (InfoBuf != NULL) {
    FreePool (InfoBuf);
  }

  Dir->Close (Dir);
  return Status;
}

VOID
SfbCertsReset (
  VOID
  )
{
  UINTN  Index;

  for (Index = 0; Index < mAuthCerts.Count; Index++) {
    FreePool (mAuthCerts.Der[Index]);
  }

  for (Index = 0; Index < mBootCerts.Count; Index++) {
    FreePool (mBootCerts.Der[Index]);
  }

  ZeroMem (&mAuthCerts, sizeof (mAuthCerts));
  ZeroMem (&mBootCerts, sizeof (mBootCerts));
  mAuthCerts.Status = EFI_NOT_READY;
  mBootCerts.Status = EFI_NOT_READY;
}

VOID
SfbSecurityLoadCerts (
  VOID
  )
{
  EFI_STATUS                       Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs   = NULL;
  EFI_FILE_PROTOCOL                *Root = NULL;

  /* Reload also revokes any authorization granted under the old key set. */
  SfbSecurityReset ();

  Status = SfbConfigStoreGetFs (&Fs, NULL);
  if (Status == EFI_NOT_FOUND) {
    /* An absent optional efisp container is the unprovisioned policy. */
    mAuthCerts.Status = EFI_SUCCESS;
    mBootCerts.Status = EFI_SUCCESS;
    return;
  }

  if (EFI_ERROR (Status) || (Fs == NULL)) {
    DEBUG ((EFI_D_ERROR, "SFB-SEC: config store unavailable; policy locked\n"));
    return;
  }

  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status) || (Root == NULL)) {
    DEBUG ((EFI_D_ERROR, "SFB-SEC: cannot open container root; policy locked\n"));
    return;
  }

  mAuthCerts.Status = LoadCertDir (&mAuthCerts, SFB_SECURITY_AUTH_DIR, Root);
  mBootCerts.Status = LoadCertDir (&mBootCerts, SFB_SECURITY_BOOT_DIR, Root);
  Root->Close (Root);

  DEBUG ((
    EFI_D_INFO,
    "SFB-SEC: %u auth certs (%r), %u boot certs (%r)\n",
    (UINT32)mAuthCerts.Count,
    mAuthCerts.Status,
    (UINT32)mBootCerts.Count,
    mBootCerts.Status
    ));
}

/* ---- queries used by this library and the policy checks ------------------ */

CONST SFB_CERT_POOL *
SfbCertsAuthPool (
  VOID
  )
{
  return &mAuthCerts;
}

CONST SFB_CERT_POOL *
SfbCertsBootPool (
  VOID
  )
{
  return &mBootCerts;
}

BOOLEAN
SfbSecurityAuthEnabled (
  VOID
  )
{
  return (BOOLEAN)(EFI_ERROR (mAuthCerts.Status) || mAuthCerts.Count > 0);
}

BOOLEAN
SfbSecurityVerifyEnabled (
  VOID
  )
{
  return (BOOLEAN)(EFI_ERROR (mBootCerts.Status) || mBootCerts.Count > 0);
}
