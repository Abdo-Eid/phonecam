import csv
import tempfile
import unittest
from pathlib import Path

from analyze_receipt import summarize


class ReceiptTests(unittest.TestCase):
    def test_mf_sample_age_is_not_file_age(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'format.txt').write_text('consumer=test\nqpc_frequency=1000000\nempty_reads=0\nread_delay_ms=100\n')
            with (root / 'receipt.csv').open('w', newline='') as output:
                writer = csv.writer(output)
                writer.writerow(['frame','arrival_qpc','arrival_unix_us','sample_time_100ns',
                                 'flags','bytes','image','mf_receipt_100ns'])
                writer.writerow([1,1000000,0,10000000,0,100,'',10100000])
                writer.writerow([2,1100000,0,11000000,0,100,'',11100000])
            result = summarize(root)
            self.assertEqual(result['mf_sample_to_receipt_us']['p50'], 10000)
            self.assertEqual(result['read_delay_ms'], 100)

    def test_receipt_timing_and_optical_join_ignore_image_write_times(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'format.txt').write_text('consumer=test\nqpc_frequency=1000000\nempty_reads=0\n')
            with (root / 'receipt.csv').open('w', newline='') as output:
                writer = csv.writer(output)
                writer.writerow(['frame', 'arrival_qpc', 'arrival_unix_us',
                                 'sample_time_100ns', 'flags', 'bytes', 'image'])
                writer.writerow([1, 1000000, 1234567890123000, 0, 0, 100, 'sample-1.bmp'])
                writer.writerow([2, 1033333, 1234567890156333, 333333, 0, 100, ''])
                writer.writerow([3, 1066666, 1234567890189666, 666666, 0, 100, ''])
            readings = root / 'timer.csv'
            readings.write_text('frame,timer_unix_ms\n1,1234567890000\n')
            result = summarize(root, readings)
            self.assertAlmostEqual(result['receipt_fps'], 30, places=2)
            self.assertEqual(result['visual_age_us']['p50'], 123000)
            self.assertEqual(result['duplicate_sample_times'], 0)
            self.assertEqual(result['backward_sample_times'], 0)
            self.assertEqual(result['saved_images'], 1)


if __name__ == '__main__':
    unittest.main()
