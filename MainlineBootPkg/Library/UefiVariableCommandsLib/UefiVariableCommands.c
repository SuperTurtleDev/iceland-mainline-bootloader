/** @file
  UEFI runtime variable management with typed values and bounded output.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Guid/GlobalVariable.h>
#include <Guid/ImageAuthentication.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BdsSecurityLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiVariableCommandsLib.h>

#define VARIABLE_LIMIT      SIZE_1MB
#define DEFAULT_ATTRIBUTES  (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)
#define AUTH_ATTRIBUTES     (DEFAULT_ATTRIBUTES | EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS)
#define STORED_ATTRIBUTES   AUTH_ATTRIBUTES
#define WRITE_ATTRIBUTES    (STORED_ATTRIBUTES | EFI_VARIABLE_APPEND_WRITE)

typedef struct {
  EFI_GUID    Guid;
  CHAR16      *Name;
  BOOLEAN     Qualified;
} VARIABLE_KEY;

STATIC INTN
HexDigit (
  IN CHAR8  Ch
  )
{
  if ((Ch >= '0') && (Ch <= '9')) {
    return Ch - '0';
  }

  if ((Ch >= 'a') && (Ch <= 'f')) {
    return Ch - 'a' + 10;
  }

  if ((Ch >= 'A') && (Ch <= 'F')) {
    return Ch - 'A' + 10;
  }

  return -1;
}

STATIC EFI_STATUS
Utf8Decode (
  IN CONST CHAR8  *Text,
  OUT CHAR16      **Unicode
  )
{
  UINTN   Length;
  UINTN   Index;
  UINTN   Used;
  UINTN   Count;
  UINTN   Byte;
  UINT32  Code;
  UINT32  Minimum;
  CHAR16  *Result;

  *Unicode = NULL;
  Length   = AsciiStrLen (Text);
  Result   = AllocateZeroPool ((Length + 1) * sizeof (CHAR16));
  if (Result == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Index = Used = 0;
  while (Index < Length) {
    Byte    = (UINT8)Text[Index++];
    Count   = 0;
    Minimum = 0;
    if (Byte < 0x80) {
      Code = (UINT32)Byte;
    } else if ((Byte & 0xE0) == 0xC0) {
      Code    = (UINT32)Byte & 0x1F;
      Count   = 1;
      Minimum = 0x80;
    } else if ((Byte & 0xF0) == 0xE0) {
      Code    = (UINT32)Byte & 0x0F;
      Count   = 2;
      Minimum = 0x800;
    } else if ((Byte & 0xF8) == 0xF0) {
      Code    = (UINT32)Byte & 7;
      Count   = 3;
      Minimum = 0x10000;
    } else {
      goto Invalid;
    }

    if (Count > Length - Index) {
      goto Invalid;
    }

    while (Count-- != 0) {
      Byte = (UINT8)Text[Index++];
      if ((Byte & 0xC0) != 0x80) {
        goto Invalid;
      }

      Code = (Code << 6) | ((UINT32)Byte & 0x3F);
    }

    if ((Code < Minimum) || (Code > 0x10FFFF) || ((Code >= 0xD800) && (Code <= 0xDFFF))) {
      goto Invalid;
    }

    if (Code < 0x10000) {
      Result[Used++] = (CHAR16)Code;
    } else {
      Code          -= 0x10000;
      Result[Used++] = (CHAR16)(0xD800 | (Code >> 10));
      Result[Used++] = (CHAR16)(0xDC00 | (Code & 0x3FF));
    }
  }

  *Unicode = Result;
  return EFI_SUCCESS;
Invalid:
  FreePool (Result);
  return EFI_INVALID_PARAMETER;
}

STATIC EFI_STATUS
ParseKey (
  IN CONST CHAR8    *Text,
  OUT VARIABLE_KEY  *Key
  )
{
  CHAR8       GuidText[37];
  UINTN       Index;
  UINTN       Length;
  EFI_STATUS  Status;

  ZeroMem (Key, sizeof (*Key));
  Length = AsciiStrLen (Text);
  if ((Length > 36) && (Text[36] == '-')) {
    for (Index = 0; Index < 36; Index++) {
      if ((Index == 8) || (Index == 13) || (Index == 18) || (Index == 23)) {
        if (Text[Index] != '-') {
          return EFI_INVALID_PARAMETER;
        }
      } else if (HexDigit (Text[Index]) < 0) {
        return EFI_INVALID_PARAMETER;
      }
    }

    CopyMem (GuidText, Text, 36);
    GuidText[36] = 0;
    Status       = AsciiStrToGuid (GuidText, &Key->Guid);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Key->Qualified = TRUE;
    Text          += 37;
  }

  if (*Text == 0) {
    return EFI_INVALID_PARAMETER;
  }

  return Utf8Decode (Text, &Key->Name);
}

STATIC BOOLEAN
IsCommandReadOnly (
  IN CONST CHAR16    *Name,
  IN CONST EFI_GUID  *Guid
  )
{
  // The Setup-only Secure Fastboot switch must never acquire RT or bypass
  // physical setup through generic OEM writes or deletion. Its setter retains NV|BS.
  if (CompareGuid (Guid, &gMainlineBootVariableGuid) &&
      (StrCmp (Name, BDS_SECURE_FASTBOOT_ENABLE_NAME) == 0))
  {
    return TRUE;
  }

  return (BOOLEAN)(CompareGuid (Guid, &gEfiGlobalVariableGuid) &&
                   ((StrCmp (Name, L"SecureBoot") == 0) ||
                    (StrCmp (Name, L"SetupMode") == 0) ||
                    (StrCmp (Name, L"AuditMode") == 0) ||
                    (StrCmp (Name, L"DeployedMode") == 0)));
}

STATIC BOOLEAN
IsSecureBootKey (
  IN CONST CHAR16    *Name,
  IN CONST EFI_GUID  *Guid
  )
{
  if (CompareGuid (Guid, &gEfiGlobalVariableGuid)) {
    return (BOOLEAN)((StrCmp (Name, L"PK") == 0) || (StrCmp (Name, L"KEK") == 0));
  }

  return (BOOLEAN)(CompareGuid (Guid, &gEfiImageSecurityDatabaseGuid) &&
                   ((StrCmp (Name, L"db") == 0) ||
                    (StrCmp (Name, L"dbx") == 0) ||
                    (StrCmp (Name, L"dbt") == 0)));
}

// A size query retrieves attributes without copying the existing value.
STATIC EFI_STATUS
ReadAttributes (
  IN CHAR16    *Name,
  IN EFI_GUID  *Guid,
  OUT UINT32   *Attributes
  )
{
  EFI_STATUS  Status;
  UINTN       Size;

  Size        = 0;
  *Attributes = 0;
  Status      = gRT->GetVariable (Name, Guid, Attributes, &Size, NULL);
  return Status == EFI_BUFFER_TOO_SMALL ? EFI_SUCCESS : Status;
}

STATIC EFI_STATUS
RemoveVariable (
  IN CHAR16    *Name,
  IN EFI_GUID  *Guid
  )
{
  EFI_STATUS  Status;
  UINT32      Attributes;

  if (IsCommandReadOnly (Name, Guid)) {
    return EFI_WRITE_PROTECTED;
  }

  // Authenticated deletion needs its signed descriptor, supplied via setvar.
  if (IsSecureBootKey (Name, Guid)) {
    return EFI_SECURITY_VIOLATION;
  }

  Status = ReadAttributes (Name, Guid, &Attributes);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Attributes & EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS) != 0) {
    return EFI_SECURITY_VIOLATION;
  }

  if ((Attributes & ~STORED_ATTRIBUTES) != 0) {
    return EFI_UNSUPPORTED;
  }

  return gRT->SetVariable (Name, Guid, 0, 0, NULL);
}

STATIC EFI_STATUS
ReadVariable (
  IN CHAR16    *Name,
  IN EFI_GUID  *Guid,
  OUT UINT32   *Attributes,
  OUT UINT8    **Data,
  OUT UINTN    *Size
  )
{
  EFI_STATUS  Status;
  UINTN       Required;
  UINTN       Attempt;
  UINT8       *Buffer;

  *Data = NULL;
  *Size = 0;
  for (Attempt = 0; Attempt < 3; Attempt++) {
    Required = 0;
    Status   = gRT->GetVariable (Name, Guid, Attributes, &Required, NULL);
    if (Status == EFI_SUCCESS) {
      return EFI_SUCCESS;
    }

    if (Status != EFI_BUFFER_TOO_SMALL) {
      return Status;
    }

    if ((Required == 0) || (Required > VARIABLE_LIMIT)) {
      return EFI_BAD_BUFFER_SIZE;
    }

    Buffer = AllocatePool (Required);
    if (Buffer == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    *Size  = Required;
    Status = gRT->GetVariable (Name, Guid, Attributes, Size, Buffer);
    if (!EFI_ERROR (Status) && (*Size <= Required)) {
      *Data = Buffer;
      return EFI_SUCCESS;
    }

    FreePool (Buffer);
    if (Status != EFI_BUFFER_TOO_SMALL) {
      return EFI_ERROR (Status) ? Status : EFI_COMPROMISED_DATA;
    }
  }

  return EFI_NOT_READY;
}

STATIC VOID
EmitFragments (
  IN CONST CHAR8         *Prefix,
  IN CONST CHAR8         *Text,
  IN UEFI_VARIABLE_EMIT  Emit
  )
{
  CHAR8  Line[61];
  UINTN  PrefixSize;
  UINTN  Count;
  UINTN  Length;
  UINTN  Offset;

  PrefixSize = AsciiStrLen (Prefix);
  Length     = AsciiStrLen (Text);
  Offset     = 0;
  do {
    Count = MIN (Length - Offset, 60 - PrefixSize);
    CopyMem (Line, Prefix, PrefixSize);
    CopyMem (Line + PrefixSize, Text + Offset, Count);
    Line[PrefixSize + Count] = 0;
    Emit (Line);
    Offset += Count;
  } while (Offset < Length);
}

STATIC EFI_STATUS
EmitVariable (
  IN CHAR16              *Name,
  IN EFI_GUID            *Guid,
  IN UEFI_VARIABLE_EMIT  Emit
  )
{
  EFI_STATUS   Status;
  UINT32       Attributes;
  UINT8        *Data;
  UINTN        Size;
  UINTN        Index;
  UINTN        Offset;
  UINTN        Count;
  UINTN        Used;
  UINT32       Code;
  CHAR8        Line[61];
  CHAR8        *Encoded;
  CONST CHAR8  *Hex = "0123456789abcdef";

  Status = ReadVariable (Name, Guid, &Attributes, &Data, &Size);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Encoded = AllocatePool (StrLen (Name) * 4 + 1);
  if (Encoded == NULL) {
    if (Data != NULL) {
      FreePool (Data);
    }

    return EFI_OUT_OF_RESOURCES;
  }

  // Exact UTF-16 code units keep names unambiguous, including firmware names
  // that cannot be represented by a well-formed UTF-8 string.
  for (Index = 0; Index < StrLen (Name); Index++) {
    Encoded[Index * 4]     = Hex[(Name[Index] >> 12) & 15];
    Encoded[Index * 4 + 1] = Hex[(Name[Index] >> 8) & 15];
    Encoded[Index * 4 + 2] = Hex[(Name[Index] >> 4) & 15];
    Encoded[Index * 4 + 3] = Hex[Name[Index] & 15];
  }

  Encoded[Index * 4] = 0;
  AsciiSPrint (Line, sizeof (Line), "begin guid=%g", Guid);
  Emit (Line);
  EmitFragments ("name16:", Encoded, Emit);
  Used = 0;
  for (Index = 0; Name[Index] != 0; Index++) {
    Code = Name[Index];
    if ((Code >= 0xD800) && (Code <= 0xDBFF) && (Name[Index + 1] >= 0xDC00) && (Name[Index + 1] <= 0xDFFF)) {
      Code = 0x10000 + ((Code - 0xD800) << 10) + (Name[++Index] - 0xDC00);
    } else if ((Code >= 0xD800) && (Code <= 0xDFFF)) {
      Code = 0xFFFD;
    }

    if (Code < 0x80) {
      Encoded[Used++] = (CHAR8)Code;
    } else if (Code < 0x800) {
      Encoded[Used++] = (CHAR8)(0xC0 | (Code >> 6));
      Encoded[Used++] = (CHAR8)(0x80 | (Code & 0x3F));
    } else if (Code < 0x10000) {
      Encoded[Used++] = (CHAR8)(0xE0 | (Code >> 12));
      Encoded[Used++] = (CHAR8)(0x80 | ((Code >> 6) & 0x3F));
      Encoded[Used++] = (CHAR8)(0x80 | (Code & 0x3F));
    } else {
      Encoded[Used++] = (CHAR8)(0xF0 | (Code >> 18));
      Encoded[Used++] = (CHAR8)(0x80 | ((Code >> 12) & 0x3F));
      Encoded[Used++] = (CHAR8)(0x80 | ((Code >> 6) & 0x3F));
      Encoded[Used++] = (CHAR8)(0x80 | (Code & 0x3F));
    }
  }

  Encoded[Used] = 0;
  EmitFragments ("name:", Encoded, Emit);
  AsciiSPrint (Line, sizeof (Line), "attrs=0x%x size=%u", Attributes, Size);
  Emit (Line);
  for (Offset = 0; Offset < Size; Offset += Count) {
    Count = MIN (Size - Offset, 23);
    AsciiSPrint (Line, sizeof (Line), "hex@%08x:", (UINT32)Offset);
    for (Index = 0; Index < Count; Index++) {
      Line[13 + Index * 2] = Hex[Data[Offset + Index] >> 4];
      Line[14 + Index * 2] = Hex[Data[Offset + Index] & 15];
    }

    Line[13 + Count * 2] = 0;
    Emit (Line);
  }

  Emit ("end");
  FreePool (Encoded);
  if (Data != NULL) {
    FreePool (Data);
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
FindVariables (
  IN VARIABLE_KEY        *Key,
  IN BOOLEAN             All,
  IN BOOLEAN             Remove,
  IN UEFI_VARIABLE_EMIT  Emit
  )
{
  EFI_STATUS  Status;
  EFI_GUID    Guid;
  EFI_GUID    MatchedGuid;
  CHAR16      *Name;
  CHAR16      *Larger;
  UINTN       Capacity;
  UINTN       Size;
  UINTN       Matches;

  Capacity = 256;
  Name     = AllocateZeroPool (Capacity);
  if (Name == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (&Guid, sizeof (Guid));
  Matches = 0;
  while (TRUE) {
    Size   = Capacity;
    Status = gRT->GetNextVariableName (&Size, Name, &Guid);
    if (Status == EFI_BUFFER_TOO_SMALL) {
      if ((Size <= Capacity) || (Size > VARIABLE_LIMIT) || ((Size & 1) != 0)) {
        Status = EFI_COMPROMISED_DATA;
        break;
      }

      Larger = AllocateZeroPool (Size);
      if (Larger == NULL) {
        Status = EFI_OUT_OF_RESOURCES;
        break;
      }

      CopyMem (Larger, Name, Capacity);
      FreePool (Name);
      Name     = Larger;
      Capacity = Size;
      continue;
    }

    if (Status == EFI_NOT_FOUND) {
      Status = Matches != 0 || All ? EFI_SUCCESS : EFI_NOT_FOUND;
      break;
    }

    if (EFI_ERROR (Status)) {
      break;
    }

    if ((Size < 2) || (Size > Capacity) || ((Size & 1) != 0) || (Name[Size / 2 - 1] != 0)) {
      Status = EFI_COMPROMISED_DATA;
      break;
    }

    if (!All && (StrCmp (Name, Key->Name) != 0)) {
      continue;
    }

    Matches++;
    CopyGuid (&MatchedGuid, &Guid);
    if (!Remove) {
      Status = EmitVariable (Name, &Guid, Emit);
      if (EFI_ERROR (Status)) {
        break;
      }
    }
  }

  FreePool (Name);
  if (!EFI_ERROR (Status) && Remove) {
    if (Matches != 1) {
      return EFI_NO_MAPPING;
    }

    Status = RemoveVariable (Key->Name, &MatchedGuid);
  }

  return Status;
}

STATIC EFI_STATUS
ParseInteger (
  IN CONST CHAR8  *Text,
  IN UINTN        Bytes,
  OUT UINT8       *Data
  )
{
  UINT64  Value;
  UINT64  Maximum;
  UINTN   Base;
  UINTN   Index;
  INTN    Digit;

  Base = 10;
  if ((Text[0] == '0') && ((Text[1] == 'x') || (Text[1] == 'X'))) {
    Base  = 16;
    Text += 2;
  }

  if (*Text == 0) {
    return EFI_INVALID_PARAMETER;
  }

  Maximum = Bytes == 8 ? MAX_UINT64 : (LShiftU64 (1, Bytes * 8) - 1);
  Value   = 0;
  while (*Text != 0) {
    Digit = HexDigit (*Text++);
    if ((Digit < 0) || ((UINTN)Digit >= Base) || (Value > (Maximum - (UINTN)Digit) / Base)) {
      return EFI_INVALID_PARAMETER;
    }

    Value = Value * Base + (UINTN)Digit;
  }

  for (Index = 0; Index < Bytes; Index++) {
    Data[Index] = (UINT8)RShiftU64 (Value, Index * 8);
  }

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
ParseValue (
  IN CONST CHAR8  *Text,
  OUT UINT8       **Data,
  OUT UINTN       *Size
  )
{
  EFI_STATUS  Status;
  CHAR16      *Unicode;
  UINTN       Length;
  UINTN       Index;
  UINTN       Bytes;
  INTN        High;
  INTN        Low;

  *Data = NULL;
  *Size = 0;
  if (AsciiStrnCmp (Text, "hex:", 4) == 0) {
    Text  += 4;
    Length = AsciiStrLen (Text);
    if ((Length == 0) || ((Length & 1) != 0)) {
      return EFI_INVALID_PARAMETER;
    }

    *Size = Length / 2;
    *Data = AllocatePool (*Size);
    if (*Data == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    for (Index = 0; Index < *Size; Index++) {
      High = HexDigit (Text[Index * 2]);
      Low  = HexDigit (Text[Index * 2 + 1]);
      if ((High < 0) || (Low < 0)) {
        FreePool (*Data);
        *Data = NULL;
        return EFI_INVALID_PARAMETER;
      }

      (*Data)[Index] = (UINT8)((High << 4) | Low);
    }

    return EFI_SUCCESS;
  }

  if ((AsciiStrnCmp (Text, "utf8:", 5) == 0) || (AsciiStrnCmp (Text, "utf16:", 6) == 0)) {
    Bytes  = Text[3] == '8' ? 5 : 6;
    Status = Utf8Decode (Text + Bytes, &Unicode);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (Bytes == 6) {
      *Data = (UINT8 *)Unicode;
      *Size = StrSize (Unicode);
      return EFI_SUCCESS;
    }

    FreePool (Unicode);
    *Size = AsciiStrSize (Text + Bytes);
    *Data = AllocateCopyPool (*Size, Text + Bytes);
    return *Data == NULL ? EFI_OUT_OF_RESOURCES : EFI_SUCCESS;
  }

  Bytes  = 0;
  Length = 0;
  if (AsciiStrnCmp (Text, "u8:", 3) == 0) {
    Bytes  = 1;
    Length = 3;
  }

  if (AsciiStrnCmp (Text, "u16:", 4) == 0) {
    Bytes  = 2;
    Length = 4;
  }

  if (AsciiStrnCmp (Text, "u32:", 4) == 0) {
    Bytes  = 4;
    Length = 4;
  }

  if (AsciiStrnCmp (Text, "u64:", 4) == 0) {
    Bytes  = 8;
    Length = 4;
  }

  if (Bytes == 0) {
    return EFI_INVALID_PARAMETER;
  }

  *Data = AllocatePool (Bytes);
  if (*Data == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = ParseInteger (Text + Length, Bytes, *Data);
  if (EFI_ERROR (Status)) {
    FreePool (*Data);
    *Data = NULL;
    return Status;
  }

  *Size = Bytes;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
ParseAttributes (
  IN OUT CHAR8  **Text,
  OUT UINT32    *Attributes,
  OUT BOOLEAN   *Explicit
  )
{
  CHAR8   *Cursor;
  CHAR8   *End;
  CHAR8   Saved;
  UINT32  Flag;

  *Explicit = FALSE;
  if (AsciiStrnCmp (*Text, "attrs=", 6) != 0) {
    return EFI_SUCCESS;
  }

  *Explicit   = TRUE;
  *Attributes = 0;
  Cursor      = *Text + 6;
  while (TRUE) {
    End = Cursor;
    while ((*End != 0) && (*End != ',') && (*End != ' ')) {
      End++;
    }

    Saved = *End;
    *End  = 0;
    Flag  = 0;
    if (AsciiStrCmp (Cursor, "NV") == 0) {
      Flag = EFI_VARIABLE_NON_VOLATILE;
    }

    if (AsciiStrCmp (Cursor, "BS") == 0) {
      Flag = EFI_VARIABLE_BOOTSERVICE_ACCESS;
    }

    if (AsciiStrCmp (Cursor, "RT") == 0) {
      Flag = EFI_VARIABLE_RUNTIME_ACCESS;
    }

    if (AsciiStrCmp (Cursor, "AUTH") == 0) {
      Flag = EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS;
    }

    if (AsciiStrCmp (Cursor, "APPEND") == 0) {
      Flag = EFI_VARIABLE_APPEND_WRITE;
    }

    *End = Saved;
    if ((Flag == 0) || ((*Attributes & Flag) != 0)) {
      return EFI_INVALID_PARAMETER;
    }

    *Attributes |= Flag;
    if (Saved != ',') {
      break;
    }

    Cursor = End + 1;
  }

  if ((Saved != ' ') || ((*Attributes & EFI_VARIABLE_BOOTSERVICE_ACCESS) == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  *Text = End;
  while (**Text == ' ') {
    (*Text)++;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
UefiVariableCommandWithData (
  IN CONST CHAR8         *Command,
  IN CONST CHAR8         *Arguments,
  IN UEFI_VARIABLE_EMIT  Emit,
  OUT CHAR8              *Error,
  IN UINTN               ErrorSize,
  IN CONST VOID          *DownloadedData OPTIONAL,
  IN UINTN               DownloadedSize
  )
{
  EFI_STATUS    Status;
  VARIABLE_KEY  Key;
  CHAR8         *Copy;
  CHAR8         *Value;
  CHAR8         *KeyText;
  UINT32        Attributes;
  UINT32        PreviousAttributes;
  UINT8         *Data;
  CONST VOID    *Payload;
  UINTN         Size;
  BOOLEAN       Explicit;
  BOOLEAN       Set;
  BOOLEAN       Remove;
  BOOLEAN       All;
  BOOLEAN       Download;
  BOOLEAN       AuthDownload;

  if ((Command == NULL) || (Arguments == NULL) || (Emit == NULL) || (Error == NULL) || (ErrorSize == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Error[0] = 0;
  if (AsciiStrnLenS (Arguments, UEFI_VARIABLE_COMMAND_MAX + 1) > UEFI_VARIABLE_COMMAND_MAX) {
    return EFI_BAD_BUFFER_SIZE;
  }

  Set    = (BOOLEAN)(AsciiStrCmp (Command, "setvar") == 0);
  Remove = (BOOLEAN)(AsciiStrCmp (Command, "rmvar") == 0);
  if (!Set && !Remove && (AsciiStrCmp (Command, "getvar") != 0)) {
    return EFI_UNSUPPORTED;
  }

  while (*Arguments == ' ') {
    Arguments++;
  }

  Copy = AllocateCopyPool (AsciiStrSize (Arguments), Arguments);
  if (Copy == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (&Key, sizeof (Key));
  Data    = NULL;
  KeyText = Copy;
  Value   = Copy;
  if (*KeyText == '"') {
    KeyText++;
    Value = KeyText;
    while ((*Value != 0) && (*Value != '"')) {
      Value++;
    }

    if (*Value != '"') {
      Status = EFI_INVALID_PARAMETER;
      goto Done;
    }

    *Value++ = 0;
    if ((Set && (*Value != ' ')) || (!Set && (*Value != 0))) {
      Status = EFI_INVALID_PARAMETER;
      goto Done;
    }
  } else if (Set) {
    while ((*Value != 0) && (*Value != ' ')) {
      Value++;
    }

    if (*Value != ' ') {
      Status = EFI_INVALID_PARAMETER;
      goto Done;
    }

    *Value++ = 0;
  }

  if (Set) {
    while (*Value == ' ') {
      Value++;
    }
  }

  All    = (BOOLEAN)(!Set && !Remove && (AsciiStrCmp (KeyText, "all") == 0));
  Status = All ? EFI_SUCCESS : ParseKey (KeyText, &Key);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  if (Set) {
    if (!Key.Qualified) {
      Status = EFI_INVALID_PARAMETER;
      goto Done;
    }

    if (IsCommandReadOnly (Key.Name, &Key.Guid)) {
      Status = EFI_WRITE_PROTECTED;
      goto Done;
    }

    Attributes = IsSecureBootKey (Key.Name, &Key.Guid) ? AUTH_ATTRIBUTES : DEFAULT_ATTRIBUTES;
    Status     = ParseAttributes (&Value, &Attributes, &Explicit);
    if (EFI_ERROR (Status)) {
      goto Done;
    }

    Status = ReadAttributes (Key.Name, &Key.Guid, &PreviousAttributes);
    if (!EFI_ERROR (Status)) {
      if ((PreviousAttributes & ~STORED_ATTRIBUTES) != 0) {
        Status = EFI_UNSUPPORTED;
        goto Done;
      }

      if (!Explicit) {
        Attributes = PreviousAttributes;
      } else if ((Attributes & ~EFI_VARIABLE_APPEND_WRITE) != PreviousAttributes) {
        // In particular, an explicit attribute list cannot drop AUTH.
        Status = EFI_INVALID_PARAMETER;
        goto Done;
      }
    } else if (Status != EFI_NOT_FOUND) {
      goto Done;
    }

    if (((Attributes & ~WRITE_ATTRIBUTES) != 0) ||
        ((Attributes & EFI_VARIABLE_BOOTSERVICE_ACCESS) == 0) ||
        (IsSecureBootKey (Key.Name, &Key.Guid) &&
         ((Attributes & ~EFI_VARIABLE_APPEND_WRITE) != AUTH_ATTRIBUTES)))
    {
      Status = EFI_INVALID_PARAMETER;
      goto Done;
    }

    AuthDownload = (BOOLEAN)(AsciiStrCmp (Value, "auth:download") == 0);
    Download     = (BOOLEAN)(AuthDownload || (AsciiStrCmp (Value, "download") == 0));
    if ((((Attributes & EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS) != 0) && !Download) ||
        (AuthDownload && ((Attributes & EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS) == 0)))
    {
      Status = EFI_INVALID_PARAMETER;
      goto Done;
    }

    if (Download) {
      if ((DownloadedData == NULL) || (DownloadedSize == 0)) {
        AsciiSPrint (Error, ErrorSize, "download a variable payload first");
        Status = EFI_INVALID_PARAMETER;
        goto Done;
      }

      if (DownloadedSize > VARIABLE_LIMIT) {
        Status = EFI_BAD_BUFFER_SIZE;
        goto Done;
      }

      Payload = DownloadedData;
      Size    = DownloadedSize;
    } else {
      Status = ParseValue (Value, &Data, &Size);
      if (EFI_ERROR (Status)) {
        goto Done;
      }

      Payload = Data;
    }

    // The firmware backend alone verifies .auth descriptors and signatures.
    Status = gRT->SetVariable (Key.Name, &Key.Guid, Attributes, Size, (VOID *)Payload);
  } else if (Key.Qualified) {
    Status = Remove ? RemoveVariable (Key.Name, &Key.Guid) : EmitVariable (Key.Name, &Key.Guid, Emit);
  } else {
    Status = FindVariables (&Key, All, Remove, Emit);
  }

Done:
  if (EFI_ERROR (Status) && (Error[0] == 0)) {
    if (Status == EFI_NO_MAPPING) {
      AsciiSPrint (Error, ErrorSize, "ambiguous name; use guid-Name");
    } else if (Status == EFI_INVALID_PARAMETER) {
      AsciiSPrint (Error, ErrorSize, "invalid name, attributes or typed value");
    } else {
      AsciiSPrint (Error, ErrorSize, "%a: %r", Command, Status);
    }
  }

  if (Key.Name != NULL) {
    FreePool (Key.Name);
  }

  if (Data != NULL) {
    FreePool (Data);
  }

  FreePool (Copy);
  return Status;
}

EFI_STATUS
UefiVariableCommand (
  IN CONST CHAR8         *Command,
  IN CONST CHAR8         *Arguments,
  IN UEFI_VARIABLE_EMIT  Emit,
  OUT CHAR8              *Error,
  IN UINTN               ErrorSize
  )
{
  return UefiVariableCommandWithData (Command, Arguments, Emit, Error, ErrorSize, NULL, 0);
}
