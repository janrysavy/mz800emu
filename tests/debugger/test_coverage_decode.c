/**
 * @file test_coverage_decode.c
 * @brief Testy dekodéru kroku Z80 pro pokrytí kódu (coverage_decode.h).
 *
 * Délky a výsledky podmínek se neověřují proti druhé tabulce, ale proti
 * skutečnému kroku jádra `libs/cpu-z80` (z80_step):
 * - u instrukce, která nemění tok řízení, musí PC po kroku být začátek
 *   kroku + délka z dekodéru (všechny opkódy bez prefixu, CB, ED, DD/FD,
 *   DD/FD CB a dvojice prefixů),
 * - u instrukce, která mění tok (podle z80_dasm() na prvním bajtu za
 *   prefixy), se délka porovná s disassemblerem,
 * - výsledek podmínky (JR/JP/CALL/RET cc pro všech 256 hodnot F, DJNZ,
 *   osm opakujících blokových instrukcí, i za prefixem DD) musí souhlasit
 *   s tím, kam jádro skočilo; past `JR Z,$+2` se vyhodnotí z F.
 *
 * @par Licence: GPLv3
 */

#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "libs/cpu-z80/z80.h"
#include "libs/dasm-z80/z80_dasm.h"
#include "emulator/debugger/coverage/coverage_decode.h"


/** @brief Adresa, na které testy skládají zkoumaný krok. */
#define TEST_PC 0x4000

static uint8_t g_mem[ 0x10000 ];
static z80_t *g_cpu;

static uint8_t test_mread ( struct z80_s *cpu, uint16_t addr, int m1, void *u )
{ (void) cpu; (void) m1; (void) u; return g_mem[ addr ]; }

static void test_mwrite ( struct z80_s *cpu, uint16_t addr, uint8_t v, void *u )
{ (void) cpu; (void) u; g_mem[ addr ] = v; }

static uint8_t test_pread ( struct z80_s *cpu, uint16_t port, void *u )
{ (void) cpu; (void) port; (void) u; return 0xFF; }

static void test_pwrite ( struct z80_s *cpu, uint16_t port, uint8_t v, void *u )
{ (void) cpu; (void) port; (void) v; (void) u; }

static uint8_t test_intread ( struct z80_s *cpu, void *u )
{ (void) cpu; (void) u; return 0xFF; }


void setUp ( void )
{
    memset ( g_mem, 0, sizeof ( g_mem ) );
    g_cpu = z80_create ( test_mread, NULL, test_mwrite, NULL,
                         test_pread, NULL, test_pwrite, NULL,
                         test_intread, NULL );
    TEST_ASSERT_NOT_NULL ( g_cpu );
    g_cpu->external_int_handling = true;
}


void tearDown ( void )
{
    z80_destroy ( g_cpu );
    g_cpu = NULL;
}


/* ===== Pomocné funkce ===== */

/** @brief Čtení pro dekodér: bajt kroku na TEST_PC + index z testovací paměti. */
static uint8_t read_mem ( void *ctx, unsigned index )
{
    (void) ctx;
    return g_mem[ (uint16_t) ( TEST_PC + index ) ];
}


/** @brief Čtení pro z80_dasm() z testovací paměti. */
static uint8_t dasm_read ( uint16_t addr, void *u )
{
    (void) u;
    return g_mem[ addr ];
}


/**
 * @brief Připraví paměť a registry pro krok: bajty na TEST_PC, operandy 12h 34h 56h.
 *
 * Registry volí cíle, které se liší od adresy za instrukcí: HL = IX = IY =
 * 8000h, SP = F000h se zásobníkem 5678h, BC = 0001h (bloková instrukce
 * neopakuje), A = 00h.
 *
 * @param bytes Bajty kroku.
 * @param n     Počet bajtů.
 */
static void put_step ( const uint8_t *bytes, unsigned n )
{
    memset ( g_mem, 0, sizeof ( g_mem ) );
    memcpy ( &g_mem[ TEST_PC ], bytes, n );
    g_mem[ TEST_PC + n ] = 0x12;
    g_mem[ TEST_PC + n + 1 ] = 0x34;
    g_mem[ TEST_PC + n + 2 ] = 0x56;
    g_mem[ 0xF000 ] = 0x78;
    g_mem[ 0xF001 ] = 0x56;
    z80_reset ( g_cpu );
    g_cpu->external_int_handling = true;
    g_cpu->pc = TEST_PC;
    g_cpu->sp = 0xF000;
    g_cpu->hl.w = 0x8000;
    g_cpu->ix.w = 0x8000;
    g_cpu->iy.w = 0x8000;
    g_cpu->bc.w = 0x0001;
    g_cpu->af.w = 0x0000;
}


/**
 * @brief Ověří délku kroku proti jádru, nebo u změny toku proti disassembleru.
 *
 * @param bytes Bajty kroku (prefixy a opkód, operandy doplní put_step()).
 * @param n     Počet bajtů v @p bytes.
 */
static void check_len ( const uint8_t *bytes, unsigned n )
{
    char msg[ 96 ];
    snprintf ( msg, sizeof ( msg ), "bytes %02X %02X %02X %02X n=%u",
               bytes[ 0 ], n > 1 ? bytes[ 1 ] : 0, n > 2 ? bytes[ 2 ] : 0,
               n > 3 ? bytes[ 3 ] : 0, n );

    put_step ( bytes, n );
    st_COVERAGE_INSN insn;
    coverage_decode ( read_mem, NULL, &insn );
    TEST_ASSERT_TRUE_MESSAGE ( insn.len >= 1 && insn.op_len <= insn.len, msg );
    TEST_ASSERT_TRUE_MESSAGE ( insn.prefixes <= insn.op_len, msg );

    /* Tok řízení podle disassembleru na první instrukci za prefixy. */
    z80_dasm_inst_t di;
    int base_len = z80_dasm ( &di, dasm_read, NULL, (uint16_t) ( TEST_PC + insn.prefixes ) );

    if ( insn.cond == COVERAGE_COND_REPEAT ) {
        /* Poslední iterace: LDxR/CPxR počítají BC, INxR/OTxR jen B. */
        uint8_t ed_op = g_mem[ TEST_PC + insn.prefixes + 1 ];
        g_cpu->bc.w = ( ed_op & 0x02 ) ? 0x0101 : 0x0001;
    };

    z80_step ( g_cpu );
    uint16_t next_pc = (uint16_t) g_cpu->pc;

    if ( insn.halt ) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE ( 0x76, g_mem[ TEST_PC + insn.prefixes ], msg );
        return;
    };
    if ( di.flow == Z80_FLOW_NORMAL ) {
        TEST_ASSERT_EQUAL_HEX16_MESSAGE ( (uint16_t) ( TEST_PC + insn.len ), next_pc, msg );
    } else {
        TEST_ASSERT_EQUAL_INT_MESSAGE ( insn.prefixes + base_len, insn.len, msg );
    };
}


/* ===== Délky ===== */

/** @brief Opkódy bez prefixu, CB a ED proti jádru a disassembleru. */
void test_len_unprefixed_cb_ed ( void )
{
    for ( unsigned op = 0; op < 256; op++ ) {
        if ( op == 0xDD || op == 0xFD ) continue;
        if ( op == 0xCB || op == 0xED ) {
            for ( unsigned op2 = 0; op2 < 256; op2++ ) {
                uint8_t b[ 2 ] = { (uint8_t) op, (uint8_t) op2 };
                check_len ( b, 2 );
            };
        } else {
            uint8_t b[ 1 ] = { (uint8_t) op };
            check_len ( b, 1 );
        };
    };
}


/** @brief DD/FD + každý bajt (IX/IY instrukce i ignorovaný prefix), DD/FD CB d op. */
void test_len_index_prefix ( void )
{
    static const uint8_t prefixes[] = { 0xDD, 0xFD };
    for ( unsigned k = 0; k < 2; k++ ) {
        for ( unsigned op = 0; op < 256; op++ ) {
            if ( op == 0xDD || op == 0xFD ) continue;
            if ( op == 0xCB ) {
                for ( unsigned op2 = 0; op2 < 256; op2++ ) {
                    uint8_t b[ 4 ] = { prefixes[ k ], 0xCB, 0x05, (uint8_t) op2 };
                    check_len ( b, 4 );
                };
            } else if ( op == 0xED ) {
                for ( unsigned op2 = 0; op2 < 256; op2++ ) {
                    uint8_t b[ 3 ] = { prefixes[ k ], 0xED, (uint8_t) op2 };
                    check_len ( b, 3 );
                };
            } else {
                uint8_t b[ 2 ] = { prefixes[ k ], (uint8_t) op };
                check_len ( b, 2 );
            };
        };
    };
}


/** @brief Řetěz prefixů DD FD a FD DD: platí poslední prefix, jeden krok. */
void test_len_prefix_pairs ( void )
{
    for ( unsigned op = 0; op < 256; op++ ) {
        if ( op == 0xDD || op == 0xFD || op == 0xCB || op == 0xED ) continue;
        uint8_t b1[ 3 ] = { 0xDD, 0xFD, (uint8_t) op };
        check_len ( b1, 3 );
        uint8_t b2[ 3 ] = { 0xFD, 0xDD, (uint8_t) op };
        check_len ( b2, 3 );
    };
}


/** @brief Strop řetězu: 64 prefixů + NOP je jeden krok, 65 prefixů se rozdělí. */
void test_len_prefix_chain_limit ( void )
{
    uint8_t b[ 80 ];
    memset ( b, 0xDD, sizeof ( b ) );

    b[ 64 ] = 0x00;
    put_step ( b, 65 );
    st_COVERAGE_INSN insn;
    coverage_decode ( read_mem, NULL, &insn );
    TEST_ASSERT_EQUAL_UINT8 ( 65, insn.len );
    TEST_ASSERT_EQUAL_UINT8 ( 64, insn.prefixes );
    TEST_ASSERT_FALSE ( insn.prefix_split );
    z80_step ( g_cpu );
    TEST_ASSERT_EQUAL_HEX16 ( TEST_PC + 65, g_cpu->pc );

    memset ( b, 0xDD, sizeof ( b ) );
    put_step ( b, 70 );
    coverage_decode ( read_mem, NULL, &insn );
    TEST_ASSERT_TRUE ( insn.prefix_split );
    TEST_ASSERT_EQUAL_UINT8 ( 64, insn.len );
    TEST_ASSERT_EQUAL_UINT8 ( COVERAGE_COND_NONE, insn.cond );
    z80_step ( g_cpu );
    TEST_ASSERT_EQUAL_HEX16 ( TEST_PC + 64, g_cpu->pc );
}


/** @brief Rozložení opkódu a operandů (klíč verze kódu). */
void test_op_layout ( void )
{
    st_COVERAGE_INSN insn;

    static const uint8_t ld_hl[] = { 0x21, 0x34, 0x12 };
    coverage_decode_bytes ( ld_hl, 3, &insn );
    TEST_ASSERT_EQUAL_UINT8 ( 3, insn.len );
    TEST_ASSERT_EQUAL_UINT8 ( 1, insn.op_len );
    TEST_ASSERT_FALSE ( insn.op_last );

    static const uint8_t ld_ix[] = { 0xDD, 0x36, 0x05, 0x99 };
    coverage_decode_bytes ( ld_ix, 4, &insn );
    TEST_ASSERT_EQUAL_UINT8 ( 4, insn.len );
    TEST_ASSERT_EQUAL_UINT8 ( 2, insn.op_len );
    TEST_ASSERT_EQUAL_UINT8 ( 1, insn.prefixes );

    static const uint8_t bit_ix[] = { 0xFD, 0xCB, 0x05, 0x46 };
    coverage_decode_bytes ( bit_ix, 4, &insn );
    TEST_ASSERT_EQUAL_UINT8 ( 4, insn.len );
    TEST_ASSERT_EQUAL_UINT8 ( 2, insn.op_len );
    TEST_ASSERT_TRUE ( insn.op_last );

    static const uint8_t ld_nn_de[] = { 0xED, 0x53, 0x00, 0x80 };
    coverage_decode_bytes ( ld_nn_de, 4, &insn );
    TEST_ASSERT_EQUAL_UINT8 ( 4, insn.len );
    TEST_ASSERT_EQUAL_UINT8 ( 2, insn.op_len );

    static const uint8_t ign_jp[] = { 0xDD, 0xC3, 0x00, 0x80 };
    coverage_decode_bytes ( ign_jp, 4, &insn );
    TEST_ASSERT_EQUAL_UINT8 ( 4, insn.len );
    TEST_ASSERT_EQUAL_UINT8 ( 2, insn.op_len );

    static const uint8_t halt[] = { 0xFD, 0x76 };
    coverage_decode_bytes ( halt, 2, &insn );
    TEST_ASSERT_TRUE ( insn.halt );
}


/* ===== Výsledek podmínky ===== */

/**
 * @brief Provede krok na TEST_PC s daným F a ověří výsledek z dekodéru proti jádru.
 *
 * Skutečný výsledek: PC po kroku se liší od adresy za instrukcí (cíle volí
 * put_step() a operand 12h jinde).
 *
 * @param bytes Bajty kroku.
 * @param n     Počet bajtů.
 * @param f     Registr F před krokem.
 * @param b     Registr B před krokem.
 * @return Výsledek z dekodéru.
 */
static en_COVERAGE_OUTCOME run_cond ( const uint8_t *bytes, unsigned n, uint8_t f, uint8_t b )
{
    put_step ( bytes, n );
    g_cpu->af.l = f;
    g_cpu->bc.h = b;
    st_COVERAGE_INSN insn;
    coverage_decode ( read_mem, NULL, &insn );
    z80_step ( g_cpu );
    uint16_t next_pc = (uint16_t) g_cpu->pc;
    en_COVERAGE_OUTCOME o = coverage_decode_outcome ( &insn, TEST_PC, next_pc,
                                                      g_cpu->af.l, g_cpu->bc.h );
    bool actual = ( next_pc != (uint16_t) ( TEST_PC + insn.len ) );

    char msg[ 64 ];
    snprintf ( msg, sizeof ( msg ), "%02X %02X f=%02X b=%02X", bytes[ 0 ], n > 1 ? bytes[ 1 ] : 0, f, b );
    TEST_ASSERT_NOT_EQUAL_MESSAGE ( COVERAGE_OUTCOME_NONE, o, msg );
    TEST_ASSERT_EQUAL_INT_MESSAGE ( actual ? COVERAGE_OUTCOME_TAKEN : COVERAGE_OUTCOME_NOT_TAKEN, o, msg );
    return o;
}


/** @brief JR cc, JP cc, CALL cc, RET cc pro všech 256 hodnot F, bez prefixu i za DD. */
void test_outcome_flag_conditions ( void )
{
    static const uint8_t ops[] = {
        0x20, 0x28, 0x30, 0x38,
        0xC0, 0xC8, 0xD0, 0xD8, 0xE0, 0xE8, 0xF0, 0xF8,
        0xC2, 0xCA, 0xD2, 0xDA, 0xE2, 0xEA, 0xF2, 0xFA,
        0xC4, 0xCC, 0xD4, 0xDC, 0xE4, 0xEC, 0xF4, 0xFC,
    };
    unsigned taken = 0, not_taken = 0;
    for ( size_t i = 0; i < sizeof ( ops ); i++ ) {
        for ( unsigned f = 0; f < 256; f++ ) {
            uint8_t b1[ 1 ] = { ops[ i ] };
            en_COVERAGE_OUTCOME o = run_cond ( b1, 1, (uint8_t) f, 0 );
            ( o == COVERAGE_OUTCOME_TAKEN ) ? taken++ : not_taken++;
            uint8_t b2[ 2 ] = { 0xDD, ops[ i ] };
            run_cond ( b2, 2, (uint8_t) f, 0 );
        };
    };
    /* Každá podmínka platí právě pro polovinu hodnot F. */
    TEST_ASSERT_EQUAL_UINT ( sizeof ( ops ) * 128, taken );
    TEST_ASSERT_EQUAL_UINT ( sizeof ( ops ) * 128, not_taken );
}


/** @brief `JR Z,$+2` (posun 0): obě větve končí na stejném PC, výsledek je z F. */
void test_outcome_jr_zero_offset ( void )
{
    static const uint8_t jrz0[] = { 0x28, 0x00 };
    st_COVERAGE_INSN insn;
    coverage_decode_bytes ( jrz0, 2, &insn );
    TEST_ASSERT_EQUAL_INT ( COVERAGE_OUTCOME_TAKEN,
                            coverage_decode_outcome ( &insn, TEST_PC, TEST_PC + 2, 0x40, 0 ) );
    TEST_ASSERT_EQUAL_INT ( COVERAGE_OUTCOME_NOT_TAKEN,
                            coverage_decode_outcome ( &insn, TEST_PC, TEST_PC + 2, 0x00, 0 ) );

    /* Totéž přes jádro: PC po kroku je v obou případech TEST_PC + 2. */
    put_step ( jrz0, 2 );
    g_mem[ TEST_PC + 1 ] = 0x00;
    g_cpu->af.l = 0x40;
    z80_step ( g_cpu );
    TEST_ASSERT_EQUAL_HEX16 ( TEST_PC + 2, g_cpu->pc );
    TEST_ASSERT_EQUAL_INT ( COVERAGE_OUTCOME_TAKEN,
                            coverage_decode_outcome ( &insn, TEST_PC, (uint16_t) g_cpu->pc,
                                                      g_cpu->af.l, g_cpu->bc.h ) );
}


/** @brief DJNZ: B 2 -> skok, B 1 -> propad, B 0 -> skok (B = FFh). */
void test_outcome_djnz ( void )
{
    static const uint8_t djnz[] = { 0x10 };
    TEST_ASSERT_EQUAL_INT ( COVERAGE_OUTCOME_TAKEN, run_cond ( djnz, 1, 0, 2 ) );
    TEST_ASSERT_EQUAL_INT ( COVERAGE_OUTCOME_NOT_TAKEN, run_cond ( djnz, 1, 0, 1 ) );
    TEST_ASSERT_EQUAL_INT ( COVERAGE_OUTCOME_TAKEN, run_cond ( djnz, 1, 0, 0 ) );
    static const uint8_t dd_djnz[] = { 0xDD, 0x10 };
    TEST_ASSERT_EQUAL_INT ( COVERAGE_OUTCOME_TAKEN, run_cond ( dd_djnz, 2, 0, 2 ) );
}


/**
 * @brief Opakující blokové instrukce: další iterace = TAKEN, poslední = NOT_TAKEN.
 *
 * Čítač 2 -> opakuje se, 1 -> konec (LDxR/CPxR počítají BC, INxR/OTxR
 * jen B). CPIR/CPDR skončí i shodou A s (HL). Za prefixem DD vrací jádro
 * PC na EDh, ne na DD.
 */
void test_outcome_block_repeat ( void )
{
    static const uint8_t ops[] = { 0xB0, 0xB1, 0xB2, 0xB3, 0xB8, 0xB9, 0xBA, 0xBB };
    for ( size_t i = 0; i < sizeof ( ops ); i++ ) {
        bool io = ( ops[ i ] & 0x02 ) != 0;
        for ( unsigned pre = 0; pre < 2; pre++ ) {
            uint8_t b[ 3 ];
            unsigned n = 0;
            if ( pre ) b[ n++ ] = 0xDD;
            b[ n++ ] = 0xED;
            b[ n++ ] = ops[ i ];

            put_step ( b, n );
            g_cpu->bc.w = io ? 0x0200 : 0x0002;
            g_cpu->af.h = 0x55;   /* CPxR: (HL) = 00h, bez shody */
            st_COVERAGE_INSN insn;
            coverage_decode ( read_mem, NULL, &insn );
            TEST_ASSERT_EQUAL_INT ( COVERAGE_COND_REPEAT, insn.cond );
            z80_step ( g_cpu );
            TEST_ASSERT_EQUAL_HEX16 ( TEST_PC + pre, g_cpu->pc );
            TEST_ASSERT_EQUAL_INT ( COVERAGE_OUTCOME_TAKEN,
                                    coverage_decode_outcome ( &insn, TEST_PC, (uint16_t) g_cpu->pc,
                                                              g_cpu->af.l, g_cpu->bc.h ) );

            put_step ( b, n );
            g_cpu->bc.w = io ? 0x0100 : 0x0001;
            g_cpu->af.h = 0x55;
            z80_step ( g_cpu );
            TEST_ASSERT_EQUAL_HEX16 ( TEST_PC + insn.len, g_cpu->pc );
            TEST_ASSERT_EQUAL_INT ( COVERAGE_OUTCOME_NOT_TAKEN,
                                    coverage_decode_outcome ( &insn, TEST_PC, (uint16_t) g_cpu->pc,
                                                              g_cpu->af.l, g_cpu->bc.h ) );
        };
    };

    /* CPIR se shodou v prvním bajtu skončí i při BC > 1. */
    static const uint8_t cpir[] = { 0xED, 0xB1 };
    put_step ( cpir, 2 );
    g_cpu->bc.w = 0x0010;
    g_cpu->af.h = 0x00;
    st_COVERAGE_INSN insn;
    coverage_decode ( read_mem, NULL, &insn );
    z80_step ( g_cpu );
    TEST_ASSERT_EQUAL_INT ( COVERAGE_OUTCOME_NOT_TAKEN,
                            coverage_decode_outcome ( &insn, TEST_PC, (uint16_t) g_cpu->pc,
                                                      g_cpu->af.l, g_cpu->bc.h ) );
}


/** @brief Nepodmíněné instrukce vrací NONE (JP, JP (HL), RET, RST, CALL, NOP). */
void test_outcome_unconditional ( void )
{
    static const uint8_t ops[] = { 0x00, 0xC3, 0xE9, 0xC9, 0xFF, 0xCD, 0x18 };
    for ( size_t i = 0; i < sizeof ( ops ); i++ ) {
        st_COVERAGE_INSN insn;
        coverage_decode_bytes ( &ops[ i ], 1, &insn );
        TEST_ASSERT_EQUAL_INT ( COVERAGE_COND_NONE, insn.cond );
        TEST_ASSERT_EQUAL_INT ( COVERAGE_OUTCOME_NONE,
                                coverage_decode_outcome ( &insn, TEST_PC, 0x1234, 0xFF, 0xFF ) );
    };
}


int main ( void )
{
    UNITY_BEGIN ( );

    RUN_TEST ( test_len_unprefixed_cb_ed );
    RUN_TEST ( test_len_index_prefix );
    RUN_TEST ( test_len_prefix_pairs );
    RUN_TEST ( test_len_prefix_chain_limit );
    RUN_TEST ( test_op_layout );
    RUN_TEST ( test_outcome_flag_conditions );
    RUN_TEST ( test_outcome_jr_zero_offset );
    RUN_TEST ( test_outcome_djnz );
    RUN_TEST ( test_outcome_block_repeat );
    RUN_TEST ( test_outcome_unconditional );

    return UNITY_END ( );
}
