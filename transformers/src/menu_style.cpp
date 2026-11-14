// The menu's look (menu_style.h).

#include "menu_style.h"

#include "imgui.h"
#include "raylib.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace menu_style
{
    namespace
    {
        Fonts g_fonts;
        bool g_prepared = false;
        std::string g_help;

        constexpr ImU32 kRed = IM_COL32(214, 32, 47, 255);
        constexpr ImU32 kRedDark = IM_COL32(110, 14, 24, 255);
        constexpr ImU32 kBlue = IM_COL32(54, 209, 255, 255);
        constexpr ImU32 kBlueDim = IM_COL32(54, 209, 255, 70);
        constexpr ImU32 kAmber = IM_COL32(255, 181, 71, 255);
        constexpr ImU32 kText = IM_COL32(232, 236, 242, 255);
        constexpr ImU32 kDim = IM_COL32(138, 148, 166, 255);
        constexpr ImU32 kPanel = IM_COL32(18, 22, 29, 240);
        constexpr ImU32 kPanelLift = IM_COL32(30, 37, 48, 255);
        constexpr ImU32 kLine = IM_COL32(48, 58, 72, 255);

        ImVec4 v(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

        // (TODO 212) The controller's focus over a page (navBegin .. navEnd).
        struct Nav
        {
            bool inPage = false;
            NavInput in;
            int index = 0;          // widgets drawn so far this frame
            int count = 0;          // ... and the frame before
            int focus = 0;
            bool scroll = false;    // bring the focused widget into view
            bool hold = false;      // a row held: Left / Right step it
            int holdId = -1, holdValue = 0;
            bool backTaken = false;
            bool focusNext = false; // navFocusNext
            int returnFocus = -1;   // where Circle takes the focus back to
        } g_nav;

        // The widget being drawn: its place in the focus order, and whether it has the focus.
        int navItem()
        {
            const int id = g_nav.index++;
            if (g_nav.inPage && g_nav.focusNext)
            {
                g_nav.focusNext = false;
                if (g_nav.focus != id)
                {
                    g_nav.returnFocus = g_nav.focus;
                    g_nav.focus = id;
                }
            }
            return id;
        }

        bool navFocused(int id) { return g_nav.inPage && id == g_nav.focus; }

        // Cross for the focused widget: taken once.
        bool navTakeConfirm()
        {
            if (!g_nav.in.confirm)
                return false;
            g_nav.in.confirm = false;
            return true;
        }

        // The focused widget's frame (and the page scrolled to it).
        void navMark(ImDrawList *dl, const ImVec2 &a, const ImVec2 &b, bool held)
        {
            dl->AddRect(ImVec2(a.x - 1.0f, a.y - 1.0f), ImVec2(b.x + 1.0f, b.y + 1.0f), held ? kAmber : kBlue, 3.0f, 0, 2.0f);
            if (g_nav.scroll)
            {
                g_nav.scroll = false;
                // (only when it is out of the page's view: then to its middle)
                const ImVec2 at = ImGui::GetWindowPos();
                const float top = at.y, bottom = at.y + ImGui::GetWindowHeight();
                if (a.y < top || b.y > bottom)
                    ImGui::SetScrollFromPosY((a.y + b.y) * 0.5f - at.y, 0.5f);
            }
        }

        fs::path findFont(const char *name)
        {
            std::error_code e;
            for (const fs::path &dir : {fs::current_path(e) / "fonts", fs::path(GetApplicationDirectory()) / "fonts"})
                if (fs::exists(dir / name, e))
                    return dir / name;
            if (std::strcmp(name, "Bahnschrift.ttf") == 0 && fs::exists("C:/Windows/Fonts/bahnschrift.ttf", e))
                return "C:/Windows/Fonts/bahnschrift.ttf";
#if defined(__linux__)
            // The system's own sans (SteamOS / Arch, Debian / Ubuntu, Fedora paths) in place of the Windows fonts: else
            // ImGui's built-in pixel font is scaled up, blocky.
            const bool bold = std::strcmp(name, "Bahnschrift.ttf") != 0;
            for (const char *dir : {"/usr/share/fonts/noto/", "/usr/share/fonts/truetype/noto/", "/usr/share/fonts/google-noto/"})
                if (const fs::path p = fs::path(dir) / (bold ? "NotoSans-Bold.ttf" : "NotoSans-Regular.ttf"); fs::exists(p, e))
                    return p;
            for (const char *dir : {"/usr/share/fonts/TTF/", "/usr/share/fonts/truetype/dejavu/", "/usr/share/fonts/dejavu-sans-fonts/"})
                if (const fs::path p = fs::path(dir) / (bold ? "DejaVuSans-Bold.ttf" : "DejaVuSans.ttf"); fs::exists(p, e))
                    return p;
#endif
            return {};
        }

        ImFont *load(const char *name)
        {
            const fs::path file = findFont(name);
            if (file.empty())
                return nullptr;
            ImFont *font = ImGui::GetIO().Fonts->AddFontFromFileTTF(file.string().c_str(), 20.0f);
            if (font)
                std::fprintf(stderr, "[menu] font %s\n", file.string().c_str());
            return font;
        }

        // A box with its top-left and bottom-right corners cut off: the panels' and tabs' shape.
        void chamfered(ImDrawList *dl, ImVec2 a, ImVec2 b, float cut, ImU32 fill)
        {
            const ImVec2 points[6] = {{a.x + cut, a.y}, {b.x, a.y}, {b.x, b.y - cut}, {b.x - cut, b.y}, {a.x, b.y}, {a.x, a.y + cut}};
            dl->AddConvexPolyFilled(points, 6, fill);
        }

        void arrow(ImDrawList *dl, ImVec2 centre, float size, bool left, ImU32 col)
        {
            const float h = size * 0.5f;
            if (left)
                dl->AddTriangleFilled({centre.x - h * 0.6f, centre.y}, {centre.x + h * 0.5f, centre.y - h}, {centre.x + h * 0.5f, centre.y + h}, col);
            else
                dl->AddTriangleFilled({centre.x + h * 0.6f, centre.y}, {centre.x - h * 0.5f, centre.y - h}, {centre.x - h * 0.5f, centre.y + h}, col);
        }

        std::string upper(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return s;
        }
    }

    void prepare()
    {
        if (g_prepared || !ImGui::GetCurrentContext())
            return;
        g_prepared = true;
        g_fonts.text = load("Bahnschrift.ttf");
        g_fonts.heading = load("Nulshock Bd.otf");
        g_fonts.title = load("Transformers Movie.ttf");
        ImFont *fallback = ImGui::GetIO().Fonts->Fonts.empty() ? nullptr : ImGui::GetIO().Fonts->Fonts[0];
        if (!g_fonts.text)
            g_fonts.text = fallback;
        if (!g_fonts.heading)
            g_fonts.heading = g_fonts.text;
        if (!g_fonts.title)
            g_fonts.title = g_fonts.heading;
    }

    const Fonts &fonts() { return g_fonts; }

#if defined(__linux__)
    namespace
    {
        // A handheld PC (the BIOS's chassis type: 8 portable - the Steam Deck, 11 hand held, 30 tablet, 32 detachable):
        // a 7-inch screen, the text a third larger.
        float handheldScale()
        {
            static const float scale = []
            {
                std::ifstream in("/sys/class/dmi/id/chassis_type");
                int type = 0;
                in >> type;
                return type == 8 || type == 11 || type == 30 || type == 32 ? 1.35f : 1.0f;
            }();
            return scale;
        }
    }
    float textSize() { return std::clamp(static_cast<float>(GetScreenHeight()) / 42.0f * handheldScale(), 15.0f, 40.0f); }
#else
    float textSize() { return std::clamp(static_cast<float>(GetScreenHeight()) / 42.0f, 15.0f, 40.0f); }
#endif

    unsigned int colour(const char *name)
    {
        const std::string n = name;
        if (n == "red")
            return kRed;
        if (n == "red_dark")
            return kRedDark;
        if (n == "blue")
            return kBlue;
        if (n == "amber")
            return kAmber;
        if (n == "dim")
            return kDim;
        if (n == "panel")
            return kPanel;
        if (n == "line")
            return kLine;
        return kText;
    }

    void applyStyle()
    {
        ImGuiStyle &s = ImGui::GetStyle();
        const float k = textSize() / 18.0f;
        s.FontScaleMain = 1.0f;
        s.WindowRounding = 0.0f;
        s.ChildRounding = 0.0f;
        s.FrameRounding = 2.0f * k;
        s.PopupRounding = 2.0f * k;
        s.GrabRounding = 2.0f * k;
        s.ScrollbarRounding = 2.0f * k;
        s.TabRounding = 0.0f;
        s.WindowBorderSize = 0.0f;
        s.ChildBorderSize = 0.0f;
        s.FrameBorderSize = 1.0f;
        s.PopupBorderSize = 1.0f;
        s.WindowPadding = ImVec2(0.0f, 0.0f);
        s.FramePadding = ImVec2(10.0f * k, 6.0f * k);
        s.ItemSpacing = ImVec2(10.0f * k, 8.0f * k);
        s.ItemInnerSpacing = ImVec2(8.0f * k, 6.0f * k);
        s.ScrollbarSize = 10.0f * k;
        s.SeparatorTextBorderSize = 2.0f;
        ImVec4 *c = s.Colors;
        c[ImGuiCol_Text] = v(kText);
        c[ImGuiCol_TextDisabled] = v(kDim);
        c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_PopupBg] = v(IM_COL32(16, 20, 27, 250));
        c[ImGuiCol_Border] = v(kLine);
        c[ImGuiCol_FrameBg] = v(IM_COL32(12, 15, 20, 255));
        c[ImGuiCol_FrameBgHovered] = v(kPanelLift);
        c[ImGuiCol_FrameBgActive] = v(kPanelLift);
        c[ImGuiCol_Button] = v(IM_COL32(26, 32, 42, 255));
        c[ImGuiCol_ButtonHovered] = v(IM_COL32(40, 50, 64, 255));
        c[ImGuiCol_ButtonActive] = v(kRedDark);
        c[ImGuiCol_Header] = v(IM_COL32(214, 32, 47, 90));
        c[ImGuiCol_HeaderHovered] = v(IM_COL32(54, 209, 255, 50));
        c[ImGuiCol_HeaderActive] = v(IM_COL32(214, 32, 47, 140));
        c[ImGuiCol_CheckMark] = v(kBlue);
        c[ImGuiCol_SliderGrab] = v(kBlue);
        c[ImGuiCol_SliderGrabActive] = v(kRed);
        c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_ScrollbarGrab] = v(kLine);
        c[ImGuiCol_ScrollbarGrabHovered] = v(kBlueDim);
        c[ImGuiCol_ScrollbarGrabActive] = v(kBlue);
        c[ImGuiCol_Separator] = v(kLine);
        c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_TableRowBgAlt] = v(IM_COL32(255, 255, 255, 8));
        c[ImGuiCol_TableBorderLight] = v(kLine);
        c[ImGuiCol_TableBorderStrong] = v(kLine);
        c[ImGuiCol_NavCursor] = v(kBlue);
        c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
    }

    bool sideTab(const char *label, bool selected, float width)
    {
        const float size = textSize();
        const float height = size * 2.3f;
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const ImVec2 b(a.x + width, a.y + height);
        const bool clicked = ImGui::InvisibleButton(label, ImVec2(width, height));
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const float cut = size * 0.55f;
        if (selected)
        {
            chamfered(dl, a, b, cut, kRed);
            dl->AddRectFilled(ImVec2(b.x - size * 0.18f, a.y), ImVec2(b.x, b.y - cut), IM_COL32(255, 255, 255, 60));
        }
        else if (hovered)
        {
            chamfered(dl, a, b, cut, kPanelLift);
            dl->AddRectFilled(a, ImVec2(a.x + size * 0.15f, b.y), kBlue);
        }
        ImFont *font = fonts().heading;
        const float fontSize = size * 0.78f;
        const std::string text = upper(label);
        const ImVec2 extent = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str());
        dl->AddText(font, fontSize, ImVec2(a.x + size * 0.9f, a.y + (height - extent.y) * 0.5f),
                    selected ? IM_COL32(255, 255, 255, 255) : (hovered ? kText : kDim), text.c_str());
        return clicked;
    }

    void heading(const char *text)
    {
        const float size = textSize();
        ImGui::Dummy(ImVec2(0.0f, size * 0.35f));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImFont *font = fonts().heading;
        const std::string t = upper(text);
        const float fontSize = size * 0.72f;
        const ImVec2 extent = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, t.c_str());
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(at, ImVec2(at.x + size * 0.22f, at.y + extent.y), kRed);
        dl->AddText(font, fontSize, ImVec2(at.x + size * 0.55f, at.y), kBlue, t.c_str());
        const float lineX = at.x + size * 0.55f + extent.x + size * 0.6f;
        const float right = at.x + ImGui::GetContentRegionAvail().x;
        if (lineX < right)
            dl->AddLine(ImVec2(lineX, at.y + extent.y * 0.55f), ImVec2(right, at.y + extent.y * 0.55f), kLine, 1.0f);
        ImGui::Dummy(ImVec2(0.0f, extent.y + size * 0.25f));
    }

    namespace
    {
        // (TODO 286 d) The control a row of choices is drawn as.
        enum class ControlKind { Switch, Segments, Slider, List };
        bool sameWord(const std::string &a, const char *b)
        {
            if (a.size() != std::strlen(b))
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            return true;
        }
        int onIndexOf(const std::vector<std::string> &c)
        {
            return c.size() == 2 && sameWord(c[1], "On") ? 1 : 0;
        }
        // A number at the start ("90", "100%", "1.5x", "60 fps"), else false.
        bool leadingNumber(const std::string &t, double &v)
        {
            char *end = nullptr;
            v = std::strtod(t.c_str(), &end);
            return end != t.c_str();
        }
        ControlKind controlKindOf(const std::vector<std::string> &c, float width, float size)
        {
            static const bool old = [] { const char *e = std::getenv("TF_MENU_CONTROLS"); return e && std::string(e) == "old"; }();
            if (old)
                return ControlKind::List;
            const size_t n = c.size();
            if (n == 2 && ((sameWord(c[0], "Off") && sameWord(c[1], "On")) || (sameWord(c[0], "On") && sameWord(c[1], "Off"))))
                return ControlKind::Switch;
            if (n >= 5)
            {
                // a range: every choice a number, in order (one word such as "Off" or "Auto" first allowed)
                size_t first = 0;
                double v = 0.0, last = 0.0;
                if (!leadingNumber(c[0], v))
                    first = 1;
                bool range = n - first >= 5;
                for (size_t i = first; range && i < n; ++i)
                {
                    if (!leadingNumber(c[i], v) || (i > first && v <= last))
                        range = false;
                    last = v;
                }
                if (range)
                    return ControlKind::Slider;
            }
            if (n >= 2 && n <= 4)
            {
                ImFont *f = fonts().text;
                const float segW = (width - 3.0f * static_cast<float>(n - 1)) / static_cast<float>(n);
                bool fits = true;
                for (const std::string &t : c)
                    fits = fits && f->CalcTextSizeA(size * 0.86f, FLT_MAX, 0.0f, t.c_str()).x + size * 0.8f <= segW;
                if (fits)
                    return ControlKind::Segments;
            }
            return ControlKind::List;
        }
    }

    int choiceRow(const char *id, const std::string &label, const std::vector<std::string> &choices, int current,
                  bool enabled, const char *badge, const std::string &help)
    {
        const float size = textSize();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = size * 2.0f;
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const ImVec2 b(a.x + width, a.y + height);
        ImGui::PushID(id);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const int count = static_cast<int>(choices.size());
        // (TODO 212) The controller: Cross flips an On / Off row, holds a row of more choices (Left / Right step it,
        // Cross sets it, Circle lets go), Circle at a held row lets go unchanged.
        int chosen = -1;
        const int navId = navItem();
        const bool focused = navFocused(navId);
        const bool held = focused && g_nav.hold && g_nav.holdId == navId;
        if (focused && enabled && count > 0)
        {
            if (held)
            {
                if (g_nav.in.left)
                    g_nav.holdValue = std::max(0, g_nav.holdValue - 1);
                if (g_nav.in.right)
                    g_nav.holdValue = std::min(count - 1, g_nav.holdValue + 1);
                if (navTakeConfirm())
                {
                    if (g_nav.holdValue != current)
                        chosen = g_nav.holdValue;
                    g_nav.hold = false;
                }
                else if (g_nav.in.back)
                {
                    g_nav.in.back = false;
                    g_nav.backTaken = true;
                    g_nav.hold = false;
                }
            }
            else if (navTakeConfirm())
            {
                if (count == 2)
                    chosen = 1 - std::clamp(current, 0, 1);
                else
                {
                    g_nav.hold = true;
                    g_nav.holdId = navId;
                    g_nav.holdValue = std::clamp(current, 0, count - 1);
                }
            }
        }
        const bool holding = held && g_nav.hold;
        if (holding)
            current = g_nav.holdValue; // (shown as it would be set)
        if (focused)
        {
            navMark(dl, a, b, holding);
            setHelp(help);
        }
        const bool rowHovered = ImGui::IsMouseHoveringRect(a, b);
        if (rowHovered)
        {
            dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 10));
            dl->AddRectFilled(a, ImVec2(a.x + 3.0f, b.y), kBlue);
            setHelp(help);
        }
        dl->AddLine(ImVec2(a.x, b.y - 1.0f), ImVec2(b.x, b.y - 1.0f), IM_COL32(255, 255, 255, 14));
        ImFont *textFont = fonts().text;
        const float labelSize = size;
        const ImVec2 labelExtent = textFont->CalcTextSizeA(labelSize, FLT_MAX, 0.0f, label.c_str());
        const float labelY = a.y + (height - labelExtent.y) * 0.5f;
        dl->AddText(textFont, labelSize, ImVec2(a.x + size * 0.7f, labelY), enabled ? kText : kDim, label.c_str());
        if (badge && *badge)
        {
            ImFont *h = fonts().heading;
            const float bs = size * 0.5f;
            const ImVec2 be = h->CalcTextSizeA(bs, FLT_MAX, 0.0f, badge);
            const ImVec2 ba(a.x + size * 0.7f + labelExtent.x + size * 0.6f, a.y + (height - be.y) * 0.5f - 3.0f);
            dl->AddRect(ba, ImVec2(ba.x + be.x + 10.0f, ba.y + be.y + 6.0f), kAmber, 2.0f);
            dl->AddText(h, bs, ImVec2(ba.x + 5.0f, ba.y + 3.0f), kAmber, badge);
        }

        // (TODO 286 d, the approved redesign) One control per kind of setting, in one column of one width at the right:
        // On / Off a switch, 2 to 4 short choices side by side, a numeric range of 5 or more a slider, anything else a
        // list that opens under its row with the current choice ticked. TF_MENU_CONTROLS=old: the "< value >" selector
        // for every row, as before. The controller is the same for each: Left / Right step, Cross holds or flips.
        const float selWidth = std::min(width * 0.46f, size * 17.0f);
        const float selX = b.x - selWidth - size * 0.6f;
        const float selH = height - size * 0.55f;
        const float selY = a.y + (height - selH) * 0.5f;
        const ImVec2 sa(selX, selY), sb(selX + selWidth, selY + selH);
        const ControlKind kind = controlKindOf(choices, selWidth, size);
        const ImU32 valueCol = enabled ? IM_COL32(255, 255, 255, 255) : kDim;
        // (TODO 344) A row that takes no choice: its chosen value is grey too, not the live blue
        const ImU32 kOff = IM_COL32(70, 80, 96, 255);
        ImGui::BeginDisabled(!enabled || count == 0);
        if (kind == ControlKind::Switch)
        {
            const int onIndex = onIndexOf(choices);
            const bool on = std::clamp(current, 0, count - 1) == onIndex;
            const float sw = selH * 1.9f, sh = selH * 0.78f;
            const ImVec2 ka(sb.x - sw, selY + (selH - sh) * 0.5f), kb(sb.x, ka.y + sh);
            ImGui::SetCursorScreenPos(ImVec2(selX, selY));
            if (ImGui::InvisibleButton("switch", ImVec2(selWidth, selH)))
                chosen = on ? 1 - onIndex : onIndex;
            const bool hot = ImGui::IsItemHovered();
            dl->AddRectFilled(ka, kb, on ? (enabled ? kBlue : kOff) : IM_COL32(30, 37, 49, 255), sh * 0.5f);
            if (!on)
                dl->AddRect(ka, kb, hot ? kBlue : kLine, sh * 0.5f);
            const float r = sh * 0.5f - 3.0f;
            const ImVec2 knob(on ? kb.x - sh * 0.5f : ka.x + sh * 0.5f, ka.y + sh * 0.5f);
            dl->AddCircleFilled(knob, r, enabled ? kText : kDim);
            const std::string &word = choices[static_cast<size_t>(std::clamp(current, 0, count - 1))];
            const float vs = size * 0.92f;
            const ImVec2 we = textFont->CalcTextSizeA(vs, FLT_MAX, 0.0f, word.c_str());
            dl->AddText(textFont, vs, ImVec2(ka.x - size * 0.5f - we.x, selY + (selH - we.y) * 0.5f), on ? valueCol : kDim, word.c_str());
        }
        else if (kind == ControlKind::Segments)
        {
            const float gap = 3.0f;
            const float segW = (selWidth - gap * static_cast<float>(count - 1)) / static_cast<float>(count);
            for (int i = 0; i < count; ++i)
            {
                const ImVec2 pa(selX + static_cast<float>(i) * (segW + gap), selY), pb(pa.x + segW, sb.y);
                ImGui::SetCursorScreenPos(pa);
                ImGui::PushID(i);
                if (ImGui::InvisibleButton("seg", ImVec2(segW, selH)))
                    chosen = i;
                const bool hot = ImGui::IsItemHovered();
                ImGui::PopID();
                const bool sel = i == current;
                dl->AddRectFilled(pa, pb, sel ? (enabled ? kBlue : kOff) : IM_COL32(30, 37, 49, 255), 2.0f);
                if (!sel && hot)
                    dl->AddRect(pa, pb, kBlue, 2.0f);
                const std::string &t = choices[static_cast<size_t>(i)];
                const float vs = size * 0.86f;
                const ImVec2 te = textFont->CalcTextSizeA(vs, FLT_MAX, 0.0f, t.c_str());
                dl->AddText(textFont, vs, ImVec2(pa.x + (segW - te.x) * 0.5f, selY + (selH - te.y) * 0.5f),
                            sel ? (enabled ? IM_COL32(8, 10, 15, 255) : IM_COL32(190, 196, 206, 255)) : (enabled ? kDim : IM_COL32(90, 98, 112, 255)), t.c_str());
            }
        }
        else if (kind == ControlKind::Slider)
        {
            const std::string &value = choices[static_cast<size_t>(std::clamp(current, 0, count - 1))];
            const float vs = size * 0.92f;
            const float valueW = size * 4.2f;
            const float tx0 = selX + selH * 0.4f, tx1 = sb.x - valueW;
            const float ty = selY + selH * 0.5f;
            ImGui::SetCursorScreenPos(ImVec2(selX, selY));
            ImGui::InvisibleButton("slider", ImVec2(selWidth - valueW + selH * 0.4f, selH));
            const bool hot = ImGui::IsItemHovered();
            if (ImGui::IsItemActive() && count > 1)
            {
                const float t = std::clamp((ImGui::GetIO().MousePos.x - tx0) / std::max(tx1 - tx0, 1.0f), 0.0f, 1.0f);
                const int at = static_cast<int>(std::lround(t * static_cast<float>(count - 1)));
                if (at != current)
                    chosen = at;
            }
            const float t = count > 1 ? static_cast<float>(std::clamp(current, 0, count - 1)) / static_cast<float>(count - 1) : 0.0f;
            const float kx = tx0 + (tx1 - tx0) * t;
            dl->AddRectFilled(ImVec2(tx0, ty - 3.0f), ImVec2(tx1, ty + 3.0f), IM_COL32(30, 37, 49, 255), 3.0f);
            dl->AddRectFilled(ImVec2(tx0, ty - 3.0f), ImVec2(kx, ty + 3.0f), enabled ? kBlue : kDim, 3.0f);
            dl->AddCircleFilled(ImVec2(kx, ty), selH * 0.3f, hot ? IM_COL32(255, 255, 255, 255) : kText);
            const ImVec2 ve = textFont->CalcTextSizeA(vs, FLT_MAX, 0.0f, value.c_str());
            dl->AddText(textFont, vs, ImVec2(sb.x - ve.x, selY + (selH - ve.y) * 0.5f), valueCol, value.c_str());
        }
        else
        {
            ImGui::SetCursorScreenPos(ImVec2(selX, selY));
            if (ImGui::InvisibleButton("value", ImVec2(selWidth, selH)) && count > 0)
                ImGui::OpenPopup("list");
            const bool hot = ImGui::IsItemHovered();
            dl->AddRectFilled(sa, sb, IM_COL32(30, 37, 49, 255), 2.0f);
            dl->AddRect(sa, sb, hot ? kBlue : kLine, 2.0f);
            const float caret = selH * 0.22f;
            const ImVec2 cc(sb.x - selH * 0.55f, selY + selH * 0.5f);
            dl->AddTriangleFilled(ImVec2(cc.x - caret, cc.y - caret * 0.5f), ImVec2(cc.x + caret, cc.y - caret * 0.5f),
                                  ImVec2(cc.x, cc.y + caret * 0.6f), enabled ? kDim : IM_COL32(90, 98, 112, 255));
            if (count > 0)
            {
                const std::string &value = choices[static_cast<size_t>(std::clamp(current, 0, count - 1))];
                const float vs = size * 0.92f;
                const float room = selWidth - selH * 1.3f;
                std::string shown = value;
                while (shown.size() > 3 && textFont->CalcTextSizeA(vs, FLT_MAX, 0.0f, shown.c_str()).x > room)
                    shown = shown.substr(0, shown.size() - 4) + "...";
                const ImVec2 ve = textFont->CalcTextSizeA(vs, FLT_MAX, 0.0f, shown.c_str());
                dl->AddText(textFont, vs, ImVec2(selX + size * 0.6f, selY + (selH - ve.y) * 0.5f), valueCol, shown.c_str());
            }
        }
        ImGui::EndDisabled();
        // The list, under its own row and as wide as the control (every kind opens it from the value with Enter on a held
        // row's double click; the list kind on a click).
        ImGui::SetNextWindowPos(ImVec2(selX, sb.y + 2.0f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(selWidth, 0.0f), ImVec2(selWidth, size * 22.0f));
        if (ImGui::BeginPopup("list"))
        {
            ImGui::PushFont(textFont, size);
            for (int i = 0; i < count; ++i)
            {
                const bool sel = i == current;
                if (ImGui::Selectable(choices[static_cast<size_t>(i)].c_str(), sel, 0, ImVec2(selWidth - size * 1.6f, 0.0f)))
                    chosen = i;
                if (sel)
                {
                    const ImVec2 ra = ImGui::GetItemRectMin(), rb = ImGui::GetItemRectMax();
                    const float cx = rb.x + size * 0.5f, cy = (ra.y + rb.y) * 0.5f;
                    ImGui::GetWindowDrawList()->AddPolyline(std::array<ImVec2, 3>{ImVec2(cx - 6.0f, cy), ImVec2(cx - 2.0f, cy + 4.0f), ImVec2(cx + 6.0f, cy - 5.0f)}.data(),
                                                          3, kBlue, 0, 2.5f);
                }
            }
            ImGui::PopFont();
            ImGui::EndPopup();
        }
        ImGui::PopID();
        ImGui::SetCursorScreenPos(ImVec2(a.x, b.y));
        ImGui::Dummy(ImVec2(width, 0.0f));
        return chosen;
    }

    bool toggleRow(const char *id, const std::string &label, bool &on, const std::string &help)
    {
        const int chosen = choiceRow(id, label, {"Off", "On"}, on ? 1 : 0, true, nullptr, help);
        if (chosen < 0)
            return false;
        on = chosen == 1;
        return true;
    }

    namespace
    {
        bool drawButton(const char *label, bool primary, float scale)
        {
        const float size = textSize() * scale;
        ImFont *font = fonts().heading;
        const float fs = size * 0.62f;
        const std::string text = upper(label);
        const ImVec2 extent = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, text.c_str());
        const ImVec2 padding(size * 0.9f, size * 0.5f);
        const ImVec2 dim(extent.x + padding.x * 2.0f, extent.y + padding.y * 2.0f);
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const ImVec2 b(a.x + dim.x, a.y + dim.y);
        bool clicked = ImGui::InvisibleButton(label, dim);
        const int navId = navItem(); // (TODO 212: Cross presses the focused button)
        const bool focused = navFocused(navId);
        if (focused && navTakeConfirm())
            clicked = true;
        const bool hovered = ImGui::IsItemHovered() || focused;
        const bool disabled = false;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        if (focused)
            navMark(dl, a, b, false);
        const float cut = size * 0.35f;
        if (primary)
            chamfered(dl, a, b, cut, disabled ? kRedDark : (hovered ? IM_COL32(236, 52, 66, 255) : kRed));
        else
        {
            chamfered(dl, a, b, cut, hovered ? kPanelLift : IM_COL32(22, 27, 36, 255));
            dl->AddLine(ImVec2(a.x, b.y - 1.0f), ImVec2(b.x - cut, b.y - 1.0f), hovered ? kBlue : kLine, 2.0f);
        }
        dl->AddText(font, fs, ImVec2(a.x + padding.x, a.y + padding.y), disabled ? kDim : IM_COL32(255, 255, 255, 255), text.c_str());
        return clicked && !disabled;
        }
    }

    bool button(const char *label, bool primary) { return drawButton(label, primary, 1.0f); }

    bool smallButton(const char *label) { return drawButton(label, false, 0.85f); }

    void chip(const char *text, const char *colourName)
    {
        const float size = textSize();
        ImFont *font = fonts().heading;
        const float fs = size * 0.6f;
        const std::string t = upper(text);
        const ImVec2 e = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, t.c_str());
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const ImVec2 dim(e.x + size * 0.8f, e.y + size * 0.35f);
        ImGui::Dummy(dim);
        const ImU32 c = colour(colourName);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, ImVec2(a.x + dim.x, a.y + dim.y), (c & 0x00FFFFFFu) | 0x30000000u, 2.0f);
        dl->AddRect(a, ImVec2(a.x + dim.x, a.y + dim.y), c, 2.0f);
        dl->AddText(font, fs, ImVec2(a.x + size * 0.4f, a.y + size * 0.175f), c, t.c_str());
    }

    void navBegin(bool inPage, const NavInput &input)
    {
        g_nav.inPage = inPage;
        g_nav.in = inPage ? input : NavInput{};
        g_nav.index = 0;
        g_nav.backTaken = false;
        if (!inPage)
        {
            g_nav.hold = false;
            g_nav.focusNext = false;
            return;
        }
        if (!g_nav.hold && g_nav.count > 0)
        {
            if (g_nav.in.up && g_nav.focus > 0)
            {
                --g_nav.focus;
                g_nav.scroll = true;
                g_nav.returnFocus = -1;
            }
            if (g_nav.in.down && g_nav.focus < g_nav.count - 1)
            {
                ++g_nav.focus;
                g_nav.scroll = true;
                g_nav.returnFocus = -1;
            }
        }
        // Circle at a widget that took the focus by itself (the Restart button): back to where it was.
        if (!g_nav.hold && g_nav.in.back && g_nav.returnFocus >= 0)
        {
            g_nav.focus = g_nav.returnFocus;
            g_nav.returnFocus = -1;
            g_nav.scroll = true;
            g_nav.in.back = false;
            g_nav.backTaken = true;
        }
    }

    bool navEnd()
    {
        if (!g_nav.inPage)
            return false;
        g_nav.count = g_nav.index;
        if (g_nav.focus >= g_nav.count)
            g_nav.focus = std::max(0, g_nav.count - 1);
        if (g_nav.hold && g_nav.holdId >= g_nav.count)
            g_nav.hold = false;
        return g_nav.in.back && !g_nav.backTaken;
    }

    void navReset()
    {
        g_nav.focus = 0;
        g_nav.count = 0;
        g_nav.hold = false;
        g_nav.returnFocus = -1;
        g_nav.focusNext = false;
        g_nav.scroll = false;
        ImGui::SetScrollY(0.0f); // (the page's window: called while it is the current one)
    }

    void navFocusNext() { g_nav.focusNext = true; }

    bool navHolding() { return g_nav.hold; }

    void setHelp(const std::string &help) { g_help = help; }
    const std::string &help() { return g_help; }
    void clearHelp() { g_help.clear(); }
}
