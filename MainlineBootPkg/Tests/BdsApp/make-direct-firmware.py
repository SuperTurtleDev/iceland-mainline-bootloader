#!/usr/bin/env python3
"""Replace only AAVMF's BdsDxe PE image with the fixed-entry test driver.

The input firmware is never modified. Reject unfamiliar layouts rather than
patching guessed offsets. Standard firmware BDS code is absent from the output.
"""
import argparse
import hashlib
import json
import lzma
import struct
import uuid
from pathlib import Path

LZMA_GUID = uuid.UUID("ee4e5898-3914-4259-9d6e-dc7bd79403cf").bytes_le
BDS_GUID = uuid.UUID("6d33944a-ec75-4855-a54d-809c75241f6c").bytes_le


def size24(data, offset):
    return int.from_bytes(data[offset:offset + 3], "little")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("firmware", type=Path)
    parser.add_argument("driver", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.exists() or args.output.resolve() == args.firmware.resolve():
        raise SystemExit("output must be a new file")
    firmware = bytearray(args.firmware.read_bytes())
    driver = args.driver.read_bytes()
    matches = []
    cursor = 0
    while True:
        guid = firmware.find(LZMA_GUID, cursor)
        if guid < 0:
            break
        cursor = guid + 16
        section = guid - 4
        if section < 24 or firmware[section + 3] != 2:
            continue
        length = size24(firmware, section)
        data_offset, attributes = struct.unpack_from("<HH", firmware, section + 20)
        if not (24 <= data_offset < length and section + length <= len(firmware)):
            continue
        try:
            unpacked = bytearray(lzma.decompress(firmware[section + data_offset:section + length], format=lzma.FORMAT_ALONE))
        except lzma.LZMAError:
            continue
        bds = unpacked.find(BDS_GUID)
        if bds >= 0 and unpacked[bds + 18] == 7:
            matches.append((section, length, data_offset, unpacked, bds))
    if len(matches) != 1:
        raise SystemExit(f"expected one LZMA volume containing BdsDxe; found {len(matches)}")
    section, length, data_offset, unpacked, bds = matches[0]
    bds_size = size24(unpacked, bds + 20)
    if bds_size == 0xFFFFFF or bds + bds_size > len(unpacked):
        raise SystemExit("unsupported BDS FFS size")
    cursor = bds + 24
    replaced = 0
    old_hash = None
    while cursor + 4 <= bds + bds_size:
        section_size = size24(unpacked, cursor)
        if section_size < 4 or cursor + section_size > bds + bds_size:
            raise SystemExit("invalid BDS section")
        if unpacked[cursor + 3] == 0x10:
            capacity = section_size - 4
            if len(driver) > capacity:
                raise SystemExit(f"direct-entry driver {len(driver)} exceeds original PE slot {capacity}")
            old_hash = hashlib.sha256(unpacked[cursor + 4:cursor + section_size]).hexdigest()
            unpacked[cursor + 4:cursor + section_size] = driver + bytes(capacity - len(driver))
            replaced += 1
        cursor = (cursor + section_size + 3) & ~3
    if replaced != 1:
        raise SystemExit("expected exactly one BDS PE section")
    if unpacked[bds + 19] & 0x40:
        unpacked[bds + 17] = (-sum(unpacked[bds + 24:bds + bds_size])) & 0xFF
    compressed = bytearray(lzma.compress(bytes(unpacked), format=lzma.FORMAT_ALONE,
                                         filters=[{"id": lzma.FILTER_LZMA1, "dict_size": 1 << 24}]))
    # EDK II's guided decoder requires a known uncompressed size.
    compressed[5:13] = len(unpacked).to_bytes(8, "little")
    new_section_size = data_offset + len(compressed)
    ffs = section - 24
    old_ffs_size = size24(firmware, ffs + 20)
    if firmware[ffs + 18] != 0x0B or old_ffs_size != 24 + length:
        raise SystemExit("unsupported outer volume-image FFS")
    outer_fv = firmware.rfind(b"_FVH", 0, ffs) - 40
    fv_length = struct.unpack_from("<Q", firmware, outer_fv + 32)[0]
    fv_end = outer_fv + fv_length
    old_end = (ffs + old_ffs_size + 7) & ~7
    if fv_end > len(firmware) or any(x != 0xFF for x in firmware[old_end:fv_end]):
        raise SystemExit("volume-image file is not followed only by free space")
    new_ffs_size = 24 + new_section_size
    if ffs + new_ffs_size > fv_end or new_ffs_size >= 0xFFFFFF:
        raise SystemExit("replacement firmware volume does not fit")
    section_header = bytearray(firmware[section:section + data_offset])
    section_header[:3] = new_section_size.to_bytes(3, "little")
    body = section_header + compressed
    header = bytearray(firmware[ffs:ffs + 24])
    header[20:23] = new_ffs_size.to_bytes(3, "little")
    header[16] = 0
    header[17] = (-sum(body)) & 0xFF if header[19] & 0x40 else 0xAA
    header[16] = (-(sum(header) - header[17] - header[23])) & 0xFF
    firmware[ffs:fv_end] = header + body + b"\xff" * (fv_end - ffs - new_ffs_size)
    args.output.write_bytes(firmware)
    report = {"source": str(args.firmware.resolve()), "source_sha256": hashlib.sha256(args.firmware.read_bytes()).hexdigest(),
              "driver_sha256": hashlib.sha256(driver).hexdigest(), "replaced_bds_pe_sha256": old_hash,
              "output_sha256": hashlib.sha256(firmware).hexdigest(), "standard_bds_replaced": True}
    args.output.with_suffix(args.output.suffix + ".json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
