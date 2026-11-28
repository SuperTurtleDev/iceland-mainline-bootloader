/** @file
  Real AAVMF runtime-service boundary test. After ExitBootServices this payload
  uses only direct QEMU PL011 MMIO, runtime variable services and ResetSystem.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "SecureBootTest.h"
#include <Library/BdsSecurityLib.h>

STATIC VOID
SerialText (IN CONST CHAR8 *Text)
{
  while (*Text != 0) {
    while ((*(volatile UINT32 *)(UINTN)0x09000018 & BIT5) != 0) {
      // PL011 UARTFR.TXFF; no boot-service calls after ExitBootServices.
    }
    *(volatile UINT32 *)(UINTN)0x09000000 = (UINT8)*Text++;
  }
}

STATIC VOID
SerialStatus (IN CONST CHAR8 *Operation, IN EFI_STATUS Status)
{
  CHAR8         Text[19];
  UINTN         Index;
  STATIC CHAR8  Hex[] = "0123456789abcdef";

  Text[0] = '0';
  Text[1] = 'x';
  for (Index = 0; Index < 16; Index++) {
    Text[Index + 2] = Hex[(Status >> ((15 - Index) * 4)) & 15];
  }
  Text[18] = 0;
  SerialText ("SBTEST RUNTIME: ");
  SerialText (Operation);
  SerialText (" -> ");
  SerialText (Text);
  SerialText ("\r\n");
}

EFI_STATUS EFIAPI
RuntimeProbeEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  SB_TEST_STATE             *State;
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  EFI_MEMORY_DESCRIPTOR     *Map;
  UINTN                     MapSize;
  UINTN                     Capacity;
  UINTN                     MapKey;
  UINTN                     DescriptorSize;
  UINT32                    DescriptorVersion;
  UINTN                     Attempt;
  UINTN                     Size;
  UINT32                    Attributes;
  UINT8                     Enabled;
  EFI_STATUS                Status;
  EFI_STATUS                ReadStatus;
  EFI_STATUS                BsWriteStatus;
  EFI_STATUS                RtWriteStatus;

  State = SbGetState ();
  SbStatus (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded), L"runtime probe loaded image");
  SbRequire ((BOOLEAN)(Loaded->ParentHandle == State->BdsImage), L"runtime probe parent is BdsApp");
  SbCheckMode (0, 1);
  SbCheckLegacyIsolation (State);
  Size = sizeof (Enabled);
  Status = gRT->GetVariable (BDS_SECURE_FASTBOOT_ENABLE_NAME, &gMainlineBootVariableGuid, &Attributes, &Size, &Enabled);
  SbRequire ((BOOLEAN)(Status == EFI_NOT_FOUND), L"default-off fastboot does not create a setting variable");
  Enabled = 0;
  SbStatus (gRT->SetVariable (BDS_SECURE_FASTBOOT_ENABLE_NAME, &gMainlineBootVariableGuid,
                             EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS,
                             sizeof (Enabled), &Enabled), L"fixture explicitly saves NV|BS setting for runtime protection test");
  Size = sizeof (Enabled);
  SbStatus (gRT->GetVariable (BDS_SECURE_FASTBOOT_ENABLE_NAME, &gMainlineBootVariableGuid, &Attributes, &Size, &Enabled), L"pre-EBS EnableSecureFastboot");
  SbRequire ((BOOLEAN)(Size == sizeof (Enabled) && Enabled == 0 && Attributes == (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS)), L"saved fastboot setting is FALSE with NV|BS without RT");
  Print (L"SBTEST RUNTIME PRE: EnableSecureFastboot=0 attributes=NV|BS; no runtime access\n");

  MapSize = 0;
  Status = gBS->GetMemoryMap (&MapSize, NULL, &MapKey, &DescriptorSize, &DescriptorVersion);
  SbRequire ((BOOLEAN)(Status == EFI_BUFFER_TOO_SMALL && DescriptorSize >= sizeof (EFI_MEMORY_DESCRIPTOR)), L"memory map size");
  Capacity = MapSize + 64 * DescriptorSize;
  Map = AllocatePool (Capacity);
  SbRequire ((BOOLEAN)(Map != NULL), L"memory map allocation");
  Status = EFI_ABORTED;
  for (Attempt = 0; Attempt < 3; Attempt++) {
    MapSize = Capacity;
    Status = gBS->GetMemoryMap (&MapSize, Map, &MapKey, &DescriptorSize, &DescriptorVersion);
    if (EFI_ERROR (Status)) {
      break;
    }
    Status = gBS->ExitBootServices (ImageHandle, MapKey);
    if (!EFI_ERROR (Status)) {
      break;
    }
  }
  // From the first ExitBootServices attempt onward, never use Print, allocation,
  // protocol services or any other boot service except the map/retry above.
  if (EFI_ERROR (Status)) {
    SerialStatus ("ExitBootServices failed", Status);
    SerialText ("SBTEST FAIL: ExitBootServices did not complete\r\n");
    gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    CpuDeadLoop ();
  }
  SerialText ("SBTEST RUNTIME EBS: ExitBootServices succeeded\r\n");
  Size = sizeof (Enabled);
  ReadStatus = gRT->GetVariable (BDS_SECURE_FASTBOOT_ENABLE_NAME, &gMainlineBootVariableGuid, &Attributes, &Size, &Enabled);
  SerialStatus ("GetVariable EnableSecureFastboot", ReadStatus);
  Enabled = 1;
  BsWriteStatus = gRT->SetVariable (BDS_SECURE_FASTBOOT_ENABLE_NAME, &gMainlineBootVariableGuid,
                                  EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS,
                                  sizeof (Enabled), &Enabled);
  SerialStatus ("SetVariable Fastboot NV|BS", BsWriteStatus);
  RtWriteStatus = gRT->SetVariable (BDS_SECURE_FASTBOOT_ENABLE_NAME, &gMainlineBootVariableGuid,
                                  EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                                  sizeof (Enabled), &Enabled);
  SerialStatus ("SetVariable Fastboot NV|BS|RT", RtWriteStatus);
  if (ReadStatus != EFI_NOT_FOUND || !EFI_ERROR (BsWriteStatus) || !EFI_ERROR (RtWriteStatus)) {
    SerialText ("SBTEST FAIL: runtime can access or write EnableSecureFastboot\r\n");
  } else {
    SerialText ("SBTEST RUNTIME PASS: OS phase cannot read or write boot-service-only EnableSecureFastboot\r\n");
  }
  gRT->ResetSystem (EfiResetShutdown, EFI_SUCCESS, 0, NULL);
  CpuDeadLoop ();
  return EFI_ABORTED;
}
