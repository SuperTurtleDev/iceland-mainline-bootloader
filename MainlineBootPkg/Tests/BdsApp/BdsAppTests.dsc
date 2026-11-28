## @file
# Test applications only; no dependency on the production platform DSC or FDF.
# SPDX-License-Identifier: BSD-2-Clause-Patent
##
[Defines]
  PLATFORM_NAME = BdsAppTests
  PLATFORM_GUID = 4E10EE52-073F-4D35-9403-EF96161F3E95
  PLATFORM_VERSION = 1.0
  DSC_SPECIFICATION = 0x00010005
  DEFINE BDS_TEST_OUT = Build/BdsAppTest
  OUTPUT_DIRECTORY = $(BDS_TEST_OUT)
  SUPPORTED_ARCHITECTURES = AARCH64
  BUILD_TARGETS = DEBUG|RELEASE
  SKUID_IDENTIFIER = DEFAULT

[LibraryClasses.common]
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLibOptDxe/BaseMemoryLibOptDxe.inf
  BaseMemoryLibOptDxe|MdePkg/Library/BaseMemoryLibOptDxe/BaseMemoryLibOptDxe.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  ReportStatusCodeLib|MdeModulePkg/Library/DxeReportStatusCodeLib/DxeReportStatusCodeLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  DxeServicesLib|MdePkg/Library/DxeServicesLib/DxeServicesLib.inf
  UefiVariableCommandsLib|MainlineBootPkg/Library/UefiVariableCommandsLib/UefiVariableCommandsLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf

[LibraryClasses.AARCH64]
  CompilerIntrinsicsLib|ArmPkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf
  NULL|ArmPkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf

[BuildOptions.common]
  GCC:*_*_*_ARCHCC_FLAGS = -fno-stack-protector -fno-common -Wno-unknown-warning-option -Wno-varargs -Wno-misleading-indentation -Wno-shift-negative-value
  GCC:*_*_*_DLINK_FLAGS = -Wl,-Ttext=0x0 $(CLANG_EXTRA_DLINK_FLAGS)
  !ifdef BDS_TEST_DIAGNOSTICS
  GCC:*_*_*_CC_FLAGS = -DBDS_TEST_DIAGNOSTICS
  !endif

[Components.common]
  MainlineBootPkg/Tests/BdsApp/Launcher.inf
  MainlineBootPkg/Tests/BdsApp/Payload.inf
  MainlineBootPkg/Tests/BdsApp/DirectBoot.inf
