import unittest
from whole_aot_cpp import Block, Function, Instruction, LoweringError, _render_region_function
from whole_aot_memory_regions import carried_pair_copy


def copy_fixture():
    raw = (0xE5B0C008, 0xE5813004, 0xE5903004, 0xE2522001, 0xE5A1C008, 0x1AFFFFF9)
    return Function(0x1000, "Copy", (
        Block(0, 0x1000, 0x1018,
              tuple(Instruction(0x1000 + i * 4, value) for i, value in enumerate(raw)),
              ({"kind": "branch", "target": 0x1000}, {"kind": "fallthrough", "target": 0x1018})),
        Block(1, 0x1018, 0x101C, (Instruction(0x1018, 0xE12FFF1E),), ({"kind": "return"},)),
    ), (), (0x1018,))


class MemoryRegionTests(unittest.TestCase):
    def test_recognizes_registers_and_rejects_each_mutated_opcode(self):
        from dataclasses import replace
        block = copy_fixture().blocks[0]
        self.assertEqual(tuple(vars(carried_pair_copy(block)).values()), (0, 1, 2, 3, 12))
        for i in range(6):
            instructions = list(block.instructions)
            instructions[i] = replace(instructions[i], raw=instructions[i].raw ^ 1)
            self.assertIsNone(carried_pair_copy(replace(block, instructions=tuple(instructions))))

    def test_rejects_exclusive_operations(self):
        function = Function(0x1000, "Exclusive", (Block(0, 0x1000, 0x1008,
            (Instruction(0x1000, 0xE1901F9F), Instruction(0x1004, 0xE12FFF1E)),
            ({"kind": "return"},)),), (), ())
        with self.assertRaises(LoweringError):
            _render_region_function(function, {}, frozenset(), frozenset(), frozenset({0x1000}))


if __name__ == '__main__':
    unittest.main()
