#include "ui-imgui/debugger_workspace.h"
/**
 * @file gdg_window.cpp
 * @brief Debugger: GDG state inspector window - arch-independent shell
 *        (gdg-panel F1 scaffold + F6 spolecna Raster sekce).
 *
 * Zde sídlí společný entry point `imgui_gdg_state_window()`: vytvoří
 * ImGui okno (společný titulek + stable ID "###GDGState"), připraví prostor
 * a deleguje renderování těla na per-arch implementaci. Po per-arch těle
 * vykreslí společnou Raster sekci (F6) - scanline, pixel clock, blanking
 * flagy, sync, tempo, CTC0 divider - která je pro všechny 3 archy shodná.
 *
 * Per-arch dispatch:
 *  - MZARCH == 800  -> `gdg_window_render_mz800(void)` z `gdg_window_mz800.cpp`
 *  - MZARCH == 700  -> `gdg_window_render_mz700(void)` z `gdg_window_mz700.cpp`
 *  - MZARCH == 1500 -> `gdg_window_render_mz1500(void)` z `gdg_window_mz1500.cpp`
 *
 * Side-effect free invariant (stejný jako u CTC/PPI/PIO/PSG panelů):
 *  - Okno vždy POUZE čte stav GDG přes mirror gettery.
 *  - Žádné `gdg_write_*()`, žádný stepping, žádný state mutation.
 *
 * License: GPLv3.
 */

#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "libs/imgui/imgui.h"

#include "i18n.h"

#include "mzarch/mzarch_config.h"

#include "ui-imgui/debugger/gdg_window.h"

#include <stdio.h>

extern "C"
{
#include "emulator/mzarch/gdg_raster_snapshot.h"
}

extern "C"
{
    void imgui_gdg_state_window(bool *p_open);
}

/* ---- Per-arch render funkce - forward deklarace ----------------------- */

#if MZARCH == 800
extern void gdg_window_render_mz800(void);
#elif MZARCH == 700
extern void gdg_window_render_mz700(void);
#elif MZARCH == 1500
extern void gdg_window_render_mz1500(void);
#else
#error "gdg_window.cpp: unsupported MZARCH (expected 700, 800 or 1500)"
#endif

/**
 * @brief Vykreslí společnou Raster sekci nad arch-independent snapshot
 *        strukturou.
 *
 * Sekce je `CollapsingHeader` s defaultně otevřeným stavem a stable ID
 * `###GDGRaster`. Obsah: scanline, pixel clock (= ticks v aktuálním screenu),
 * VBLN counter (= snímky od resetu), HBLN/VBLN/HSYNC/VSYNC flagy, tempo
 * signál, tempo divider counter a CTC0 divider konstanta.
 *
 * Side-effect free: jen jedno volání `gdg_mirror_raster_snapshot()`.
 *
 * @param snap  Snapshot vyplněný před voláním (read-only kopie).
 */
static void render_raster_section(const st_GDG_RASTER_SNAPSHOT *snap)
{
    if (snap == NULL) {
        return;
    }

    ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    if (!ImGui::CollapsingHeader(_L("Raster###GDGRaster"))) {
        return;
    }

    ImGui::Indent();

    /* Zarovnání label / value do dvou sloupců přes ImGui Table API.
     * Proporcionální font + printf padding mezerami nefunguje (mezera má
     * jinou pixelovou šířku než digit). SizingFixedFit nechá label sloupec
     * růst přesně podle nejdelšího textu (= přežije i překlad do jazyka
     * s delšími labely). */
    if (ImGui::BeginTable("##GDGRasterTable", 2,
                          ImGuiTableFlags_SizingFixedFit
                              | ImGuiTableFlags_NoSavedSettings))
    {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);

        /* Scanline + pixel clock - aktuální pozice paprsku v rámci snímku.
         * screen_ticks se po každém screen done event odečte
         * (VIDEO_SCREEN_TICKS), hodnota je relativní k aktuálnímu screenu. */
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(_("Scanline:"));
        ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)snap->beam_row);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(_("Pixel clock:"));
        ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)snap->screen_ticks);

        /* VBLN counter = monotonic přes celý běh emulace (od reset/load
         * snapshotu). Slouží jako frame counter pro UI. */
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(_("VBLN count:"));
        ImGui::TableNextColumn();
        ImGui::Text("%llu", (unsigned long long)snap->screens_done);

        ImGui::EndTable();
    }

    ImGui::Separator();

    /* Blanking flagy + sync bity viditelné v Status registru. */
    if (ImGui::BeginTable("##GDGRasterFlagsTable", 2,
                          ImGuiTableFlags_SizingFixedFit
                              | ImGuiTableFlags_NoSavedSettings))
    {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(_("HBLN flag:"));
        ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)snap->hbln);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(_("VBLN flag:"));
        ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)snap->vbln);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(_("HSYNC:"));
        ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)snap->sts_hsync);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(_("VSYNC:"));
        ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)snap->sts_vsync);

        ImGui::EndTable();
    }

    ImGui::Separator();

    /* Tempo signál + jeho generátor.
     *  - tempo (0/1) = aktuální stav tempo bitu (live výstup pro Status).
     *  - tempo_divider = živý čítač 0..228 ve smyčce (po 229 reset).
     *  - ctc0_divider = compile-time GDGCLK_CTC0_DIVIDER, kolik master
     *    GDGCLK ticků = 1 CTC0 tick (MZ-800=16, MZ-1500=16,
     *    MZ-700 NTSC=13, MZ-700 PAL=16). */
    if (ImGui::BeginTable("##GDGRasterTempoTable", 2,
                          ImGuiTableFlags_SizingFixedFit
                              | ImGuiTableFlags_NoSavedSettings))
    {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(_("Tempo:"));
        ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)snap->tempo);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(_("Tempo divider:"));
        ImGui::TableNextColumn();
        ImGui::Text("%u", (unsigned)snap->tempo_divider);

        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(_("CTC0 divider:"));
        ImGui::TableNextColumn();
        ImGui::Text("%u", (unsigned)snap->ctc0_divider);

        ImGui::EndTable();
    }

    ImGui::Unindent();
}

/* ---- Entry point ------------------------------------------------------ */

void imgui_gdg_state_window(bool *p_open)
{
    ImGui::SetNextWindowSize(ImVec2(460, 620), ImGuiCond_FirstUseEver);

    /* Stable ID "###GDGState" je anglický a unique - umožňuje runtime změnu
     * titulku (např. překlad "GDG State") bez resetování ImGui pozice/velikosti. */
    if (!ImGui::Begin(debugger_workspace_panel(_L("GDG State###GDGState")), p_open,
                      ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    }

    /* Per-arch dispatch - jen jedna z funkcí existuje v daném buildu. */
#if MZARCH == 800
    gdg_window_render_mz800();
#elif MZARCH == 700
    gdg_window_render_mz700();
#elif MZARCH == 1500
    gdg_window_render_mz1500();
#endif

    /* Spolecna Raster sekce (F6) - vyzaduje arch-independent snapshot. */
    st_GDG_RASTER_SNAPSHOT raster_snap;
    gdg_mirror_raster_snapshot(&raster_snap);
    render_raster_section(&raster_snap);

    ImGui::End();
}
