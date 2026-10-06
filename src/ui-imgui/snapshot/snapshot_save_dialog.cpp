/**
 * @file snapshot_save_dialog.cpp
 * @brief Dialog pro uložení snapshotu — ImGuiFileDialog + description input
 */

#include "main.h"
#include <stdio.h>
#include <string.h>
#include <string>
#include <glib.h>

#include "libs/imgui/imgui.h"
#include "libs/igfd/ImGuiFileDialog.h"
#include "ui-imgui/filechooser/imgui_filechooser.h"
#include "ui-imgui/bootstrap/myimgui.h"

// Lokalizace
#include "i18n.h"

#include "emulator.h"
#include "ui-imgui/debugger/dbgapi_helpers.h"

extern "C"
{
#include "snapshot/snapshot.h"
}

/* Deklarace z menu_snapshot.cpp */
extern "C" bool snapshot_ui_is_save_requested(void);
extern "C" void snapshot_ui_clear_save_request(void);
extern "C" bool snapshot_ui_was_running_before_pause(void);

/* Deklarace z snapshot_notification.cpp */
extern "C" void snapshot_notification_show(const char *message, bool is_error);

/* Stav dialogu */
static bool s_file_dialog_open = false;
static bool s_description_dialog_open = false;
static char s_selected_path[1024] = "";
static char s_description[256] = "";


static void snapshot_save_restore_emulation(void)
{
    if (snapshot_ui_was_running_before_pause())
        dbg_ui_run();
}


static void snapshot_save_open_file_dialog(void)
{
    IGFD::FileDialogConfig config;
    config.path = g_snapshot_settings.default_directory;
    config.countSelectionMax = 1;
    config.flags = ImGuiFileDialogFlags_Modal |
                   ImGuiFileDialogFlags_DontShowHiddenFiles |
                   ImGuiFileDialogFlags_ShowDevicesButton |
                   ImGuiFileDialogFlags_CaseInsensitiveExtentionFiltering |
                   ImGuiFileDialogFlags_ConfirmOverwrite;

    imgui_filechooser_prepare_config(BASEUI_FCHOOSER_CAT_SNAPSHOT, config);
    ImGuiFileDialog::Instance()->OpenDialog(
        "SnapshotSaveDialog", _("Save Snapshot"), ".mzs", config);
    s_file_dialog_open = true;
}


extern "C" void imgui_snapshot_save_dialog(void)
{
    /* Fáze 1: Otevření souborového dialogu po požadavku z menu */
    if (snapshot_ui_is_save_requested()) {
        snapshot_ui_clear_save_request();
        s_description[0] = '\0';
        snapshot_save_open_file_dialog();
    }

    /* Fáze 2: Zpracování souborového dialogu */
    if (s_file_dialog_open) {
        ImGui::SetNextWindowSize(ImVec2(1200, 768), ImGuiCond_FirstUseEver);
        if (ImGuiFileDialog::Instance()->Display("SnapshotSaveDialog")) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                std::string path = ImGuiFileDialog::Instance()->GetFilePathName();
                snprintf(s_selected_path, sizeof(s_selected_path), "%s", path.c_str());
                s_description_dialog_open = true;
            } else {
                /* Cancel — obnovit emulaci */
                snapshot_save_restore_emulation();
            }
            ImGuiFileDialog::Instance()->Close();
            s_file_dialog_open = false;
        }
    }

    /* Fáze 3: Dialog pro popis snapshotu */
    if (s_description_dialog_open) {
        ImGui::OpenPopup(_L("Save Snapshot"));
        s_description_dialog_open = false;  /* OpenPopup stačí zavolat jednou */
    }

    if (ImGui::BeginPopupModal(_L("Save Snapshot"), NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s:", _("File"));
        ImGui::TextWrapped("%s", s_selected_path);
        ImGui::Spacing();

        ImGui::Text("%s:", _("Description"));
        ImGui::InputTextMultiline("##desc", s_description, sizeof(s_description),
                                   ImVec2(400, 80));
        ImGui::Spacing();

        ImGui::Checkbox(_L("Include RAM disk content"),
                        &g_snapshot_settings.include_ramdisk);
        ImGui::Checkbox(_L("Include MEMEXT content"),
                        &g_snapshot_settings.include_memext);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Button(_L("Save"), ImVec2(120, 0))) {
            /* Provést uložení */
            en_SNAPSHOT_RESULT result = snapshot_save(s_selected_path,
                                                      s_description[0] ? s_description : NULL);
            if (result == SNAPSHOT_OK) {
                char msg[256];
                snprintf(msg, sizeof(msg), "%s: %.200s", _("Snapshot saved"), s_selected_path);
                snapshot_notification_show(msg, false);
            } else {
                char msg[256];
                snprintf(msg, sizeof(msg), "%s: %s", _("Save failed"),
                         snapshot_result_to_string(result));
                snapshot_notification_show(msg, true);
            }
            snapshot_save_restore_emulation();
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();

        if (ImGui::Button(_L("Cancel"), ImVec2(120, 0))) {
            snapshot_save_restore_emulation();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}
