import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('frame_report', Path(__file__).resolve().parents[1] / 'scripts/perf/frame_report.py')
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)

class FrameReportTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.csv = Path(self.folder.name) / 'frames.csv'
        self.csv.write_text('frame,elapsed_ms,interval_ms\n1,0,0\n2,10,10\n3,30,20\n4,60,30\n')

    def test_statistics(self):
        x = report.summarize(self.csv)
        self.assertEqual((x['frames'], x['average_fps'], x['median_ms'], x['p99_ms']), (3, 50, 20, 30))

    def test_only_complete_intervals_inside_window(self):
        self.assertEqual(report.summarize(self.csv, .01, .06)['frames'], 2)
        self.assertEqual(report.summarize(self.csv, .011, .06)['frames'], 1)

    def test_empty_window(self):
        with self.assertRaises(ValueError):
            report.summarize(self.csv, 1, 2)

    def test_nonfinite_intervals_are_excluded(self):
        with self.csv.open('a') as f:
            f.write('5,nan,10\n6,70,nan\n7,80,-1\n')
        self.assertEqual(report.summarize(self.csv)['frames'], 3)
