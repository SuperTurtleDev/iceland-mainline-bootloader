/* Host-only UEFI shim for the real SFB security and mbedtls implementations.
 * Copyright (c) 2026, MainlineBootPkg contributors.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef SFB_SECURITY_HOST_H
#define SFB_SECURITY_HOST_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

#define IN
#define OUT
#define OPTIONAL
#define CONST   const
#define STATIC  static
#define VOID    void
#define EFIAPI
#define TRUE                    1
#define FALSE                   0
#define MAX_UINTN               SIZE_MAX
#define SIZE_1MB                0x100000
#define SIZE_64KB               0x10000
#define EFI_OUT_OF_RESOURCES    9
#define EFI_VOLUME_CORRUPTED    10
#define EFI_FILE_MODE_READ      1
#define EFI_FILE_DIRECTORY      0x10
#define EFI_SUCCESS             0
#define EFI_INVALID_PARAMETER   2
#define EFI_UNSUPPORTED         3
#define EFI_NOT_READY           6
#define EFI_DEVICE_ERROR        7
#define EFI_NOT_FOUND           14
#define EFI_SECURITY_VIOLATION  26
#define EFI_ERROR(Status)  ((Status) != EFI_SUCCESS)
#define DEBUG(Expression)  ((void)0)

typedef uint8_t                       UINT8;
typedef uint16_t                      UINT16;
typedef uint32_t                      UINT32;
typedef uint64_t                      UINT64;
typedef int32_t                       INT32;
typedef size_t                        UINTN;
typedef char                          CHAR8;
typedef uint16_t                      CHAR16;
typedef int                           BOOLEAN;
typedef UINTN                         EFI_STATUS;
typedef VOID                          *EFI_HANDLE;
typedef struct EFI_BLOCK_IO_PROTOCOL  EFI_BLOCK_IO_PROTOCOL;
typedef struct EFI_FILE_PROTOCOL      EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
  EFI_STATUS    (*Open)(
    EFI_FILE_PROTOCOL *,
    EFI_FILE_PROTOCOL **,
    CHAR16 *,
    UINT64,
    UINT64
    );
  EFI_STATUS    (*Close)(
    EFI_FILE_PROTOCOL *
    );
  EFI_STATUS    (*Read)(
    EFI_FILE_PROTOCOL *,
    UINTN *,
    VOID *
    );
  EFI_STATUS    (*SetPosition)(
    EFI_FILE_PROTOCOL *,
    UINT64
    );
};

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
  EFI_STATUS    (*OpenVolume)(
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *,
    EFI_FILE_PROTOCOL **
    );
};

typedef struct {
  UINT64    Size;
  UINT64    FileSize;
  UINT64    Attribute;
  CHAR16    FileName[1];
} EFI_FILE_INFO;
#define SIZE_OF_EFI_FILE_INFO  offsetof(EFI_FILE_INFO, FileName)
#define AllocatePool(Size)  malloc(Size)
#define FreePool(Buffer)    free(Buffer)
static inline UINTN
StrLen (
  const CHAR16  *text
  )
{
  UINTN  length = 0;

  while (text[length]) {
    length++;
  }

  return length;
}

typedef struct {
  UINT32    Data1;
} EFI_GUID;
typedef struct EFI_RNG_PROTOCOL EFI_RNG_PROTOCOL;
struct EFI_RNG_PROTOCOL {
  EFI_STATUS    (*GetRNG)(
    EFI_RNG_PROTOCOL *,
    EFI_GUID *,
    UINTN,
    UINT8 *
    );
};

typedef struct {
  EFI_STATUS (*LocateProtocol)(
    EFI_GUID *,
    VOID *,
    VOID **
    );
} EFI_BOOT_SERVICES;
extern EFI_BOOT_SERVICES  *gBS;
extern EFI_GUID           gEfiRngProtocolGuid;

#define ZeroMem(Buffer, Size)          memset((Buffer), 0, (Size))
#define CopyMem(Dest, Source, Size)    memcpy((Dest), (Source), (Size))
#define CompareMem(Left, Right, Size)  memcmp((Left), (Right), (Size))
static inline UINT16
ReadUnaligned16 (
  const UINT16  *p
  )
{
  UINT16  value;

  memcpy (&value, p, sizeof (value));
  return value;
}

static inline UINT32
ReadUnaligned32 (
  const UINT32  *p
  )
{
  UINT32  value;

  memcpy (&value, p, sizeof (value));
  return value;
}

#endif
