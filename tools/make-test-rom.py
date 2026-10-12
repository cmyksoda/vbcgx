#!/usr/bin/env python3
"""Make an original V810 diagnostic cartridge, with no commercial game data."""
import struct
import sys
from pathlib import Path
rom = bytearray(65536)
code = bytearray()
def op(opcode, src, dst, imm=None):
    code.extend(struct.pack('<H', (opcode << 10) | (dst << 5) | src))
    if imm is not None:
        code.extend(struct.pack('<H', imm & 65535))
# Fill both framebuffer pairs with shade 3; keep VIP drawing disabled.
op(0x10, 0, 1)
op(0x10, 31, 2)
op(0x2f, 0, 3, 2)
loop = len(code)
op(0x37, 1, 2, 0)
op(0x11, 4, 1)
op(0x03, 3, 1)
code.extend(struct.pack('<H', 0x8200 | ((loop - len(code)) & 511)))
# Display brightness registers at 0x5f824/26/28.
op(0x2f, 0, 1, 5)
op(0x2c, 1, 1, 0xf800)
for register, value in [(0x24, 32), (0x26, 64), (0x28, 128)]:
    op(0x28, 0, 2, value)
    op(0x35, 1, 2, register)
# An SRAM marker makes persistence tests observable.
op(0x2f, 0, 1, 0x0600)
op(0x28, 0, 2, 0x4758)
op(0x35, 1, 2, 0)
code.extend(struct.pack('<H', 0x8a00))
rom[:len(code)] = code
# Reset vector jumps to the beginning of the cartridge.
rom[-16:-10] = struct.pack('<HHH', (0x2f << 10) | (5 << 5), 0x0700, (0x06 << 10) | 5)
Path(sys.argv[1]).write_bytes(rom)
