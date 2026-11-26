/*
 * PE image signature verification for the SFB security policy: compute the
 * Secure-Boot authentihash of the image and verify the embedded PKCS#7
 * signature against the class B (BOOT_CERTS) keys.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/SfbSecurity.h>

#include <mbedtls/md.h>

#include "SfbSecurityInternal.h"

#define PE_SIGNATURE     0x00004550
#define PE_DIR_SECURITY  4
#define PE_OPT_MAGIC_32  0x010b
#define PE_OPT_MAGIC_64  0x020b
#define PE_SECTION_SIZE  40
#define PE_MAX_SECTIONS  96

/* Keep offsets as values, never as potentially unaligned mapped structures. */
typedef struct {
  UINT32    Offset;
  UINT32    Size;
} PE_RANGE;

typedef struct {
  CONST UINT8    *Image;
  UINTN          ImageSize;
  UINTN          CheckSumOffset;
  UINTN          SecDirOffset;
  UINT32         SizeOfHeaders;
  PE_RANGE       Certificate;
  PE_RANGE       Sections[PE_MAX_SECTIONS];
  UINT16         NumSections;
} PE_INFO;

STATIC
BOOLEAN
RangeFits (
  IN UINTN  Offset,
  IN UINTN  Length,
  IN UINTN  Limit
  )
{
  return (BOOLEAN)(Offset <= Limit && Length <= Limit - Offset);
}

STATIC
BOOLEAN
ReadU16 (
  IN CONST UINT8  *Buf,
  IN UINTN        Size,
  IN UINTN        Offset,
  OUT UINT16      *Value
  )
{
  if (!RangeFits (Offset, sizeof (*Value), Size)) {
    return FALSE;
  }

  *Value = ReadUnaligned16 ((CONST UINT16 *)(Buf + Offset));
  return TRUE;
}

STATIC
BOOLEAN
ReadU32 (
  IN CONST UINT8  *Buf,
  IN UINTN        Size,
  IN UINTN        Offset,
  OUT UINT32      *Value
  )
{
  if (!RangeFits (Offset, sizeof (*Value), Size)) {
    return FALSE;
  }

  *Value = ReadUnaligned32 ((CONST UINT32 *)(Buf + Offset));
  return TRUE;
}

STATIC
BOOLEAN
PeParse (
  IN CONST VOID  *Image,
  IN UINTN       ImageSize,
  OUT PE_INFO    *Info
  )
{
  CONST UINT8  *Buf = Image;
  UINT16       Magic;
  UINT16       OptSize;
  UINT16       Count;
  UINT32       PeOffset;
  UINT32       Signature;
  UINT32       Directories;
  UINTN        OptOffset;
  UINTN        DirOffset;
  UINTN        SectionOffset;
  UINTN        DataEnd;
  UINTN        PreviousEnd;
  UINTN        Index;

  ZeroMem (Info, sizeof (*Info));
  if ((Buf == NULL) || !ReadU16 (Buf, ImageSize, 0, &Magic) || (Magic != 0x5a4d) ||
      !ReadU32 (Buf, ImageSize, 0x3c, &PeOffset) || (PeOffset < 0x40) ||
      !RangeFits (PeOffset, 24, ImageSize) ||
      !ReadU32 (Buf, ImageSize, PeOffset, &Signature) || (Signature != PE_SIGNATURE) ||
      !ReadU16 (Buf, ImageSize, (UINTN)PeOffset + 6, &Count) ||
      (Count == 0) || (Count > PE_MAX_SECTIONS) ||
      !ReadU16 (Buf, ImageSize, (UINTN)PeOffset + 20, &OptSize))
  {
    return FALSE;
  }

  OptOffset = (UINTN)PeOffset + 24;
  if (!RangeFits (OptOffset, OptSize, ImageSize) ||
      !ReadU16 (Buf, ImageSize, OptOffset, &Magic) ||
      ((Magic != PE_OPT_MAGIC_32) && (Magic != PE_OPT_MAGIC_64)))
  {
    return FALSE;
  }

  DirOffset = Magic == PE_OPT_MAGIC_32 ? 96 : 112;
  if ((OptSize < DirOffset + (PE_DIR_SECURITY + 1) * 8) ||
      !ReadU32 (Buf, ImageSize, OptOffset + DirOffset - 4, &Directories) ||
      (Directories <= PE_DIR_SECURITY) || (Directories > (OptSize - DirOffset) / 8) ||
      !ReadU32 (Buf, ImageSize, OptOffset + 60, &Info->SizeOfHeaders))
  {
    return FALSE;
  }

  Info->CheckSumOffset = OptOffset + 64;
  Info->SecDirOffset   = OptOffset + DirOffset + PE_DIR_SECURITY * 8;
  SectionOffset        = OptOffset + OptSize;
  if ((Info->SizeOfHeaders > ImageSize) ||
      !RangeFits (SectionOffset, (UINTN)Count * PE_SECTION_SIZE, Info->SizeOfHeaders) ||
      !ReadU32 (Buf, ImageSize, Info->SecDirOffset, &Info->Certificate.Offset) ||
      !ReadU32 (Buf, ImageSize, Info->SecDirOffset + 4, &Info->Certificate.Size))
  {
    return FALSE;
  }

  DataEnd = ImageSize;
  if ((Info->Certificate.Size != 0) || (Info->Certificate.Offset != 0)) {
    /* Authenticode places the non-loaded certificate table at the file end.
     * Reject overlap and trailing data rather than hash an ambiguous layout. */
    if ((Info->Certificate.Size < 8) || ((Info->Certificate.Offset & 7) != 0) ||
        ((Info->Certificate.Size & 7) != 0) ||
        (Info->Certificate.Offset < Info->SizeOfHeaders) ||
        !RangeFits (Info->Certificate.Offset, Info->Certificate.Size, ImageSize) ||
        (Info->Certificate.Size != ImageSize - Info->Certificate.Offset))
    {
      return FALSE;
    }

    DataEnd = Info->Certificate.Offset;
  }

  /* Authenticode hashes sections sorted by PointerToRawData, not RVA or
   * section-table order. Zero-length sections contribute no data. */
  for (Index = 0; Index < Count; Index++) {
    PE_RANGE  Range;
    UINTN     Position;
    UINTN     Header = SectionOffset + Index * PE_SECTION_SIZE;

    if (!ReadU32 (Buf, ImageSize, Header + 16, &Range.Size) ||
        !ReadU32 (Buf, ImageSize, Header + 20, &Range.Offset))
    {
      return FALSE;
    }

    if (Range.Size == 0) {
      continue;
    }

    if ((Range.Offset < Info->SizeOfHeaders) ||
        !RangeFits (Range.Offset, Range.Size, DataEnd))
    {
      return FALSE;
    }

    Position = Info->NumSections;
    while (Position != 0 && Info->Sections[Position - 1].Offset > Range.Offset) {
      Info->Sections[Position] = Info->Sections[Position - 1];
      Position--;
    }

    Info->Sections[Position] = Range;
    Info->NumSections++;
  }

  PreviousEnd = Info->SizeOfHeaders;
  for (Index = 0; Index < Info->NumSections; Index++) {
    if (Info->Sections[Index].Offset < PreviousEnd) {
      return FALSE;
    }

    PreviousEnd = (UINTN)Info->Sections[Index].Offset + Info->Sections[Index].Size;
  }

  Info->Image     = Buf;
  Info->ImageSize = ImageSize;
  return TRUE;
}

STATIC
BOOLEAN
AuthentiHash (
  IN CONST PE_INFO  *Info,
  OUT UINT8         Hash[32]
  )
{
  mbedtls_md_context_t  Md;
  UINTN                 Sum = Info->SizeOfHeaders;
  UINTN                 End = Info->ImageSize - Info->Certificate.Size;
  UINTN                 Index;
  BOOLEAN               Ok = FALSE;

  mbedtls_md_init (&Md);
  if ((mbedtls_md_setup (&Md, mbedtls_md_info_from_type (MBEDTLS_MD_SHA256), 0) != 0) ||
      (mbedtls_md_starts (&Md) != 0) ||
      (mbedtls_md_update (&Md, Info->Image, Info->CheckSumOffset) != 0) ||
      (mbedtls_md_update (
         &Md,
         Info->Image + Info->CheckSumOffset + 4,
         Info->SecDirOffset - Info->CheckSumOffset - 4
         ) != 0) ||
      (mbedtls_md_update (
         &Md,
         Info->Image + Info->SecDirOffset + 8,
         Info->SizeOfHeaders - Info->SecDirOffset - 8
         ) != 0))
  {
    goto out;
  }

  for (Index = 0; Index < Info->NumSections; Index++) {
    CONST PE_RANGE  *Range = &Info->Sections[Index];

    if (!RangeFits (Sum, Range->Size, End) ||
        (mbedtls_md_update (&Md, Info->Image + Range->Offset, Range->Size) != 0))
    {
      goto out;
    }

    Sum += Range->Size;
  }

  if ((Sum > End) || (mbedtls_md_update (&Md, Info->Image + Sum, End - Sum) != 0)) {
    goto out;
  }

  Ok = (BOOLEAN)(mbedtls_md_finish (&Md, Hash) == 0);

out:
  mbedtls_md_free (&Md);
  return Ok;
}

EFI_STATUS
SfbSecurityVerifyImage (
  IN CONST VOID  *PeImage,
  IN UINTN       PeSize
  )
{
  PE_INFO              Info;
  UINT8                Hash[32];
  CONST SFB_CERT_POOL  *Pool = SfbCertsBootPool ();
  UINTN                Index;
  UINTN                Offset;
  BOOLEAN              Verified = FALSE;

  if ((PeImage == NULL) || (PeSize == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  /* Class B verification is independent of authorization privilege. */
  if (EFI_ERROR (Pool->Status)) {
    return EFI_SECURITY_VIOLATION;
  }

  if (Pool->Count == 0) {
    return EFI_SUCCESS;
  }

  if (!PeParse (PeImage, PeSize, &Info)) {
    return EFI_SECURITY_VIOLATION;
  }

  if (Info.Certificate.Size == 0) {
    return EFI_UNSUPPORTED;
  }

  if (!AuthentiHash (&Info, Hash)) {
    return EFI_SECURITY_VIOLATION;
  }

  Offset = Info.Certificate.Offset;
  while (Offset < PeSize) {
    UINT32  Length;
    UINT16  Revision;
    UINT16  Type;
    UINTN   AlignedLength;

    /* WIN_CERTIFICATE has DWORD dwLength followed by two WORD fields. */
    if (!RangeFits (Offset, 8, PeSize) ||
        !ReadU32 (Info.Image, PeSize, Offset, &Length) || (Length < 8) ||
        !ReadU16 (Info.Image, PeSize, Offset + 4, &Revision) ||
        !ReadU16 (Info.Image, PeSize, Offset + 6, &Type) ||
        !RangeFits (Offset, Length, PeSize) ||
        !RangeFits (Length, 7, MAX_UINTN))
    {
      return EFI_SECURITY_VIOLATION;
    }

    AlignedLength = ((UINTN)Length + 7) & ~(UINTN)7;
    if (!RangeFits (Offset, AlignedLength, PeSize)) {
      return EFI_SECURITY_VIOLATION;
    }

    for (Index = Length; Index < AlignedLength; Index++) {
      if (Info.Image[Offset + Index] != 0) {
        return EFI_SECURITY_VIOLATION;
      }
    }

    if (!Verified && (Type == 0x0002) && (Revision == 0x0200)) {
      for (Index = 0; Index < Pool->Count && !Verified; Index++) {
        Verified = SfbAuthenticodeVerify (
                     Info.Image + Offset + 8,
                     Length - 8,
                     Pool->Der[Index],
                     Pool->DerSize[Index],
                     Hash
                     );
      }
    }

    Offset += AlignedLength;
  }

  DEBUG ((EFI_D_INFO, "SFB-SEC: image signature %a\n", Verified ? "verified" : "rejected"));
  return Verified ? EFI_SUCCESS : EFI_SECURITY_VIOLATION;
}
