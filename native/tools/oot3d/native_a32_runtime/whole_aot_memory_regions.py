"""Conservative instruction-shape proofs for optional AOT memory regions.

No title addresses: selection belongs to the caller's pilot manifest.
"""
from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class CarriedPairCopy:
    source: int
    destination: int
    count: int
    carried: int
    scratch: int


def carried_pair_copy(block) -> CarriedPairCopy | None:
    """Recognize a six-instruction, self-looping LDR/STR carried pair copy."""
    if len(block.instructions) != 6:
        return None
    a, b, c, d, e, branch = (item.raw for item in block.instructions)
    src, scratch = (a >> 16) & 15, (a >> 12) & 15
    dst, carry = (b >> 16) & 15, (b >> 12) & 15
    count = (d >> 16) & 15
    if len({src, scratch, dst, carry, count}) != 5 or 15 in {src, scratch, dst, carry, count}:
        return None
    if (a != (0xE5B00008 | (src << 16) | (scratch << 12)) or
        b != (0xE5800004 | (dst << 16) | (carry << 12)) or
        c != (0xE5900004 | (src << 16) | (carry << 12)) or
        d != (0xE2500001 | (count << 16) | (count << 12)) or
        e != (0xE5A00008 | (dst << 16) | (scratch << 12)) or
        branch >> 24 != 0x1A):
        return None
    immediate = branch & 0xFFFFFF
    if immediate & 0x800000:
        immediate -= 0x1000000
    if block.instructions[-1].pc + 8 + immediate * 4 != block.pc:
        return None
    return CarriedPairCopy(src, dst, count, carry, scratch)


def region_memory_safe(function) -> bool:
    # Atomic/exclusive/swap and unknown memory-bus operations must retain their
    # ordinary writer-generation and observer boundaries.
    for block in function.blocks:
        for item in block.instructions:
            raw = item.raw
            if ((raw & 0x0F000FF0) == 0x01000F90 or  # LDREX/STREX family
                (raw & 0x0FB00FF0) == 0x01000090 or  # SWP/SWPB
                (raw & 0x0F000010) == 0x0E000010):  # system/barrier
                return False
    return True
