/** @file
  C Run-Time Libraries (CRT) Wrapper Implementation for MbedTLS-based
  Cryptographic Library.

Copyright (c) 2023, Intel Corporation. All rights reserved.<BR>
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Base.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <stdio.h>

#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>

int
my_snprintf (
  char        *str,
  size_t      size,
  const char  *format,
  ...
  )
{
  CHAR8    Converted[512];
  UINTN    Index;
  UINTN    Length;
  BOOLEAN  Conversion = FALSE;
  VA_LIST  Marker;

  if (format == NULL || (str == NULL && size != 0)) {
    return -1;
  }

  // PrintLib uses %a for ASCII strings; mbedtls uses standard C %s.
  // Other conversions used by this minimal mbedtls configuration agree.
  for (Index = 0; format[Index] != '\0'; Index++) {
    CHAR8  Ch = format[Index];

    if (Index + 1 >= sizeof (Converted)) {
      return -1;
    }
    Converted[Index] = Ch;
    if (!Conversion) {
      Conversion = (BOOLEAN)(Ch == '%');
    } else if (Ch == 's') {
      Converted[Index] = 'a';
      Conversion = FALSE;
    } else if (Ch == '%' || Ch == 'c' || Ch == 'd' || Ch == 'i' ||
               Ch == 'u' || Ch == 'x' || Ch == 'X' || Ch == 'p') {
      Conversion = FALSE;
    }
  }
  Converted[Index] = '\0';
  if (Conversion) {
    return -1;
  }

  // C snprintf reports the untruncated length, including for size == 0.
  VA_START (Marker, format);
  Length = SPrintLengthAsciiFormat (Converted, Marker);
  VA_END (Marker);
  if (Length > MAX_INT32) {
    return -1;
  }
  if (size != 0) {
    VA_START (Marker, format);
    AsciiVSPrint (str, size, Converted, Marker);
    VA_END (Marker);
  }
  return (int)Length;
}

//
// Extra header to record the memory buffer size from malloc routine.
//
#define CRYPTMEM_HEAD_SIGNATURE  SIGNATURE_32('c','m','h','d')
typedef struct {
  UINT32    Signature;
  UINT32    Reserved;
  UINTN     Size;
} CRYPTMEM_HEAD;

#define CRYPTMEM_OVERHEAD  sizeof(CRYPTMEM_HEAD)

//
// -- Memory-Allocation Routines --
//

/* Allocates memory blocks */
void *
mbedtls_calloc (
  size_t  num,
  size_t  size
  )
{
  CRYPTMEM_HEAD  *PoolHdr;
  UINTN          NewSize;
  VOID           *Data;

  //
  // Adjust the size by the buffer header overhead
  //
  if (size != 0 && num > (MAX_UINTN - CRYPTMEM_OVERHEAD) / size) {
    return NULL;
  }
  NewSize = (UINTN)(size * num) + CRYPTMEM_OVERHEAD;

  Data = AllocateZeroPool (NewSize);
  if (Data != NULL) {
    PoolHdr = (CRYPTMEM_HEAD *)Data;
    //
    // Record the memory brief information
    //
    PoolHdr->Signature = CRYPTMEM_HEAD_SIGNATURE;
    PoolHdr->Size      = size;

    return (VOID *)(PoolHdr + 1);
  } else {
    //
    // The buffer allocation failed.
    //
    return NULL;
  }
}

/* De-allocates or frees a memory block */
void
mbedtls_free (
  void  *ptr
  )
{
  CRYPTMEM_HEAD  *PoolHdr;

  //
  // In Standard C, free() handles a null pointer argument transparently. This
  // is not true of FreePool() below, so protect it.
  //
  if (ptr != NULL) {
    PoolHdr = (CRYPTMEM_HEAD *)ptr - 1;
    ASSERT (PoolHdr->Signature == CRYPTMEM_HEAD_SIGNATURE);
    FreePool (PoolHdr);
  }
}
