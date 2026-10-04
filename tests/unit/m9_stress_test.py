from array import array
from pathlib import Path
import sys
import tempfile
import unittest
import wave

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.datasets.prepare_m9_stress import make
from tools.datasets.telephone import read_pcm16


class M9StressTest(unittest.TestCase):
    def test_schedule_contains_interruption_and_exact_silence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.wav"
            with wave.open(str(source), "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(16000)
                wav.writeframes(array("h", [3000] * 16000).tobytes())
            report = make(source, root / "stress", 4, 500)
            kinds = [segment["kind"] for segment in report["segments"]]
            self.assertEqual(kinds.count("interrupted_speech"), 1)
            self.assertEqual(kinds.count("digital_silence"), 4)
            self.assertEqual(report["samples"], 8000 + 3 * 16000 + 4 * 8000)
            self.assertEqual(len(read_pcm16(root / "stress/stress.wav")), report["samples"])
            with self.assertRaises(FileExistsError):
                make(source, root / "stress", 4, 500)


if __name__ == "__main__":
    unittest.main()
