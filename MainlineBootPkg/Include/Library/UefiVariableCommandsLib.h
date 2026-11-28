/** @file
  Transport-independent UEFI variable commands for fastboot and firmware tests.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef UEFI_VARIABLE_COMMANDS_LIB_H_
#define UEFI_VARIABLE_COMMANDS_LIB_H_

#include <Uefi.h>

#define UEFI_VARIABLE_COMMAND_MAX  4096

typedef VOID (*UEFI_VARIABLE_EMIT) (
  IN CONST CHAR8  *Text
  );

/** Execute getvar, setvar or rmvar. Emit receives INFO payloads of at most 60 bytes.

  setvar accepts "guid-Name [attrs=NV,BS,RT,AUTH,APPEND] type:value". AUTH means
  EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS. Existing attributes are
  retained; explicit attributes must match, except for the APPEND operation.
  New variables default to NV|BS|RT; standard PK/KEK/db/dbx/dbt variables also
  require AUTH. Strings are NUL-terminated, integers are little-endian, hex is
  raw bytes. A name without GUID can select all matching getvar results; rmvar
  rejects ambiguity. Standard Secure Boot status variables and the
  firmware-setup-only EnableSecureFastboot switch are read-only to these
  commands.

  The values "download" and "auth:download" borrow DownloadedData for the
  duration of this call. AUTH writes require a downloaded .auth payload and
  are verified only by firmware SetVariable. "auth:download" requires AUTH
  attributes. Signed deletion uses setvar with a .auth payload containing no
  variable data; rmvar does not delete authenticated variables. Empty payloads
  are rejected. DownloadedData remains owned by the caller and is not modified.
**/
EFI_STATUS
UefiVariableCommandWithData (
  IN CONST CHAR8         *Command,
  IN CONST CHAR8         *Arguments,
  IN UEFI_VARIABLE_EMIT  Emit,
  OUT CHAR8              *Error,
  IN UINTN               ErrorSize,
  IN CONST VOID          *DownloadedData OPTIONAL,
  IN UINTN               DownloadedSize
  );

/** Execute a variable command without a downloaded payload. **/
EFI_STATUS
UefiVariableCommand (
  IN CONST CHAR8         *Command,
  IN CONST CHAR8         *Arguments,
  IN UEFI_VARIABLE_EMIT  Emit,
  OUT CHAR8              *Error,
  IN UINTN               ErrorSize
  );

#endif
