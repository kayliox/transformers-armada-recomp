"""No disc needed: generated glyphs stay inside cells and PNG bytes repeat exactly."""
import io
import unittest
import numpy as np
import make_crisp_fonts as fonts

class XboxGlyphTests(unittest.TestCase):
    def test_cells_and_determinism(self):
        for style in ("one", "360"):
          for kind in fonts.XBOX_LABELS:
            for w, h in ((48, 64), (128, 64), (13, 19)):
                with self.subTest(style=style, kind=kind, size=(w,h)):
                    a = fonts.draw_xbox(kind, w, h, (3,3,w-3,h-3), .75, style)
                    b = fonts.draw_xbox(kind, w, h, (3,3,w-3,h-3), .75, style)
                    self.assertEqual(a.shape, (h,w,4))
                    self.assertTrue(np.isfinite(a).all())
                    self.assertGreater(a[...,3].max(), 0)
                    self.assertLessEqual(a[...,3].max(), .75)
                    self.assertFalse(a[:3,:,3].any())
                    self.assertFalse(a[-3:,:,3].any())
                    self.assertFalse(a[:,:3,3].any())
                    self.assertFalse(a[:,-3:,3].any())
                    canvas = np.zeros((h+12,w+14,4), dtype=np.float32)
                    canvas[5:5+h,7:7+w] = a
                    self.assertFalse(canvas[:5].any())
                    self.assertFalse(canvas[5+h:].any())
                    self.assertFalse(canvas[:,:7].any())
                    self.assertFalse(canvas[:,7+w:].any())
                    def png(arr):
                        data = io.BytesIO()
                        fonts.to_image(arr).save(data, format='PNG', optimize=False)
                        return data.getvalue()
                    self.assertEqual(png(a), png(b))

    def test_styles_differ_only_where_the_pads_do(self):
        # The face buttons and VIEW / MENU differ between the 360 and One / Series looks; the shoulder keys do not.
        def png(arr):
            data = io.BytesIO()
            fonts.to_image(arr).save(data, format='PNG', optimize=False)
            return data.getvalue()
        for kind in fonts.XBOX_LABELS:
            with self.subTest(kind=kind):
                one = png(fonts.draw_xbox(kind, 96, 96, (4,4,92,92), 1.0, "one"))
                old = png(fonts.draw_xbox(kind, 96, 96, (4,4,92,92), 1.0, "360"))
                if kind in fonts.FACE_KINDS or kind in ("select", "start"):
                    self.assertNotEqual(one, old)
                else:
                    self.assertEqual(one, old)

if __name__ == '__main__':
    unittest.main()
