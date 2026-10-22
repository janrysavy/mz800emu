/**
 * @file gdg_window_mz800.cpp
 * @brief Debugger: GDG state inspector - MZ-800 specifický render
 *        (gdg-panel F3 obsah).
 *
 * Vykresluje sekce specifické pro MZ-800 GDG:
 *   - Video mode (dekód z DMD bitů [2:0] + MZ700 compat bit [3]).
 *   - RF/WF stav VRAM controlleru (`g_vramctrl`): plane select masky,
 *     write mode (SINGLE/EXOR/OR/RESET/REPLACE/PSET), search mask, VBANK,
 *     MZ-700 one-shot WAIT latch.
 *   - HW scroll stav (`g_hwscroll`): SSA, SEA, SW, SOF, enabled flag.
 *   - CG-RAM (700 compat mode): pouze informativní poznámka, samotná data
 *     v Memory Map okně (= 4 KB v `g_memoryVRAM[0..0xFFF]`, panel ji
 *     neduplikuje).
 *   - Palette: 16 IGRB swatchů 4x4 grid (regPALGRP + regPAL0..3 dekódované
 *     do 16 HW barev). RGB tabulka přes `g_display_predef_colors[16]`
 *     aktuálního color schema (Normal/Grayscale/Green). Klik na swatch ->
 *     popup s informací o indexu a RGB.
 *   - Border: regBOR jako IGRB index + ColorButton preview. BCOL live
 *     (mid-frame border change) zatím TODO (není field v `st_GDG`).
 *
 * Data source: výhradně přes `gdg_mirror_snapshot()` (side-effect free,
 * F2 + F3 mirror API). Žádné `gdg_write_*()`, žádný state mutation.
 *
 * Video mode dekód: stejný switch jako v `mz800_framebuffer.c` řádky
 * 276-606 (case 0x00..0x07 na `g_gdg.regDMD & 0x07`). Zde duplikujeme
 * tabulku jako lokální LUT - veřejná dekódovací funkce v emulator core
 * neexistuje (renderer ji má jen jako interní switch).
 *
 * License: GPLv3.
 */

#include "mzarch/mzarch_config.h"

#if MZARCH == 800

#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "libs/imgui/imgui.h"

#include "i18n.h"

#include <stdio.h>
#include <stdint.h>

extern "C"
{
#include "emulator/mzarch/mz800/gdg/mz800_gdg_mirror.h"
#include "emulator/display.h"
}

/* ========================================================================
 * Konstanty a label tabulky
 * ====================================================================== */

/**
 * @brief Lidsky čitelná jména 8 grafických módů MZ-800.
 *
 * Index = `regDMD & 0x07` (= DMD bity SCRW640 + HICOLOR + VBANK). Zdroj
 * pojmenování: `mz800_framebuffer.c` switch řádky 276-606, kde je každý
 * case okomentován reálnou rozlišovací popiskou. N_() značí pro gettext
 * extraction; runtime překlad přes _() v render funkci.
 */
static const char *const c_dmd_video_mode_names[8] = {
    N_("320x200 4-color VBANK A"), /* 0x00 */
    N_("320x200 4-color VBANK B"), /* 0x01 */
    N_("320x200 16-color"),        /* 0x02 */
    N_("320x200 16-color undoc"),  /* 0x03 */
    N_("640x200 2-color VBANK A"), /* 0x04 */
    N_("640x200 2-color VBANK B"), /* 0x05 */
    N_("640x200 4-color"),         /* 0x06 */
    N_("640x200 4-color undoc"),   /* 0x07 */
};

/**
 * @brief Jména WF_MODE pro WFR_MODE enum z `mz800_vramctrl.h`.
 *
 * Enum hodnoty: SINGLE=0, EXOR=1, OR=2, RESET=3, REPLACE=4, (5 vynechán),
 * PSET=6. Index 5 = neznámý/undefined.
 */
static const char *const c_wf_mode_names[7] = {
    N_("SINGLE"),  /* 0 */
    N_("EXOR"),    /* 1 */
    N_("OR"),      /* 2 */
    N_("RESET"),   /* 3 */
    N_("REPLACE"), /* 4 */
    N_("???"),     /* 5 - undefined */
    N_("PSET"),    /* 6 */
};

/**
 * @brief Velikost jednoho palette swatch ve gridu (px).
 *
 * Empiricky volené - dostatečně velké pro klik + popup, nepřebíjí
 * okolní text. Grid 4x4 = 4 sloupce * (size + spacing) zhruba 200 px.
 */
static const ImVec2 c_palette_swatch_size = ImVec2(40.0f, 40.0f);

/* ========================================================================
 * Pomocné funkce
 * ====================================================================== */

/**
 * @brief Vrátí ImVec4 (RGBA float) ze surového 24-bit RGB packed uint32.
 *
 * Vstup je formát `0x00RRGGBB` (= jako v `g_display_predef_colors[]`).
 * Alfa je vždy 1.0 (immutable swatches).
 *
 * @param rgb24  0xRRGGBB hodnota.
 * @return ImVec4 v rozsahu 0..1.
 */
static ImVec4 rgb24_to_imvec4(uint32_t rgb24)
{
    float r = (float)((rgb24 >> 16) & 0xFFu) / 255.0f;
    float g = (float)((rgb24 >> 8)  & 0xFFu) / 255.0f;
    float b = (float)((rgb24)       & 0xFFu) / 255.0f;
    return ImVec4(r, g, b, 1.0f);
}

/**
 * @brief Vrátí aktuální IGRB → RGB tabulku podle nastaveného color schema.
 *
 * Color schema (Normal/Grayscale/Green) je live nastavení uživatele -
 * panel ukazuje barvy jak je vidí na displeji, ne pevné "kanonické" RGB.
 *
 * @return Pointer na 16-prvkové pole `uint32_t` (0xRRGGBB).
 */
static const uint32_t *current_color_table(void)
{
    switch (g_display.color_schema)
    {
        case DISPLAY_GRAYSCALE:
            return g_display_predef_grays;
        case DISPLAY_GREEN:
            return g_display_predef_greens;
        case DISPLAY_NORMAL:
        default:
            return g_display_predef_colors;
    }
}

/**
 * @brief Vykreslí jeden 4-bit plane mask field jako řetězec "I II III IV".
 *
 * Bit 0 = plane I, bit 1 = II, bit 2 = III, bit 3 = IV. Pro nastavené
 * plane vypíše název, pro nenastavené tečku.
 *
 * @param mask   4-bit maska (bits 0..3).
 * @param out    Výstupní buffer (min 16 bajtů).
 * @param sz     Velikost bufferu.
 */
static void format_plane_mask(uint8_t mask, char *out, size_t sz)
{
    snprintf(out, sz, "%s %s %s %s",
             (mask & 0x01u) ? "I"   : ".",
             (mask & 0x02u) ? "II"  : ".",
             (mask & 0x04u) ? "III" : ".",
             (mask & 0x08u) ? "IV"  : ".");
}

/* ========================================================================
 * Render sekce
 * ====================================================================== */

/**
 * @brief Vykreslí "Video mode" sekci - dekód DMD bitů.
 *
 * Ukazuje surovou hodnotu DMD (4 low bity), dekódované video mode jméno
 * (z low 3 bitů), a MZ-700 compat flag (bit 3). Banking bity (HICOLOR,
 * SCRW640, VBANK) ukazuje jen jako součást mode jména, neduplikuje
 * jednotlivé řádky (= patří Memory Map oknu).
 *
 * @param snap  Snapshot mirror struktura.
 */
static void render_video_mode_section(const st_GDG_MIRROR_MZ800 *snap)
{
    unsigned dmd_low = snap->regDMD & 0x07u;
    const char *mode_name = _(c_dmd_video_mode_names[dmd_low]);

    if (snap->dmd_mz700)
    {
        /* MZ-700 compat: bity [2:0] DMD nejsou jako 800 grafické módy
         * interpretovány - aktivní je textový mód 40x25 z MZ-700. */
        ImGui::Text("%s: %s", _("Mode"), _("MZ-700 compat (text 40x25)"));
    }
    else
    {
        ImGui::Text("%s: %s", _("Mode"), mode_name);
    }

    ImGui::Text("%s: 0x%02X  (MZ700=%u  SCRW640=%u  HICOLOR=%u  VBANK=%u)",
                _("DMD"),
                snap->regDMD,
                (unsigned)snap->dmd_mz700,
                (unsigned)snap->dmd_scrw640,
                (unsigned)snap->dmd_hicolor,
                (unsigned)snap->dmd_vbank);
}

/**
 * @brief Vykreslí "RF / WF" sekci - VRAM controller registry.
 *
 * @param snap  Snapshot mirror struktura.
 */
static void render_vramctrl_section(const st_GDG_MIRROR_MZ800 *snap)
{
    char wf_buf[16];
    char rf_buf[16];
    char search_buf[16];
    format_plane_mask(snap->vc_wf_plane,  wf_buf,     sizeof(wf_buf));
    format_plane_mask(snap->vc_rf_plane,  rf_buf,     sizeof(rf_buf));
    format_plane_mask(snap->vc_rf_search, search_buf, sizeof(search_buf));

    unsigned mode_idx = snap->vc_wf_mode;
    const char *mode_name = (mode_idx < 7u)
                                ? _(c_wf_mode_names[mode_idx])
                                : "?";

    /* Table layout: 2 sloupce (label + value) přes BeginTable, stejný vzor
     * jako Raster sekce v gdg_window.cpp. SizingFixedFit nechá label
     * sloupec růst přesně podle nejdelšího překladu (cs/sk labely jsou
     * delší než anglické - bez Table by hodnoty nesvírali ve sloupci). */
    if (ImGui::BeginTable("##GDGMZ800VRAMCtrlTable", 2,
                          ImGuiTableFlags_SizingFixedFit
                              | ImGuiTableFlags_NoSavedSettings))
    {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);

        /* POZN: msgid jsou ponechány bez dvojtečky (jak v F3); dvojtečka
         * vlevo se přidává až přes format string, aby Task 2 (table polish)
         * nevytvořil nové i18n msgid. */
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%s:", _("WF plane"));
        ImGui::TableNextColumn(); ImGui::TextUnformatted(wf_buf);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%s:", _("WF mode"));
        ImGui::TableNextColumn(); ImGui::Text("%s (%u)", mode_name, mode_idx);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%s:", _("RF plane"));
        ImGui::TableNextColumn(); ImGui::TextUnformatted(rf_buf);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%s:", _("RF search"));
        ImGui::TableNextColumn(); ImGui::TextUnformatted(search_buf);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%s:", _("VBANK (WF/RF)"));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(snap->vc_wfrf_vbank ? _("B") : _("A"));

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%s:", _("WRITE FF"));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(snap->vc_vram_wrff_set
                                   ? _("set") : _("clear"));

        ImGui::EndTable();
    }
}

/**
 * @brief Vykreslí "HW scroll" sekci.
 *
 * @param snap  Snapshot mirror struktura.
 */
static void render_hwscroll_section(const st_GDG_MIRROR_MZ800 *snap)
{
    /* Table layout: 2 sloupce (label + value), stejný vzor jako Raster
     * sekce v gdg_window.cpp. Labely SSA/SEA/SW/SOF nejsou v msgid
     * (jsou to register zkratky bez překladu) - jen "Enabled" je _(). */
    if (ImGui::BeginTable("##GDGMZ800HWScrollTable", 2,
                          ImGuiTableFlags_SizingFixedFit
                              | ImGuiTableFlags_NoSavedSettings))
    {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%s:", _("Enabled"));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(snap->hs_enabled ? _("yes") : _("no"));

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("SSA:");
        ImGui::TableNextColumn(); ImGui::Text("0x%04X", snap->hs_ssa);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("SEA:");
        ImGui::TableNextColumn(); ImGui::Text("0x%04X", snap->hs_sea);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("SW:");
        ImGui::TableNextColumn(); ImGui::Text("0x%04X", snap->hs_sw);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted("SOF:");
        ImGui::TableNextColumn(); ImGui::Text("0x%04X", snap->hs_sof);

        ImGui::EndTable();
    }
}

/**
 * @brief Vykreslí "CG-RAM (700 mode)" informativní sekci.
 *
 * V MZ-800 nemá samostatný field; data sídlí v `g_memoryVRAM[0..0xFFF]`
 * (= prvních 4 KB plane I). Aktuální mapování na $C000-$CFFF se rozhoduje
 * v Memory Map okně (MEMORY_MZ800_MAP_FLAG_CGRAM_VRAM). Tato sekce panelu
 * jen informativně referuje, nezobrazuje VRAM data.
 *
 * @param snap  Snapshot mirror struktura (zde použit jen pro dmd_mz700).
 */
static void render_cgram_section(const st_GDG_MIRROR_MZ800 *snap)
{
    if (snap->dmd_mz700)
    {
        ImGui::TextUnformatted(_("MZ-700 compat mode active."));
        ImGui::TextUnformatted(_("CG-RAM = first 4 KB of VRAM plane I."));
        ImGui::TextUnformatted(_("See Memory Map window for active mapping."));
    }
    else
    {
        ImGui::TextUnformatted(_("Not active - MZ-800 graphics mode."));
        ImGui::TextUnformatted(_("CG-RAM is only meaningful in MZ-700 compat mode."));
    }
}

/**
 * @brief Vykreslí "Palette" sekci - 16 IGRB swatchů v 4x4 gridu.
 *
 * Aktivní paleta (regPALGRP + regPAL0..3) je live mapování 4-bit kódu
 * v graphics mode na 16-color HW barvu. Panel zobrazuje VŠECH 16 IGRB
 * indexů (0..15), neumi se ohraničit jen na "aktuálně používané".
 * regPALGRP a regPAL0..3 jsou navíc zobrazeny pod gridem jako text.
 *
 * Klik na swatch otevře popup s indexem a RGB.
 *
 * @param snap  Snapshot mirror struktura.
 */
static void render_palette_section(const st_GDG_MIRROR_MZ800 *snap)
{
    const uint32_t *table = current_color_table();

    /* Static state pro popup - drží index naposledy klinutého swatche.
     * Popup se otevírá přes OpenPopupOnItemClick + BeginPopup. */
    static int s_popup_index = -1;

    /* Push style: tight spacing pro grid swatchů. */
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));

    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            int idx = row * 4 + col;
            ImVec4 c = rgb24_to_imvec4(table[idx]);

            char btn_id[32];
            snprintf(btn_id, sizeof(btn_id), "##pal_%d", idx);
            if (ImGui::ColorButton(btn_id, c,
                                   ImGuiColorEditFlags_NoTooltip
                                       | ImGuiColorEditFlags_NoAlpha,
                                   c_palette_swatch_size))
            {
                s_popup_index = idx;
                ImGui::OpenPopup("##palette_popup");
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("IGRB %u (0x%X)", (unsigned)idx, (unsigned)idx);
            }
            if (col < 3)
                ImGui::SameLine();
        }
    }

    ImGui::PopStyleVar();

    /* Popup s detailem zvolené barvy. */
    if (ImGui::BeginPopup("##palette_popup"))
    {
        if (s_popup_index >= 0 && s_popup_index < 16)
        {
            uint32_t rgb = table[s_popup_index];
            ImGui::Text("%s: %d (0x%X)",
                        _("Index"), s_popup_index, (unsigned)s_popup_index);
            ImGui::Text("RGB: 0x%06X", rgb & 0x00FFFFFFu);
            ImGui::Text("R=%u  G=%u  B=%u",
                        (unsigned)((rgb >> 16) & 0xFFu),
                        (unsigned)((rgb >> 8)  & 0xFFu),
                        (unsigned)((rgb)       & 0xFFu));
        }
        ImGui::EndPopup();
    }

    /* Aktivní palette registry pod gridem - tj. co se reálně použije
     * v rendereru jako mapování 4-bit code -> IGRB. */
    ImGui::Spacing();
    ImGui::Text("PALGRP = %u   PAL0..3 = %u, %u, %u, %u",
                (unsigned)snap->regPALGRP,
                (unsigned)snap->regPAL0,
                (unsigned)snap->regPAL1,
                (unsigned)snap->regPAL2,
                (unsigned)snap->regPAL3);
}

/**
 * @brief Vykreslí "Border" sekci - regBOR jako index + ColorButton preview.
 *
 * BCOL (mid-frame border change) zatím TODO - není field v `st_GDG`,
 * border se rendrují jednoznačně podle aktuálního regBOR v okamžiku
 * `framebuffer_border_all_rows_fill()`. Reálný BCOL live by potřeboval
 * sledování změn regBOR per scanline.
 *
 * @param snap  Snapshot mirror struktura.
 */
static void render_border_section(const st_GDG_MIRROR_MZ800 *snap)
{
    const uint32_t *table = current_color_table();
    uint8_t bor = snap->regBOR & 0x0Fu;
    ImVec4 c = rgb24_to_imvec4(table[bor]);

    ImGui::Text("BOR = %u (0x%X)", (unsigned)bor, (unsigned)bor);
    ImGui::SameLine();
    ImGui::ColorButton("##border_preview", c,
                       ImGuiColorEditFlags_NoTooltip
                           | ImGuiColorEditFlags_NoAlpha,
                       ImVec2(40.0f, 20.0f));
    /* TODO: BCOL live (mid-frame border) - není field v st_GDG. */
}

/* ========================================================================
 * Entry point
 * ====================================================================== */

/**
 * @brief Vykreslí tělo GDG State okna pro MZ-800.
 *
 * Volá se z `gdg_window.cpp` shell vrstvy mezi `ImGui::Begin()` a
 * `ImGui::End()`. Side-effect free: jen čte přes `gdg_mirror_snapshot()`.
 *
 * Sekce (CollapsingHeader, defaultně otevřené):
 *   - Video mode (DMD dekód)
 *   - RF / WF (VRAM controller)
 *   - HW scroll
 *   - CG-RAM (700 mode informativně)
 *   - Palette (4x4 swatch grid + popup)
 *   - Border (regBOR + preview)
 */
void gdg_window_render_mz800(void)
{
    st_GDG_MIRROR_MZ800 snap;
    gdg_mirror_snapshot(&snap);

    if (ImGui::CollapsingHeader(_L("Video mode###GDGMZ800VideoMode"),
                                ImGuiTreeNodeFlags_DefaultOpen))
    {
        render_video_mode_section(&snap);
    }

    if (ImGui::CollapsingHeader(_L("RF / WF###GDGMZ800RFWF"),
                                ImGuiTreeNodeFlags_DefaultOpen))
    {
        render_vramctrl_section(&snap);
    }

    if (ImGui::CollapsingHeader(_L("HW scroll###GDGMZ800HWScroll"),
                                ImGuiTreeNodeFlags_DefaultOpen))
    {
        render_hwscroll_section(&snap);
    }

    if (ImGui::CollapsingHeader(_L("CG-RAM (MZ-700 mode)###GDGMZ800CGRAM")))
    {
        render_cgram_section(&snap);
    }

    if (ImGui::CollapsingHeader(_L("Palette###GDGMZ800Palette"),
                                ImGuiTreeNodeFlags_DefaultOpen))
    {
        render_palette_section(&snap);
    }

    if (ImGui::CollapsingHeader(_L("Border###GDGMZ800Border"),
                                ImGuiTreeNodeFlags_DefaultOpen))
    {
        render_border_section(&snap);
    }
}

#endif /* MZARCH == 800 */
