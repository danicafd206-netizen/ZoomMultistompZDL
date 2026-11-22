"""Fahhh: run the pedal C kernel on the host and check what the knobs promise.

Uses tools/audio_preview/render_kernel.py, which compiles src/custom/fahhh/
fahhh.c with only the firmware pointer plumbing replaced and gives it an arena
of the pedal's size. Needs cc and numpy; skipped without them.
"""
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DIR = ROOT / "src/custom/fahhh"
SRC = DIR / "fahhh.c"
sys.path.insert(0, str(ROOT / "tools/audio_preview"))
sys.path.insert(0, str(DIR))

try:
    import numpy as np
    from render_kernel import Kernel, PEDAL_ARENA_BYTES
except ImportError:  # numpy missing
    np = None

SR = 44100
REF_HZ = 243.0
# The largest proven ZDL is 32,126 bytes (code+data 29,576). The first real
# build measured the effect without its sample at 10,786 bytes of code+data,
# which leaves ~18.7 KB for the ADPCM; keep ~700 bytes of that for code.
MAX_ADPCM_BYTES = 18000


def rms(y):
    return float(np.sqrt(np.mean(np.asarray(y, np.float64) ** 2)))


def pluck(f, dur=4.0, amp=0.25, at=0.0, seed=0):
    """A guitar-like note: decaying harmonics and a burst of pick noise."""
    rng = np.random.default_rng(seed)
    n = int(SR * dur)
    t = np.arange(n) / SR
    y = np.zeros(n)
    for h in range(1, 12):
        if h * f > 8000:
            break
        y += np.exp(-t * (1.5 + 0.6 * h)) * np.sin(2 * np.pi * h * f * t + rng.uniform(0, 6)) / h
    y[:300] += rng.normal(0, 0.3, 300) * np.linspace(1, 0, 300)
    y = y / np.abs(y).max() * amp
    start = int(at * SR)
    y = np.concatenate([np.zeros(start), y])[:n]
    return np.stack([y, y], 1).astype(np.float32)


def held(f, dur=6.0, amp=0.2):
    """A note that never fades: one pick, then a steady tone."""
    t = np.arange(int(SR * dur)) / SR
    y = amp * (np.sin(2 * np.pi * f * t) + 0.3 * np.sin(4 * np.pi * f * t))
    y[:200] += np.random.default_rng(1).normal(0, 0.2, 200)
    return np.stack([y, y], 1).astype(np.float32)


def sounding(y, floor=1e-4):
    """First and last sample above the floor, in seconds."""
    nz = np.nonzero(np.abs(y[:, 0]) > floor)[0]
    return (nz[0] / SR, nz[-1] / SR) if len(nz) else (None, None)


@unittest.skipUnless(shutil.which("cc") and np is not None, "needs cc and numpy")
class Fahhh(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.k = Kernel(DIR, Path(cls.tmp.name))
        # The sample alone, at its own pitch, without echoes.
        cls.fah = dict(Mix=100, Time=0, Pitch=0)
        cls.full_len = cls.length(cls, pluck(REF_HZ, dur=4.0), **cls.fah)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def length(self, x, **ui):
        a, b = sounding(self.k.render(x, **ui))
        assert a is not None, "the sample did not fire"
        return b - a

    # --- the pedal's rules, checked on the source ---------------------------

    def test_source_follows_safe_dsp_rules(self):
        text = SRC.read_text()
        code = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        code = re.sub(r"//[^\n]*", "", code)
        code = re.sub(r'"[^"\n]*"', '""', code)
        body = code[code.index("void FAHHH_AUDIO_FUNC"):]
        self.assertNotIn("switch", code, "a switch becomes a jump table: freezes the DSP")
        self.assertNotIn("double", code)
        self.assertNotRegex(code, r"#include\s*<math\.h>", "no math library on the pedal")
        self.assertEqual(re.findall(r"\bstatic\s+const\s+\w+\s+(\w+)\[", code), ["FH_STEP"],
                         "read-only tables only: the ADPCM steps (and the sample, in its header)")
        self.assertNotRegex(code, r"\bstatic\s+(?!const|inline)\w+\s+\w+\s*[=;\[]", "no writable statics")
        self.assertNotRegex(code, r"[^/*]/[^/*=]|%", "no runtime divide or modulo")
        self.assertNotRegex(body, r"\b(float|int|int32_t|uint32_t)\s+\w+\s*\[",
                            "no stack arrays in the audio function")
        for helper in re.findall(r"static inline \w+ (fh_\w+)\(", code):
            self.assertIn(f"ZOOM_ALWAYS_INLINE({helper})", code,
                          f"{helper} must be pinned inline: an outlined helper is a CALL that freezes")

    def test_no_implicit_double_math(self):
        """A float literal without f promotes the whole expression to double,
        which on the C6000 becomes helper calls the ZDL does not carry."""
        r = subprocess.run(
            ["cc", "-std=c99", "-fsyntax-only", "-Wdouble-promotion", "-Werror=double-promotion",
             "-Wno-unknown-pragmas", f"-I{DIR}", str(SRC)],
            capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_sample_header_is_consistent_and_fits(self):
        h = (DIR / "fahhh_sample.h").read_text()
        n = int(re.search(r"#define FAH_SAMPLES (\d+)", h).group(1))
        nbytes = int(re.search(r"#define FAH_ADPCM_BYTES (\d+)", h).group(1))
        data = re.search(r"FAH_ADPCM\[\d+\] = \{(.*?)\};", h, re.S).group(1)
        self.assertEqual(len(re.findall(r"0x[0-9A-F]{2}", data)), nbytes)
        self.assertEqual(nbytes, (n + 1) // 2)
        self.assertLessEqual(nbytes, MAX_ADPCM_BYTES, "the sample would push the ZDL past the pedal's size cap")

    # --- memory ---------------------------------------------------------------

    def test_fits_the_pedal_arena_and_fails_safe_when_it_does_not(self):
        x = pluck(REF_HZ, dur=1.0)
        self.assertGreater(rms(self.k.render(x, arena_bytes=PEDAL_ARENA_BYTES, **self.fah)), 1e-3)
        small = self.k.render(x, arena_bytes=200_000, Mix=50)
        np.testing.assert_array_equal(small, x, "a too-small arena must leave the signal dry")

    # --- triggering -------------------------------------------------------------

    def test_mix_zero_is_an_exact_bypass(self):
        x = pluck(110, dur=2.0)
        np.testing.assert_array_equal(self.k.render(x, Mix=0, Time=60, Fdbk=80), x)

    def test_silence_stays_silent(self):
        x = np.zeros((SR * 2, 2), np.float32)
        self.assertEqual(float(np.abs(self.k.render(x, Mix=100, Sens=100)).max()), 0.0)

    def test_a_pick_fires_the_whole_sample_at_once(self):
        y = self.k.render(pluck(REF_HZ, dur=4.0), **self.fah)
        a, b = sounding(y)
        self.assertLess(a, 0.005, "the sample must start with the pick, not after the pitch search")
        self.assertAlmostEqual(b - a, self.full_len, delta=0.01)
        self.assertGreater(self.full_len, 0.9)   # the 0.94 s cut (make_sample.py)

    def test_a_held_note_fires_once(self):
        a, b = sounding(self.k.render(held(REF_HZ, dur=6.0), **self.fah))
        self.assertLess(b, self.full_len + 0.1, "a sustained note must not retrigger")

    def test_every_pick_retriggers(self):
        x = sum(pluck(110, dur=4.0, at=t, seed=i) for i, t in enumerate((0.0, 0.5, 1.0, 1.5)))
        _, b = sounding(self.k.render(x, **self.fah))
        self.assertAlmostEqual(b, 1.5 + self.full_len, delta=0.05, msg="the last pick restarts the sample")

    def test_sens_sets_how_light_a_pick_fires(self):
        quiet = pluck(196, dur=1.0, amp=0.012)
        self.assertGreater(rms(self.k.render(quiet, Sens=100, **self.fah)), 1e-3)
        self.assertEqual(float(np.abs(self.k.render(quiet, Sens=0, **self.fah)).max()), 0.0)

    # --- pitch ------------------------------------------------------------------

    def test_orig_plays_every_note_at_the_recordings_own_speed(self):
        for f in (82.4, 196.0, 659.3):
            self.assertAlmostEqual(self.length(pluck(f), **self.fah), self.full_len, delta=0.01)

    # Before the pitch is known (~50 ms), a fresh kernel plays at the
    # recording's own speed, then glides to the note's. On a 0.94 s sample
    # that lead-in is too large a share of the length to ignore.
    LEAD_IN = 0.05

    def expected_len(self, ratio):
        return self.LEAD_IN + (self.full_len - self.LEAD_IN) / ratio

    def test_note_retunes_to_the_note_picked(self):
        """Varispeed: the length shrinks by the pitch ratio, after the lead-in."""
        for f in (146.8, 196.0, 329.6, 440.0):
            ratio = f / REF_HZ
            got = self.length(pluck(f, dur=6.0), Mix=100, Time=0, Pitch=100)
            self.assertAlmostEqual(got, self.expected_len(ratio), delta=0.03 * got, msg=f"{f} Hz")

    def test_note_folds_extremes_by_octaves(self):
        for f, ratio in ((82.4, 2 * 82.4 / REF_HZ), (659.3, 0.5 * 659.3 / REF_HZ)):
            got = self.length(pluck(f, dur=6.0), Mix=100, Time=0, Pitch=100)
            self.assertAlmostEqual(got, self.expected_len(ratio), delta=0.03 * got, msg=f"{f} Hz")

    # --- delay on the sample only ----------------------------------------------

    def test_time_zero_has_no_echo(self):
        _, b = sounding(self.k.render(pluck(REF_HZ, dur=5.0), Mix=100, Time=0, Fdbk=90, Pitch=0))
        self.assertLess(b, self.full_len + 0.01)

    def test_delay_repeats_the_sample_and_feedback_extends_it(self):
        x = pluck(REF_HZ, dur=8.0)
        dt = 0.020 + 1.180 * 0.5 ** 2
        _, one = sounding(self.k.render(x, Mix=100, Time=50, Fdbk=0, Pitch=0))
        self.assertAlmostEqual(one, self.full_len + dt, delta=0.02, msg="one echo at the knob's time")
        _, many = sounding(self.k.render(x, Mix=100, Time=50, Fdbk=80, Pitch=0))
        self.assertGreater(many, one + 3 * dt)

    def test_delay_never_touches_the_dry(self):
        """Mix 50 with the sample silenced is the dry at half level, whatever
        the delay: a pick too light for Sens 0 never reaches the delay."""
        x = pluck(196, dur=3.0, amp=0.012)
        y = self.k.render(x, Mix=50, Time=80, Fdbk=90, Sens=0)
        np.testing.assert_allclose(y, 0.5 * x, atol=1e-7)

    # --- robustness ---------------------------------------------------------------

    def test_extreme_settings_stay_bounded(self):
        x = sum(pluck(82.4 * 2 ** (i / 12), dur=12.0, amp=0.5, at=0.1 * i, seed=i) for i in range(40))
        y = self.k.render(np.ascontiguousarray(x, np.float32), Mix=100, Time=100, Fdbk=100, Sens=100, Pitch=100)
        self.assertTrue(np.isfinite(y).all())
        self.assertLess(float(np.abs(y).max()), 2.0)

    def test_garbage_right_input_cannot_poison_it(self):
        x = pluck(REF_HZ, dur=2.0)
        x[::97, 1] = np.nan
        x[::89, 1] = np.inf
        y = self.k.render(x, Mix=100, Time=40, Fdbk=60)
        self.assertTrue(np.isfinite(y).all())
        self.assertGreater(rms(y), 1e-3)


if __name__ == "__main__":
    unittest.main()
