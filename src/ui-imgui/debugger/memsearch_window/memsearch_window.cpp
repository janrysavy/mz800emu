/*
 * memsearch_window.cpp - okno Memory Search (postupné hledání proměnných).
 *
 * Tři části:
 *   - nastavení prostoru a typu hodnoty (předvolba prostoru, typ, šířka,
 *     pořadí bajtů, zarovnání, kódování číslic, povolení velkého prostoru)
 *     - ukládá se do [MEMSEARCH_WINDOW],
 *   - kroky hledání: New search, Sample, Filter (operace, reference,
 *     hodnota, negace, nový vzorek), Undo / Redo, náhled filtru,
 *   - tabulka kandidátů (ImGuiListClipper, jen pod MS_TABLE_MAX) s
 *     kontextovým menu: Memory Browser, Watch, write breakpoint, Freeze,
 *     Who wrote, kopie adresy, vyřazení.
 *
 * Sezení (st_MEMSEARCH_SESSION) vlastní okno a pracuje s ním jen UI
 * vlákno. Vzorek pořizuje emu vlákno přes dbg_ui_memsearch_sample
 * (DBGAPI_CMD_MEMSEARCH_SAMPLE), filtr a výpis běží tady. Převod kandidáta
 * na CPU adresu a zónu BP dělá memsearch_target() (stejně jako MCP
 * memsearch_list).
 *
 * Stabilní ID okna: "Memory Search###memsearch_window".
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
#include <inttypes.h>

#include <glib.h>

#include "libs/imgui/imgui.h"
#include "i18n.h"

#include "memsearch_window.h"
#include "ui-imgui/debugger/dbgapi_helpers.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/debugger/membrowser/membrowser_window.h"
#include "ui-imgui/debugger/ramlog_window/ramlog_window.h"
#include "emulator/debugger/dbgapi_regions.h"
#include "emulator/debugger/dbgapi_ui.h"
#include "emulator/debugger/memsearch/memsearch.h"
#include "emulator/debugger/breakpoints.h"
#include "emulator/debugger/watch.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cfgfile/cfgelement.h"


/** Počet kandidátů, od kterého se tabulka nevypisuje (vypíše se jen méně). */
#define MS_TABLE_MAX 10000
/** Interval obnovy stavu MemExt pro sloupec Z80 (s). */
#define MS_MEMEXT_REFRESH_S 0.5
/** Timeout dotazu na stav MemExt (ms). */
#define MS_MEMEXT_TIMEOUT_MS 200


/** @brief Typy hodnot v pořadí kombo boxu (index = persistovaná hodnota `type`). */
static const char *const s_type_names[] = {
    "u8", "s8", "u16", "s16", "u24", "s24", "u32", "s32", "bcd", "digits",
};
#define MS_TYPE_COUNT ((int) (sizeof(s_type_names) / sizeof(s_type_names[0])))
#define MS_TYPE_BCD    8
#define MS_TYPE_DIGITS 9


/*
 * Persistované nastavení (vázané na INI, proto unsigned i u příznaků).
 *
 * @invariant s_p_space < MEMSEARCH_PRESET_COUNT, s_p_type < MS_TYPE_COUNT,
 *            s_p_digit_enc < MEMSEARCH_DIGITS_COUNT (hlídá INI rozsah).
 */
static unsigned s_p_space = MEMSEARCH_PRESET_RAM;
static unsigned s_p_type = 0;
static unsigned s_p_width = 2;
static unsigned s_p_big_endian = 0;
static unsigned s_p_aligned = 0;
static unsigned s_p_digit_enc = MEMSEARCH_DIGITS_DISPLAY;
static unsigned s_p_leading_space = 1;
static unsigned s_p_allow_large = 0;

/* Sezení okna (vlastní okno, jen UI vlákno). */
static st_MEMSEARCH_SESSION *s_sess = NULL;

/* Formulář filtru. */
static int s_op = MEMSEARCH_OP_EQ;
static int s_cmp = MEMSEARCH_CMP_VALUE;
static int64_t s_value = 0;
static int64_t s_value2 = 0;
static uint32_t s_mask = 0xFF;
static bool s_negate = false;
static bool s_take_sample = true;
static bool s_preview = false;

/* Text poslední chyby nebo zprávy (prázdný = žádná). */
static char s_msg[200] = "";
static bool s_msg_error = false;

/* Stav MemExt pro převod kandidátů na CPU adresu (obnova po MS_MEMEXT_REFRESH_S). */
static st_MEMSEARCH_MEMEXT s_mx;
static bool s_mx_valid = false;
static double s_mx_next = 0.0;

/* Vybraný kandidát (offset v prostoru). */
static bool s_sel_valid = false;
static uint32_t s_sel = 0;


/** @brief Zapamatuje zprávu pod ovládáním (chyba = červeně). */
static void ms_message(bool error, const char *text) {
    std::snprintf(s_msg, sizeof(s_msg), "%s", text ? text : "");
    s_msg_error = error;
}


/**
 * @brief Formát hodnoty podle persistovaného nastavení.
 * @param[out] f Formát.
 */
static void ms_fmt_from_settings(st_MEMSEARCH_FMT *f) {
    memset(f, 0, sizeof(*f));
    unsigned w = 1;
    (void) memsearch_type_from_name(s_type_names[s_p_type % MS_TYPE_COUNT], &f->type, &w);
    f->width = (w != 0) ? w : s_p_width;
    f->big_endian = s_p_big_endian != 0;
    f->aligned = s_p_aligned != 0;
    f->digit_enc = (en_MEMSEARCH_DIGIT_ENC) (s_p_digit_enc % MEMSEARCH_DIGITS_COUNT);
    f->leading_space = s_p_leading_space != 0;
}


/** @brief Zavře sezení okna. */
static void ms_close(void) {
    memsearch_session_free(s_sess);
    s_sess = NULL;
    s_sel_valid = false;
}


/**
 * @brief Pořídí nový vzorek prostoru sezení přes emu vlákno.
 * @return Vzorek (uvolnit memsearch_sample_unref), NULL = timeout / chyba (zpráva nastavena).
 */
static st_MEMSEARCH_SAMPLE *ms_take_sample(void) {
    st_MEMSEARCH_SAMPLE *smp = memsearch_sample_new(memsearch_space_size(s_sess),
                                                    memsearch_span_count(s_sess));
    if (!smp) {
        ms_message(true, _("Not enough memory for the sample."));
        return NULL;
    }
    if (!dbg_ui_memsearch_sample(memsearch_span(s_sess, 0), memsearch_span_count(s_sess), smp)) {
        memsearch_sample_unref(smp);
        ms_message(true, _("The emulator did not respond in time."));
        return NULL;
    }
    return smp;
}


/** @brief New search: nové sezení nad zvoleným prostorem a první vzorek. */
static void ms_new_search(void) {
    ms_close();
    int count = 0;
    st_REGION_DESC *regs = dbgapi_regions_enumerate_dup(&count);
    st_MEMSEARCH_SPAN *spans = (st_MEMSEARCH_SPAN *) g_malloc0_n((gsize) (count > 0 ? count : 1),
                                                               sizeof(st_MEMSEARCH_SPAN));
    unsigned n = memsearch_spans_from_preset((en_MEMSEARCH_PRESET) s_p_space, regs, count, spans);
    g_free(regs);
    if (n == 0) {
        g_free(spans);
        ms_message(true, _("This memory space is not present in the current configuration."));
        return;
    }
    st_MEMSEARCH_FMT fmt;
    ms_fmt_from_settings(&fmt);
    const char *err = NULL;
    uint64_t total = 0;
    for (unsigned i = 0; i < n; i++) total += spans[i].length;
    s_sess = memsearch_session_new(spans, n, &fmt,
                                   s_p_allow_large ? MEMSEARCH_HARD_MAX_SPACE : MEMSEARCH_DEFAULT_MAX_SPACE,
                                   &err);
    g_free(spans);
    if (!s_sess) {
        char t[200];
        if (total > MEMSEARCH_DEFAULT_MAX_SPACE && !s_p_allow_large) {
            std::snprintf(t, sizeof(t), _("The space has %.1f MiB, more than 4 MiB. Enable \"Allow large space\" to search it."),
                          (double) total / (1024.0 * 1024.0));
        } else {
            /* Text z jádra je anglický (jádro je bez lokalizace). */
            std::snprintf(t, sizeof(t), "%s %s", _("Cannot start the search:"), err ? err : "");
        }
        ms_message(true, t);
        return;
    }
    st_MEMSEARCH_SAMPLE *smp = ms_take_sample();
    if (!smp) {
        ms_close();
        return;
    }
    if (!memsearch_start(s_sess, smp, NULL, &err)) {
        ms_message(true, err);
        ms_close();
    } else {
        ms_message(false, "");
    }
    memsearch_sample_unref(smp);
    s_mx_next = 0.0;
}


/** @brief Sample: nový vzorek bez filtrace. */
static void ms_sample(void) {
    st_MEMSEARCH_SAMPLE *smp = ms_take_sample();
    if (!smp) return;
    const char *err = NULL;
    if (!memsearch_sample(s_sess, smp, &err)) ms_message(true, err);
    else ms_message(false, "");
    memsearch_sample_unref(smp);
}


/**
 * @brief Filtr z formuláře.
 * @param[out] f Filtr.
 * @param[out] err Popis neplatného filtru.
 * @return true = filtr je pro sezení platný.
 */
static bool ms_filter_from_form(st_MEMSEARCH_FILTER *f, const char **err) {
    memset(f, 0, sizeof(*f));
    f->op = (en_MEMSEARCH_OP) s_op;
    f->compare_to = (en_MEMSEARCH_CMP) s_cmp;
    f->value = s_value;
    f->value2 = s_value2;
    f->mask = s_mask;
    f->negate = s_negate;
    return memsearch_filter_validate(f, memsearch_fmt(s_sess), err);
}


/** @brief Filter: zúží kandidáty (volitelně nejdřív nový vzorek). */
static void ms_filter(void) {
    st_MEMSEARCH_FILTER f;
    const char *err = NULL;
    if (!ms_filter_from_form(&f, &err)) {
        ms_message(true, err);
        return;
    }
    st_MEMSEARCH_SAMPLE *smp = NULL;
    if (s_take_sample) {
        smp = ms_take_sample();
        if (!smp) return;
    }
    uint64_t removed = 0;
    if (!memsearch_filter(s_sess, smp, &f, &removed, &err)) {
        ms_message(true, err);
    } else {
        char t[96];
        std::snprintf(t, sizeof(t), _("Removed %llu candidates."), (unsigned long long) removed);
        ms_message(false, t);
    }
    memsearch_sample_unref(smp);
}


/** @brief Obnoví stav MemExt (mapování oken), když prostor obsahuje MemExt. */
static void ms_refresh_memext(void) {
    double now = ImGui::GetTime();
    if (now < s_mx_next) return;
    s_mx_next = now + MS_MEMEXT_REFRESH_S;
    bool has = false;
    for (unsigned i = 0; s_sess && i < memsearch_span_count(s_sess); i++) {
        if (memsearch_span(s_sess, i)->region_kind == REGION_KIND_MEMEXT_RAM) has = true;
    }
    if (!has) {
        s_mx_valid = false;
        return;
    }
    st_DBGAPI_MEMEXT_INFO_PARAM mi;
    memset(&mi, 0, sizeof(mi));
    if (!dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue, DBGAPI_CMD_GET_MEMEXT_INFO, &mi, NULL,
                                   MS_MEMEXT_TIMEOUT_MS)) {
        return;
    }
    memset(&s_mx, 0, sizeof(s_mx));
    s_mx.connected = mi.connected != 0;
    s_mx.pehu = strcmp(mi.type, "pehu") == 0;
    s_mx.map_valid = mi.map_available != 0;
    memcpy(s_mx.map, mi.current_map, sizeof(s_mx.map));
    s_mx_valid = true;
}


/**
 * @brief Krátký zobrazovaný název regionu kandidáta.
 * @param buf Výstup.
 * @param n Velikost výstupu.
 * @param kind en_REGION_KIND.
 * @param sub sub_id.
 */
static void ms_region_label(char *buf, size_t n, int kind, int sub) {
    switch ((en_REGION_KIND) kind) {
        case REGION_KIND_RAM:             std::snprintf(buf, n, "%s", _("User RAM")); break;
        case REGION_KIND_LOGICAL:         std::snprintf(buf, n, "%s", _("Logical Z80")); break;
        case REGION_KIND_VRAM_700_CHAR:   std::snprintf(buf, n, "%s", _("VRAM 700 char")); break;
        case REGION_KIND_VRAM_700_ATTR:   std::snprintf(buf, n, "%s", _("VRAM 700 attr")); break;
        case REGION_KIND_VRAM_PHYS_PLANE: std::snprintf(buf, n, _("VRAM plane %d"), sub + 1); break;
        case REGION_KIND_CGRAM_700:       std::snprintf(buf, n, "%s", _("CG-RAM")); break;
        case REGION_KIND_PCG_1500:        std::snprintf(buf, n, _("PCG %d"), sub + 1); break;
        case REGION_KIND_MEMEXT_RAM:      std::snprintf(buf, n, _("MemExt %02X"), (unsigned) sub); break;
        case REGION_KIND_RAMDISK_STD:     std::snprintf(buf, n, _("RAM disk %02X"), (unsigned) sub); break;
        case REGION_KIND_RAMDISK_PEZIK:   std::snprintf(buf, n, _("PEZIK %d/%d"), sub / 8, sub % 8); break;
        default:                          std::snprintf(buf, n, "%d/%d", kind, sub); break;
    }
}


/** @brief Hodnota do buňky tabulky (neplatná = pomlčka). */
static void ms_value_cell(const st_MEMSEARCH_VALUE *v) {
    if (v->valid) ImGui::Text("%" PRId64, v->value);
    else ImGui::TextDisabled("-");
}


/** @brief Typ Watch podle formátu hodnoty (BCD, číslice a 24 bitů jako bajty). */
static en_WATCH_TYPE ms_watch_type(const st_MEMSEARCH_FMT *f) {
    bool be = f->big_endian;
    if (f->type == MEMSEARCH_TYPE_UINT || f->type == MEMSEARCH_TYPE_SINT) {
        bool s = (f->type == MEMSEARCH_TYPE_SINT);
        switch (f->width) {
            case 1: return s ? WATCH_TYPE_I8 : WATCH_TYPE_U8;
            case 2: return s ? (be ? WATCH_TYPE_I16BE : WATCH_TYPE_I16LE) : (be ? WATCH_TYPE_U16BE : WATCH_TYPE_U16LE);
            case 4: return s ? (be ? WATCH_TYPE_I32BE : WATCH_TYPE_I32LE) : (be ? WATCH_TYPE_U32BE : WATCH_TYPE_U32LE);
            default: break;
        }
    }
    return WATCH_TYPE_BYTES;
}


/** @brief Zóna BP podle zóny memsearch. */
static en_BP_ZONE ms_bp_zone(en_MEMSEARCH_BPZONE z) {
    switch (z) {
        case MEMSEARCH_BPZONE_RAM:        return BP_ZONE_RAM;
        case MEMSEARCH_BPZONE_VRAM_FB:    return BP_ZONE_VRAM_FB;
        case MEMSEARCH_BPZONE_MMEXT_BANK: return BP_ZONE_MMEXT_BANK;
        default:                          return BP_ZONE_CPU_VIEW;
    }
}


/**
 * @brief Write breakpoint (MEM_W) na kandidáta přes CMD_BP_CREATE_WITH_INIT.
 * @param t Převod kandidáta (bp_zone != NONE).
 * @param width Šířka hodnoty.
 */
static void ms_add_write_bp(const st_MEMSEARCH_TARGET *t, unsigned width) {
    st_DBGAPI_BP_UPDATE_PARAM p;
    memset(&p, 0, sizeof(p));
    p.id = -1;
    p.update_mask = DBGAPI_BP_UM_ADDR | DBGAPI_BP_UM_TYPE | DBGAPI_BP_UM_ZONE;
    p.addr = t->bp_addr;
    p.type = (uint8_t) BPT_TYPE_MEM_W;
    p.zone = (uint8_t) ms_bp_zone(t->bp_zone);
    if (t->bp_zone == MEMSEARCH_BPZONE_MMEXT_BANK) {
        p.update_mask |= DBGAPI_BP_UM_BANK_ID | DBGAPI_BP_UM_ADDR_SPACE;
        p.bank_id = (uint8_t) t->bp_bank;
        p.bp_addr_space = (uint8_t) BP_ADDR_SPACE_BANK_OFFSET;
    }
    if (width > 1) {
        p.update_mask |= DBGAPI_BP_UM_ADDR_END | DBGAPI_BP_UM_ADDR_MATCH_MODE;
        p.addr_end = t->bp_addr_end;
        p.addr_match_mode = (uint8_t) BP_MATCH_RANGE;
    }
    int id = -1;
    if (dbg_ui_bp_create_with_init(&p, &id)) {
        char m[96];
        std::snprintf(m, sizeof(m), _("Write breakpoint %d created."), id);
        ms_message(false, m);
    } else {
        ms_message(true, _("The breakpoint could not be created."));
    }
}


/** @brief Kontextové menu kandidáta. */
static void ms_row_menu(const st_MEMSEARCH_ITEM *it) {
    const st_MEMSEARCH_FMT *f = memsearch_fmt(s_sess);
    st_MEMSEARCH_TARGET t;
    (void) memsearch_target(it->region_kind, it->sub_id, it->region_offset, f->width,
                            s_mx_valid ? &s_mx : NULL, &t);

    if (ImGui::MenuItem(_("Show in Memory Browser"))) {
        membrowser_window_show_at(it->region_kind, it->sub_id, it->region_offset);
    }
    if (ImGui::MenuItem(_("Add to Watch"), NULL, false, t.has_z80)) {
        char name[24];
        std::snprintf(name, sizeof(name), "ms_%04X", (unsigned) t.z80_addr);
        int idx = watch_add(name, t.z80_addr, -1);
        if (idx >= 0) {
            en_WATCH_TYPE wt = ms_watch_type(f);
            watch_set_type(idx, wt);
            if (watch_type_has_length(wt)) watch_set_length(idx, (uint16_t) f->width);
            if (g_gui) g_gui->showWatchWindow = true;
        }
    }
    if (!t.has_z80 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", _("Watch needs a CPU address; this cell is not mapped to the CPU now."));
    }
    if (ImGui::MenuItem(_("Add write breakpoint"), NULL, false, t.bp_zone != MEMSEARCH_BPZONE_NONE)) {
        ms_add_write_bp(&t, f->width);
    }
    if (t.bp_zone == MEMSEARCH_BPZONE_NONE && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", _("A write breakpoint cannot match this cell (VRAM plane, CG-RAM, PCG, RAM disk or an unmapped MemExt bank)."));
    }
    if (ImGui::MenuItem(_("Freeze at current value"), NULL, false, it->connected)) {
        bool ok = true;
        for (unsigned b = 0; b < f->width && ok; b++) {
            ok = dbg_ui_freeze_add(it->region_kind, it->sub_id, it->region_offset + b, it->cur.raw[b]);
        }
        if (!ok) ms_message(true, _("Freeze failed (the table holds at most 256 bytes)."));
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", _("The value is written back once per frame: the program sees its own change until the end of the frame."));
    }
    if (ramlog_window_region_supported(it->region_kind) && ImGui::MenuItem(_("Who wrote this byte?"))) {
        ramlog_window_query_region(it->region_kind, it->sub_id, it->region_offset);
    }
    if (ImGui::MenuItem(_("Copy address"))) {
        char a[48];
        if (t.has_z80) std::snprintf(a, sizeof(a), "%04X", (unsigned) t.z80_addr);
        else std::snprintf(a, sizeof(a), "%05X", (unsigned) it->region_offset);
        ImGui::SetClipboardText(a);
    }
    ImGui::Separator();
    if (ImGui::MenuItem(_("Eliminate"))) {
        uint32_t o = it->offset;
        (void) memsearch_eliminate(s_sess, &o, 1);
    }
}


/** @brief Tabulka kandidátů. */
static void ms_table(void) {
    uint64_t total = memsearch_candidate_count(s_sess);
    if (total == 0) {
        ImGui::TextDisabled("%s", _("No candidates left. Undo the last step or start a new search."));
        return;
    }
    if (total >= MS_TABLE_MAX) {
        ImGui::TextDisabled(_("%llu candidates - the list is shown below %d. Change the value in the program and filter again."),
                            (unsigned long long) total, MS_TABLE_MAX);
        return;
    }
    ms_refresh_memext();
    st_MEMSEARCH_FILTER pf;
    const char *perr = NULL;
    bool preview = s_preview && ms_filter_from_form(&pf, &perr);

    ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                       | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable
                       | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##ms_table", 7, tf, ImVec2(0.0f, 0.0f))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(_("Region"));
    ImGui::TableSetupColumn(_("Offset"));
    ImGui::TableSetupColumn(_("Z80"));
    ImGui::TableSetupColumn(_("Bytes"));
    ImGui::TableSetupColumn(_("Current"));
    ImGui::TableSetupColumn(_("Previous"));
    ImGui::TableSetupColumn(_("First"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();

    unsigned width = memsearch_fmt(s_sess)->width;
    ImGuiListClipper clip;
    clip.Begin((int) total);
    while (clip.Step()) {
        for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
            uint32_t off = 0;
            st_MEMSEARCH_ITEM it;
            if (!memsearch_nth_candidate(s_sess, (uint64_t) row, &off)
                || !memsearch_item(s_sess, off, &it)) {
                break;
            }
            st_MEMSEARCH_TARGET t;
            (void) memsearch_target(it.region_kind, it.sub_id, it.region_offset, width,
                                    s_mx_valid ? &s_mx : NULL, &t);
            ImGui::TableNextRow();
            if (preview && memsearch_eval(s_sess, off, &pf) == 0) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(140, 40, 40, 110));
            }
            ImGui::PushID(row);
            ImGui::TableNextColumn();
            char reg[40];
            ms_region_label(reg, sizeof(reg), it.region_kind, it.sub_id);
            bool sel = s_sel_valid && s_sel == off;
            if (ImGui::Selectable(reg, sel, ImGuiSelectableFlags_SpanAllColumns
                                            | ImGuiSelectableFlags_AllowOverlap)) {
                s_sel_valid = true;
                s_sel = off;
            }
            if (ImGui::BeginPopupContextItem("##ms_row_ctx")) {
                s_sel_valid = true;
                s_sel = off;
                ms_row_menu(&it);
                ImGui::EndPopup();
            }
            if (!it.connected && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", _("The region was disconnected at the last sample; its candidates are kept unchanged."));
            }
            ImGui::TableNextColumn(); ImGui::Text("%05X", (unsigned) it.region_offset);
            ImGui::TableNextColumn();
            if (t.has_z80) ImGui::Text("%04X", (unsigned) t.z80_addr);
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            char hex[2 * MEMSEARCH_MAX_WIDTH + 1];
            for (unsigned b = 0; b < width; b++) std::snprintf(&hex[2 * b], 3, "%02X", it.cur.raw[b]);
            hex[2 * width] = '\0';
            if (it.connected) ImGui::TextUnformatted(hex);
            else ImGui::TextDisabled("%s", _("disconnected"));
            ImGui::TableNextColumn(); ms_value_cell(&it.cur);
            ImGui::TableNextColumn(); ms_value_cell(&it.prev);
            ImGui::TableNextColumn(); ms_value_cell(&it.first);
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}


/** @brief Šířka kombo boxu pro nejdelší položku. */
static float ms_combo_width(const char *const *items, int n) {
    float w = 0.0f;
    for (int i = 0; i < n; i++) {
        float x = ImGui::CalcTextSize(items[i]).x;
        if (x > w) w = x;
    }
    return w + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 2.0f;
}


/** @brief Nastavení prostoru a typu hodnoty. */
static void ms_settings(void) {
    const char *spaces[MEMSEARCH_PRESET_COUNT] = {
        _("User RAM"), _("Logical Z80"), _("VRAM"), _("MemExt banks"), _("RAM disks"), _("All writable"),
    };
    int sp = (int) (s_p_space % MEMSEARCH_PRESET_COUNT);
    ImGui::SetNextItemWidth(ms_combo_width(spaces, MEMSEARCH_PRESET_COUNT));
    if (ImGui::Combo(_L("Space"), &sp, spaces, MEMSEARCH_PRESET_COUNT)) s_p_space = (unsigned) sp;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", _("User RAM: 64 KB as the CPU reads RAM (with a MemExt the banks mapped at sample time - "
                                 "search the MemExt banks if the program switches them). Logical Z80: what the CPU "
                                 "sees now, including ROM, VRAM and the 0E000h ports (values from the last update)."));
    }
    ImGui::SameLine();
    int ty = (int) (s_p_type % MS_TYPE_COUNT);
    ImGui::SetNextItemWidth(ms_combo_width(s_type_names, MS_TYPE_COUNT));
    if (ImGui::Combo(_L("Type"), &ty, s_type_names, MS_TYPE_COUNT)) {
        s_p_type = (unsigned) ty;
        if (ty == MS_TYPE_DIGITS) {
            s_p_big_endian = 1;
            if (s_p_width < 1 || s_p_width > 8) s_p_width = 4;
        } else if (ty == MS_TYPE_BCD) {
            if (s_p_width < 1 || s_p_width > 4) s_p_width = 2;
        }
    }
    if (ty == MS_TYPE_BCD || ty == MS_TYPE_DIGITS) {
        ImGui::SameLine();
        int w = (int) s_p_width;
        ImGui::SetNextItemWidth(ImGui::GetFrameHeight() * 3.0f);
        if (ImGui::InputInt(_L("Bytes"), &w, 1, 1)) {
            int maxw = (ty == MS_TYPE_BCD) ? 4 : 8;
            if (w < 1) w = 1;
            if (w > maxw) w = maxw;
            s_p_width = (unsigned) w;
        }
    }
    ImGui::SameLine();
    const char *endian[2] = { _("LE (Z80)"), _("BE") };
    int be = s_p_big_endian ? 1 : 0;
    ImGui::SetNextItemWidth(ms_combo_width(endian, 2));
    if (ImGui::Combo(_L("Byte order"), &be, endian, 2)) s_p_big_endian = (unsigned) be;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", _("Digits: BE = the highest digit first (print order)."));
    }
    ImGui::SameLine();
    bool al = s_p_aligned != 0;
    if (ImGui::Checkbox(_L("Aligned"), &al)) s_p_aligned = al ? 1 : 0;

    if (ty == MS_TYPE_DIGITS) {
        const char *enc[MEMSEARCH_DIGITS_COUNT] = { _("Raw 00h-09h"), _("ASCII 30h-39h"), _("Display code 20h-29h") };
        int e = (int) (s_p_digit_enc % MEMSEARCH_DIGITS_COUNT);
        ImGui::SetNextItemWidth(ms_combo_width(enc, MEMSEARCH_DIGITS_COUNT));
        if (ImGui::Combo(_L("Digits"), &e, enc, MEMSEARCH_DIGITS_COUNT)) s_p_digit_enc = (unsigned) e;
        ImGui::SameLine();
        bool ls = s_p_leading_space != 0;
        if (ImGui::Checkbox(_L("Leading spaces as zeros"), &ls)) s_p_leading_space = ls ? 1 : 0;
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", _("Space is 20h in ASCII and 00h in the display code."));
        }
        ImGui::SameLine();
    }
    bool lg = s_p_allow_large != 0;
    if (ImGui::Checkbox(_L("Allow large space"), &lg)) s_p_allow_large = lg ? 1 : 0;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", _("Allow a space over 4 MiB (up to 32 MiB, e.g. a 16 MB RAM disk). The search keeps "
                                 "three samples plus the undo history in memory."));
    }
}


/** @brief Kroky hledání a stav. */
static void ms_steps(void) {
    if (ImGui::Button(_L("New search"))) ms_new_search();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", _("Take a sample of the space; every position is a candidate. While the emulation "
                                 "runs, the sample is taken at the next frame."));
    }
    ImGui::SameLine();
    bool started = s_sess && memsearch_started(s_sess);
    if (!started) ImGui::BeginDisabled();
    if (ImGui::Button(_L("Sample"))) ms_sample();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", _("New sample without filtering; filter then with \"Take new sample\" off."));
    }
    if (!started) ImGui::EndDisabled();
    ImGui::SameLine();
    bool can_undo = s_sess && memsearch_undo_depth(s_sess) > 0;
    if (!can_undo) ImGui::BeginDisabled();
    if (ImGui::Button(_L("Undo"))) (void) memsearch_undo(s_sess);
    if (!can_undo) ImGui::EndDisabled();
    ImGui::SameLine();
    bool can_redo = s_sess && memsearch_redo_depth(s_sess) > 0;
    if (!can_redo) ImGui::BeginDisabled();
    if (ImGui::Button(_L("Redo"))) (void) memsearch_redo(s_sess);
    if (!can_redo) ImGui::EndDisabled();
    ImGui::SameLine();
    if (started) {
        ImGui::Text(_("Candidates: %llu  (step %u, sample at frame %llu)"),
                    (unsigned long long) memsearch_candidate_count(s_sess), memsearch_step(s_sess),
                    (unsigned long long) memsearch_sample_frame(s_sess));
    } else {
        ImGui::TextDisabled("%s", _("No search - choose the space and press New search."));
    }
}


/** @brief Formulář filtru. */
static void ms_filter_form(void) {
    const char *ops[MEMSEARCH_OP_COUNT] = {
        _("equal to"), _("not equal to"), _("less than"), _("less or equal"), _("greater than"),
        _("greater or equal"), _("between"), _("increased"), _("decreased"), _("increased by"),
        _("decreased by"), _("changed"), _("unchanged"), _("changed by"), _("bits under mask equal"),
    };
    const char *cmps[MEMSEARCH_CMP_COUNT] = { _("value"), _("previous"), _("first") };
    bool started = s_sess && memsearch_started(s_sess);
    if (!started) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(ms_combo_width(ops, MEMSEARCH_OP_COUNT));
    if (ImGui::Combo(_L("Filter"), &s_op, ops, MEMSEARCH_OP_COUNT)) {
        switch ((en_MEMSEARCH_OP) s_op) {
            case MEMSEARCH_OP_INC: case MEMSEARCH_OP_DEC: case MEMSEARCH_OP_INC_BY:
            case MEMSEARCH_OP_DEC_BY: case MEMSEARCH_OP_CHANGED: case MEMSEARCH_OP_UNCHANGED:
            case MEMSEARCH_OP_CHANGED_BY:
                if (s_cmp == MEMSEARCH_CMP_VALUE) s_cmp = MEMSEARCH_CMP_PREVIOUS;
                break;
            case MEMSEARCH_OP_BETWEEN:
                s_cmp = MEMSEARCH_CMP_VALUE;
                break;
            default:
                break;
        }
    }
    en_MEMSEARCH_OP op = (en_MEMSEARCH_OP) s_op;
    bool needs_value = !(op == MEMSEARCH_OP_INC || op == MEMSEARCH_OP_DEC
                         || op == MEMSEARCH_OP_CHANGED || op == MEMSEARCH_OP_UNCHANGED);
    bool diff = (op == MEMSEARCH_OP_INC_BY || op == MEMSEARCH_OP_DEC_BY || op == MEMSEARCH_OP_CHANGED_BY);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ms_combo_width(cmps, MEMSEARCH_CMP_COUNT));
    ImGui::Combo(_L("compared with"), &s_cmp, cmps, MEMSEARCH_CMP_COUNT);
    if (needs_value && (s_cmp == MEMSEARCH_CMP_VALUE || diff)) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("-2147483648").x + ImGui::GetFrameHeight() * 2.0f);
        ImGui::InputScalar(diff ? _L("Difference") : _L("Value"), ImGuiDataType_S64, &s_value);
    }
    if (op == MEMSEARCH_OP_BETWEEN) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("-2147483648").x + ImGui::GetFrameHeight() * 2.0f);
        ImGui::InputScalar(_L("and"), ImGuiDataType_S64, &s_value2);
    }
    if (op == MEMSEARCH_OP_MASK_EQ) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::CalcTextSize("FFFFFFFF").x + ImGui::GetStyle().FramePadding.x * 2.0f);
        ImGui::InputScalar(_L("Mask"), ImGuiDataType_U32, &s_mask, NULL, NULL, "%X",
                           ImGuiInputTextFlags_CharsHexadecimal);
    }
    ImGui::SameLine();
    ImGui::Checkbox(_L("Not"), &s_negate);
    ImGui::SameLine();
    ImGui::Checkbox(_L("Take new sample"), &s_take_sample);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", _("On: sample first and compare it with the previous sample. Off: compare the last "
                                 "sample with the one before it."));
    }
    ImGui::SameLine();
    if (ImGui::Button(_L("Apply filter"))) ms_filter();
    ImGui::SameLine();
    ImGui::Checkbox(_L("Preview"), &s_preview);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", _("Mark the candidates the filter would remove, using the last sample (no new sample)."));
    }
    if (!started) ImGui::EndDisabled();
    if (op == MEMSEARCH_OP_INC_BY || op == MEMSEARCH_OP_DEC_BY) {
        ImGui::TextDisabled("%s", _("Arithmetic without wrap-around: an u8 going 00h -> FFh is not \"decreased by 1\"."));
    }
}


extern "C" void memsearch_window_render(bool *p_open) {
    if (!p_open || !*p_open) return;
    ImGui::SetNextWindowSize(ImVec2(860.0f, 560.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(_L("Memory Search###memsearch_window"), p_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ms_settings();
    ms_steps();
    ms_filter_form();
    if (s_msg[0]) {
        if (s_msg_error) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", s_msg);
        else ImGui::TextDisabled("%s", s_msg);
    }
    ImGui::Separator();
    if (s_sess && memsearch_started(s_sess)) ms_table();
    ImGui::End();
}


extern "C" void memsearch_window_register_persistence(void *cmod_void) {
    if (!cmod_void) return;
    st_CFGMODULE *cmod = (st_CFGMODULE *) cmod_void;
    st_CFGELEMENT *elm;

    elm = cfgmodule_register_new_element(cmod, (char *) "space", CFGENTYPE_UNSIGNED,
                                         (int) MEMSEARCH_PRESET_RAM, 0, (int) MEMSEARCH_PRESET_COUNT - 1);
    cfgelement_set_handlers(elm, (void *) &s_p_space, (void *) &s_p_space);
    elm = cfgmodule_register_new_element(cmod, (char *) "type", CFGENTYPE_UNSIGNED, 0, 0, MS_TYPE_COUNT - 1);
    cfgelement_set_handlers(elm, (void *) &s_p_type, (void *) &s_p_type);
    elm = cfgmodule_register_new_element(cmod, (char *) "width", CFGENTYPE_UNSIGNED, 2, 1, MEMSEARCH_MAX_WIDTH);
    cfgelement_set_handlers(elm, (void *) &s_p_width, (void *) &s_p_width);
    elm = cfgmodule_register_new_element(cmod, (char *) "big_endian", CFGENTYPE_BOOL, 0);
    cfgelement_set_handlers(elm, (void *) &s_p_big_endian, (void *) &s_p_big_endian);
    elm = cfgmodule_register_new_element(cmod, (char *) "aligned", CFGENTYPE_BOOL, 0);
    cfgelement_set_handlers(elm, (void *) &s_p_aligned, (void *) &s_p_aligned);
    elm = cfgmodule_register_new_element(cmod, (char *) "digit_encoding", CFGENTYPE_UNSIGNED,
                                         (int) MEMSEARCH_DIGITS_DISPLAY, 0, (int) MEMSEARCH_DIGITS_COUNT - 1);
    cfgelement_set_handlers(elm, (void *) &s_p_digit_enc, (void *) &s_p_digit_enc);
    elm = cfgmodule_register_new_element(cmod, (char *) "leading_space", CFGENTYPE_BOOL, 1);
    cfgelement_set_handlers(elm, (void *) &s_p_leading_space, (void *) &s_p_leading_space);
    elm = cfgmodule_register_new_element(cmod, (char *) "allow_large", CFGENTYPE_BOOL, 0);
    cfgelement_set_handlers(elm, (void *) &s_p_allow_large, (void *) &s_p_allow_large);
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
