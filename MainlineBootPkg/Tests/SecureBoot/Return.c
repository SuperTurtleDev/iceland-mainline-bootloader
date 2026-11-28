/** @file
  Same PE payload is signed with the trusted and untrusted keys, or left unsigned.
  Reaching this entry point increments a shared execution counter.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "SecureBootTest.h"

EFI_STATUS EFIAPI
SecureBootReturnEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  SB_TEST_STATE *State;

  State = SbGetState ();
  State->Executions++;
  Print (L"SBTEST PAYLOAD: executed count=%u\n", State->Executions);
  return EFI_SUCCESS;
}
