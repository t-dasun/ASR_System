"""Deterministic narrowband/G.711-style telephone simulation for PCM16 WAV.

The output stays 16 kHz mono PCM16 for the shared ASR runner. Processing is
300–3400 Hz Blackman-window FIR -> 8 kHz -> 8-bit mu-law -> 16 kHz interpolation.
No random noise, gain normalization, VAD, or inference is performed here.
"""
from array import array
from functools import lru_cache
import math
from pathlib import Path
import sys
import wave

SAMPLE_RATE = 16000
LOW_HZ = 300
HIGH_HZ = 3400
TAPS = 127
MU = 255
VERSION = "telephone_fir127_mulaw8k_v1"


def _sinc(value):
    return 1.0 if value == 0 else math.sin(math.pi * value) / (math.pi * value)


@lru_cache(maxsize=1)
def _coefficients():
    center = (TAPS - 1) // 2
    taps = []
    for index in range(TAPS):
        position = index - center
        high = 2 * HIGH_HZ / SAMPLE_RATE * _sinc(2 * HIGH_HZ * position / SAMPLE_RATE)
        low = 2 * LOW_HZ / SAMPLE_RATE * _sinc(2 * LOW_HZ * position / SAMPLE_RATE)
        window = .42 - .5 * math.cos(2 * math.pi * index / (TAPS - 1)) + \
                 .08 * math.cos(4 * math.pi * index / (TAPS - 1))
        taps.append((high - low) * window)
    gain = sum(tap * math.cos(2 * math.pi * 1000 * (index - center) / SAMPLE_RATE)
               for index, tap in enumerate(taps))
    if gain <= 0:
        raise RuntimeError("telephone FIR passband gain is invalid")
    return tuple(tap / gain for tap in taps)


def _mulaw_roundtrip(sample):
    amplitude = min(1.0, abs(sample) / 32768.0)
    compressed = math.log1p(MU * amplitude) / math.log1p(MU)
    level = min(127, round(compressed * 127))
    expanded = math.expm1(level / 127 * math.log1p(MU)) / MU
    return math.copysign(expanded * 32767, sample)


def simulate(samples):
    """Return the same sample count after a fixed 8 kHz telephone codec path."""
    if not samples:
        raise ValueError("telephone input must be nonempty")
    taps = _coefficients()
    center = (TAPS - 1) // 2
    length = len(samples)
    narrow = []
    for index in range(0, length, 2):
        value = 0.0
        for tap_index, coefficient in enumerate(taps):
            source = index + tap_index - center
            if 0 <= source < length:
                value += coefficient * samples[source]
        narrow.append(_mulaw_roundtrip(value))
    output = array("h")
    for index in range(length):
        low = index // 2
        value = narrow[low] if index % 2 == 0 else \
            (narrow[low] + narrow[min(low + 1, len(narrow) - 1)]) / 2
        output.append(max(-32768, min(32767, round(value))))
    return output


def read_pcm16(path):
    with wave.open(str(path), "rb") as source:
        if (source.getnchannels(), source.getsampwidth(), source.getframerate(),
                source.getcomptype()) != (1, 2, SAMPLE_RATE, "NONE"):
            raise ValueError("telephone simulation requires 16 kHz mono PCM16 WAV")
        samples = array("h")
        samples.frombytes(source.readframes(source.getnframes()))
        if sys.byteorder != "little":
            samples.byteswap()
        if len(samples) != source.getnframes():
            raise ValueError("truncated PCM16 WAV")
        return samples


def write_telephone(source, destination):
    source, destination = Path(source), Path(destination)
    if source.resolve() == destination.resolve():
        raise ValueError("telephone output cannot overwrite source")
    audio = simulate(read_pcm16(source))
    if sys.byteorder != "little":
        audio.byteswap()
    with destination.open("xb") as binary:
        with wave.open(binary, "wb") as target:
            target.setnchannels(1)
            target.setsampwidth(2)
            target.setframerate(SAMPLE_RATE)
            target.writeframes(audio.tobytes())
    return len(audio)
