/*
 * Shared internals of SfbSecurityLib: the certificate pools.
 *
 * Copyright (c) 2026, contributors to the canoe ABL tree.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __SFB_SECURITY_INTERNAL_H__
#define __SFB_SECURITY_INTERNAL_H__

#include <Uefi.h>
#include <Library/SfbSecurity.h>

typedef struct {
  UINT8         *Der[SFB_SECURITY_MAX_CERTS];
  UINTN         DerSize[SFB_SECURITY_MAX_CERTS];
  UINTN         Count;
  EFI_STATUS    Status; /* Errors enforce policy even when no key could load. */
} SFB_CERT_POOL;

CONST SFB_CERT_POOL *
SfbCertsAuthPool (
  VOID
  );

CONST SFB_CERT_POOL *
SfbCertsBootPool (
  VOID
  );

/* Free both pools (SfbSecurityReset calls this). */
VOID
SfbCertsReset (
  VOID
  );

/* Verify an Authenticode SignedData blob against one pinned certificate key. */
BOOLEAN
SfbAuthenticodeVerify (
  IN CONST UINT8  *Data,
  IN UINTN        DataSize,
  IN CONST UINT8  *Cert,
  IN UINTN        CertSize,
  IN CONST UINT8  ImageHash[32]
  );

#endif /* __SFB_SECURITY_INTERNAL_H__ */
