/**
 * @file snapshot_setup_dialog.cpp
 * @brief Konfigurační okno pro snapshot systém
 */

#include "main.h"
#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "libs/imgui/imgui.h"
#include "libs/igfd/ImGuiFileDialog.h"
#include "ui-imgui/filechooser/imgui_filechooser.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/auto_layout.h"

// Lokalizace
#include "i18n.h"

extern "C"
{
#include "snapshot/snapshot.h"
}

/* Lokální kopie nastavení pro editaci (aplikuje se po OK) */
static st_SNAPSHOT_SETTINGS s_settings;
static bool s_initialized = false;

/* Flag pro directory chooser */
static bool s_dir_chooser_open = false;

static const char *s_quicksave_modes[] = {
    "Basic",
    "Incremental",
    "Rotational"
};

static const char *s_resume_modes[] = {
    "Always Run",
    "Always Pause",
    "Previous State"
};

extern "C" void imgui_snapshot_setup_dialog(void)
{
    if (!g_gui->showSnapshotSetupWindow)
        return;

    /* Při prvním otevření zkopírovat aktuální nastavení */
    if (!s_initialized) {
        s_settings = g_snapshot_settings;
        s_initialized = true;
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoCollapse;

    /* Auto-layout při fresh open - cache _L() do lokální proměnné. */
    const char *snap_title = _L("Snapshot Settings");
    auto_layout_first_use_portrait(snap_title, 500.0f, 400.0f);
    if (ImGui::Begin(snap_title, &g_gui->showSnapshotSetupWindow, flags)) {

        /* Výchozí adresář */
        ImGui::Text("%s:", _("Default Directory"));
        float btn_width = ImGui::GetFrameHeight(); /* šířka = výška řádku (čtvercové tlačítko) */
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - btn_width - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputText("##dir", s_settings.default_directory,
                         sizeof(s_settings.default_directory));
        ImGui::SameLine();
        if (ImGui::Button("...", ImVec2(btn_width, 0))) {
            /* Otevřít directory chooser */
            IGFD::FileDialogConfig config;
            config.path = s_settings.default_directory[0] ? s_settings.default_directory : ".";
            config.countSelectionMax = 1;
            config.flags = ImGuiFileDialogFlags_Modal |
                           ImGuiFileDialogFlags_DontShowHiddenFiles |
                           ImGuiFileDialogFlags_ShowDevicesButton;
            imgui_filechooser_prepare_config(BASEUI_FCHOOSER_CAT_SNAPSHOT, config);
            ImGuiFileDialog::Instance()->OpenDialog(
                "SnapshotDirChooser", _("Select Directory"), nullptr, config);
            s_dir_chooser_open = true;
        }
        ImGui::Spacing();

        /* Oddělovač — výchozí volby uložení */
        ImGui::SeparatorText(_("Default Save Options"));

        ImGui::Checkbox(_L("Include RAM disk content"), &s_settings.include_ramdisk);
        ImGui::Checkbox(_L("Include MEMEXT content"), &s_settings.include_memext);

        ImGui::Spacing();
        ImGui::Text("%s:", _("Compression level"));
        ImGui::SliderInt("##compress", &s_settings.compression_level, 0, 9);
        ImGui::Spacing();

        /* Oddělovač — Quick Save */
        ImGui::SeparatorText(_("Quick Save"));

        ImGui::Text("%s:", _("Mode"));
        if (ImGui::BeginCombo("##mode", _(s_quicksave_modes[s_settings.quicksave_mode]))) {
            for (int i = 0; i < 3; i++) {
                bool selected = (s_settings.quicksave_mode == i);
                if (ImGui::Selectable(_L(s_quicksave_modes[i]), selected))
                    s_settings.quicksave_mode = i;
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();
        ImGui::Text("%s:", _("Quick save filename"));
        float suffix_width = ImGui::CalcTextSize(".mzs").x + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - suffix_width);
        ImGui::InputText("##qsname", s_settings.quicksave_filename,
                         sizeof(s_settings.quicksave_filename));
        ImGui::SameLine();
        ImGui::TextDisabled(".mzs");

        /* Max slots jen pro Rotational mód */
        if (s_settings.quicksave_mode == 2) {
            ImGui::Spacing();
            ImGui::Text("%s:", _("Max snapshots"));
            ImGui::SliderInt("##maxslots", &s_settings.quicksave_max_slots, 2, 20);
        }

        ImGui::Spacing();

        /* Oddělovač — Chování po načtení */
        ImGui::SeparatorText(_("After Load"));

        ImGui::Text("%s:", _("After Load Snapshot"));
        if (ImGui::BeginCombo("##resume_load", _(s_resume_modes[s_settings.load_resume_mode]))) {
            for (int i = 0; i < 3; i++) {
                bool selected = (s_settings.load_resume_mode == i);
                if (ImGui::Selectable(_L(s_resume_modes[i]), selected))
                    s_settings.load_resume_mode = i;
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::Text("%s:", _("After Quick Load"));
        if (ImGui::BeginCombo("##resume_quickload", _(s_resume_modes[s_settings.quickload_resume_mode]))) {
            for (int i = 0; i < 3; i++) {
                bool selected = (s_settings.quickload_resume_mode == i);
                if (ImGui::Selectable(_L(s_resume_modes[i]), selected))
                    s_settings.quickload_resume_mode = i;
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        /* Tlačítka OK / Cancel — zarovnání na střed */
        float total_btn_width = 120 * 2 + ImGui::GetStyle().ItemSpacing.x;
        float avail = ImGui::GetContentRegionAvail().x;
        float offset = (avail - total_btn_width) * 0.5f;
        if (offset > 0.0f)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);

        if (ImGui::Button("OK", ImVec2(120, 0))) {
            /* Aplikovat nastavení */
            g_snapshot_settings = s_settings;
            s_initialized = false;
            g_gui->showSnapshotSetupWindow = false;
        }

        ImGui::SameLine();

        if (ImGui::Button(_L("Cancel"), ImVec2(120, 0))) {
            s_initialized = false;
            g_gui->showSnapshotSetupWindow = false;
        }
    }
    ImGui::End();

    /* Zavření křížkem — zahodit dočasný stav */
    if (!g_gui->showSnapshotSetupWindow) {
        s_initialized = false;
    }

    /* Directory chooser dialog (modální, vykresluje se nad setup oknem) */
    if (s_dir_chooser_open) {
        ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);
        if (ImGuiFileDialog::Instance()->Display("SnapshotDirChooser")) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                std::string dir = ImGuiFileDialog::Instance()->GetCurrentPath();
                snprintf(s_settings.default_directory,
                         sizeof(s_settings.default_directory),
                         "%s", dir.c_str());
            }
            ImGuiFileDialog::Instance()->Close();
            s_dir_chooser_open = false;
        }
    }
}
