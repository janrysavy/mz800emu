/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file test_step_cycles.c
 * @brief Testy návratové hodnoty z80_step() na hranicích čítače total_cycles.
 *
 * z80_step() vrací počet T-stavů instrukce jako rozdíl cpu->total_cycles
 * (uint32_t, běží modulo 2^32). Původní implementace převáděla absolutní
 * hodnoty na int a odečítala je - na hranici 2^31 to je přetečení se
 * znaménkem (nedefinované chování v C). Běžný build vracel správné číslo
 * náhodou, proto tento test hlídá kontrakt (délka instrukce i stav čítače
 * po přetečení), ne samotné UB. UB ukáže jen build s -ftrapv.
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
 * @brief Provede jednu instrukci od PC 0100h se zadaným stavem čítače.
 *
 * @param start     Počáteční hodnota cpu->total_cycles.
 * @param expected  Očekávaná délka instrukce v T-stavech.
 * @pre Instrukce je připravená v g_mem od adresy 0100h.
 * @post cpu->total_cycles == start + expected (modulo 2^32).
 */
static void step_from(uint32_t start, int expected)
{
    g_cpu->pc = 0x0100;
    g_cpu->total_cycles = start;
    int t = z80_step(g_cpu);
    TEST_ASSERT_EQUAL_INT(expected, t);
    TEST_ASSERT_EQUAL_HEX32((uint32_t)(start + (uint32_t)expected), g_cpu->total_cycles);
}


/* ===== Testy ===== */

/** NOP (4 T) přes hranici 2^31: start těsně pod, na a těsně za hranicí. */
void test_step_nop_across_2pow31(void)
{
    g_mem[0x0100] = 0x00;   /* NOP */
    step_from(0x7FFFFFFDu, 4);
    step_from(0x7FFFFFFEu, 4);
    step_from(0x7FFFFFFFu, 4);
    step_from(0x80000000u, 4);
}

/** NOP (4 T) přes hranici 2^32: čítač přeteče, délka zůstane 4. */
void test_step_nop_across_2pow32(void)
{
    g_mem[0x0100] = 0x00;   /* NOP */
    step_from(0xFFFFFFFDu, 4);
    step_from(0xFFFFFFFEu, 4);
    step_from(0xFFFFFFFFu, 4);
    step_from(0x00000000u, 4);
}

/** Delší instrukce LD BC,1234h (10 T) přes obě hranice. */
void test_step_ld_bc_nn_across_boundaries(void)
{
    g_mem[0x0100] = 0x01;   /* LD BC,1234h */
    g_mem[0x0101] = 0x34;
    g_mem[0x0102] = 0x12;
    step_from(0x7FFFFFF9u, 10);
    step_from(0xFFFFFFF9u, 10);
}


int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_step_nop_across_2pow31);
    RUN_TEST(test_step_nop_across_2pow32);
    RUN_TEST(test_step_ld_bc_nn_across_boundaries);
    return UNITY_END();
}
