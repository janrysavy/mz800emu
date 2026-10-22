/**
 * @file   replay_window.cpp
 * @brief  Okno Replay Control (`###ReplayRemote`): stav záznamu a přehrání
 *         vstupů a jejich ovládání, persistence viditelnosti okna.
 *
 * Plovoucí okno jako dálkové ovládání nahrávání videa
 * (videorec_remote_window.cpp): stav (režim, soubor, snímek a čas, události,
 * hashe, checkpointy, návraty, odchýlení, poslední výsledek) a tlačítka
 * Record Inputs..., Play Replay..., Stop, Rewind a Seek (cílový snímek přímo
 * v okně). U tlačítek se zkratkou je zkratka viditelně vypsaná
 * (IMGUI_REPLAY_SHORTCUT_* z topmenu.h, stejný text jako v nabídce).
 *
 * Stav se čte jen přes replay_get_status() (kopie pod zámkem modulu
 * záznamu), okno tedy nikdy nečeká na emu vlákno. Akce posílají požadavky
 * (replay_request_*()) stejně jako nabídka Replay; souborové dialogy
 * a oznámení výsledku obsluhuje menu_replay.cpp.
 *
 * Viditelnost se ukládá do INI (sekce `[REPLAY_UI]`, klíč `window`); vzor
 * videorec_remote_window.cpp včetně cache pro headless běh, kde g_gui je jen
 * nulový stub.
 *
 * @par Vlákna
 * Vše volá UI (hlavní) vlákno; persistence i z inicializace videa (také
 * hlavní vlákno, před první ImGui frame).
 *
 * @par Licence: GPLv3
 */

#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "libs/imgui/imgui.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/topmenu/topmenu.h"
#include "ui-imgui/videorec/videorec_menu.h"

// Lokalizace
#include "i18n.h"

#include "emulator/replay.h"
#include "emulator/videorec/videorec.h"
#include "emulator/cfgmain.h"
#include "libs/cfgfile/cfgroot.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"

/** Viditelnost okna načtená z INI (cache do vzniku g_gui, a v headless trvale). */
static bool s_cfg_open = false;
/** Cache byla aplikovaná do g_gui (GUI režim) - od té doby je zdrojem pravdy g_gui. */
static bool s_cfg_applied = false;
/** Modul `[REPLAY_UI]` je zaregistrovaný. */
static bool s_cfg_registered = false;
/** Cílový snímek v poli Seek (nastaví se na aktuální snímek při otevření přehrávání). */
static int s_seek_frame = 0;
/** Přehrávání, pro které je pole Seek naplněné (cesta souboru), "" = žádné. */
static char s_seek_for[REPLAY_STATUS_PATH_MAX] = "";

/*******************************************************************************
 *
 *                  Persistence viditelnosti
 *
 ******************************************************************************/

/**
 * @brief INI -> cache viditelnosti (případně rovnou do g_gui, pokud už byla aplikovaná).
 * @param e    Prvek `window` (st_CFGELEMENT*).
 * @param data Nepoužito.
 */
static void rp_cfg_propagate_window(void *e, void *data)
{
    (void)data;
    s_cfg_open = cfgelement_get_bool_value((st_CFGELEMENT *)e) ? true : false;
    if (s_cfg_applied && g_gui) g_gui->showReplayWindow = s_cfg_open;
}

/**
 * @brief Viditelnost -> INI: z g_gui po aplikaci cache, jinak hodnota cache (headless ji nepřepíše).
 * @param e    Prvek `window` (st_CFGELEMENT*).
 * @param data Nepoužito.
 */
static void rp_cfg_save_window(void *e, void *data)
{
    (void)data;
    bool open = (s_cfg_applied && g_gui) ? g_gui->showReplayWindow : s_cfg_open;
    cfgelement_set_bool_value((st_CFGELEMENT *)e, open ? 1 : 0);
}

void imgui_replay_window_cfg_init(void)
{
    if (s_cfg_registered) return;
    s_cfg_registered = true;

    CFGMOD *cmod = cfgroot_register_new_module(g_cfgmain, (char *)"REPLAY_UI");
    if (!cmod) return; /* duplicitní modul - okno pojede bez persistence */

    CFGELM *elm = cfgmodule_register_new_element(cmod, (char *)"window", CFGENTYPE_BOOL, 0);
    cfgelement_set_propagate_cb(elm, rp_cfg_propagate_window, NULL);
    cfgelement_set_save_cb(elm, rp_cfg_save_window, NULL);

    /* Globální propagace se v emulátoru nevolá - modul si sekci načte sám. */
    cfgmodule_parse(cmod);
    cfgmodule_propagate(cmod);
}

void imgui_replay_window_apply_persisted(void)
{
    if (!g_gui) return;
    g_gui->showReplayWindow = s_cfg_open;
    s_cfg_applied = true;
}

/*******************************************************************************
 *
 *                  Okno
 *
 ******************************************************************************/

/**
 * @brief Řádek tabulky stavu: popisek v prvním sloupci, hodnota ve druhém.
 * @param label Přeložený popisek.
 * @param value Hodnota (UTF-8).
 */
static void rp_status_row(const char *label, const char *value)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    ImGui::TextUnformatted(value);
}

/**
 * @brief Text zkratky do druhého sloupce tabulky akcí (mimo BeginDisabled tlačítka).
 * @param label Text zkratky nebo NULL (prázdná buňka).
 */
static void rp_shortcut_hint(const char *label)
{
    if (!label) return;
    ImGui::TableSetColumnIndex(1);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
}

/**
 * @brief Tlačítko akce na novém řádku tabulky akcí.
 * @param label   Popisek s ImGui ID (`_L()`).
 * @param enabled Akce je dostupná.
 * @param width   Šířka tlačítka.
 * @param hint    Text zkratky nebo NULL.
 * @return true = stisknuto.
 */
static bool rp_action(const char *label, bool enabled, float width, const char *hint)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::BeginDisabled(!enabled);
    bool pressed = ImGui::Button(label, ImVec2(width, 0.0f));
    ImGui::EndDisabled();
    rp_shortcut_hint(hint);
    return pressed && enabled;
}

void imgui_replay_window(void)
{
    if (!g_gui->showReplayWindow) return;

    char title[128];
    g_snprintf(title, sizeof(title), "%s###ReplayRemote", _("Replay Control"));
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 60.0f, vp->WorkPos.y + 60.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &g_gui->showReplayWindow, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::End();
        return;
    };

    st_REPLAY_STATUS st;
    replay_get_status(&st);
    bool off = (st.mode == REPLAY_MODE_OFF);
    bool playing = (st.mode == REPLAY_MODE_PLAYING);
    unsigned fps = videorec_get_fps();

    /* Pole Seek: při novém přehrávání začni od aktuálního snímku. */
    if (playing && strcmp(s_seek_for, st.path) != 0)
    {
        g_strlcpy(s_seek_for, st.path, sizeof(s_seek_for));
        s_seek_frame = (int)st.frame;
    }
    else if (!playing)
    {
        s_seek_for[0] = '\0';
    };

    /* Stav */
    if (ImGui::BeginTable("##rp_status", 2, ImGuiTableFlags_SizingFixedFit))
    {
        char buf[REPLAY_STATUS_PATH_MAX + 64];
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(_("State"));
        ImGui::TableSetColumnIndex(1);
        if (st.mode == REPLAY_MODE_RECORDING)
        {
            ImGui::TextColored(ImVec4(0.95f, 0.30f, 0.30f, 1.0f), "%s", _("Recording"));
        }
        else if (playing && st.seeking)
        {
            ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "%s", _("Seeking"));
        }
        else if (playing)
        {
            ImGui::TextColored(ImVec4(0.45f, 0.90f, 0.45f, 1.0f), "%s", _("Playing"));
        }
        else
        {
            ImGui::TextUnformatted(_("Off"));
        };

        if (st.path[0])
        {
            char *base = g_path_get_basename(st.path);
            rp_status_row(_("File"), base);
            g_free(base);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", st.path);
        }
        else
        {
            rp_status_row(_("File"), "-");
        };

        if (!off)
        {
            char t1[64], t2[64];
            videorec_ui_format_time(st.frame, fps, t1, sizeof(t1));
            if (playing)
            {
                videorec_ui_format_time(st.frames, fps, t2, sizeof(t2));
                g_snprintf(buf, sizeof(buf), _("%u / %u (%s / %s)"), st.frame, st.frames, t1, t2);
            }
            else
            {
                g_snprintf(buf, sizeof(buf), "%u (%s)", st.frame, t1);
            };
            rp_status_row(_("Frame"), buf);

            g_snprintf(buf, sizeof(buf), "%u", st.events);
            rp_status_row(_("Events"), buf);

            if (st.checkpoint_interval == 0)
            {
                g_strlcpy(buf, _("off"), sizeof(buf));
            }
            else if (st.checkpoints)
            {
                g_snprintf(buf, sizeof(buf), _("%u (frames %u - %u, %u kB)"), st.checkpoints, st.checkpoint_first,
                           st.checkpoint_last, (unsigned)((st.checkpoint_bytes + 1023) / 1024));
            }
            else
            {
                g_strlcpy(buf, "0", sizeof(buf));
            };
            rp_status_row(_("Checkpoints"), buf);

            if (st.mode == REPLAY_MODE_RECORDING || st.rerecord_count)
            {
                g_snprintf(buf, sizeof(buf), "%u", st.rerecord_count);
                rp_status_row(_("Rewinds"), buf);
            };
            if (playing && st.seeking)
            {
                g_snprintf(buf, sizeof(buf), "%u", st.seek_target);
                rp_status_row(_("Seek target"), buf);
            };
            if (playing && st.deviated)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(_("Deviated"));
                ImGui::TableSetColumnIndex(1);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 22.0f);
                ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "%s", st.deviation);
                ImGui::PopTextWrapPos();
            };
        }
        else if (st.outcome != REPLAY_OUTCOME_NONE)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(_("Last result"));
            ImGui::TableSetColumnIndex(1);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 22.0f);
            if (st.outcome == REPLAY_OUTCOME_DESYNC || st.outcome == REPLAY_OUTCOME_ERROR)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", st.outcome_message);
            }
            else
            {
                ImGui::TextUnformatted(st.outcome_message);
            };
            ImGui::PopTextWrapPos();
        };
        ImGui::EndTable();
    };

    /* Akce: tlačítko + viditelná zkratka. */
    ImGui::SeparatorText(_("Controls"));
    const ImGuiStyle &style = ImGui::GetStyle();
    const char *const widest[] = { N_("Record Inputs..."), N_("Play Replay..."), N_("Stop Replay"), N_("Rewind") };
    float btn_w = 0.0f;
    for (size_t i = 0; i < G_N_ELEMENTS(widest); i++)
    {
        float w = ImGui::CalcTextSize(_(widest[i])).x;
        if (w > btn_w) btn_w = w;
    };
    btn_w += style.FramePadding.x * 2.0f;

    if (ImGui::BeginTable("##rp_actions", 2, ImGuiTableFlags_SizingFixedFit))
    {
        if (rp_action(_L("Record Inputs..."), off, btn_w, NULL)) imgui_replay_open_record_dialog();
        if (rp_action(_L("Play Replay..."), off, btn_w, NULL)) imgui_replay_open_play_dialog();
        if (rp_action(_L("Stop Replay"), !off, btn_w, NULL)) imgui_replay_request(replay_request_stop());
        if (rp_action(_L("Rewind"), !off && st.checkpoints > 0 && !st.seeking, btn_w, IMGUI_REPLAY_SHORTCUT_REWIND))
        {
            imgui_replay_rewind();
        };
        ImGui::EndTable();
    };
    ImGui::TextDisabled("%s", _("Rewind goes back to the previous checkpoint; while recording the inputs after it are dropped."));

    /* Seek: jen při přehrávání. */
    ImGui::SeparatorText(_("Seek"));
    ImGui::BeginDisabled(!playing || st.seeking);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    ImGui::InputInt("##rp_seek_frame", &s_seek_frame, 50, (int)(fps ? fps * 10 : 500));
    if (s_seek_frame < 0) s_seek_frame = 0;
    if (playing && s_seek_frame > (int)st.frames) s_seek_frame = (int)st.frames;
    ImGui::SameLine();
    if (ImGui::Button(_L("Seek")) && playing && !st.seeking)
    {
        imgui_replay_request(replay_request_seek((uint32_t)s_seek_frame));
    };
    ImGui::SameLine();
    if (ImGui::Button(_L("Current")) && playing)
    {
        s_seek_frame = (int)st.frame;
    };
    ImGui::EndDisabled();
    if (playing)
    {
        char t[64];
        videorec_ui_format_time((uint64_t)s_seek_frame, fps, t, sizeof(t));
        ImGui::TextDisabled(_("Frame %d = %s; back via a checkpoint, forward at maximum speed."), s_seek_frame, t);
    }
    else
    {
        ImGui::TextDisabled("%s", _("Available while playing a replay."));
    };

    ImGui::Separator();
    ImGui::TextDisabled(_("Show or hide this window: %s"), IMGUI_REPLAY_SHORTCUT_WINDOW);

    ImGui::End();
}
