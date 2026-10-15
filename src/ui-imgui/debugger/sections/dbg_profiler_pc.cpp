/**
 * @file   dbg_profiler_pc.cpp
 * @brief  Záložka "Per PC" okna CPU Profiler (viz dbg_profiler_pc.h).
 *
 * Každých 250 ms si vyžádá nejvýše DBGAPI_PROFILER_PC_ROWS_MAX řádků
 * seřazených emulátorem podle zvoleného sloupce (aby se při víc adresách
 * zobrazily ty podstatné) a k nim souhrn. Symbol řádku = nejbližší symbol
 * na adrese <= PC (+ posun), dohledaný v UI vlákně.
 *
 * Levý klik na řádek otevře adresu v Disassembly #1, pravý klik nabídne
 * breakpoint a Disassembly #2-#5 (stejně jako záložka Functions).
 *
 * @par Licence: GPLv3
 */

#include "main.h"
#include "mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <string>
#include <vector>

#include <glib.h>

#include "libs/imgui/imgui.h"
#include "libs/igfd/ImGuiFileDialog.h"
#include "ui-imgui/filechooser/imgui_filechooser.h"
#include "i18n.h"

#include "dbg_profiler_pc.h"
#include "ui-imgui/debugger/sections/dbg_disassembled.h"  /* dbg_disasm_show_in_slot */
#include "ui-imgui/debugger/dbgapi_helpers.h"             /* dbg_ui_bp_add */

extern "C" {
#include "emulator/debugger/dbgapi_ui.h"
#include "emulator/debugger/dbgapi_cmdrq.h"
#include "emulator/debugger/profiler_pc.h"
#include "emulator/debugger/symbols/sym_db.h"
}


/* g_dbgapi_cmdrq_queue je definovaný v emulator/debugger/dbgapi.c. */
extern "C" st_DBGAPI_CMDRQ_QUEUE g_dbgapi_cmdrq_queue;


namespace {

/** @brief Sloupce tabulky (index = pořadí v BeginTable). */
enum PcCol {
    PC_COL_ADDR = 0,  /**< Adresa instrukce. */
    PC_COL_SYMBOL,    /**< Nejbližší symbol + posun (neřaditelné). */
    PC_COL_COUNT,     /**< Počet vykonání. */
    PC_COL_PCT,       /**< Podíl na T-stavech všech instrukcí (řadí jako T-states). */
    PC_COL_TSTATES,   /**< Součet T-stavů. */
    PC_COL_AVG,       /**< Průměr T-stavů na vykonání (neřaditelné na serveru). */
    PC_COL_WAIT,      /**< Součet čekacích stavů. */
    PC_COL_COUNT_ALL  /**< Počet sloupců. */
};

/**
 * @brief Stav záložky mezi snímky (jen UI vlákno).
 *
 * @invariant rows obsahuje nejvýše DBGAPI_PROFILER_PC_ROWS_MAX položek.
 * @invariant sym_addrs je seřazené vzestupně a odpovídá sym_db verze sym_version.
 */
struct PcPanelState {
    std::vector< st_PROFILER_PC_ROW > rows;   /**< Poslední načtené řádky. */
    st_PROFILER_PC_SUMMARY summary {};        /**< Poslední souhrn. */
    size_t total_rows = 0;                    /**< Adres s vykonáním v rozsahu. */
    bool valid = false;                       /**< Aspoň jeden úspěšný GET. */
    double last_refresh_s = -1.0;             /**< Čas posledního GET (ImGui::GetTime). */

    en_PROFILER_PC_SORT server_sort = PROFILER_PC_SORT_TSTATES; /**< Řazení pro GET. */
    int sort_col = PC_COL_TSTATES;            /**< Sloupec řazení v tabulce. */
    bool sort_asc = false;                    /**< Směr řazení v tabulce. */

    char from_buf[ 8 ] = "0000";              /**< Rozsah adres - od (hex). */
    char to_buf[ 8 ] = "FFFF";                /**< Rozsah adres - do (hex). */

    std::vector< uint16_t > sym_addrs;        /**< Adresy globálních symbolů, seřazené. */
    unsigned sym_version = ~0u;               /**< sym_db verze, ke které patří sym_addrs. */

    bool export_dialog_open = false;          /**< IGFD dialog exportu je otevřený. */
    bool export_error_queue = false;          /**< Otevřít popup s chybou exportu. */
    char export_error[ 256 ] = "";            /**< Text chyby exportu. */
};

PcPanelState g_pc;


/**
 * @brief Přečte hex adresu z bufferu.
 * @return Adresa, nebo `def` při prázdném nebo neplatném textu.
 */
uint16_t parse_hex_addr ( const char *s, uint16_t def )
{
    if ( !s || !s[ 0 ] ) return def;
    char *end = NULL;
    unsigned long v = strtoul ( s, &end, 16 );
    if ( !end || *end != '\0' || v > 0xFFFFul ) return def;
    return (uint16_t) v;
}


/**
 * @brief Odešle DBGAPI_CMD_PROFILER_PC s danou operací (bez řádků).
 * @param op Operace (START, STOP, RESET).
 */
void pc_send_op ( en_DBGAPI_PROFILER_PC_OP op )
{
    st_DBGAPI_PROFILER_PC_PARAM p;
    memset ( &p, 0, sizeof ( p ) );
    p.op = op;
    p.reset = true;
    if ( dbgapi_ui_submit_cmd_sync ( &g_dbgapi_cmdrq_queue, DBGAPI_CMD_PROFILER_PC, &p, NULL, 100 ) ) {
        g_pc.summary = p.out_summary;
    };
    g_pc.last_refresh_s = -1.0;  /* hned znovu načíst */
}


/**
 * @brief Načte souhrn a řádky (GET) do g_pc.
 *
 * Při plné frontě nebo timeoutu nechá předchozí data.
 */
void pc_refresh ( void )
{
    if ( dbgapi_ui_queue_is_full ( &g_dbgapi_cmdrq_queue ) ) return;
    std::vector< st_DBGAPI_PROFILER_PC_ROW > buf ( DBGAPI_PROFILER_PC_ROWS_MAX );
    st_DBGAPI_PROFILER_PC_PARAM p;
    memset ( &p, 0, sizeof ( p ) );
    p.op = DBGAPI_PROFILER_PC_OP_GET;
    p.sort = g_pc.server_sort;
    p.lo = parse_hex_addr ( g_pc.from_buf, 0x0000 );
    p.hi = parse_hex_addr ( g_pc.to_buf, 0xFFFF );
    p.with_symbols = false;
    p.rows = buf.data ( );
    p.rows_max = buf.size ( );
    if ( !dbgapi_ui_submit_cmd_sync ( &g_dbgapi_cmdrq_queue, DBGAPI_CMD_PROFILER_PC, &p, NULL, 50 ) ) {
        return;
    };
    g_pc.rows.clear ( );
    for ( size_t i = 0; i < p.out_written; i++ ) g_pc.rows.push_back ( buf[ i ].r );
    g_pc.summary = p.out_summary;
    g_pc.total_rows = p.out_total;
    g_pc.valid = true;
}


/** @brief Obnoví sym_addrs, pokud se sym_db od posledně změnila. */
void pc_sym_cache_update ( void )
{
    unsigned ver = sym_db_get_version ( );
    if ( ver == g_pc.sym_version ) return;
    g_pc.sym_version = ver;
    g_pc.sym_addrs.clear ( );
    st_SYM_DB_ITER it;
    sym_db_iter_init ( &it );
    const st_SYMBOL *s;
    while ( ( s = sym_db_iter_next ( &it ) ) != NULL ) {
        if ( s->bank_id != 0 || s->addr > 0xFFFFu || !s->name ) continue;
        g_pc.sym_addrs.push_back ( (uint16_t) s->addr );
    };
    std::sort ( g_pc.sym_addrs.begin ( ), g_pc.sym_addrs.end ( ) );
    g_pc.sym_addrs.erase ( std::unique ( g_pc.sym_addrs.begin ( ), g_pc.sym_addrs.end ( ) ),
                           g_pc.sym_addrs.end ( ) );
}


/**
 * @brief Nejbližší globální symbol na adrese <= pc.
 * @param pc         Adresa.
 * @param[out] off   pc - adresa symbolu.
 * @return Symbol, nebo NULL.
 */
const st_SYMBOL *pc_nearest_symbol ( uint16_t pc, unsigned *off )
{
    auto it = std::upper_bound ( g_pc.sym_addrs.begin ( ), g_pc.sym_addrs.end ( ), pc );
    if ( it == g_pc.sym_addrs.begin ( ) ) return NULL;
    uint16_t sa = *( it - 1 );
    *off = (unsigned) ( pc - sa );
    return sym_db_lookup_by_addr ( sa, 0 );
}


/** @brief Seřadí g_pc.rows podle sort_col a sort_asc (při shodě podle adresy). */
void pc_sort_rows ( void )
{
    const int col = g_pc.sort_col;
    const bool asc = g_pc.sort_asc;
    std::stable_sort ( g_pc.rows.begin ( ), g_pc.rows.end ( ),
        [ col, asc ] ( const st_PROFILER_PC_ROW &a, const st_PROFILER_PC_ROW &b ) {
            uint64_t ka, kb;
            switch ( col ) {
                case PC_COL_COUNT:   ka = a.count;   kb = b.count;   break;
                case PC_COL_PCT:
                case PC_COL_TSTATES: ka = a.tstates; kb = b.tstates; break;
                case PC_COL_WAIT:    ka = a.wait;    kb = b.wait;    break;
                default:             ka = a.pc;      kb = b.pc;      break;
            };
            if ( ka != kb ) return asc ? ( ka < kb ) : ( ka > kb );
            return a.pc < b.pc;
        } );
}


/** @brief Řazení na serveru odpovídající sloupci tabulky. */
en_PROFILER_PC_SORT pc_server_sort_for_col ( int col )
{
    switch ( col ) {
        case PC_COL_COUNT: return PROFILER_PC_SORT_COUNT;
        case PC_COL_WAIT:  return PROFILER_PC_SORT_WAIT;
        case PC_COL_ADDR:  return PROFILER_PC_SORT_ADDR;
        default:           return PROFILER_PC_SORT_TSTATES;
    };
}


/** @brief Otevře IGFD dialog pro export CSV. */
void pc_open_export_dialog ( void )
{
    g_pc.export_dialog_open = true;
    static char default_name[ 64 ];
    time_t now = time ( NULL );
    struct tm *tmv = localtime ( &now );
    if ( tmv ) {
        snprintf ( default_name, sizeof ( default_name ), "profiler_pc_%04d%02d%02d_%02d%02d%02d.csv",
                   tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday,
                   tmv->tm_hour, tmv->tm_min, tmv->tm_sec );
    }
    else {
        snprintf ( default_name, sizeof ( default_name ), "profiler_pc.csv" );
    };
    IGFD::FileDialogConfig config;
    config.path = ".";
    config.fileName = default_name;
    config.countSelectionMax = 1;
    config.flags = ImGuiFileDialogFlags_Modal
                 | ImGuiFileDialogFlags_DontShowHiddenFiles
                 | ImGuiFileDialogFlags_ShowDevicesButton
                 | ImGuiFileDialogFlags_ConfirmOverwrite;
    imgui_filechooser_prepare_config ( BASEUI_FCHOOSER_CAT_DBG_EXPORT, config );
    ImGuiFileDialog::Instance ( )->OpenDialog ( "ProfilerPcExportDialog",
                                                _( "Export per-PC profile as CSV" ),
                                                ".csv,.*", config );
}


/**
 * @brief Obslouží otevřený dialog exportu; po potvrzení zapíše CSV emulátorem.
 *
 * Export dělá emu vlákno (DBGAPI_PROFILER_PC_OP_EXPORT), zapíše všechny
 * adresy (ne jen zobrazené) seřazené podle adresy.
 */
void pc_render_export_dialog ( void )
{
    if ( !g_pc.export_dialog_open ) return;
    ImGui::SetNextWindowSize ( ImVec2 ( 800.0f, 500.0f ), ImGuiCond_FirstUseEver );
    if ( !ImGuiFileDialog::Instance ( )->Display ( "ProfilerPcExportDialog" ) ) return;
    if ( ImGuiFileDialog::Instance ( )->IsOk ( ) ) {
        std::string path = ImGuiFileDialog::Instance ( )->GetFilePathName ( );
        st_DBGAPI_PROFILER_PC_PARAM p;
        memset ( &p, 0, sizeof ( p ) );
        p.op = DBGAPI_PROFILER_PC_OP_EXPORT;
        p.sort = PROFILER_PC_SORT_ADDR;
        p.path = path.c_str ( );
        if ( !dbgapi_ui_submit_cmd_sync ( &g_dbgapi_cmdrq_queue, DBGAPI_CMD_PROFILER_PC, &p, NULL, 2000 ) ) {
            g_strlcpy ( g_pc.export_error, p.out_error[ 0 ] ? p.out_error : _( "Export failed" ),
                        sizeof ( g_pc.export_error ) );
            g_pc.export_error_queue = true;
        };
    };
    ImGuiFileDialog::Instance ( )->Close ( );
    g_pc.export_dialog_open = false;
}


/** @brief Toolbar: Start/Stop, Reset, Export, rozsah adres. */
void pc_render_toolbar ( void )
{
    const bool active = g_pc.summary.active;
    if ( ImGui::Button ( active ? _L ( "Stop###pf_startstop" ) : _L ( "Start###pf_startstop" ) ) ) {
        pc_send_op ( active ? DBGAPI_PROFILER_PC_OP_STOP : DBGAPI_PROFILER_PC_OP_START );
    };
    if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_DelayNormal ) ) {
        ImGui::SetTooltip ( "%s", _( "Start clears the previous data." ) );
    };
    ImGui::SameLine ( );
    if ( ImGui::Button ( _L ( "Reset###pf_reset" ) ) ) {
        pc_send_op ( DBGAPI_PROFILER_PC_OP_RESET );
    };
    ImGui::SameLine ( );
    ImGui::BeginDisabled ( g_pc.summary.unique_pcs == 0 );
    if ( ImGui::Button ( _L ( "Export CSV...###pf_export" ) ) ) {
        pc_open_export_dialog ( );
    };
    ImGui::EndDisabled ( );

    ImGui::SameLine ( );
    ImGui::TextUnformatted ( _( "Range:" ) );
    ImGui::SameLine ( );
    const float w = ImGui::CalcTextSize ( "FFFFW" ).x + ImGui::GetStyle ( ).FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth ( w );
    ImGui::InputText ( "###pfpc_from", g_pc.from_buf, 5, ImGuiInputTextFlags_CharsHexadecimal );
    ImGui::SameLine ( );
    ImGui::TextUnformatted ( "-" );
    ImGui::SameLine ( );
    ImGui::SetNextItemWidth ( w );
    ImGui::InputText ( "###pfpc_to", g_pc.to_buf, 5, ImGuiInputTextFlags_CharsHexadecimal );

    ImGui::SameLine ( );
    ImGui::TextDisabled ( "(?)" );
    if ( ImGui::IsItemHovered ( ) ) {
        ImGui::BeginTooltip ( );
        ImGui::TextUnformatted ( _( "Per-PC profiler" ) );
        ImGui::Separator ( );
        ImGui::BulletText ( "%s", _( "Counts every executed instruction by its address: executions,\n"
                                     "T-states (including wait states) and WAIT states." ) );
        ImGui::BulletText ( "%s", _( "Unlike Functions it also sees code that is jumped to, not called." ) );
        ImGui::BulletText ( "%s", _( "Block instructions (LDIR, ...) count every repetition." ) );
        ImGui::BulletText ( "%s", _( "Address = logical PC, memory banks are not told apart." ) );
        ImGui::BulletText ( "%s", _( "Range: show only addresses within it (hex)." ) );
        ImGui::BulletText ( "%s", _( "Only the first 1000 rows of the chosen sort column are shown;\n"
                                     "Export CSV writes all addresses." ) );
        ImGui::EndTooltip ( );
    };
}


/** @brief Tabulka řádků s řazením a akcemi. */
void pc_render_table ( void )
{
    if ( !g_pc.valid || g_pc.rows.empty ( ) ) {
        ImGui::TextDisabled ( "%s", g_pc.summary.active
                                    ? _( "Collecting - the table fills while the emulation runs." )
                                    : _( "No data. Press Start and let the emulation run." ) );
        return;
    };

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                          | ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable
                          | ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingFixedFit;
    if ( !ImGui::BeginTable ( "###pfpc_table", PC_COL_COUNT_ALL, flags ) ) return;

    const float w_hex = ImGui::CalcTextSize ( "FFFFh" ).x + 16.0f;
    const float w_sym = ImGui::CalcTextSize ( "monitor_print_string+999" ).x + 16.0f;
    const float w_num = ImGui::CalcTextSize ( "9999999999" ).x + 16.0f;
    const float w_pct = ImGui::CalcTextSize ( "100.00%" ).x + 16.0f;
    ImGui::TableSetupScrollFreeze ( 0, 1 );
    ImGui::TableSetupColumn ( "Addr",     ImGuiTableColumnFlags_WidthFixed, w_hex, PC_COL_ADDR );
    ImGui::TableSetupColumn ( "Symbol",   ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
                              w_sym, PC_COL_SYMBOL );
    ImGui::TableSetupColumn ( "Count",    ImGuiTableColumnFlags_WidthFixed, w_num, PC_COL_COUNT );
    ImGui::TableSetupColumn ( "T%",       ImGuiTableColumnFlags_WidthFixed, w_pct, PC_COL_PCT );
    ImGui::TableSetupColumn ( "T-states", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort
                              | ImGuiTableColumnFlags_PreferSortDescending, w_num, PC_COL_TSTATES );
    ImGui::TableSetupColumn ( "Avg T",    ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,
                              w_pct, PC_COL_AVG );
    ImGui::TableSetupColumn ( "WAIT",     ImGuiTableColumnFlags_WidthFixed, w_num, PC_COL_WAIT );

    static const char *const k_tips[ PC_COL_COUNT_ALL ] = {
        N_( "Address of the instruction (logical PC).\nLeft click: open in Disassembly #1." ),
        N_( "Nearest symbol at or below the address, + offset." ),
        N_( "How many times the instruction was executed." ),
        N_( "Share of all instruction T-states (without HALT idle\nand interrupt acknowledge)." ),
        N_( "T-states spent in the instruction, including wait states.\nSort here to find hot spots." ),
        N_( "Average T-states per execution = T-states / Count." ),
        N_( "Wait states inserted by the hardware (VRAM access, PSG write)." ),
    };
    ImGui::TableNextRow ( ImGuiTableRowFlags_Headers );
    for ( int col = 0; col < PC_COL_COUNT_ALL; col++ ) {
        ImGui::TableSetColumnIndex ( col );
        const char *name = ImGui::TableGetColumnName ( col );
        ImGui::TableHeader ( name ? name : "" );
        if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_DelayNormal ) ) {
            ImGui::SetTooltip ( "%s", _( k_tips[ col ] ) );
        };
    };

    ImGuiTableSortSpecs *specs = ImGui::TableGetSortSpecs ( );
    if ( specs && specs->SpecsCount > 0 ) {
        const ImGuiTableColumnSortSpecs &s = specs->Specs[ 0 ];
        int col = (int) s.ColumnUserID;
        bool asc = ( s.SortDirection == ImGuiSortDirection_Ascending );
        if ( col != g_pc.sort_col || asc != g_pc.sort_asc || specs->SpecsDirty ) {
            g_pc.sort_col = col;
            g_pc.sort_asc = asc;
            en_PROFILER_PC_SORT ss = pc_server_sort_for_col ( col );
            if ( ss != g_pc.server_sort ) {
                g_pc.server_sort = ss;
                g_pc.last_refresh_s = -1.0;
            };
            specs->SpecsDirty = false;
        };
    };
    pc_sort_rows ( );

    const uint64_t total_t = g_pc.summary.tstates;
    ImGuiListClipper clipper;
    clipper.Begin ( (int) g_pc.rows.size ( ) );
    while ( clipper.Step ( ) ) {
        for ( int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++ ) {
            const st_PROFILER_PC_ROW &r = g_pc.rows[ (size_t) i ];
            ImGui::TableNextRow ( );
            ImGui::TableSetColumnIndex ( PC_COL_ADDR );
            ImGui::PushID ( (int) r.pc );
            char label[ 16 ];
            snprintf ( label, sizeof ( label ), "%04Xh", r.pc );
            if ( ImGui::Selectable ( label, false, ImGuiSelectableFlags_SpanAllColumns ) ) {
                dbg_disasm_show_in_slot ( 0, r.pc );
            };
            if ( ImGui::BeginPopupContextItem ( "###pfpc_row_ctx" ) ) {
                if ( ImGui::MenuItem ( _L ( "Set Breakpoint###pf_bp" ) ) ) {
                    dbg_ui_bp_add ( r.pc, NULL );
                };
                ImGui::Separator ( );
                if ( ImGui::MenuItem ( _L ( "Show in Disassembly #2###pf_d2" ) ) ) dbg_disasm_show_in_slot ( 1, r.pc );
                if ( ImGui::MenuItem ( _L ( "Show in Disassembly #3###pf_d3" ) ) ) dbg_disasm_show_in_slot ( 2, r.pc );
                if ( ImGui::MenuItem ( _L ( "Show in Disassembly #4###pf_d4" ) ) ) dbg_disasm_show_in_slot ( 3, r.pc );
                if ( ImGui::MenuItem ( _L ( "Show in Disassembly #5###pf_d5" ) ) ) dbg_disasm_show_in_slot ( 4, r.pc );
                ImGui::EndPopup ( );
            };
            ImGui::PopID ( );

            ImGui::TableSetColumnIndex ( PC_COL_SYMBOL );
            unsigned off = 0;
            const st_SYMBOL *sym = pc_nearest_symbol ( r.pc, &off );
            if ( sym && sym->name ) {
                if ( off ) ImGui::Text ( "%s+%u", sym->name, off );
                else ImGui::TextUnformatted ( sym->name );
            };

            ImGui::TableSetColumnIndex ( PC_COL_COUNT );
            ImGui::Text ( "%llu", (unsigned long long) r.count );
            ImGui::TableSetColumnIndex ( PC_COL_PCT );
            ImGui::Text ( "%.2f%%", total_t ? 100.0 * (double) r.tstates / (double) total_t : 0.0 );
            ImGui::TableSetColumnIndex ( PC_COL_TSTATES );
            ImGui::Text ( "%llu", (unsigned long long) r.tstates );
            ImGui::TableSetColumnIndex ( PC_COL_AVG );
            ImGui::Text ( "%.1f", r.count ? (double) r.tstates / (double) r.count : 0.0 );
            ImGui::TableSetColumnIndex ( PC_COL_WAIT );
            if ( r.wait ) ImGui::Text ( "%llu", (unsigned long long) r.wait );
            else ImGui::TextDisabled ( "0" );
        };
    };
    ImGui::EndTable ( );
}


/** @brief Stavový řádek: běh, součty a rozdělení času. */
void pc_render_status ( void )
{
    const st_PROFILER_PC_SUMMARY &s = g_pc.summary;
    if ( s.active ) {
        ImGui::PushStyleColor ( ImGuiCol_Text, IM_COL32 ( 80, 220, 120, 255 ) );
        ImGui::TextUnformatted ( _( "● RUNNING" ) );
        ImGui::PopStyleColor ( );
    }
    else {
        ImGui::TextDisabled ( "%s", _( "○ STOPPED" ) );
    };
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );
    ImGui::Text ( _( "Instructions: %llu" ), (unsigned long long) s.instructions );
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );
    const double el = s.elapsed_tstates ? (double) s.elapsed_tstates : 1.0;
    ImGui::Text ( _( "Time: %llu T = instructions %.1f%% + HALT %.1f%% + other %.1f%%" ),
                  (unsigned long long) s.elapsed_tstates,
                  100.0 * (double) s.tstates / el,
                  100.0 * (double) s.halt_idle / el,
                  100.0 * (double) s.other_tstates / el );
    if ( ImGui::IsItemHovered ( ImGuiHoveredFlags_DelayNormal ) ) {
        ImGui::SetTooltip ( "%s", _( "Emulated CPU time since Start.\n"
                                     "HALT = CPU waiting in HALT for an interrupt.\n"
                                     "Other = mainly interrupt acknowledge." ) );
    };
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );
    ImGui::Text ( _( "WAIT: %llu T" ), (unsigned long long) s.wait );
    ImGui::SameLine ( );
    ImGui::TextDisabled ( "|" );
    ImGui::SameLine ( );
    ImGui::Text ( _( "Addresses: %u of %u" ), (unsigned) g_pc.rows.size ( ), s.unique_pcs );
}

} /* namespace */


extern "C" void dbg_profiler_pc_render ( void )
{
    ImGui::PushID ( "pfpc" );
    const double now = ImGui::GetTime ( );
    if ( g_pc.last_refresh_s < 0.0 || ( now - g_pc.last_refresh_s ) > 0.25 ) {
        g_pc.last_refresh_s = now;
        pc_refresh ( );
    };
    pc_sym_cache_update ( );

    pc_render_toolbar ( );
    ImGui::Separator ( );

    ImVec2 avail = ImGui::GetContentRegionAvail ( );
    float footer_h = ImGui::GetTextLineHeightWithSpacing ( ) + ImGui::GetStyle ( ).ItemSpacing.y * 2.0f;
    float table_h = avail.y - footer_h;
    if ( table_h < 60.0f ) table_h = 60.0f;
    ImGui::BeginChild ( "###pfpc_table_child", ImVec2 ( 0.0f, table_h ), false, ImGuiWindowFlags_None );
    pc_render_table ( );
    ImGui::EndChild ( );

    ImGui::Separator ( );
    pc_render_status ( );

    pc_render_export_dialog ( );
    if ( g_pc.export_error_queue ) {
        g_pc.export_error_queue = false;
        ImGui::OpenPopup ( "###pf_export_error" );
    };
    if ( ImGui::BeginPopupModal ( _L ( "Export error###pf_export_error" ), NULL,
                                  ImGuiWindowFlags_AlwaysAutoResize ) ) {
        ImGui::TextUnformatted ( g_pc.export_error );
        ImGui::Separator ( );
        if ( ImGui::Button ( _L ( "OK###pf_export_error_ok" ), ImVec2 ( 120.0f, 0.0f ) ) ) {
            ImGui::CloseCurrentPopup ( );
        };
        ImGui::EndPopup ( );
    };
    ImGui::PopID ( );
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
