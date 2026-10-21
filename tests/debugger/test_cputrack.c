/*
 * test_cputrack.c - unit testy pro trace-suite CPU tracking log
 *
 * Testuje:
 *  - cputrack_init / cputrack_start / cputrack_stop lifecycle
 *  - emit eventu - 12-byte layout v2 (PC + len + bytes + wait_clk + t_std)
 *  - HALT/self-loop collapse (1 event + akumulované wait_clk)
 *  - chunk + meta.json soubory vytvořené
 *  - initial RAM dump v hlavičce
 *  - subsys_header obsahuje initial_regs
 *
 * Test simuluje "vykonané instrukce" přímým voláním
 * cputrack_on_instruction_complete s nastaveným cpu->pc, aby šla
 * otestovat collapse logika i běžný emit bez nutnosti spouštět
 * mzarch_main_emulator_run.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

#include "debugger/trace/cputrack.h"
#include "debugger/trace/tlog_common.h"
#include "mzarch/mzarch.h"
#include "hw-generic/memory/memory.h"
#include "libs/cpu-z80/z80.h"
#include "json_test_helpers.h"


/* ================================================================
 * setUp / tearDown
 * ================================================================ */

static char *s_test_dir = NULL;


static int rmtree ( const char *path )
{
    if ( !path || !g_file_test ( path, G_FILE_TEST_EXISTS ) ) return 0;
    if ( g_file_test ( path, G_FILE_TEST_IS_DIR ) && !g_file_test ( path, G_FILE_TEST_IS_SYMLINK ) ) {
        GError *err = NULL;
        GDir *d = g_dir_open ( path, 0, &err );
        if ( !d ) {
            if ( err ) g_error_free ( err );
            return -1;
        }
        const gchar *name;
        while ( ( name = g_dir_read_name ( d ) ) != NULL ) {
            char *child = g_build_filename ( path, name, NULL );
            rmtree ( child );
            g_free ( child );
        }
        g_dir_close ( d );
        return g_rmdir ( path );
    }
    return g_remove ( path );
}


void setUp ( void )
{
    GError *err = NULL;
    s_test_dir = g_dir_make_tmp ( "mztest-cputrack-XXXXXX", &err );
    if ( !s_test_dir ) {
        if ( err ) g_error_free ( err );
        TEST_FAIL_MESSAGE ( "Nepodařilo se vytvořit tmpdir" );
        return;
    }

    /* Reset config a aktivace state */
    g_free ( g_cputrack_config.dir );
    g_free ( g_cputrack_config.name );
    g_cputrack_config.dir = g_strdup ( s_test_dir );
    g_cputrack_config.name = g_strdup ( "cput" );
    g_cputrack_config.chunk_mb = 1;
    g_cputrack_config.max_total_mb = 0;
    g_cputrack_config.save_on_exit = 1;
    g_cputrack_config.mode = TLOG_MODE_OFF;
    g_cputrack_active = 0;
}


void tearDown ( void )
{
    /* Pokud je writer otevřený, zavřít ho. */
    if ( g_cputrack_active ) {
        cputrack_stop ( );
        g_cputrack_active = 0;
    }

    if ( s_test_dir ) {
        rmtree ( s_test_dir );
        g_free ( s_test_dir );
        s_test_dir = NULL;
    }
}


/* ================================================================
 * Pomocné helpery
 * ================================================================ */

static uint8_t *read_whole_file ( const char *dir, const char *filename, size_t *out_size )
{
    char *path = g_build_filename ( dir, filename, NULL );
    FILE *fp = g_fopen ( path, "rb" );
    g_free ( path );
    if ( !fp ) {
        if ( out_size ) *out_size = 0;
        return NULL;
    }
    fseek ( fp, 0, SEEK_END );
    long size = ftell ( fp );
    fseek ( fp, 0, SEEK_SET );
    if ( size < 0 ) {
        fclose ( fp );
        if ( out_size ) *out_size = 0;
        return NULL;
    }
    uint8_t *buf = (uint8_t *) g_malloc ( (size_t) size + 1 );
    size_t n = fread ( buf, 1, (size_t) size, fp );
    buf[ n ] = 0;
    fclose ( fp );
    if ( out_size ) *out_size = n;
    return buf;
}


/* Parsování 12 B eventu. */
typedef struct {
    uint16_t pc;
    uint8_t  insn_len;
    uint8_t  insn_bytes[ 4 ];
    uint32_t wait_clk;
    uint8_t  t_std;
} parsed_event_t;


static void parse_event ( const uint8_t *buf, parsed_event_t *e )
{
    e->pc = (uint16_t) buf[ 0 ] | ( (uint16_t) buf[ 1 ] << 8 );
    e->insn_len = buf[ 2 ];
    e->insn_bytes[ 0 ] = buf[ 3 ];
    e->insn_bytes[ 1 ] = buf[ 4 ];
    e->insn_bytes[ 2 ] = buf[ 5 ];
    e->insn_bytes[ 3 ] = buf[ 6 ];
    e->wait_clk = (uint32_t) buf[ 7 ]
                | ( (uint32_t) buf[ 8 ] << 8 )
                | ( (uint32_t) buf[ 9 ] << 16 )
                | ( (uint32_t) buf[ 10 ] << 24 );
    e->t_std = buf[ 11 ];
}


/* Activate cputrack ručně - obejdeme TLOG_MODE_OFF default. */
static int activate_cputrack ( void )
{
    int rc = cputrack_start ( );
    if ( rc == 0 ) g_cputrack_active = 1;
    return rc;
}


/* ================================================================
 * Lifecycle
 * ================================================================ */

void test_cputrack_start_stop_basic ( void )
{
    TEST_ASSERT_EQUAL_INT_MESSAGE ( 0, activate_cputrack ( ),
                                    "cputrack_start musí uspět" );
    cputrack_stop ( );
    g_cputrack_active = 0;

    /* meta.json a initial dumpy musí existovat. */
    char *meta_path = g_build_filename ( s_test_dir, "cput.json", NULL );
    TEST_ASSERT_TRUE_MESSAGE ( g_file_test ( meta_path, G_FILE_TEST_IS_REGULAR ),
                               "cput.json musí být zapsán" );
    g_free ( meta_path );

    char *ram_path = g_build_filename ( s_test_dir, "cput_initial_ram.bin", NULL );
    TEST_ASSERT_TRUE_MESSAGE ( g_file_test ( ram_path, G_FILE_TEST_IS_REGULAR ),
                               "cput_initial_ram.bin musí být zapsán" );
    g_free ( ram_path );
}


/**
 * @brief Přepočet aktivity podle režimu přepne bit cputrack v masce háčků
 *        po instrukci (brána hlavní smyčky čte jen masku).
 */
void test_cputrack_recompute_sets_post_insn_mask ( void )
{
    g_cputrack_config.mode = TLOG_MODE_ALWAYS;
    cputrack_recompute_active ( 0 );
    TEST_ASSERT_EQUAL_INT ( 1, g_cputrack_active );
    TEST_ASSERT_TRUE ( g_mzarch_post_insn_hooks & MZARCH_POST_INSN_CPUTRACK );

    g_cputrack_config.mode = TLOG_MODE_OFF;
    cputrack_recompute_active ( 0 );
    TEST_ASSERT_EQUAL_INT ( 0, g_cputrack_active );
    TEST_ASSERT_FALSE ( g_mzarch_post_insn_hooks & MZARCH_POST_INSN_CPUTRACK );
}


void test_cputrack_start_idempotent ( void )
{
    TEST_ASSERT_EQUAL_INT ( 0, activate_cputrack ( ) );
    /* Druhé volání nesmí spadnout (vrací 0 = už aktivní). */
    TEST_ASSERT_EQUAL_INT ( 0, cputrack_start ( ) );
    cputrack_stop ( );
    g_cputrack_active = 0;
}


void test_cputrack_stop_idempotent ( void )
{
    TEST_ASSERT_EQUAL_INT ( 0, activate_cputrack ( ) );
    cputrack_stop ( );
    g_cputrack_active = 0;
    /* Druhé stop = no-op. */
    cputrack_stop ( );
}


/* ================================================================
 * Initial RAM dump v hlavičce
 * ================================================================ */

void test_cputrack_initial_ram_dump_size ( void )
{
    activate_cputrack ( );
    cputrack_stop ( );
    g_cputrack_active = 0;

    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "cput_initial_ram.bin", &fsize );
    TEST_ASSERT_NOT_NULL ( buf );
    TEST_ASSERT_EQUAL_size_t_MESSAGE ( MEMORY_SIZE_RAM, fsize,
                                       "RAM dump musí mít MEMORY_SIZE_RAM B" );
    g_free ( buf );
}


void test_cputrack_initial_vram_dump_exists ( void )
{
    activate_cputrack ( );
    cputrack_stop ( );
    g_cputrack_active = 0;

    char *p = g_build_filename ( s_test_dir, "cput_initial_vram.bin", NULL );
    TEST_ASSERT_TRUE_MESSAGE ( g_file_test ( p, G_FILE_TEST_IS_REGULAR ),
                               "VRAM dump musí existovat" );
    g_free ( p );
}


/* ================================================================
 * Header obsahuje initial_regs
 * ================================================================ */

void test_cputrack_meta_contains_initial_regs ( void )
{
    activate_cputrack ( );
    cputrack_stop ( );
    g_cputrack_active = 0;

    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "cput.json", &fsize );
    TEST_ASSERT_NOT_NULL ( buf );
    const char *s = (const char *) buf;

    TEST_ASSERT_NOT_NULL_MESSAGE ( strstr ( s, "\"initial_regs\"" ),
                                   "meta musí obsahovat initial_regs" );
    /* Klíčové registry. Pretty print json-glib formátuje "klíč" : value
     * (mezera před i po :), proto json_key_present místo strstr. */
    TEST_ASSERT_TRUE ( json_key_present ( s, "AF" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "BC" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "DE" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "HL" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "IX" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "IY" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "SP" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "PC" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "AF_alt" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "BC_alt" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "DE_alt" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "HL_alt" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "I" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "R" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "IM" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "IFF1" ) );
    TEST_ASSERT_TRUE ( json_key_present ( s, "IFF2" ) );

    TEST_ASSERT_TRUE_MESSAGE ( json_kv_present ( s, "event_record_size", "12" ),
                                   "event_record_size musí být 12" );
    TEST_ASSERT_TRUE_MESSAGE ( json_kv_present ( s, "event_record_version", "2" ),
                                   "event_record_version musí být 2 (bajt 11 = t_std)" );

    g_free ( buf );
}


/* ================================================================
 * Per-event 12 B layout
 * ================================================================ */

void test_cputrack_event_layout_basic ( void )
{
    activate_cputrack ( );

    /* Simulujeme jednu instrukci na PC=0x1234. Aby NEdošlo k collapse,
     * nastavíme cpu->pc na jinou hodnotu (= "PC po stepu se posunul"). */
    g_mzarch_main.cpu->pc = 0x1237;
    cputrack_on_instruction_complete ( 0x1234, 4, 0 );

    /* Druhá instrukce na PC=0x1237 - taky PC se posune. */
    g_mzarch_main.cpu->pc = 0x123A;
    cputrack_on_instruction_complete ( 0x1237, 4, 0 );

    cputrack_stop ( );
    g_cputrack_active = 0;

    /* Načíst chunk soubor. Po close se 24 B (2 events) flushnou. */
    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "cput.000.bin", &fsize );
    TEST_ASSERT_NOT_NULL_MESSAGE ( buf, "cput.000.bin musí existovat" );
    TEST_ASSERT_EQUAL_size_t_MESSAGE ( 24, fsize,
                                       "Po 2 events musí mít chunk 24 B" );

    parsed_event_t e0, e1;
    parse_event ( buf, &e0 );
    parse_event ( buf + 12, &e1 );

    TEST_ASSERT_EQUAL_HEX16_MESSAGE ( 0x1234, e0.pc, "PC eventu 0" );
    TEST_ASSERT_EQUAL_UINT32_MESSAGE ( 0, e0.wait_clk, "wait_clk = 0 pro běžnou instr" );
    TEST_ASSERT_EQUAL_UINT8_MESSAGE ( 4, e0.t_std, "bajt 11 (v2) = standardní T-stavy instrukce" );
    TEST_ASSERT_TRUE_MESSAGE ( e0.insn_len >= 1 && e0.insn_len <= 4,
                               "insn_len v rozsahu 1-4" );

    TEST_ASSERT_EQUAL_HEX16_MESSAGE ( 0x1237, e1.pc, "PC eventu 1" );

    g_free ( buf );
}


void test_cputrack_event_with_wait ( void )
{
    activate_cputrack ( );

    /* Instr s explicit wait_extra. */
    g_mzarch_main.cpu->pc = 0x2003;
    cputrack_on_instruction_complete ( 0x2000, 4, 7 );

    /* Druhá instr - posune PC, aby se předchozí emitnula. */
    g_mzarch_main.cpu->pc = 0x2006;
    cputrack_on_instruction_complete ( 0x2003, 4, 0 );

    cputrack_stop ( );
    g_cputrack_active = 0;

    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "cput.000.bin", &fsize );
    TEST_ASSERT_NOT_NULL ( buf );
    TEST_ASSERT_EQUAL_size_t ( 24, fsize );

    parsed_event_t e0;
    parse_event ( buf, &e0 );
    TEST_ASSERT_EQUAL_HEX16 ( 0x2000, e0.pc );
    TEST_ASSERT_EQUAL_UINT32_MESSAGE ( 7, e0.wait_clk,
                                       "wait_extra=7 musí být v eventu" );

    g_free ( buf );
}


void test_cputrack_event_wait_saturates_at_max ( void )
{
    /* Per michal.txt: "Pro WAIT bych dal uint 32b a pokud jej překročíme,
     * tak bude nastaven na full stav" => 0xFFFFFFFF sentinel. Dosažitelné
     * v praxi přes long HALT smyčku s mid-instr WAIT, nebo přímým passu
     * 0xFFFFFFFF do hooku pro non-collapse instr. */
    activate_cputrack ( );

    /* Instr s wait_extra = 0xFFFFFFFF (= explicit sentinel od caller). */
    g_mzarch_main.cpu->pc = 0x3003;
    cputrack_on_instruction_complete ( 0x3000, 4, 0xFFFFFFFFu );

    /* Posun PC aby se 0x3000 emitnulo. */
    g_mzarch_main.cpu->pc = 0x3006;
    cputrack_on_instruction_complete ( 0x3003, 4, 0 );

    cputrack_stop ( );
    g_cputrack_active = 0;

    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "cput.000.bin", &fsize );
    TEST_ASSERT_NOT_NULL ( buf );

    parsed_event_t e0;
    parse_event ( buf, &e0 );
    TEST_ASSERT_EQUAL_HEX16 ( 0x3000, e0.pc );
    TEST_ASSERT_EQUAL_HEX32_MESSAGE ( 0xFFFFFFFFu, e0.wait_clk,
                                      "wait_clk = 0xFFFFFFFF (saturated full state)" );

    g_free ( buf );
}


void test_cputrack_collapse_wait_extra_accumulates ( void )
{
    /* Self-loop s nenulovym wait_extra v každé iteraci - akumulace
     * by měla zahrnout jak insn_tstates, tak wait_extra. */
    activate_cputrack ( );

    /* Bootstrap - nějaká instr před smyčkou. */
    g_mzarch_main.cpu->pc = 0x6000;
    cputrack_on_instruction_complete ( 0x5FFD, 4, 0 );

    /* 5 iterací self-loop, každá insn_tstates=4, wait_extra=10 => total
     * pending_wait_clk = 5*(4+10) = 70. */
    for ( int i = 0; i < 5; i++ ) {
        g_mzarch_main.cpu->pc = 0x6000;
        cputrack_on_instruction_complete ( 0x6000, 4, 10 );
    }
    /* Exit smyčky. */
    g_mzarch_main.cpu->pc = 0x7000;
    cputrack_on_instruction_complete ( 0x6000, 4, 0 );

    cputrack_stop ( );
    g_cputrack_active = 0;

    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "cput.000.bin", &fsize );
    TEST_ASSERT_NOT_NULL ( buf );
    TEST_ASSERT_EQUAL_size_t ( 36, fsize );

    parsed_event_t e1;
    parse_event ( buf + 12, &e1 );
    TEST_ASSERT_EQUAL_HEX16 ( 0x6000, e1.pc );
    TEST_ASSERT_EQUAL_UINT32_MESSAGE ( 5 * (4 + 10), e1.wait_clk,
        "Collapsed event akumuluje insn_tstates + wait_extra per iteraci" );

    g_free ( buf );
}


/* ================================================================
 * HALT / self-loop collapse
 * ================================================================ */

/* v2: přijetí přerušení = záznam s insn_length 0 (pc obsluhy, t_std =
 * T-stavy přijetí); sbalená smyčka HALT, kterou přerušení ukončilo, se
 * zapíše před něj. */
void test_cputrack_interrupt_accept_record ( void )
{
    activate_cputrack ( );

    g_mzarch_main.cpu->pc = 0x2001;
    cputrack_on_instruction_complete ( 0x2000, 4, 0 );
    /* HALT na 2001h se opakuje (sbalí se) */
    for ( int i = 0; i < 10; i++ ) {
        g_mzarch_main.cpu->pc = 0x2001;
        cputrack_on_instruction_complete ( 0x2001, 4, 0 );
    }
    /* přijetí IM 2, obsluha na 3434h */
    cputrack_on_interrupt_accept ( 0x3434, 19 );
    g_mzarch_main.cpu->pc = 0x3435;
    cputrack_on_instruction_complete ( 0x3434, 4, 0 );

    cputrack_stop ( );
    g_cputrack_active = 0;

    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "cput.000.bin", &fsize );
    TEST_ASSERT_NOT_NULL ( buf );
    TEST_ASSERT_EQUAL_size_t_MESSAGE ( 48, fsize, "instr + sbalený HALT + přijetí + instr = 4 záznamy" );
    parsed_event_t e1, e2, e3;
    parse_event ( buf + 12, &e1 );
    parse_event ( buf + 24, &e2 );
    parse_event ( buf + 36, &e3 );
    TEST_ASSERT_EQUAL_HEX16 ( 0x2001, e1.pc );
    TEST_ASSERT_EQUAL_UINT32 ( 10 * 4, e1.wait_clk );
    TEST_ASSERT_EQUAL_HEX16_MESSAGE ( 0x3434, e2.pc, "přijetí: pc = adresa obsluhy" );
    TEST_ASSERT_EQUAL_UINT8_MESSAGE ( 0, e2.insn_len, "přijetí: insn_length 0" );
    TEST_ASSERT_EQUAL_UINT8_MESSAGE ( 19, e2.t_std, "přijetí: t_std = T-stavy přijetí" );
    TEST_ASSERT_EQUAL_UINT32 ( 0, e2.wait_clk );
    TEST_ASSERT_EQUAL_HEX16 ( 0x3434, e3.pc );
    TEST_ASSERT_EQUAL_UINT8 ( 4, e3.t_std );
    g_free ( buf );
}


void test_cputrack_self_loop_collapses_to_single_event ( void )
{
    activate_cputrack ( );

    /* Předchozí instr - posuneme stav a vytvoříme s_prev_pc_valid. */
    g_mzarch_main.cpu->pc = 0x3000;
    cputrack_on_instruction_complete ( 0x2FFD, 4, 0 );

    /* Self-loop: JR $-2 na 0x3000. PC po stepu zůstane = 0x3000.
     * 100 iterací musí dát 0 emitů (vše se akumuluje). */
    for ( int i = 0; i < 100; i++ ) {
        g_mzarch_main.cpu->pc = 0x3000;
        cputrack_on_instruction_complete ( 0x3000, 12, 0 );
    }

    /* Exit smyčky - PC se konečně posune. To emituje collapse event. */
    g_mzarch_main.cpu->pc = 0x4000;
    cputrack_on_instruction_complete ( 0x3000, 12, 0 );

    cputrack_stop ( );
    g_cputrack_active = 0;

    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "cput.000.bin", &fsize );
    TEST_ASSERT_NOT_NULL ( buf );

    /* Očekáváme 3 events:
     *   1. instr na 0x2FFD (před smyčkou)
     *   2. collapsed event na 0x3000 s wait_clk = 100*12 = 1200
     *   3. exit instr (= ta která změnila PC) - to je opět 0x3000
     *      (sama sebe nahradila volání s new_pc=0x4000 != pc=0x3000)
     * Pozor: implementace emit pro "exit" instrukci taky emit s wait=0,
     * ale je to stejná instr co byla v collapse. To je per design.
     */
    TEST_ASSERT_EQUAL_size_t_MESSAGE ( 36, fsize,
        "Self-loop collapse: 1 pre + 1 collapsed + 1 exit = 3 events = 36 B" );

    parsed_event_t e0, e1, e2;
    parse_event ( buf, &e0 );
    parse_event ( buf + 12, &e1 );
    parse_event ( buf + 24, &e2 );

    TEST_ASSERT_EQUAL_HEX16 ( 0x2FFD, e0.pc );
    TEST_ASSERT_EQUAL_HEX16_MESSAGE ( 0x3000, e1.pc,
                                      "Collapsed event musí mít PC smyčky" );
    TEST_ASSERT_EQUAL_UINT32_MESSAGE ( 100 * 12, e1.wait_clk,
                                       "Collapsed event akumuluje 100*12 T-states" );
    TEST_ASSERT_EQUAL_UINT8_MESSAGE ( 4, e0.t_std, "běžný záznam nese standardní T-stavy" );
    TEST_ASSERT_EQUAL_UINT8_MESSAGE ( 0, e1.t_std,
                                      "sbalený záznam má t_std 0 (čas je ve wait_clk)" );
    TEST_ASSERT_EQUAL_UINT8_MESSAGE ( 12, e2.t_std, "exit instrukce nese standardní T-stavy" );
    TEST_ASSERT_EQUAL_HEX16_MESSAGE ( 0x3000, e2.pc,
                                      "Exit instr emit s pc=collapse pc" );

    g_free ( buf );
}


void test_cputrack_no_collapse_when_pc_changes ( void )
{
    activate_cputrack ( );

    /* Sekvence růžných PC - žádný collapse. */
    uint16_t pcs[] = { 0x1000, 0x1003, 0x1006, 0x1009, 0x100C };
    /* Bootstrap - první instr nemá s_prev_pc_valid */
    g_mzarch_main.cpu->pc = pcs[ 0 ];
    cputrack_on_instruction_complete ( 0xFFFF, 4, 0 );  /* sentinel */

    for ( unsigned i = 0; i < G_N_ELEMENTS ( pcs ); i++ ) {
        uint16_t next = ( i + 1 < G_N_ELEMENTS ( pcs ) ) ? pcs[ i + 1 ] : 0x2000;
        g_mzarch_main.cpu->pc = next;
        cputrack_on_instruction_complete ( pcs[ i ], 4, 0 );
    }

    cputrack_stop ( );
    g_cputrack_active = 0;

    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "cput.000.bin", &fsize );
    TEST_ASSERT_NOT_NULL ( buf );
    /* sentinel + 5 = 6 eventů = 72 B. */
    TEST_ASSERT_EQUAL_size_t_MESSAGE ( 72, fsize,
                                       "Bez collapse 6 events = 72 B" );
    g_free ( buf );
}


void test_cputrack_collapse_state_reset ( void )
{
    activate_cputrack ( );

    /* Vytvoř collapse state. */
    g_mzarch_main.cpu->pc = 0x5000;
    cputrack_on_instruction_complete ( 0x4FFD, 4, 0 );
    g_mzarch_main.cpu->pc = 0x5000;
    cputrack_on_instruction_complete ( 0x5000, 12, 0 );
    g_mzarch_main.cpu->pc = 0x5000;
    cputrack_on_instruction_complete ( 0x5000, 12, 0 );

    /* Reset state. Po něm další instrukce se chová jako "první". */
    cputrack_reset_collapse_state ( );

    /* PC == 0x5000 znova (po resetu = bootstrap). */
    g_mzarch_main.cpu->pc = 0x5000;
    cputrack_on_instruction_complete ( 0x5000, 12, 0 );

    cputrack_stop ( );
    g_cputrack_active = 0;

    /* Očekávat: 1 instr před smyčkou (= 0x4FFD) + accumulated collapsed
     * event po stop_writeru (s_pending > 0 → emit při stop).
     * Test ověřuje hlavně že reset_collapse_state nepadne. */
    char *p = g_build_filename ( s_test_dir, "cput.000.bin", NULL );
    TEST_ASSERT_TRUE_MESSAGE ( g_file_test ( p, G_FILE_TEST_IS_REGULAR ),
                               "chunk soubor musí existovat" );
    g_free ( p );
}


/* ================================================================
 * MAIN
 * ================================================================ */

int main ( int argc, char *argv[] )
{
    mztest_parse_args ( argc, argv );
    mztest_init ( );

    UNITY_BEGIN ( );

    /* Lifecycle */
    RUN_TEST ( test_cputrack_start_stop_basic );
    RUN_TEST ( test_cputrack_start_idempotent );
    RUN_TEST ( test_cputrack_recompute_sets_post_insn_mask );
    RUN_TEST ( test_cputrack_stop_idempotent );

    /* Initial dumps + meta */
    RUN_TEST ( test_cputrack_initial_ram_dump_size );
    RUN_TEST ( test_cputrack_initial_vram_dump_exists );
    RUN_TEST ( test_cputrack_meta_contains_initial_regs );

    /* Per-event layout */
    RUN_TEST ( test_cputrack_event_layout_basic );
    RUN_TEST ( test_cputrack_event_with_wait );
    RUN_TEST ( test_cputrack_event_wait_saturates_at_max );

    /* Collapse logic */
    RUN_TEST ( test_cputrack_self_loop_collapses_to_single_event );
    RUN_TEST ( test_cputrack_interrupt_accept_record );
    RUN_TEST ( test_cputrack_no_collapse_when_pc_changes );
    RUN_TEST ( test_cputrack_collapse_state_reset );
    RUN_TEST ( test_cputrack_collapse_wait_extra_accumulates );

    int result = UNITY_END ( );

    mztest_teardown ( );
    return result;
}
