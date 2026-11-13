// Crisp text and controller button symbols (TODO 119): Display settings (crisp_text.h).
#include "crisp_text.h"

#include "nr_prompt_style.h"
#include "runtime/ps2_settings.h"

namespace crisp_text
{
    void addSettings()
    {
        const auto native = []() { return ps2_settings::choice("TF_RENDERER") == "native"; };
        {
            ps2_settings::Setting s;
            s.key = "TF_CRISP_TEXT";
            s.section = "Display";
            s.label = "Sharp text";
            s.order = 64;
            s.help = "The menus', button hints' and HUD's letters drawn sharp at your screen's size, in the same places and "
                     "sizes as the game's: the menu font from a free font of the same typeface, the others with sharp edges. "
                     "Off: the upscaled letters, as before. Made from your disc with tools\\make_hd_textures.ps1 (-FontsOnly needs no upscale). "
                     "Taken at the next start.";
            s.choices = {{"1", "On"}, {"0", "Off"}};
            s.defaultValue = "1";
            s.available = native;
            s.unavailableHelp = "Only for the native renderer (Display: Renderer).";
            ps2_settings::add(s);
        }
        {
            ps2_settings::Setting s;
            s.key = "TF_BUTTON_SYMBOLS";
            s.section = "Display";
            s.label = "Button symbols";
            s.order = 65;
            s.help = "The buttons in the game's hints and prompts. PS5: drawn in the DualSense's style (light symbols on dark "
                     "buttons, L1 / R1 / L2 / R2 keys, Create for SELECT). PS2: the game's own coloured symbols. Made from your "
                     "disc with tools\\make_hd_textures.ps1 (-FontsOnly works without HD textures). Taken at the next start.";
            s.choices = {{"ps5", "PS5"}, {"ps2", "PS2"}};
            s.defaultValue = "ps5";
            s.available = native;
            s.unavailableHelp = "Only for the native renderer (Display: Renderer).";
            ps2_settings::add(s);
        }
        {
            ps2_settings::Setting s;
            s.key = "TF_PAD_PROMPTS";
            s.section = "Display";
            s.label = "Controller prompt style";
            s.order = 66;
            s.help = "Auto follows the last controller used while the prompts follow the device (TF_KEY_PROMPTS on): a Sony "
                     "pad shows the Button symbols above, other pads Xbox labels. PlayStation and Xbox force a controller "
                     "style. The Xbox labels look like the pad plugged in: an Xbox 360 pad's or an Xbox One / Series pad's "
                     "buttons. Keyboard prompts still follow the keyboard. Changes live. (In the overlay: Display, Button prompts.)";
            s.choices = {{"auto", "Auto"}, {"ps", "PlayStation"}, {"xbox", "Xbox"}};
            s.defaultValue = "auto";
            s.apply = [](const std::string &value)
            {
                nr::g_padPrompts.store(nr::padPromptsFrom(value), std::memory_order_relaxed);
                return true;
            };
            s.available = native;
            s.unavailableHelp = "Only for the native renderer (Display: Renderer).";
            ps2_settings::add(s);
            nr::g_padPrompts.store(nr::padPromptsFrom(ps2_settings::value("TF_PAD_PROMPTS")), std::memory_order_relaxed);
        }
    }
}
