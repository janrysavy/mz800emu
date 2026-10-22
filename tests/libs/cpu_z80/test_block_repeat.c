/*
 * Copyright (c) 2026 Michal Hucik
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/**
 * @file test_block_repeat.c
 * @brief Testy příznaků v opakující iteraci blokových instrukcí Z80.
 *
 * Když se LDIR, LDDR, CPIR, CPDR, INIR, INDR, OTIR nebo OTDR opakuje, 5T
 * cyklus, který vrací PC o 2 zpět, přepíše část F (měření na reálných
 * procesorech Z80, ověřeno programem BLKFLAGS na MZ-800):
 * - všech 8 instrukcí: YF (bit 5) = PC.13, XF (bit 3) = PC.11, kde PC je
 *   adresa prefixu EDh,
 * - INxR a OTxR navíc mění PF a HF podle CF, bitu 7 přeneseného bajtu
 *   a dolních bitů už dekrementovaného B.
 * Program to vidí, když se instrukce zastaví mezi iteracemi (přijetí INT
 * nebo NMI, krok v debuggeru, instrukce přepíše svůj vlastní opkód).
 * Poslední (neopakující) iterace a jednorázové LDI ... OUTD se nemění.
 *
 * Očekávané hodnoty jsou měření, ne výpočet z modelu jádra:
 * - tabulky "MZ-800": výsledek BLKFLAGS na skutečném MZ-800 (obsluha IM 1
 *   z CTC2 a instrukce, která přepíše svůj druhý bajt), B a L u OTxR jsou
 *   hodnoty zachycené obsluhou přerušení,
 * - tabulka "Ped7g": Z80 Block Flags Test v5.0, hodnoty z reálných čipů Z80
 *   (A = 0A1h, F = 00h, BC = 16, data 0DEh).
 * Báze mz800-knowledge: cpu/z80/04-instructions/09-block-transfer.md
 * a 10-block-io.md (oddíly o opakující iteraci).
 *
 * Každý případ běží v per-step variantě jádra (z80_step, cesta MZ emulace)
 * i v dávkové variantě (z80_execute); obě vznikají ze z80_execute.inc.
 */

#include "unity.h"

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "libs/cpu-z80/z80.h"


/* ===== Test paměť + IO simulace (vzor test_r_register.c) ===== */

static uint8_t g_mem[0x10000];
static uint8_t g_port_in;       /**< Hodnota, kterou vrací čtení libovolného portu. */

static uint8_t test_mread(struct z80_s *cpu, uint16_t addr, int m1, void *u)
{ (void)cpu; (void)m1; (void)u; return g_mem[addr]; }

static void test_mwrite(struct z80_s *cpu, uint16_t addr, uint8_t v, void *u)
{ (void)cpu; (void)u; g_mem[addr] = v; }

static uint8_t test_pread(struct z80_s *cpu, uint16_t port, void *u)
{ (void)cpu; (void)port; (void)u; return g_port_in; }

static void test_pwrite(struct z80_s *cpu, uint16_t port, uint8_t v, void *u)
{ (void)cpu; (void)port; (void)v; (void)u; }

static uint8_t test_intread(struct z80_s *cpu, void *u)
{ (void)cpu; (void)u; return 0xFF; }


/* ===== setUp / tearDown ===== */

static z80_t *g_cpu;

void setUp(void)
{
    memset(g_mem, 0, sizeof(g_mem));
    g_port_in = 0xFF;
    g_cpu = z80_create(test_mread, NULL, test_mwrite, NULL,
                       test_pread, NULL, test_pwrite, NULL,
                       test_intread, NULL);
    TEST_ASSERT_NOT_NULL(g_cpu);
    /* Přerušení obsluhuje volající (jako MZ emulace), z80_execute(cpu, 1)
     * pak provede právě jednu iteraci. */
    g_cpu->external_int_handling = true;
    g_cpu->sp = 0xF000;
}

void tearDown(void)
{
    z80_destroy(g_cpu);
    g_cpu = NULL;
}


/* ===== Pomocné funkce ===== */

/**
 * @brief Provede jednu instrukci (jednu iteraci blokové instrukce).
 *
 * @param batch false = per-step varianta (z80_step), true = dávková
 *              varianta (z80_execute s cílem 1 T, skončí po první instrukci).
 * @return Počet T-stavů provedené instrukce.
 */
static int run_one(bool batch)
{
    return batch ? z80_execute(g_cpu, 1) : z80_step(g_cpu);
}

/**
 * @brief Uloží blokovou instrukci ED op na adresu pc a nastaví PC.
 */
static void put_block_insn(uint16_t pc, uint8_t op)
{
    g_mem[pc] = 0xED;
    g_mem[(uint16_t)(pc + 1)] = op;
    g_cpu->pc = pc;
}


/* ===== LDxR a CPxR ===== */

/**
 * @brief Případ LDxR/CPxR: instrukce na pc, BC = 16, (HL) = data, F = 00h.
 */
typedef struct {
    uint8_t  op;        /**< Druhý bajt opkódu (B0h LDIR, B1h CPIR, B8h LDDR, B9h CPDR). */
    uint16_t pc;        /**< Adresa prefixu EDh. */
    uint8_t  a;         /**< Registr A. */
    uint8_t  data;      /**< Bajt na (HL). */
    uint8_t  exp_f;     /**< F po opakující iteraci (měřeno). */
} st_LDCP_CASE;

static const st_LDCP_CASE s_ldcp_cases[] = {
    /* Ped7g, reálné čipy Z80 */
    { 0xB0, 0x8002, 0xA1, 0xDE, 0x04 }, { 0xB0, 0x8802, 0xA1, 0xDE, 0x0C },
    { 0xB0, 0xA002, 0xA1, 0xDE, 0x24 }, { 0xB0, 0xA802, 0xA1, 0xDE, 0x2C },
    { 0xB8, 0x8002, 0xA1, 0xDE, 0x04 }, { 0xB8, 0x8802, 0xA1, 0xDE, 0x0C },
    { 0xB8, 0xA002, 0xA1, 0xDE, 0x24 }, { 0xB8, 0xA802, 0xA1, 0xDE, 0x2C },
    { 0xB1, 0x8002, 0xA1, 0xDE, 0x96 }, { 0xB1, 0x8802, 0xA1, 0xDE, 0x9E },
    { 0xB1, 0xA002, 0xA1, 0xDE, 0xB6 }, { 0xB1, 0xA802, 0xA1, 0xDE, 0xBE },
    { 0xB9, 0x8002, 0xA1, 0xDE, 0x96 }, { 0xB9, 0x8802, 0xA1, 0xDE, 0x9E },
    { 0xB9, 0xA002, 0xA1, 0xDE, 0xB6 }, { 0xB9, 0xA802, 0xA1, 0xDE, 0xBE },
    /* MZ-800, BLKFLAGS část B (přerušení IM 1) */
    { 0xB1, 0x402F, 0xA1, 0xDE, 0x96 }, { 0xB1, 0x482F, 0xA1, 0xDE, 0x9E },
    { 0xB1, 0x602F, 0xA1, 0xDE, 0xB6 }, { 0xB1, 0x682F, 0xA1, 0xDE, 0xBE },
    { 0xB9, 0x402F, 0xA1, 0xDE, 0x96 }, { 0xB9, 0x482F, 0xA1, 0xDE, 0x9E },
    { 0xB9, 0x602F, 0xA1, 0xDE, 0xB6 }, { 0xB9, 0x682F, 0xA1, 0xDE, 0xBE },
    { 0xB0, 0x402F, 0x0A, 0x00, 0x04 }, { 0xB0, 0x482F, 0x0A, 0x00, 0x0C },
    { 0xB0, 0x602F, 0x0A, 0x00, 0x24 }, { 0xB0, 0x682F, 0x0A, 0x00, 0x2C },
};

/**
 * @brief Projde tabulku LDxR/CPxR jednou variantou jádra.
 */
static void check_ldcp_cases(bool batch)
{
    for (size_t i = 0; i < sizeof(s_ldcp_cases) / sizeof(s_ldcp_cases[0]); i++) {
        const st_LDCP_CASE *c = &s_ldcp_cases[i];
        char msg[64];
        snprintf(msg, sizeof(msg), "case %u op %02X pc %04X batch %d",
                 (unsigned)i, c->op, c->pc, (int)batch);
        memset(g_mem, 0, sizeof(g_mem));
        put_block_insn(c->pc, c->op);
        g_cpu->af.h = c->a;
        g_cpu->af.l = 0x00;
        g_cpu->bc.w = 16;
        g_cpu->hl.w = 0x3100;
        g_cpu->de.w = 0x3200;
        g_mem[0x3100] = c->data;

        TEST_ASSERT_EQUAL_INT_MESSAGE(21, run_one(batch), msg);
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(c->pc, g_cpu->pc, msg);
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(15, g_cpu->bc.w, msg);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(c->exp_f, g_cpu->af.l, msg);
    }
}

/** @test LDxR a CPxR, opakující iterace, per-step jádro. */
void test_ldcp_repeat_flags_step(void)  { check_ldcp_cases(false); }

/** @test LDxR a CPxR, opakující iterace, dávkové jádro. */
void test_ldcp_repeat_flags_batch(void) { check_ldcp_cases(true); }


/* ===== INxR a OTxR ===== */

/**
 * @brief Případ INxR/OTxR (hodnoty z BLKFLAGS na MZ-800).
 *
 * INxR čte port C (test vrací data), OTxR posílá (HL) = data. F před
 * instrukcí je 00h; INI/OUTI F nečtou, takže na něm nezáleží.
 */
typedef struct {
    uint8_t  op;        /**< B2h INIR, B3h OTIR, BAh INDR, BBh OTDR. */
    uint16_t pc;        /**< Adresa prefixu EDh. */
    uint8_t  f;         /**< F před instrukcí. */
    uint8_t  b;         /**< B před instrukcí (iterace ho sníží o 1). */
    uint8_t  c;         /**< C (port). */
    uint16_t hl;        /**< HL před instrukcí. */
    uint8_t  data;      /**< Přenesený bajt. */
    uint8_t  exp_f;     /**< F po opakující iteraci (měřeno na MZ-800). */
} st_IO_CASE;

static const st_IO_CASE s_io_cases[] = {
    /* Část A: instrukce přepíše svůj druhý bajt, port 0D1h vrací 0FFh. */
    { 0xB2, 0x4010, 0x00, 0x02, 0xD1, 0x4030, 0xFF, 0x07 },
    { 0xB2, 0x4810, 0x00, 0x02, 0xD1, 0x4830, 0xFF, 0x0F },
    { 0xB2, 0x6010, 0x00, 0x02, 0xD1, 0x6030, 0xFF, 0x27 },
    { 0xB2, 0x6810, 0x00, 0x02, 0xD1, 0x6830, 0xFF, 0x2F },
    { 0xB2, 0x4010, 0x00, 0x11, 0xD1, 0x4030, 0xFF, 0x13 },
    { 0xB2, 0x4810, 0x00, 0x11, 0xD1, 0x4830, 0xFF, 0x1B },
    { 0xB2, 0x6010, 0x00, 0x11, 0xD1, 0x6030, 0xFF, 0x33 },
    { 0xB2, 0x6810, 0x00, 0x11, 0xD1, 0x6830, 0xFF, 0x3B },
    { 0xBA, 0x4010, 0x00, 0x02, 0xD1, 0x4030, 0xFF, 0x07 },
    { 0xBA, 0x4810, 0x00, 0x02, 0xD1, 0x4830, 0xFF, 0x0F },
    { 0xBA, 0x6010, 0x00, 0x02, 0xD1, 0x6030, 0xFF, 0x27 },
    { 0xBA, 0x6810, 0x00, 0x02, 0xD1, 0x6830, 0xFF, 0x2F },
    { 0xBA, 0x4010, 0x00, 0x11, 0xD1, 0x4030, 0xFF, 0x13 },
    { 0xBA, 0x4810, 0x00, 0x11, 0xD1, 0x4830, 0xFF, 0x1B },
    { 0xBA, 0x6010, 0x00, 0x11, 0xD1, 0x6030, 0xFF, 0x33 },
    { 0xBA, 0x6810, 0x00, 0x11, 0xD1, 0x6830, 0xFF, 0x3B },
    /* Část B: přerušení IM 1, B a L zachycené obsluhou. */
    { 0xB3, 0x402F, 0x00, 0x9B, 0xF1, 0x9065, 0x00, 0x80 },
    { 0xB3, 0x482F, 0x00, 0x9B, 0xF1, 0x9065, 0x00, 0x88 },
    { 0xB3, 0x602F, 0x00, 0x9B, 0xF1, 0x9065, 0x00, 0xA0 },
    { 0xB3, 0x682F, 0x00, 0x9A, 0xF1, 0x9066, 0x00, 0xAC },
    { 0xB3, 0x402F, 0x00, 0xA1, 0xF1, 0x906F, 0xFF, 0x97 },
    { 0xB3, 0x482F, 0x00, 0xA2, 0xF1, 0x906E, 0xFF, 0x8B },
    { 0xB3, 0x602F, 0x00, 0xA1, 0xF1, 0x906F, 0xFF, 0xB7 },
    { 0xB3, 0x682F, 0x00, 0xA0, 0xF1, 0x9070, 0xFF, 0xAF },
    { 0xB3, 0x402F, 0x00, 0x25, 0xF1, 0x90DB, 0x47, 0x05 },
    { 0xB3, 0x482F, 0x00, 0x24, 0xF1, 0x90DC, 0x47, 0x09 },
    { 0xB3, 0x602F, 0x00, 0x23, 0xF1, 0x90DD, 0x47, 0x25 },
    { 0xB3, 0x682F, 0x00, 0x22, 0xF1, 0x90DE, 0x47, 0x29 },
    { 0xBB, 0x402F, 0x00, 0xA0, 0xF1, 0x90A0, 0x00, 0x84 },
    { 0xBB, 0x482F, 0x00, 0xA0, 0xF1, 0x90A0, 0x00, 0x8C },
    { 0xBB, 0x602F, 0x00, 0x9F, 0xF1, 0x909F, 0x00, 0xA0 },
    { 0xBB, 0x682F, 0x00, 0x9E, 0xF1, 0x909E, 0x00, 0xA8 },
    { 0xBB, 0x402F, 0x00, 0xA2, 0xF1, 0x90A2, 0xFF, 0x83 },
    { 0xBB, 0x482F, 0x00, 0xA2, 0xF1, 0x90A2, 0xFF, 0x8B },
    { 0xBB, 0x602F, 0x00, 0xA2, 0xF1, 0x90A2, 0xFF, 0xA3 },
    { 0xBB, 0x682F, 0x00, 0xA1, 0xF1, 0x90A1, 0xFF, 0xBF },
    { 0xBB, 0x402F, 0x00, 0x23, 0xF1, 0x90E2, 0x47, 0x05 },
    { 0xBB, 0x482F, 0x00, 0x22, 0xF1, 0x90E1, 0x47, 0x0D },
    { 0xBB, 0x602F, 0x00, 0x20, 0xF1, 0x90DF, 0x47, 0x31 },
    { 0xBB, 0x682F, 0x00, 0x2A, 0xF1, 0x90E9, 0x47, 0x29 },
};

/**
 * @brief Připraví případ INxR/OTxR do paměti a registrů.
 */
static void setup_io_case(const st_IO_CASE *c)
{
    memset(g_mem, 0, sizeof(g_mem));
    put_block_insn(c->pc, c->op);
    g_cpu->af.l = c->f;
    g_cpu->bc.h = c->b;
    g_cpu->bc.l = c->c;
    g_cpu->hl.w = c->hl;
    g_mem[c->hl] = c->data;
    g_port_in = c->data;
}

/**
 * @brief Projde tabulku INxR/OTxR jednou variantou jádra.
 */
static void check_io_cases(bool batch)
{
    for (size_t i = 0; i < sizeof(s_io_cases) / sizeof(s_io_cases[0]); i++) {
        const st_IO_CASE *c = &s_io_cases[i];
        char msg[64];
        snprintf(msg, sizeof(msg), "case %u op %02X pc %04X batch %d",
                 (unsigned)i, c->op, c->pc, (int)batch);
        setup_io_case(c);

        TEST_ASSERT_EQUAL_INT_MESSAGE(21, run_one(batch), msg);
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(c->pc, g_cpu->pc, msg);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE((uint8_t)(c->b - 1), g_cpu->bc.h, msg);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(c->exp_f, g_cpu->af.l, msg);
    }
}

/** @test INxR a OTxR, opakující iterace, per-step jádro. */
void test_io_repeat_flags_step(void)  { check_io_cases(false); }

/** @test INxR a OTxR, opakující iterace, dávkové jádro. */
void test_io_repeat_flags_batch(void) { check_io_cases(true); }


/* ===== Instrukce přepíše svůj druhý bajt (metoda z80test, BLKFLAGS část A) ===== */

/**
 * @test LDIR s BC = 2 zkopíruje 00h na svůj druhý bajt. Opakující iterace
 *       nastaví F, CPU pak načte ED 00h (NOP', 8 T), který F nemění.
 *       Hodnoty F z MZ-800 (A = 0Ah).
 */
void test_ldir_self_overwrite(void)
{
    static const uint16_t pcs[] = { 0x4010, 0x4810, 0x6010, 0x6810 };
    static const uint8_t  exp_f[] = { 0x04, 0x0C, 0x24, 0x2C };

    for (int batch = 0; batch < 2; batch++) {
        for (size_t i = 0; i < 4; i++) {
            memset(g_mem, 0, sizeof(g_mem));
            put_block_insn(pcs[i], 0xB0);
            g_cpu->af.h = 0x0A;
            g_cpu->af.l = 0x00;
            g_cpu->bc.w = 2;
            g_cpu->hl.w = 0x3100;                       /* zdroj: 00h */
            g_cpu->de.w = (uint16_t)(pcs[i] + 1);       /* cíl: druhý bajt LDIR */

            TEST_ASSERT_EQUAL_INT(21, run_one(batch));
            TEST_ASSERT_EQUAL_HEX8(0x00, g_mem[pcs[i] + 1]);
            TEST_ASSERT_EQUAL_INT(8, run_one(batch));   /* ED 00h = NOP' */
            TEST_ASSERT_EQUAL_HEX16((uint16_t)(pcs[i] + 2), g_cpu->pc);
            TEST_ASSERT_EQUAL_HEX8(exp_f[i], g_cpu->af.l);
        }
    }
}


/* ===== Poslední iterace a ukončení beze změny ===== */

/**
 * @test Poslední iterace (BC = 1 resp. B = 1) a CPIR se shodou: F jako
 *       u LDI/CPI/INI/OUTI (S. Young, The Undocumented Z80 Documented
 *       v0.91), PC za instrukcí, 16 T. Jádro je před opravou počítalo
 *       stejně; YF/XF tu nesmí záviset na adrese instrukce.
 */
void test_last_iteration_unchanged(void)
{
    for (int batch = 0; batch < 2; batch++) {
        /* LDIR, BC = 1, A = 0Ah, data 00h: n = 0Ah -> YF (bit 1), XF (bit 3) */
        memset(g_mem, 0, sizeof(g_mem));
        put_block_insn(0x8002, 0xB0);
        g_cpu->af.h = 0x0A; g_cpu->af.l = 0x00;
        g_cpu->bc.w = 1; g_cpu->hl.w = 0x3100; g_cpu->de.w = 0x3200;
        TEST_ASSERT_EQUAL_INT(16, run_one(batch));
        TEST_ASSERT_EQUAL_HEX16(0x8004, g_cpu->pc);
        TEST_ASSERT_EQUAL_HEX8(0x28, g_cpu->af.l);

        /* CPIR, BC = 1, A = 0A1h, data 0DEh */
        memset(g_mem, 0, sizeof(g_mem));
        put_block_insn(0xA802, 0xB1);
        g_cpu->af.h = 0xA1; g_cpu->af.l = 0x00;
        g_cpu->bc.w = 1; g_cpu->hl.w = 0x3100;
        g_mem[0x3100] = 0xDE;
        TEST_ASSERT_EQUAL_INT(16, run_one(batch));
        TEST_ASSERT_EQUAL_HEX16(0xA804, g_cpu->pc);
        TEST_ASSERT_EQUAL_HEX8(0xB2, g_cpu->af.l);

        /* CPIR se shodou A = (HL) při BC > 1: konec bez opakování */
        memset(g_mem, 0, sizeof(g_mem));
        put_block_insn(0xA802, 0xB1);
        g_cpu->af.h = 0xDE; g_cpu->af.l = 0x00;
        g_cpu->bc.w = 16; g_cpu->hl.w = 0x3100;
        g_mem[0x3100] = 0xDE;
        TEST_ASSERT_EQUAL_INT(16, run_one(batch));
        TEST_ASSERT_EQUAL_HEX16(0xA804, g_cpu->pc);
        TEST_ASSERT_EQUAL_HEX8(0x46, g_cpu->af.l);

        /* OTIR, B = 1, data 0FFh, L po inkrementu 11h */
        memset(g_mem, 0, sizeof(g_mem));
        put_block_insn(0xA802, 0xB3);
        g_cpu->af.l = 0x00;
        g_cpu->bc.h = 0x01; g_cpu->bc.l = 0xF1; g_cpu->hl.w = 0x3110;
        g_mem[0x3110] = 0xFF;
        TEST_ASSERT_EQUAL_INT(16, run_one(batch));
        TEST_ASSERT_EQUAL_HEX16(0xA804, g_cpu->pc);
        TEST_ASSERT_EQUAL_HEX8(0x57, g_cpu->af.l);

        /* INIR a INDR, B = 1, C = 0D1h, data 0FFh */
        static const uint8_t in_ops[] = { 0xB2, 0xBA };
        for (int k = 0; k < 2; k++) {
            memset(g_mem, 0, sizeof(g_mem));
            put_block_insn(0xA802, in_ops[k]);
            g_cpu->af.l = 0x00;
            g_cpu->bc.h = 0x01; g_cpu->bc.l = 0xD1; g_cpu->hl.w = 0x3100;
            g_port_in = 0xFF;
            TEST_ASSERT_EQUAL_INT(16, run_one(batch));
            TEST_ASSERT_EQUAL_HEX16(0xA804, g_cpu->pc);
            TEST_ASSERT_EQUAL_HEX8(0x53, g_cpu->af.l);
        }
    }
}


/* ===== MEMPTR (WZ) ===== */

/**
 * @test MEMPTR v opakující iteraci INIR/INDR/OTIR/OTDR je adresa instrukce
 *       + 1, stejně jako u LDxR/CPxR (měřeno na MZ-800, BLKFLAGS část A,
 *       a na procesorech Zilog a NEC). Poslední iterace nechává MEMPTR
 *       jako INI/IND/OUTI/OUTD: BC +- 1 (INxR B před dekrementem, OTxR po něm).
 */
void test_io_repeat_memptr(void)
{
    static const uint8_t ops[] = { 0xB2, 0xBA, 0xB3, 0xBB };

    for (int batch = 0; batch < 2; batch++) {
        for (size_t i = 0; i < 4; i++) {
            const uint8_t op = ops[i];
            const bool is_in = (op == 0xB2 || op == 0xBA);
            const bool inc = (op == 0xB2 || op == 0xB3);
            char msg[48];
            snprintf(msg, sizeof(msg), "op %02X batch %d", op, batch);

            /* Opakující iterace: B 10h -> 0Fh */
            memset(g_mem, 0, sizeof(g_mem));
            put_block_insn(0x6810, op);
            g_cpu->bc.h = 0x10; g_cpu->bc.l = 0xD1; g_cpu->hl.w = 0x3100;
            g_cpu->wz.w = 0x0000;
            TEST_ASSERT_EQUAL_INT_MESSAGE(21, run_one(batch), msg);
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(0x6810, g_cpu->pc, msg);
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(0x6811, g_cpu->wz.w, msg);

            /* Poslední iterace: B 01h -> 00h */
            memset(g_mem, 0, sizeof(g_mem));
            put_block_insn(0x6810, op);
            g_cpu->bc.h = 0x01; g_cpu->bc.l = 0xD1; g_cpu->hl.w = 0x3100;
            g_cpu->wz.w = 0x0000;
            TEST_ASSERT_EQUAL_INT_MESSAGE(16, run_one(batch), msg);
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(0x6812, g_cpu->pc, msg);
            const uint16_t bc = is_in ? 0x01D1 : 0x00D1;
            TEST_ASSERT_EQUAL_HEX16_MESSAGE((uint16_t)(inc ? bc + 1 : bc - 1),
                                            g_cpu->wz.w, msg);
        }
    }
}


int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ldcp_repeat_flags_step);
    RUN_TEST(test_ldcp_repeat_flags_batch);
    RUN_TEST(test_io_repeat_flags_step);
    RUN_TEST(test_io_repeat_flags_batch);
    RUN_TEST(test_ldir_self_overwrite);
    RUN_TEST(test_last_iteration_unchanged);
    RUN_TEST(test_io_repeat_memptr);
    return UNITY_END();
}
