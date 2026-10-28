import sys
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).parents[1]))
from nightly_linux_qualification import final_ctest_result

SKIPS = '\n141 - regression_playnite_connector_lifecycle (Skipped)\n155 - pyrowave_linux_gpu (Skipped)\n'

class QualificationTests(unittest.TestCase):
    def test_modern_and_legacy_summary_preserve_exact_skips(self):
        for summary in ('100% tests passed out of 155', '100% tests passed, 0 tests failed out of 155'):
            result = final_ctest_result(summary + SKIPS)
            self.assertEqual((result['total'],result['passed'],result['failed'],len(result['skipped'])), (155,153,0,2))

    def test_failed_partial_missing_or_extra_skips_rejected(self):
        for log in ('99% tests passed, 1 tests failed out of 155'+SKIPS,
                    '100% tests passed out of 12'+SKIPS,
                    '100% tests passed out of 155',
                    '100% tests passed out of 155'+SKIPS+'156 - hardware_other (Skipped)',
                    '155/155 Test #1: failed ***Failed\n100% tests passed out of 155'+SKIPS,
                    'cache saved'):
            with self.subTest(log=log), self.assertRaises(ValueError): final_ctest_result(log)

    def test_last_suite_must_pass_even_after_an_earlier_success(self):
        with self.assertRaises(ValueError):
            final_ctest_result('100% tests passed out of 155'+SKIPS+'99% tests passed, 1 tests failed out of 168'+SKIPS)

if __name__ == '__main__': unittest.main()
