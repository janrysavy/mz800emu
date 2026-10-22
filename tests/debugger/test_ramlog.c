/**
 * @file   test_ramlog.c
 * @brief  Unit testy jádra záznamu zápisů do paměti (ramlog.h) bez emulátoru.
 *
 * Ověřuje:
 * - ring v režimu ring (přepis nejstarších, first_seq) a fill (dropped),
 * - mapu posledního zapisovatele i po přetečení ringu, líně alokovaný MemExt,
 * - filtr CPU a fyzických rozsahů a masku stránek pro RAM fast-path,
 * - dotazy who_wrote (od nejnovějšího, skrytí nezměněných) a writers,
 * - souhrnný záznam bloku (LOAD), potlačený zápis (bez mapy),
 * - clear (nový segment), stop (data zůstanou), kopii úseku ringu.
 *
 * @par Licence: GPLv3
 */

#include "unity.h"

#include <string.h>

#include "ramlog.h"

/** @brief Velikosti prostorů jako MZ-800 (RAM 64 KB, VRAM 32 KB, MemExt 512 KB). */
static const uint32_t SIZES[RAMLOG_SPACE_COUNT] = { 0x10000, 0x8000, 0, 0x80000, 0 };


void setUp(void) {}

void tearDown(void)
{
    ramlog_free();
}


/** @brief Záznam zápisu do RAM. */
static st_RAMLOG_REC rec(uint32_t off, uint16_t pc, uint8_t oldv, uint8_t newv)
{
    st_RAMLOG_REC r;
    memset(&r, 0, sizeof(r));
    r.t = 1000u + off;
    r.insn_pc = pc;
    r.cpu_addr = (uint16_t) off;
    r.offset = off;
    r.space = RAMLOG_SPACE_RAM;
    r.old_value = oldv;
    r.new_value = newv;
    r.src = RAMLOG_SRC_INSN;
    r.count = 1;
    if (oldv == newv) r.flags |= RAMLOG_FLAG_UNCHANGED;
    return r;
}


/** @brief Konfigurace s kapacitou a režimem, bez rozsahů. */
static st_RAMLOG_CONFIG cfg(en_RAMLOG_MODE mode, uint32_t cap)
{
    st_RAMLOG_CONFIG c;
    memset(&c, 0, sizeof(c));
    c.mode = mode;
    c.capacity = cap;
    return c;
}


/** @brief Režim ring: přepis nejstarších, mapa platí i pro přepsané. */
void test_ramlog_ring_mode(void)
{
    st_RAMLOG_CONFIG c = cfg(RAMLOG_MODE_RING, 16);
    TEST_ASSERT_TRUE(ramlog_start(&c, SIZES));
    TEST_ASSERT_EQUAL_INT(1, g_ramlog_active);
    for (unsigned i = 0; i < 20; i++) {
        st_RAMLOG_REC r = rec(0x4000 + i, (uint16_t) (0x3000 + i), 0, (uint8_t) i);
        ramlog_record(&r);
    }
    st_RAMLOG_STATUS st;
    ramlog_get_status(&st);
    TEST_ASSERT_EQUAL_UINT64(4, st.first_seq);
    TEST_ASSERT_EQUAL_UINT64(20, st.next_seq);
    TEST_ASSERT_EQUAL_UINT64(4, st.overwritten);
    TEST_ASSERT_EQUAL_UINT64(0, st.dropped);
    st_RAMLOG_REC r;
    TEST_ASSERT_FALSE(ramlog_get(3, &r));
    TEST_ASSERT_TRUE(ramlog_get(4, &r));
    TEST_ASSERT_EQUAL_UINT16(0x3004, r.insn_pc);
    /* Zápis seq 0 už v ringu není, mapa ho zná. */
    st_RAMLOG_MAPENT m;
    TEST_ASSERT_TRUE(ramlog_lastwriter(RAMLOG_SPACE_RAM, 0x4000, &m));
    TEST_ASSERT_EQUAL_UINT32(1, m.seq1);
    TEST_ASSERT_EQUAL_UINT16(0x3000, m.insn_pc);
    TEST_ASSERT_FALSE(ramlog_lastwriter(RAMLOG_SPACE_RAM, 0x5000, &m));
}


/** @brief ramlog_recent: od nejnovějšího, skrytí nezměněných, omezení průchodu, přetečený ring. */
void test_ramlog_recent(void)
{
    st_RAMLOG_CONFIG c = cfg(RAMLOG_MODE_RING, 16);
    TEST_ASSERT_TRUE(ramlog_start(&c, SIZES));
    /* 20 záznamů, liché nezměněné; v ringu zůstane seq 4-19. */
    for (unsigned i = 0; i < 20; i++) {
        st_RAMLOG_REC r = rec(0x4000 + i, (uint16_t) (0x3000 + i), 0, (uint8_t) ((i & 1) ? 0 : i + 1));
        ramlog_record(&r);
    }
    st_RAMLOG_REC out[32];
    uint64_t seqs[32];
    size_t n = ramlog_recent(seqs, out, 3, false, 0);
    TEST_ASSERT_EQUAL_UINT(3, n);
    TEST_ASSERT_EQUAL_UINT64(19, seqs[0]);
    TEST_ASSERT_EQUAL_UINT64(17, seqs[2]);
    TEST_ASSERT_EQUAL_UINT16(0x3013, out[0].insn_pc);
    n = ramlog_recent(seqs, out, 32, true, 0);
    TEST_ASSERT_EQUAL_UINT(8, n);                      /* sudé 18..4 */
    TEST_ASSERT_EQUAL_UINT64(18, seqs[0]);
    TEST_ASSERT_EQUAL_UINT64(4, seqs[7]);
    n = ramlog_recent(seqs, out, 32, false, 0);
    TEST_ASSERT_EQUAL_UINT(16, n);                     /* jen záznamy v ringu */
    n = ramlog_recent(seqs, out, 32, true, 5);         /* průchod seq 19..15 */
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_UINT(0, ramlog_recent(NULL, out, 0, false, 0));
    ramlog_free();
    TEST_ASSERT_EQUAL_UINT(0, ramlog_recent(seqs, out, 32, false, 0));
}


/** @brief Režim fill: drží prvních N, další počítá; mapa je zná. */
void test_ramlog_fill_mode(void)
{
    st_RAMLOG_CONFIG c = cfg(RAMLOG_MODE_FILL, 16);
    TEST_ASSERT_TRUE(ramlog_start(&c, SIZES));
    for (unsigned i = 0; i < 20; i++) {
        st_RAMLOG_REC r = rec(0x4000 + i, (uint16_t) (0x3000 + i), 0, 1);
        ramlog_record(&r);
    }
    st_RAMLOG_STATUS st;
    ramlog_get_status(&st);
    TEST_ASSERT_EQUAL_UINT64(0, st.first_seq);
    TEST_ASSERT_EQUAL_UINT64(16, st.next_seq);
    TEST_ASSERT_EQUAL_UINT64(20, st.total);
    TEST_ASSERT_EQUAL_UINT64(4, st.dropped);
    st_RAMLOG_MAPENT m;
    TEST_ASSERT_TRUE(ramlog_lastwriter(RAMLOG_SPACE_RAM, 0x4013, &m));
    TEST_ASSERT_EQUAL_UINT32(20, m.seq1);              /* seq 19 >= next_seq: mimo ring */
    TEST_ASSERT_EQUAL_UINT16(0x3013, m.insn_pc);
}


/** @brief Filtr rozsahů a maska stránek fast-path. */
void test_ramlog_filter_and_pages(void)
{
    st_RAMLOG_CONFIG c = cfg(RAMLOG_MODE_RING, 64);
    TEST_ASSERT_TRUE(ramlog_start(&c, SIZES));
    TEST_ASSERT_TRUE(ramlog_match(0x1234, true, RAMLOG_SPACE_RAM, 0x1234)); /* bez rozsahů vše */
    TEST_ASSERT_TRUE(ramlog_page_watched(7));

    c.n_ranges = 2;
    c.ranges[0].phys = false;
    c.ranges[0].from = 0x4000;
    c.ranges[0].to = 0x4001;
    c.ranges[1].phys = true;
    c.ranges[1].space = RAMLOG_SPACE_VRAM;
    c.ranges[1].from = 0x2000;
    c.ranges[1].to = 0x20FF;
    TEST_ASSERT_TRUE(ramlog_start(&c, SIZES));
    TEST_ASSERT_TRUE(ramlog_match(0x4001, true, RAMLOG_SPACE_RAM, 0x4001));
    TEST_ASSERT_FALSE(ramlog_match(0x4002, true, RAMLOG_SPACE_RAM, 0x4002));
    TEST_ASSERT_FALSE(ramlog_match(0x4001, false, RAMLOG_SPACE_RAM, 0x4001)); /* bez CPU adresy */
    TEST_ASSERT_TRUE(ramlog_match(0x8000, true, RAMLOG_SPACE_VRAM, 0x2010));
    TEST_ASSERT_FALSE(ramlog_match(0x8000, true, RAMLOG_SPACE_VRAM, 0x0010));
    TEST_ASSERT_TRUE(ramlog_page_watched(4));
    TEST_ASSERT_FALSE(ramlog_page_watched(5));
    TEST_ASSERT_FALSE(ramlog_page_watched(8));         /* VRAM rozsah RAM stránky nevyžaduje */

    /* Fyzický rozsah v RAM: stránky nelze předpočítat, vyřadit všechny. */
    c.n_ranges = 1;
    c.ranges[0].phys = true;
    c.ranges[0].space = RAMLOG_SPACE_RAM;
    TEST_ASSERT_TRUE(ramlog_start(&c, SIZES));
    TEST_ASSERT_TRUE(ramlog_page_watched(0));
    TEST_ASSERT_TRUE(ramlog_page_watched(15));
}


/** @brief who_wrote od nejnovějšího, skrytí nezměněných; writers s počty. */
void test_ramlog_queries(void)
{
    st_RAMLOG_CONFIG c = cfg(RAMLOG_MODE_RING, 64);
    TEST_ASSERT_TRUE(ramlog_start(&c, SIZES));
    st_RAMLOG_REC a = rec(0x4001, 0x3009, 0xAA, 0x66);
    st_RAMLOG_REC b = rec(0x4001, 0x3100, 0x66, 0x66);  /* nezměněno */
    st_RAMLOG_REC d = rec(0x4002, 0x3100, 0x00, 0x01);
    st_RAMLOG_REC e = rec(0x4003, 0x3100, 0x00, 0x02);
    ramlog_record(&a);
    ramlog_record(&b);
    ramlog_record(&d);
    ramlog_record(&e);

    st_RAMLOG_REC out[4];
    uint64_t seq[4];
    size_t n = ramlog_who_wrote(RAMLOG_SPACE_RAM, 0x4001, seq, out, 4, false);
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_UINT64(1, seq[0]);
    TEST_ASSERT_EQUAL_UINT16(0x3100, out[0].insn_pc);
    TEST_ASSERT_EQUAL_UINT16(0x3009, out[1].insn_pc);
    n = ramlog_who_wrote(RAMLOG_SPACE_RAM, 0x4001, seq, out, 4, true);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_UINT8(0x66, out[0].new_value);

    st_RAMLOG_WRITER w[4];
    size_t total = 0;
    n = ramlog_writers(RAMLOG_SPACE_RAM, 0x4000, 0x4003, w, 4, &total, false);
    TEST_ASSERT_EQUAL_UINT(2, n);
    TEST_ASSERT_EQUAL_UINT(2, total);
    TEST_ASSERT_EQUAL_UINT16(0x3100, w[0].insn_pc);    /* 3 zápisy */
    TEST_ASSERT_EQUAL_UINT64(3, w[0].count);
    TEST_ASSERT_EQUAL_UINT64(1, w[0].first_seq);
    TEST_ASSERT_EQUAL_UINT64(3, w[0].last_seq);
    TEST_ASSERT_EQUAL_UINT16(0x3009, w[1].insn_pc);
    n = ramlog_writers(RAMLOG_SPACE_RAM, 0x4002, 0x4002, w, 4, &total, false);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_UINT64(1, w[0].count);
    /* Podle CPU adresy: totéž (cpu_addr = offset v rec()), skrytí nezměněných. */
    n = ramlog_writers_cpu(0x4001, 0x4001, w, 4, &total, true);
    TEST_ASSERT_EQUAL_UINT(1, n);
    TEST_ASSERT_EQUAL_UINT16(0x3009, w[0].insn_pc);
}


/** @brief Souhrnný záznam bloku, potlačený zápis, líně alokovaný MemExt. */
void test_ramlog_summary_suppressed_memext(void)
{
    st_RAMLOG_CONFIG c = cfg(RAMLOG_MODE_RING, 64);
    TEST_ASSERT_TRUE(ramlog_start(&c, SIZES));
    st_RAMLOG_STATUS st;
    ramlog_get_status(&st);
    uint64_t mem0 = st.memory_bytes;
    TEST_ASSERT_EQUAL_UINT64(64u * 32u + (0x10000u + 0x8000u) * 8u, mem0);

    st_RAMLOG_REC s = rec(0x1200, 0, 0, 0);
    s.flags = RAMLOG_FLAG_SUMMARY;
    s.src = RAMLOG_SRC_LOAD;
    s.count = 0x100;
    ramlog_record(&s);
    st_RAMLOG_MAPENT m;
    TEST_ASSERT_TRUE(ramlog_lastwriter(RAMLOG_SPACE_RAM, 0x12FF, &m));
    TEST_ASSERT_EQUAL_UINT8(RAMLOG_SRC_LOAD, m.src);
    TEST_ASSERT_FALSE(ramlog_lastwriter(RAMLOG_SPACE_RAM, 0x1300, &m));
    st_RAMLOG_REC out[2];
    TEST_ASSERT_EQUAL_UINT(1, ramlog_who_wrote(RAMLOG_SPACE_RAM, 0x1250, NULL, out, 2, false));

    st_RAMLOG_REC rom = rec(0x0005, 0x3000, 0x12, 0x12);
    rom.space = RAMLOG_SPACE_ROM;
    rom.flags = RAMLOG_FLAG_SUPPRESSED;
    ramlog_record(&rom);
    TEST_ASSERT_FALSE(ramlog_lastwriter(RAMLOG_SPACE_ROM, 0x0005, &m));
    TEST_ASSERT_EQUAL_UINT(1, ramlog_who_wrote(RAMLOG_SPACE_ROM, 0x0005, NULL, out, 2, false));

    st_RAMLOG_REC mx = rec(0x70000, 0x3000, 0, 9);
    mx.space = RAMLOG_SPACE_MEMEXT;
    ramlog_record(&mx);
    ramlog_get_status(&st);
    TEST_ASSERT_EQUAL_UINT64(mem0 + 0x80000u * 8u, st.memory_bytes);
    TEST_ASSERT_TRUE(ramlog_lastwriter(RAMLOG_SPACE_MEMEXT, 0x70000, &m));
}


/** @brief clear = nový segment a prázdná mapa; stop nechá data; kopie úseku. */
void test_ramlog_clear_stop_copy(void)
{
    st_RAMLOG_CONFIG c = cfg(RAMLOG_MODE_RING, 16);
    TEST_ASSERT_TRUE(ramlog_start(&c, SIZES));
    st_RAMLOG_STATUS st;
    ramlog_get_status(&st);
    unsigned seg = st.segment;
    for (unsigned i = 0; i < 20; i++) {
        st_RAMLOG_REC r = rec(i, (uint16_t) i, 0, 1);
        ramlog_record(&r);
    }
    st_RAMLOG_REC buf[16];
    uint64_t first = 0;
    TEST_ASSERT_EQUAL_UINT64(16, ramlog_copy(0, 100, buf, &first));
    TEST_ASSERT_EQUAL_UINT64(4, first);
    TEST_ASSERT_EQUAL_UINT16(4, buf[0].insn_pc);
    TEST_ASSERT_EQUAL_UINT64(2, ramlog_copy(18, 100, buf, &first));

    ramlog_stop();
    TEST_ASSERT_EQUAL_INT(0, g_ramlog_active);
    ramlog_get_status(&st);
    TEST_ASSERT_TRUE(st.allocated);
    TEST_ASSERT_EQUAL_UINT64(20, st.next_seq);

    ramlog_clear();
    ramlog_get_status(&st);
    TEST_ASSERT_EQUAL_UINT(seg + 1, st.segment);
    TEST_ASSERT_EQUAL_UINT64(0, st.next_seq);
    st_RAMLOG_MAPENT m;
    TEST_ASSERT_FALSE(ramlog_lastwriter(RAMLOG_SPACE_RAM, 0, &m));

    /* Zdroj zápisu: push / pop. */
    uint32_t saved = ramlog_source_push(RAMLOG_SRC_DEBUGGER, 0x1234);
    TEST_ASSERT_EQUAL_UINT8(RAMLOG_SRC_DEBUGGER, g_ramlog_src);
    ramlog_source_pop(saved);
    TEST_ASSERT_EQUAL_UINT8(RAMLOG_SRC_INSN, g_ramlog_src);
    TEST_ASSERT_EQUAL_STRING("int_ack", ramlog_src_name(RAMLOG_SRC_INT_ACK));
    TEST_ASSERT_EQUAL_INT(RAMLOG_SPACE_MEMEXT, ramlog_space_from_name("memext"));
    TEST_ASSERT_EQUAL_INT(-1, ramlog_space_from_name("nope"));
}


int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ramlog_ring_mode);
    RUN_TEST(test_ramlog_fill_mode);
    RUN_TEST(test_ramlog_filter_and_pages);
    RUN_TEST(test_ramlog_queries);
    RUN_TEST(test_ramlog_summary_suppressed_memext);
    RUN_TEST(test_ramlog_clear_stop_copy);
    RUN_TEST(test_ramlog_recent);
    return UNITY_END();
}
