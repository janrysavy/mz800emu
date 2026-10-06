#include "main.h"
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <glib.h>

#include <filesystem>
#include <string>
#include <algorithm>

// Lokalizace
#include "i18n.h"

#include "libs/imgui/imgui.h"
#include "libs/imgui/imgui_internal.h"
#include "libs/igfd/ImGuiFileDialog.h"
#include "baseui/baseui_filechooser.h"
#include "baseui/baseui_fchooser_lastdir.h"
#include "imgui_filechooser.h"
#include "res/CustomFont.h"
#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"

extern "C"
{
    void imgui_file_chooser_window(void);
    void imgui_filechooser_new(baseui_fchooser_t *fch);
    void imgui_filechooser_settings_init(void);
};

// Cache pro persistované šířky panelů FileDialogu a rozbalení skupin Places
static struct {
    float placesPaneWidth = 200.0f;
    bool placesPaneShown = false;
    float sidePaneWidth = 350.0f;
    bool bookmarksOpen = false; // skupina Bookmarks rozbalená
    bool recentOpen = false;    // skupina Recent rozbalená
    bool devicesOpen = true;    // skupina Devices rozbalená
    bool loaded = false;
} s_igfd_panel_state;

static void places_groups_apply(void);

// --- ImGuiSettingsHandler callbacky ---

static void IGFD_ClearAllFn(ImGuiContext *, ImGuiSettingsHandler *)
{
    s_igfd_panel_state.placesPaneWidth = 200.0f;
    s_igfd_panel_state.placesPaneShown = false;
    s_igfd_panel_state.sidePaneWidth = 350.0f;
    s_igfd_panel_state.bookmarksOpen = false;
    s_igfd_panel_state.recentOpen = false;
    s_igfd_panel_state.devicesOpen = true;
    s_igfd_panel_state.loaded = false;
}

static void *IGFD_ReadOpenFn(ImGuiContext *, ImGuiSettingsHandler *, const char *name)
{
    if (strcmp(name, "Panels") == 0)
        return &s_igfd_panel_state;
    return nullptr;
}

static void IGFD_ReadLineFn(ImGuiContext *, ImGuiSettingsHandler *, void *entry, const char *line)
{
    if (entry != &s_igfd_panel_state)
        return;

    float fval;
    int ival;

    if (sscanf(line, "PlacesPaneWidth=%f", &fval) == 1) {
        s_igfd_panel_state.placesPaneWidth = fval;
        s_igfd_panel_state.loaded = true;
    } else if (sscanf(line, "PlacesPaneShown=%d", &ival) == 1) {
        s_igfd_panel_state.placesPaneShown = (ival != 0);
        s_igfd_panel_state.loaded = true;
    } else if (sscanf(line, "PlacesBookmarksOpen=%d", &ival) == 1) {
        s_igfd_panel_state.bookmarksOpen = (ival != 0);
        s_igfd_panel_state.loaded = true;
    } else if (sscanf(line, "PlacesRecentOpen=%d", &ival) == 1) {
        s_igfd_panel_state.recentOpen = (ival != 0);
        s_igfd_panel_state.loaded = true;
    } else if (sscanf(line, "PlacesDevicesOpen=%d", &ival) == 1) {
        s_igfd_panel_state.devicesOpen = (ival != 0);
        s_igfd_panel_state.loaded = true;
    } else if (sscanf(line, "SidePaneWidth=%f", &fval) == 1) {
        /* Defenzivní clamp - nesmyslně nízká hodnota = ignorovat, vrátit
         * default. Ochrana pro případ, že se v dříve uloženém .ini ocitla
         * 0 (regrese, viz IGFD_WriteAllFn). */
        if (fval < 50.0f) fval = 350.0f;
        s_igfd_panel_state.sidePaneWidth = fval;
        s_igfd_panel_state.loaded = true;
    }
}

static void IGFD_ApplyAllFn(ImGuiContext *, ImGuiSettingsHandler *)
{
    if (!s_igfd_panel_state.loaded)
        return;

    auto *dlg = ImGuiFileDialog::Instance();
    dlg->SetPlacesPaneWidth(s_igfd_panel_state.placesPaneWidth);
    dlg->SetPlacesPaneShown(s_igfd_panel_state.placesPaneShown);
    places_groups_apply();
    // sidePaneWidth se aplikuje při otevření dialogu přes config
}

static void IGFD_WriteAllFn(ImGuiContext *, ImGuiSettingsHandler *handler, ImGuiTextBuffer *buf)
{
    auto *dlg = ImGuiFileDialog::Instance();

    /* Pozor na sidePaneWidth: GetSidePaneWidth() vrací aktuální hodnotu
     * z config posledně otevřeného dialogu. Pokud byl posledně otevřen
     * dialog BEZ sidePane (např. CDL meta.json picker), IGFD vynuluje
     * sidePaneWidth na 0 (configureDialog řádek "if (sidePane==null)
     * sidePaneWidth = 0"). Pak by se 0 zapsala do .ini a další otevření
     * dialogu se sidePane (MZF preview) by mělo neviditelný panel.
     *
     * Proto persistujeme z cached state @ref s_igfd_panel_state, který
     * se updatuje JEN při zavření dialogu, který sidePane má. */
    float cached_sidepane_w = s_igfd_panel_state.sidePaneWidth;
    if (cached_sidepane_w < 50.0f) cached_sidepane_w = 350.0f;  /* defenzivní clamp */

    buf->appendf("[%s][Panels]\n", handler->TypeName);
    buf->appendf("PlacesPaneWidth=%.1f\n", dlg->GetPlacesPaneWidth());
    buf->appendf("PlacesPaneShown=%d\n", dlg->IsPlacesPaneShown() ? 1 : 0);
    buf->appendf("SidePaneWidth=%.1f\n", cached_sidepane_w);
    buf->appendf("PlacesBookmarksOpen=%d\n", s_igfd_panel_state.bookmarksOpen ? 1 : 0);
    buf->appendf("PlacesRecentOpen=%d\n", s_igfd_panel_state.recentOpen ? 1 : 0);
    buf->appendf("PlacesDevicesOpen=%d\n", s_igfd_panel_state.devicesOpen ? 1 : 0);
    buf->append("\n");
}

/* ========================================================================= */
/*          Paměť adresářů: Recent, záložky, zachycení výsledku              */
/* ========================================================================= */

/** @brief Fáze sledování dialogu otevřeného přes imgui_filechooser_prepare_config(). */
typedef enum
{
    LASTDIR_TRACK_IDLE,      /**< nic se nesleduje */
    LASTDIR_TRACK_REQUESTED, /**< prepare proběhlo, OpenDialog ještě nemusel proběhnout */
    LASTDIR_TRACK_OPEN,      /**< dialog je (nebo byl) otevřený, čeká se na zavření */
} lastdir_track_t;

/**
 * @brief Stav napojení IGFD na paměť adresářů.
 *
 * @c mutex chrání @c track, @c category a @c recent_dirty, protože
 * imgui_filechooser_prepare_config() se volá i z EMU vlákna. Ostatní členy
 * používá jen UI vlákno.
 */
static struct
{
    GMutex mutex;
    lastdir_track_t track;               /**< fáze sledovaného dialogu */
    baseui_fchooser_category_t category; /**< kategorie sledovaného dialogu */
    bool recent_dirty;                   /**< skupinu Recent je třeba přestavět */
    bool bookmarks_loaded;               /**< záložky z INI už jsou v IGFD */
    std::string bookmarks_cache;         /**< naposledy uložená serializace záložek */
    std::string recent_group_name;       /**< jméno skupiny Recent (lokalizované při vzniku) */
} s_lastdir_ui = {};

/**
 * @brief Zapamatuje adresář sledovaného dialogu, pokud byl potvrzen.
 *
 * IGFD po Close() zachovává IsOk() i výsledné cesty až do dalšího
 * OpenDialog(), takže výsledek lze vyzvednout i snímek po zavření.
 *
 * @pre Volající drží s_lastdir_ui.mutex a dialog není otevřený.
 * @post track == LASTDIR_TRACK_IDLE.
 */
static void lastdir_collect_result_locked(void)
{
    auto *dlg = ImGuiFileDialog::Instance();
    if (dlg->IsOk())
    {
        /* U souboru adresář, kde byl vybrán; u výběru adresáře adresář sám
         * (baseui pro OPEN_DIR vrací také GetCurrentPath()). */
        std::string dir = dlg->GetCurrentPath();
        baseui_fchooser_lastdir_remember(s_lastdir_ui.category, dir.c_str());
        s_lastdir_ui.recent_dirty = true;
    }
    s_lastdir_ui.track = LASTDIR_TRACK_IDLE;
}

/**
 * @brief Naplní skupinu Recent v panelu Places ze seznamu posledních adresářů.
 * @pre Volá se z UI vlákna, když se panel Places nevykresluje (mimo Display()).
 */
static void lastdir_rebuild_recent_group(void)
{
    auto *dlg = ImGuiFileDialog::Instance();
    if (s_lastdir_ui.recent_group_name.empty())
    {
        s_lastdir_ui.recent_group_name = std::string(ICON_IGFD_FOLDER_OPEN " ") + _("Recent");
        /* Pořadí 5: mezi záložkami (0) a disky (10); needitovatelná =
         * neserializuje se do záložek. */
        dlg->AddPlacesGroup(s_lastdir_ui.recent_group_name, 5, false, s_igfd_panel_state.recentOpen);
    }
    auto *group = dlg->GetPlacesGroupPtr(s_lastdir_ui.recent_group_name);
    if (group == nullptr)
        return;

    group->places.clear();
    char **recent = baseui_fchooser_lastdir_get_recent();
    for (char **p = recent; *p != NULL; p++)
    {
        /* Jméno = poslední složka cesty; plná cesta je v tooltipu (IGFD). */
        char *base = g_path_get_basename(*p);
        bool use_full = (base[0] == '\0' || strcmp(base, ".") == 0 || strcmp(base, G_DIR_SEPARATOR_S) == 0);
        group->AddPlace(use_full ? *p : base, *p, false);
        g_free(base);
    }
    g_strfreev(recent);
}

/**
 * @brief Vrátí ukazatel na příznak rozbalení v cache pro skupinu Places.
 * @param i Index skupiny: 0 = Bookmarks, 1 = Recent, 2 = Devices.
 * @param name [out] Jméno skupiny v IGFD (u Recent prázdné, dokud skupina nevznikla).
 * @return Ukazatel do s_igfd_panel_state.
 */
static bool *places_group_state(int i, std::string &name)
{
    switch (i)
    {
    case 0:
        name = placesBookmarksGroupName;
        return &s_igfd_panel_state.bookmarksOpen;
    case 1:
        name = s_lastdir_ui.recent_group_name;
        return &s_igfd_panel_state.recentOpen;
    default:
        name = placesDevicesGroupName;
        return &s_igfd_panel_state.devicesOpen;
    }
}

/**
 * @brief Nastaví rozbalení skupin Places v IGFD podle cache (po načtení imgui.ini).
 * @pre UI vlákno, mimo Display(). Neexistující skupinu (Recent před prvním
 *      otevřením dialogu) přeskočí - ta převezme stav z cache při vzniku.
 */
static void places_groups_apply(void)
{
    for (int i = 0; i < 3; i++)
    {
        std::string name;
        bool *state = places_group_state(i, name);
        auto *group = name.empty() ? nullptr : ImGuiFileDialog::Instance()->GetPlacesGroupPtr(name);
        if (group != nullptr)
            group->opened = *state;
    }
}

/**
 * @brief Přenese rozbalení skupin Places z IGFD do cache (uživatel klikl na hlavičku).
 * @return true, pokud se některý stav změnil (je třeba uložit imgui.ini).
 */
static bool places_groups_sync(void)
{
    bool changed = false;
    for (int i = 0; i < 3; i++)
    {
        std::string name;
        bool *state = places_group_state(i, name);
        auto *group = name.empty() ? nullptr : ImGuiFileDialog::Instance()->GetPlacesGroupPtr(name);
        if (group != nullptr && group->opened != *state)
        {
            *state = group->opened;
            changed = true;
        }
    }
    return changed;
}

/**
 * @brief Jednou za snímek: údržba paměti adresářů pro všechny IGFD dialogy.
 *
 * - při prvním volání načte záložky z INI do IGFD,
 * - přestaví skupinu Recent, když se změnila,
 * - sleduje otevření a zavření dialogu a po potvrzení zapamatuje adresář,
 * - když je dialog otevřený, ukládá změny záložek do paměti (INI se zapíše
 *   při ukončení).
 *
 * @pre Volat z UI vlákna, mimo vykreslování dialogu.
 */
static void lastdir_frame_update(void)
{
    auto *dlg = ImGuiFileDialog::Instance();

    if (!s_lastdir_ui.bookmarks_loaded)
    {
        char *bm = baseui_fchooser_lastdir_get_bookmarks();
        dlg->DeserializePlaces(bm);
        s_lastdir_ui.bookmarks_cache = bm;
        g_free(bm);
        s_lastdir_ui.bookmarks_loaded = true;
    }

    bool open = dlg->IsOpened();

    g_mutex_lock(&s_lastdir_ui.mutex);
    if (s_lastdir_ui.track == LASTDIR_TRACK_REQUESTED && open)
        s_lastdir_ui.track = LASTDIR_TRACK_OPEN;
    else if (s_lastdir_ui.track == LASTDIR_TRACK_OPEN && !open)
        lastdir_collect_result_locked();
    bool rebuild = s_lastdir_ui.recent_dirty;
    s_lastdir_ui.recent_dirty = false;
    g_mutex_unlock(&s_lastdir_ui.mutex);

    if (rebuild)
        lastdir_rebuild_recent_group();

    /* Záložky se mění jen v otevřeném dialogu (+/-, přejmenování). */
    if (open)
    {
        std::string bm = dlg->SerializePlaces();
        if (bm != s_lastdir_ui.bookmarks_cache)
        {
            baseui_fchooser_lastdir_set_bookmarks(bm.c_str());
            s_lastdir_ui.bookmarks_cache = bm;
        }
    }
}

void imgui_filechooser_prepare_config(baseui_fchooser_category_t category, IGFD::FileDialogConfig &config)
{
    g_mutex_lock(&s_lastdir_ui.mutex);
    if (!ImGuiFileDialog::Instance()->IsOpened())
    {
        /* Předchozí dialog zavřený ve stejném snímku (řetězené dialogy) -
         * vyzvednout jeho výsledek dřív, než ho OpenDialog() smaže. */
        if (s_lastdir_ui.track == LASTDIR_TRACK_OPEN)
            lastdir_collect_result_locked();
        s_lastdir_ui.track = LASTDIR_TRACK_REQUESTED;
        s_lastdir_ui.category = category;
        s_lastdir_ui.recent_dirty = true;
    }
    g_mutex_unlock(&s_lastdir_ui.mutex);

    char *out_path = NULL;
    char *out_fileName = NULL;
    char *out_filePathName = NULL;
    baseui_fchooser_lastdir_resolve(category,
                                    config.path.empty() ? NULL : config.path.c_str(),
                                    config.fileName.empty() ? NULL : config.fileName.c_str(),
                                    config.filePathName.empty() ? NULL : config.filePathName.c_str(),
                                    &out_path, &out_fileName, &out_filePathName);

    config.filePathName = (out_filePathName != NULL) ? out_filePathName : "";
    config.path = (out_path != NULL) ? out_path : "";
    config.fileName = (out_fileName != NULL) ? out_fileName : "";

    g_free(out_path);
    g_free(out_fileName);
    g_free(out_filePathName);
}

void imgui_filechooser_settings_init(void)
{
    ImGuiSettingsHandler handler;
    handler.TypeName = "FileDialog";
    handler.TypeHash = ImHashStr("FileDialog");
    handler.ClearAllFn = IGFD_ClearAllFn;
    handler.ReadOpenFn = IGFD_ReadOpenFn;
    handler.ReadLineFn = IGFD_ReadLineFn;
    handler.ApplyAllFn = IGFD_ApplyAllFn;
    handler.WriteAllFn = IGFD_WriteAllFn;
    ImGui::AddSettingsHandler(&handler);
}

bool canValidateDialog = true;

// Callback pro pravý panel (custom pane)
void MZ_InfoPane(const char *vFilter, IGFD::UserDatas vUserDatas, bool *vCantContinue)
{
    (void)vFilter;
    (void)vUserDatas;
    (void)vCantContinue;
    // Pokud nechceme povolit vyber aktualniho souboru, tak zde nastavime false
    // if (vCantContinue)
    //     *vCantContinue = canValidateDialog;

    // Získáme aktuálně vybraný soubor v dialogu
    std::string selectedFile = ImGuiFileDialog::Instance()->GetCurrentFileName();
    std::string fullPath = ImGuiFileDialog::Instance()->GetCurrentPath() + "/" + selectedFile;

    if (selectedFile.empty() || selectedFile == ".." || selectedFile.size() <= 4 || (!std::filesystem::exists(fullPath)))
    {
        // g_print("Bad file: '%s'\n", selectedFile.c_str());
        ImGui::Dummy(ImVec2(1, 1));
        return;
    };

    std::string extension = selectedFile.substr(selectedFile.size() - 4);
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

    // Ověříme, zda má soubor příponu .mzf
    if (extension == ".mzf")
    {
        // g_print("MZF File: '%s'\n", selectedFile.c_str());
        ImGui::TextColored(ImVec4(1, 1, 0, 1), ICON_MZGLYPH_CASETTE " ");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1, 1, 0, 1), _("MZF File Info"));

        ImGui::Separator();

        try
        {
            auto fileSize = std::filesystem::file_size(fullPath);
            ImGui::Text("Size: %" PRIu64 " bytes", (uint64_t)fileSize);

            ImGui::Text("\n");
            ImGui::SeparatorText("MZF Header");

            // Otevřeme soubor
            FILE *file = fopen(fullPath.c_str(), "rb");
            if (!file)
            {
                fprintf(stderr, "%s(%d): Failed to open file: %s\n", __FILE__, __LINE__, fullPath.c_str());
                return;
            };

            // Cely soubor nacteme do pameti
            uint8_t *buffer = (uint8_t *)malloc(fileSize);
            if (!buffer)
            {
                fprintf(stderr, "%s(%d): Failed to allocate memory for file buffer\n", __FILE__, __LINE__);
                fclose(file);
                return;
            };

            size_t bytesRead = fread(buffer, 1, fileSize, file);
            if (bytesRead != fileSize)
            {
                fprintf(stderr, "%s(%d): Failed to read file: %s\n", __FILE__, __LINE__, fullPath.c_str());
                free(buffer);
                fclose(file);
                return;
            };

            fclose(file);

            st_MZF_HEADER *mzfhdr = (st_MZF_HEADER *)buffer;
            char fname[MZF_FNAME_FULL_LENGTH];
            mzf_tools_get_fname(mzfhdr, fname);

            if (ImGui::BeginTable("FileInfoTable", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 50.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                // NAME
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text(_("NAME:"));
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "%s", fname);

                // TYPE
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text(_("TYPE:"));
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "0x%02x", mzfhdr->ftype);

                // SIZE
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text(_("SIZE:"));
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "0x%04x", mzfhdr->fsize);

                // STRT
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text(_("STRT:"));
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "0x%04x", mzfhdr->fstrt);

                // EXEC
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Text(_("EXEC:"));
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "0x%04x", mzfhdr->fexec);

                ImGui::EndTable();
            };
            free(buffer);
        }
        catch (...)
        {
            ImGui::Text("Size: Unknown");
        };
    }
    else
    {
        // g_print("OTHER File: '%s'\n", selectedFile.c_str());
        ImGui::Dummy(ImVec2(1, 1)); // ImGui neumožňuje prázdné panely, proto vložíme neviditelný element
    }
}

void imgui_file_chooser_window(void)
{
    // ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.5f, 0.0f, 1.0f)); // Oranžová při hoveru
    ImGui::SetNextWindowSize(ImVec2(1200, 768), ImGuiCond_FirstUseEver);

    /* Paměť adresářů pro všechny IGFD dialogy (i ty z debuggeru). */
    lastdir_frame_update();

    /* Detekce změny šířky Devices panelu (placesPaneWidth) během dialog
     * session - splitter drag mění m_PlacesPaneWidth přímo bez ImGui dirty
     * markeru, takže auto-save .ini neuložil změněnou hodnotu. Per frame
     * porovnáme s cached state a při změně markneme dirty + updatujeme
     * cache. Stejně tak pro placesPaneShown a sidePaneWidth. */
    auto *dlg_persist = ImGuiFileDialog::Instance();
    {
        float pw = dlg_persist->GetPlacesPaneWidth();
        bool ps = dlg_persist->IsPlacesPaneShown();
        float sw = dlg_persist->GetSidePaneWidth();
        bool changed = false;
        if (pw != s_igfd_panel_state.placesPaneWidth)
        {
            s_igfd_panel_state.placesPaneWidth = pw;
            changed = true;
        };
        if (ps != s_igfd_panel_state.placesPaneShown)
        {
            s_igfd_panel_state.placesPaneShown = ps;
            changed = true;
        };
        /* sidePaneWidth: GetSidePaneWidth vrací 0 pokud aktuální dialog
         * nepoužívá sidePane - takový update do cache nepouštíme. */
        if (sw >= 50.0f && sw != s_igfd_panel_state.sidePaneWidth)
        {
            s_igfd_panel_state.sidePaneWidth = sw;
            changed = true;
        };
        if (places_groups_sync())
            changed = true;
        if (changed)
            ImGui::MarkIniSettingsDirty();
    }

    if (ImGuiFileDialog::Instance()->Display("ChooseFileDlgKey"))
    {
        baseui_fchooser_t *fch = (baseui_fchooser_t *)ImGuiFileDialog::Instance()->GetUserDatas();
        if (fch == NULL)
        {
            fprintf(stderr, "%s(%d): Invalid filechooser data\n", __FILE__, __LINE__);
            s_igfd_panel_state.placesPaneWidth = ImGuiFileDialog::Instance()->GetPlacesPaneWidth();
            s_igfd_panel_state.placesPaneShown = ImGuiFileDialog::Instance()->IsPlacesPaneShown();
            s_igfd_panel_state.sidePaneWidth = ImGuiFileDialog::Instance()->GetSidePaneWidth();
            ImGui::MarkIniSettingsDirty();
            ImGuiFileDialog::Instance()->Close();
            // ImGui::PopStyleColor(); // Vrácení zpět na původní barvu
            return;
        };

        g_mutex_lock(&fch->mutex);

        if (ImGuiFileDialog::Instance()->IsOk())
        {
            std::string filePathName = ImGuiFileDialog::Instance()->GetFilePathName();
            std::string filePath = ImGuiFileDialog::Instance()->GetCurrentPath();
            // action
            fch->selected_filePathName = g_strdup(filePathName.c_str());
            fch->selected_path = g_strdup(filePath.c_str());
            fch->state = BASEUI_FCHOOSER_STATE_CLOSED_OK;
        }
        else
        {
            fch->state = BASEUI_FCHOOSER_STATE_CLOSED_CANCEL;
        }

        // Aktualizace cache šířek panelů před zavřením
        s_igfd_panel_state.placesPaneWidth = ImGuiFileDialog::Instance()->GetPlacesPaneWidth();
        s_igfd_panel_state.placesPaneShown = ImGuiFileDialog::Instance()->IsPlacesPaneShown();
        s_igfd_panel_state.sidePaneWidth = ImGuiFileDialog::Instance()->GetSidePaneWidth();
        ImGui::MarkIniSettingsDirty();

        // close
        ImGuiFileDialog::Instance()->Close();

        BaseuiFchooserCb cb = fch->cb;
        g_cond_signal(&fch->cond);
        g_mutex_unlock(&fch->mutex);

        if (cb)
            cb(fch);
    }
    // ImGui::PopStyleColor(); // Vrácení zpět na původní barvu
}

void imgui_filechooser_new(baseui_fchooser_t *fch)
{
    if (fch == NULL)
        return;

    g_mutex_lock(&fch->mutex);

    fch->selected_filePathName = NULL;
    fch->selected_path = NULL;

    // Pokud jiz exisuje bezici dialog, tak vratime error
    if (ImGuiFileDialog::Instance()->IsOpened("ChooseFileDlgKey"))
    {
        fprintf(stderr, "%s(%d): Filechooser dialog is already opened\n", __FILE__, __LINE__);
        fch->state = BASEUI_FCHOOSER_STATE_CLOSED_ERROR;

        g_cond_signal(&fch->cond);
        g_mutex_unlock(&fch->mutex);

        if (fch->cb)
            fch->cb(fch);
        return;
    };

    fch->state = BASEUI_FCHOOSER_STATE_OPENED;

    const char *default_title = _("Untitled File Chooser");
    const char *default_filter = ".*";
    const char *default_path = ".";

    const char *use_title = (fch->title != NULL) ? fch->title : default_title;
    const char *use_filter = NULL;

    IGFD::FileDialogConfig config;

    /* Výchozí hodnoty volajícího; "." a prázdné hodnoty nahradí paměť
     * adresářů podle kategorie. (Pozn.: std::string nelze přiřadit NULL.) */
    if (fch->filePathName != NULL)
    {
        config.filePathName = fch->filePathName;
    }
    else
    {
        config.path = (fch->path != NULL) ? fch->path : default_path;
        if (fch->fileName != NULL)
            config.fileName = fch->fileName;
    };
    imgui_filechooser_prepare_config(fch->category, config);

    config.countSelectionMax = 1;
    config.flags = ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_DontShowHiddenFiles | ImGuiFileDialogFlags_ShowDevicesButton | ImGuiFileDialogFlags_CaseInsensitiveExtentionFiltering;
    switch (fch->type)
    {
    case BASEUI_FCHOOSER_TYPE_OPEN_FILE:
        config.flags |= ImGuiFileDialogFlags_ReadOnlyFileNameField;
        use_filter = (fch->filter != NULL) ? fch->filter : default_filter;
        break;

    case BASEUI_FCHOOSER_TYPE_SAVE_FILE:
        config.flags |= ImGuiFileDialogFlags_ConfirmOverwrite;
        use_filter = (fch->filter != NULL) ? fch->filter : default_filter;
        break;

    case BASEUI_FCHOOSER_TYPE_RW_FILE:
        use_filter = (fch->filter != NULL) ? fch->filter : default_filter;
        break;

    default:
        break;
    }

    // Použití custom panelu pro .mzf soubory
    config.sidePane = std::bind(&MZ_InfoPane, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
    config.sidePaneWidth = s_igfd_panel_state.sidePaneWidth;
    config.userDatas = fch;

    ImVec4 mzfColor = ImVec4(0.0f, 1.0f, 1.0f, 0.9f);
    const char *mzfIcon = ICON_MZGLYPH_CASETTE;

    ImVec4 dskColor = ImVec4(0.0f, 1.0f, 1.0f, 0.9f);
    const char *dskIcon = ICON_MZGLYPH_FLOPPY_DISC;

    ImGuiFileDialog::Instance()->SetFileStyle(IGFD_FileStyleByExtention, ".mzf", mzfColor, mzfIcon);
    ImGuiFileDialog::Instance()->SetFileStyle(IGFD_FileStyleByExtention, ".MZF", mzfColor, mzfIcon);
    ImGuiFileDialog::Instance()->SetFileStyle(IGFD_FileStyleByExtention, ".m12", mzfColor, mzfIcon);
    ImGuiFileDialog::Instance()->SetFileStyle(IGFD_FileStyleByExtention, ".M12", mzfColor, mzfIcon);

    ImGuiFileDialog::Instance()->SetFileStyle(IGFD_FileStyleByExtention, ".dsk", dskColor, dskIcon);
    ImGuiFileDialog::Instance()->SetFileStyle(IGFD_FileStyleByExtention, ".DSK", dskColor, dskIcon);

    // const char *filters = ".*, .mzf";
    const char *filters = use_filter;
    ImGuiFileDialog::Instance()->OpenDialog("ChooseFileDlgKey", use_title, filters, config);

    g_cond_signal(&fch->cond);
    g_mutex_unlock(&fch->mutex);
}