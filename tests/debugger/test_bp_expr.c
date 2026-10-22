/*
 * test_bp_expr.c - testy expression evaluatoru (D.1.2)
 *
 * Pokrývá lexer (literály, disambig %), parser (precedence, závorky,
 * fn calls), evaluator (operátory, short-circuit, deref, registry,
 * built-ins, kontextové proměnné).
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include "debugger/bp_expr.h"
#include "debugger/breakpoints.h"
#include "libs/cpu-z80/z80.h"
#include "mzarch/mzarch.h"

/* V1.5 fáze 2.4 - test že breakpoints_fill_global_ctx čerpá z g_gdg.
 * Per-arch include matchne breakpoints.c. */
#include "mzarch/mzarch_config.h"
#if MZARCH == 800
#include "mzarch/mz800/gdg/mz800_gdg.h"
#elif MZARCH == 1500
#include "mzarch/mz1500/gdg/mz1500_gdg.h"
#endif

#include <string.h>
#include <stdio.h>


/* ========================================================================= */
/*  Pomůcky                                                                  */
/* ========================================================================= */


static z80_t g_test_cpu;


/**
 * @brief Inicializuje fake CPU strukturu pro testy register access.
 */
static void test_cpu_init ( void ) {
    memset ( &g_test_cpu, 0, sizeof ( g_test_cpu ) );
    g_test_cpu.af.h = 0x12;
    g_test_cpu.af.l = 0x34;
    g_test_cpu.bc.h = 0x56;
    g_test_cpu.bc.l = 0x78;
    g_test_cpu.de.h = 0x9A;
    g_test_cpu.de.l = 0xBC;
    g_test_cpu.hl.h = 0xDE;
    g_test_cpu.hl.l = 0xF0;
    g_test_cpu.ix.h = 0x11;  /* IX = 0x1122 */
    g_test_cpu.ix.l = 0x22;
    g_test_cpu.iy.h = 0x33;  /* IY = 0x3344 */
    g_test_cpu.iy.l = 0x44;
    g_test_cpu.sp = 0xFFFE;
    g_test_cpu.pc = 0x4000;
    g_test_cpu.i = 0xAA;
    g_test_cpu.r = 0xBB;
    g_test_cpu.im = 2;
    g_test_cpu.iff1 = 1;
    g_test_cpu.iff2 = 0;
    g_test_cpu.af2.w = 0xCAFE;
    g_test_cpu.bc2.w = 0xBABE;
    g_test_cpu.de2.w = 0xDEAD;
    g_test_cpu.hl2.w = 0xBEEF;
}


/* Vyhodnotí výraz s implicitním ctx, vrátí int hodnotu. */
static int32_t eval_simple ( const char *src ) {
    bp_expr_ctx_t ctx;
    bp_expr_ctx_zero ( &ctx );
    int32_t v = 0;
    char err[128] = {0};
    bool ok = bp_expr_parse_eval ( src, &ctx, &v, err, sizeof ( err ) );
    if ( !ok ) {
        printf ( "PARSE ERR for '%s': %s\n", src, err );
    };
    return v;
}


/* Vyhodnotí výraz s nastaveným CPU kontextem. */
static int32_t eval_cpu ( const char *src ) {
    bp_expr_ctx_t ctx;
    bp_expr_ctx_zero ( &ctx );
    ctx.cpu = &g_test_cpu;
    int32_t v = 0;
    char err[128] = {0};
    bool ok = bp_expr_parse_eval ( src, &ctx, &v, err, sizeof ( err ) );
    if ( !ok ) {
        printf ( "PARSE ERR for '%s': %s\n", src, err );
    };
    return v;
}


/* Ověří, že parse selže a errbuf se naplní. */
static void assert_parse_fails ( const char *src ) {
    char err[128] = {0};
    bp_expr_t *e = bp_expr_parse ( src, err, sizeof ( err ) );
    if ( e ) {
        printf ( "PARSE UNEXPECTEDLY OK: '%s'\n", src );
    };
    TEST_ASSERT_NULL ( e );
    TEST_ASSERT_TRUE ( err[0] != '\0' );
}


void setUp ( void ) {
    test_cpu_init ( );
}


void tearDown ( void ) {
}


/* ========================================================================= */
/*  Lexer / literály                                                         */
/* ========================================================================= */


void test_literal_dec ( void ) {
    TEST_ASSERT_EQUAL_INT32 ( 0,    eval_simple ( "0" ) );
    TEST_ASSERT_EQUAL_INT32 ( 42,   eval_simple ( "42" ) );
    TEST_ASSERT_EQUAL_INT32 ( 1234, eval_simple ( "1234" ) );
}


void test_literal_hex_0x ( void ) {
    TEST_ASSERT_EQUAL_INT32 ( 0x10,    eval_simple ( "0x10" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0xFF,    eval_simple ( "0xFF" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0xCafe,  eval_simple ( "0xCafe" ) );
    /* 0X uppercase */
    TEST_ASSERT_EQUAL_INT32 ( 0xABCD,  eval_simple ( "0XABCD" ) );
}


void test_literal_hex_hash ( void ) {
    TEST_ASSERT_EQUAL_INT32 ( 0x10,   eval_simple ( "#10" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0xFFFF, eval_simple ( "#FFFF" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0xCD,   eval_simple ( "#CD" ) );
}


void test_literal_bin ( void ) {
    TEST_ASSERT_EQUAL_INT32 ( 0,    eval_simple ( "%0" ) );
    TEST_ASSERT_EQUAL_INT32 ( 1,    eval_simple ( "%1" ) );
    TEST_ASSERT_EQUAL_INT32 ( 10,   eval_simple ( "%1010" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0xFF, eval_simple ( "%11111111" ) );
}


void test_literal_lex_disambig_percent ( void ) {
    /* % po hodnotě = modulo */
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_simple ( "10 % 3" ) );
    TEST_ASSERT_EQUAL_INT32 ( 2, eval_simple ( "5 % 3" ) );
    /* % na začátku / po operátoru = bin literal */
    TEST_ASSERT_EQUAL_INT32 ( 0xF, eval_simple ( "%1111" ) );
    TEST_ASSERT_EQUAL_INT32 ( 5, eval_simple ( "0 + %101" ) );
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_simple ( "%11 == 3" ) );
}


/* ========================================================================= */
/*  Operátory + precedence                                                   */
/* ========================================================================= */


void test_arith_basic ( void ) {
    TEST_ASSERT_EQUAL_INT32 ( 5, eval_simple ( "2 + 3" ) );
    TEST_ASSERT_EQUAL_INT32 ( 6, eval_simple ( "2 * 3" ) );
    TEST_ASSERT_EQUAL_INT32 ( 4, eval_simple ( "10 - 6" ) );
    TEST_ASSERT_EQUAL_INT32 ( 4, eval_simple ( "20 / 5" ) );
    TEST_ASSERT_EQUAL_INT32 ( 2, eval_simple ( "8 % 3" ) );
}


void test_precedence_mul_over_add ( void ) {
    /* 1 + 2 * 3 = 7 (ne 9) */
    TEST_ASSERT_EQUAL_INT32 ( 7, eval_simple ( "1 + 2 * 3" ) );
    TEST_ASSERT_EQUAL_INT32 ( 7, eval_simple ( "2 * 3 + 1" ) );
    /* (1+2)*3 = 9 */
    TEST_ASSERT_EQUAL_INT32 ( 9, eval_simple ( "(1 + 2) * 3" ) );
}


void test_precedence_shift_below_arith ( void ) {
    /* 1 + 2 << 1 = (1+2) << 1 = 6 */
    TEST_ASSERT_EQUAL_INT32 ( 6, eval_simple ( "1 + 2 << 1" ) );
    /* 1 << 2 + 1 = 1 << (2+1) = 8 */
    TEST_ASSERT_EQUAL_INT32 ( 8, eval_simple ( "1 << 2 + 1" ) );
}


void test_precedence_relational ( void ) {
    /* 1 + 2 == 3 → true (= relational níž než arith) */
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_simple ( "1 + 2 == 3" ) );
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_simple ( "5 > 3" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0, eval_simple ( "5 < 3" ) );
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_simple ( "5 >= 5" ) );
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_simple ( "5 <= 5" ) );
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_simple ( "5 != 4" ) );
}


void test_precedence_bitwise ( void ) {
    /* & má vyšší prioritu než | */
    TEST_ASSERT_EQUAL_INT32 ( 0xF1, eval_simple ( "0xF0 | 1 & 3" ) );
    /* (0xF0 | (1 & 3)) = 0xF0 | 1 = 0xF1 */
    /* ^ má prioritu mezi & a | */
    TEST_ASSERT_EQUAL_INT32 ( 0x6, eval_simple ( "0x3 ^ 0x5" ) );
}


void test_unary ( void ) {
    TEST_ASSERT_EQUAL_INT32 ( -5, eval_simple ( "-5" ) );
    TEST_ASSERT_EQUAL_INT32 ( 5,  eval_simple ( "+5" ) );
    TEST_ASSERT_EQUAL_INT32 ( -1, eval_simple ( "~0" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0,  eval_simple ( "!1" ) );
    TEST_ASSERT_EQUAL_INT32 ( 1,  eval_simple ( "!0" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0,  eval_simple ( "!42" ) );
    /* -(2+3) = -5 */
    TEST_ASSERT_EQUAL_INT32 ( -5, eval_simple ( "-(2+3)" ) );
}


void test_shifts ( void ) {
    TEST_ASSERT_EQUAL_INT32 ( 8,    eval_simple ( "1 << 3" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0x10, eval_simple ( "0x100 >> 4" ) );
}


void test_div_by_zero_returns_zero ( void ) {
    /* Tolerantní eval - dělení nulou = 0 (NEcrash). */
    TEST_ASSERT_EQUAL_INT32 ( 0, eval_simple ( "10 / 0" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0, eval_simple ( "10 % 0" ) );
}


/* ========================================================================= */
/*  Short-circuit && / ||                                                    */
/* ========================================================================= */


void test_logical_and_basic ( void ) {
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_simple ( "1 && 1" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0, eval_simple ( "1 && 0" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0, eval_simple ( "0 && 1" ) );
}


void test_logical_or_basic ( void ) {
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_simple ( "1 || 0" ) );
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_simple ( "0 || 1" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0, eval_simple ( "0 || 0" ) );
}


void test_short_circuit_and_skips_rhs ( void ) {
    /* Pokud LHS = 0, RHS se NEvyhodnocuje. Test: "0 && (10/0)" by
     * normálně projevil eager eval (= chyba na 10/0 ale tolerovaná),
     * sledujeme jen že výsledek je 0 a nehavaruje. */
    TEST_ASSERT_EQUAL_INT32 ( 0, eval_simple ( "0 && (1/0)" ) );
    /* Naopak true && X provede X */
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_simple ( "1 && 1" ) );
}


void test_short_circuit_or_skips_rhs ( void ) {
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_simple ( "1 || (1/0)" ) );
}


void test_logical_precedence_below_bitwise ( void ) {
    /* (a == b) && (c == d) - parsing musí "==" před "&&". */
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_simple ( "1 == 1 && 2 == 2" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0, eval_simple ( "1 == 1 && 2 == 3" ) );
    /* && má vyšší prioritu než || */
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_simple ( "0 && 0 || 1" ) );
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_simple ( "1 || 0 && 0" ) );
}


/* ========================================================================= */
/*  Z80 registry                                                             */
/* ========================================================================= */


void test_z80_reg_8bit ( void ) {
    TEST_ASSERT_EQUAL_INT32 ( 0x12, eval_cpu ( "A" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0x34, eval_cpu ( "F" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0x56, eval_cpu ( "B" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0x78, eval_cpu ( "C" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0x9A, eval_cpu ( "D" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0xBC, eval_cpu ( "E" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0xDE, eval_cpu ( "H" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0xF0, eval_cpu ( "L" ) );
}


void test_z80_reg_16bit ( void ) {
    TEST_ASSERT_EQUAL_INT32 ( 0x1234, eval_cpu ( "AF" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0x5678, eval_cpu ( "BC" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0x9ABC, eval_cpu ( "DE" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0xDEF0, eval_cpu ( "HL" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0x1122, eval_cpu ( "IX" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0x3344, eval_cpu ( "IY" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0xFFFE, eval_cpu ( "SP" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0x4000, eval_cpu ( "PC" ) );
}


void test_z80_reg_case_insensitive ( void ) {
    /* Lowercase, uppercase, mixed - vše stejné. */
    TEST_ASSERT_EQUAL_INT32 ( 0x12, eval_cpu ( "a" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0x12, eval_cpu ( "A" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0x5678, eval_cpu ( "bc" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0x5678, eval_cpu ( "Bc" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0x4000, eval_cpu ( "pc" ) );
}


void test_z80_reg_specials ( void ) {
    TEST_ASSERT_EQUAL_INT32 ( 0xAA, eval_cpu ( "I" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0xBB, eval_cpu ( "R" ) );
    TEST_ASSERT_EQUAL_INT32 ( 2,    eval_cpu ( "IM" ) );
    TEST_ASSERT_EQUAL_INT32 ( 1,    eval_cpu ( "IFF1" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0,    eval_cpu ( "IFF2" ) );
}


void test_z80_reg_shadow ( void ) {
    TEST_ASSERT_EQUAL_INT32 ( 0xCAFE, eval_cpu ( "AF'" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0xBABE, eval_cpu ( "BC'" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0xDEAD, eval_cpu ( "DE'" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0xBEEF, eval_cpu ( "HL'" ) );
}


void test_z80_flags ( void ) {
    /* F = 0x34 = 0011 0100
     *   bit 0 (Cf) = 0
     *   bit 1 (Nf) = 0
     *   bit 2 (Pf) = 1
     *   bit 4 (Hf) = 1
     *   bit 5 (5)  = 1   -- nepoužitý flag
     *   bit 6 (Zf) = 0
     *   bit 7 (Sf) = 0
     */
    TEST_ASSERT_EQUAL_INT32 ( 0, eval_cpu ( "Cf" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0, eval_cpu ( "Nf" ) );
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_cpu ( "Pf" ) );
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_cpu ( "Hf" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0, eval_cpu ( "Zf" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0, eval_cpu ( "Sf" ) );
}


void test_z80_reg_in_expr ( void ) {
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_cpu ( "A == 0x12" ) );
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_cpu ( "HL > 0x4000" ) );
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_cpu ( "PC == 0x4000 && A == 0x12" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0, eval_cpu ( "A == 0xFF" ) );
}


/* ========================================================================= */
/*  Kontextové proměnné                                                      */
/* ========================================================================= */


void test_context_address_value ( void ) {
    bp_expr_ctx_t ctx;
    bp_expr_ctx_zero ( &ctx );
    ctx.Address = 0x4242;
    ctx.Value = 0xAB;
    char err[128] = {0};
    int32_t v = 0;

    TEST_ASSERT_TRUE ( bp_expr_parse_eval ( "Address == 0x4242", &ctx, &v, err, sizeof(err) ) );
    TEST_ASSERT_EQUAL_INT32 ( 1, v );

    TEST_ASSERT_TRUE ( bp_expr_parse_eval ( "Value", &ctx, &v, err, sizeof(err) ) );
    TEST_ASSERT_EQUAL_INT32 ( 0xAB, v );
}


void test_context_flags ( void ) {
    bp_expr_ctx_t ctx;
    bp_expr_ctx_zero ( &ctx );
    ctx.IsRead = true;
    ctx.IsWrite = false;
    ctx.IsExec = false;
    ctx.IsPort = true;
    int32_t v;
    char err[128];
    TEST_ASSERT_TRUE ( bp_expr_parse_eval ( "IsRead", &ctx, &v, err, sizeof(err) ) );
    TEST_ASSERT_EQUAL_INT32 ( 1, v );
    TEST_ASSERT_TRUE ( bp_expr_parse_eval ( "IsWrite", &ctx, &v, err, sizeof(err) ) );
    TEST_ASSERT_EQUAL_INT32 ( 0, v );
    TEST_ASSERT_TRUE ( bp_expr_parse_eval ( "IsRead && IsPort", &ctx, &v, err, sizeof(err) ) );
    TEST_ASSERT_EQUAL_INT32 ( 1, v );
    TEST_ASSERT_TRUE ( bp_expr_parse_eval ( "IsRead && IsWrite", &ctx, &v, err, sizeof(err) ) );
    TEST_ASSERT_EQUAL_INT32 ( 0, v );
}


void test_context_global_state ( void ) {
    bp_expr_ctx_t ctx;
    bp_expr_ctx_zero ( &ctx );
    ctx.Frame = 100;
    ctx.Cycle = 12345;
    ctx.Scanline = 50;
    ctx.BankPC = 3;
    ctx.BankAddr = 7;
    int32_t v;
    char err[128];
    TEST_ASSERT_TRUE ( bp_expr_parse_eval ( "Frame", &ctx, &v, err, sizeof(err) ) );
    TEST_ASSERT_EQUAL_INT32 ( 100, v );
    TEST_ASSERT_TRUE ( bp_expr_parse_eval ( "Cycle", &ctx, &v, err, sizeof(err) ) );
    TEST_ASSERT_EQUAL_INT32 ( 12345, v );
    TEST_ASSERT_TRUE ( bp_expr_parse_eval ( "Scanline", &ctx, &v, err, sizeof(err) ) );
    TEST_ASSERT_EQUAL_INT32 ( 50, v );
    TEST_ASSERT_TRUE ( bp_expr_parse_eval ( "BankPC + BankAddr", &ctx, &v, err, sizeof(err) ) );
    TEST_ASSERT_EQUAL_INT32 ( 10, v );
}


/* InsnPC = adresa instrukce (nezávislá na živém PC), FrameCycle = takty
 * od začátku snímku; Cycle se ve výrazu ořízne na int32 (přetečení). */
void test_context_insnpc_framecycle ( void ) {
    bp_expr_ctx_t ctx;
    bp_expr_ctx_zero ( &ctx );
    ctx.cpu = &g_test_cpu;
    g_test_cpu.pc = 0x3006;
    ctx.InsnPC = 0x3003;
    ctx.FrameCycle = 354431;
    ctx.Cycle = (int64_t) 0x80000000LL + 5;
    int32_t v;
    char err[128];
    TEST_ASSERT_TRUE ( bp_expr_parse_eval ( "InsnPC", &ctx, &v, err, sizeof(err) ) );
    TEST_ASSERT_EQUAL_INT32 ( 0x3003, v );
    TEST_ASSERT_TRUE ( bp_expr_parse_eval ( "insnpc != PC", &ctx, &v, err, sizeof(err) ) );
    TEST_ASSERT_EQUAL_INT32 ( 1, v );
    TEST_ASSERT_TRUE ( bp_expr_parse_eval ( "FrameCycle", &ctx, &v, err, sizeof(err) ) );
    TEST_ASSERT_EQUAL_INT32 ( 354431, v );
    TEST_ASSERT_TRUE ( bp_expr_parse_eval ( "Cycle < 0", &ctx, &v, err, sizeof(err) ) );
    TEST_ASSERT_EQUAL_INT32 ( 1, v );
}


/**
 * V1.5 fáze 2.4: breakpoints_fill_global_ctx musí čerpat Frame/Scanline/Cycle
 * z g_gdg (= reálný GDG state, ne hardcoded 0).
 *
 * Test manipulátor: nastaví g_gdg.beam_row a g_gdg.total_elapsed.screens,
 * zavolá fill_global_ctx, ověří že ctx.Scanline/Frame matchnou. Cycle
 * ověřuje jen že je nenulové (přesná hodnota závisí na celkovém
 * gdg_get_total_ticks() makro = ticks + screens*VIDEO_SCREEN_TICKS).
 */
void test_fill_global_ctx_from_gdg ( void ) {
#if (MZARCH == 800) || (MZARCH == 1500)
    /* Backup. */
    unsigned saved_beam_row = g_gdg.beam_row;
    unsigned saved_screens  = g_gdg.total_elapsed.screens;
    unsigned saved_ticks    = g_gdg.total_elapsed.ticks;

    g_gdg.beam_row              = 192;
    g_gdg.total_elapsed.screens = 42;
    g_gdg.total_elapsed.ticks   = 1000;

    bp_expr_ctx_t ctx;
    bp_expr_ctx_zero ( &ctx );
    breakpoints_fill_global_ctx ( &ctx );

    TEST_ASSERT_EQUAL_INT32 ( 192, ctx.Scanline );
    TEST_ASSERT_EQUAL_INT64 ( 42, ctx.Frame );
    /* Cycle = gdg_get_total_ticks() = ticks + screens * VIDEO_SCREEN_TICKS.
     * Pro screens=42 a ticks=1000 musí být >0 a > ticks (= screens je
     * vynásobené velkou konstantou). */
    TEST_ASSERT_TRUE ( ctx.Cycle > 1000 );

    /* Restore. */
    g_gdg.beam_row              = saved_beam_row;
    g_gdg.total_elapsed.screens = saved_screens;
    g_gdg.total_elapsed.ticks   = saved_ticks;
#else
    TEST_IGNORE_MESSAGE ( "MZARCH != 800/1500 - GDG ctx zdroje TODO" );
#endif
}


/**
 * V1.5 fáze 2.4: ověření že Scanline a Frame jsou přístupné v expression
 * po naplnění z g_gdg (= end-to-end test).
 */
void test_expr_scanline_frame_from_gdg ( void ) {
#if (MZARCH == 800) || (MZARCH == 1500)
    unsigned saved_beam_row = g_gdg.beam_row;
    unsigned saved_screens  = g_gdg.total_elapsed.screens;

    g_gdg.beam_row              = 100;
    g_gdg.total_elapsed.screens = 7;

    bp_expr_ctx_t ctx;
    bp_expr_ctx_zero ( &ctx );
    breakpoints_fill_global_ctx ( &ctx );

    int32_t v;
    char err[128];
    TEST_ASSERT_TRUE ( bp_expr_parse_eval ( "Scanline == 100", &ctx, &v, err, sizeof(err) ) );
    TEST_ASSERT_EQUAL_INT32 ( 1, v );
    TEST_ASSERT_TRUE ( bp_expr_parse_eval ( "Frame == 7", &ctx, &v, err, sizeof(err) ) );
    TEST_ASSERT_EQUAL_INT32 ( 1, v );

    g_gdg.beam_row              = saved_beam_row;
    g_gdg.total_elapsed.screens = saved_screens;
#else
    TEST_IGNORE_MESSAGE ( "MZARCH != 800/1500 - GDG ctx zdroje TODO" );
#endif
}


/* ========================================================================= */
/*  Built-in funkce                                                          */
/* ========================================================================= */


void test_fn_min_max ( void ) {
    TEST_ASSERT_EQUAL_INT32 ( 3, eval_simple ( "min(3, 5)" ) );
    TEST_ASSERT_EQUAL_INT32 ( 3, eval_simple ( "min(5, 3)" ) );
    TEST_ASSERT_EQUAL_INT32 ( 5, eval_simple ( "max(3, 5)" ) );
    TEST_ASSERT_EQUAL_INT32 ( -3, eval_simple ( "min(-3, 0)" ) );
}


void test_fn_abs ( void ) {
    TEST_ASSERT_EQUAL_INT32 ( 5, eval_simple ( "abs(5)" ) );
    TEST_ASSERT_EQUAL_INT32 ( 5, eval_simple ( "abs(-5)" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0, eval_simple ( "abs(0)" ) );
}


void test_fn_if ( void ) {
    TEST_ASSERT_EQUAL_INT32 ( 10, eval_simple ( "if(1, 10, 20)" ) );
    TEST_ASSERT_EQUAL_INT32 ( 20, eval_simple ( "if(0, 10, 20)" ) );
    /* if jako select: if(C > 0, 1, 0) */
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_simple ( "if(5 > 0, 1, 0)" ) );
}


void test_fn_bit ( void ) {
    /* bit(value, n) = (value >> n) & 1 */
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_simple ( "bit(0x80, 7)" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0, eval_simple ( "bit(0x80, 6)" ) );
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_simple ( "bit(0xAA, 1)" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0, eval_simple ( "bit(0xAA, 0)" ) );
}


void test_fn_sign_extend ( void ) {
    TEST_ASSERT_EQUAL_INT32 ( -1,   eval_simple ( "s8(0xFF)" ) );
    TEST_ASSERT_EQUAL_INT32 ( 127,  eval_simple ( "s8(0x7F)" ) );
    TEST_ASSERT_EQUAL_INT32 ( -128, eval_simple ( "s8(0x80)" ) );
    TEST_ASSERT_EQUAL_INT32 ( -1,   eval_simple ( "s16(0xFFFF)" ) );
    TEST_ASSERT_EQUAL_INT32 ( 32767, eval_simple ( "s16(0x7FFF)" ) );
    TEST_ASSERT_EQUAL_INT32 ( 42,    eval_simple ( "s32(42)" ) );
}


void test_fn_with_register ( void ) {
    /* min(A, B) - A=0x12, B=0x56 - min = 0x12 */
    TEST_ASSERT_EQUAL_INT32 ( 0x12, eval_cpu ( "min(A, B)" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0x56, eval_cpu ( "max(A, B)" ) );
    /* if(C > 0, 1, 0) - C = 0x78, > 0 → 1 */
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_cpu ( "if(C > 0, 1, 0)" ) );
}


void test_fn_arity_error ( void ) {
    assert_parse_fails ( "min(1)" );           /* arity 2 */
    assert_parse_fails ( "min(1, 2, 3)" );     /* arity 2 */
    assert_parse_fails ( "if(1, 2)" );         /* arity 3 */
    assert_parse_fails ( "abs(1, 2)" );        /* arity 1 */
}


/* ========================================================================= */
/*  User vars $name                                                          */
/* ========================================================================= */


void test_user_vars_default_zero ( void ) {
    /* V1.2 storage neexistuje, $vars vrací 0. */
    TEST_ASSERT_EQUAL_INT32 ( 0, eval_simple ( "$counter" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0, eval_simple ( "$foo + $bar" ) );
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_simple ( "$counter == 0" ) );
}


void test_user_vars_lex_ident ( void ) {
    /* $ s podtržítkem a číslicemi by mělo být OK. */
    TEST_ASSERT_EQUAL_INT32 ( 0, eval_simple ( "$my_var_42" ) );
    /* $ bez identifier je chyba */
    assert_parse_fails ( "$" );
    assert_parse_fails ( "$ + 1" );
}


/* ========================================================================= */
/*  Memory deref a port                                                      */
/* ========================================================================= */


/* POZNÁMKA: bp_read_byte/word/port zatím interně volá memory_read_byte
 * resp. vrací 0 pro port. Tyto testy jen ověří, že parser akceptuje
 * syntax a evaluator dispatchne na správný path - nemůžeme bez plně
 * inicializovaného emu memory subsystému ověřit konkrétní hodnoty. */


void test_mem_deref_byte_parses ( void ) {
    /* Neoveřujeme hodnotu (memory subsystem není init); jen že parse OK. */
    char err[128] = {0};
    bp_expr_t *e = bp_expr_parse ( "[0x4000]", err, sizeof(err) );
    TEST_ASSERT_NOT_NULL ( e );
    bp_expr_free ( e );

    e = bp_expr_parse ( "[0x4000]!", err, sizeof(err) );
    TEST_ASSERT_NOT_NULL ( e );
    bp_expr_free ( e );

    e = bp_expr_parse ( "{0x4000}", err, sizeof(err) );
    TEST_ASSERT_NOT_NULL ( e );
    bp_expr_free ( e );
}


void test_mem_deref_with_register_addr ( void ) {
    /* Parsovat [HL] - eval závisí na memory subsystem, jen ověřit parse. */
    char err[128] = {0};
    bp_expr_t *e = bp_expr_parse ( "[HL]", err, sizeof(err) );
    TEST_ASSERT_NOT_NULL ( e );
    bp_expr_free ( e );

    e = bp_expr_parse ( "[HL + 1]", err, sizeof(err) );
    TEST_ASSERT_NOT_NULL ( e );
    bp_expr_free ( e );

    e = bp_expr_parse ( "[HL]! == 0xFF && PC > 0x4000", err, sizeof(err) );
    TEST_ASSERT_NOT_NULL ( e );
    bp_expr_free ( e );
}


void test_port_deref_parses ( void ) {
    char err[128] = {0};
    bp_expr_t *e = bp_expr_parse ( "port[0xCE]", err, sizeof(err) );
    TEST_ASSERT_NOT_NULL ( e );
    bp_expr_free ( e );

    e = bp_expr_parse ( "port[0xCE]!", err, sizeof(err) );
    TEST_ASSERT_NOT_NULL ( e );
    bp_expr_free ( e );

    /* V1.6+ 4.2 5b: port[N] (no_se) probe pro side-effect-heavy
     * chip vraci bus latch (regDBUS_latch). V test fixture je latch
     * inicializovan na 0, takze probe vrati 0. Sanity test = parse OK
     * + eval bez crashe. */
    bp_expr_ctx_t ctx;
    bp_expr_ctx_zero ( &ctx );
    int32_t v = 0;
    TEST_ASSERT_TRUE ( bp_expr_parse_eval ( "port[0xCE]", &ctx, &v, err, sizeof(err) ) );
    /* Value muze byt 0 (= latch v init stavu) ale neverify konkretni
     * cislo - probe vraci stav HW dispatcher chipu. */
}


/**
 * V1.6+ 4.2 5b ✅: port[N]! (side_effect=true) volá realny port_read_cb.
 * V test fixtures bez plne emu init je return value HW-state-dependent
 * (= GDG DMD status, PSG, ...). Sanity = parse + eval bez crashe,
 * NEnverify konkretni cislo (= MZ-800 dependent na chip init).
 */
void test_port_deref_with_bang_eval_no_crash ( void ) {
    char err[128] = {0};
    bp_expr_ctx_t ctx;
    bp_expr_ctx_zero ( &ctx );
    int32_t v = 0xDEAD;
    /* Real port_read_cb pro 0xCE = GDG DMD status. Vraci aktualni stav
     * HW; typicky 0x20 nebo 0x00 podle DMD/raster pozice. Sanity:
     * eval funguje bez crashe a value je 8-bit. */
    TEST_ASSERT_TRUE ( bp_expr_parse_eval ( "port[0xCE]!", &ctx, &v, err, sizeof(err) ) );
    TEST_ASSERT_TRUE ( ( v >= 0 ) && ( v <= 0xFF ) );
}


/**
 * V1.6+ 4.2 5b ✅: port[N] (side_effect=false) probe.
 *
 * Joystick (0xF0/F1 na MZ-800) je no_se safe (= cista funkce). MZ-1500
 * nema joystick a probe vraci bus latch.
 *
 * Side-effect-heavy chip (0xD0-D3 PIO 8255) probe vraci bus latch
 * (= safer fallback, nemutuje PortC tape state).
 */
void test_port_no_se_probe_no_side_effect ( void ) {
    char err[128] = {0};
    bp_expr_ctx_t ctx;
    bp_expr_ctx_zero ( &ctx );
    int32_t v;

    /* Probe na PIO 8255 PortC (0xD2) - vraci latch fallback. */
    TEST_ASSERT_TRUE ( bp_expr_parse_eval ( "port[0xD2]", &ctx, &v, err, sizeof(err) ) );
    TEST_ASSERT_TRUE ( ( v >= 0 ) && ( v <= 0xFF ) );

    /* Sequencni probe nemenni state - druhe volani vraci stejnou
     * hodnotu jako prvni (= no side effect na latch). */
    int32_t v1, v2;
    TEST_ASSERT_TRUE ( bp_expr_parse_eval ( "port[0xCE]", &ctx, &v1, err, sizeof(err) ) );
    TEST_ASSERT_TRUE ( bp_expr_parse_eval ( "port[0xCE]", &ctx, &v2, err, sizeof(err) ) );
    TEST_ASSERT_EQUAL_INT32 ( v1, v2 );
}


/* ========================================================================= */
/*  Parser errors                                                            */
/* ========================================================================= */


void test_parser_unfinished ( void ) {
    assert_parse_fails ( "1 +" );
    assert_parse_fails ( "(1 + 2" );          /* missing ')' */
    assert_parse_fails ( "1 + 2)" );          /* extra ')' */
}


void test_parser_invalid_chars ( void ) {
    assert_parse_fails ( "1 @ 2" );           /* @ není operátor */
    assert_parse_fails ( "1 ?" );             /* ? není podporované */
}


void test_parser_double_unary_ok ( void ) {
    /* Ověřuje, že --5 = 5 a !!1 = 1 (= legální dvojitý unární). */
    TEST_ASSERT_EQUAL_INT32 ( 5, eval_simple ( "--5" ) );
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_simple ( "!!1" ) );
    TEST_ASSERT_EQUAL_INT32 ( 1, eval_simple ( "!!42" ) );
}


void test_parser_unknown_function ( void ) {
    assert_parse_fails ( "foo(1)" );
    assert_parse_fails ( "sin(0)" );
}


void test_parser_assignment_rejected ( void ) {
    /* '=' samotný (ne '==') = nepodporováno */
    assert_parse_fails ( "A = 5" );
}


/* ========================================================================= */
/*  Edge cases                                                               */
/* ========================================================================= */


void test_eval_null_expr_returns_zero ( void ) {
    bp_expr_ctx_t ctx;
    bp_expr_ctx_zero ( &ctx );
    TEST_ASSERT_EQUAL_INT32 ( 0, bp_expr_eval ( NULL, &ctx ) );
}


void test_parse_null_source ( void ) {
    char err[128] = {0};
    bp_expr_t *e = bp_expr_parse ( NULL, err, sizeof(err) );
    TEST_ASSERT_NULL ( e );
}


void test_complex_expression ( void ) {
    /* Reálná BP condition z PLAN: A == 0x42 && PC > 0x4000 */
    bp_expr_ctx_t ctx;
    bp_expr_ctx_zero ( &ctx );
    g_test_cpu.af.h = 0x42;
    g_test_cpu.pc   = 0x5000;
    ctx.cpu = &g_test_cpu;

    int32_t v = 0;
    char err[128] = {0};
    TEST_ASSERT_TRUE ( bp_expr_parse_eval ( "A == 0x42 && PC > 0x4000", &ctx, &v, err, sizeof(err) ) );
    TEST_ASSERT_EQUAL_INT32 ( 1, v );

    g_test_cpu.af.h = 0x00;
    TEST_ASSERT_TRUE ( bp_expr_parse_eval ( "A == 0x42 && PC > 0x4000", &ctx, &v, err, sizeof(err) ) );
    TEST_ASSERT_EQUAL_INT32 ( 0, v );
}


void test_expr_reuse ( void ) {
    /* Ověří, že parsovaný AST lze evaluovat opakovaně. */
    char err[128] = {0};
    bp_expr_t *e = bp_expr_parse ( "A + B", err, sizeof(err) );
    TEST_ASSERT_NOT_NULL ( e );

    bp_expr_ctx_t ctx;
    bp_expr_ctx_zero ( &ctx );
    g_test_cpu.af.h = 1;
    g_test_cpu.bc.h = 2;
    ctx.cpu = &g_test_cpu;
    TEST_ASSERT_EQUAL_INT32 ( 3, bp_expr_eval ( e, &ctx ) );

    g_test_cpu.af.h = 10;
    g_test_cpu.bc.h = 20;
    TEST_ASSERT_EQUAL_INT32 ( 30, bp_expr_eval ( e, &ctx ) );

    bp_expr_free ( e );
}


/* ========================================================================= */
/*  V1.5 - Half registers IXH/IXL/IYH/IYL                                    */
/* ========================================================================= */


void test_z80_half_reg_ix ( void ) {
    /* IX = 0x1122 (test_cpu_init: ix.h=0x11, ix.l=0x22). */
    TEST_ASSERT_EQUAL_INT32 ( 0x11, eval_cpu ( "IXH" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0x22, eval_cpu ( "IXL" ) );
}


void test_z80_half_reg_iy ( void ) {
    /* IY = 0x3344 (test_cpu_init: iy.h=0x33, iy.l=0x44). */
    TEST_ASSERT_EQUAL_INT32 ( 0x33, eval_cpu ( "IYH" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0x44, eval_cpu ( "IYL" ) );
}


void test_z80_half_reg_case_insensitive ( void ) {
    TEST_ASSERT_EQUAL_INT32 ( 0x11, eval_cpu ( "ixh" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0x22, eval_cpu ( "Ixl" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0x33, eval_cpu ( "iyH" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0x44, eval_cpu ( "IyL" ) );
}


void test_z80_half_reg_in_expr ( void ) {
    /* Půl-registry v aritmetických / porovnávacích výrazech. */
    TEST_ASSERT_EQUAL_INT32 ( 0x33, eval_cpu ( "IXH + IXL" ) );  /* 0x11 + 0x22 */
    TEST_ASSERT_EQUAL_INT32 ( 1,    eval_cpu ( "IXH < IYH" ) );   /* 0x11 < 0x33 */
    TEST_ASSERT_EQUAL_INT32 ( 1,    eval_cpu ( "IXH == 0x11 && IYL == 0x44" ) );
}


/* ========================================================================= */
/*  V1.5 - Shadow apostrophe required (BC vs BC')                            */
/* ========================================================================= */


void test_shadow_apostrophe_distinct ( void ) {
    /* Bez apostrofu = main register, s apostrofem = shadow. */
    TEST_ASSERT_EQUAL_INT32 ( 0x5678, eval_cpu ( "BC" ) );
    TEST_ASSERT_EQUAL_INT32 ( 0xBABE, eval_cpu ( "BC'" ) );
    /* Rozdílné. */
    TEST_ASSERT_NOT_EQUAL_INT32 ( eval_cpu ( "BC" ), eval_cpu ( "BC'" ) );
}


void test_shadow_in_expr ( void ) {
    /* Shadow registers v aritmetických výrazech. */
    /* AF' = 0xCAFE; high byte 0xCA. */
    TEST_ASSERT_EQUAL_INT32 ( 0xCAFE, eval_cpu ( "AF'" ) );
    /* BC' + DE' = 0xBABE + 0xDEAD = 0x19996B. */
    TEST_ASSERT_EQUAL_INT32 ( 0xBABE + 0xDEAD, eval_cpu ( "BC' + DE'" ) );
}


/* ========================================================================= */
/*  V1.5.B - $vars edge cases                                                */
/* ========================================================================= */


#include "debugger/bp_vars.h"


void test_user_vars_after_set ( void ) {
    /* Storage je init mztest_init - nastavíme + ověříme přes expr. */
    bp_vars_clear_storage ( );
    bp_var_set ( "score", 1234 );

    TEST_ASSERT_EQUAL_INT32 ( 1234, eval_simple ( "$score" ) );
    TEST_ASSERT_EQUAL_INT32 ( 1234 * 2, eval_simple ( "$score * 2" ) );

    bp_var_set ( "score", -5 );
    TEST_ASSERT_EQUAL_INT32 ( -5, eval_simple ( "$score" ) );
}


void test_user_vars_max_31_chars ( void ) {
    /* 31 znaků = boundary OK. */
    bp_vars_clear_storage ( );
    char name31[ 32 ];
    memset ( name31, 'a', 31 );
    name31[ 31 ] = '\0';
    bp_var_set ( name31, 42 );

    /* Construct $name expression. */
    char expr[ 36 ];
    snprintf ( expr, sizeof ( expr ), "$%s", name31 );
    TEST_ASSERT_EQUAL_INT32 ( 42, eval_simple ( expr ) );
}


void test_user_vars_case_sensitive_in_expr ( void ) {
    bp_vars_clear_storage ( );
    bp_var_set ( "Foo", 100 );
    bp_var_set ( "foo", 200 );

    TEST_ASSERT_EQUAL_INT32 ( 100, eval_simple ( "$Foo" ) );
    TEST_ASSERT_EQUAL_INT32 ( 200, eval_simple ( "$foo" ) );
    TEST_ASSERT_EQUAL_INT32 ( 300, eval_simple ( "$Foo + $foo" ) );
}


void test_user_vars_keywords_as_names ( void ) {
    /* $-prefix je distinktivní → keywords jsou platná jména. */
    bp_vars_clear_storage ( );
    bp_var_set ( "if",  10 );
    bp_var_set ( "set", 20 );
    bp_var_set ( "log", 30 );

    TEST_ASSERT_EQUAL_INT32 ( 10, eval_simple ( "$if" ) );
    TEST_ASSERT_EQUAL_INT32 ( 20, eval_simple ( "$set" ) );
    TEST_ASSERT_EQUAL_INT32 ( 30, eval_simple ( "$log" ) );
}


/* ========================================================================= */
/*  MAIN                                                                     */
/* ========================================================================= */


int main ( int argc, char *argv[] ) {
    mztest_parse_args ( argc, argv );
    mztest_init ( );

    UNITY_BEGIN ( );

    /* Lexer / literály */
    RUN_TEST ( test_literal_dec );
    RUN_TEST ( test_literal_hex_0x );
    RUN_TEST ( test_literal_hex_hash );
    RUN_TEST ( test_literal_bin );
    RUN_TEST ( test_literal_lex_disambig_percent );

    /* Operátory + precedence */
    RUN_TEST ( test_arith_basic );
    RUN_TEST ( test_precedence_mul_over_add );
    RUN_TEST ( test_precedence_shift_below_arith );
    RUN_TEST ( test_precedence_relational );
    RUN_TEST ( test_precedence_bitwise );
    RUN_TEST ( test_unary );
    RUN_TEST ( test_shifts );
    RUN_TEST ( test_div_by_zero_returns_zero );

    /* Short-circuit && / || */
    RUN_TEST ( test_logical_and_basic );
    RUN_TEST ( test_logical_or_basic );
    RUN_TEST ( test_short_circuit_and_skips_rhs );
    RUN_TEST ( test_short_circuit_or_skips_rhs );
    RUN_TEST ( test_logical_precedence_below_bitwise );

    /* Z80 registry */
    RUN_TEST ( test_z80_reg_8bit );
    RUN_TEST ( test_z80_reg_16bit );
    RUN_TEST ( test_z80_reg_case_insensitive );
    RUN_TEST ( test_z80_reg_specials );
    RUN_TEST ( test_z80_reg_shadow );
    RUN_TEST ( test_z80_flags );
    RUN_TEST ( test_z80_reg_in_expr );

    /* V1.5 - Half registers IXH/IXL/IYH/IYL */
    RUN_TEST ( test_z80_half_reg_ix );
    RUN_TEST ( test_z80_half_reg_iy );
    RUN_TEST ( test_z80_half_reg_case_insensitive );
    RUN_TEST ( test_z80_half_reg_in_expr );

    /* V1.5 - Shadow apostrophe */
    RUN_TEST ( test_shadow_apostrophe_distinct );
    RUN_TEST ( test_shadow_in_expr );

    /* V1.5.B - $vars edge cases */
    RUN_TEST ( test_user_vars_after_set );
    RUN_TEST ( test_user_vars_max_31_chars );
    RUN_TEST ( test_user_vars_case_sensitive_in_expr );
    RUN_TEST ( test_user_vars_keywords_as_names );

    /* Kontextové proměnné */
    RUN_TEST ( test_context_address_value );
    RUN_TEST ( test_context_flags );
    RUN_TEST ( test_context_global_state );
    RUN_TEST ( test_context_insnpc_framecycle );
    RUN_TEST ( test_fill_global_ctx_from_gdg );
    RUN_TEST ( test_expr_scanline_frame_from_gdg );

    /* Built-in funkce */
    RUN_TEST ( test_fn_min_max );
    RUN_TEST ( test_fn_abs );
    RUN_TEST ( test_fn_if );
    RUN_TEST ( test_fn_bit );
    RUN_TEST ( test_fn_sign_extend );
    RUN_TEST ( test_fn_with_register );
    RUN_TEST ( test_fn_arity_error );

    /* User vars */
    RUN_TEST ( test_user_vars_default_zero );
    RUN_TEST ( test_user_vars_lex_ident );

    /* Memory / port deref */
    RUN_TEST ( test_mem_deref_byte_parses );
    RUN_TEST ( test_mem_deref_with_register_addr );
    RUN_TEST ( test_port_deref_parses );
    RUN_TEST ( test_port_deref_with_bang_eval_no_crash );
    RUN_TEST ( test_port_no_se_probe_no_side_effect );

    /* Parser errors */
    RUN_TEST ( test_parser_unfinished );
    RUN_TEST ( test_parser_invalid_chars );
    RUN_TEST ( test_parser_double_unary_ok );
    RUN_TEST ( test_parser_unknown_function );
    RUN_TEST ( test_parser_assignment_rejected );

    /* Edge cases */
    RUN_TEST ( test_eval_null_expr_returns_zero );
    RUN_TEST ( test_parse_null_source );
    RUN_TEST ( test_complex_expression );
    RUN_TEST ( test_expr_reuse );

    int result = UNITY_END ( );

    mztest_teardown ( );
    return result;
}
