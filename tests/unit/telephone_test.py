import math
from pathlib import Path
import sys
import tempfile
import unittest
import wave
from array import array

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.datasets.telephone import read_pcm16, simulate, write_telephone


class TelephoneTest(unittest.TestCase):
    def test_deterministic_and_sample_preserving(self):
        samples = array("h", [round(10000 * math.sin(2 * math.pi * 1000 * i / 16000))
                              for i in range(16001)])
        self.assertEqual(simulate(samples), simulate(samples))
        self.assertEqual(len(simulate(samples)), len(samples))
        self.assertNotEqual(simulate(samples), samples)

    def test_band_limits(self):
        def rms(frequency):
            samples = array("h", [round(10000 * math.sin(2 * math.pi * frequency * i / 16000))
                                  for i in range(16000)])
            result = simulate(samples)[200:-200]
            return math.sqrt(sum(value * value for value in result) / len(result))
        speech = rms(1000)
        self.assertGreater(speech, 5000)
        self.assertLess(rms(100), speech * .1)
        self.assertLess(rms(6000), speech * .1)

    def test_wav_rejects_unsupported_format_and_preserves_source(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.wav"
            output = Path(directory) / "telephone.wav"
            samples = array("h", range(101))
            with wave.open(str(source), "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(16000)
                wav.writeframes(samples.tobytes())
            before = source.read_bytes()
            self.assertEqual(write_telephone(source, output), 101)
            self.assertEqual(source.read_bytes(), before)
            self.assertEqual(len(read_pcm16(output)), 101)
            with self.assertRaises(ValueError):
                write_telephone(source, source)
            with wave.open(str(source), "wb") as wav:
                wav.setnchannels(1)
                wav.setsampwidth(2)
                wav.setframerate(8000)
                wav.writeframes(samples.tobytes())
            with self.assertRaises(ValueError):
                read_pcm16(source)


if __name__ == "__main__":
    unittest.main()
