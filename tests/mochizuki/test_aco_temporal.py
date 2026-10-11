"""Reject vacuous scheduling comparisons without requiring a GPU/model."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('aco_temporal', Path(__file__).with_name('aco_temporal.py'))
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


class TemporalFixtureTests(unittest.TestCase):
    def test_empty_selection_is_rejected(self):
        matrix = list(fixture.cases())
        for start, single in ((0, None), (len(matrix)+1, None), (2, 1), (1, 0)):
            with self.subTest(start=start, single=single), self.assertRaises(ValueError):
                fixture.select_cases(matrix, start, single)
        self.assertEqual(len(fixture.select_cases(matrix, 1)), 45)
        self.assertEqual([n for n, _ in fixture.select_cases(matrix, 1, 9)], [9])

    def test_inherited_diagnostics_cannot_disable_overlap(self):
        with patch.dict(fixture.os.environ, {'NR_TCHAIN': '0', 'NR_TCHAIN_KINDS': '0',
                        'NR_TCHAIN_C512': '0', 'NR_ACO_NATIVE_KERNELS': '*',
                        'MZ_TEST_REJECT_ACO_DEVICE': '1', 'PATH': 'retained'}):
            env = fixture.controlled_environment()
        self.assertEqual(env['NR_TCHAIN'], '1')
        self.assertEqual(env['NR_TCHAIN_KINDS'], '0x1ff')
        self.assertEqual(env['NR_TCHAIN_C512'], '0xf')
        self.assertNotIn('NR_ACO_NATIVE_KERNELS', env)
        self.assertNotIn('MZ_TEST_REJECT_ACO_DEVICE', env)
        self.assertEqual(env['PATH'], 'retained')


if __name__ == '__main__':
    unittest.main()
