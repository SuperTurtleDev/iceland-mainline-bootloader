## @file
# Build test helpers only. The runner takes a separately built production BdsApp.
# SPDX-License-Identifier: BSD-2-Clause-Patent
##
[Defines]
  PLATFORM_NAME = SecureBootTests
  PLATFORM_GUID = 3881B41B-CDE4-4B20-8BF1-A6302D820A6C
  PLATFORM_VERSION = 1.0
  DSC_SPECIFICATION = 0x00010005
  DEFINE SB_TEST_OUT = Build/SecureBootTest
  OUTPUT_DIRECTORY = $(SB_TEST_OUT)
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
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
!ifdef SB_CHALLENGE_TEST
  BdsSecurityLib|MainlineBootPkg/Library/BdsSecurityLib/BdsSecurityLib.inf
  BaseCryptLib|CryptoPkg/Library/BaseCryptLib/BaseCryptLib.inf
  OpensslLib|CryptoPkg/Library/OpensslLib/OpensslLibCrypto.inf
  IntrinsicLib|CryptoPkg/Library/IntrinsicLib/IntrinsicLib.inf
  SecurityManagementLib|MdeModulePkg/Library/DxeSecurityManagementLib/DxeSecurityManagementLib.inf
  PeCoffLib|MdePkg/Library/BasePeCoffLib/BasePeCoffLib.inf
  PeCoffExtraActionLib|MdePkg/Library/BasePeCoffExtraActionLibNull/BasePeCoffExtraActionLibNull.inf
  TpmMeasurementLib|MdeModulePkg/Library/TpmMeasurementLibNull/TpmMeasurementLibNull.inf
  TimerLib|ArmPkg/Library/ArmArchTimerLib/ArmArchTimerLib.inf
  ArmGenericTimerCounterLib|ArmPkg/Library/ArmGenericTimerPhyCounterLib/ArmGenericTimerPhyCounterLib.inf
  ArmLib|ArmPkg/Library/ArmLib/ArmBaseLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
!endif

[LibraryClasses.AARCH64]
  CompilerIntrinsicsLib|ArmPkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf
  NULL|ArmPkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf

[BuildOptions.common]
  GCC:*_CLANG35_*_CC_FLAGS = -Wno-unused-but-set-variable -Wno-unaligned-access
  GCC:*_*_*_ARCHCC_FLAGS = -fno-stack-protector -fno-common -mno-outline-atomics -Wno-unknown-warning-option -Wno-varargs -Wno-misleading-indentation -Wno-shift-negative-value
  GCC:*_*_*_DLINK_FLAGS = -Wl,-Ttext=0x0 $(CLANG_EXTRA_DLINK_FLAGS)

!ifdef SB_CHALLENGE_TEST
[PcdsFixedAtBuild]
  gEfiSecurityPkgTokenSpaceGuid.PcdOptionRomImageVerificationPolicy|0x04
  gEfiSecurityPkgTokenSpaceGuid.PcdRemovableMediaImageVerificationPolicy|0x04
  gEfiSecurityPkgTokenSpaceGuid.PcdFixedMediaImageVerificationPolicy|0x04
!endif

[Components.common]
  MainlineBootPkg/Tests/SecureBoot/SecureBootDirect.inf
  MainlineBootPkg/Tests/SecureBoot/SecureBootProbe.inf
  MainlineBootPkg/Tests/SecureBoot/SecureBootReturn.inf
  MainlineBootPkg/Tests/SecureBoot/RuntimeProbe.inf
  MainlineBootPkg/Tests/SecureBoot/DisabledProbe.inf
!ifdef SB_CHALLENGE_TEST
  MainlineBootPkg/Tests/SecureBoot/ChallengeProbe.inf {
    <LibraryClasses>
      NULL|SecurityPkg/Library/DxeImageVerificationLib/DxeImageVerificationLib.inf
  }
!endif
