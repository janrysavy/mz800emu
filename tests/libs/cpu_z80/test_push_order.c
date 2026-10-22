/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file test_push_order.c
 * @brief Unit testy pořadí zápisů 16bitové hodnoty na zásobník.
 *
 * Zilog UM0080 (Z80 CPU User Manual, UM008005-0205): PUSH qq, PUSH IX,
 * PUSH IY, CALL nn, CALL cc,nn, RST p i přijetí NMI nejdřív sníží SP
 * a zapíše **vyšší** bajt, pak znovu sníží SP a zapíše nižší bajt. IM 1 je
 * "identické s NMI" (restart na 0038h), IM 2 "provede CALL". Naopak
 * LD (nn),rr zapisuje nižší bajt na nn a vyšší na nn+1 v tomto pořadí.
 *
 * Pokrytí (pořadí zápisů z callbacku paměti, obsah paměti po instrukci):
 *   - PUSH BC, PUSH IX, CALL nn, CALL NZ,nn (splněno), RST 38h,
 *   - přijetí NMI, INT v IM 0 (vektor RST 38h), IM 1, IM 2,
 *   - LD (nn),HL a LD (nn),BC (ED 43) zůstávají nižší bajt dřív.
 *
 * Standalone test - cpu-z80 bez emulátoru, paměť simulovaná v testu.
 */

#include "unity.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "libs/cpu-z80/z80.h"


/** Plná 64 KB paměť testu. */
static uint8_t g_mem[0x10000];

/** Vektor přerušení pro IM 0 / IM 2. */
static uint8_t g_int_vector;

/** Největší počet zaznamenaných zápisů. */
#define MAX_WRITES 16

/** Jeden zápis do paměti v pořadí, jak ho CPU provedlo. */
typedef struct {
    uint16_t addr;  /**< Adresa. */
    uint8_t value;  /**< Zapsaná hodnota. */
} write_t;

/** Zápisy od posledního clear_writes(). */
static write_t g_writes[MAX_WRITES];
static int g_nwrites;


static uint8_t test_mread(struct z80_s *cpu, uint16_t addr, int m1_state, void *user_data)
{
    (void)cpu;
    (void)m1_state;
    (void)user_data;
    return g_mem[addr];
}


static void test_mwrite(struct z80_s *cpu, uint16_t addr, uint8_t value, void *user_data)
{
    (void)cpu;
    (void)user_data;
    g_mem[addr] = value;
    if (g_nwrites < MAX_WRITES) {
        g_writes[g_nwrites].addr = addr;
        g_writes[g_nwrites].value = value;
    }
    g_nwrites++;
}


static uint8_t test_pread(struct z80_s *cpu, uint16_t port, void *user_data)
{
    (void)cpu;
    (void)port;
    (void)user_data;
    return 0xFF;
}


static void test_pwrite(struct z80_s *cpu, uint16_t port, uint8_t value, void *user_data)
{
    (void)cpu;
    (void)port;
    (void)value;
    (void)user_data;
}


static uint8_t test_intread(struct z80_s *cpu, void *user_data)
{
    (void)cpu;
    (void)user_data;
    return g_int_vector;
}


static z80_t *g_cpu;


void setUp(void)
{
    memset(g_mem, 0, sizeof(g_mem));
    g_nwrites = 0;
    g_int_vector = 0xFF;
    g_cpu = z80_create(test_mread, NULL, test_mwrite, NULL, test_pread, NULL, test_pwrite, NULL,
                       test_intread, NULL);
    TEST_ASSERT_NOT_NULL(g_cpu);
}


void tearDown(void)
{
    z80_destroy(g_cpu);
    g_cpu = NULL;
}


/** Smaže záznam zápisů (před měřenou instrukcí). */
static void clear_writes(void)
{
    g_nwrites = 0;
}


/**
 * @brief Ověří, že poslední zápisy byly vyšší bajt na @p sp-1, pak nižší na @p sp-2.
 * @param sp  SP před instrukcí.
 * @param val Ukládaná 16bitová hodnota.
 */
static void assert_stack_push(uint16_t sp, uint16_t val)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, g_nwrites, "expected exactly two writes");
    TEST_ASSERT_EQUAL_HEX16_MESSAGE((uint16_t)(sp - 1), g_writes[0].addr, "first write must go to SP-1");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE((uint8_t)(val >> 8), g_writes[0].value, "first write must be the high byte");
    TEST_ASSERT_EQUAL_HEX16_MESSAGE((uint16_t)(sp - 2), g_writes[1].addr, "second write must go to SP-2");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE((uint8_t)(val & 0xFF), g_writes[1].value, "second write must be the low byte");
    /* Obsah paměti po instrukci je stejný jako dřív (little endian). */
    TEST_ASSERT_EQUAL_HEX16((uint16_t)(sp - 2), z80_get_reg(g_cpu, Z80_REG_SP));
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(val & 0xFF), g_mem[(uint16_t)(sp - 2)]);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(val >> 8), g_mem[(uint16_t)(sp - 1)]);
}


/** @test PUSH BC: vyšší bajt (B) na SP-1 dřív. */
void test_push_bc_high_byte_first(void)
{
    g_mem[0x0100] = 0xC5; /* PUSH BC */
    z80_set_reg(g_cpu, Z80_REG_PC, 0x0100);
    z80_set_reg(g_cpu, Z80_REG_SP, 0x8000);
    z80_set_reg(g_cpu, Z80_REG_BC, 0x1234);
    clear_writes();
    z80_step(g_cpu);
    assert_stack_push(0x8000, 0x1234);
}


/** @test PUSH IX: vyšší bajt na SP-1 dřív. */
void test_push_ix_high_byte_first(void)
{
    g_mem[0x0100] = 0xDD;
    g_mem[0x0101] = 0xE5; /* PUSH IX */
    z80_set_reg(g_cpu, Z80_REG_PC, 0x0100);
    z80_set_reg(g_cpu, Z80_REG_SP, 0x8000);
    z80_set_reg(g_cpu, Z80_REG_IX, 0xA1B2);
    clear_writes();
    z80_step(g_cpu);
    assert_stack_push(0x8000, 0xA1B2);
}


/** @test CALL nn: návratová adresa, vyšší bajt dřív. */
void test_call_high_byte_first(void)
{
    g_mem[0x1234] = 0xCD; /* CALL 2000h */
    g_mem[0x1235] = 0x00;
    g_mem[0x1236] = 0x20;
    z80_set_reg(g_cpu, Z80_REG_PC, 0x1234);
    z80_set_reg(g_cpu, Z80_REG_SP, 0x8000);
    clear_writes();
    z80_step(g_cpu);
    assert_stack_push(0x8000, 0x1237);
    TEST_ASSERT_EQUAL_HEX16(0x2000, z80_get_reg(g_cpu, Z80_REG_PC));
}


/** @test CALL NZ,nn (splněno): vyšší bajt dřív. */
void test_call_cc_high_byte_first(void)
{
    g_mem[0x1234] = 0xC4; /* CALL NZ,2000h */
    g_mem[0x1235] = 0x00;
    g_mem[0x1236] = 0x20;
    z80_set_reg(g_cpu, Z80_REG_PC, 0x1234);
    z80_set_reg(g_cpu, Z80_REG_SP, 0x8000);
    z80_set_reg(g_cpu, Z80_REG_AF, 0x0000); /* Z = 0 */
    clear_writes();
    z80_step(g_cpu);
    assert_stack_push(0x8000, 0x1237);
}


/** @test RST 38h: vyšší bajt dřív. */
void test_rst_high_byte_first(void)
{
    g_mem[0x1234] = 0xFF; /* RST 38h */
    z80_set_reg(g_cpu, Z80_REG_PC, 0x1234);
    z80_set_reg(g_cpu, Z80_REG_SP, 0x8000);
    clear_writes();
    z80_step(g_cpu);
    assert_stack_push(0x8000, 0x1235);
    TEST_ASSERT_EQUAL_HEX16(0x0038, z80_get_reg(g_cpu, Z80_REG_PC));
}


/**
 * @brief Připraví CPU na přijetí maskovaného přerušení v daném IM.
 *
 * Program: IM n, EI, NOP (odklad EI); PC pak 1234h... - INT se přijme
 * před další instrukcí na 0005h. Handlery jsou NOP (zápisy jen z přijetí).
 */
static void prepare_int(uint8_t im_opcode)
{
    g_mem[0x0000] = 0xED;
    g_mem[0x0001] = im_opcode; /* IM 0 / 1 / 2 */
    g_mem[0x0002] = 0xFB;      /* EI */
    g_mem[0x0003] = 0x00;      /* NOP - konec odkladu EI */
    z80_set_reg(g_cpu, Z80_REG_PC, 0x0000);
    z80_set_reg(g_cpu, Z80_REG_SP, 0x8000);
    z80_step(g_cpu);
    z80_step(g_cpu);
    z80_step(g_cpu);
    TEST_ASSERT_EQUAL_HEX16(0x0004, z80_get_reg(g_cpu, Z80_REG_PC));
}


/** @brief Spustí přijetí INT a vrátí jen zápisy přijetí (handler je NOP). */
static void accept_int(void)
{
    clear_writes();
    z80_int(g_cpu);
    z80_step(g_cpu);
}


/** @test Přijetí INT v IM 1: vyšší bajt dřív (jako NMI / RST 38h). */
void test_int_im1_high_byte_first(void)
{
    prepare_int(0x56); /* IM 1 */
    accept_int();
    assert_stack_push(0x8000, 0x0004);
}


/** @test Přijetí INT v IM 0 s vektorem RST 38h: vyšší bajt dřív. */
void test_int_im0_high_byte_first(void)
{
    g_int_vector = 0xFF; /* RST 38h na sběrnici */
    prepare_int(0x46);   /* IM 0 */
    accept_int();
    assert_stack_push(0x8000, 0x0004);
}


/** @test Přijetí INT v IM 2: vyšší bajt dřív (CALL přes tabulku). */
void test_int_im2_high_byte_first(void)
{
    g_int_vector = 0x40;
    g_mem[0x3040] = 0x00; /* tabulka: 3000h + vektor -> rutina 5000h */
    g_mem[0x3041] = 0x50;
    prepare_int(0x5E); /* IM 2 */
    z80_set_reg(g_cpu, Z80_REG_IR, (uint16_t)((0x30 << 8) | (z80_get_reg(g_cpu, Z80_REG_IR) & 0xFF)));
    accept_int();
    assert_stack_push(0x8000, 0x0004);
}


/** @test Přijetí NMI: vyšší bajt dřív (UM0080, příklad u RETN). */
void test_nmi_high_byte_first(void)
{
    g_mem[0x1234] = 0x00; /* NOP */
    z80_set_reg(g_cpu, Z80_REG_PC, 0x1234);
    z80_set_reg(g_cpu, Z80_REG_SP, 0x8000);
    clear_writes();
    z80_nmi(g_cpu);
    z80_step(g_cpu);
    assert_stack_push(0x8000, 0x1234);
}


/** @test LD (nn),HL zůstává nižší bajt na nn dřív. */
void test_ld_nn_hl_low_byte_first(void)
{
    g_mem[0x0100] = 0x22; /* LD (4000h),HL */
    g_mem[0x0101] = 0x00;
    g_mem[0x0102] = 0x40;
    z80_set_reg(g_cpu, Z80_REG_PC, 0x0100);
    z80_set_reg(g_cpu, Z80_REG_HL, 0xBEEF);
    clear_writes();
    z80_step(g_cpu);
    TEST_ASSERT_EQUAL_INT(2, g_nwrites);
    TEST_ASSERT_EQUAL_HEX16(0x4000, g_writes[0].addr);
    TEST_ASSERT_EQUAL_HEX8(0xEF, g_writes[0].value);
    TEST_ASSERT_EQUAL_HEX16(0x4001, g_writes[1].addr);
    TEST_ASSERT_EQUAL_HEX8(0xBE, g_writes[1].value);
}


/** @test LD (nn),BC (ED 43) zůstává nižší bajt na nn dřív. */
void test_ld_nn_bc_low_byte_first(void)
{
    g_mem[0x0100] = 0xED;
    g_mem[0x0101] = 0x43; /* LD (4000h),BC */
    g_mem[0x0102] = 0x00;
    g_mem[0x0103] = 0x40;
    z80_set_reg(g_cpu, Z80_REG_PC, 0x0100);
    z80_set_reg(g_cpu, Z80_REG_BC, 0x1234);
    clear_writes();
    z80_step(g_cpu);
    TEST_ASSERT_EQUAL_INT(2, g_nwrites);
    TEST_ASSERT_EQUAL_HEX16(0x4000, g_writes[0].addr);
    TEST_ASSERT_EQUAL_HEX8(0x34, g_writes[0].value);
    TEST_ASSERT_EQUAL_HEX16(0x4001, g_writes[1].addr);
    TEST_ASSERT_EQUAL_HEX8(0x12, g_writes[1].value);
}


int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_push_bc_high_byte_first);
    RUN_TEST(test_push_ix_high_byte_first);
    RUN_TEST(test_call_high_byte_first);
    RUN_TEST(test_call_cc_high_byte_first);
    RUN_TEST(test_rst_high_byte_first);
    RUN_TEST(test_int_im0_high_byte_first);
    RUN_TEST(test_int_im1_high_byte_first);
    RUN_TEST(test_int_im2_high_byte_first);
    RUN_TEST(test_nmi_high_byte_first);
    RUN_TEST(test_ld_nn_hl_low_byte_first);
    RUN_TEST(test_ld_nn_bc_low_byte_first);
    return UNITY_END();
}
