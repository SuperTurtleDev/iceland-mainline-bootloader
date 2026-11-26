/** @file
  BIOS Setup: boot variables, file browser and Canoe USB storage export.

  Copyright (c) 2026, contributors to the canoe ABL tree.
  SPDX-License-Identifier: BSD-3-Clause
**/

#ifndef MAINLINE_BDS_SETUP_H_
#define MAINLINE_BDS_SETUP_H_

#include <Uefi.h>

// TRUE requests fastboot authorization/transport; FALSE returns to Boot Menu.
BOOLEAN
BdsRunSetup (
  VOID
  );

#endif
