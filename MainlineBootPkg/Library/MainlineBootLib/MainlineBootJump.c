/** @file MainlineBootJump.c

  Kernel handoff through the Linux EFI stub (replaces the ABL bare jump:
  ExitBootServices + cache/MMU quiesce + jump with x0 = DTB).

  The kernel Image (a PE/COFF binary when built with CONFIG_EFI_STUB) is
  started like any other EFI application:
    - the patched DTB is published in the EFI configuration table
      (gFdtTableGuid) and the initrd through a LoadFile2 protocol on the
      LINUX_EFI_INITRD_MEDIA_GUID vendor media device path;
    - LoadImage()/StartImage() enter the stub, which does its own kernel
      relocation, initrd load, FDT rebuild, ExitBootServices and cache/MMU
      quiesce, and never returns.

  Everything below runs while boot services are alive; shutting them down
  is the stub's job (it needs them).

  SPDX-License-Identifier: BSD-3-Clause-Clear
**/

#include "MainlineBootInternal.h"
#include <Protocol/DevicePath.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/LoadFile2.h>
#include <Guid/Fdt.h>

/* ------------------------------------------------------------------------ */
/* display: GOP lookup and detach                                            */
/* ------------------------------------------------------------------------ */

/* ABL DrawUI order: console-out handle first, then any GOP. */
EFI_GRAPHICS_OUTPUT_PROTOCOL *
MlbLocateGop (
  VOID
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop = NULL;

  if (gST != NULL && gST->ConsoleOutHandle != NULL) {
    gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid,
                         (VOID **)&Gop);
  }
  if (Gop == NULL) {
    gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  }
  return Gop;
}

/**
  Remove every EFI_GRAPHICS_OUTPUT_PROTOCOL instance before StartImage().

  The stub's setup_primary_display() enumerates GOP handles and hands the
  framebuffer to the kernel as screen_info; the kernel then runs efifb and
  fbcon on ABL's live scanout buffer until msm DRM evicts them. The bare-jump
  path never exposed that buffer (the simple-framebuffer handoff is disabled
  for exactly this reason: panel abnormal states), and the display plays no
  part in booting - msm DRM initialises the panel from scratch. With the
  protocol gone the stub finds no display and the kernel starts without a
  framebuffer, as before.

  The firmware text console is a GOP consumer and is disconnected as a side
  effect (UninstallProtocolInterface detaches BY_DRIVER consumers; if that
  is refused the controller is disconnected explicitly and the removal
  retried), so nothing can be printed to the panel afterwards. Failure is
  reported, not fatal: the kernel then merely sees the framebuffer again.
**/
EFI_STATUS
MlbDetachGop (
  VOID
  )
{
  EFI_STATUS   Status;
  EFI_HANDLE  *Handles = NULL;
  UINTN        Count = 0;
  UINTN        Removed = 0;
  UINTN        i;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiGraphicsOutputProtocolGuid,
                                    NULL, &Count, &Handles);
  if (Status == EFI_NOT_FOUND) {
    return EFI_SUCCESS;
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "MainlineBoot: GOP handle enumeration: %r\n", Status));
    return Status;
  }

  for (i = 0; i < Count; i++) {
    VOID  *Gop = NULL;

    if (EFI_ERROR (gBS->HandleProtocol (Handles[i], &gEfiGraphicsOutputProtocolGuid,
                                        &Gop))) {
      continue;
    }
    Status = gBS->UninstallProtocolInterface (Handles[i],
                                              &gEfiGraphicsOutputProtocolGuid, Gop);
    if (Status == EFI_ACCESS_DENIED) {
      gBS->DisconnectController (Handles[i], NULL, NULL);
      Status = gBS->UninstallProtocolInterface (Handles[i],
                                                &gEfiGraphicsOutputProtocolGuid, Gop);
    }
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "MainlineBoot: GOP uninstall on handle %u: %r\n",
              (UINT32)i, Status));
    } else {
      Removed++;
    }
  }
  FreePool (Handles);

  DEBUG ((EFI_D_INFO, "MainlineBoot: GOP detached: %u of %u instance(s)\n",
          (UINT32)Removed, (UINT32)Count));
  return Removed == Count ? EFI_SUCCESS : EFI_ACCESS_DENIED;
}

/* ------------------------------------------------------------------------ */
/* initrd: LINUX_EFI_INITRD_MEDIA_GUID LoadFile2                             */
/* ------------------------------------------------------------------------ */

/* include/linux/efi.h LINUX_EFI_INITRD_MEDIA_GUID - not in this edk2
 * (codelinaro uefi.lnx.6.0.r49) yet, hence the local definition. */
#define MLB_LINUX_EFI_INITRD_MEDIA_GUID \
  { 0x5568e427, 0x68fc, 0x4f3d, { 0xac, 0x74, 0xca, 0x55, 0x52, 0x31, 0xcc, 0x68 } }

/* The device path the stub looks up via LocateDevicePath
 * (libstub initrd_dev_path: vendor media node + end entire node). */
#pragma pack(1)
typedef struct {
  VENDOR_DEVICE_PATH        Vendor;
  EFI_DEVICE_PATH_PROTOCOL  End;
} MLB_INITRD_DEVICE_PATH;
#pragma pack()

STATIC CONST MLB_INITRD_DEVICE_PATH  mMlbInitrdDevicePath = {
  {
    {
      MEDIA_DEVICE_PATH,
      MEDIA_VENDOR_DP,
      { (UINT8)sizeof (VENDOR_DEVICE_PATH),
        (UINT8)(sizeof (VENDOR_DEVICE_PATH) >> 8) }
    },
    MLB_LINUX_EFI_INITRD_MEDIA_GUID
  },
  {
    END_DEVICE_PATH_TYPE,
    END_ENTIRE_DEVICE_PATH_SUBTYPE,
    { (UINT8)sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
  }
};

/* Note: no const on This/FilePath - the EFI_LOAD_FILE2 typedef in this
 * edk2 (2018/codelinaro) declares them without const, and -Werror rejects
 * the mismatched function pointer type. */
STATIC
EFI_STATUS
EFIAPI
MlbInitrdLoadFile2 (
  IN EFI_LOAD_FILE2_PROTOCOL    *This,
  IN EFI_DEVICE_PATH_PROTOCOL   *FilePath,
  IN BOOLEAN                     BootPolicy,
  IN OUT UINTN                  *BufferSize,
  IN OUT VOID                   *Buffer OPTIONAL
  );

STATIC EFI_LOAD_FILE2_PROTOCOL  mMlbInitrdLoadFile2 = {
  MlbInitrdLoadFile2
};

STATIC
EFI_STATUS
EFIAPI
MlbInitrdLoadFile2 (
  IN EFI_LOAD_FILE2_PROTOCOL    *This,
  IN EFI_DEVICE_PATH_PROTOCOL   *FilePath,
  IN BOOLEAN                     BootPolicy,
  IN OUT UINTN                  *BufferSize,
  IN OUT VOID                   *Buffer OPTIONAL
  )
{
  if (This != &mMlbInitrdLoadFile2 || BufferSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (BootPolicy) {
    return EFI_UNSUPPORTED;
  }
  /* the protocol is only installed while an initrd is loaded */
  if (gMlb.Initrd == NULL || gMlb.InitrdSize == 0) {
    return EFI_NOT_FOUND;
  }

  /* efi_load_initrd_dev_path() probes with *BufferSize = 0 / Buffer = NULL
   * and requires EFI_BUFFER_TOO_SMALL (anything else is EFI_LOAD_ERROR),
   * then allocates that many pages and loads in one call. */
  if (Buffer == NULL || *BufferSize < gMlb.InitrdSize) {
    *BufferSize = gMlb.InitrdSize;
    return EFI_BUFFER_TOO_SMALL;
  }

  CopyMem (Buffer, gMlb.Initrd, gMlb.InitrdSize);
  *BufferSize = gMlb.InitrdSize;
  return EFI_SUCCESS;
}

/**
  Publish gMlb.Initrd on a new handle carrying the initrd device path and
  an EFI_LOAD_FILE2_PROTOCOL. The stub pulls the data through two LoadFile()
  calls before its ExitBootServices, so serving straight from the
  library-owned pool buffer is safe (no staging copy needed).

  The LINUX_EFI_INITRD_MEDIA_GUID configuration table entry itself is
  installed by the stub, pointing at its own linux_efi_initrd descriptor;
  installing it here would leave a handle pointer the kernel could misread
  as struct linux_efi_initrd when the stub fails to replace it.

  No-op when no initrd is loaded.
**/
EFI_STATUS
MlbInstallInitrdMedia (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  Handle = NULL;

  if (gMlb.Initrd == NULL || gMlb.InitrdSize == 0) {
    return EFI_SUCCESS;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Handle,
                  &gEfiDevicePathProtocolGuid,
                  (VOID *)&mMlbInitrdDevicePath,
                  &gEfiLoadFile2ProtocolGuid,
                  &mMlbInitrdLoadFile2,
                  NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR,
            "MainlineBoot: initrd LoadFile2 protocol install failed: %r\n",
            Status));
    return Status;
  }

  DEBUG ((EFI_D_INFO,
          "MainlineBoot: initrd: %lu bytes via LINUX_EFI_INITRD_MEDIA_GUID LoadFile2\n",
          (UINT64)gMlb.InitrdSize));
  return EFI_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* DTB: EFI configuration table                                              */
/* ------------------------------------------------------------------------ */

/**
  Publish the patched DTB as the gFdtTableGuid configuration table entry
  (get_fdt() in the stub). Replaces the entry when the firmware already
  published one. Fdt must live in pool (EfiLoaderData) memory: the stub
  copies it into its own allocation only after allocating the kernel and
  the initrd, and raw window memory could be handed out by then.
**/
EFI_STATUS
MlbInstallFdtConfigTable (
  IN VOID  *Fdt
  )
{
  EFI_STATUS  Status;

  Status = gBS->InstallConfigurationTable (&gFdtTableGuid, Fdt);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR,
            "MainlineBoot: FDT configuration table install failed: %r\n",
            Status));
    return Status;
  }

  DEBUG ((EFI_D_INFO,
          "MainlineBoot: DTB 0x%016lx published in the EFI configuration table\n",
          (UINT64)(UINTN)Fdt));
  return EFI_SUCCESS;
}


/* ------------------------------------------------------------------------ */
/* kernel: LoadImage + StartImage                                            */
/* ------------------------------------------------------------------------ */

#define MLB_PE_SIGNATURE            0x00004550       /* "PE\0\0"           */
#define MLB_IMAGE_FILE_MACHINE_ARM64 0xaa64
#define MLB_IMAGE_FILE_HEADER_SIZE  20               /* after the signature */

/**
  TRUE when the loaded arm64 Image carries a PE/COFF header for an ARM64
  machine, i.e. the kernel was built with CONFIG_EFI_STUB. Checked up front
  so a plain Image fails with a clear message instead of a bare
  LoadImage() EFI_UNSUPPORTED.
**/
BOOLEAN
MlbKernelHasEfiStub (
  IN CONST VOID  *Kernel,
  IN UINTN        Size
  )
{
  MLB_ARM64_IMAGE_HEADER  Hdr;
  UINT32                  PeOff;
  UINT32                  Signature;
  UINT16                  Machine;
  CONST UINT8            *Pe;

  if (Kernel == NULL || Size < MLB_ARM64_IMAGE_HDR_SIZE) {
    return FALSE;
  }

  /* Res5 (offset 0x3c) is the PE/COFF header offset ("MZ" e_lfanew) */
  CopyMem (&Hdr, Kernel, sizeof (Hdr));
  PeOff = Hdr.Res5;
  if (PeOff < MLB_ARM64_IMAGE_HDR_SIZE ||
      PeOff + 4 + MLB_IMAGE_FILE_HEADER_SIZE > Size) {
    return FALSE;
  }

  Pe = (CONST UINT8 *)Kernel + PeOff;
  CopyMem (&Signature, Pe, sizeof (Signature));
  if (Signature != MLB_PE_SIGNATURE) {
    return FALSE;
  }

  CopyMem (&Machine, Pe + 4, sizeof (Machine));   /* IMAGE_FILE_HEADER.Machine */
  return Machine == MLB_IMAGE_FILE_MACHINE_ARM64;
}

/**
  Load the kernel Image as an EFI application and start it. Any return is a
  failure: the stub returns a status only on its pre-ExitBootServices error
  paths, and on success it enters the kernel for good.

  CmdLine (optional, may be "") is passed as the image LoadOptions; the stub
  converts it to the kernel command line, parses it for its own parameters
  (quiet, initrd=, ...) and writes it into the new FDT /chosen/bootargs,
  overwriting the value MlbUpdateDeviceTree() patched in. An empty/absent
  CmdLine leaves LoadOptions NULL and the DTB bootargs in place.
**/
EFI_STATUS
MlbBootKernelEfiStub (
  IN CONST CHAR8  *CmdLine OPTIONAL
  )
{
  EFI_STATUS                 Status;
  EFI_HANDLE                 KernelImage = NULL;
  EFI_LOADED_IMAGE_PROTOCOL *LoadedImage;
  CHAR16                    *LoadOptions = NULL;
  UINTN                      Len, i;

  /* LoadImage(): PE/COFF validation + allocation + relocation into an
   * EfiLoaderCode image. Not optional: the arm64 stub reads its own
   * image_base from the LoadedImage protocol. */
  Status = gBS->LoadImage (FALSE, gImageHandle, NULL, gMlb.Kernel,
                           gMlb.KernelSize, &KernelImage);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "MainlineBoot: LoadImage(kernel) failed: %r\n",
            Status));
    return Status;
  }

  if (CmdLine != NULL && CmdLine[0] != '\0') {
    Len = AsciiStrLen (CmdLine);
    LoadOptions = AllocateZeroPool ((Len + 1) * sizeof (CHAR16));
    if (LoadOptions == NULL) {
      gBS->UnloadImage (KernelImage);
      return EFI_OUT_OF_RESOURCES;
    }
    for (i = 0; i < Len; i++) {
      LoadOptions[i] = (CHAR16)(UINT8)CmdLine[i];
    }

    Status = gBS->HandleProtocol (KernelImage, &gEfiLoadedImageProtocolGuid,
                                  (VOID **)&LoadedImage);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR,
              "MainlineBoot: kernel LoadedImage protocol: %r\n", Status));
      FreePool (LoadOptions);
      gBS->UnloadImage (KernelImage);
      return Status;
    }
    LoadedImage->LoadOptions = LoadOptions;
    LoadedImage->LoadOptionsSize = (UINT32)((Len + 1) * sizeof (CHAR16));
  }

  DEBUG ((EFI_D_INFO,
          "MainlineBoot: starting kernel EFI stub (LoadOptions: \"%a\")\n",
          (CmdLine != NULL && CmdLine[0] != '\0') ? CmdLine : "(dtb bootargs)"));

  /* last boot-service-level step: the firmware console goes with the GOP */
  Status = MlbDetachGop ();
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_WARN,
            "MainlineBoot: GOP still present (%r): kernel will see efifb\n",
            Status));
  }

  /* StartImage(): the stub relocates the kernel, pulls the initrd through
   * the LoadFile2 handle, rebuilds the FDT from the configuration table
   * copy, calls ExitBootServices and jumps - no return on success. */
  Status = gBS->StartImage (KernelImage, NULL, NULL);

  DEBUG ((EFI_D_ERROR,
          "MainlineBoot: kernel EFI stub returned: %r\n", Status));
  if (LoadOptions != NULL) {
    FreePool (LoadOptions);
  }
  gBS->UnloadImage (KernelImage);
  return EFI_ERROR (Status) ? Status : EFI_LOAD_ERROR;
}
