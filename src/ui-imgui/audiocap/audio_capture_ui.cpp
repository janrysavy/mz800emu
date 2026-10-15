/**
 * @file   audio_capture_ui.cpp
 * @brief  Zachycení zvuku z GUI (viz audio_capture_ui.h).
 *
 * @par Licence: GPLv3
 */

#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <SDL3/SDL.h>

#include "libs/imgui/imgui.h"
#include "libs/igfd/ImGuiFileDialog.h"
#include "ui-imgui/filechooser/imgui_filechooser.h"
#include "mzarch/mzarch_config.h"
#include "gdg/video.h"
#include "ui-imgui/audiocap/audio_capture_ui.h"
#include "emulator/audio_capture.h"
#include "emulator/screenshot/screenshot_series.h"

#include "i18n.h"

/* Implementace v snapshot/snapshot_notification.cpp. */
extern "C" void snapshot_notification_show_ex(const char *message, bool is_error, const char *error_title);

/** @brief Zobrazovaný popisek zkratky startu / stopu. */
#define AUDIO_CAPTURE_SHORTCUT_LABEL "Alt+Q"

/** @brief GUI spustilo zachycení a ještě neohlásilo jeho konec. */
static bool s_pending_report = false;
/** @brief Dialog nastavení je otevřený. */
static bool s_setup_open = false;


/** @brief Délka zachycení v sekundách podle počtu zvukových snímků. */
static double ac_ui_seconds(uint64_t frames)
{
    return (double) frames / (double) VIDEO_SCREENS_PER_SEC;
}


/** @brief Otevře výstupní složku zachycení v systémovém prohlížeči souborů. */
static void ac_ui_open_output_folder(void)
{
    char dir[AUDIO_CAPTURE_PATH_MAX];
    audio_capture_resolve_output_dir(dir, sizeof(dir));
    char *abs = g_canonicalize_filename(dir, NULL);
    char *uri = g_filename_to_uri(abs, NULL, NULL);
    if (!uri || !SDL_OpenURL(uri)) {
        char *msg = g_strdup_printf(_("Cannot open folder: %s"), abs);
        snapshot_notification_show_ex(msg, true, _("Audio Capture Error"));
        g_free(msg);
    }
    g_free(uri);
    g_free(abs);
}


void audio_capture_ui_toggle(void)
{
    if (audio_capture_stop()) return; /* běželo - konec ohlásí imgui_audio_capture_poll() */

    st_AUDIO_CAPTURE_PARAMS p;
    audio_capture_params_from_settings(&p);
    char err[AUDIO_CAPTURE_TEXT_MAX] = "";
    if (audio_capture_start(&p, err, sizeof(err))) {
        s_pending_report = true;
    } else {
        char *msg = g_strdup_printf(_("Cannot start the audio capture: %s"), err);
        snapshot_notification_show_ex(msg, true, _("Audio Capture Error"));
        g_free(msg);
    }
}


void imgui_audio_capture_menu(void)
{
    if (!ImGui::BeginMenu(_L("Audio Capture"))) return;
    st_AUDIO_CAPTURE_STATUS st;
    audio_capture_get_status(&st);
    if (ImGui::MenuItem(_L("Record Audio Capture"), AUDIO_CAPTURE_SHORTCUT_LABEL, st.active || st.pending)) {
        audio_capture_ui_toggle();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", _("Records what the sound hardware does into a new folder until you stop it:\n"
                                  "changes of the channel levels, PSG writes, the exact sound (reference.wav)\n"
                                  "and the sound you hear (sdl.wav). For finding sound bugs."));
    }
    ImGui::Separator();
    if (ImGui::MenuItem(_L("Settings..."))) {
        s_setup_open = true;
    }
    if (ImGui::MenuItem(_L("Open Output Folder"))) {
        ac_ui_open_output_folder();
    }
    ImGui::EndMenu();
}


void imgui_audio_capture_shortcuts(void)
{
    ImGuiIO &io = ImGui::GetIO();
    if (!io.KeyAlt || io.KeyCtrl || io.KeyShift || !ImGui::IsKeyPressed(ImGuiKey_Q, false)) return;
    audio_capture_ui_toggle();
}


void imgui_audio_capture_poll(void)
{
    if (!s_pending_report) return;
    st_AUDIO_CAPTURE_STATUS st;
    audio_capture_get_status(&st);
    if (st.active || st.pending) return;
    s_pending_report = false;
    if (st.last_error[0]) {
        char *msg = g_strdup_printf(_("Audio capture stopped after %.1f s: %s"), ac_ui_seconds(st.frames), st.last_error);
        snapshot_notification_show_ex(msg, true, _("Audio Capture Error"));
        g_free(msg);
    } else if (strcmp(st.stop_reason, "Stopped") != 0 && strcmp(st.stop_reason, "Emulator exit") != 0) {
        /* Konec bez stisku stop: reset nebo snapshot (skok času), pojistka. */
        char *msg = g_strdup_printf(_("Audio capture ended (%s): %.1f s saved to %s"),
                                    st.stop_reason, ac_ui_seconds(st.frames), st.dir);
        snapshot_notification_show_ex(msg, false, NULL);
        g_free(msg);
    } else {
        char *msg = g_strdup_printf(_("Audio capture saved: %.1f s to %s"), ac_ui_seconds(st.frames), st.dir);
        snapshot_notification_show_ex(msg, false, NULL);
        g_free(msg);
    }
}


void imgui_audio_capture_overlay(void)
{
    st_AUDIO_CAPTURE_STATUS st;
    audio_capture_get_status(&st);
    if (!st.active && !st.pending) return;

    char text[128];
    if (st.active) {
        g_snprintf(text, sizeof(text), _("AUDIO %.1f s"), ac_ui_seconds(st.frames));
    } else {
        g_snprintf(text, sizeof(text), "%s", _("AUDIO starting"));
    }

    ImVec2 img_min = ImGui::GetItemRectMin();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const float pad = 6.0f;
    const float margin = 10.0f;
    ImVec2 ts = ImGui::CalcTextSize(text);
    float r = ts.y * 0.4f;
    float w = pad + 2.0f * r + pad + ts.x + pad;
    float h = ts.y + 2.0f * pad;
    /* Pod indikátorem série snímků, pokud běží (stejná výška boxu). */
    st_SCREENSHOT_SERIES_STATUS ss;
    screenshot_series_get_status(&ss);
    float y0 = img_min.y + margin + (ss.active ? h + margin * 0.5f : 0.0f);
    ImVec2 box_min(img_min.x + margin, y0);
    ImVec2 box_max(img_min.x + margin + w, y0 + h);
    dl->AddRectFilled(box_min, box_max, IM_COL32(0, 0, 0, 160), 4.0f);
    /* Symbol: tři svislé pruhy (úrovně zvuku). */
    ImVec2 c(box_min.x + pad + r, box_min.y + h * 0.5f);
    ImU32 col = st.active ? IM_COL32(120, 230, 120, 255) : IM_COL32(200, 200, 200, 255);
    float bw = r * 0.5f;
    dl->AddRectFilled(ImVec2(c.x - r, c.y - r * 0.3f), ImVec2(c.x - r + bw, c.y + r), col);
    dl->AddRectFilled(ImVec2(c.x - bw * 0.5f, c.y - r), ImVec2(c.x + bw * 0.5f, c.y + r), col);
    dl->AddRectFilled(ImVec2(c.x + r - bw, c.y - r * 0.6f), ImVec2(c.x + r, c.y + r), col);
    dl->AddText(ImVec2(c.x + r + pad, box_min.y + pad), IM_COL32(255, 255, 255, 255), text);
}


void imgui_audio_capture_setup_dialog(void)
{
    static bool s_initialized = false;
    static bool s_dir_chooser_open = false;
    static st_AUDIO_CAPTURE_SETTINGS s_settings;
    if (!s_setup_open) {
        s_initialized = false;
        return;
    }
    if (!s_initialized) {
        s_settings = g_audio_capture_settings;
        s_initialized = true;
    }

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse;
    char title[128];
    g_snprintf(title, sizeof(title), "%s###AudioCaptureSetup", _("Audio Capture Settings"));

    if (ImGui::Begin(title, &s_setup_open, flags)) {
        ImGui::Text("%s:", _("Files to write"));
        ImGui::Checkbox(_L("events.csv - changes of the channel levels"), &s_settings.events);
        ImGui::Checkbox(_L("psg_writes.csv - PSG writes, decoded"), &s_settings.psg_writes);
        ImGui::Checkbox(_L("reference.wav - exact sound in emulated time"), &s_settings.reference_wav);
        ImGui::Checkbox(_L("sdl.wav - sound of the audio output (what you hear)"), &s_settings.sdl_wav);
        ImGui::Spacing();

        /* Výstupní složka */
        ImGui::Text("%s:", _("Output folder"));
        float btn_width = ImGui::GetFrameHeight();
        ImGui::SetNextItemWidth(360.0f);
        ImGui::InputTextWithHint("##audio_capture_dir", _("(default: audio_captures folder next to the emulator)"),
                                 s_settings.output_dir, sizeof(s_settings.output_dir));
        ImGui::SameLine();
        if (ImGui::Button("...##audio_capture_dir_browse", ImVec2(btn_width, 0))) {
            IGFD::FileDialogConfig config;
            config.path = s_settings.output_dir[0] ? s_settings.output_dir : ".";
            config.countSelectionMax = 1;
            config.flags = ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_DontShowHiddenFiles |
                           ImGuiFileDialogFlags_ShowDevicesButton;
            imgui_filechooser_prepare_config(BASEUI_FCHOOSER_CAT_AUDIO_CAPTURE, config);
            ImGuiFileDialog::Instance()->OpenDialog("AudioCaptureDirChooser", _("Select Directory"), nullptr, config);
            s_dir_chooser_open = true;
        }
        ImGui::TextDisabled("%s", _("Each capture creates a new folder in the output folder."));
        ImGui::TextDisabled(_("Record Audio Capture (%s) starts and stops the capture."), AUDIO_CAPTURE_SHORTCUT_LABEL);
        ImGui::Separator();
        ImGui::Spacing();

        bool any = s_settings.events || s_settings.psg_writes || s_settings.reference_wav || s_settings.sdl_wav;
        float total_btn_width = 120 * 2 + ImGui::GetStyle().ItemSpacing.x;
        float offset = (ImGui::GetContentRegionAvail().x - total_btn_width) * 0.5f;
        if (offset > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
        ImGui::BeginDisabled(!any);
        if (ImGui::Button(_L("OK"), ImVec2(120, 0))) {
            g_audio_capture_settings = s_settings;
            s_setup_open = false;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(_L("Cancel"), ImVec2(120, 0))) {
            s_setup_open = false;
        }
    }
    ImGui::End();
    if (!s_setup_open) s_initialized = false;

    /* Výběr adresáře (modální, nad dialogem) */
    if (s_dir_chooser_open) {
        ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);
        if (ImGuiFileDialog::Instance()->Display("AudioCaptureDirChooser")) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                std::string dir = ImGuiFileDialog::Instance()->GetCurrentPath();
                g_strlcpy(s_settings.output_dir, dir.c_str(), sizeof(s_settings.output_dir));
            }
            ImGuiFileDialog::Instance()->Close();
            s_dir_chooser_open = false;
        }
    }
}
