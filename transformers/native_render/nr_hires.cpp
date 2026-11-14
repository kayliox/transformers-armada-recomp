// High-resolution replacements for the game's textures (README.md): the remaster's upscaled set
// (Real-ESRGAN, made once from every texture on the disc: 4K heroes, 2K environments, alpha kept;
// private, never redistributed), found for a texture by its name and confirmed by its content.
//
// Names are bare stems that several folders share with different images, so each candidate's
// native-resolution conversion (the same tree before upscaling) is compared with the texels the
// game holds, and the closest one under a threshold is taken - which also picks a palette variant
// (_palN) by its colours. UVs are normalised, so a larger image maps 1:1 onto the original's.
//
// TF_HIRES_DIR: the upscaled tree (default: hd_textures\upscaled where the game starts, made by
// tools\make_hd_textures.ps1, else the remaster's Extracted/upscaled); its native twin is
// ../converted/textures beside it. TF_HIRES=0 turns replacement off.
#include "dev_tree.h"
#include "nr_types.h"
#include "nr_backend.h"
#include "nr_policy.h"
#include "nr_prompt_style.h"
#include "key_text.h"

#include "raylib.h"

#include "runtime/ps2_host_input.h"
#include "runtime/ps2_thread_placement.h"

#include "ext/bc7enc/bc7enc.h"
#include "ext/bc7enc/bc7decomp.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
// (Not <windows.h>: its names clash with raylib's.)
extern "C" __declspec(dllimport) void *__stdcall GetCurrentThread(void);
extern "C" __declspec(dllimport) int __stdcall SetThreadPriority(void *thread, int priority);
#endif

namespace nr
{
    namespace
    {
        namespace fs = std::filesystem;

        // (TODO 186) The developer's own tree (dev_tree.h): the last place looked in, never a player's.

        struct Index
        {
            bool ready = false, enabled = false;
            fs::path upscaled, native;
            std::unordered_map<std::string, std::vector<std::string>> byStem; // upper-case stem -> relpaths (no extension)
            // (TODO 119) The sharp font atlases (tools\hd_textures\make_crisp_fonts.py): the set the two
            // settings choose - empty when crisp text is off and the symbols are the PS2's.
            fs::path crisp;
            std::string crispSet;
            // (the keyboard's key prompts: text_keys / keys, drawn while the keyboard is the device last used)
            std::string keysSet, xboxSet, xbox360Set;
            bool fontsOnly = false;
            bool ps2Symbols = false; // (TF_BUTTON_SYMBOLS=ps2: Auto keeps them, xboxPromptsShown)
            // (TODO 290 (17)) A PCSX2 texture pack (TF_HD_PACK): our texture name (relpath) -> the pack's image, through
            // tools\hd_textures\pcsx2_names.json. Empty: no pack, the upscaled set as before.
            std::unordered_map<std::string, fs::path> pack;
        } g;

        // (TODO 290 (17)) The PCSX2 pack's name table: PCSX2's image name (lower case, no extension) -> our texture's relpath.
        // Looked for where the game starts, then beside the exe and above it (as hd_textures is).
        std::unordered_map<std::string, std::string> packNames()
        {
            std::unordered_map<std::string, std::string> out;
            std::error_code ec;
            fs::path file = fs::path("tools") / "hd_textures" / "pcsx2_names.json";
            for (fs::path at = fs::path(GetApplicationDirectory()); !fs::exists(file, ec) && !at.empty(); at = at.parent_path())
            {
                file = at / "tools" / "hd_textures" / "pcsx2_names.json";
                if (at == at.parent_path())
                    break;
            }
            std::ifstream f(file, std::ios::binary);
            std::string line;
            while (std::getline(f, line))
            {
                // (the file's lines: "<hash>-<hash>-<bits>": "<relpath>",)
                const size_t a = line.find('"'), b = a == std::string::npos ? a : line.find('"', a + 1);
                const size_t c = b == std::string::npos ? b : line.find('"', b + 1), d = c == std::string::npos ? c : line.find('"', c + 1);
                if (d == std::string::npos || line.compare(a + 1, 1, "_") == 0 || line.find("\": \"", b) != b)
                    continue;
                out[line.substr(a + 1, b - a - 1)] = line.substr(c + 1, d - c - 1);
            }
            return out;
        }

        // (TODO 290 (17)) The pack's images in a folder (and its subfolders), by our texture names. `matched` / `images`:
        // how many images it has.
        std::unordered_map<std::string, fs::path> indexPack(const fs::path &folder, size_t &images)
        {
            std::unordered_map<std::string, fs::path> out;
            images = 0;
            const auto names = packNames();
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                if (!it->is_regular_file())
                    continue;
                std::string ext = it->path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                if (ext != ".dds" && ext != ".png")
                    continue;
                ++images;
                std::string stem = it->path().stem().string();
                std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                if (auto n = names.find(stem); n != names.end())
                    out.emplace(n->second, it->path());
            }
            return out;
        }

        // A BC7 DDS (DX10 header, DXGI 98 / 99): its largest level the cap allows, as RGBA8, the alpha doubled. False: not one.
        bool decodeDds(const fs::path &file, const Texture &game, uint32_t cap, uint32_t &w, uint32_t &h, std::vector<uint32_t> &px);

        // (TODO 119) Crisp text and PS5 button symbols: the font atlases made again at 8x their size, every
        // glyph in its own rectangle (the game draws the same quads: nothing moves). Three sets beside the
        // upscaled tree (crisp_fonts\, TF_CRISP_DIR another): text_ps5, text_ps2 (TF_CRISP_TEXT=1, the
        // default, with TF_BUTTON_SYMBOLS=ps5 / ps2), ps5 (crisp text off, PS5 symbols); off with the PS2's =
        // none, the images as before. An atlas the set has replaces its upscaled image (found by the same
        // name and content check); the others are untouched.
        void chooseCrisp()
        {
            const char *t = std::getenv("TF_CRISP_TEXT");
            const char *b = std::getenv("TF_BUTTON_SYMBOLS");
            const bool text = !(t && *t == '0');
            const bool ps5 = !(b && (std::string(b) == "ps2" || std::string(b) == "PS2"));
            g.crispSet = text ? (ps5 ? "text_ps5" : "text_ps2") : (ps5 ? "ps5" : "");
            g.ps2Symbols = !ps5;
            const char *dir = std::getenv("TF_CRISP_DIR");
            g.crisp = dir && *dir ? fs::path(dir) : g.upscaled.parent_path() / "crisp_fonts";
            std::error_code ec;
            // (TODO 186: the install's own set first; the developer's tree only when the install has none)
            if (!(dir && *dir) && !g.crispSet.empty() && !fs::is_directory(g.crisp / g.crispSet, ec) &&
                !devTree().empty() && fs::is_directory(devTree() / "crisp_fonts" / g.crispSet, ec))
                g.crisp = devTree() / "crisp_fonts";
            // (PR #11) The Xbox prompts: each atlas with button symbols has a twin with Xbox labels in their
            // place, with the crisp text or without; drawn instead while xboxPromptsShown.
            g.xboxSet = text ? "text_xbox" : "xbox";
            if (!fs::is_directory(g.crisp / g.xboxSet, ec))
                g.xboxSet.clear();
            // (The Xbox 360 pad's look beside the Xbox One / Series one: drawn while the pads plugged in are 360 ones.
            // Fonts made before it have none: the One / Series look for every Xbox pad, as before.)
            g.xbox360Set = text ? "text_xbox360" : "xbox360";
            if (g.xboxSet.empty() || !fs::is_directory(g.crisp / g.xbox360Set, ec))
                g.xbox360Set.clear();
            // The key prompts (TF_KEY_PROMPTS, on by default): each atlas with button symbols has a twin with the
            // player's keys in their place (make_crisp_fonts.py, from controls.txt), with the crisp text or
            // without; loaded beside it and drawn instead while the keyboard and mouse are the device last
            // used (nr_d3d.cpp textureFor, keyPromptsShown).
            if (const char *k = std::getenv("TF_KEY_PROMPTS"); !(k && *k == '0'))
            {
                g.keysSet = text ? "text_keys" : "keys";
                if (!fs::is_directory(g.crisp / g.keysSet, ec))
                    g.keysSet.clear();
                else
                    std::fprintf(stderr, "[native render] key prompts: %s from %s\n", g.keysSet.c_str(), g.crisp.string().c_str());
            }
            if (g.crispSet.empty() || !fs::is_directory(g.crisp / g.crispSet, ec))
            {
                if (!g.crispSet.empty())
                    std::fprintf(stderr, "[native render] crisp fonts: none at %s (tools\\make_hd_textures.ps1 makes them)\n", g.crisp.string().c_str());
                g.crispSet.clear();
                return;
            }
            std::fprintf(stderr, "[native render] crisp fonts: %s from %s\n", g.crispSet.c_str(), g.crisp.string().c_str());
        }

        // The set's image for a chosen relpath; empty when it has none.
        fs::path crispImage(const std::string &rel, const std::string &set)
        {
            if (set.empty())
                return {};
            const fs::path p = g.crisp / set / (rel + ".png");
            std::error_code ec;
            return fs::is_regular_file(p, ec) ? p : fs::path();
        }

        std::string upper(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return s;
        }

        // hd_textures\upscaled of the install: in the folder the game starts in, else in the exe's folder or
        // one of the four above it (the build's out\build\...\RelWithDebInfo sits under the port's root).
        fs::path installedTree()
        {
            std::error_code ec;
            // (PR #11: an install with only the fonts-only step's crisp_fonts counts too - its fonts are used)
            const auto installed = [&ec](const fs::path &hd)
            { return fs::is_directory(hd / "upscaled", ec) || fs::is_directory(hd / "crisp_fonts", ec); };
            if (installed(fs::path("hd_textures")))
                return fs::absolute(fs::path("hd_textures") / "upscaled", ec);
            fs::path at = fs::path(GetApplicationDirectory());
            for (int up = 0; up < 5 && !at.empty(); ++up, at = at.parent_path())
            {
                if (installed(at / "hd_textures"))
                    return at / "hd_textures" / "upscaled";
                if (at == at.parent_path())
                    break;
            }
            return {};
        }

        // Where the set is: TF_HIRES_DIR; else hd_textures\upscaled in the folder the game starts in
        // (what tools\make_hd_textures.ps1 makes from your disc), or in the exe's folder or one above it
        // (TODO 186: a start from another folder); only then the remaster's folder on the developer's PC.
        fs::path resolveUpscaled()
        {
            const char *dir = std::getenv("TF_HIRES_DIR");
            if (dir && *dir)
                return fs::path(dir);
            if (const fs::path found = installedTree(); !found.empty())
                return found;
            if (!devTree().empty())
                return devTree() / "upscaled";
            return {};
        }

        // (TODO 259) The set's marker (upscaled/.made_on.json, written by the maker: upscale_textures.py) says the set
        // was set aside: made on a graphics card whose HD images come out broken, or by an unknown card on a PC whose
        // card's do. Returns why ("" when it is not set aside). TF_HIRES_TRUST=1: used all the same.
        std::string setAsideWhy(const fs::path &upscaled)
        {
            if (const char *trust = std::getenv("TF_HIRES_TRUST"); trust && *trust == '1')
                return {};
            std::error_code ec;
            const fs::path marker = upscaled / ".made_on.json";
            if (upscaled.empty() || !fs::is_regular_file(marker, ec))
                return {};
            std::ifstream in(marker, std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const size_t at = text.find("\"set_aside\"");
            if (at == std::string::npos)
                return {};
            const size_t value = text.find_first_not_of(" \t\r\n:", at + 11u);
            if (value == std::string::npos || text.compare(value, 4u, "true") != 0)
                return {};
            std::string why = "made on a graphics card whose HD images come out broken";
            if (const size_t w = text.find("\"why\""); w != std::string::npos)
                if (const size_t open = text.find('"', text.find(':', w)); open != std::string::npos)
                    if (const size_t close = text.find('"', open + 1u); close != std::string::npos && close > open + 1u)
                        why = text.substr(open + 1u, close - open - 1u);
            return why;
        }

        void build()
        {
            g.ready = true;
            // (TF_HIRES=0, F1 > Graphics > HD textures Off - TODO 243: the game's own textures; the sharp text still
            // follows its own setting)
            const char *off = std::getenv("TF_HIRES");
            bool hdOff = off && *off == '0';
            std::error_code ec;
            g.upscaled = resolveUpscaled();
            if (const std::string why = hdOff ? std::string() : setAsideWhy(g.upscaled); !why.empty())
            {
                std::fprintf(stderr, "[native render] HD textures set aside (%s): the game's own textures. After a graphics driver "
                                     "update, F1 > Graphics > Make HD textures tries the card again\n", why.c_str());
                hdOff = true;
            }
            g.native = g.upscaled.empty() ? fs::path() : g.upscaled.parent_path() / "converted" / "textures";
            chooseCrisp();
            // (TODO 290 (17)) A PCSX2 texture pack chosen in F1 (TF_HD_PACK: its folder, used where it is): its images in place of
            // an upscaled set, found through the name table; the originals (hd_textures\converted) pick among same-named ones.
            if (const char *pk = std::getenv("TF_HD_PACK"); pk && *pk && !hdOff)
            {
                // (The game's own textures where the game starts first: with no upscaled set the folder above may be
                // another one's - the developer's tree - whose converted\textures is not there. Seen on the Vostro.)
                if (const fs::path here = fs::absolute(fs::path("hd_textures") / "converted" / "textures", ec);
                    g.native.empty() || (!fs::is_directory(g.native, ec) && fs::is_directory(here, ec)))
                    g.native = here;
                size_t images = 0;
                if (!fs::is_directory(fs::path(pk), ec))
                    std::fprintf(stderr, "[native render] HD texture pack: its folder is not there any more (%s): the game's own textures\n", pk);
                else if (!fs::is_directory(g.native, ec))
                    std::fprintf(stderr, "[native render] HD texture pack: the game's own textures are not prepared yet (%s)\n", g.native.string().c_str());
                else
                {
                    g.pack = indexPack(fs::path(pk), images);
                    std::fprintf(stderr, "[native render] HD texture pack %s: %zu of its %zu images used\n", pk, g.pack.size(), images);
                }
            }
            if (g.pack.empty() && (hdOff || !fs::is_directory(g.upscaled, ec) || !fs::is_directory(g.native, ec)))
            {
                if (hdOff && off && *off == '0')
                    std::fprintf(stderr, "[native render] HD textures off (TF_HIRES=0): the game's own textures\n");
                else if (hdOff)
                    ; // (set aside: said above)
                else
                    std::fprintf(stderr, "[native render] no upscaled textures at %s\n", g.upscaled.string().c_str());
                g.fontsOnly = true;
                if (g.native.empty() || !fs::is_directory(g.native / "TFA2" / "FONTS", ec))
                {
                    // (the sharp fonts are matched with the game's own font images: without them none is used)
                    if (!g.crispSet.empty() || !g.keysSet.empty() || !g.xboxSet.empty())
                        std::fprintf(stderr, "[native render] crisp fonts: not used, the game's own font images are missing (%s); "
                                             "tools\\make_hd_textures.ps1 -FontsOnly makes them again\n", (g.native / "TFA2" / "FONTS").string().c_str());
                    return;
                }
            }
            size_t files = 0;
            for (auto it = fs::recursive_directory_iterator(g.native, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                if (!it->is_regular_file() || upper(it->path().extension().string()) != ".PNG")
                    continue;
                std::string rel = fs::relative(it->path(), g.native, ec).replace_extension().generic_string();
                if (!g.pack.empty() && g.pack.find(rel) == g.pack.end() &&
                    (rel.rfind("TFA2/FONTS/", 0) != 0 || (crispImage(rel, g.crispSet).empty() && crispImage(rel, g.xboxSet).empty())))
                    continue; // (with a pack: its images and the sharp fonts)
                if (g.fontsOnly && (rel.rfind("TFA2/FONTS/", 0) != 0 ||
                    (crispImage(rel, g.crispSet).empty() && crispImage(rel, g.xboxSet).empty()))) continue;
                std::string stem = upper(it->path().stem().string());
                if (const size_t pal = stem.rfind("_PAL"); pal != std::string::npos && pal + 4 < stem.size() &&
                                                           std::all_of(stem.begin() + pal + 4, stem.end(), ::isdigit))
                    stem.resize(pal);
                g.byStem[stem].push_back(std::move(rel));
                ++files;
            }
            g.enabled = files > 0;
            std::fprintf(stderr, "[native render] upscaled textures: %zu images, %zu names\n", files, g.byStem.size());
        }

        // An image as RGBA8 (R in the low byte); empty when it does not load.
        bool load(const fs::path &file, uint32_t &width, uint32_t &height, std::vector<uint32_t> &pixels)
        {
            Image image = LoadImage(file.string().c_str());
            if (!image.data)
                return false;
            ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            width = static_cast<uint32_t>(image.width);
            height = static_cast<uint32_t>(image.height);
            pixels.resize(static_cast<size_t>(width) * height);
            std::memcpy(pixels.data(), image.data, pixels.size() * 4u);
            UnloadImage(image);
            return true;
        }

        // The candidate (an upscaled image's relpath) whose native image matches the game's texels best (mean
        // RGB difference, at most 8); null when none does or the name is on the skip list.
        const std::string *chooseCandidate(const std::string &name, const Texture &texture)
        {
            if (!g.enabled || name.empty())
                return nullptr;
            auto it = g.byStem.find(upper(name));
            if (it == g.byStem.end())
                return nullptr;
            // Large images known to be wrong keep the game's own (2026-10-29): PROJ - the HQ band's glow, a
            // soft band of blue a few rows above its bottom edge, which the upscaler turned into a thin line
            // on the very edge: the streak along the Drop Zones band (TODO 72). It passes every check below
            // (mean difference 0.51: the image is almost all black). TF_HIRES_SKIP=<name>[,<name>]: more
            // names; TF_HIRES_BAD=0: none of the built-in ones.
            {
                static const bool builtIn = !(std::getenv("TF_HIRES_BAD") && *std::getenv("TF_HIRES_BAD") == '0');
                const char *skip = std::getenv("TF_HIRES_SKIP");
                const std::string list = "," + std::string(builtIn ? "PROJ," : "") + (skip ? upper(skip) : std::string()) + ",";
                if (list.find("," + upper(name) + ",") != std::string::npos)
                    return nullptr;
            }
            // (TODO 307 (k)) The sphere maps (TFA2/REFLECTIONS: the water's, the chrome's) keep the game's own: they are
            // soft pictures of the sky that a reflection samples at a grazing angle, and the upscaler drew thin bright
            // lines into them (JUNGLEREF's rim: cyan arcs; ACREF scratches, EASTERREF rings). On the river by the broken
            // bridge each wave's slope swept those lines across the water every few frames - bright streaks racing
            // over it, where the PS2's 128 x 128 map gives a faint, soft sheen: the river looked far too fast, while its
            // animation is the PS2's at every rate. TF_HIRES_REFLECTIONS=1: their large images as before.
            static const bool reflections = std::getenv("TF_HIRES_REFLECTIONS") && *std::getenv("TF_HIRES_REFLECTIONS") == '1';
            double best = 1e9;
            const std::string *chosen = nullptr;
            for (const std::string &rel : it->second)
            {
                if (!reflections && rel.rfind("TFA2/REFLECTIONS/", 0) == 0)
                    continue;
                uint32_t w = 0, h = 0;
                std::vector<uint32_t> px;
                if (!load(g.native / (rel + ".png"), w, h, px) || w != texture.width || h != texture.height)
                    continue;
                double diff = 0.0;
                for (size_t i = 0; i < px.size(); ++i)
                    for (int c = 0; c < 3; ++c)
                        diff += std::abs(static_cast<int>((px[i] >> (8 * c)) & 0xFFu) - static_cast<int>((texture.pixels[i] >> (8 * c)) & 0xFFu));
                diff /= static_cast<double>(px.size()) * 3.0;
                if (diff < best)
                    best = diff, chosen = &rel;
            }
            static const bool log = std::getenv("TF_NATIVE_NAMES") != nullptr;
            if (log)
                std::fprintf(stderr, "[native render] upscale %s %ux%u: %zu candidates, best %s (difference %.2f)\n", name.c_str(), texture.width,
                             texture.height, it->second.size(), chosen ? chosen->c_str() : "-", best);
            return chosen && best <= 8.0 ? chosen : nullptr;
        }

        // The large image box-averaged down to the game's size (each channel the truncated mean of its fx x fy
        // texels) - what the content check below compares, and what the compressed cache keeps of it.
        std::vector<uint32_t> boxAverage(const std::vector<uint32_t> &px, uint32_t w, uint32_t h, uint32_t tw, uint32_t th)
        {
            std::vector<uint32_t> out(static_cast<size_t>(tw) * th);
            const uint32_t fx = w / tw, fy = h / th;
            for (uint32_t y = 0; y < th; ++y)
                for (uint32_t x = 0; x < tw; ++x)
                {
                    uint32_t sum[4] = {};
                    for (uint32_t dy = 0; dy < fy; ++dy)
                        for (uint32_t dx = 0; dx < fx; ++dx)
                        {
                            const uint32_t c = px[static_cast<size_t>(y * fy + dy) * w + x * fx + dx];
                            for (int k = 0; k < 4; ++k)
                                sum[k] += (c >> (8 * k)) & 0xFFu;
                        }
                    uint32_t v = 0;
                    for (int k = 0; k < 4; ++k)
                        v |= (sum[k] / (fx * fy)) << (8 * k);
                    out[static_cast<size_t>(y) * tw + x] = v;
                }
            return out;
        }

        // The large image must still be the same picture: the tree also holds redrawn art (HUD icons
        // regenerated for the remaster - other shapes, other proportions), which an upscale's own
        // native twin cannot tell apart. Box-filtered down to the game's size (`avg`), it is compared with
        // the game's texels (where they are opaque enough to matter).
        bool looksLikeGame(const std::vector<uint32_t> &avg, const Texture &texture, const std::string &name)
        {
            static const bool log = std::getenv("TF_NATIVE_NAMES") != nullptr;
            double diff = 0.0, alphaDiff = 0.0, swappedDiff = 0.0;
            double gameSum[3] = {}, largeSum[3] = {}; // (each channel's sum over the texels compared)
            double light = 0.0, kept = 0.0; // (the game's light, and how much of it the large image has there)
            double chromaOff = 0.0, gameChroma = 0.0; // (TODO 259: red and blue against green - the large image's off the game's)
            double gl = 0.0, ll = 0.0, gl2 = 0.0, ll2 = 0.0, glll = 0.0; // (TODO 259: the brightness of each, for their correlation)
            size_t counted = 0;
            for (size_t i = 0; i < avg.size() && i < texture.pixels.size(); ++i)
            {
                const uint32_t g0 = texture.pixels[i], a = avg[i];
                alphaDiff += std::abs(static_cast<int>(a >> 24) - static_cast<int>(g0 >> 24));
                if ((g0 >> 24) < 16u)
                    continue;
                for (int k = 0; k < 3; ++k)
                {
                    const int gc = static_cast<int>((g0 >> (8 * k)) & 0xFFu), lc = static_cast<int>((a >> (8 * k)) & 0xFFu);
                    diff += std::abs(lc - gc);
                    // (the large image's texel with its red and blue exchanged, against the same game texel)
                    swappedDiff += std::abs(static_cast<int>((a >> (8 * (2 - k))) & 0xFFu) - gc);
                    light += gc;
                    kept += std::min(gc, lc);
                    gameSum[k] += gc;
                    largeSum[k] += lc;
                }
                {
                    const auto ch = [](uint32_t c, int k) { return static_cast<int>((c >> (8 * k)) & 0xFFu); };
                    const int gr = ch(g0, 0) - ch(g0, 1), gb = ch(g0, 2) - ch(g0, 1), lr = ch(a, 0) - ch(a, 1), lb = ch(a, 2) - ch(a, 1);
                    chromaOff += std::abs(lr - gr) + std::abs(lb - gb);
                    gameChroma += std::abs(gr) + std::abs(gb);
                    const double gy = 0.299 * ch(g0, 0) + 0.587 * ch(g0, 1) + 0.114 * ch(g0, 2);
                    const double ly = 0.299 * ch(a, 0) + 0.587 * ch(a, 1) + 0.114 * ch(a, 2);
                    gl += gy, ll += ly, gl2 += gy * gy, ll2 += ly * ly, glll += gy * ly;
                }
                ++counted;
            }
            const double mean = counted ? diff / (static_cast<double>(counted) * 3.0) : 0.0;
            // (TODO 229) Red and blue exchanged: an HD image whose colours fit the game's texture far better with its
            // red and blue swapped was made with its channels in the wrong order. A dark red texture swapped stays
            // near enough to pass the 20 below (Hot Shot's chest: 17.6), its yellow parts would not. Such an image is
            // not used: the game's own texture is drawn. It needs a clear mismatch first (over 8), so a right image -
            // or a grey one, the same either way - is never taken for one (over the whole remaster set the lowest
            // swapped-to-as-is ratio of a used image is 0.85). TF_HIRES_SWAPCHECK=0: not checked.
            const double swappedMean = counted ? swappedDiff / (static_cast<double>(counted) * 3.0) : 0.0;
            static const bool swapCheck = !(std::getenv("TF_HIRES_SWAPCHECK") && *std::getenv("TF_HIRES_SWAPCHECK") == '0');
            const bool swapped = swapCheck && mean > 8.0 && swappedMean < mean * 0.5;
            if (swapped)
                std::fprintf(stderr, "[native render] upscale %s: its red and blue are exchanged (off the game's by %.2f, by %.2f swapped) - not used\n",
                             name.c_str(), mean, swappedMean);
            // (TODO 229) Black, or a colour missing: an upscaler that failed on the player's graphics card (Real-ESRGAN's
            // black images on GTX 16xx cards) leaves a black image behind, and against a dark texture a black one
            // passes the 20 below (Hot Shot's dark red HOTLEGU: 19.7, drawn black-navy). A colour the game's texture
            // has (a channel's mean 24 or more) of which the large image keeps under a quarter: not used. Over the
            // whole remaster set the lowest any image keeps is 38% (the redrawn STRIPGLOW), the next 69%.
            // TF_HIRES_DARKCHECK=0: not checked.
            static const bool darkCheck = !(std::getenv("TF_HIRES_DARKCHECK") && *std::getenv("TF_HIRES_DARKCHECK") == '0');
            bool lost = false;
            for (int k = 0; darkCheck && counted && k < 3; ++k)
                if (gameSum[k] >= 24.0 * static_cast<double>(counted) && largeSum[k] < 0.25 * gameSum[k])
                    lost = true;
            if (lost)
                std::fprintf(stderr, "[native render] upscale %s: black, or a colour missing (RGB %.0f %.0f %.0f where the game's has %.0f %.0f %.0f) - not used\n",
                             name.c_str(), largeSum[0] / counted, largeSum[1] / counted, largeSum[2] / counted, gameSum[0] / counted, gameSum[1] / counted,
                             gameSum[2] / counted);
            // (TODO 259) Its colours shifted: a GTX 1660 SUPER's set drew the floor plates of Antarctica in pale green and
            // purple bands - the right picture, the same brightness (off the game's by under 20: it passed the test below),
            // but red and blue against green moved. Box-averaged down, a right upscale keeps the game's chroma (red - green,
            // blue - green per texel): over the whole remaster set off by 5.6 on average (median), and only where the game's
            // texture is very colourful itself (flames, rainbows, stars) more than 20 + a quarter of that colourfulness - the
            // three images that do are off by over 20 anyway, not used below. Bands of green and purple of +-20 are caught 98
            // times in 100, of +-10 70 (measured with such bands put on every image of the set). Not used then.
            // TF_HIRES_CHROMACHECK=0: not checked.
            static const bool chromaCheck = !(std::getenv("TF_HIRES_CHROMACHECK") && *std::getenv("TF_HIRES_CHROMACHECK") == '0');
            const double chromaMean = counted ? chromaOff / static_cast<double>(counted) : 0.0;
            const double chromaAllowed = 20.0 + 0.25 * (counted ? gameChroma / static_cast<double>(counted) : 0.0);
            const bool shifted = chromaCheck && counted >= 16u && chromaMean > chromaAllowed;
            if (shifted)
                std::fprintf(stderr, "[native render] upscale %s: its colours are shifted off the game's (red and blue against green off by %.1f, "
                                     "at most %.1f) - not used\n", name.c_str(), chromaMean, chromaAllowed);
            // (TODO 259) Another picture altogether: the same player's Deep Amazon riverbed drew a sci-fi floor with round green
            // lights - a wrong image under the right name, whose colours can be near enough to pass everything above (of the
            // images of a river texture's size, 2 to 5 in 100 do). Box-averaged down, a right upscale follows the game's
            // texture texel by texel: their brightness correlates (median 0.99 over a whole set). Where it does not, the
            // image is the wrong picture if it has a structure of its own: an upscaler that only smoothed a grainy texture
            // away (variation a tenth to a fifth of the game's) correlates little too and is right. A flat game texture with
            // a picture on it likewise. Over two whole good sets (one with hand-redrawn art) it flags only images the rules
            // above reject already. TF_HIRES_STRUCTCHECK=0: not checked.
            static const bool structCheck = !(std::getenv("TF_HIRES_STRUCTCHECK") && *std::getenv("TF_HIRES_STRUCTCHECK") == '0');
            bool otherPicture = false;
            double corr = 0.0, gameVar = 0.0, largeVar = 0.0;
            if (structCheck && counted >= 16u)
            {
                const double n = static_cast<double>(counted);
                gameVar = std::sqrt(std::max(0.0, gl2 / n - (gl / n) * (gl / n)));
                largeVar = std::sqrt(std::max(0.0, ll2 / n - (ll / n) * (ll / n)));
                if (gameVar > 0.0 && largeVar > 0.0)
                    corr = (glll / n - (gl / n) * (ll / n)) / (gameVar * largeVar);
                otherPicture = gameVar >= 5.0 ? corr < 0.3 && largeVar >= 4.0 && largeVar >= 0.25 * gameVar : largeVar > gameVar + 20.0;
            }
            if (otherPicture)
                std::fprintf(stderr, "[native render] upscale %s: another picture than the game's (brightness correlation %.2f, variation %.1f where the game's has %.1f) - not used\n",
                             name.c_str(), corr, largeVar, gameVar);
            // (And its alpha: a translucent layer whose large image has other alpha than the game's -
            // colour under once-transparent texels let through - draws that colour (Alaska's snow
            // overlay showed purple blotches). Off by more than 24 of 255 on average: not used.
            // TF_HIRES_ALPHA=0: not checked.)
            const double meanAlpha = alphaDiff / (static_cast<double>(texture.width) * texture.height);
            static const bool alphaCheck = !(std::getenv("TF_HIRES_ALPHA") && *std::getenv("TF_HIRES_ALPHA") == '0');
            const bool alphaOff = alphaCheck && meanAlpha > 24.0;
            // (Diagnostic: how much of the game's light the large image keeps where the game has it - PROJ
            // 30%. Logged with TF_NATIVE_NAMES; rejecting on it (TF_HIRES_KEPT=1: under 60%) would also throw
            // away JUNGLE_FLOOR (47%) and the user's own redrawn HUD/HQ icons (RM_*, ARMADALOGO...), so it is
            // off by default.)
            static const bool keptCheck = std::getenv("TF_HIRES_KEPT") && *std::getenv("TF_HIRES_KEPT") == '1';
            const double keptShare = light > 0.0 ? kept / light : 1.0;
            const bool lightLost = keptCheck && light > 64.0 * 3.0 && keptShare < 0.6;
            if (log)
                std::fprintf(stderr, "[native render] upscale %s: light kept %.0f%%%s\n", name.c_str(), keptShare * 100.0,
                             lightLost ? " - the game's light lost, not used" : "");
            if (lightLost)
                return false;
            if (log)
                std::fprintf(stderr, "[native render] upscale %s: the large image differs from the game's by %.2f, alpha %.2f%s\n", name.c_str(),
                             mean, meanAlpha, mean > 20.0 ? " - redrawn art, not used" : alphaOff ? " - its alpha is not the game's, not used" : "");
            return !(mean > 20.0 || alphaOff || swapped || lost || shifted || otherPicture);
        }

        // (TODO 44) The large image's colour matched to the game's: per channel, the large image scaled so that,
        // box-averaged down, its mean over the game's opaque texels is the game's. The remaster's upscales kept the
        // pictures but drifted in colour - Deep Amazon's, against the software renderer at the same moments: red
        // 6-17% low, green 4-9%, blue about right (the scene darker and less warm); with the game's own textures
        // the native picture is within 1-4% of it. Gains held to 0.67-1.5, a channel left as it is under 1% off
        // or where the large image has almost nothing of it. Not for the crisp font atlases (our own drawing).
        // TF_HIRES_COLOUR=0: off, the images as they are.
        bool colourOn()
        {
            static const bool on = !(std::getenv("TF_HIRES_COLOUR") && *std::getenv("TF_HIRES_COLOUR") == '0');
            return on;
        }

        void matchColour(std::vector<uint32_t> &px, std::vector<uint32_t> &avg, const Texture &texture, const std::string &name)
        {
            static const bool log = std::getenv("TF_NATIVE_NAMES") != nullptr;
            if (!colourOn())
                return;
            double game[3] = {}, large[3] = {};
            size_t counted = 0;
            for (size_t i = 0; i < avg.size() && i < texture.pixels.size(); ++i)
            {
                const uint32_t g0 = texture.pixels[i], a = avg[i];
                if ((g0 >> 24) < 16u)
                    continue;
                for (int k = 0; k < 3; ++k)
                {
                    game[k] += static_cast<double>((g0 >> (8 * k)) & 0xFFu);
                    large[k] += static_cast<double>((a >> (8 * k)) & 0xFFu);
                }
                ++counted;
            }
            if (counted < 16u)
                return;
            double gain[3] = {1.0, 1.0, 1.0};
            bool any = false;
            for (int k = 0; k < 3; ++k)
            {
                if (large[k] < 4.0 * static_cast<double>(counted) || game[k] < 4.0 * static_cast<double>(counted))
                    continue;
                const double g = std::clamp(game[k] / large[k], 0.67, 1.5);
                if (std::abs(g - 1.0) >= 0.01)
                {
                    gain[k] = g;
                    any = true;
                }
            }
            if (log)
                std::fprintf(stderr, "[native render] upscale %s: colour gains %.3f %.3f %.3f%s\n", name.c_str(), gain[0], gain[1], gain[2],
                             any ? "" : " (as it is)");
            if (!any)
                return;
            uint32_t lut[3][256];
            for (int k = 0; k < 3; ++k)
                for (uint32_t v = 0; v < 256u; ++v)
                    lut[k][v] = static_cast<uint32_t>(std::min(255.0, std::floor(static_cast<double>(v) * gain[k] + 0.5)));
            auto apply = [&](std::vector<uint32_t> &image)
            {
                for (uint32_t &c : image)
                    c = (c & 0xFF000000u) | (lut[2][(c >> 16) & 0xFFu] << 16) | (lut[1][(c >> 8) & 0xFFu] << 8) | lut[0][c & 0xFFu];
            };
            apply(px);
            apply(avg);
        }
    }

    namespace
    {
        // (The game's texture held weakly, 2026-10-28, TODO 59: a request whose texture the capture let
        // go of meanwhile - the HQ's, evicted once a level runs - is dropped, not worked through. Held
        // strongly, the loader spent the level's first half minute on the HQ's backlog, results nobody
        // took, and the level's own images came late and all together.)
        struct Request
        {
            uint64_t ticket;
            std::string name;
            std::weak_ptr<const Texture> game;
            std::chrono::steady_clock::time_point asked{std::chrono::steady_clock::now()}; // (TODO 124: the queue's wait)
        };
        // (TODO 124) Where a request's time goes: in the queue (asked to a loader taking it) and a loader's work
        // (the cache's chain read or the PNG made into one), summed over the requests finished; cache reads apart.
        std::atomic<uint64_t> g_doneCount{0}, g_queueUs{0}, g_workUs{0}, g_workCacheUs{0}, g_doneCache{0};
        std::mutex g_queueMutex;
        std::condition_variable g_queueCv;
        std::deque<Request> g_requests;
        std::vector<UpscaledResult> g_finished;
        bool g_workerStarted = false;
        std::atomic<uint64_t> g_dropped{0};

        // (TODO 102) The heavy part - a PNG decoded, its chain built and compressed: a 4096 image takes ~150 MB
        // and a second or more of a core - on at most g_heavyMax loaders at once; the others go on with the
        // cache's ready chains (a few ms each) meanwhile.
        unsigned g_heavyMax = 1u;
        std::mutex g_heavyMutex;
        std::condition_variable g_heavyCv;
        unsigned g_heavyActive = 0u;
        // (TODO 121) Windows' BACKGROUND mode (TODO 102) only for the heavy part: a loader in it gets the lowest
        // CPU, I/O and memory priority, so while any other program keeps the processor busy - a compile at
        // below-normal priority beside the game, say - it hardly runs at all: the level's images from the
        // compressed cache (a few ms each) waited 25 s and more in the queue (82 queued, 0 decoding), and the
        // trees showed the game's small textures meanwhile (the user's blocky canopies). The loaders now run
        // below normal, as before TODO 102, and enter BACKGROUND mode only for a PNG's decode and compression
        // (TF_HIRES_BACKGROUND=0: never; =all: the whole time, as TODO 102 had it).
        // (TODO 124) And outside BACKGROUND mode at the game's own priority, not below it: on a six-core PC with
        // the game running flat out (the test PC at 240) a below-normal loader took 435-514 ms of wall time
        // for one image from the compressed cache (7 ms at normal), so a tree seen for the first time showed the
        // game's small texture for 1.3-3.8 s (median; 7 s at worst). The cache's reads are a few ms of work each;
        // the PNG decode and compression, the burst TODO 102 is about, still run in BACKGROUND mode.
        // TF_HIRES_PRIO=<n>: another priority (-2..2; -1 = below normal, as TODO 121 had it).
        bool g_backgroundHeavy = true;
        int loaderPriority()
        {
            static const int prio = std::getenv("TF_HIRES_PRIO") ? std::clamp(std::atoi(std::getenv("TF_HIRES_PRIO")), -2, 2) : 0;
            return prio;
        }
        struct HeavySlot
        {
            HeavySlot()
            {
                {
                    std::unique_lock<std::mutex> lock(g_heavyMutex);
                    g_heavyCv.wait(lock, [] { return g_heavyActive < g_heavyMax; });
                    ++g_heavyActive;
                }
#if defined(_WIN32)
                if (g_backgroundHeavy)
                    background = SetThreadPriority(GetCurrentThread(), 0x00010000) != 0; // (THREAD_MODE_BACKGROUND_BEGIN)
#endif
            }
            ~HeavySlot()
            {
#if defined(_WIN32)
                if (background)
                {
                    SetThreadPriority(GetCurrentThread(), 0x00020000); // (THREAD_MODE_BACKGROUND_END)
                    SetThreadPriority(GetCurrentThread(), loaderPriority());
                }
#endif
                {
                    std::lock_guard<std::mutex> lock(g_heavyMutex);
                    --g_heavyActive;
                }
                g_heavyCv.notify_one();
            }
            bool background = false;
            HeavySlot(const HeavySlot &) = delete;
            HeavySlot &operator=(const HeavySlot &) = delete;
        };

        // ---- The large images block-compressed, capped and cached (2026-10-30, TODO 94) ----
        // Uncompressed RGBA, a level's thousand large images took 6-9 GB of video memory and the HQ 15-18 GB
        // (a 4096 x 4096 image with its mips: 85 MB). Now each is BC7 (a quarter of that: 8 bits a texel,
        // colour and alpha alike), its longest side held to what the adapter's video memory allows
        // (policy::largeImageCap: 2048 on a 4 GB card - the chain read from the level of that size, not the
        // game's texture), and kept compressed on disk beside the upscaled tree (TF_HIRES_CACHE=<dir>, default
        // <upscaled>/../upscaled_bc7_cache; =0 none), so the PNG is decoded and compressed once per machine.
        // TF_HIRES_BC=0: RGBA as before (the cap still applies).
        bool bcOn()
        {
            static const bool on = !(std::getenv("TF_HIRES_BC") && *std::getenv("TF_HIRES_BC") == '0') &&
                                   !(std::getenv("TF_NATIVE_UPLOADER") && *std::getenv("TF_NATIVE_UPLOADER") == '0');
            return on;
        }

        std::atomic<uint64_t> g_cacheHits{0}, g_cacheMade{0}, g_pngLoads{0}, g_bcMs{0}, g_loadMs{0}, g_capped{0};

        size_t bcBytes(uint32_t w, uint32_t h) { return static_cast<size_t>((w + 3u) / 4u) * ((h + 3u) / 4u) * 16u; }

        // A level's texels as BC7 blocks (mode 6 alone, linear weights, no least-squares refinement: ~31 Mtexels a
        // second a thread, 51.0 dB on OPT_CHEST; the refinement costs 1.65x for 0.14 dB, the other modes 3-10x for
        // under 1 dB). An edge's partial block repeats its last row / column.
        std::vector<uint8_t> compressBc7(const uint32_t *px, uint32_t w, uint32_t h)
        {
            static std::once_flag once;
            std::call_once(once, [] { bc7enc_compress_block_init(); });
            static const bc7enc_compress_block_params params = [] {
                bc7enc_compress_block_params p;
                bc7enc_compress_block_params_init(&p);
                p.m_max_partitions = 0;
                p.m_try_least_squares = false;
                bc7enc_compress_block_params_init_linear_weights(&p);
                return p;
            }();
            const uint32_t bw = (w + 3u) / 4u, bh = (h + 3u) / 4u;
            std::vector<uint8_t> out(static_cast<size_t>(bw) * bh * 16u);
            uint32_t block[16];
            for (uint32_t by = 0; by < bh; ++by)
                for (uint32_t bx = 0; bx < bw; ++bx)
                {
                    for (uint32_t y = 0; y < 4u; ++y)
                        for (uint32_t x = 0; x < 4u; ++x)
                            block[y * 4u + x] = px[static_cast<size_t>(std::min(h - 1u, by * 4u + y)) * w + std::min(w - 1u, bx * 4u + x)];
                    bc7enc_compress_block(out.data() + (static_cast<size_t>(by) * bw + bx) * 16u, block, &params);
                }
            return out;
        }

        // The cache file: this header, the box average at the game's size (RGBA8, the content check's input),
        // then the chain's levels from `first` (the largest this machine's cap uses: a 4 GB card never compresses
        // or keeps the 4096 level) down to 1 x 1, BC7, one after the other. A larger cap later: made again.
        struct CacheHeader
        {
            uint32_t magic = 0x3742524Eu, version = colourOn() ? 3u : 2u; // 'NRB7' (3: colour matched, TODO 44)
            uint64_t sourceSize = 0, sourceTime = 0;     // the PNG's (an image made again: compressed again)
            uint32_t width = 0, height = 0, levels = 0;
            uint32_t avgWidth = 0, avgHeight = 0, first = 0;
        };

        fs::path cacheRoot()
        {
            static const fs::path root = [] {
                const char *e = std::getenv("TF_HIRES_CACHE");
                if (e && e[0] == '0' && !e[1])
                    return fs::path();
                // (TODO 44: the colour-matched images in a cache of their own, so that builds without the match -
                // other branches on the same machine, TF_HIRES_COLOUR=0 - keep theirs instead of remaking each other's)
                return e && *e ? fs::path(e) : g.upscaled.parent_path() / (colourOn() ? "upscaled_bc7_cache_colour" : "upscaled_bc7_cache");
            }();
            return root;
        }

        // The chain's first level used: the largest whose longest side is within the cap - never below the
        // game's own size, and (compressed) a whole number of blocks.
        uint32_t firstLevel(uint32_t w, uint32_t h, size_t levels, uint32_t cap, const Texture &game, bool blocks)
        {
            uint32_t l = 0;
            while (cap && l + 1u < levels && std::max(w >> l, h >> l) > cap)
            {
                const uint32_t nw = w >> (l + 1u), nh = h >> (l + 1u);
                if (nw < game.width || nh < game.height || (blocks && ((nw & 3u) || (nh & 3u))))
                    break;
                ++l;
            }
            return l;
        }

        bool decodeDds(const fs::path &file, const Texture &game, uint32_t cap, uint32_t &w, uint32_t &h, std::vector<uint32_t> &px)
        {
            std::ifstream f(file, std::ios::binary);
            uint8_t hd[148] = {};
            if (!f.read(reinterpret_cast<char *>(hd), sizeof hd) || std::memcmp(hd, "DDS ", 4) != 0 || std::memcmp(hd + 84, "DX10", 4) != 0)
                return false;
            auto u32 = [&hd](size_t at) { uint32_t v; std::memcpy(&v, hd + at, 4); return v; };
            const uint32_t H = u32(12), W = u32(16), levels = std::max<uint32_t>(1u, u32(28)), dxgi = u32(128);
            if ((dxgi != 98u && dxgi != 99u) || W < game.width || H < game.height || (W & 3u) || (H & 3u) || levels > 16u)
                return false;
            // (the largest level the cap allows: the others are never read)
            const uint32_t first = firstLevel(W, H, levels, cap, game, true);
            uint64_t at = sizeof hd;
            for (uint32_t l = 0; l < first; ++l)
                at += bcBytes(std::max(1u, W >> l), std::max(1u, H >> l));
            w = std::max(1u, W >> first);
            h = std::max(1u, H >> first);
            std::vector<uint8_t> blocks(bcBytes(w, h));
            f.seekg(static_cast<std::streamoff>(at));
            if (!f.read(reinterpret_cast<char *>(blocks.data()), static_cast<std::streamsize>(blocks.size())))
                return false;
            px.assign(static_cast<size_t>(w) * h, 0u);
            const uint32_t bw = (w + 3u) / 4u, bh = (h + 3u) / 4u;
            bc7decomp::color_rgba texels[16];
            for (uint32_t by = 0; by < bh; ++by)
                for (uint32_t bx = 0; bx < bw; ++bx)
                {
                    bc7decomp::unpack_bc7(blocks.data() + (static_cast<size_t>(by) * bw + bx) * 16u, texels);
                    for (uint32_t y = 0; y < 4u && by * 4u + y < h; ++y)
                        for (uint32_t x = 0; x < 4u && bx * 4u + x < w; ++x)
                        {
                            const bc7decomp::color_rgba &c = texels[y * 4u + x];
                            // (A PCSX2 pack keeps the GS's alpha, 0x80 = opaque; this port's large images have it doubled,
                            // as its converted textures do: 1,068 of the user's pack's images measured at half.)
                            const uint32_t alpha = std::min(255u, static_cast<uint32_t>(c.a) * 2u);
                            px[static_cast<size_t>(by * 4u + y) * w + bx * 4u + x] = c.r | (c.g << 8) | (c.b << 16) | (alpha << 24);
                        }
                }
            return true;
        }

        // The game's own mip levels (Texture::gameMips) in the large image's chain: level
        // base + k is the game's level k, and the levels below the last one taken in box-filtered from it.
        // Returned as (level, texels); `last` the last level the game's gave (0: none).
        std::vector<std::pair<size_t, std::vector<uint32_t>>> gameLevels(uint32_t w, uint32_t h, size_t levels, const Texture &game,
                                                                         uint32_t &last)
        {
            std::vector<std::pair<size_t, std::vector<uint32_t>>> out;
            last = 0;
            if (!game.gameMips || game.width == 0u || w < game.width)
                return out;
            uint32_t base = 0;
            while ((game.width << base) < w)
                ++base;
            if ((game.width << base) != w || (game.height << base) != h)
                return out;
            for (uint32_t k = 1; k <= game.gameMips && k - 1u < game.mips.size(); ++k)
            {
                const size_t level = base + k;
                if (level >= levels || static_cast<size_t>(std::max(1u, w >> level)) * std::max(1u, h >> level) != game.mips[k - 1u].size())
                    break;
                out.emplace_back(level, game.mips[k - 1u]);
                last = static_cast<uint32_t>(level);
            }
            if (!last)
                return out;
            Texture below;
            below.width = std::max(1u, w >> last);
            below.height = std::max(1u, h >> last);
            below.pixels = out.back().second;
            buildMips(below);
            for (size_t i = 0; i < below.mips.size() && last + 1u + i < levels; ++i)
                out.emplace_back(last + 1u + i, std::move(below.mips[i]));
            return out;
        }

        // The large image for the game's texture (name, texels): chosen, checked, compressed, capped, its
        // game levels spliced in. False: none (the game's own is drawn).
        bool loadLarge(const std::string &name, const Texture &game, Texture &out, const std::string *forceSet = nullptr,
                       bool *missing = nullptr)
        {
            const auto t0 = std::chrono::steady_clock::now();
            // (TODO 219) None of the name's images made yet (the set is still being made, hd_progress.h): told as such
            // at once - a few file checks, no image decoded - so that it is asked for again once the set has grown.
            if (missing && !forceSet && !g.fontsOnly && g.enabled && g.pack.empty())
                if (auto it = g.byStem.find(upper(name)); it != g.byStem.end())
                {
                    std::error_code mec;
                    bool any = false;
                    for (const std::string &rel : it->second)
                        if (fs::exists(g.upscaled / (rel + ".png"), mec) || !crispImage(rel, g.crispSet).empty())
                        {
                            any = true;
                            break;
                        }
                    if (!any)
                    {
                        *missing = true;
                        return false;
                    }
                }
            const std::string *chosen = chooseCandidate(name, game);
            if (!chosen)
                return false;
            // (forceSet: the key prompts' twin - that set's image or none)
            const std::string &set = forceSet ? *forceSet : g.crispSet;
            if (forceSet && crispImage(*chosen, set).empty())
                return false;
            // (TODO 119: a font atlas of the crisp set in place of its upscaled image - our own drawing of the
            // same glyphs in the same rectangles, so the content check below, made for redrawn art, is not
            // asked of it; its compressed copy kept apart from the upscaled image's.)
            const fs::path crisp = crispImage(*chosen, set);
            fs::path png = crisp.empty() ? (g.fontsOnly ? g.native : g.upscaled) / (*chosen + ".png") : crisp;
            // (TODO 290 (17)) A PCSX2 pack's image in place of the upscaled one: a BC7 DDS taken as it is (its own levels, from
            // the one the cap allows: no decoding), a PNG through the path below (not cached: the pack is the player's).
            fs::path packDds;
            bool fromPack = false;
            if (crisp.empty() && !g.pack.empty())
            {
                const auto it = g.pack.find(*chosen);
                if (it == g.pack.end())
                    return false;
                fromPack = true;
                std::string ext = it->second.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                (ext == ".dds" ? packDds : png) = it->second;
            }
            std::error_code ec;
            const uint64_t srcSize = fs::file_size(packDds.empty() ? png : packDds, ec);
            if (ec)
            {
                if (missing && !forceSet)
                    *missing = true; // (TODO 219: its image not made yet)
                return false;
            }
            const uint64_t srcTime = static_cast<uint64_t>(fs::last_write_time(packDds.empty() ? png : packDds, ec).time_since_epoch().count());
            const uint32_t cap = policy::largeImageCap();
            const fs::path root = bcOn() ? cacheRoot() : fs::path();
            const fs::path cached = root.empty() ? fs::path() : fromPack ? root / "pcsx2_pack" / (*chosen + ".bc7") : crisp.empty() ? root / (*chosen + ".bc7") : root / ("crisp_" + set) / (*chosen + ".bc7");
            if (!crisp.empty())
                std::fprintf(stderr, "[native render] crisp fonts: %s (%s)\n", chosen->c_str(), set.c_str());
            auto looksRight = [&](const std::vector<uint32_t> &avg) { return !crisp.empty() || looksLikeGame(avg, game, name); };
            out.id = game.id;
            out.source = game.source;
            out.upscaled = true;
            uint32_t w = 0, h = 0, first = 0;
            size_t levels = 0;
            bool done = false;
            // (1) The compressed chain from the cache.
            if (!cached.empty())
            {
                std::ifstream f(cached, std::ios::binary);
                CacheHeader hd;
                const CacheHeader want;
                if (f && f.read(reinterpret_cast<char *>(&hd), sizeof hd) && hd.magic == want.magic && hd.version == want.version &&
                    hd.sourceSize == srcSize && hd.sourceTime == srcTime && hd.avgWidth == game.width && hd.avgHeight == game.height &&
                    hd.width >= game.width && hd.height >= game.height && hd.levels > 0u && hd.levels <= 16u && hd.first < hd.levels && !(hd.width & 3u) && !(hd.height & 3u) &&
                    firstLevel(hd.width, hd.height, hd.levels, cap, game, true) >= hd.first)
                {
                    std::vector<uint32_t> avg(static_cast<size_t>(hd.avgWidth) * hd.avgHeight);
                    if (f.read(reinterpret_cast<char *>(avg.data()), static_cast<std::streamsize>(avg.size() * 4u)))
                    {
                        if (!looksRight(avg))
                            return false;
                        w = hd.width;
                        h = hd.height;
                        levels = hd.levels;
                        first = firstLevel(w, h, levels, cap, game, true);
                        uint64_t at = sizeof hd + avg.size() * 4u;
                        for (uint32_t l = hd.first; l < first; ++l)
                            at += bcBytes(std::max(1u, w >> l), std::max(1u, h >> l));
                        f.seekg(static_cast<std::streamoff>(at));
                        out.blocks.clear();
                        bool ok = static_cast<bool>(f);
                        for (size_t l = first; ok && l < levels; ++l)
                        {
                            std::vector<uint8_t> b(bcBytes(std::max(1u, w >> l), std::max(1u, h >> l)));
                            ok = static_cast<bool>(f.read(reinterpret_cast<char *>(b.data()), static_cast<std::streamsize>(b.size())));
                            out.blocks.push_back(std::move(b));
                        }
                        if (ok)
                        {
                            done = true;
                            out.format = 1u;
                            ++g_cacheHits;
                        }
                        else
                            out.blocks.clear();
                    }
                }
            }
            // (2) The PNG: checked, its chain built (and compressed, and cached).
            if (!done)
            {
                const HeavySlot slot; // (TODO 102: at most g_heavyMax of these at once)
                std::vector<uint32_t> px;
                if (!packDds.empty() ? !decodeDds(packDds, game, cap, w, h, px) : !load(png, w, h, px))
                {
                    if (!packDds.empty())
                        std::fprintf(stderr, "[native render] HD texture pack: %s is not a BC7 DDS this can use\n", packDds.filename().string().c_str());
                    return false;
                }
                if (w < game.width || h < game.height)
                    return false;
                ++g_pngLoads;
                std::vector<uint32_t> avg = boxAverage(px, w, h, game.width, game.height);
                if (!looksRight(avg))
                    return false;
                if (crisp.empty())
                    matchColour(px, avg, game, name); // (the corrected box average is what the cache keeps)
                Texture full;
                full.width = w;
                full.height = h;
                full.pixels = std::move(px);
                buildMips(full);
                levels = 1u + full.mips.size();
                const bool compress = bcOn() && !(w & 3u) && !(h & 3u);
                first = firstLevel(w, h, levels, cap, game, compress);
                if (compress)
                {
                    const auto c0 = std::chrono::steady_clock::now();
                    std::vector<std::vector<uint8_t>> all;
                    all.reserve(levels - first);
                    for (size_t l = first; l < levels; ++l)
                    {
                        const std::vector<uint32_t> &lv = l ? full.mips[l - 1u] : full.pixels;
                        all.push_back(compressBc7(lv.data(), std::max(1u, w >> l), std::max(1u, h >> l)));
                    }
                    g_bcMs += static_cast<uint64_t>(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count());
                    if (!cached.empty())
                    {
                        // (Written whole to a file of its own, then renamed into place: a reader never sees half.)
                        fs::create_directories(cached.parent_path(), ec);
                        fs::path tmp = cached;
                        tmp += ".tmp" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
                        {
                            std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
                            CacheHeader hd;
                            hd.sourceSize = srcSize;
                            hd.sourceTime = srcTime;
                            hd.width = w;
                            hd.height = h;
                            hd.levels = static_cast<uint32_t>(levels);
                            hd.first = first;
                            hd.avgWidth = game.width;
                            hd.avgHeight = game.height;
                            o.write(reinterpret_cast<const char *>(&hd), sizeof hd);
                            o.write(reinterpret_cast<const char *>(avg.data()), static_cast<std::streamsize>(avg.size() * 4u));
                            for (const auto &b : all)
                                o.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
                        }
                        fs::rename(tmp, cached, ec);
                        if (ec)
                            fs::remove(tmp, ec);
                        else
                            ++g_cacheMade;
                    }
                    out.format = 1u;
                    out.blocks = std::move(all);
                }
                else
                {
                    out.format = 0u;
                    out.pixels = first ? std::move(full.mips[first - 1u]) : std::move(full.pixels);
                    out.mips.assign(std::make_move_iterator(full.mips.begin() + first), std::make_move_iterator(full.mips.end()));
                }
            }
            if (first)
                ++g_capped;
            out.width = std::max(1u, w >> first);
            out.height = std::max(1u, h >> first);
            levels -= first;
            // The game's own levels in (what the GS reads past level 0 stays the game's).
            uint32_t last = 0;
            for (auto &[level, texels] : gameLevels(out.width, out.height, levels, game, last))
            {
                if (out.format == 1u)
                    out.blocks[level] = compressBc7(texels.data(), std::max(1u, out.width >> level), std::max(1u, out.height >> level));
                else if (level >= 1u && level - 1u < out.mips.size())
                    out.mips[level - 1u] = std::move(texels);
            }
            out.gameMips = last;
            // The key prompts' twin (TODO 119): the same atlas with the keyboard's keys, its own GPU copy (an id
            // apart from the game's ids, which count up from 1).
            if (!forceSet && !g.keysSet.empty())
            {
                auto keys = std::make_shared<Texture>();
                if (loadLarge(name, game, *keys, &g.keysSet))
                {
                    keys->id = game.id | (1ull << 62);
                    out.keys = std::move(keys);
                }
            }
            if (!forceSet && !g.xboxSet.empty())
            {
                auto xbox = std::make_shared<Texture>();
                if (loadLarge(name, game, *xbox, &g.xboxSet))
                {
                    xbox->id = game.id | (1ull << 61);
                    out.xbox = std::move(xbox);
                }
            }
            if (!forceSet && out.xbox && !g.xbox360Set.empty())
            {
                auto xbox360 = std::make_shared<Texture>();
                if (loadLarge(name, game, *xbox360, &g.xbox360Set))
                {
                    xbox360->id = game.id | (1ull << 60);
                    out.xbox360 = std::move(xbox360);
                }
            }
            g_loadMs += static_cast<uint64_t>(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            return true;
        }

        void work()
        {
            for (;;)
            {
                Request r;
                {
                    std::unique_lock<std::mutex> lock(g_queueMutex);
                    g_queueCv.wait(lock, [] { return !g_requests.empty(); });
                    r = std::move(g_requests.front());
                    g_requests.pop_front();
                }
                const auto taken = std::chrono::steady_clock::now();
                const uint64_t pngsBefore = g_pngLoads.load(std::memory_order_relaxed);
                const std::shared_ptr<const Texture> game = r.game.lock();
                // (Told back either way - let go of meanwhile, or no large image - so that the other draws
                // waiting on the same ticket ask again or stop waiting: TODO 94's sharing, nr_capture.cpp.)
                UpscaledResult done;
                done.ticket = r.ticket;
                if (!game)
                {
                    ++g_dropped; // (let go of meanwhile: not worked through)
                    done.dropped = true;
                }
                else
                {
                    auto large = std::make_shared<Texture>();
                    bool missing = false;
                    if (loadLarge(r.name, *game, *large, nullptr, &missing))
                    {
                        large->fallback = game; // (drawn until the large one is on the GPU)
                        if (large->keys)
                            std::const_pointer_cast<Texture>(large->keys)->fallback = game;
                        if (large->xbox)
                            std::const_pointer_cast<Texture>(large->xbox)->fallback = game;
                        if (large->xbox360)
                            std::const_pointer_cast<Texture>(large->xbox360)->fallback = game;
                        done.large = std::move(large);
                    }
                    else
                        done.missing = missing;
                }
                {
                    const auto end = std::chrono::steady_clock::now();
                    const uint64_t workUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - taken).count());
                    g_queueUs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(taken - r.asked).count());
                    g_workUs += workUs;
                    ++g_doneCount;
                    if (g_pngLoads.load(std::memory_order_relaxed) == pngsBefore && done.large)
                    {
                        g_workCacheUs += workUs;
                        ++g_doneCache;
                    }
                }
                std::lock_guard<std::mutex> lock(g_queueMutex);
                g_finished.push_back(std::move(done));
            }
        }
    }

    void requestUpscaled(uint64_t ticket, const std::string &name, std::shared_ptr<const Texture> game)
    {
        if (!game || name.empty())
            return;
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (!g_workerStarted)
        {
            // A few loaders (a level's thousand images: one thread took a minute and more), below the
            // game's threads in priority. TF_HIRES_THREADS=<n> (default: half of the hardware
            // threads, 1 to 8).
            g_workerStarted = true;
            if (!g.ready)
                build(); // (the index made once, before the loaders share it)
            const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
            const char *e = std::getenv("TF_HIRES_THREADS");
            // (2026-10-29, TODO 57: half the hardware threads, at most 8 - with a quarter, at most 4, an HQ
            // screen's ~60 large images took seconds to come in at full speed.)
            // (2026-10-31, TODO 102: a warp froze a player's whole PC for seconds - not reproduced on the
            // workers, but a place's first visit decodes and compresses its large images on every loader at
            // once: all of them at 100% for seconds, ~150,000-400,000 fresh pages a second, the disk read
            // flat out - the one burst a warp makes that a smaller PC would feel. Now: at most 6, and
            // never more than the hardware threads less 4 (the game's own and the desktop's); each in
            // Windows' BACKGROUND mode - lowest CPU, I/O and memory priority, so any other program, the
            // desktop and the game's own threads always go first; and the PNG decode + compression on at
            // most half of them at once (TF_HIRES_HEAVY=<n>). TF_HIRES_BACKGROUND=0: below normal, as before.)
            const unsigned byMachine = std::max(1u, std::min(hw / 2u, hw > 4u ? hw - 4u : 1u));
            const unsigned n = std::clamp(e && *e ? static_cast<unsigned>(std::atoi(e)) : byMachine, 1u, e && *e ? 16u : 6u);
            const char *h = std::getenv("TF_HIRES_HEAVY");
            g_heavyMax = std::clamp(h && *h ? static_cast<unsigned>(std::atoi(h)) : (n + 1u) / 2u, 1u, n);
            // (TODO 121: BACKGROUND mode only while decoding and compressing a PNG - HeavySlot; the whole time
            // starved the cache's reads behind any busy program. TF_HIRES_BACKGROUND=0: never, =all: always.)
            const char *bg = std::getenv("TF_HIRES_BACKGROUND");
            const bool backgroundAll = bg && std::string(bg) == "all";
            g_backgroundHeavy = !(bg && *bg == '0') && !backgroundAll;
            for (unsigned k = 0; k < n; ++k)
                std::thread([backgroundAll] {
#if defined(_WIN32)
                    // (THREAD_MODE_BACKGROUND_BEGIN 0x10000, else THREAD_PRIORITY_BELOW_NORMAL)
                    if (!backgroundAll || !SetThreadPriority(GetCurrentThread(), 0x00010000))
                        SetThreadPriority(GetCurrentThread(), loaderPriority());
#endif
                    ps2_thread_placement::place(ps2_thread_placement::Role::Helper); // (PS2X_THREAD_PLACEMENT)
                    work();
                }).detach();
            std::fprintf(stderr, "[native render] upscaled images: %u loader threads (%u decoding at once, %s)\n", n, g_heavyMax,
                         backgroundAll ? "background priority" : g_backgroundHeavy ? "the game's priority, background while decoding a PNG" : "the game's priority");
        }
        // (TODO 186) A font atlas with a crisp image goes to the front of the queue: a menu's text was drawn with the
        // game's letters for as long as the place's other large images (an HQ screen's ~60, a level's hundreds after a
        // start or a state) came in before it - on the first launch the old round button symbols were seen.
        bool font = false;
        if (!g.crispSet.empty() || !g.keysSet.empty())
            if (auto it = g.byStem.find(upper(name)); it != g.byStem.end())
                for (const std::string &rel : it->second)
                    if (!crispImage(rel, g.crispSet).empty() || !crispImage(rel, g.keysSet).empty())
                    {
                        font = true;
                        break;
                    }
        if (font)
            g_requests.push_front(Request{ticket, name, std::move(game)});
        else
            g_requests.push_back(Request{ticket, name, std::move(game)});
        g_queueCv.notify_one();
    }

    // (TODO 348 (b): in step with key_text's words for the sticks - the symbols switch when they do)
    bool keyPromptsShown() { return key_text::symbolsAsKeys(ps2_host_input::keyboardPromptsShown()); }
    bool xboxPromptsShown()
    {
        return useXboxPrompts(g_padPrompts.load(std::memory_order_relaxed),
                              ps2_host_input::lastPadStyle() == ps2_host_input::PadStyle::PlayStation,
                              ps2_host_input::keyboardPrompts(), g.ps2Symbols);
    }
    bool xbox360PromptsShown() { return ps2_host_input::xbox360Pads(); }

    void prepareHires()
    {
        // (TF_RENDERER=emulated draws without them: nothing to make)
        if (const char *renderer = std::getenv("TF_RENDERER"); renderer && std::string(renderer) != "native")
            return;
        std::thread([] {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            if (!g.ready)
                build();
        }).detach();
    }

    bool hasUpscaled(const std::string &name)
    {
        if (name.empty())
            return false;
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (!g.ready)
            build();
        return g.enabled && g.byStem.find(upper(name)) != g.byStem.end();
    }

    // (The F1 menu's "Open texture folder") The folder the HD images are loaded from: the one in use once the index is
    // made, else where it would be looked for (no scan here). Empty with TF_HIRES=0 or none anywhere.
    std::filesystem::path hdTextureFolder()
    {
        if (const char *off = std::getenv("TF_HIRES"); off && *off == '0')
            return {};
        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            if (g.ready)
                return g.upscaled;
        }
        return resolveUpscaled();
    }

    // (F1, "Sharp text") Whether the sharp font atlases exist where the game looks for them (TF_CRISP_DIR, else
    // crisp_fonts beside the HD set; the developer's tree last, as chooseCrisp). False: the old letters are drawn.
    std::string hdSetAside() { return setAsideWhy(resolveUpscaled()); }

    bool crispFontsFound()
    {
        std::error_code ec;
        std::vector<fs::path> dirs;
        if (const char *dir = std::getenv("TF_CRISP_DIR"); dir && *dir)
            dirs.push_back(fs::path(dir));
        else
        {
            if (const fs::path up = resolveUpscaled(); !up.empty())
                dirs.push_back(up.parent_path() / "crisp_fonts");
            if (!devTree().empty())
                dirs.push_back(devTree() / "crisp_fonts");
        }
        for (const fs::path &d : dirs)
            for (const char *set : {"text_ps5", "text_ps2", "ps5"})
                if (fs::is_directory(d / set, ec))
                    return true;
        return false;
    }

    std::vector<UpscaledResult> takeUpscaled()
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        std::vector<UpscaledResult> out;
        out.swap(g_finished);
        return out;
    }

    void largeImageTimes(uint64_t &done, uint64_t &queueUs, uint64_t &workUs, uint64_t &doneCache, uint64_t &workCacheUs)
    {
        done = g_doneCount.load();
        queueUs = g_queueUs.load();
        workUs = g_workUs.load();
        doneCache = g_doneCache.load();
        workCacheUs = g_workCacheUs.load();
    }

    void largeImageActivity(unsigned &queued, unsigned &heavy, uint64_t &compressed, uint64_t &pngs)
    {
        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            queued = static_cast<unsigned>(g_requests.size());
        }
        {
            std::lock_guard<std::mutex> lock(g_heavyMutex);
            heavy = g_heavyActive;
        }
        compressed = g_cacheMade.load(std::memory_order_relaxed);
        pngs = g_pngLoads.load(std::memory_order_relaxed);
    }

    std::string largeImageStats()
    {
        char line[256];
        std::snprintf(line, sizeof line, "%llu from the compressed cache, %llu PNGs decoded (%llu cached), %llu capped; %llu ms compressing, %llu ms loading in all",
                      static_cast<unsigned long long>(g_cacheHits.load()), static_cast<unsigned long long>(g_pngLoads.load()),
                      static_cast<unsigned long long>(g_cacheMade.load()), static_cast<unsigned long long>(g_capped.load()),
                      static_cast<unsigned long long>(g_bcMs.load()), static_cast<unsigned long long>(g_loadMs.load()));
        return line;
    }

    bool replaceWithUpscaled(const std::string &name, Texture &texture)
    {
        // (The RGBA path, as before TODO 94's compression: the large image at full size, no mips.)
        if (!g.ready)
            build();
        const std::string *chosen = chooseCandidate(name, texture);
        if (!chosen)
            return false;
        uint32_t w = 0, h = 0;
        std::vector<uint32_t> px;
        if (!load(g.upscaled / (*chosen + ".png"), w, h, px) || w < texture.width || h < texture.height)
            return false;
        if (!looksLikeGame(boxAverage(px, w, h, texture.width, texture.height), texture, name))
            return false;
        texture.width = w;
        texture.height = h;
        texture.pixels = std::move(px);
        texture.upscaled = true;
        return true;
    }
}
