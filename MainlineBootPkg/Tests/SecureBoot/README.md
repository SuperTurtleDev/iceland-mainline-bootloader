# Secure Boot integration tests

These tests replace AAVMF's BDS with a fixed-entry launcher. AAVMF provides the
real authenticated-variable backend; production BdsApp supplies image policy.
The launcher installs always-rejecting Security and Security2 callbacks and
proves that they reject file and pathless-buffer loads before starting BdsApp.
Each probe checks that both counters stay at that baseline after BdsApp installs
its own callbacks; direct legacy Security calls must succeed without calling
firmware. A test-only QCOM PlatformBds trap fails on any security initialization
call. No variable-service function or SecureBoot status is spoofed.
The fixture uses a private GPT ESP and VARS copy. It never modifies the input
firmware or a physical device.

## Build and run

Initialize the OpenSSL submodule pinned by this repository's edk2 revision before
building BdsApp. Build the helpers in a prepared isolated edk2 workspace:

```sh
bash MainlineBootPkg/Tests/SecureBoot/build-tests.sh "$EDK_WORKSPACE" -D SB_CHALLENGE_TEST
python3 MainlineBootPkg/Tests/SecureBoot/run-qemu.py \
  --bds "$EDK_WORKSPACE/Build/MainlineBoot/RELEASE_CLANG35/AARCH64/BdsApp.efi" \
  --test-bin "$EDK_WORKSPACE/Build/SecureBootTest/RELEASE_CLANG35/AARCH64" \
  --mode disabled --output /tmp/bds-disabled-test
```

Requires QEMU AArch64, AAVMF_CODE.secboot.fd, openssl, sbsign, sbverify, efitools,
sfdisk, dosfstools and mtools. `--efitools-dir` can point to extracted local tools.
The output directory must be new. Repeat with separate output paths for each mode:

| Mode | Coverage |
| --- | --- |
| disabled | Leave PK/KEK/db/dbx absent in real SetupMode=1, SecureBoot=0. The unsigned default child, unsigned file and wrong-signed buffer execute through BdsApp's allow-all stubs despite the rejecting firmware callbacks. |
| images | Enroll real time-authenticated PK/KEK/db and verify exact bytes and attributes, reaching SetupMode=0, SecureBoot=1. Trusted images execute through the local verifier despite firmware rejection; unsigned, wrong-key and authenticated dbx-revoked images fail. Authenticated updates, wrong signers and replay are checked. |
| challenge | A separate probe initializes production BdsSecurityLib with BdsSecurityInitialize. Real db/dbx and test-only deterministic RNG cover RSA/CMS responses, replay, revocation, default-OFF and independent Fastboot switching. Unsigned images remain denied with SecureBoot=1 before and after authorization, Fastboot disable and database deletion. This is not a USB transport test. |
| runtime | Real ExitBootServices followed only by runtime services and direct serial output. The stored EnableSecureFastboot NV|BS setting cannot be read or modified, including attempts to add RT access. The fixture explicitly saves the Fastboot setting; default reads do not create it. |

Host tests:

```sh
python3 MainlineBootPkg/Library/BdsSecurityLib/TestSecurity/run.py
python3 MainlineBootPkg/Library/BdsSecurityLib/TestChallenge/run.py
python3 MainlineBootPkg/Tests/BdsApp/test-storage-policy.py
```

The initialization tests cover recursion, cached results, mode mismatch, malformed
settings, missing keys, and a firmware that rejects all variable writes while
Secure Boot is disabled. The challenge test runs production code with edk2 crypto
sources and OpenSSL under ASan/UBSan. The UI policy test extracts production guard
functions and verifies OFF permits RO/RW, ON rejects exports, and setting errors
are not silently converted into read-only operation.

The separate `../SecureBootSct` runner executes official unmodified UEFI SCT
assertions. These integration tests are not SCT/CTS certification. Preserve real
SCT failures and original logs. QEMU does not validate Qualcomm USB hardware,
secapp persistence or device-specific mode transitions.
