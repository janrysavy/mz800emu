/*
 * callstack.c - shadow stack implementace + Z80 hot-path callbacky.
 *
 * Viz callstack.h pro popis API. Tento soubor obsahuje:
 *   - statický shadow buffer + counters
 *   - Z80 CALL/RET callback funkce (registrované přes z80_set_call/ret)
 *   - fan-out hooky volané z mzarch_cpu_ctrl_event_cb a mzarch_iff_change_cb
 *   - cfgmain integraci ([CALLSTACK] sekce s klíčem active)
 *   - snapshot a listener API
 *
 * Hot-path strategie:
 *   - Z80 CALL/RET callback slot je single-consumer (= jen callstack);
 *     gate g_callstack_active rozhoduje pouze o (re)registraci slotu.
 *     Pokud subsystém OFF, slot drží NULL = z80_execute() ho přeskakuje
 *     NULL-checkem bez volání (= nulový overhead per CALL/RET).
 *   - cpu_ctrl_event_cb a iff_change_cb slot je single-consumer pro
 *     mzarch_*_cb wrapper. Wrapper dělá fan-out: eventlog + bp_event +
 *     callstack (= callstack_on_cpu_ctrl_event / callstack_on_iff_change).
 *     Callstack fan-out funkce gate-checkne g_callstack_active a vrátí
 *     se ihned pokud OFF.
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

#include "callstack.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <glib.h>

#include "libs/sdlapp/sdlapp_options.h"
#include "libs/cfgfile/cfgmodule.h"
#include "libs/cpu-z80/z80.h"
#include "emulator/cfgmain.h"
#include "emulator/mzarch/mzarch.h"
#include "emulator/debugger/trace/tlog_common.h"


/* ============================================================================
 * Globální stav (private, exposed přes API)
 * ============================================================================ */

/**
 * @brief Gate flag (= viz callstack.h dokumentace).
 *
 * Modifikuje pouze @ref callstack_set_active. Hot path používá raw
 * čtení (volatile není nutné - GCC v MSYS2 single-thread per modify).
 */
uint8_t g_callstack_active = 0;


/**
 * @brief Shadow stack buffer.
 *
 * BSS sekce, žádná dynamická alokace. ~5 KB statické paměti.
 * Index 0 = nejstarší frame, index g_depth-1 = top stacku.
 */
static st_CALLSTACK_ENTRY g_shadow[ CALLSTACK_MAX_DEPTH ];

/**
 * @brief Aktuální hloubka shadow stacku (0..CALLSTACK_MAX_DEPTH).
 */
static int g_depth = 0;


/**
 * @brief Persistent statistiky (drží se přes set_active toggles,
 *        reset jen explicitně přes @ref callstack_reset_stats nebo
 *        @ref callstack_reset).
 */
static struct {
    int      max_depth_reached;
    uint32_t divergence_count;       /* total = trampoline + longjmp + mismatch */
    uint32_t diverg_trampoline;
    uint32_t diverg_longjmp;
    uint32_t diverg_mismatch;
    uint32_t sp_swap_count;
    uint32_t overflow_count;
    uint32_t stack_discard_count;
} g_stats = { 0, 0, 0, 0, 0, 0, 0, 0 };


/**
 * @brief Listener slot (V1 single).
 */
static callstack_on_enter_cb_t g_listener_on_enter = NULL;
static callstack_on_exit_cb_t  g_listener_on_exit  = NULL;
static void                   *g_listener_user     = NULL;


/**
 * @brief Ring buffer nedávných divergence events pro diagnostiku.
 *
 * Slot 0..head-1 + (pokud overflow) slot head..RING_SIZE-1. Index
 * g_diverg_count drží celkový počet zapsaných events (saturuje na RING_SIZE
 * pro indikaci "buffer plný"; pro stat purposes je tu diverg_count v
 * g_stats).
 */
static st_CALLSTACK_DIVERG_EVENT g_diverg_ring [ CALLSTACK_DIVERG_RING_SIZE ];
static int g_diverg_head  = 0;
static int g_diverg_count = 0;  /* Saturuje na RING_SIZE. */


/**
 * @brief Idempotence flag pro callstack_init.
 */
static bool s_cfg_registered = false;


/* ============================================================================
 * Pomocné funkce (private)
 * ============================================================================ */

/**
 * @brief Vrátí aktuální cycle counter z CPU pro cycles_at_entry pole.
 *
 * V1 čte cpu->total_cycles (uint32_t v z80_t). Pole struktury je
 * uint64_t - extend bez ztráty informace. Při wraparound counteru
 * (~1224 sekund při 3.5 MHz, ~5.7 min při 14 MHz) může listener
 * inclusive cycles výpočet vrátit nesmyslnou hodnotu - V1 acceptable,
 * Profiler V1.5 si bude pamatovat reset moment.
 *
 * @return cpu->total_cycles cast na uint64_t.
 *
 * @note Pokud g_mzarch_main.cpu == NULL (= před init), vrací 0.
 *       To by se ale nemělo stát - callstack je registrován až po
 *       mzarch_platform init.
 */
static uint64_t get_cycles_now ( void )
{
    if ( !g_mzarch_main.cpu ) return 0;
    return (uint64_t) g_mzarch_main.cpu->total_cycles;
}


/**
 * @brief Snapshot banking state hash.
 *
 * V1 placeholder = 0 (= žádné banking awareness). Detailní implementace
 * je odložená - banking layout se per-mzarch liší (mz800_memory.map,
 * mz1500 GDG bank, ...) a unified hash potřebuje per-arch větve.
 *
 * @return 0 (= V1 placeholder).
 *
 * @todo Fáze 1C+: spočítat CRC8 přes per-arch banking state (= g_memory.map
 *       + g_memext stav). UI ukáže jako tooltip.
 */
static uint8_t snapshot_bank_id ( void )
{
    /* TODO Fáze 1C+: per-arch banking hash. */
    return 0;
}


/**
 * @brief Zapíše divergence event do ring bufferu (pro diagnostiku).
 *
 * Hot path - volá se z RET/RETI/RETN hooků jen při skutečně detected
 * divergenci. Saturuje na RING_SIZE = pokud plný, přepíše nejstarší.
 *
 * @param kind        en_CALLSTACK_DIVERG_KIND (trampoline/longjmp/mismatch).
 * @param ret_pc      Adresa RET opcode (0 pokud nedostupná = RETI/RETN).
 * @param pop_target  pop_target z RET hooku (0 pokud RETI/RETN).
 * @param sp_before   sp_before_pop z RET hooku (0 pokud RETI/RETN).
 */
static void diverg_record ( uint8_t kind, uint16_t ret_pc,
                             uint16_t pop_target, uint16_t sp_before )
{
    st_CALLSTACK_DIVERG_EVENT *ev = &g_diverg_ring[ g_diverg_head ];
    ev->ret_pc       = ret_pc;
    ev->pop_target   = pop_target;
    ev->sp_before    = sp_before;
    if ( g_depth > 0 ) {
        const st_CALLSTACK_ENTRY *top = &g_shadow[ g_depth - 1 ];
        ev->top_return = top->return_addr;
        ev->top_sp     = top->sp_at_entry;
        ev->top_kind   = top->kind;
    } else {
        ev->top_return = 0;
        ev->top_sp     = 0;
        ev->top_kind   = 0xFF;
    }
    ev->shadow_depth = (uint8_t) ( ( g_depth > 255 ) ? 255 : g_depth );
    ev->kind         = kind;
    ev->_pad         = 0;
    ev->cycles_at    = get_cycles_now ( );

    g_diverg_head = ( g_diverg_head + 1 ) % CALLSTACK_DIVERG_RING_SIZE;
    if ( g_diverg_count < CALLSTACK_DIVERG_RING_SIZE ) g_diverg_count++;
}


/**
 * @brief Vyplní raster/timing fieldy entry z tlog_common counterů.
 *
 * Snapshot v okamžiku push - umožňuje UI korelaci frame s raster
 * pozicí (frame number, scanline, pixel col). Funguje per-arch
 * (mz800/mz700/mz1500), VIDEO_GET_SCREEN_ROW/COL macro je sdílený
 * pattern.
 *
 * @param e  Entry kterou plníme (in-out, modifikuje pxclk_* a
 *           screens_total).
 */
static void snapshot_raster_timing ( st_CALLSTACK_ENTRY *e )
{
    e->pxclk_total     = tlog_common_get_pxclk_total ( );
    e->screens_total   = tlog_common_get_screens_total ( );
    e->pxclk_in_screen = tlog_common_get_pxclk_in_screen ( );
}


/**
 * @brief Vlastní push do shadow stacku + listener notify.
 *
 * Caller už ověřil g_callstack_active. Při full shadow inkrementuje
 * overflow_count a vrátí se bez push (= emulátor není impactnut).
 *
 * @param entry  Naplněná entry (push se kopíruje do shadow slotu).
 */
static void push_entry ( const st_CALLSTACK_ENTRY *entry )
{
    if ( g_depth >= CALLSTACK_MAX_DEPTH ) {
        g_stats.overflow_count++;
        return;
    }
    g_shadow[ g_depth ] = *entry;
    g_depth++;
    if ( g_depth > g_stats.max_depth_reached ) {
        g_stats.max_depth_reached = g_depth;
    }
    if ( g_listener_on_enter ) {
        g_listener_on_enter ( &g_shadow[ g_depth - 1 ], g_listener_user );
    }
}


/**
 * @brief Vlastní pop ze shadow stacku + listener notify.
 *
 * Caller už ověřil g_depth > 0. Listener dostane snapshot popnuté
 * entry před odstraněním (= pointer platný po dobu callback).
 */
static void pop_top_entry ( void )
{
    if ( g_depth <= 0 ) return;
    st_CALLSTACK_ENTRY snapshot = g_shadow[ g_depth - 1 ];
    g_depth--;
    if ( g_listener_on_exit ) {
        g_listener_on_exit ( &snapshot, g_listener_user );
    }
}


/**
 * @brief Hledá v shadow stacku entry s daným return_addr (= longjmp pattern).
 *
 * Prochází shadow od TOP dolů. Vrací hloubku matche (0 = top, depth-1 =
 * nejstarší) nebo -1 pokud nenalezeno.
 *
 * @param target Adresa kterou hledáme.
 * @return Hloubka od TOP, nebo -1.
 */
static int find_in_shadow_from_top ( uint16_t target )
{
    for ( int i = g_depth - 1; i >= 0; i-- ) {
        if ( g_shadow[ i ].return_addr == target ) {
            return ( g_depth - 1 ) - i;
        }
    }
    return -1;
}


/* ============================================================================
 * Z80 hot-path callbacky (registrované přes z80_set_call/z80_set_ret)
 * ============================================================================ */

/**
 * @brief Z80 call_cb - fire při taken CALL/CALL cc dispatch.
 *
 * Subsystém je gate-checkován faktem že slot je NULL pokud
 * g_callstack_active == 0 (= tato funkce se nezavolá). Tj. interní
 * gate test neopakujeme = lehčí hot path.
 */
static void z80_hook_call ( z80_t *cpu, uint16_t call_site, uint16_t target,
                            uint16_t return_addr, void *user_data )
{
    (void) cpu;
    (void) user_data;

    st_CALLSTACK_ENTRY e;
    memset ( &e, 0, sizeof ( e ) );
    e.return_addr      = return_addr;
    e.call_site_addr   = call_site;
    e.target_addr      = target;
    /* sp_at_entry = SP PO pushi return_addr. V okamžiku fire callback je
     * PUSH ještě před, ale Z80 lib API garantuje že "vzápětí provede push
     * a skok". Tedy sp_at_entry = cpu->sp - 2. */
    e.sp_at_entry      = (uint16_t) ( cpu->sp - 2 );
    e.cycles_at_entry  = get_cycles_now ( );
    snapshot_raster_timing ( &e );
    e.bank_id          = snapshot_bank_id ( );
    e.kind             = (uint8_t) CS_KIND_CALL;
    e.flags            = 0;
    e.im_mode          = 0;
    e.im2_vector_addr  = 0;

    push_entry ( &e );
}


/**
 * @brief Z80 ret_cb - fire při taken RET/RET cc dispatch.
 *
 * Divergence handling:
 *   - empty shadow: emit SYNTHETIC frame skrz listener (= notify),
 *     žádný entry se nepushuje; divergence_count++;
 *   - top match (return_addr + sp): happy path pop.
 *   - mismatch: hledání entry s odpovídajícím return_addr v stacku
 *     (longjmp), pop N entries. Pokud nenalezeno, pop top + count
 *     divergence.
 */
static void z80_hook_ret ( z80_t *cpu, uint16_t pop_target,
                           uint16_t sp_before_pop, void *user_data )
{
    (void) user_data;

    /* ret_pc = adresa RET opcode. RET je 1-byte (0xC9 / 0xC0..F8), takže
     * cpu->pc (= adresa za RET) - 1. RETI/RETN jdou přes iff_change a
     * mají vlastní recording path. */
    uint16_t ret_pc = (uint16_t) ( cpu->pc - 1 );

    if ( g_depth <= 0 ) {
        /* Prázdný shadow + RET = klasická PUSH+RET trampolína (manual
         * call frame, sprite dispatcher) - žádný CALL nepředcházel. */
        g_stats.divergence_count++;
        g_stats.diverg_trampoline++;
        diverg_record ( CS_DIVERG_TRAMPOLINE, ret_pc, pop_target, sp_before_pop );
        if ( g_listener_on_exit ) {
            st_CALLSTACK_ENTRY synth;
            memset ( &synth, 0, sizeof ( synth ) );
            synth.return_addr = pop_target;
            synth.sp_at_entry = sp_before_pop;
            synth.cycles_at_entry = get_cycles_now ( );
            synth.kind  = (uint8_t) CS_KIND_SYNTHETIC;
            synth.flags = CS_FLAG_DIVERGENT;
            g_listener_on_exit ( &synth, g_listener_user );
        }
        return;
    }

    /* Top match - happy path. sp_before_pop je SP v okamžiku PEEK16,
     * tj. shoduje se s sp_at_entry uloženým při push (= SP po push). */
    st_CALLSTACK_ENTRY *top = &g_shadow[ g_depth - 1 ];
    if ( top->return_addr == pop_target && top->sp_at_entry == sp_before_pop ) {
        pop_top_entry ( );
        return;
    }

    /* SP-aware klasifikace (Z80 SP roste dolů, push = SP -= 2):
     *
     * sp_before_pop < top.sp_at_entry → SP klesl POD úroveň entry pushe,
     * tj. byly pushnuté extra hodnoty NAD top.return_addr. Tento RET
     * tedy POP-ne tu extra pushnutou hodnotu (= trampolína VNOŘENÁ uvnitř
     * top frame), ne návrat z top frame. Klasický CP/M BDOS dispatch:
     *     CALL BDOS    ; push USER+3, sp=X-2 (top frame)
     *     BDOS: PUSH func ; sp=X-4
     *           RET    ; ← tady jsme: pop func, sp=X-2; jump to func
     *
     * Správné chování: NEPOPOVAT shadow (= top frame je stále aktivní,
     * jen jsme uvnitř něj v "vnořeném" trampoline-jumped kódu). Diverg
     * counter++ jen pro statistiku.
     *
     * Bez tohoto fixu by každé BDOS volání generovalo 2× diverg event a
     * dlouhodobý shadow desync (= popl by se CALL_BDOS entry chybně).
     */
    if ( sp_before_pop < top->sp_at_entry ) {
        g_stats.divergence_count++;
        g_stats.diverg_trampoline++;
        diverg_record ( CS_DIVERG_TRAMPOLINE, ret_pc, pop_target, sp_before_pop );
        /* Shadow zůstává intact. Žádný listener emit - to není exit
         * frame, jen vnořený jump. */
        return;
    }

    /* sp_before_pop > top.sp_at_entry → SP vystoupil VÝŠ než top entry.
     * To znamená že někdo manuálně popnul (POP HL pattern, manual stack
     * unwinding) víc úrovní než shadow eviduje. Najdi entry s matchnoutím
     * SP a unwinduj shadow. Pokud match SP nikde nenajdeme, použij
     * return_addr lookup (longjmp via return). */
    if ( sp_before_pop > top->sp_at_entry ) {
        /* Hledat shadow entry s sp_at_entry == sp_before_pop. */
        int sp_match_depth = -1;
        for ( int i = g_depth - 1; i >= 0; i-- ) {
            if ( g_shadow[ i ].sp_at_entry == sp_before_pop ) {
                sp_match_depth = ( g_depth - 1 ) - i;
                break;
            }
        }
        if ( sp_match_depth >= 0 ) {
            diverg_record ( CS_DIVERG_LONGJMP, ret_pc, pop_target, sp_before_pop );
            if ( g_listener_on_exit ) {
                st_CALLSTACK_ENTRY snapshot = g_shadow[ g_depth - 1 ];
                snapshot.flags |= CS_FLAG_DIVERGENT;
                g_listener_on_exit ( &snapshot, g_listener_user );
            }
            g_depth -= ( sp_match_depth + 1 );
            g_stats.divergence_count++;
            g_stats.diverg_longjmp++;
            return;
        }
        /* SP-based fallback - hledat return_addr match. */
        int found_depth = find_in_shadow_from_top ( pop_target );
        if ( found_depth >= 0 ) {
            diverg_record ( CS_DIVERG_LONGJMP, ret_pc, pop_target, sp_before_pop );
            if ( g_listener_on_exit ) {
                st_CALLSTACK_ENTRY snapshot = g_shadow[ g_depth - 1 ];
                snapshot.flags |= CS_FLAG_DIVERGENT;
                g_listener_on_exit ( &snapshot, g_listener_user );
            }
            g_depth -= ( found_depth + 1 );
            g_stats.divergence_count++;
            g_stats.diverg_longjmp++;
            return;
        }
        /* Žádný SP ani return match nikde - conservative pop + counter.
         * Pokud SP vystoupil NAD nejhlubší tracked frame, počítáme to
         * navíc jako "stack discard" signál (= warm boot / ROM reset /
         * exception handler / OS scheduler) - JEN counter, žádný
         * auto-clear shadow. User si v takovém případě stiskne Reset
         * sám pokud chce čistý start (= heuristika je v některých OS
         * patternech zavádějící, např. CP/M BDOS dispatch mění SP přes
         * vlastní stacky). */
        if ( sp_before_pop > g_shadow[ 0 ].sp_at_entry ) {
            g_stats.stack_discard_count++;
            diverg_record ( CS_DIVERG_STACK_DISCARD, ret_pc,
                            pop_target, sp_before_pop );
        }
        g_stats.divergence_count++;
        g_stats.diverg_mismatch++;
        diverg_record ( CS_DIVERG_MISMATCH, ret_pc, pop_target, sp_before_pop );
        pop_top_entry ( );
        return;
    }

    /* sp_before_pop == top.sp_at_entry, ale return_addr nesedí.
     * Zkusíme deep search (= klasický longjmp via return_addr - např.
     * manual unwind kde rutina vrátí o N úrovní nahoru se stejným SP
     * jako top entry). Pokud nenajdeme, je to self-modifying RET adresa
     * → conservative pop. */
    int found_depth = find_in_shadow_from_top ( pop_target );
    if ( found_depth >= 0 ) {
        diverg_record ( CS_DIVERG_LONGJMP, ret_pc, pop_target, sp_before_pop );
        if ( g_listener_on_exit ) {
            st_CALLSTACK_ENTRY snapshot = g_shadow[ g_depth - 1 ];
            snapshot.flags |= CS_FLAG_DIVERGENT;
            g_listener_on_exit ( &snapshot, g_listener_user );
        }
        g_depth -= ( found_depth + 1 );
        g_stats.divergence_count++;
        g_stats.diverg_longjmp++;
        return;
    }
    /* Self-modifying RET addr nebo neznámý pattern - conservative pop. */
    g_stats.divergence_count++;
    g_stats.diverg_mismatch++;
    diverg_record ( CS_DIVERG_MISMATCH, ret_pc, pop_target, sp_before_pop );
    pop_top_entry ( );
}


/* ============================================================================
 * Fan-out hooky (volá mzarch wrapper)
 * ============================================================================ */

void callstack_on_cpu_ctrl_event ( uint8_t event, uint16_t pc )
{
    if ( !g_callstack_active ) return;

    /* HALT_ENTER (0) a HALT_EXIT (1) nás nezajímají - callstack řeší
     * jen RST opcody. */
    if ( event < 2 ) return;

    /* event 2..9 = RST_00..RST_38. Target = (event - 2) * 8. */
    uint16_t target = (uint16_t) ( ( event - 2 ) * 8 );

    st_CALLSTACK_ENTRY e;
    memset ( &e, 0, sizeof ( e ) );
    /* PC v cpu_ctrl_event_cb pro RST = za RST opcode (= rPC po fetch,
     * před push do stacku). RST je 1-byte opcode, takže RST adresa =
     * pc - 1, návratová adresa (= co se pushne) = pc. */
    e.return_addr     = pc;
    e.call_site_addr  = (uint16_t) ( pc - 1 );
    e.target_addr     = target;
    /* sp_at_entry = SP po push (= cpu->sp - 2, jako u CALL). */
    if ( g_mzarch_main.cpu ) {
        e.sp_at_entry = (uint16_t) ( g_mzarch_main.cpu->sp - 2 );
    } else {
        e.sp_at_entry = 0;
    }
    e.cycles_at_entry = get_cycles_now ( );
    snapshot_raster_timing ( &e );
    e.bank_id         = snapshot_bank_id ( );
    e.kind            = (uint8_t) CS_KIND_RST;
    e.flags           = 0;
    e.im_mode         = 0;
    e.im2_vector_addr = 0;

    push_entry ( &e );
}


void callstack_on_iff_change ( uint8_t reason )
{
    if ( !g_callstack_active ) return;

    /* Zajímá nás jen RETI (5) a RETN (6) - to jsou návraty z IRQ/NMI
     * frame. Pro ně provádíme top-pop. Ostatní reasony (EI/DI/INT_ACK/
     * NMI_ACK/RESET) callstack zde neřeší - INT_ACK a NMI_ACK push
     * jdou přes mzarch IRQ accept hook (Fáze 1C), RESET clearuje
     * shadow přes callstack_reset z platform reset path. */
    if ( reason != 5 && reason != 6 ) return;

    /* RETI/RETN unwind. Stejná divergence logika jako u běžného RET,
     * ale match check podle return_addr není možný (= pop_target není
     * předán - musíme bezpečně pop top). */
    if ( g_depth <= 0 ) {
        g_stats.divergence_count++;
        g_stats.diverg_trampoline++;  /* RETI nad prázdným = trampoline-like */
        /* ret_pc/pop_target/sp_before nedostupné z iff_change hook (= ED
         * RETI/RETN po POP fíruje). Zapíšeme 0 jako "nedostupné". */
        diverg_record ( CS_DIVERG_TRAMPOLINE, 0, 0, 0 );
        if ( g_listener_on_exit ) {
            st_CALLSTACK_ENTRY synth;
            memset ( &synth, 0, sizeof ( synth ) );
            synth.cycles_at_entry = get_cycles_now ( );
            synth.kind  = (uint8_t) CS_KIND_SYNTHETIC;
            synth.flags = CS_FLAG_DIVERGENT;
            g_listener_on_exit ( &synth, g_listener_user );
        }
        return;
    }
    pop_top_entry ( );
}


void callstack_on_irq_accept ( uint16_t pc_before_irq, uint16_t isr_addr,
                                uint8_t im_mode, uint16_t im2_vector_addr )
{
    if ( !g_callstack_active ) return;

    st_CALLSTACK_ENTRY e;
    memset ( &e, 0, sizeof ( e ) );
    e.return_addr      = pc_before_irq;
    e.call_site_addr   = pc_before_irq;
    e.target_addr      = isr_addr;
    if ( g_mzarch_main.cpu ) {
        /* IRQ accept push proběhl uvnitř z80_process_interrupt;
         * cpu->sp v okamžiku volání tohoto hooku už ukazuje na pushovanou
         * návratovou adresu. Tj. sp_at_entry = cpu->sp. */
        e.sp_at_entry = (uint16_t) g_mzarch_main.cpu->sp;
    } else {
        e.sp_at_entry = 0;
    }
    e.cycles_at_entry = get_cycles_now ( );
    snapshot_raster_timing ( &e );
    e.bank_id         = snapshot_bank_id ( );
    e.flags           = 0;
    e.im_mode         = im_mode;
    e.im2_vector_addr = ( im_mode == 2 ) ? im2_vector_addr : 0;
    switch ( im_mode ) {
        case 0:  e.kind = (uint8_t) CS_KIND_IRQ_IM0; break;
        case 1:  e.kind = (uint8_t) CS_KIND_IRQ_IM1; break;
        case 2:  e.kind = (uint8_t) CS_KIND_IRQ_IM2; break;
        default: e.kind = (uint8_t) CS_KIND_IRQ_IM1; break;
    }

    push_entry ( &e );
}


/* Volá mzarch_main_process_interrupt() po přijetí NMI. Žádný HW zdroj
 * v emu modelu (MZ-800/700/1500) NMI pin netáhne; NMI nastavuje jen MCP
 * nmi_inject (z80_nmi). Mzarch_nmi_cb v interrupt.c je vázán na assertion
 * (= setup nmi_pending), ne accept.
 *
 * Fáze 1C TODO/SP swap detekce (LD SP,nn / EX (SP),HL / ADD HL,SP atd.):
 * mimo scope V1. Potřebuje opcode classification (cputrack reuse) -
 * až V1.5+ mutant. Statistika sp_swap_count zůstává 0 ve V1. */
void callstack_on_nmi_accept ( uint16_t pc_before_nmi )
{
    if ( !g_callstack_active ) return;

    st_CALLSTACK_ENTRY e;
    memset ( &e, 0, sizeof ( e ) );
    e.return_addr      = pc_before_nmi;
    e.call_site_addr   = pc_before_nmi;
    e.target_addr      = 0x0066;
    if ( g_mzarch_main.cpu ) {
        e.sp_at_entry = (uint16_t) g_mzarch_main.cpu->sp;
    } else {
        e.sp_at_entry = 0;
    }
    e.cycles_at_entry = get_cycles_now ( );
    snapshot_raster_timing ( &e );
    e.bank_id         = snapshot_bank_id ( );
    e.kind            = (uint8_t) CS_KIND_NMI;
    e.flags           = 0;
    e.im_mode         = 0;
    e.im2_vector_addr = 0;

    push_entry ( &e );
}


/* ============================================================================
 * Lifecycle (activation, reset)
 * ============================================================================ */

void callstack_reset ( void )
{
    g_depth = 0;
    g_stats.max_depth_reached = 0;
    g_stats.divergence_count  = 0;
    g_stats.diverg_trampoline = 0;
    g_stats.diverg_longjmp    = 0;
    g_stats.diverg_mismatch   = 0;
    g_stats.sp_swap_count     = 0;
    g_stats.overflow_count    = 0;
    g_stats.stack_discard_count = 0;
    g_diverg_head  = 0;
    g_diverg_count = 0;
}


int callstack_get_recent_diverg ( st_CALLSTACK_DIVERG_EVENT *out_events )
{
    if ( !out_events ) return 0;
    /* Vrátíme entries v chronologickém pořadí (oldest → newest). Pokud
     * count < RING_SIZE, slot 0..count-1 obsahuje data. Pokud count ==
     * RING_SIZE, ring je plný a oldest je na head (= další slot za
     * naposledy zapsaným). */
    int n = g_diverg_count;
    if ( n <= 0 ) return 0;
    int start = ( g_diverg_count == CALLSTACK_DIVERG_RING_SIZE )
                ? g_diverg_head : 0;
    for ( int i = 0; i < n; i++ ) {
        int src = ( start + i ) % CALLSTACK_DIVERG_RING_SIZE;
        out_events[ i ] = g_diverg_ring[ src ];
    }
    return n;
}


void callstack_set_active ( bool enable )
{
    /* Idempotence. */
    if ( (bool) g_callstack_active == enable ) return;

    if ( enable ) {
        /* Zaregistruj Z80 CALL/RET hooky. cpu_ctrl_event a iff_change
         * jsou už registrovány v mzarch_platform_init na mzarch_*_cb -
         * naše fan-out se aktivuje skrz g_callstack_active gate uvnitř
         * callstack_on_cpu_ctrl_event / callstack_on_iff_change.
         *
         * Mzarch source soubor (interrupt.c) musí v post-event fan-out
         * sekci tyto funkce volat - úprava je součástí callstack mutantu
         * Fáze 1B (= zde, viz interrupt.c diff). */
        if ( g_mzarch_main.cpu ) {
            z80_set_call ( g_mzarch_main.cpu, z80_hook_call, NULL );
            z80_set_ret  ( g_mzarch_main.cpu, z80_hook_ret,  NULL );
        }
        callstack_reset ( );
        g_callstack_active = 1;
    } else {
        g_callstack_active = 0;
        if ( g_mzarch_main.cpu ) {
            z80_set_call ( g_mzarch_main.cpu, NULL, NULL );
            z80_set_ret  ( g_mzarch_main.cpu, NULL, NULL );
        }
        /* Shadow stack neresetujeme - UI může chtít zobrazit poslední
         * stav. callstack_reset volá platform reset (jiná cesta). */
    }
}


/* ============================================================================
 * Snapshot API (volá dbgapi sync handler, Fáze 2B)
 * ============================================================================ */

int callstack_snapshot_get ( st_CALLSTACK_ENTRY **out_entries, int *out_count )
{
    if ( !out_entries || !out_count ) return -1;

    if ( g_depth <= 0 ) {
        *out_entries = NULL;
        *out_count   = 0;
        return 0;
    }

    size_t bytes = (size_t) g_depth * sizeof ( st_CALLSTACK_ENTRY );
    st_CALLSTACK_ENTRY *buf = (st_CALLSTACK_ENTRY *) g_malloc ( bytes );
    if ( !buf ) {
        *out_entries = NULL;
        *out_count   = 0;
        return -1;
    }
    memcpy ( buf, g_shadow, bytes );
    *out_entries = buf;
    *out_count   = g_depth;
    return 0;
}


void callstack_snapshot_free ( st_CALLSTACK_ENTRY *entries )
{
    if ( entries ) g_free ( entries );
}


/* ============================================================================
 * Statistiky
 * ============================================================================ */

void callstack_get_stats ( st_CALLSTACK_STATS *out )
{
    if ( !out ) return;
    out->current_depth        = g_depth;
    out->max_depth_reached    = g_stats.max_depth_reached;
    out->divergence_count     = g_stats.divergence_count;
    out->diverg_trampoline    = g_stats.diverg_trampoline;
    out->diverg_longjmp       = g_stats.diverg_longjmp;
    out->diverg_mismatch      = g_stats.diverg_mismatch;
    out->sp_swap_count        = g_stats.sp_swap_count;
    out->overflow_count       = g_stats.overflow_count;
    out->stack_discard_count  = g_stats.stack_discard_count;
}


void callstack_reset_stats ( void )
{
    g_stats.max_depth_reached = g_depth; /* current depth zachováno */
    g_stats.divergence_count  = 0;
    g_stats.diverg_trampoline = 0;
    g_stats.diverg_longjmp    = 0;
    g_stats.diverg_mismatch   = 0;
    g_stats.sp_swap_count     = 0;
    g_stats.overflow_count    = 0;
    g_stats.stack_discard_count = 0;
    g_diverg_head  = 0;
    g_diverg_count = 0;
}


/* ============================================================================
 * Listener API
 * ============================================================================ */

void callstack_register_listener ( callstack_on_enter_cb_t on_enter,
                                    callstack_on_exit_cb_t on_exit,
                                    void *user )
{
    g_listener_on_enter = on_enter;
    g_listener_on_exit  = on_exit;
    g_listener_user     = user;
}


void callstack_unregister_listener ( void *user )
{
    (void) user;  /* V1 single slot - user porovnání nepoužito. */
    g_listener_on_enter = NULL;
    g_listener_on_exit  = NULL;
    g_listener_user     = NULL;
}


/* ============================================================================
 * cfgmain integrace ([CALLSTACK] sekce)
 * ============================================================================ */

/**
 * @brief Propagate cb pro [CALLSTACK] active - aplikuje hodnotu z INI.
 */
static void cfg_propagate_active ( void *e, void *data )
{
    (void) data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *) e;
    int v = cfgelement_get_bool_value ( elm );
    callstack_set_active ( v ? true : false );
}

/**
 * @brief Save cb pro [CALLSTACK] active - zapíše aktuální g_callstack_active.
 */
static void cfg_save_active ( void *e, void *data )
{
    (void) data;
    st_CFGELEMENT *elm = (st_CFGELEMENT *) e;
    cfgelement_set_bool_value ( elm, g_callstack_active ? 1 : 0 );
}


void callstack_init ( void )
{
    if ( s_cfg_registered ) return;
    s_cfg_registered = true;

    CFGMOD *cmod = cfgroot_register_new_module ( g_cfgmain, "CALLSTACK" );
    if ( !cmod ) {
        /* Duplicate registrace - běh bez persistence. */
        return;
    }

    CFGELM *elm;
    elm = cfgmodule_register_new_element ( cmod, "active", CFGENTYPE_BOOL, 0 );
    cfgelement_set_propagate_cb ( elm, cfg_propagate_active, NULL );
    cfgelement_set_save_cb      ( elm, cfg_save_active,      NULL );

    cfgmodule_parse ( cmod );
    cfgmodule_propagate ( cmod );
}


void callstack_apply_cli_options ( void )
{
    /* CLI --callstack je FLAG (žádná hodnota). Pokud přítomen, vynutí
     * aktivaci nad INI hodnotou. */
    if ( sdlapp_option_present ( "--callstack" ) ) {
        callstack_set_active ( true );
    }
}
