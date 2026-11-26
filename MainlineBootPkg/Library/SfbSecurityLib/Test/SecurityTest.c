/* Regression vectors use the actual parser, crypto, and challenge state.
 * Copyright (c) 2026, MainlineBootPkg contributors.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "Host.h"
#include "../SfbSecurityInternal.h"
#include <mbedtls/md.h>

/* Include the translation unit to inspect its private parsing/hash helpers. */
#include "../SfbPeVerify.c"
#include "../SfbCerts.c"

#define AuthPool  mAuthCerts
#define BootPool  mBootCerts
static EFI_STATUS         StoreStatus = EFI_NOT_FOUND;
static int                FileMode;
static int                DirectoryRead;
static UINT8              *FileData;
static size_t             FileSize;
static EFI_FILE_PROTOCOL  RootFile, Directory, CertFile;
static EFI_STATUS
CloseFile (
  EFI_FILE_PROTOCOL  *file
  )
{
  (void)file;
  return EFI_SUCCESS;
}

static EFI_STATUS
OpenFile (
  EFI_FILE_PROTOCOL  *file,
  EFI_FILE_PROTOCOL  **out,
  CHAR16             *path,
  UINT64             mode,
  UINT64             attr
  )
{
  (void)path;
  (void)mode;
  (void)attr;
  if (file == &RootFile) {
    if (FileMode == 0) {
      return EFI_NOT_FOUND;
    }

    if (FileMode == 1) {
      return EFI_DEVICE_ERROR;
    }

    DirectoryRead = 0;
    *out          = &Directory;
    return EFI_SUCCESS;
  }

  *out = &CertFile;
  return EFI_SUCCESS;
}

static EFI_STATUS
SetPosition (
  EFI_FILE_PROTOCOL  *file,
  UINT64             position
  )
{
  (void)file;
  (void)position;
  DirectoryRead = 0;
  return EFI_SUCCESS;
}

static EFI_STATUS
ReadFile (
  EFI_FILE_PROTOCOL  *file,
  UINTN              *size,
  VOID               *buffer
  )
{
  if (file == &Directory) {
    if (FileMode == 2) {
      return EFI_DEVICE_ERROR;
    }

    if (DirectoryRead++) {
      *size = 0;
      return EFI_SUCCESS;
    }

    EFI_FILE_INFO  *info = buffer;
    *size = SIZE_OF_EFI_FILE_INFO + 16;
    memset (buffer, 0, *size);
    info->Size     = *size;
    info->FileSize = FileSize;
    const CHAR16  name[] = L"key.cer";
    memcpy (info->FileName, name, sizeof (name));
    return EFI_SUCCESS;
  }

  if (FileMode == 4) {
    *size = 0;
    return EFI_SUCCESS;
  }

  memcpy (buffer, FileData, FileSize);
  *size = FileSize;
  return EFI_SUCCESS;
}

static EFI_STATUS
OpenVolume (
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *fs,
  EFI_FILE_PROTOCOL                **out
  )
{
  (void)fs;
  if (FileMode == 5) {
    return EFI_DEVICE_ERROR;
  }

  *out = &RootFile;
  return EFI_SUCCESS;
}

static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  FileSystem = { OpenVolume };
EFI_STATUS
SfbConfigStoreGetFs (
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  **fs,
  EFI_HANDLE                       *volume
  )
{
  (void)volume;
  *fs = StoreStatus == EFI_SUCCESS ? &FileSystem : NULL;
  return StoreStatus;
}

static int    RngMode;
static UINT8  RngByte = 1;
EFI_GUID      gEfiRngProtocolGuid;

static EFI_STATUS
GetRng (
  EFI_RNG_PROTOCOL  *self,
  EFI_GUID          *algo,
  UINTN             size,
  UINT8             *out
  )
{
  (void)self;
  (void)algo;
  memset (out, RngByte++, size);
  return RngMode == 2 ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

static EFI_RNG_PROTOCOL  Rng = { GetRng };
static EFI_STATUS
Locate (
  EFI_GUID  *guid,
  VOID      *registration,
  VOID      **out
  )
{
  (void)guid;
  (void)registration;
  *out = RngMode == 0 ? NULL : &Rng;
  return RngMode == 0 ? EFI_NOT_FOUND : EFI_SUCCESS;
}

static EFI_BOOT_SERVICES  Services = { Locate };
EFI_BOOT_SERVICES         *gBS     = &Services;

void *
mbedtls_calloc (
  size_t  n,
  size_t  s
  )
{
  return calloc (n, s);
}

void
mbedtls_free (
  void  *p
  )
{
  free (p);
}

int
my_snprintf (
  char        *out,
  size_t      size,
  const char  *format,
  ...
  )
{
  va_list  args;

  va_start (args, format);
  int  result = vsnprintf (out, size, format, args);

  va_end (args);
  return result;
}

static int  Failures;
#define CHECK(Test)  do { if (!(Test)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #Test); Failures++; } } while (0)

static UINT8 *
Load (
  const char  *path,
  size_t      *size
  )
{
  FILE  *file = fopen (path, "rb");

  if (!file || fseek (file, 0, SEEK_END)) {
    exit (2);
  }

  long  length = ftell (file);

  if ((length < 0) || fseek (file, 0, SEEK_SET)) {
    exit (2);
  }

  UINT8  *data = malloc ((size_t)length + 1);

  if (!data || (fread (data, 1, (size_t)length, file) != (size_t)length)) {
    exit (2);
  }

  fclose (file);
  *size = (size_t)length;
  return data;
}

static void
Put16 (
  UINT8   *p,
  UINT16  value
  )
{
  memcpy (p, &value, 2);
}

static void
Put32 (
  UINT8   *p,
  UINT32  value
  )
{
  memcpy (p, &value, 4);
}

int
main (
  int   argc,
  char  **argv
  )
{
  if (argc != 6) {
    return 2;
  }

  size_t   imageSize, certSize, wrongSize, expectedSize, responseSize;
  UINT8    *image    = Load (argv[1], &imageSize);
  UINT8    *cert     = Load (argv[2], &certSize);
  UINT8    *wrong    = Load (argv[3], &wrongSize);
  UINT8    *expected = Load (argv[4], &expectedSize);
  UINT8    *response = Load (argv[5], &responseSize);
  PE_INFO  info;
  UINT8    hash[32];
  UINT8    *copy = malloc (imageSize + 16);

  CHECK (copy != NULL);
  if (!copy) {
    return 2;
  }

  AuthPool.Status     = EFI_SUCCESS;
  BootPool.Status     = EFI_SUCCESS;
  BootPool.Count      = 1;
  BootPool.Der[0]     = cert;
  BootPool.DerSize[0] = certSize;
  CHECK (PeParse (image, imageSize, &info));
  CHECK (AuthentiHash (&info, hash));
  CHECK (expectedSize == sizeof (hash) && memcmp (hash, expected, sizeof (hash)) == 0);
  CHECK (SfbSecurityVerifyImage (image, imageSize) == EFI_SUCCESS);
  SfbSecurityGrantPrivilege ();
  BootPool.Der[0]     = wrong;
  BootPool.DerSize[0] = wrongSize;
  CHECK (SfbSecurityVerifyImage (image, imageSize) == EFI_SECURITY_VIOLATION);
  BootPool.Der[0]     = cert;
  BootPool.DerSize[0] = certSize;

  memcpy (copy, image, imageSize);
  copy[info.Sections[0].Offset] ^= 1;
  CHECK (SfbSecurityVerifyImage (copy, imageSize) == EFI_SECURITY_VIOLATION);
  memcpy (copy, image, imageSize);
  copy[info.CheckSumOffset] ^= 1;
  CHECK (SfbSecurityVerifyImage (copy, imageSize) == EFI_SUCCESS);
  memcpy (copy, image, imageSize);
  Put32 (copy + 0x3c, 0xfffffff0);
  CHECK (SfbSecurityVerifyImage (copy, imageSize) == EFI_SECURITY_VIOLATION);
  memcpy (copy, image, imageSize);
  Put32 (copy + info.SecDirOffset, 8);
  CHECK (SfbSecurityVerifyImage (copy, imageSize) == EFI_SECURITY_VIOLATION);
  memcpy (copy, image, imageSize);
  Put32 (copy + info.Certificate.Offset, 0x10000);
  CHECK (SfbSecurityVerifyImage (copy, imageSize) == EFI_SECURITY_VIOLATION);
  memcpy (copy, image, imageSize);
  Put16 (copy + info.Certificate.Offset + 4, 0x0100);
  CHECK (SfbSecurityVerifyImage (copy, imageSize) == EFI_SECURITY_VIOLATION);
  memcpy (copy, image, imageSize);
  Put16 (copy + info.Certificate.Offset + 6, 1);
  CHECK (SfbSecurityVerifyImage (copy, imageSize) == EFI_SECURITY_VIOLATION);
  memcpy (copy, image, imageSize);
  Put32 (copy + info.Certificate.Offset, 7);
  CHECK (SfbSecurityVerifyImage (copy, imageSize) == EFI_SECURITY_VIOLATION);
  memcpy (copy, image, imageSize);
  copy[info.Certificate.Offset + 8] = 0x31;
  CHECK (SfbSecurityVerifyImage (copy, imageSize) == EFI_SECURITY_VIOLATION);
  memcpy (copy, image, imageSize);
  Put32 (copy + info.SecDirOffset, 0);
  Put32 (copy + info.SecDirOffset + 4, 0);
  CHECK (SfbSecurityVerifyImage (copy, info.Certificate.Offset) == EFI_UNSUPPORTED);
  for (size_t n = 0; n < imageSize; n++) {
    CHECK (SfbSecurityVerifyImage (image, n) != EFI_SUCCESS);
  }

  /* Flip every certificate-envelope byte. Signature-covered bytes must fail;
   * untrusted embedded certificates and padding may be ignored, never crash. */
  for (size_t n = info.Certificate.Offset + 8; n < imageSize; n++) {
    memcpy (copy, image, imageSize);
    copy[n] ^= 0x80;
    (void)SfbSecurityVerifyImage (copy, imageSize);
  }

  BootPool.Count = 0;  /* borrowed fixture buffers are not pool allocations */
  SfbSecurityReset ();
  AuthPool.Count  = 1;
  AuthPool.Status = EFI_SUCCESS;
  CHECK (SfbSecurityChallenge () == NULL);
  CHECK (!SfbSecurityIsPrivileged ());
  CHECK (SfbSecurityVerifyChallenge (image, 1) == EFI_NOT_READY);
  RngMode = 2;
  CHECK (SfbSecurityChallenge () == NULL);
  CHECK (SfbSecurityVerifyChallenge (image, 1) == EFI_NOT_READY);
  RngMode = 1;
  const UINT8  *nonce = SfbSecurityChallenge ();

  CHECK (nonce != NULL);
  UINT8  first[32];

  memcpy (first, nonce, sizeof (first));

  CHECK (memcmp (first, SfbSecurityChallenge (), sizeof (first)) == 0);
  CHECK (SfbSecurityVerifyChallenge (NULL, 0) == EFI_SECURITY_VIOLATION);
  CHECK (SfbSecurityVerifyChallenge (image, 1) == EFI_NOT_READY);
  CHECK (!SfbSecurityIsPrivileged ());
  CHECK (memcmp (first, SfbSecurityChallenge (), sizeof (first)) != 0);
  SfbSecurityNewChallenge ();
  RngMode = 0;
  CHECK (SfbSecurityChallenge () == NULL);
  CHECK (!SfbSecurityIsPrivileged ());

  AuthPool.Der[0]     = cert;
  AuthPool.DerSize[0] = certSize;
  RngMode             = 1;
  RngByte             = 1;
  CHECK (SfbSecurityChallenge () != NULL);
  CHECK (SfbSecurityVerifyChallenge (response, responseSize) == EFI_SUCCESS);
  CHECK (SfbSecurityIsPrivileged ());
  CHECK (SfbSecurityVerifyChallenge (response, responseSize) == EFI_NOT_READY);
  AuthPool.Count = 0;
  SfbSecurityReset ();

  RootFile.Open         = OpenFile;
  RootFile.Close        = CloseFile;
  Directory.Open        = OpenFile;
  Directory.Close       = CloseFile;
  Directory.Read        = ReadFile;
  Directory.SetPosition = SetPosition;
  CertFile.Read         = ReadFile;
  CertFile.Close        = CloseFile;
  FileData              = cert;
  FileSize              = certSize;
  SfbSecurityLoadCerts ();
  CHECK (!SfbSecurityAuthEnabled () && !SfbSecurityVerifyEnabled ());
  CHECK (SfbSecurityVerifyImage (image, imageSize) == EFI_SUCCESS);
  StoreStatus = EFI_DEVICE_ERROR;
  SfbSecurityLoadCerts ();
  CHECK (SfbSecurityAuthEnabled () && SfbSecurityVerifyEnabled ());
  CHECK (SfbSecurityVerifyImage (image, imageSize) == EFI_SECURITY_VIOLATION);
  SfbSecurityGrantPrivilege ();
  CHECK (!SfbSecurityIsPrivileged ());
  StoreStatus = EFI_SUCCESS;
  FileMode    = 0;
  SfbSecurityLoadCerts ();
  CHECK (!SfbSecurityAuthEnabled () && !SfbSecurityVerifyEnabled ());
  for (int mode = 1; mode <= 5; mode++) {
    if (mode == 3) {
      continue;
    }

    FileMode = mode;
    SfbSecurityLoadCerts ();
    CHECK (SfbSecurityAuthEnabled () && SfbSecurityVerifyEnabled ());
    CHECK (SfbSecurityVerifyImage (image, imageSize) == EFI_SECURITY_VIOLATION);
  }

  FileMode = 3;
  FileData = image;
  FileSize = 32;
  SfbSecurityLoadCerts ();
  CHECK (SfbSecurityAuthEnabled () && SfbSecurityVerifyEnabled ());
  CHECK (SfbSecurityVerifyImage (image, imageSize) == EFI_SECURITY_VIOLATION);
  FileData = cert;
  FileSize = certSize;
  SfbSecurityLoadCerts ();
  CHECK (AuthPool.Count == 1 && BootPool.Count == 1);
  CHECK (SfbSecurityVerifyImage (image, imageSize) == EFI_SUCCESS);
  SfbSecurityGrantPrivilege ();
  CHECK (SfbSecurityIsPrivileged ());
  SfbSecurityLoadCerts ();
  CHECK (!SfbSecurityIsPrivileged ());
  SfbSecurityReset ();

  free (image);
  free (cert);
  free (wrong);
  free (expected);
  free (response);
  free (copy);
  printf ("Security parser/hash/RNG/certificate policy tests: %d failure(s)\n", Failures);
  return Failures != 0;
}
