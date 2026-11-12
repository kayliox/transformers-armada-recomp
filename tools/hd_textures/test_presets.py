"""The HD textures presets (tier_rules.PRESETS, PR #20) as the maker plans them."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tier_rules  # noqa: E402
import upscale_textures as u  # noqa: E402


def src(rel, w, h, sha):
    return u.Src(path=Path(rel), relpath=rel, sha1=sha, w=w, h=h, alpha="opaque", tier=tier_rules.tier_for(rel))


class Presets(unittest.TestCase):
    def test_sets_from_before_the_presets_are_still_recognised(self):
        # (the hashes Best / Fast sets carried before the presets: their images are kept, not made again)
        self.assertEqual(tier_rules.legacy_settings_hash("best"), "8051af3c77a8")
        self.assertEqual(tier_rules.legacy_settings_hash("fast"), "413c8158d34a")
        self.assertLessEqual({"8051af3c77a8", "413c8158d34a"}, tier_rules.known_settings())

    def test_every_preset_names_every_category(self):
        for q in tier_rules.QUALITIES:
            self.assertEqual(set(tier_rules.PRESETS[q]), set(tier_rules.CATEGORY_ORDER), q)
            self.assertIn(q, tier_rules.ESTIMATES)

    def test_balanced_keeps_enemies_and_minicons_at_4x(self):
        b = tier_rules.PRESETS["balanced"]
        self.assertEqual((b["enemy"], b["minicon"]), (4, 4))
        self.assertEqual((b["terrain"], b["foliage"], b["sky"]), (8, 8, 8))
        self.assertEqual(tier_rules.category_for("TFA/ANIMATION/DECEPTICONS/CYCLONUS/BODY.png"), "enemy")
        self.assertEqual(tier_rules.category_for("TFA2/ANIMATION/MISCELLANEOUS/MINICONS/X.png"), "minicon")

    def test_ratios_become_targets_and_passes(self):
        srcs = [src("TFA/ANIMATION/TRANSFORMERS/HOTSHOT/BODY.png", 256, 256, "a"),
                src("TFA/LEVELS/EPISODES/01_AMAZON/ROCK.png", 128, 64, "b"),
                src("TFA/USERINTERFACE/TEXTURES/MENU.png", 256, 128, "c"),
                src("TFA/ANIMATION/TRANSFORMERS/HOTSHOT/BIG.png", 512, 512, "d")]
        def plan(q):
            return {j.sha1: (j.passes, j.out_w, j.out_h) for j in u.plan_jobs(srcs, None, "direct", [], "ncnn", q)}
        fast, best = plan("fast"), plan("best")
        self.assertEqual(fast["a"], (2, 2048, 2048))   # player 8x: two passes
        self.assertEqual(fast["b"], (1, 512, 256))     # terrain 4x: one
        self.assertEqual(fast["c"], (1, 512, 256))     # UI 2x: one pass, scaled down
        self.assertEqual(best["a"], (2, 4096, 4096))   # player 16x
        self.assertEqual(best["d"], (2, 4096, 4096))   # capped at 4096
        self.assertEqual(plan("balanced")["b"], (2, 1024, 512))

    def test_an_image_already_made_at_this_size_is_kept(self):
        job = u.plan_jobs([src("TFA/LEVELS/EPISODES/01_AMAZON/ROCK.png", 128, 64, "b")],
                          None, "direct", [], "ncnn", "fast")[0]
        rec = {"settings": tier_rules.legacy_settings_hash("best"), "out": [1024, 512], "relpaths": job.relpaths}
        also = tier_rules.known_settings() - {tier_rules.settings_hash("fast")}
        with tempfile.TemporaryDirectory() as d:
            out = Path(d)
            for rp in job.relpaths:
                (out / rp).parent.mkdir(parents=True, exist_ok=True)
                (out / rp).write_bytes(b"")
            self.assertFalse(u.job_done(job, {"b": rec}, out, tier_rules.settings_hash("fast"), also))  # shrunk first
            rec["out"] = [job.out_w, job.out_h]  # the same size: kept
            self.assertTrue(u.job_done(job, {"b": rec}, out, tier_rules.settings_hash("fast"), also))
            rec["out"] = [256, 128]  # smaller than Fast's 512: made again
            self.assertFalse(u.job_done(job, {"b": rec}, out, tier_rules.settings_hash("fast"), also))
    def test_a_larger_image_is_shrunk_to_the_preset(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            rel = "TFA/LEVELS/EPISODES/01_AMAZON/ROCK.png"
            srcp = d / "src" / rel
            srcp.parent.mkdir(parents=True)
            small = Image.radial_gradient("L").resize((64, 32)).convert("RGBA")
            small.save(srcp)
            s = u.Src(path=srcp, relpath=rel, sha1="b", w=64, h=32, alpha="opaque", tier="env")
            job = u.plan_jobs([s], None, "direct", [], "ncnn", "fast")[0]   # terrain 4x: 256 x 128
            out = d / "upscaled"
            (out / rel).parent.mkdir(parents=True)
            small.resize((1024, 512), Image.LANCZOS).save(out / rel)       # an old Best image, 16x
            rec = {"sha1": "b", "relpaths": [rel], "tier": "env", "model": job.model, "passes": 2,
                   "alpha": "opaque", "alpha_mode": "rgb", "out": [1024, 512],
                   "settings": tier_rules.legacy_settings_hash("best")}
            recs = {"b": rec}
            settings = tier_rules.settings_hash("fast")
            also = tier_rules.known_settings() - {settings}
            self.assertFalse(u.job_done(job, recs, out, settings, also))
            got = u.shrink_larger([job], recs, out, d / "upscaled_precolour", d / "src", settings, also)
            self.assertEqual(got, (1, 0))
            self.assertEqual(Image.open(out / rel).size, (256, 128))
            self.assertTrue(u.job_done(job, u.load_manifest(out), out, settings, also))
            self.assertEqual(u.shrink_larger([job], u.load_manifest(out), out, d / "upscaled_precolour", d / "src",
                                             settings, also), (0, 0))


if __name__ == "__main__":
    unittest.main()
