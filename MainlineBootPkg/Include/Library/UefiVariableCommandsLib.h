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

  setvar accepts "guid-Name [attrs=NV,BS,RT] type:value". Existing attributes are
  retained unless explicitly supplied; new variables default to NV|BS|RT.
  Strings are NUL-terminated, integers are little-endian, hex is raw bytes.
  A name without GUID can select all matching getvar results; rmvar rejects
  ambiguity. Empty writes and authentication-attribute creation are rejected.
**/
EFI_STATUS
UefiVariableCommand (
  IN CONST CHAR8         *Command,
  IN CONST CHAR8         *Arguments,
  IN UEFI_VARIABLE_EMIT  Emit,
  OUT CHAR8              *Error,
  IN UINTN               ErrorSize
  );

#endif
