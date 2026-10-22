/*
 * File:   test_state_hash.c
 *
 * Sdílený hash SHA-256 částí stavu (state_hash.c): známé testovací vektory,
 * jména částí a definice "co patří do hashe" (RAM, MemExt jen připojený,
 * VRAM MZ-800 = VRAM + EXVRAM, PCG jen MZ-1500, obraz = framebuffer INDEX8).
 */

/**
 * @file test_state_hash.c
 * @brief Testy state_hash_buffer() a state_hash_part().
 */

#include "mztest.h"
#include <glib.h>
#include <string.h>

#include "emulator/state_hash.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/memory/memext.h"
#include "hw-generic/gdg/framebuffer.h"
#include "mzarch/mzarch.h"
#include "libs/cpu-z80/z80.h"


void setUp(void)
{
    memext_disconnect();
}

void tearDown(void)
{
    memext_disconnect();
}


/** @brief Standardní testovací vektory SHA-256 (prázdný vstup a "abc"). */
void test_state_hash_buffer_vectors(void)
{
    char hex[STATE_HASH_HEX_LEN + 1];
    state_hash_buffer(NULL, 0, hex);
    TEST_ASSERT_EQUAL_STRING("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", hex);
    state_hash_buffer("abc", 3, hex);
    TEST_ASSERT_EQUAL_STRING("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", hex);
}


/** @brief Jména částí v pořadí výčtu. */
void test_state_hash_part_names(void)
{
    TEST_ASSERT_EQUAL_STRING("ram", state_hash_part_name(STATE_HASH_RAM));
    TEST_ASSERT_EQUAL_STRING("memext", state_hash_part_name(STATE_HASH_MEMEXT));
    TEST_ASSERT_EQUAL_STRING("vram", state_hash_part_name(STATE_HASH_VRAM));
    TEST_ASSERT_EQUAL_STRING("pcg", state_hash_part_name(STATE_HASH_PCG));
    TEST_ASSERT_EQUAL_STRING("screen", state_hash_part_name(STATE_HASH_SCREEN));
    TEST_ASSERT_EQUAL_STRING("cpu", state_hash_part_name(STATE_HASH_CPU));
    TEST_ASSERT_EQUAL_STRING("unknown", state_hash_part_name(STATE_HASH_PART_COUNT));
}


/** @brief RAM = hash celé vnitřní RAM, změna bajtu změní hash. */
void test_state_hash_ram(void)
{
    char a[STATE_HASH_HEX_LEN + 1], b[STATE_HASH_HEX_LEN + 1], ref[STATE_HASH_HEX_LEN + 1];
    TEST_ASSERT_TRUE(state_hash_part(STATE_HASH_RAM, a));
    state_hash_buffer(g_memory.RAM, sizeof(g_memory.RAM), ref);
    TEST_ASSERT_EQUAL_STRING(ref, a);
    g_memory.RAM[0x1234] ^= 0xFF;
    TEST_ASSERT_TRUE(state_hash_part(STATE_HASH_RAM, b));
    g_memory.RAM[0x1234] ^= 0xFF;
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);
}


/** @brief MemExt jen s připojeným MemExt. */
void test_state_hash_memext_only_connected(void)
{
    char hex[STATE_HASH_HEX_LEN + 1] = "unchanged";
    TEST_ASSERT_FALSE(state_hash_part(STATE_HASH_MEMEXT, hex));
    TEST_ASSERT_EQUAL_STRING("unchanged", hex);
    memext_connect(MEMEXT_TYPE_PEHU);
    TEST_ASSERT_TRUE(state_hash_part(STATE_HASH_MEMEXT, hex));
    TEST_ASSERT_EQUAL_size_t(STATE_HASH_HEX_LEN, strlen(hex));
}


/** @brief VRAM a PCG podle platformy. */
void test_state_hash_vram_pcg(void)
{
    char hex[STATE_HASH_HEX_LEN + 1], ref[STATE_HASH_HEX_LEN + 1];
    TEST_ASSERT_TRUE(state_hash_part(STATE_HASH_VRAM, hex));
#if MZARCH == 800
    GChecksum *cs = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(cs, g_memory.VRAM, sizeof(g_memory.VRAM));
    g_checksum_update(cs, g_memory.EXVRAM, sizeof(g_memory.EXVRAM));
    g_strlcpy(ref, g_checksum_get_string(cs), sizeof(ref));
    g_checksum_free(cs);
#else
    state_hash_buffer(g_memory.VRAM, sizeof(g_memory.VRAM), ref);
#endif
    TEST_ASSERT_EQUAL_STRING(ref, hex);
#if MZARCH == 1500
    TEST_ASSERT_TRUE(state_hash_part(STATE_HASH_PCG, hex));
#else
    TEST_ASSERT_FALSE(state_hash_part(STATE_HASH_PCG, hex));
#endif
}


/** @brief Obraz = framebuffer INDEX8 celého displeje. */
void test_state_hash_screen(void)
{
    char hex[STATE_HASH_HEX_LEN + 1], ref[STATE_HASH_HEX_LEN + 1];
    TEST_ASSERT_NOT_NULL(g_framebuffer.pixels);
    TEST_ASSERT_TRUE(state_hash_part(STATE_HASH_SCREEN, hex));
    state_hash_buffer(g_framebuffer.pixels, GDG_FRAMEBUFFER_PIXBUF_SIZE, ref);
    TEST_ASSERT_EQUAL_STRING(ref, hex);
}




/** @brief CPU = registry a skrytý stav; čítače T-stavů hash nemění. */
void test_state_hash_cpu(void)
{
    z80_t *cpu = g_mzarch_main.cpu;
    TEST_ASSERT_NOT_NULL(cpu);
    char a[STATE_HASH_HEX_LEN + 1], b[STATE_HASH_HEX_LEN + 1];
    unsigned char bytes[STATE_HASH_CPU_BYTES];

    z80_set_reg(cpu, Z80_REG_PC, 0x1234);
    state_hash_cpu_bytes(bytes);
    TEST_ASSERT_EQUAL_HEX8(0x34, bytes[22]);
    TEST_ASSERT_EQUAL_HEX8(0x12, bytes[23]);

    TEST_ASSERT_TRUE(state_hash_part(STATE_HASH_CPU, a));
    uint32_t saved_total = cpu->total_cycles;
    cpu->total_cycles += 1000;
    TEST_ASSERT_TRUE(state_hash_part(STATE_HASH_CPU, b));
    cpu->total_cycles = saved_total;
    TEST_ASSERT_EQUAL_STRING(a, b);

    uint16_t wz = z80_get_reg(cpu, Z80_REG_WZ);
    z80_set_reg(cpu, Z80_REG_WZ, (uint16_t)(wz ^ 0x0101));
    TEST_ASSERT_TRUE(state_hash_part(STATE_HASH_CPU, b));
    z80_set_reg(cpu, Z80_REG_WZ, wz);
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);

    bool ei = cpu->ei_delay;
    cpu->ei_delay = !ei;
    TEST_ASSERT_TRUE(state_hash_part(STATE_HASH_CPU, b));
    cpu->ei_delay = ei;
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_state_hash_buffer_vectors);
    RUN_TEST(test_state_hash_part_names);
    RUN_TEST(test_state_hash_ram);
    RUN_TEST(test_state_hash_memext_only_connected);
    RUN_TEST(test_state_hash_vram_pcg);
    RUN_TEST(test_state_hash_screen);
    RUN_TEST(test_state_hash_cpu);
    int result = UNITY_END();

    mztest_teardown();
    return result;
}
