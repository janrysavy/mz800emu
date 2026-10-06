/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file test_r_register.c
 * @brief Regresní testy inkrementu registru R během HALT a při přijetí INT/NMI.
 *
 * Reprodukuje regresi z v2.0.2 (výměna z80ex -> cpu-z80): jádro nezvyšovalo R
 * při opakovaném fetchi HALT ani při přijetí maskovatelného přerušení a NMI.
 * Loader hry Interkarate (ochrana "CHRANNY SYSTEM ADOLF") dešifruje kód klíčem
 * z LD A,R po HALT čekajícím na VBLN přerušení; s chybným R vyšel jiný klíč
 * a loader se zacyklil.
 *
 * Očekávané chování (báze mz800-knowledge, cpu/z80/09-r-register.md a
 * 07-interrupts.md; z80ex v2.0.1):
 * - každý opakovaný fetch HALT (po 4 T) je M1 cyklus -> R += 1,
 * - přijetí INT (IM 0/1/2) i NMI -> R += 1,
 * - inkrementuje se jen dolních 7 bitů, bit 7 se zachová.
 *
 * Testy používají reálnou MZ cestu (external_int_handling = true, z80_step
 * + z80_int/z80_nmi + z80_process_interrupt) i dávkovou cestu z80_execute.
 */

#include "unity.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "libs/cpu-z80/z80.h"


/* ===== Test paměť + IO simulace (vzor test_ei_delay.c) ===== */

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
 * @brief Připraví "HALT" na 0x0100, IM 1, EI stav (iff1 = iff2 = 1), R = r0.
 */
static void prepare_halt(uint8_t r0)
{
    g_mem[0x0100] = 0x76;   /* HALT */
    g_cpu->pc  = 0x0100;
    g_cpu->sp  = 0x8000;
    g_cpu->im  = 1;
    g_cpu->iff1 = 1;
    g_cpu->iff2 = 1;
    g_cpu->r   = r0;
}


/* ===== Testy ===== */

/**
 * @test HALT: fetch HALT + každé opakování (4 T) zvýší R o 1 (per-step cesta).
 */
void test_halt_refetch_increments_r_step(void)
{
    g_cpu->external_int_handling = true;
    prepare_halt(0x10);

    z80_step(g_cpu);                      /* fetch HALT: R = 0x11 */
    TEST_ASSERT_TRUE(g_cpu->halted);
    TEST_ASSERT_EQUAL_HEX8(0x11, g_cpu->r);

    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL_INT(4, z80_step(g_cpu));
    }
    TEST_ASSERT_TRUE(g_cpu->halted);
    TEST_ASSERT_EQUAL_HEX8(0x11 + 10, g_cpu->r);
}

/**
 * @test HALT: dávková cesta z80_execute - R roste o 1 za každé 4 T v HALT.
 */
void test_halt_refetch_increments_r_batch(void)
{
    g_cpu->external_int_handling = true;  /* bez INT, jen čekání v HALT */
    prepare_halt(0x20);

    int cycles = 0;
    while (cycles < 4 + 40) {
        cycles += z80_execute(g_cpu, 4);
    }
    TEST_ASSERT_TRUE(g_cpu->halted);
    TEST_ASSERT_EQUAL_INT(44, cycles);
    TEST_ASSERT_EQUAL_HEX8(0x20 + 1 + 10, g_cpu->r);
}

/**
 * @test HALT: inkrement přeteče jen v dolních 7 bitech, bit 7 zůstane.
 */
void test_halt_refetch_keeps_bit7(void)
{
    g_cpu->external_int_handling = true;
    prepare_halt(0xFE);

    z80_step(g_cpu);                      /* HALT: 0xFE -> 0xFF */
    z80_step(g_cpu);                      /* 0xFF -> 0x80 (bit 7 drží) */
    TEST_ASSERT_EQUAL_HEX8(0x80, g_cpu->r);
}

/**
 * @test Přijetí INT (IM 1) z HALT zvýší R o 1 navíc k opakovaným fetchům.
 */
void test_int_ack_increments_r(void)
{
    g_cpu->external_int_handling = true;
    prepare_halt(0x30);

    z80_step(g_cpu);                      /* HALT: R = 0x31 */
    z80_step(g_cpu);                      /* opakování: R = 0x32 */
    z80_int(g_cpu);
    TEST_ASSERT_TRUE(z80_process_interrupt(g_cpu) > 0);
    TEST_ASSERT_FALSE(g_cpu->halted);
    TEST_ASSERT_EQUAL_HEX16(0x0038, g_cpu->pc);
    TEST_ASSERT_EQUAL_HEX8(0x33, g_cpu->r);
}

/**
 * @test Přijetí INT mimo HALT (IM 2) zvýší R o 1.
 */
void test_int_ack_im2_increments_r(void)
{
    g_cpu->external_int_handling = true;
    g_mem[0x0200] = 0x00;                 /* NOP */
    g_mem[0x40FE] = 0x00; g_mem[0x40FF] = 0x50;
    g_cpu->pc = 0x0200; g_cpu->sp = 0x8000;
    g_cpu->im = 2; g_cpu->i = 0x40;
    g_cpu->iff1 = 1; g_cpu->iff2 = 1;
    g_cpu->r = 0x45;

    z80_step(g_cpu);                      /* NOP: R = 0x46 */
    z80_int(g_cpu);
    TEST_ASSERT_TRUE(z80_process_interrupt(g_cpu) > 0);
    TEST_ASSERT_EQUAL_HEX16(0x5000, g_cpu->pc);
    TEST_ASSERT_EQUAL_HEX8(0x47, g_cpu->r);
}

/**
 * @test Přijetí NMI zvýší R o 1.
 */
void test_nmi_ack_increments_r(void)
{
    g_cpu->external_int_handling = true;
    prepare_halt(0x7F);

    z80_step(g_cpu);                      /* HALT: 0x7F -> 0x00 */
    z80_nmi(g_cpu);
    TEST_ASSERT_TRUE(z80_process_interrupt(g_cpu) > 0);
    TEST_ASSERT_EQUAL_HEX16(0x0066, g_cpu->pc);
    TEST_ASSERT_EQUAL_HEX8(0x01, g_cpu->r);
}

/**
 * @test Interní zpracování přerušení (batch, external_int_handling = false)
 *       zvýší R při přijetí INT o 1 (z80_execute pak vykoná ještě NOP
 *       na 0038h, ten přidá další 1).
 */
void test_int_ack_internal_handling_increments_r(void)
{
    g_cpu->external_int_handling = false;
    g_mem[0x0200] = 0x00;                 /* NOP */
    g_cpu->pc = 0x0200; g_cpu->sp = 0x8000;
    g_cpu->im = 1;
    g_cpu->iff1 = 1; g_cpu->iff2 = 1;
    g_cpu->r = 0x10;

    z80_int(g_cpu);
    z80_execute(g_cpu, 1);                /* vstupní check přijme INT, pak NOP na 0038h */
    TEST_ASSERT_EQUAL_HEX16(0x0039, g_cpu->pc);
    TEST_ASSERT_EQUAL_HEX8(0x12, g_cpu->r);   /* +1 INT ack, +1 NOP */
}


int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_halt_refetch_increments_r_step);
    RUN_TEST(test_halt_refetch_increments_r_batch);
    RUN_TEST(test_halt_refetch_keeps_bit7);
    RUN_TEST(test_int_ack_increments_r);
    RUN_TEST(test_int_ack_im2_increments_r);
    RUN_TEST(test_nmi_ack_increments_r);
    RUN_TEST(test_int_ack_internal_handling_increments_r);
    return UNITY_END();
}
