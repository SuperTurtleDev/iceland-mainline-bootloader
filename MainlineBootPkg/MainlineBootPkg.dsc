## @file MainlineBootPkg.dsc
#
#  Platform description for MainlineBootPkg (TestBootApp + MainlineBootLib).
#  Library class mapping follows QcomModulePkg.dsc so the resulting .efi runs
#  in the same UEFI environment as the stock ABL LinuxLoader.
#
#  Build:  build -p MainlineBootPkg/MainlineBootPkg.dsc -a AARCH64 -t CLANG35 -b DEBUG
#          (see MainlineBootPkg/build.sh for the required environment)
#
#  SPDX-License-Identifier: BSD-3-Clause-Clear
##

[Defines]
  PLATFORM_NAME                  = MainlineBootPkg
  PLATFORM_GUID                  = E67F32AF-515D-4075-8001-558FA050F2E4
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  # Overridable from the command line with -D ABL_OUT_DIR=<dir>
  DEFINE ABL_OUT_DIR             = Build/MainlineBoot
  OUTPUT_DIRECTORY               = $(ABL_OUT_DIR)
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = MainlineBootPkg/MainlineBootPkg.fdf

  # Compile-time switches (override with -D NAME=VALUE)
  # Product string reported by "fastboot getvar product".
  DEFINE BOARD_BOOTLOADER_PRODUCT_NAME = iceland
  # 1: strip /reserved-memory no-map regions out of the RAM partitions before
  #    writing /memory (same as ABL REMOVE_CARVEOUT_REGION=1 on canoe).
  DEFINE MAINLINE_REMOVE_CARVEOUT      = 1

[LibraryClasses.common]
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLibOptDxe/BaseMemoryLibOptDxe.inf
  # This (Qualcomm patched) MdePkg has several INFs consuming a library class
  # literally named "BaseMemoryLibOptDxe" (UefiLib, UefiMemoryAllocationLib,
  # DxeHobLib, BaseLib, ...). Map it to the same instance, as QcomModulePkg.dsc does.
  BaseMemoryLibOptDxe|MdePkg/Library/BaseMemoryLibOptDxe/BaseMemoryLibOptDxe.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  DebugLib|MdeModulePkg/Library/PeiDxeDebugLibReportStatusCode/PeiDxeDebugLibReportStatusCode.inf
  ReportStatusCodeLib|MdeModulePkg/Library/DxeReportStatusCodeLib/DxeReportStatusCodeLib.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
  DxeServicesTableLib|MdePkg/Library/DxeServicesTableLib/DxeServicesTableLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  FdtLib|EmbeddedPkg/Library/FdtLib/FdtLib.inf
  CacheMaintenanceLib|ArmPkg/Library/ArmCacheMaintenanceLib/ArmCacheMaintenanceLib.inf
  TimerLib|ArmPkg/Library/ArmArchTimerLib/ArmArchTimerLib.inf
  ArmGenericTimerCounterLib|ArmPkg/Library/ArmGenericTimerPhyCounterLib/ArmGenericTimerPhyCounterLib.inf
  MainlineBootLib|MainlineBootPkg/Library/MainlineBootLib/MainlineBootLib.inf

[LibraryClasses.AARCH64]
  ArmLib|ArmPkg/Library/ArmLib/ArmBaseLib.inf
  NULL|ArmPkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf

[BuildOptions.common]
  # No stack protector: we deliberately do not link QcomModulePkg's StackCanary
  # library (which provides __stack_chk_guard for ABL's -fstack-protector-all).
  # No CFI / BTI / SafeStack either.
  GCC:*_*_*_ARCHCC_FLAGS  = -fno-stack-protector -fno-common -Wno-unknown-warning-option -Wno-varargs -Wno-misleading-indentation -Wno-shift-negative-value
  GCC:*_*_*_DLINK_FLAGS   = -Wl,-Ttext=0x0
  # clang >= 17 needs these (see CLANG_NEED_EXTRA_DLINK_FLAGS in the top level
  # makefile). Left as a make variable so it expands from the environment
  # exported by build.sh, exactly like QcomModulePkg.dsc does.
  GCC:*_*_*_DLINK_FLAGS   = $(CLANG_EXTRA_DLINK_FLAGS)
  GCC:*_*_*_CC_FLAGS      = -DPRODUCT_NAME=\"$(BOARD_BOOTLOADER_PRODUCT_NAME)\"
  !if $(MAINLINE_REMOVE_CARVEOUT) == 1
  GCC:*_*_*_CC_FLAGS      = -DMAINLINE_REMOVE_CARVEOUT=1
  !endif

[PcdsFixedAtBuild.common]
  # DEBUG_ASSERT_ENABLED 0x01 | DEBUG_PRINT_ENABLED 0x02 | DEBUG_CODE_ENABLED 0x04
  # CLEAR_MEMORY_ENABLED 0x08 | ASSERT_DEADLOOP_ENABLED 0x20
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0x2f
  # DEBUG_ERROR | DEBUG_INFO | DEBUG_WARN | DEBUG_INIT | DEBUG_LOAD | DEBUG_FS
  gEfiMdePkgTokenSpaceGuid.PcdDebugPrintErrorLevel|0x8000004F
  gEfiMdePkgTokenSpaceGuid.PcdReportStatusCodePropertyMask|0x06

[Components.common]
  MainlineBootPkg/Application/TestBootApp/TestBootApp.inf
MainlineBootPkg/Application/BootApp/BootApp.inf
