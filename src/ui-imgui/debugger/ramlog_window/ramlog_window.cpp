/*
 * ramlog_window.cpp - okno RAM Write Log (záznam zápisů do paměti).
 *
 * Tři části:
 *   - stav a ovládání záznamu (rozsah CPU adres, režim ringu, kapacita,
 *     Start / Stop / Clear) - stejné operace jako MCP ramlog_start/stop/clear,
 *   - dotaz "kdo zapsal" (CPU adresa z pole nebo bajt z Memory Browseru):
 *     poslední zapisovatel z mapy a historie zápisů buňky,
 *   - tabulka nejnovějších zápisů.
 *
 * Vše jde přes dbg_ui_ramlog (DBGAPI_CMD_RAMLOG v emu vlákně); okno drží
 * jen kopie výsledků. Stav a tabulka se obnovují nejvýš dvakrát za sekundu
 * (s_next_refresh), dotaz "kdo zapsal" jen na vyžádání.
 *
 * Stabilní ID okna: "RAM Write Log###ramlog_window".
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
#include "mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "libs/imgui/imgui.h"
#include "i18n.h"

#include "ramlog_window.h"
#include "ui-imgui/debugger/dbgapi_helpers.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "emulator/debugger/dbgapi_regions.h"
#include "emulator/debugger/trace/ramlog.h"


/** Počet řádků tabulky nejnovějších zápisů. */
#define RLW_RECENT_ROWS 200
/** Nejvýš záznamů historie na buňku v dotazu "kdo zapsal". */
#define RLW_HISTORY_ROWS 100
/** Interval obnovy stavu a tabulky (s). */
#define RLW_REFRESH_S 0.5
/** Nejvýš buněk jednoho dotazu (velikost st_DBGAPI_RAMLOG_PARAM::out_cells). */
#define RLW_CELLS 4
static_assert(sizeof(((st_DBGAPI_RAMLOG_PARAM *) 0)->out_cells) == RLW_CELLS * sizeof(st_DBGAPI_RAMLOG_CELL),
              "RLW_CELLS must match st_DBGAPI_RAMLOG_PARAM::out_cells");


/** @brief Druh vyžádaného dotazu "kdo zapsal". */
typedef enum {
    RLW_Q_NONE = 0,  /**< Žádný. */
    RLW_Q_CPU,       /**< CPU adresa (s_q_cpu). */
    RLW_Q_REGION     /**< Bajt regionu Memory Browseru (s_q_kind / s_q_sub / s_q_off). */
} rlw_query_t;


/* Stav záznamu z poslední obnovy (platí jen při s_have_status). */
static st_RAMLOG_STATUS s_status;
static bool s_have_status = false;
/* Text poslední chyby operace (prázdný = žádná). */
static char s_error[160] = "";

/* Formulář startu. Prázdné meze = celá paměť. */
static char s_from_txt[8] = "";
static char s_to_txt[8] = "";
static int s_mode = RAMLOG_MODE_RING;
static int s_capacity = (int) RAMLOG_DEFAULT_CAPACITY;

/* Dotaz "kdo zapsal": pending = provést při příštím vykreslení. */
static rlw_query_t s_q_kind_sel = RLW_Q_NONE;
static bool s_q_pending = false;
static uint16_t s_q_cpu = 0;
static int s_q_kind = 0;
static int s_q_sub = 0;
static uint32_t s_q_off = 0;
static char s_q_addr_txt[8] = "";
static bool s_q_all = false;
/* Výsledek dotazu: buňky + historie (RLW_HISTORY_ROWS na buňku). */
static bool s_q_have = false;
static bool s_q_mmio = false;
static unsigned s_q_n_cells = 0;
static st_DBGAPI_RAMLOG_CELL s_q_cells[RLW_CELLS];
static std::vector<st_RAMLOG_REC> s_q_recs;
static std::vector<uint64_t> s_q_seqs;

/* Tabulka nejnovějších zápisů. */
static std::vector<st_RAMLOG_REC> s_recent;
static std::vector<uint64_t> s_recent_seqs;
static bool s_recent_all = false;
static bool s_auto_refresh = true;
static double s_next_refresh = 0.0;

static bool s_focus_pending = false;


/** @brief Zobrazovaný název prostoru (překládaný). */
static const char *rlw_space_label(unsigned space) {
    switch (space) {
        case RAMLOG_SPACE_RAM:    return _("RAM");
        case RAMLOG_SPACE_VRAM:   return _("VRAM");
        case RAMLOG_SPACE_PCG:    return _("PCG");
        case RAMLOG_SPACE_MEMEXT: return _("MemExt");
        case RAMLOG_SPACE_ROM:    return _("ROM");
        default:                  return "?";
    }
}


/** @brief Zobrazovaný název zdroje zápisu (překládaný). */
static const char *rlw_src_label(unsigned src) {
    switch (src) {
        case RAMLOG_SRC_INSN:     return _("Instruction");
        case RAMLOG_SRC_INT_ACK:  return _("Interrupt (return address)");
        case RAMLOG_SRC_NMI_ACK:  return _("NMI (return address)");
        case RAMLOG_SRC_DEBUGGER: return _("Debugger");
        case RAMLOG_SRC_POKE:     return _("Breakpoint poke");
        case RAMLOG_SRC_FREEZE:   return _("Freeze");
        case RAMLOG_SRC_CMT_HACK: return _("Tape (CMT hack)");
        case RAMLOG_SRC_LOAD:     return _("Load");
        default:                  return "?";
    }
}


/**
 * @brief Text buňky "prostor offset (plán)".
 *
 * @param buf Výstup.
 * @param n Velikost výstupu.
 * @param space en_RAMLOG_SPACE.
 * @param offset Offset v prostoru.
 * @param has_plane Buňka je plán grafické VRAM MZ-800.
 * @param plane Plán 0-3.
 */
static void rlw_cell_text(char *buf, size_t n, unsigned space, uint32_t offset,
                          bool has_plane, unsigned plane) {
    if (has_plane) {
        std::snprintf(buf, n, "%s %05X (%s %u)", rlw_space_label(space), (unsigned) offset,
                      _("plane"), plane + 1);
    } else {
        std::snprintf(buf, n, "%s %05X", rlw_space_label(space), (unsigned) offset);
    }
}


/**
 * @brief Načte hex adresu z textu (prázdný text = žádná).
 *
 * @param txt Text (hex, volitelně s prefixem 0x nebo příponou h).
 * @param out Výstup 0-FFFFh.
 * @return 1 = adresa, 0 = prázdné, -1 = neplatné.
 */
static int rlw_parse_addr(const char *txt, uint16_t *out) {
    while (*txt == ' ') txt++;
    if (!*txt) return 0;
    char *end = NULL;
    unsigned long v = strtoul(txt, &end, 16);
    if (end && (*end == 'h' || *end == 'H')) end++;
    while (end && *end == ' ') end++;
    if (!end || *end || v > 0xFFFF) return -1;
    *out = (uint16_t) v;
    return 1;
}


/** @brief Zapamatuje text chyby operace (prázdný out_error = timeout). */
static void rlw_set_error(const st_DBGAPI_RAMLOG_PARAM *p) {
    if (p->out_error[0]) {
        /* Text z jádra je anglický (jádro je bez lokalizace). */
        std::snprintf(s_error, sizeof(s_error), "%s %s", _("Command failed:"), p->out_error);
    } else {
        std::snprintf(s_error, sizeof(s_error), "%s", _("The emulator did not respond in time."));
    }
}


/** @brief Obnoví stav a tabulku nejnovějších zápisů (operace RECENT). */
static void rlw_refresh(void) {
    st_DBGAPI_RAMLOG_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_RAMLOG_OP_RECENT;
    p.limit = RLW_RECENT_ROWS;
    p.include_unchanged = s_recent_all;
    std::vector<st_RAMLOG_REC> recs(RLW_RECENT_ROWS);
    std::vector<uint64_t> seqs(RLW_RECENT_ROWS);
    p.out_recs = recs.data();
    p.out_seqs = seqs.data();
    if (!dbg_ui_ramlog(&p)) {
        rlw_set_error(&p);
        return;
    }
    s_status = p.out_status;
    s_have_status = true;
    recs.resize(p.out_n_recs);
    seqs.resize(p.out_n_recs);
    s_recent.swap(recs);
    s_recent_seqs.swap(seqs);
}


/** @brief Provede vyžádaný dotaz "kdo zapsal". */
static void rlw_run_query(void) {
    st_DBGAPI_RAMLOG_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = DBGAPI_RAMLOG_OP_WHO_WROTE;
    p.limit = RLW_HISTORY_ROWS;
    p.include_unchanged = s_q_all;
    if (s_q_kind_sel == RLW_Q_CPU) {
        p.by_cpu = true;
        p.cpu_from = s_q_cpu;
    } else {
        p.by_region = true;
        p.region_kind = s_q_kind;
        p.region_sub = s_q_sub;
        p.region_off = s_q_off;
    }
    std::vector<st_RAMLOG_REC> recs((size_t) RLW_CELLS * RLW_HISTORY_ROWS);
    std::vector<uint64_t> seqs(recs.size());
    p.out_recs = recs.data();
    p.out_seqs = seqs.data();
    s_q_have = false;
    if (!dbg_ui_ramlog(&p)) {
        rlw_set_error(&p);
        return;
    }
    s_error[0] = '\0';
    s_status = p.out_status;
    s_have_status = true;
    s_q_mmio = p.out_mmio;
    s_q_n_cells = (p.out_n_cells < RLW_CELLS) ? p.out_n_cells : RLW_CELLS;
    memcpy(s_q_cells, p.out_cells, sizeof(s_q_cells));
    s_q_recs.swap(recs);
    s_q_seqs.swap(seqs);
    s_q_have = true;
}


/** @brief Provede operaci bez výstupních polí (START / STOP / CLEAR). */
static void rlw_simple_op(en_DBGAPI_RAMLOG_OP op, const st_RAMLOG_CONFIG *cfg) {
    st_DBGAPI_RAMLOG_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = op;
    if (cfg) p.cfg = *cfg;
    if (!dbg_ui_ramlog(&p)) {
        rlw_set_error(&p);
        return;
    }
    s_error[0] = '\0';
    s_status = p.out_status;
    s_have_status = true;
    s_q_have = false;
    s_next_refresh = 0.0;
}


/** @brief Spustí záznam podle formuláře; neplatný formulář = chyba v s_error. */
static void rlw_start(void) {
    st_RAMLOG_CONFIG cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = (en_RAMLOG_MODE) s_mode;
    int cap = s_capacity;
    if (cap < (int) RAMLOG_MIN_CAPACITY) cap = (int) RAMLOG_MIN_CAPACITY;
    if (cap > (int) RAMLOG_MAX_CAPACITY) cap = (int) RAMLOG_MAX_CAPACITY;
    s_capacity = cap;
    cfg.capacity = (uint32_t) cap;
    uint16_t a = 0, b = 0;
    int ra = rlw_parse_addr(s_from_txt, &a);
    int rb = rlw_parse_addr(s_to_txt, &b);
    if (ra < 0 || rb < 0 || (ra == 0 && rb == 1)) {
        std::snprintf(s_error, sizeof(s_error), "%s",
                      _("Enter the range as hex CPU addresses (From, optionally To), or leave both empty for all memory."));
        return;
    }
    if (ra == 1) {
        if (rb == 0) b = a;
        cfg.n_ranges = 1;
        cfg.ranges[0].phys = false;
        cfg.ranges[0].from = (a < b) ? a : b;
        cfg.ranges[0].to = (a < b) ? b : a;
    }
    rlw_simple_op(DBGAPI_RAMLOG_OP_START, &cfg);
}


/** @brief Tabulka záznamů (historie buňky nebo nejnovější zápisy). */
static void rlw_records_table(const char *id, const st_RAMLOG_REC *recs, const uint64_t *seqs,
                              size_t n, float height) {
    ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                       | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable
                       | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable(id, 9, tf, ImVec2(0.0f, height))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(_("Seq"));
    ImGui::TableSetupColumn(_("Frame"));
    ImGui::TableSetupColumn(_("PC"));
    ImGui::TableSetupColumn(_("Address"));
    ImGui::TableSetupColumn(_("Cell"));
    ImGui::TableSetupColumn(_("Old"));
    ImGui::TableSetupColumn(_("New"));
    ImGui::TableSetupColumn(_("Source"));
    ImGui::TableSetupColumn(_("Note"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGuiListClipper clip;
    clip.Begin((int) n);
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const st_RAMLOG_REC *r = &recs[i];
            char cell[48];
            rlw_cell_text(cell, sizeof(cell), r->space, r->offset,
                          (r->flags & RAMLOG_FLAG_PLANE) != 0, r->plane);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long) seqs[i]);
            ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned) r->frame);
            ImGui::TableNextColumn();
            if (r->src == RAMLOG_SRC_INSN || r->src == RAMLOG_SRC_INT_ACK
                || r->src == RAMLOG_SRC_NMI_ACK || r->src == RAMLOG_SRC_POKE) {
                ImGui::Text("%04X", (unsigned) r->insn_pc);
            } else {
                ImGui::TextDisabled("-");
            }
            ImGui::TableNextColumn();
            if (r->flags & RAMLOG_FLAG_NO_CPU) ImGui::TextDisabled("-");
            else ImGui::Text("%04X", (unsigned) r->cpu_addr);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(cell);
            ImGui::TableNextColumn(); ImGui::Text("%02X", (unsigned) r->old_value);
            ImGui::TableNextColumn(); ImGui::Text("%02X", (unsigned) r->new_value);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(rlw_src_label(r->src));
            ImGui::TableNextColumn();
            if (r->flags & RAMLOG_FLAG_SUMMARY) {
                ImGui::Text(_("%u bytes loaded"), (unsigned) r->count);
            } else if (r->flags & RAMLOG_FLAG_SUPPRESSED) {
                ImGui::TextDisabled("%s", _("dropped (ROM)"));
            } else if (r->flags & RAMLOG_FLAG_UNCHANGED) {
                ImGui::TextDisabled("%s", _("value unchanged"));
            }
        }
    }
    ImGui::EndTable();
}


/** @brief Stav a ovládání záznamu. */
static void rlw_controls(void) {
    if (s_have_status && s_status.active) {
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "%s", _("Recording"));
    } else if (s_have_status && s_status.allocated) {
        ImGui::TextDisabled("%s", _("Stopped"));
    } else {
        ImGui::TextDisabled("%s", _("Not started"));
    }
    if (s_have_status && s_status.allocated) {
        ImGui::SameLine();
        ImGui::Text(_("Records: %llu of %llu writes, overwritten %llu, dropped %llu, memory %.1f MB"),
                    (unsigned long long) (s_status.next_seq - s_status.first_seq),
                    (unsigned long long) s_status.total,
                    (unsigned long long) s_status.overwritten,
                    (unsigned long long) s_status.dropped,
                    (double) s_status.memory_bytes / (1024.0 * 1024.0));
        if (s_status.n_ranges == 0) {
            ImGui::TextDisabled("%s", _("Range: all memory"));
        } else {
            const st_RAMLOG_RANGE *r = &s_status.ranges[0];
            if (r->phys) {
                ImGui::TextDisabled(_("Range: %s %05X-%05X"), rlw_space_label(r->space),
                                    (unsigned) r->from, (unsigned) r->to);
            } else {
                ImGui::TextDisabled(_("Range: CPU %04X-%04X"), (unsigned) r->from, (unsigned) r->to);
            }
            if (s_status.n_ranges > 1) {
                ImGui::SameLine();
                ImGui::TextDisabled(_("(+%u more)"), s_status.n_ranges - 1);
            }
        }
    }

    ImGui::SetNextItemWidth(ImGui::CalcTextSize("FFFFFF").x + ImGui::GetStyle().FramePadding.x * 2.0f);
    ImGui::InputText(_L("From"), s_from_txt, sizeof(s_from_txt), ImGuiInputTextFlags_CharsHexadecimal);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", _("First CPU address (hex). Leave From and To empty to record all memory."));
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("FFFFFF").x + ImGui::GetStyle().FramePadding.x * 2.0f);
    ImGui::InputText(_L("To"), s_to_txt, sizeof(s_to_txt), ImGuiInputTextFlags_CharsHexadecimal);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", _("Last CPU address (hex); empty = only the From address."));
    }
    ImGui::SameLine();
    const char *modes[2] = { _("Ring (keep newest)"), _("Fill (keep first)") };
    ImGui::SetNextItemWidth(ImGui::CalcTextSize(modes[0]).x + ImGui::GetFrameHeight() * 2.0f);
    ImGui::Combo(_L("Mode"), &s_mode, modes, 2);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("16777216").x + ImGui::GetStyle().FramePadding.x * 2.0f);
    ImGui::InputInt(_L("Records"), &s_capacity, 0, 0);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", _("Log size in records (16-16777216), 32 bytes each."));
    }

    if (ImGui::Button(_L("Start"))) rlw_start();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", _("Start recording; a running log is replaced. Reset and snapshot load empty the log."));
    }
    ImGui::SameLine();
    bool can_stop = s_have_status && s_status.active;
    if (!can_stop) ImGui::BeginDisabled();
    if (ImGui::Button(_L("Stop"))) rlw_simple_op(DBGAPI_RAMLOG_OP_STOP, NULL);
    if (!can_stop) ImGui::EndDisabled();
    ImGui::SameLine();
    bool can_clear = s_have_status && s_status.allocated;
    if (!can_clear) ImGui::BeginDisabled();
    if (ImGui::Button(_L("Clear"))) rlw_simple_op(DBGAPI_RAMLOG_OP_CLEAR, NULL);
    if (!can_clear) ImGui::EndDisabled();
    if (s_have_status && s_status.active && s_status.n_ranges == 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", _("(all memory: emulation is slower)"));
    }
}


/** @brief Dotaz "kdo zapsal" a jeho výsledek. */
static void rlw_who_wrote(void) {
    ImGui::SeparatorText(_("Who wrote"));
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("FFFFFF").x + ImGui::GetStyle().FramePadding.x * 2.0f);
    bool enter = ImGui::InputText(_L("CPU address"), s_q_addr_txt, sizeof(s_q_addr_txt),
                                  ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button(_L("Find")) || enter) {
        uint16_t a = 0;
        if (rlw_parse_addr(s_q_addr_txt, &a) == 1) {
            s_q_kind_sel = RLW_Q_CPU;
            s_q_cpu = a;
            s_q_pending = true;
        } else {
            std::snprintf(s_error, sizeof(s_error), "%s", _("Enter a hex CPU address."));
        }
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(_L("Include unchanged writes"), &s_q_all) && s_q_kind_sel != RLW_Q_NONE) {
        s_q_pending = true;
    }
    if (s_q_pending) {
        s_q_pending = false;
        rlw_run_query();
    }
    if (!s_q_have) {
        ImGui::TextDisabled("%s", _("Enter an address, or use \"Who wrote this byte?\" in the Memory Browser."));
        return;
    }
    if (s_q_mmio) {
        ImGui::TextDisabled("%s", _("This address is a memory-mapped I/O port, not a memory cell."));
        return;
    }
    if (s_q_n_cells == 0) {
        ImGui::TextDisabled("%s", _("No memory cell is mapped at this address."));
        return;
    }
    for (unsigned c = 0; c < s_q_n_cells; c++) {
        const st_DBGAPI_RAMLOG_CELL *cell = &s_q_cells[c];
        char ct[48];
        rlw_cell_text(ct, sizeof(ct), cell->space, cell->offset, cell->has_plane, cell->plane);
        ImGui::PushID((int) c);
        if (cell->has_last) {
            ImGui::Text(_("%s: last written by PC %04X (%s), record %u"), ct, (unsigned) cell->last.insn_pc,
                        rlw_src_label(cell->last.src), (unsigned) (cell->last.seq1 - 1));
        } else {
            ImGui::Text(_("%s: not written since the log started"), ct);
        }
        if (cell->n_recs) {
            float h = ImGui::GetTextLineHeightWithSpacing() * (float) ((cell->n_recs < 8 ? cell->n_recs : 8) + 2);
            rlw_records_table("##rlw_hist", &s_q_recs[(size_t) c * RLW_HISTORY_ROWS],
                              &s_q_seqs[(size_t) c * RLW_HISTORY_ROWS], cell->n_recs, h);
        } else if (cell->has_last) {
            ImGui::TextDisabled("%s", _("The write is no longer in the log (overwritten); only the last writer is known."));
        }
        ImGui::PopID();
    }
}


extern "C" void ramlog_window_query_cpu(uint16_t addr) {
    s_q_kind_sel = RLW_Q_CPU;
    s_q_cpu = addr;
    std::snprintf(s_q_addr_txt, sizeof(s_q_addr_txt), "%04X", (unsigned) addr);
    s_q_pending = true;
    s_focus_pending = true;
    if (g_gui) g_gui->showRamlogWindow = true;
}


extern "C" void ramlog_window_query_region(int region_kind, int sub_id, uint32_t offset) {
    if (region_kind == REGION_KIND_LOGICAL) {
        ramlog_window_query_cpu((uint16_t) offset);
        return;
    }
    s_q_kind_sel = RLW_Q_REGION;
    s_q_kind = region_kind;
    s_q_sub = sub_id;
    s_q_off = offset;
    s_q_addr_txt[0] = '\0';
    s_q_pending = true;
    s_focus_pending = true;
    if (g_gui) g_gui->showRamlogWindow = true;
}


extern "C" bool ramlog_window_region_supported(int region_kind) {
    switch (region_kind) {
        case REGION_KIND_LOGICAL:
        case REGION_KIND_RAM:
        case REGION_KIND_MEMEXT_RAM:
        case REGION_KIND_CGRAM_700:
        case REGION_KIND_VRAM_700_CHAR:
        case REGION_KIND_VRAM_700_ATTR:
        case REGION_KIND_VRAM_PHYS_PLANE:
        case REGION_KIND_PCG_1500:
            return true;
        default:
            return false;
    }
}


extern "C" void ramlog_window_render(bool *p_open) {
    if (!p_open || !*p_open) return;
    ImGui::SetNextWindowSize(ImVec2(820.0f, 560.0f), ImGuiCond_FirstUseEver);
    bool was_pending = s_focus_pending;
    if (s_focus_pending) {
        ImGui::SetNextWindowFocus();
        s_focus_pending = false;
    }
    if (!ImGui::Begin(_L("RAM Write Log###ramlog_window"), p_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    /* Multi-viewport: SetNextWindowFocus nezvedne okno platformy (vzor
     * stack_history_window.cpp). */
    if (was_pending) {
        ImGuiViewport *vp = ImGui::GetWindowViewport();
        ImGuiPlatformIO &pio = ImGui::GetPlatformIO();
        if (vp && vp != ImGui::GetMainViewport() && vp->PlatformWindowCreated && pio.Platform_SetWindowFocus) {
            pio.Platform_SetWindowFocus(vp);
        }
    }

    double now = ImGui::GetTime();
    if (now >= s_next_refresh && (s_auto_refresh || !s_have_status || s_next_refresh == 0.0)) {
        rlw_refresh();
        s_next_refresh = now + RLW_REFRESH_S;
    }

    rlw_controls();
    if (s_error[0]) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", s_error);
    }
    rlw_who_wrote();

    ImGui::SeparatorText(_("Newest writes"));
    ImGui::Checkbox(_L("Auto refresh"), &s_auto_refresh);
    ImGui::SameLine();
    if (ImGui::Button(_L("Refresh"))) s_next_refresh = 0.0;   /* obnoví se v příštím snímku */
    ImGui::SameLine();
    if (ImGui::Checkbox(_L("Show unchanged writes"), &s_recent_all)) s_next_refresh = 0.0;
    rlw_records_table("##rlw_recent", s_recent.data(), s_recent_seqs.data(), s_recent.size(), 0.0f);

    ImGui::End();
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
