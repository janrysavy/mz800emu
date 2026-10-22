/**
 * @file   test_ramlog_hook.c
 * @brief  Unit testy napojení záznamu zápisů (ramlog_hook.h) na jádro MZ-800.
 *
 * Ověřuje:
 * - start přepne write callback (háček A) a vyřadí z RAM fast-path jen
 *   sledovanou stránku, stop vše vrátí,
 * - zápis do RAM: insn_pc, CPU adresa, buňka, old/new; mimo filtr nic,
 * - zápis pod ROM se zaznamená jako potlačený a RAM se nezmění,
 * - zápis do grafické VRAM v režimu REPLACE dá záznam za každý zapsaný plán,
 * - zdroje: přijetí přerušení (ramlog_source_push), zápis debuggerem
 *   (memory_write_byte_no_bp), souhrnný záznam memory_load_block.
 *
 * Vyprázdnění při resetu a načtení snapshotu ověřuje e2e test přes MCP.
 *
 * Stav mapování a GDG si test nastavuje přímo a v tearDown vrací.
 *
 * @par Licence: GPLv3
 */

#include "mztest.h"

#include <string.h>

#include "debugger/trace/ramlog.h"
#include "debugger/trace/ramlog_hook.h"
#include "mzarch/mzarch.h"
#include "hw-generic/memory/memory.h"
#include "mzarch/mz800/gdg/mz800_gdg.h"
#include "mzarch/mz800/gdg/mz800_vramctrl.h"
#include "libs/cpu-z80/z80.h"

/** @brief Uložené mapování a registry GDG (obnoví tearDown). */
static uint8_t s_map;
static unsigned s_dmd;
static unsigned s_wf_plane, s_vbank;
static WFR_MODE s_wf_mode;


/** @brief Konfigurace s jedním CPU rozsahem. */
static st_RAMLOG_CONFIG cfg_cpu(uint32_t from, uint32_t to)
{
    st_RAMLOG_CONFIG c;
    memset(&c, 0, sizeof(c));
    c.mode = RAMLOG_MODE_RING;
    c.capacity = 1024;
    c.n_ranges = 1;
    c.ranges[0].from = from;
    c.ranges[0].to = to;
    return c;
}


/** @brief Buňka RAM na CPU adrese podle memram_write (hlavní RAM nebo banka MemExt). */
static uint8_t *ram_cell(uint16_t addr)
{
    return g_memory.memram_write[addr >> 12] + (addr & 0x0FFFu);
}


/** @brief Přestaví RAM fast-path po ruční změně mapování. */
static void remap(uint8_t map)
{
    g_memory.map = map;
    mz800_ram_fastpath_rebuild();
}


void setUp(void)
{
    s_map = g_memory.map;
    s_dmd = g_gdg.regDMD;
    s_wf_plane = g_vramctrl.regWF_PLANE;
    s_wf_mode = g_vramctrl.regWF_MODE;
    s_vbank = g_vramctrl.regWFRF_VBANK;
    remap(0);      /* vše RAM */
}


void tearDown(void)
{
    ramlog_hook_stop();
    ramlog_free();
    g_gdg.regDMD = s_dmd;
    g_vramctrl.regWF_PLANE = s_wf_plane;
    g_vramctrl.regWF_MODE = s_wf_mode;
    g_vramctrl.regWFRF_VBANK = s_vbank;
    remap(s_map);
}


/** @brief Start přepne callback a masku fast-path, zápis do RAM se zaznamená. */
void test_ramlog_hook_ram_write_and_fastpath(void)
{
    z80_t *cpu = g_mzarch_main.cpu;
    st_RAMLOG_CONFIG c = cfg_cpu(0x4000, 0x4001);
    TEST_ASSERT_TRUE(ramlog_hook_start(&c));
    TEST_ASSERT_TRUE(cpu->mwrite_cb == ramlog_hook_cpu_write_cb);
    TEST_ASSERT_TRUE(cpu->mread_cb == memory_read_cb);
    TEST_ASSERT_TRUE(cpu->ram_fp_enabled);
    TEST_ASSERT_NULL(cpu->ram_fp_write[4]);              /* sledovaná stránka */
    TEST_ASSERT_NOT_NULL(cpu->ram_fp_write[5]);
    TEST_ASSERT_NOT_NULL(cpu->ram_fp_read[4]);           /* čtení zůstává */

    /* Buňka podle memram_write: s připojeným MemExt (testovací konfigurace)
     * aktivní banka MemExt, jinak hlavní RAM. */
    uint8_t *cell = ram_cell(0x4000);
    uint8_t exp_space = 0;
    uint32_t exp_off = 0;
    TEST_ASSERT_TRUE(ramlog_platform_ram_cell(cell, &exp_space, &exp_off));
    *cell = 0xAA;
    g_mzarch_main.instruction_addr = 0x3003;
    ramlog_hook_cpu_write_cb(cpu, 0x4000, 0x55, NULL);
    ramlog_hook_cpu_write_cb(cpu, 0x4002, 0x66, NULL);   /* mimo filtr */
    TEST_ASSERT_EQUAL_UINT8(0x55, *cell);
    TEST_ASSERT_EQUAL_UINT8(0x66, *ram_cell(0x4002));

    st_RAMLOG_STATUS st;
    ramlog_get_status(&st);
    TEST_ASSERT_EQUAL_UINT64(1, st.next_seq);
    st_RAMLOG_REC r;
    TEST_ASSERT_TRUE(ramlog_get(0, &r));
    TEST_ASSERT_EQUAL_UINT16(0x3003, r.insn_pc);
    TEST_ASSERT_EQUAL_UINT16(0x4000, r.cpu_addr);
    TEST_ASSERT_EQUAL_UINT8(exp_space, r.space);
    TEST_ASSERT_EQUAL_UINT32(exp_off, r.offset);
    TEST_ASSERT_EQUAL_UINT8(0xAA, r.old_value);
    TEST_ASSERT_EQUAL_UINT8(0x55, r.new_value);
    TEST_ASSERT_EQUAL_UINT8(RAMLOG_SRC_INSN, r.src);
    TEST_ASSERT_EQUAL_UINT8(0, r.flags);

    ramlog_hook_stop();
    TEST_ASSERT_TRUE(cpu->mwrite_cb == memory_write_cb);
    TEST_ASSERT_NOT_NULL(cpu->ram_fp_write[4]);
    /* Data zůstanou čitelná. */
    TEST_ASSERT_TRUE(ramlog_get(0, &r));
}


/** @brief Zápis pod ROM: potlačený záznam, RAM beze změny. */
void test_ramlog_hook_rom_suppressed(void)
{
    remap(MEMORY_MZ800_MAP_FLAG_ROM_0000);
    st_RAMLOG_CONFIG c = cfg_cpu(0x0000, 0x0FFF);
    TEST_ASSERT_TRUE(ramlog_hook_start(&c));
    *ram_cell(0x0005) = 0x11;
    ramlog_hook_cpu_write_cb(g_mzarch_main.cpu, 0x0005, 0x99, NULL);
    TEST_ASSERT_EQUAL_UINT8(0x11, *ram_cell(0x0005));
    st_RAMLOG_REC r;
    TEST_ASSERT_TRUE(ramlog_get(0, &r));
    TEST_ASSERT_EQUAL_UINT8(RAMLOG_SPACE_ROM, r.space);
    TEST_ASSERT_TRUE(r.flags & RAMLOG_FLAG_SUPPRESSED);
    TEST_ASSERT_EQUAL_UINT8(0x99, r.new_value);
}


/** @brief Grafická VRAM, režim REPLACE: zapsaný i nevybraný plán banky A. */
void test_ramlog_hook_vram_planes(void)
{
    remap(MEMORY_MZ800_MAP_FLAG_CGRAM_VRAM);              /* VRAM na 8000h v režimu 800 */
    g_gdg.regDMD = 0;                                      /* 320x200, 4 barvy, banka A */
    g_vramctrl.regWFRF_VBANK = 0;
    g_vramctrl.regWF_MODE = GDG_WF_MODE_REPLACE;
    g_vramctrl.regWF_PLANE = 0x01;                         /* plán I */
    unsigned phy = 0, we = 0;
    vramctrl_mz800_write_target(0x0010, &phy, &we);
    TEST_ASSERT_EQUAL_UINT(0x03, we);                      /* REPLACE zapíše I (hodnota) i II (0) */

    g_memory.VRAM[phy] = 0x0F;
    g_memory.VRAM[MEMORY_SIZE_VRAM_BANK + phy] = 0xF0;
    st_RAMLOG_CONFIG c = cfg_cpu(0x8000, 0x80FF);
    TEST_ASSERT_TRUE(ramlog_hook_start(&c));
    ramlog_hook_cpu_write_cb(g_mzarch_main.cpu, 0x8010, 0xAA, NULL);
    st_RAMLOG_STATUS st;
    ramlog_get_status(&st);
    TEST_ASSERT_EQUAL_UINT64(2, st.next_seq);
    st_RAMLOG_REC r0, r1;
    TEST_ASSERT_TRUE(ramlog_get(0, &r0));
    TEST_ASSERT_TRUE(ramlog_get(1, &r1));
    TEST_ASSERT_EQUAL_UINT8(RAMLOG_SPACE_VRAM, r0.space);
    TEST_ASSERT_TRUE(r0.flags & RAMLOG_FLAG_PLANE);
    TEST_ASSERT_EQUAL_UINT8(0, r0.plane);
    TEST_ASSERT_EQUAL_UINT32(phy, r0.offset);
    TEST_ASSERT_EQUAL_UINT8(0x0F, r0.old_value);
    TEST_ASSERT_EQUAL_UINT8(0xAA, r0.new_value);
    TEST_ASSERT_EQUAL_UINT8(1, r1.plane);
    TEST_ASSERT_EQUAL_UINT32(MEMORY_SIZE_VRAM_BANK + phy, r1.offset);
    TEST_ASSERT_EQUAL_UINT8(0xF0, r1.old_value);
    TEST_ASSERT_EQUAL_UINT8(0x00, r1.new_value);
    TEST_ASSERT_EQUAL_UINT8(0x00, g_memory.VRAM[MEMORY_SIZE_VRAM_BANK + phy]);
}


/** @brief Zdroje mimo instrukci: přijetí přerušení, debugger, načtení bloku. */
void test_ramlog_hook_sources_and_reset(void)
{
    st_RAMLOG_CONFIG c = cfg_cpu(0x4000, 0x40FF);
    TEST_ASSERT_TRUE(ramlog_hook_start(&c));
    uint32_t saved = ramlog_source_push(RAMLOG_SRC_INT_ACK, 0x1234);
    ramlog_hook_cpu_write_cb(g_mzarch_main.cpu, 0x40FE, 0x34, NULL);
    ramlog_source_pop(saved);
    memory_write_byte_no_bp(0x4001, 0x77);
    uint8_t data[16];
    memset(data, 0x5A, sizeof(data));
    memory_load_block(data, 0x4020, sizeof(data), MEMORY_LOAD_RAMONLY);
    TEST_ASSERT_EQUAL_UINT8(0x5A, *ram_cell(0x402F));

    st_RAMLOG_REC r;
    TEST_ASSERT_TRUE(ramlog_get(0, &r));
    TEST_ASSERT_EQUAL_UINT8(RAMLOG_SRC_INT_ACK, r.src);
    TEST_ASSERT_EQUAL_UINT16(0x1234, r.insn_pc);
    TEST_ASSERT_TRUE(ramlog_get(1, &r));
    TEST_ASSERT_EQUAL_UINT8(RAMLOG_SRC_DEBUGGER, r.src);
    TEST_ASSERT_EQUAL_UINT16(0, r.insn_pc);
    TEST_ASSERT_EQUAL_UINT8(0x77, r.new_value);
    TEST_ASSERT_TRUE(ramlog_get(2, &r));
    TEST_ASSERT_EQUAL_UINT8(RAMLOG_SRC_LOAD, r.src);
    TEST_ASSERT_TRUE(r.flags & RAMLOG_FLAG_SUMMARY);
    TEST_ASSERT_EQUAL_UINT16(16, r.count);
    st_RAMLOG_MAPENT m;
    uint8_t sp = 0;
    uint32_t off = 0;
    TEST_ASSERT_TRUE(ramlog_platform_ram_cell(ram_cell(0x402F), &sp, &off));
    TEST_ASSERT_TRUE(ramlog_lastwriter((en_RAMLOG_SPACE) sp, off, &m));
    TEST_ASSERT_EQUAL_UINT8(RAMLOG_SRC_LOAD, m.src);
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_ramlog_hook_ram_write_and_fastpath);
    RUN_TEST(test_ramlog_hook_rom_suppressed);
    RUN_TEST(test_ramlog_hook_vram_planes);
    RUN_TEST(test_ramlog_hook_sources_and_reset);
    return UNITY_END();
}
