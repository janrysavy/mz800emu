/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file test_reset_state.c
 * @brief Testy stavu rozpracované instrukce po z80_reset().
 *
 * cpu->op_tstate drží T-stavy od začátku aktuální instrukce. DISPATCH ho
 * nuluje až před další instrukcí, takže mezi instrukcemi v něm zůstává délka
 * předchozí instrukce. Emulátor ho čte i mimo instrukci (mzarch_main_insideop()
 * při zápisech bootstrapu MZF nebo ladicích příkazů, výpis registrů v MCP).
 * Bez nulování v z80_reset() by se po resetu do času GDG a do výpisu
 * propsala délka poslední instrukce před resetem.
 */

#include "unity.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "libs/cpu-z80/z80.h"


/* ===== Test paměť + IO simulace (vzor test_r_register.c) ===== */

static uint8_t g_mem[0x10000];

static uint8_t test_mread(struct z80_s *cpu, uint16_t addr, int m1, void *u)
{ (void)cpu; (void)m1; (void)u; return g_mem[addr]; }

static void test_mwrite(struct z80_s *cpu, uint16_t addr, uint8_t v, void *u)
{ (void)cpu; (void)u; g_mem[addr] = v; }

static uint8_t test_pread(struct z80_s *cpu, uint16_t port, void *u)
{ (void)cpu; (void)port; (void)u; return 0xFF; }

static void test_pwrite(struct z80_s *cpu, uint16_t port, uint8_t v, void *u)
{ (void)cpu; (void)port; (void)v; (void)u; }

static uint8_t test_intread(struct z80_s *cpu, void *u)
{ (void)cpu; (void)u; return 0xFF; }


/* ===== setUp / tearDown ===== */

static z80_t *g_cpu;

void setUp(void)
{
    memset(g_mem, 0, sizeof(g_mem));
    g_cpu = z80_create(test_mread, NULL, test_mwrite, NULL,
                       test_pread, NULL, test_pwrite, NULL,
                       test_intread, NULL);
    TEST_ASSERT_NOT_NULL(g_cpu);
}

void tearDown(void)
{
    z80_destroy(g_cpu);
    g_cpu = NULL;
}


/**
 * @brief Po instrukci zůstane op_tstate nenulové, z80_reset() ho vynuluje.
 *
 * LD BC,1234h (10 T) od 0000h. Kontroluje se i předpoklad testu (op_tstate
 * po instrukci != 0), aby test nepoznal opravu jen náhodou.
 */
static void test_reset_clears_op_tstate_after_instruction(void)
{
    g_mem[0x0000] = 0x01;   /* LD BC,1234h */
    g_mem[0x0001] = 0x34;
    g_mem[0x0002] = 0x12;

    int t = z80_step(g_cpu);
    TEST_ASSERT_EQUAL_INT(10, t);
    TEST_ASSERT_NOT_EQUAL_INT(0, g_cpu->op_tstate);

    z80_reset(g_cpu);
    TEST_ASSERT_EQUAL_INT(0, g_cpu->op_tstate);
}

/**
 * @brief Čekací stavy přidané mimo instrukci po resetu nezůstanou.
 *
 * z80_add_wait_states() přičítá do wait_cycles i do op_tstate (tak to dělá
 * emulátor při zápisu do PSG mimo instrukci). Po resetu musí být oba účty 0.
 */
static void test_reset_clears_wait_accounts(void)
{
    z80_add_wait_states(g_cpu, 7);
    TEST_ASSERT_NOT_EQUAL_INT(0, g_cpu->op_tstate);

    z80_reset(g_cpu);
    TEST_ASSERT_EQUAL_INT(0, g_cpu->op_tstate);
    TEST_ASSERT_EQUAL_INT(0, g_cpu->wait_cycles);
}


int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_reset_clears_op_tstate_after_instruction);
    RUN_TEST(test_reset_clears_wait_accounts);
    return UNITY_END();
}
