#!/usr/bin/env python3
"""Compare captured native power-bomb HDMA against read-only ROM profiles.

Consumes WRAM dumps, not guest save states. Does not modify a running game.
Full native-table equality can still leave unclipped widths ambiguous.
"""
import argparse
import csv
import json
from pathlib import Path
import struct


def word(data, offset):
    return struct.unpack_from('<H', data, offset)[0]


def scaled(profile, radius):
    widths = [-1] * 192
    row = radius * profile[128] // 256
    if row >= len(widths):
        return None
    for point in range(96, 128):
        end = radius * profile[point + 32] // 256
        if end > row:
            return None
        width = radius * profile[point] // 256
        widths[end:row + 1] = [width] * (row - end + 1)
        row = end
    widths[:row + 1] = [width] * (row + 1)
    return widths


def clipped(center, width):
    if width < 0 or center + width < 0 or center - width > 255:
        return None
    return (max(0, center - width), min(255, center + width))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom', type=Path, required=True)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--wram', type=Path)
    source.add_argument('--capture', type=Path, help='version-2 renderer capture with pre-NMI state')
    parser.add_argument('--previous-wram', type=Path)
    parser.add_argument('--dma-trace', type=Path)
    args = parser.parse_args()
    rom = args.rom.read_bytes()
    previous = None
    frame = None
    if args.capture:
        capture = args.capture.read_bytes()
        if len(capture) < 12:
            parser.error('truncated renderer capture header')
        magic, version, size = struct.unpack_from('<III', capture)
        # Version 2 uses the native serialized SmSourceFrame layout.
        ram_offset = 12 + 224 * 66656
        owner_offset = ram_offset + 0x20000 + 256 * 224 * 4 + 9
        if magic != 0x534d5243 or version != 2 or len(capture) != size + 12 or len(capture) < owner_offset + 0x20001:
            parser.error('expected a complete version-2 renderer capture')
        ram = capture[ram_offset:ram_offset + 0x20000]
        previous = capture[owner_offset:owner_offset + 0x20000]
        frame = struct.unpack_from('<I', capture, owner_offset - 9)[0]
    else:
        ram = args.wram.read_bytes()
    if len(ram) != 0x20000 or len(rom) != 0x300000:
        parser.error('expected 128-KiB WRAM and unheadered 3-MiB ROM')
    center = word(ram, 0xce6) - 256
    actual = [None if ram[0xc406 + y] > ram[0xc506 + y] else
              (ram[0xc406 + y], ram[0xc506 + y]) for y in range(192)]
    profile = rom[0x42206:0x422a6]
    matches = []
    for radius in range(256):
        widths = scaled(profile, radius)
        if widths is not None and [clipped(center, width) for width in widths] == actual:
            matches.append(radius)
    pointer = word(ram, 0xcf2)
    preset_matches = []
    for address in (pointer, pointer - 192):
        if not 0x8000 <= address <= 0xff40:
            continue
        offset = 0x40000 + address - 0x8000
        widths = rom[offset:offset + 192]
        # On-screen preset generation stops at its first zero-width row.
        stop = next((i for i, width in enumerate(widths) if not width), 192)
        expected = [clipped(center, width if y < stop else -1) for y, width in enumerate(widths)]
        if expected == actual:
            preset_matches.append(f'{address:04X}')
    def fields(data):
        return dict(status=f'{word(data, 0x592):04X}', radius=word(data, 0xcea),
                    flash_radius=word(data, 0xcec), speed=word(data, 0xcf0),
                    preset_pointer=f'{word(data, 0xcf2):04X}',
                    preinstructions=[f'{word(data, 0x18f0 + i * 2):04X}' for i in range(6)],
                    table_pointers=[f'{word(data, 0x18d8 + i * 2):04X}' for i in range(6)])
    report = dict(source=str(args.capture or args.wram), frame=frame, center_x=center, stored=fields(ram),
                  native_nonempty_rows=sum(row is not None for row in actual),
                  scaled_radius_high_byte_matches=matches,
                  preset_pointer_matches=preset_matches,
                  caution='Native clipping may hide differences in wider shapes; no phase is inferred.')
    if args.previous_wram:
        previous = args.previous_wram.read_bytes()
    if previous is not None:
        if len(previous) != 0x20000:
            parser.error('previous WRAM must be 128 KiB')
        report['previous'] = fields(previous)
    if args.dma_trace:
        if frame is None:
            parser.error('DMA trace matching requires --capture')
        with args.dma_trace.open(newline='') as file:
            rows = [row for row in csv.reader(file) if int(row[0]) == frame and row[4] in ('28', '29')]
        report['dma_window2'] = [dict(channel=int(row[1]), bank=row[2], pointer=row[3],
                                     register=row[4], mode=int(row[5]), indirect=int(row[6]),
                                     indirect_bank=row[7], enabled_mask=row[8]) for row in rows]
        if len(rows) == 2 and all(row[2] == '89' and row[7] == '7e' for row in rows):
            raster_checks = []
            for scanline_offset in (0, 1):
                compared = mismatches = 0
                for y in range(32, 224):
                    for row in rows:
                        pointer = int(row[3], 16) + 3 * (y + scanline_offset)
                        if not 0x8000 <= pointer <= 0xfffd:
                            continue
                        offset = 0x48000 + pointer - 0x8000
                        if not 0 <= offset <= len(rom) - 3 or rom[offset] != 0x81:
                            continue
                        address = word(rom, offset + 1)
                        register_offset = 54 if row[4] == '28' else 55
                        compared += 1
                        mismatches += ram[address] != capture[12 + y * 66656 + register_offset]
                raster_checks.append(dict(scanline_offset=scanline_offset,
                                          compared_window_bytes=compared, mismatches=mismatches))
            report['latched_pointer_raster_checks'] = raster_checks
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
