import math
import struct
import unittest
from compare import compare, pq_encode


class PixelComparisonTests(unittest.TestCase):
    def test_identical_rgb_values_pass(self):
        data = struct.pack('<II', 0xc0000000, 0xffffffff)
        self.assertTrue(compare(data, data)['passed'])

    def test_alpha_does_not_change_rgb_comparison(self):
        self.assertTrue(compare(struct.pack('<I', 0xc0000000), struct.pack('<I', 0))['passed'])

    def test_single_channel_error_is_not_hidden_by_frame_average(self):
        data = struct.pack('<' + 'I'*100, *([0]*99 + [1]))
        result = compare(data, bytes(400))
        self.assertFalse(result['passed'])
        self.assertEqual(result['channels_outside_tolerance'], 1)
        self.assertEqual(result['max_code_error'], 1)
        self.assertTrue(compare(data, bytes(400), max_code_error=1)['passed'])

    def test_rejects_bad_frame_lengths(self):
        for a, b in [(b'', b''), (b'a', b'a'), (bytes(4), bytes(8))]:
            with self.assertRaises(ValueError):
                compare(a, b)

    def test_rejects_invalid_tolerances(self):
        for tolerance in [-1, math.nan, math.inf]:
            with self.assertRaises(ValueError):
                compare(bytes(4), bytes(4), max_code_error=tolerance)

    def test_pq_known_luminance_values(self):
        self.assertAlmostEqual(pq_encode(100), 0.5080784215, places=9)
        self.assertAlmostEqual(pq_encode(1000), 0.7518270962, places=9)
        self.assertAlmostEqual(pq_encode(10000), 1.0, places=12)

    def test_windows_neutral_scrgb_normalizes_to_pq(self):
        # 12.5 scRGB * 80 nits = 1000 nits, neutral remains neutral in Rec.2020.
        code = round(pq_encode(1000)*1023)
        candidate = struct.pack('<I', code | code << 10 | code << 20)
        reference = struct.pack('<eeee', 12.5, 12.5, 12.5, 1)
        self.assertTrue(compare(candidate, reference, 'scrgb-f16', 0.5)['passed'])

    def test_rejects_nonfinite_windows_reference(self):
        with self.assertRaises(ValueError):
            compare(bytes(4), struct.pack('<eeee', math.nan, 0, 0, 1), 'scrgb-f16')


if __name__ == '__main__':
    unittest.main()
