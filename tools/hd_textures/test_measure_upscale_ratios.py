"""No disc needed: the upscale-ratio study's simulated view and difference measure behave."""
import unittest
import numpy as np
from PIL import Image
import measure_upscale_ratios as m


def noise(w, h, seed=1):
    rng = np.random.default_rng(seed)
    a = rng.integers(0, 256, (h, w, 4), dtype=np.uint8)
    a[..., 3] = 255
    return Image.fromarray(a, "RGBA")


class StudyTests(unittest.TestCase):
    def test_categories(self):
        cases = {
            "TFA/ANIMATION/TRANSFORMERS/GAME/OPT_HEAD.png": "player",
            "TFA/ANIMATION/TRANSFORMERS/GAME/MINICONS/X.png": "minicon",
            "TFA2/ANIMATION/MISCELLANEOUS/MINICONS/ARMOR_ARMS.png": "minicon",
            "TFA/ANIMATION/MAGA_HEAD.png": "enemy",
            "TFA/LEVELS/EPISODES/01_AMAZON_1/GEOMETRY/A.png": "terrain",
            "TFA/LEVELS/EPISODES/01_AMAZON_1/ECOSYSTEM/A.png": "foliage",
            "TFA2/SKY/A.png": "sky",
            "TFA2/FXTXTR/A.png": "fx",
            "TFA/USERINTERFACE/TEXTURES/A.png": "ui",
            "TFA2/HUD/A.png": "ui",
            "TFA2/FONTS/A.png": "font",
        }
        for rel, cat in cases.items():
            with self.subTest(rel=rel):
                self.assertEqual(m.category_for(rel), cat)
        self.assertEqual(set(m.CLOSE_VIEW_1440), set(m.CATEGORY_ORDER))

    def test_view_sizes(self):
        self.assertEqual(m.view_size("S512", 128, 64), (512, 256))
        self.assertEqual(m.view_size("M3.2", 100, 50), (320, 160))
        self.assertEqual(m.view_size(("S", 256.0), 64, 64), (256, 256))
        self.assertEqual(m.close_view("terrain", 720), ("S", 512.0))
        self.assertEqual(m.close_view("ui", 896), ("M", 2.0))

    def test_identical_is_zero_and_detail_counts(self):
        img = noise(64, 64)
        a = m.on_screen(m.mip_chain(img), (256, 256))
        d, c = m.compare(a, a)
        self.assertAlmostEqual(d, 0.0, places=6)
        self.assertEqual(c, 0.0)
        # A blurred copy differs visibly; a flat image against itself does not.
        blurred = img.resize((16, 16), Image.BOX).resize((64, 64), Image.BILINEAR)
        b = m.on_screen(m.mip_chain(blurred), (256, 256))
        self.assertGreater(m.compare(a, b)[0], m.VISIBLE)

    def test_resolution_beyond_screen_is_invisible(self):
        # The same picture at 2x and 4x the screen size: both minified to the screen, the
        # mipmaps leave (almost) nothing to tell apart.
        base = noise(32, 32).resize((256, 256), Image.BICUBIC)
        lo = base.resize((512, 512), Image.LANCZOS)
        hi = base.resize((1024, 1024), Image.LANCZOS)
        size = (256, 256)
        d, _ = m.compare(m.on_screen(m.mip_chain(lo), size), m.on_screen(m.mip_chain(hi), size))
        self.assertLess(d, m.VISIBLE)

    def test_alpha_composited(self):
        img = Image.new("RGBA", (8, 8), (255, 0, 0, 0))
        a = m.on_screen(m.mip_chain(img), (8, 8))
        self.assertTrue(np.allclose(a, m.GREY))

    def test_needed_level(self):
        vm = {"step": {"1-2": (0.2, 0), "2-4": (0.05, 0), "4-8": (0.004, 0), "8-16": (0.001, 0)}}
        self.assertEqual(m.needed_level(vm, [1, 2, 4, 8, 16]), 4)
        vm["step"]["8-16"] = (0.02, 0)
        self.assertEqual(m.needed_level(vm, [1, 2, 4, 8, 16]), 16)


if __name__ == "__main__":
    unittest.main()
