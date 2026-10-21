/*
 * File:   mcp_activity_window.cpp
 *
 * Implementace MCP Activity okna - viz mcp_activity_window.h pro
 * architekturu, threading model a scope V1.C.2.
 *
 * Klíčová designová rozhodnutí:
 *
 *   1. Ring buffer = std::deque<ActivityEntry>. Drop-tail-old při
 *      přetečení MCP_ACTIVITY_RING_MAX_ENTRIES (pop_front + push_back).
 *      std::deque má amortizované O(1) na obou koncích a iteruje se
 *      lineárně pro render tabulky.
 *
 *   2. Filtr = case-insensitive substring nad sloupcem Description.
 *      Filtruje se při renderu, ne při zápisu - počet pamětí stejný
 *      bez ohledu na uživatelský filter.
 *
 *   3. Owner badge = textový string "[AI]"/"[USR]"/"[TEST]"/"[SYS]".
 *      Emoji se zatím nepoužívají - defaultní ImGui font neobsahuje
 *      celou Unicode tabulku. Textová varianta je čitelná a stable.
 *
 *   4. Pause stav je per-frame static lokální (= žádný persist).
 *      Auto-scroll také static. Při zavření a opětovném otevření okna
 *      se hodnoty udrží (= jsou static, ne member).
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------
 */

#include "main.h"
#include "libs/imgui/imgui.h"
#include "i18n.h"  /* lokalizace _() / _L() */
#include "mcp_activity_window.h"
#include "owner_badge.h"  /* V1.C.3 - sdílený badge renderer */

/* V1.E.6.A: dvojklik routing - includes pro 4 _focus_* API. */
#include "emulator/debugger/dbgapi_msg.h"  /* DBGAPI_ENTITY_KIND_* makra */
#include "ui-imgui/debugger/breakpoints/bpt_window.h"   /* bpt_window_focus_id */
#include "ui-imgui/debugger/watch/watch_window.h"       /* watch_window_focus_id */
#include "ui-imgui/debugger/symbols/sym_window.h"       /* sym_window_focus_addr */
#include "ui-imgui/debugger/bookmarks/bm_window.h"      /* bm_window_focus_id */

#ifdef MZ800EMU_CFG_MCP_TCP_ENABLED
#include "emulator/mcp/mcp_config.h"  /* V1.D.2.C - action_log_path */
#endif

#include <deque>
#include <string>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <ctime>

#include <glib.h>
#include <glib/gstdio.h>  /* V1.D.2.C - g_fopen pro action_log_path */

/* ============================================================================
 * Interní data
 * ============================================================================ */

/**
 * @brief Jeden záznam v ring bufferu.
 *
 * Description je vlastní kopie (std::string) - ring buffer si data drží
 * nezávisle na volajícím. cmd ukládáme jako enum hodnotu pro budoucí
 * routing (= klik na řádek otevře relevantní okno typu BP/Watch/Mem).
 *
 * V1.E.6.A: entity_kind + entity_id držíme z MSG_DATA pro dvojklikový
 * routing do cílového okna. entity_kind == DBGAPI_ENTITY_KIND_NONE
 * znamená "bez routing target" - dvojklik takový řádek nic neudělá.
 * Význam entity_id závisí na entity_kind (viz dbgapi_msg.h makra).
 *
 * Invariant: timestamp_us je monotonic-clock z g_get_monotonic_time(),
 * tj. delty mezi záznamy jsou smysluplné, absolutní hodnota není
 * wall-clock UTC.
 */
struct ActivityEntry
{
    uint64_t timestamp_us;
    en_DBGAPI_CMD_ORIGIN origin;
    en_DBGAPI_CMD cmd;
    std::string description;
    uint8_t entity_kind;   /**< DBGAPI_ENTITY_KIND_* (0 = NONE). */
    int32_t entity_id;     /**< ID nebo adresa cílové entity. */
    bool rejected;         /**< Podmíněný zápis odmítnutý nesplněnou podmínkou (nic se nezměnilo). */
};

/** @brief Ring buffer záznamů (drop-tail-old). */
static std::deque<ActivityEntry> g_log_ring;

/**
 * @brief Mutex chránící g_log_ring.
 *
 * V1.C.2 oba callsity (log_action i render) běží v UI vlákně, mutex je
 * tedy defenzivní. Připraven na budoucí externí zápis (= harvest MCP
 * wrapper logu z jiného vlákna).
 */
static GMutex g_log_mutex;

/** @brief Stav inicializace - true mezi init a shutdown. */
static bool g_initialized = false;

/** @brief Pause flag - true zastaví appending nových záznamů. */
static bool g_paused = false;

/* ============================================================================
 * Pomocné funkce - badge a formátování
 * ============================================================================ */

/**
 * @brief Vykreslí časový sloupec ve formátu HH:MM:SS.mmm.
 *
 * Vstup je monotonic timestamp v mikrosekundách. Konvertujeme na
 * sekundy modulo 24 hodin pro čitelnost. Vlastní hodnota není
 * wall-clock - je to relativní čas od startu emulátoru.
 */
static void render_timestamp_cell(uint64_t ts_us)
{
    uint64_t ts_ms = ts_us / 1000ULL;
    uint64_t total_s = ts_ms / 1000ULL;
    uint32_t ms = (uint32_t)(ts_ms % 1000ULL);
    uint32_t s = (uint32_t)(total_s % 60ULL);
    uint32_t m = (uint32_t)((total_s / 60ULL) % 60ULL);
    uint32_t h = (uint32_t)((total_s / 3600ULL) % 24ULL);
    ImGui::Text("%02u:%02u:%02u.%03u", h, m, s, ms);
}

/**
 * @brief Case-insensitive substring vyhledávání.
 *
 * Vrací true pokud `needle` (nebo prázdný string) je obsažen v `haystack`.
 * Prázdný needle = vždy match (= žádný filtr).
 */
static bool ci_substr(const char *haystack, const char *needle)
{
    if (!needle || !needle[0])
    {
        return true;
    }
    if (!haystack)
    {
        return false;
    }
    size_t nlen = std::strlen(needle);
    size_t hlen = std::strlen(haystack);
    if (nlen > hlen)
    {
        return false;
    }
    for (size_t i = 0; i + nlen <= hlen; ++i)
    {
        size_t j = 0;
        for (; j < nlen; ++j)
        {
            int a = std::tolower((unsigned char)haystack[i + j]);
            int b = std::tolower((unsigned char)needle[j]);
            if (a != b)
            {
                break;
            }
        }
        if (j == nlen)
        {
            return true;
        }
    }
    return false;
}

/* ============================================================================
 * Veřejné API - lifecycle
 * ============================================================================ */

extern "C" void mcp_activity_window_init(void)
{
    if (g_initialized)
    {
        return;
    }
    g_mutex_init(&g_log_mutex);
    g_log_ring.clear();
    g_paused = false;
    g_initialized = true;
}

extern "C" void mcp_activity_window_shutdown(void)
{
    if (!g_initialized)
    {
        return;
    }
    g_mutex_lock(&g_log_mutex);
    g_log_ring.clear();
    g_mutex_unlock(&g_log_mutex);
    g_mutex_clear(&g_log_mutex);
    g_initialized = false;
}

extern "C" void mcp_activity_window_log_action(en_DBGAPI_CMD_ORIGIN origin,
                                                en_DBGAPI_CMD cmd,
                                                const char *description,
                                                uint64_t timestamp_us,
                                                uint8_t entity_kind,
                                                int32_t entity_id,
                                                bool rejected)
{
    if (!g_initialized)
    {
        return;
    }

    g_mutex_lock(&g_log_mutex);

    /* V Pause módu ignorujeme nové záznamy. Toolbar Pause checkbox ho
     * přepíná. Pozor: drop pouze pro NOVÉ záznamy - již zapsané v ringu
     * zůstávají (= konzistentní s tlačítkem Pause na video playeru). */
    if (g_paused)
    {
        g_mutex_unlock(&g_log_mutex);
        return;
    }

    /* Drop-tail-old při dosažení kapacity. */
    while (g_log_ring.size() >= MCP_ACTIVITY_RING_MAX_ENTRIES)
    {
        g_log_ring.pop_front();
    }

    ActivityEntry entry;
    entry.timestamp_us = timestamp_us;
    entry.origin = origin;
    entry.cmd = cmd;
    entry.description = description ? description : "";
    entry.entity_kind = entity_kind;
    entry.entity_id = entity_id;
    entry.rejected = rejected;
    g_log_ring.push_back(std::move(entry));

    g_mutex_unlock(&g_log_mutex);

#ifdef MZ800EMU_CFG_MCP_TCP_ENABLED
    /* V1.D.2.C - disk persist Activity log entry (best-effort).
     *
     * Pokud uživatel nastavil [MCP] action_log_path v INI, appenduje
     * každý zaznamenaný entry do souboru ve formátu:
     *
     *   <ISO8601> [<origin>] <cmd>: <description>
     *
     * Best-effort: chyba otevření/zápisu se ignoruje (= ring buffer
     * v RAM zůstává validním záznamem). EN format jako wire convention.
     */
    const char *log_path = mcp_config_get_action_log_path();
    if (log_path && log_path[0])
    {
        FILE *f = g_fopen(log_path, "a");
        if (f)
        {
            /* ISO 8601 UTC timestamp ze sekundové wall-clock (= log
             * persist se synchronizuje s ostatními system logy lépe
             * než monotonic timestamp_us). */
            char iso[40];
            time_t now_sec = time(NULL);
            struct tm tm_utc;
#ifdef _WIN32
            gmtime_s(&tm_utc, &now_sec);
#else
            gmtime_r(&now_sec, &tm_utc);
#endif
            strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

            const char *origin_str = "user";
            switch (origin)
            {
                case DBGAPI_CMD_ORIGIN_MCP:      origin_str = "mcp";      break;
                case DBGAPI_CMD_ORIGIN_TEST:     origin_str = "test";     break;
                case DBGAPI_CMD_ORIGIN_INTERNAL: origin_str = "internal"; break;
                case DBGAPI_CMD_ORIGIN_USER:
                default:                         origin_str = "user";     break;
            }

            fprintf(f, "%s [%s] %s: %s%s\n",
                    iso, origin_str,
                    dbgapi_cmd_to_str(cmd),
                    description ? description : "",
                    rejected ? " (rejected)" : "");
            fclose(f);
        }
        /* Tichý fallback pokud open/write selže (= disk full, perm
         * issue, atd.). */
    }
#endif /* MZ800EMU_CFG_MCP_TCP_ENABLED */
}

/* ============================================================================
 * Render
 * ============================================================================ */

extern "C" void mcp_activity_window_render(bool *p_open)
{
    if (!p_open || !*p_open)
    {
        return;
    }
    if (!g_initialized)
    {
        /* Defenzivní - okno nesmí render-ovat před init. */
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(800.0f, 400.0f), ImGuiCond_FirstUseEver);

    /* Stable ID per memory feedback_imgui_window_id_three_hashes:
     * tři hashe = title je čistě cosmetic, ImGui ID = mcp_activity_window. */
    if (!ImGui::Begin(_L("MCP Activity###mcp_activity_window"),
                      p_open,
                      ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    }

    /* --- Toolbar --- */
    if (ImGui::Button(_L("Clear##mcp_act_clear")))
    {
        g_mutex_lock(&g_log_mutex);
        g_log_ring.clear();
        g_mutex_unlock(&g_log_mutex);
    }
    ImGui::SameLine();

    /* Pause toggle - sdílíme s global g_paused, aby se nový log_action
     * ze samostatného vlákna zachoval konzistentně mezi framy. */
    bool paused_local = g_paused;
    if (ImGui::Checkbox(_L("Pause##mcp_act_pause"), &paused_local))
    {
        g_mutex_lock(&g_log_mutex);
        g_paused = paused_local;
        g_mutex_unlock(&g_log_mutex);
    }
    ImGui::SameLine();

    static bool auto_scroll = true;
    ImGui::Checkbox(_L("Auto-scroll##mcp_act_scroll"), &auto_scroll);
    ImGui::SameLine();

    /* Filter - case-insensitive substring nad description sloupcem. */
    static char filter_buf[128] = "";
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##mcp_act_filter",
                             _("Filter description..."),
                             filter_buf, sizeof(filter_buf));

    /* V1.C.3 - per-origin filter (4 checkboxy, default USER + MCP on,
     * TEST + INTERNAL off = redukce noise pro běžnou práci). Stav je
     * static = persistuje across frames v rámci session, nepřežije restart. */
    static bool show_user = true;
    static bool show_mcp = true;
    static bool show_test = false;
    static bool show_internal = false;
    ImGui::Separator();
    ImGui::TextUnformatted(_("Origin:"));
    ImGui::SameLine();
    ImGui::Checkbox(_L("USER##mcp_act_org_user"), &show_user);
    ImGui::SameLine();
    ImGui::Checkbox(_L("AI##mcp_act_org_mcp"), &show_mcp);
    ImGui::SameLine();
    ImGui::Checkbox(_L("TEST##mcp_act_org_test"), &show_test);
    ImGui::SameLine();
    ImGui::Checkbox(_L("SYS##mcp_act_org_sys"), &show_internal);

    ImGui::Separator();

    /* --- Status bar --- */
    g_mutex_lock(&g_log_mutex);
    size_t total_entries = g_log_ring.size();
    size_t shown_entries = 0;
    /* Snapshot copy pro render - vyhneme se držení mutexu během
     * ImGui table loop (= ImGui může reentrant volat plotting, atd.). */
    std::deque<ActivityEntry> snapshot = g_log_ring;
    g_mutex_unlock(&g_log_mutex);

    /* V1.C.3 - kombinovaný filter: text substring + origin checkbox.
     * Origin matchuje pokud odpovídající show_* flag je true. */
    auto origin_allowed = [&](en_DBGAPI_CMD_ORIGIN o) {
        switch (o)
        {
            case DBGAPI_CMD_ORIGIN_USER:     return show_user;
            case DBGAPI_CMD_ORIGIN_MCP:      return show_mcp;
            case DBGAPI_CMD_ORIGIN_TEST:     return show_test;
            case DBGAPI_CMD_ORIGIN_INTERNAL: return show_internal;
        }
        return true;
    };

    for (const auto &e : snapshot)
    {
        if (!origin_allowed(e.origin)) continue;
        if (ci_substr(e.description.c_str(), filter_buf))
        {
            ++shown_entries;
        }
    }

    ImGui::Text("%s: %zu / %zu",
                _("Entries (shown / total)"),
                shown_entries, total_entries);
    if (g_paused)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "  [%s]",
                           _("PAUSED"));
    }

    ImGui::Separator();

    /* --- Tabulka --- */
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders
                                | ImGuiTableFlags_RowBg
                                | ImGuiTableFlags_ScrollY
                                | ImGuiTableFlags_Resizable
                                | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("mcp_activity_log", 4, flags))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(_L("Time##mcp_act_col_time"),
                                ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn(_L("Origin##mcp_act_col_orig"),
                                ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn(_L("Cmd##mcp_act_col_cmd"),
                                ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn(_L("Description##mcp_act_col_desc"),
                                ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        int row_idx = 0;
        for (const auto &entry : snapshot)
        {
            row_idx++;
            if (!origin_allowed(entry.origin)) continue;
            if (!ci_substr(entry.description.c_str(), filter_buf))
            {
                continue;
            }

            ImGui::PushID(row_idx);
            ImGui::TableNextRow();

            /* Routing na asociovanou entitu (BP/Watch/Symbol/Bookmark) je
             * přes explicitní tlačítko v Description sloupci - jen na
             * řádcích, které cíl mají. Ostatní akce (step, memory, media,
             * eventlog, ...) cíl nemají, takže bez tlačítka. Dříve to byl
             * skrytý dvojklik na celý řádek (neobjevitelný). */
            const bool has_target =
                (entry.entity_kind != DBGAPI_ENTITY_KIND_NONE);

            ImGui::TableNextColumn();
            render_timestamp_cell(entry.timestamp_us);

            ImGui::TableNextColumn();
            /* V1.C.3 - barevný badge přes sdílený helper. */
            owner_badge_render(entry.origin);

            ImGui::TableNextColumn();
            /* cmd ukládáme jako enum; description už obsahuje string
             * dbgapi_cmd_to_str() z emit-site (viz dbgapi.c). Pro Cmd
             * sloupec zobrazíme description (= zatím to samé), v
             * budoucnu zde půjde rozparsovaný název příkazu odděleně. */
            ImGui::Text("%s", entry.description.c_str());

            ImGui::TableNextColumn();
            /* Description V1.C.2 = pouze cmd_name. Klikatelné řádky (s
             * entitou) mají vlevo tlačítko se šipkou, které otevře
             * příslušné okno (BP/Watch/Symbol/Bookmark). ArrowButton
             * kreslí ImGui, takže nezávisí na fontu. PushID(row_idx)
             * zajistí unique ID pro stejné popisy ve více řádcích. */
            if (has_target)
            {
                if (ImGui::ArrowButton("##mcp_act_goto", ImGuiDir_Right))
                {
                    switch (entry.entity_kind)
                    {
/* Routing do cílových debugger oken (BP/Watch/Symbol/Bookmark) existuje
 * jen v buildu s debuggerem. Bez něj (MZ800EMU_NO_DEBUGGER) jsou definující
 * překladové jednotky vyřazeny - bpt_window_focus_id by selhal při linkování,
 * watch/sym/bm už při kompilaci (jejich hlavičky deklarace guardují). Bez
 * debuggeru nemá kam routovat, takže je to korektní no-op. */
#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
                        case DBGAPI_ENTITY_KIND_BP:
                            bpt_window_focus_id((int)entry.entity_id);
                            break;
                        case DBGAPI_ENTITY_KIND_WATCH:
                            watch_window_focus_id((int)entry.entity_id);
                            break;
                        case DBGAPI_ENTITY_KIND_SYMBOL:
                            /* entity_id drží 16-bit CPU adresu cast na int32. */
                            sym_window_focus_addr(
                                (uint16_t)(entry.entity_id & 0xFFFF));
                            break;
                        case DBGAPI_ENTITY_KIND_BOOKMARK:
                            bm_window_focus_id((uint32_t)entry.entity_id);
                            break;
#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
                        case DBGAPI_ENTITY_KIND_NONE:
                        default:
                            /* No-op - tento řádek nemá routing target. */
                            break;
                    }
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", _("Open the related window"));
                }
                ImGui::SameLine();
            }
            ImGui::Text("%s", entry.description.c_str());
            if (entry.rejected)
            {
                /* observe-cas (M10): podmíněný zápis odmítnut, protože se
                 * stav od čtení agenta změnil; nic se nezapsalo. */
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.0f, 1.0f), "%s",
                                   _("rejected: state changed since it was read"));
            }

            /* Suppress unused warning pro cmd enum field. */
            (void)entry.cmd;
            ImGui::PopID();
        }

        /* Auto-scroll na konec pokud uživatel není manuálně odscroll-ovaný. */
        if (auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
        {
            ImGui::SetScrollHereY(1.0f);
        }

        ImGui::EndTable();
    }

    ImGui::End();
}
