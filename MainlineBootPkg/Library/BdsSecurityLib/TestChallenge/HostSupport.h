/** @file
  Host-only portability shim for compiling the unchanged edk2 crypto sources.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#ifndef BDS_CHALLENGE_HOST_SUPPORT_H_
#define BDS_CHALLENGE_HOST_SUPPORT_H_

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseCryptLib.h>
#include <Library/MemoryAllocationLib.h>
#pragma GCC visibility push (default)
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <openssl/opensslv.h>

// Use the host C runtime, not edk2's firmware libc emulation. The implementation
// files themselves are compiled directly from the repository without edits.
#define __INTERNAL_CRYPT_LIB_H__

BOOLEAN
WrapPkcs7Data (
  IN CONST UINT8  *P7Data,
  IN UINTN        P7Length,
  OUT BOOLEAN     *WrapFlag,
  OUT UINT8       **WrapData,
  OUT UINTN       *WrapDataSize
  );

#endif
