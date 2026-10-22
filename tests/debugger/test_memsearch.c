/**
 * @file test_memsearch.c
 * @brief Unit testy jádra postupného hledání proměnných (memsearch).
 *
 * Nad syntetickými vzorky bez emulátoru:
 *  - dekódování a kódování (u8-u32 LE/BE, s8 přes 7Fh/80h, BCD včetně
 *    neplatného nibblu, řetězec číslic ve třech kódováních s mezerami),
 *  - start, filtry s hodnotou, s předchozím a s prvním vzorkem, negace,
 *    prázdný výsledek, hranice spanu, zarovnání,
 *  - undo/redo vrací kandidáty i referenční vzorky,
 *  - odpojený span nechá kandidáty beze změny,
 *  - ruční vyřazení, stránkování, převod offsetů, předvolby prostoru,
 *  - informativně čas filtru nad 16 MiB (bez pass/fail).
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <string.h>

#include "debugger/memsearch/memsearch.h"
#include "debugger/dbgapi_regions.h"


/** @brief Formát u8 bez zarovnání. */
static st_MEMSEARCH_FMT fmt_u8(void)
{
    st_MEMSEARCH_FMT f = { MEMSEARCH_TYPE_UINT, 1, false, MEMSEARCH_DIGITS_RAW, false, false };
    return f;
}


/**
 * @brief Vzorek z bufferu (všechny spany připojené).
 * @param data Data prostoru.
 * @param size Velikost.
 * @param spans Počet spanů.
 * @return Vzorek s refcount 1.
 */
static st_MEMSEARCH_SAMPLE *mk_sample(const uint8_t *data, uint32_t size,
                                      unsigned spans)
{
    st_MEMSEARCH_SAMPLE *s = memsearch_sample_new(size, spans);
    TEST_ASSERT_NOT_NULL(s);
    memcpy(s->data, data, size);
    memset(s->span_connected, 1, spans);
    return s;
}


/** @brief Filtr s hodnotou. */
static st_MEMSEARCH_FILTER flt(en_MEMSEARCH_OP op, en_MEMSEARCH_CMP c, int64_t v)
{
    st_MEMSEARCH_FILTER f;
    memset(&f, 0, sizeof(f));
    f.op = op;
    f.compare_to = c;
    f.value = v;
    return f;
}


void setUp(void) {}
void tearDown(void) {}


/* ------------------------------------------------------------------ */

/** @brief Celá čísla LE/BE, se znaménkem, šířky 1-4. */
void test_decode_integers(void)
{
    st_MEMSEARCH_FMT f = fmt_u8();
    int64_t v;
    uint8_t b[4] = { 0x80, 0x01, 0x02, 0x03 };

    TEST_ASSERT_TRUE(memsearch_decode(&f, b, &v));
    TEST_ASSERT_EQUAL_INT64(0x80, v);

    f.type = MEMSEARCH_TYPE_SINT;
    TEST_ASSERT_TRUE(memsearch_decode(&f, b, &v));
    TEST_ASSERT_EQUAL_INT64(-128, v);
    uint8_t b7f = 0x7F;
    TEST_ASSERT_TRUE(memsearch_decode(&f, &b7f, &v));
    TEST_ASSERT_EQUAL_INT64(127, v);

    f.type = MEMSEARCH_TYPE_UINT;
    f.width = 2;
    TEST_ASSERT_TRUE(memsearch_decode(&f, b, &v));
    TEST_ASSERT_EQUAL_INT64(0x0180, v);          /* LE */
    f.big_endian = true;
    TEST_ASSERT_TRUE(memsearch_decode(&f, b, &v));
    TEST_ASSERT_EQUAL_INT64(0x8001, v);          /* BE */

    f.width = 3;
    f.big_endian = false;
    TEST_ASSERT_TRUE(memsearch_decode(&f, b, &v));
    TEST_ASSERT_EQUAL_INT64(0x020180, v);

    f.width = 4;
    f.type = MEMSEARCH_TYPE_SINT;
    uint8_t m1[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    TEST_ASSERT_TRUE(memsearch_decode(&f, m1, &v));
    TEST_ASSERT_EQUAL_INT64(-1, v);
}


/** @brief BCD: neplatný nibble, pořadí bajtů. */
void test_decode_bcd(void)
{
    st_MEMSEARCH_FMT f = { MEMSEARCH_TYPE_BCD, 2, true, MEMSEARCH_DIGITS_RAW, false, false };
    int64_t v;
    uint8_t ok[2] = { 0x12, 0x34 };
    TEST_ASSERT_TRUE(memsearch_decode(&f, ok, &v));
    TEST_ASSERT_EQUAL_INT64(1234, v);
    f.big_endian = false;
    TEST_ASSERT_TRUE(memsearch_decode(&f, ok, &v));
    TEST_ASSERT_EQUAL_INT64(3412, v);

    uint8_t bad[2] = { 0x1A, 0x00 };
    TEST_ASSERT_FALSE(memsearch_decode(&f, bad, &v));
    uint8_t bad_hi[2] = { 0x00, 0xA0 };
    TEST_ASSERT_FALSE(memsearch_decode(&f, bad_hi, &v));
}


/** @brief Řetězec číslic: holé, ASCII, display kód, mezery zleva. */
void test_decode_digits(void)
{
    st_MEMSEARCH_FMT f = { MEMSEARCH_TYPE_DIGITS, 4, true, MEMSEARCH_DIGITS_RAW, true, false };
    int64_t v;

    uint8_t raw[4] = { 0x00, 0x01, 0x02, 0x09 };
    TEST_ASSERT_TRUE(memsearch_decode(&f, raw, &v));
    TEST_ASSERT_EQUAL_INT64(129, v);
    uint8_t raw_bad[4] = { 0x00, 0x0A, 0x02, 0x09 };
    TEST_ASSERT_FALSE(memsearch_decode(&f, raw_bad, &v));

    f.digit_enc = MEMSEARCH_DIGITS_ASCII;
    uint8_t asc[4] = { ' ', ' ', '4', '2' };
    TEST_ASSERT_TRUE(memsearch_decode(&f, asc, &v));
    TEST_ASSERT_EQUAL_INT64(42, v);
    uint8_t asc_mid[4] = { '4', ' ', '4', '2' };    /* mezera uprostřed */
    TEST_ASSERT_FALSE(memsearch_decode(&f, asc_mid, &v));
    f.leading_space = false;
    TEST_ASSERT_FALSE(memsearch_decode(&f, asc, &v));
    f.leading_space = true;

    /* Display kód: číslice 20h-29h, mezera 00h (báze, tabulka ADCN). */
    f.digit_enc = MEMSEARCH_DIGITS_DISPLAY;
    uint8_t disp[4] = { 0x00, 0x21, 0x20, 0x25 };
    TEST_ASSERT_TRUE(memsearch_decode(&f, disp, &v));
    TEST_ASSERT_EQUAL_INT64(105, v);
    uint8_t disp_bad[4] = { 0x20, 0x30, 0x20, 0x20 };
    TEST_ASSERT_FALSE(memsearch_decode(&f, disp_bad, &v));

    /* Malý řád první (LE). */
    f.big_endian = false;
    uint8_t disp_le[4] = { 0x25, 0x20, 0x21, 0x00 };
    TEST_ASSERT_TRUE(memsearch_decode(&f, disp_le, &v));
    TEST_ASSERT_EQUAL_INT64(105, v);
}


/** @brief Kódování je opakem dekódování; hodnoty mimo rozsah se odmítnou. */
void test_encode_roundtrip(void)
{
    uint8_t b[8];
    int64_t v;
    st_MEMSEARCH_FMT f = { MEMSEARCH_TYPE_SINT, 2, false, MEMSEARCH_DIGITS_RAW, false, false };
    TEST_ASSERT_TRUE(memsearch_encode(&f, -2, b));
    TEST_ASSERT_EQUAL_HEX8(0xFE, b[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, b[1]);
    TEST_ASSERT_FALSE(memsearch_encode(&f, 40000, b));

    st_MEMSEARCH_FMT bcd = { MEMSEARCH_TYPE_BCD, 2, false, MEMSEARCH_DIGITS_RAW, false, false };
    TEST_ASSERT_TRUE(memsearch_encode(&bcd, 100, b));
    TEST_ASSERT_EQUAL_HEX8(0x00, b[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, b[1]);
    TEST_ASSERT_TRUE(memsearch_decode(&bcd, b, &v));
    TEST_ASSERT_EQUAL_INT64(100, v);
    TEST_ASSERT_FALSE(memsearch_encode(&bcd, 10000, b));

    st_MEMSEARCH_FMT dg = { MEMSEARCH_TYPE_DIGITS, 5, true, MEMSEARCH_DIGITS_DISPLAY, true, false };
    TEST_ASSERT_TRUE(memsearch_encode(&dg, 1234, b));
    TEST_ASSERT_EQUAL_HEX8(0x20, b[0]);
    TEST_ASSERT_EQUAL_HEX8(0x24, b[4]);
    TEST_ASSERT_TRUE(memsearch_decode(&dg, b, &v));
    TEST_ASSERT_EQUAL_INT64(1234, v);
}


/** @brief Validace formátu a filtru. */
void test_validate(void)
{
    st_MEMSEARCH_FMT f = fmt_u8();
    const char *err = NULL;
    TEST_ASSERT_TRUE(memsearch_fmt_validate(&f, &err));
    f.width = 5;
    TEST_ASSERT_FALSE(memsearch_fmt_validate(&f, &err));
    TEST_ASSERT_NOT_NULL(err);
    f.type = MEMSEARCH_TYPE_DIGITS;
    f.width = 8;
    TEST_ASSERT_TRUE(memsearch_fmt_validate(&f, &err));
    f.width = 9;
    TEST_ASSERT_FALSE(memsearch_fmt_validate(&f, &err));

    st_MEMSEARCH_FMT u = fmt_u8();
    st_MEMSEARCH_FILTER x = flt(MEMSEARCH_OP_DEC, MEMSEARCH_CMP_VALUE, 0);
    TEST_ASSERT_FALSE(memsearch_filter_validate(&x, &u, &err));
    x = flt(MEMSEARCH_OP_BETWEEN, MEMSEARCH_CMP_PREVIOUS, 0);
    TEST_ASSERT_FALSE(memsearch_filter_validate(&x, &u, &err));
    x = flt(MEMSEARCH_OP_DEC_BY, MEMSEARCH_CMP_PREVIOUS, -1);
    TEST_ASSERT_FALSE(memsearch_filter_validate(&x, &u, &err));
    x = flt(MEMSEARCH_OP_MASK_EQ, MEMSEARCH_CMP_VALUE, 1);
    st_MEMSEARCH_FMT bcd = { MEMSEARCH_TYPE_BCD, 1, false, MEMSEARCH_DIGITS_RAW, false, false };
    TEST_ASSERT_FALSE(memsearch_filter_validate(&x, &bcd, &err));
    TEST_ASSERT_TRUE(memsearch_filter_validate(&x, &u, &err));
}


/**
 * @brief Typický průběh: start s eq 5, zápis 4, dec_by 1 nechá jen ten bajt.
 */
void test_flow_dec_by(void)
{
    uint8_t mem[64];
    memset(mem, 5, sizeof(mem));
    st_MEMSEARCH_SPAN sp = { REGION_KIND_RAM, 0, 0, sizeof(mem) };
    st_MEMSEARCH_FMT f = fmt_u8();
    st_MEMSEARCH_SESSION *s = memsearch_session_new(&sp, 1, &f, MEMSEARCH_DEFAULT_MAX_SPACE, NULL);
    TEST_ASSERT_NOT_NULL(s);

    mem[10] = 7;
    st_MEMSEARCH_SAMPLE *a = mk_sample(mem, sizeof(mem), 1);
    st_MEMSEARCH_FILTER eq5 = flt(MEMSEARCH_OP_EQ, MEMSEARCH_CMP_VALUE, 5);
    TEST_ASSERT_TRUE(memsearch_start(s, a, &eq5, NULL));
    TEST_ASSERT_EQUAL_UINT64(63, memsearch_candidate_count(s));
    TEST_ASSERT_FALSE(memsearch_is_candidate(s, 10));
    TEST_ASSERT_EQUAL_UINT(1, memsearch_step(s));

    mem[20] = 4;   /* ztráta života */
    mem[30] = 3;   /* snížení o 2 */
    st_MEMSEARCH_SAMPLE *b = mk_sample(mem, sizeof(mem), 1);
    st_MEMSEARCH_FILTER d1 = flt(MEMSEARCH_OP_DEC_BY, MEMSEARCH_CMP_PREVIOUS, 1);
    uint64_t removed = 0;
    TEST_ASSERT_TRUE(memsearch_filter(s, b, &d1, &removed, NULL));
    TEST_ASSERT_EQUAL_UINT64(1, memsearch_candidate_count(s));
    TEST_ASSERT_EQUAL_UINT64(62, removed);
    TEST_ASSERT_TRUE(memsearch_is_candidate(s, 20));

    st_MEMSEARCH_ITEM it;
    TEST_ASSERT_TRUE(memsearch_item(s, 20, &it));
    TEST_ASSERT_EQUAL_INT64(4, it.cur.value);
    TEST_ASSERT_EQUAL_INT64(5, it.prev.value);
    TEST_ASSERT_EQUAL_INT64(5, it.first.value);
    TEST_ASSERT_EQUAL_INT(REGION_KIND_RAM, it.region_kind);
    TEST_ASSERT_EQUAL_UINT32(20, it.region_offset);
    TEST_ASSERT_TRUE(it.connected);

    memsearch_sample_unref(a);
    memsearch_sample_unref(b);
    memsearch_session_free(s);
}


/** @brief inc/dec/changed/unchanged/changed_by, compare_to first, negate, prázdný výsledek. */
void test_ops_and_first(void)
{
    uint8_t m0[8] = { 10, 10, 10, 10, 10, 10, 10, 10 };
    uint8_t m1[8] = { 11, 9, 10, 12, 8, 10, 10, 10 };
    uint8_t m2[8] = { 11, 9, 10, 12, 8, 10, 10, 13 };
    st_MEMSEARCH_SPAN sp = { REGION_KIND_RAM, 0, 0, 8 };
    st_MEMSEARCH_FMT f = fmt_u8();
    st_MEMSEARCH_SESSION *s = memsearch_session_new(&sp, 1, &f, MEMSEARCH_DEFAULT_MAX_SPACE, NULL);
    st_MEMSEARCH_SAMPLE *a = mk_sample(m0, 8, 1), *b = mk_sample(m1, 8, 1), *c = mk_sample(m2, 8, 1);

    TEST_ASSERT_TRUE(memsearch_start(s, a, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64(8, memsearch_candidate_count(s));

    /* changed_by 2 vůči předchozímu: 12 a 8. */
    st_MEMSEARCH_FILTER cb = flt(MEMSEARCH_OP_CHANGED_BY, MEMSEARCH_CMP_PREVIOUS, 2);
    TEST_ASSERT_TRUE(memsearch_sample(s, b, NULL));
    TEST_ASSERT_EQUAL_INT(1, memsearch_eval(s, 3, &cb));
    TEST_ASSERT_EQUAL_INT(1, memsearch_eval(s, 4, &cb));
    TEST_ASSERT_EQUAL_INT(0, memsearch_eval(s, 0, &cb));

    /* Bez nového vzorku: cur (m1) proti prev (m0), negace "unchanged" = changed. */
    st_MEMSEARCH_FILTER un = flt(MEMSEARCH_OP_UNCHANGED, MEMSEARCH_CMP_PREVIOUS, 0);
    un.negate = true;
    TEST_ASSERT_TRUE(memsearch_filter(s, NULL, &un, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64(4, memsearch_candidate_count(s));   /* 0,1,3,4 */

    /* S novým vzorkem m2 vůči prvnímu (m0): inc = 0 a 3 (7 už vyřazen). */
    st_MEMSEARCH_FILTER inc = flt(MEMSEARCH_OP_INC, MEMSEARCH_CMP_FIRST, 0);
    TEST_ASSERT_TRUE(memsearch_filter(s, c, &inc, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64(2, memsearch_candidate_count(s));
    TEST_ASSERT_TRUE(memsearch_is_candidate(s, 0));
    TEST_ASSERT_TRUE(memsearch_is_candidate(s, 3));

    /* Prázdný výsledek. */
    st_MEMSEARCH_FILTER gt = flt(MEMSEARCH_OP_GT, MEMSEARCH_CMP_VALUE, 200);
    TEST_ASSERT_TRUE(memsearch_filter(s, NULL, &gt, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64(0, memsearch_candidate_count(s));
    uint32_t o;
    TEST_ASSERT_FALSE(memsearch_nth_candidate(s, 0, &o));

    memsearch_sample_unref(a);
    memsearch_sample_unref(b);
    memsearch_sample_unref(c);
    memsearch_session_free(s);
}


/** @brief u16 nesmí začínat na posledním bajtu spanu; dva spany, zarovnání. */
void test_span_boundary_and_aligned(void)
{
    uint8_t mem[8] = { 0 };
    st_MEMSEARCH_SPAN sp[2] = {
        { REGION_KIND_VRAM_700_CHAR, 0, 0x10, 4 },
        { REGION_KIND_VRAM_700_ATTR, 0, 0, 4 },
    };
    st_MEMSEARCH_FMT f = fmt_u8();
    f.width = 2;
    st_MEMSEARCH_SESSION *s = memsearch_session_new(sp, 2, &f, MEMSEARCH_DEFAULT_MAX_SPACE, NULL);
    st_MEMSEARCH_SAMPLE *a = mk_sample(mem, 8, 2);
    TEST_ASSERT_TRUE(memsearch_start(s, a, NULL, NULL));
    /* Každý span 4 B dává 3 starty u16 (offset 3 a 7 ne). */
    TEST_ASSERT_EQUAL_UINT64(6, memsearch_candidate_count(s));
    TEST_ASSERT_FALSE(memsearch_is_candidate(s, 3));
    TEST_ASSERT_FALSE(memsearch_is_candidate(s, 7));
    TEST_ASSERT_TRUE(memsearch_is_candidate(s, 4));

    st_MEMSEARCH_ITEM it;
    TEST_ASSERT_TRUE(memsearch_item(s, 1, &it));
    TEST_ASSERT_EQUAL_INT(REGION_KIND_VRAM_700_CHAR, it.region_kind);
    TEST_ASSERT_EQUAL_UINT32(0x11, it.region_offset);
    TEST_ASSERT_TRUE(memsearch_item(s, 5, &it));
    TEST_ASSERT_EQUAL_INT(REGION_KIND_VRAM_700_ATTR, it.region_kind);
    TEST_ASSERT_EQUAL_UINT32(1, it.region_offset);

    uint32_t off = 0;
    TEST_ASSERT_TRUE(memsearch_find_offset(s, REGION_KIND_VRAM_700_ATTR, 0, 2, &off));
    TEST_ASSERT_EQUAL_UINT32(6, off);
    TEST_ASSERT_FALSE(memsearch_find_offset(s, REGION_KIND_VRAM_700_CHAR, 0, 0x0F, &off));

    memsearch_sample_unref(a);
    memsearch_session_free(s);

    f.aligned = true;
    s = memsearch_session_new(sp, 2, &f, MEMSEARCH_DEFAULT_MAX_SPACE, NULL);
    a = mk_sample(mem, 8, 2);
    TEST_ASSERT_TRUE(memsearch_start(s, a, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64(4, memsearch_candidate_count(s));   /* 0,2 a 4,6 */
    TEST_ASSERT_FALSE(memsearch_is_candidate(s, 1));
    memsearch_sample_unref(a);
    memsearch_session_free(s);
}


/** @brief BCD u16 LE: 0099h -> 0100h projde "inc_by 1", neplatné BCD vypadne. */
void test_bcd_inc_over_99(void)
{
    uint8_t m0[4] = { 0x99, 0x00, 0x1A, 0x00 };
    uint8_t m1[4] = { 0x00, 0x01, 0x1B, 0x00 };
    st_MEMSEARCH_SPAN sp = { REGION_KIND_RAM, 0, 0, 4 };
    st_MEMSEARCH_FMT f = { MEMSEARCH_TYPE_BCD, 2, false, MEMSEARCH_DIGITS_RAW, false, false };
    st_MEMSEARCH_SESSION *s = memsearch_session_new(&sp, 1, &f, MEMSEARCH_DEFAULT_MAX_SPACE, NULL);
    st_MEMSEARCH_SAMPLE *a = mk_sample(m0, 4, 1), *b = mk_sample(m1, 4, 1);
    TEST_ASSERT_TRUE(memsearch_start(s, a, NULL, NULL));
    st_MEMSEARCH_FILTER inc1 = flt(MEMSEARCH_OP_INC_BY, MEMSEARCH_CMP_PREVIOUS, 1);
    TEST_ASSERT_TRUE(memsearch_filter(s, b, &inc1, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64(1, memsearch_candidate_count(s));
    TEST_ASSERT_TRUE(memsearch_is_candidate(s, 0));
    st_MEMSEARCH_ITEM it;
    TEST_ASSERT_TRUE(memsearch_item(s, 0, &it));
    TEST_ASSERT_EQUAL_INT64(100, it.cur.value);
    TEST_ASSERT_EQUAL_INT64(99, it.prev.value);
    memsearch_sample_unref(a);
    memsearch_sample_unref(b);
    memsearch_session_free(s);
}


/** @brief Řetězec číslic v display kódu (skóre na obrazovce). */
void test_digits_display_search(void)
{
    /* "0120" a "0130" v display kódu od offsetu 2, okolo mezery 00h. */
    uint8_t m0[8] = { 0x00, 0x00, 0x20, 0x21, 0x22, 0x20, 0x00, 0x00 };
    uint8_t m1[8] = { 0x00, 0x00, 0x20, 0x21, 0x23, 0x20, 0x00, 0x00 };
    st_MEMSEARCH_SPAN sp = { REGION_KIND_VRAM_700_CHAR, 0, 0, 8 };
    st_MEMSEARCH_FMT f = { MEMSEARCH_TYPE_DIGITS, 4, true, MEMSEARCH_DIGITS_DISPLAY, true, false };
    st_MEMSEARCH_SESSION *s = memsearch_session_new(&sp, 1, &f, MEMSEARCH_DEFAULT_MAX_SPACE, NULL);
    st_MEMSEARCH_SAMPLE *a = mk_sample(m0, 8, 1), *b = mk_sample(m1, 8, 1);
    st_MEMSEARCH_FILTER eq = flt(MEMSEARCH_OP_EQ, MEMSEARCH_CMP_VALUE, 120);
    TEST_ASSERT_TRUE(memsearch_start(s, a, &eq, NULL));
    /* Offset 0 = "  01" (1), 1 = " 012" (12), 2 = "0120", 3 a 4 mají mezeru za číslicí. */
    TEST_ASSERT_EQUAL_UINT64(1, memsearch_candidate_count(s));
    TEST_ASSERT_TRUE(memsearch_is_candidate(s, 2));
    st_MEMSEARCH_FILTER inc10 = flt(MEMSEARCH_OP_INC_BY, MEMSEARCH_CMP_PREVIOUS, 10);
    TEST_ASSERT_TRUE(memsearch_filter(s, b, &inc10, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64(1, memsearch_candidate_count(s));
    memsearch_sample_unref(a);
    memsearch_sample_unref(b);
    memsearch_session_free(s);
}


/** @brief Undo/redo vrací kandidáty i referenční vzorek prev. */
void test_undo_redo_restores_reference(void)
{
    uint8_t m0[4] = { 5, 5, 5, 5 };
    uint8_t m1[4] = { 4, 5, 5, 5 };
    uint8_t m2[4] = { 3, 5, 5, 5 };
    st_MEMSEARCH_SPAN sp = { REGION_KIND_RAM, 0, 0, 4 };
    st_MEMSEARCH_FMT f = fmt_u8();
    st_MEMSEARCH_SESSION *s = memsearch_session_new(&sp, 1, &f, MEMSEARCH_DEFAULT_MAX_SPACE, NULL);
    st_MEMSEARCH_SAMPLE *a = mk_sample(m0, 4, 1), *b = mk_sample(m1, 4, 1), *c = mk_sample(m2, 4, 1);
    st_MEMSEARCH_FILTER dec = flt(MEMSEARCH_OP_DEC, MEMSEARCH_CMP_PREVIOUS, 0);

    TEST_ASSERT_TRUE(memsearch_start(s, a, NULL, NULL));
    TEST_ASSERT_TRUE(memsearch_filter(s, b, &dec, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64(1, memsearch_candidate_count(s));
    TEST_ASSERT_EQUAL_UINT(2, memsearch_step(s));

    /* Undo: zpět 4 kandidáti a cur je znovu m0. */
    TEST_ASSERT_TRUE(memsearch_undo(s));
    TEST_ASSERT_EQUAL_UINT64(4, memsearch_candidate_count(s));
    TEST_ASSERT_EQUAL_UINT(1, memsearch_step(s));
    st_MEMSEARCH_ITEM it;
    TEST_ASSERT_TRUE(memsearch_item(s, 0, &it));
    TEST_ASSERT_EQUAL_INT64(5, it.cur.value);

    /* Filtr s m2 po undo: reference je m0, takže dec projde u bajtu 0. */
    TEST_ASSERT_TRUE(memsearch_redo(s));
    TEST_ASSERT_TRUE(memsearch_item(s, 0, &it));
    TEST_ASSERT_EQUAL_INT64(4, it.cur.value);
    TEST_ASSERT_EQUAL_INT64(5, it.prev.value);
    TEST_ASSERT_TRUE(memsearch_undo(s));
    TEST_ASSERT_TRUE(memsearch_filter(s, c, &dec, NULL, NULL));
    TEST_ASSERT_TRUE(memsearch_item(s, 0, &it));
    TEST_ASSERT_EQUAL_INT64(3, it.cur.value);
    TEST_ASSERT_EQUAL_INT64(5, it.prev.value);
    TEST_ASSERT_EQUAL_UINT(0, memsearch_redo_depth(s));   /* nový krok zahodil redo */

    /* Undo až před start: hledání neběží. */
    TEST_ASSERT_TRUE(memsearch_undo(s));
    TEST_ASSERT_TRUE(memsearch_undo(s));
    TEST_ASSERT_FALSE(memsearch_started(s));
    TEST_ASSERT_FALSE(memsearch_undo(s));
    TEST_ASSERT_TRUE(memsearch_redo(s));
    TEST_ASSERT_TRUE(memsearch_started(s));

    memsearch_sample_unref(a);
    memsearch_sample_unref(b);
    memsearch_sample_unref(c);
    memsearch_session_free(s);
}


/** @brief Odpojený span ve vzorku nechá své kandidáty beze změny. */
void test_disconnected_span_keeps_candidates(void)
{
    uint8_t m0[8] = { 1, 1, 1, 1, 1, 1, 1, 1 };
    uint8_t m1[8] = { 2, 2, 2, 2, 0, 0, 0, 0 };
    st_MEMSEARCH_SPAN sp[2] = {
        { REGION_KIND_RAM, 0, 0, 4 },
        { REGION_KIND_MEMEXT_RAM, 0x41, 0, 4 },
    };
    st_MEMSEARCH_FMT f = fmt_u8();
    st_MEMSEARCH_SESSION *s = memsearch_session_new(sp, 2, &f, MEMSEARCH_DEFAULT_MAX_SPACE, NULL);
    st_MEMSEARCH_SAMPLE *a = mk_sample(m0, 8, 2), *b = mk_sample(m1, 8, 2);
    b->span_connected[1] = 0;
    TEST_ASSERT_TRUE(memsearch_start(s, a, NULL, NULL));
    st_MEMSEARCH_FILTER unch = flt(MEMSEARCH_OP_UNCHANGED, MEMSEARCH_CMP_PREVIOUS, 0);
    TEST_ASSERT_TRUE(memsearch_filter(s, b, &unch, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT64(4, memsearch_candidate_count(s));   /* jen span 1 */
    st_MEMSEARCH_ITEM it;
    TEST_ASSERT_TRUE(memsearch_item(s, 5, &it));
    TEST_ASSERT_FALSE(it.connected);
    TEST_ASSERT_FALSE(it.cur.valid);
    TEST_ASSERT_EQUAL_INT(0x41, it.sub_id);
    memsearch_sample_unref(a);
    memsearch_sample_unref(b);
    memsearch_session_free(s);
}


/** @brief Ruční vyřazení a stránkování přes nth_candidate. */
void test_eliminate_and_paging(void)
{
    uint8_t mem[256] = { 0 };
    st_MEMSEARCH_SPAN sp = { REGION_KIND_RAM, 0, 0, 256 };
    st_MEMSEARCH_FMT f = fmt_u8();
    st_MEMSEARCH_SESSION *s = memsearch_session_new(&sp, 1, &f, MEMSEARCH_DEFAULT_MAX_SPACE, NULL);
    st_MEMSEARCH_SAMPLE *a = mk_sample(mem, 256, 1);
    TEST_ASSERT_TRUE(memsearch_start(s, a, NULL, NULL));

    uint32_t el[3] = { 0, 100, 100 };
    TEST_ASSERT_EQUAL_UINT64(2, memsearch_eliminate(s, el, 3));
    TEST_ASSERT_EQUAL_UINT64(254, memsearch_candidate_count(s));
    uint32_t none[1] = { 100 };
    unsigned step = memsearch_step(s);
    TEST_ASSERT_EQUAL_UINT64(0, memsearch_eliminate(s, none, 1));
    TEST_ASSERT_EQUAL_UINT(step, memsearch_step(s));     /* bez vyřazení žádný krok */

    uint32_t o = 0;
    TEST_ASSERT_TRUE(memsearch_nth_candidate(s, 0, &o));
    TEST_ASSERT_EQUAL_UINT32(1, o);
    TEST_ASSERT_TRUE(memsearch_nth_candidate(s, 99, &o));
    TEST_ASSERT_EQUAL_UINT32(101, o);
    TEST_ASSERT_TRUE(memsearch_nth_candidate(s, 253, &o));
    TEST_ASSERT_EQUAL_UINT32(255, o);
    TEST_ASSERT_FALSE(memsearch_nth_candidate(s, 254, &o));

    TEST_ASSERT_TRUE(memsearch_undo(s));
    TEST_ASSERT_TRUE(memsearch_nth_candidate(s, 0, &o));
    TEST_ASSERT_EQUAL_UINT32(0, o);

    memsearch_sample_unref(a);
    memsearch_session_free(s);
}


/** @brief Strop velikosti prostoru a vzorek s jiným rozměrem. */
void test_limits(void)
{
    st_MEMSEARCH_SPAN big = { REGION_KIND_RAMDISK_STD, 0, 0, MEMSEARCH_DEFAULT_MAX_SPACE + 1 };
    st_MEMSEARCH_FMT f = fmt_u8();
    const char *err = NULL;
    TEST_ASSERT_NULL(memsearch_session_new(&big, 1, &f, MEMSEARCH_DEFAULT_MAX_SPACE, &err));
    TEST_ASSERT_NOT_NULL(err);
    st_MEMSEARCH_SESSION *s = memsearch_session_new(&big, 1, &f, MEMSEARCH_HARD_MAX_SPACE, NULL);
    TEST_ASSERT_NOT_NULL(s);
    memsearch_session_free(s);
    st_MEMSEARCH_SPAN huge = { REGION_KIND_RAMDISK_STD, 0, 0, MEMSEARCH_HARD_MAX_SPACE + 1 };
    TEST_ASSERT_NULL(memsearch_session_new(&huge, 1, &f, 0xFFFFFFFFu, NULL));

    uint8_t mem[4] = { 0 };
    st_MEMSEARCH_SPAN sp = { REGION_KIND_RAM, 0, 0, 4 };
    s = memsearch_session_new(&sp, 1, &f, MEMSEARCH_DEFAULT_MAX_SPACE, NULL);
    st_MEMSEARCH_SAMPLE *wrong = mk_sample(mem, 3, 1);
    TEST_ASSERT_FALSE(memsearch_start(s, wrong, NULL, &err));
    TEST_ASSERT_FALSE(memsearch_started(s));
    memsearch_sample_unref(wrong);
    memsearch_session_free(s);
}


/** @brief Předvolby: jen připojené regiony daných druhů, bez ROM a Logical. */
void test_presets(void)
{
    st_REGION_DESC r[6];
    memset(r, 0, sizeof(r));
    r[0].kind = REGION_KIND_LOGICAL;      r[0].size = 0x10000; r[0].connected = 1;
    r[1].kind = REGION_KIND_RAM;          r[1].size = 0x10000; r[1].connected = 1;
    r[2].kind = REGION_KIND_ROM_LOWER;    r[2].size = 0x1000;  r[2].connected = 1; r[2].writable = 1;
    r[3].kind = REGION_KIND_MEMEXT_RAM;   r[3].size = 0x1000;  r[3].connected = 1; r[3].sub_id = 0x7F;
    r[4].kind = REGION_KIND_MEMEXT_FLASH; r[4].size = 0x1000;  r[4].connected = 1;
    r[5].kind = REGION_KIND_RAMDISK_STD;  r[5].size = 0x10000; r[5].connected = 0;
    st_MEMSEARCH_SPAN out[6];

    TEST_ASSERT_EQUAL_UINT(1, memsearch_spans_from_preset(MEMSEARCH_PRESET_RAM, r, 6, out));
    TEST_ASSERT_EQUAL_INT(REGION_KIND_RAM, out[0].region_kind);
    TEST_ASSERT_EQUAL_UINT32(0x10000, out[0].length);

    TEST_ASSERT_EQUAL_UINT(1, memsearch_spans_from_preset(MEMSEARCH_PRESET_MEMEXT, r, 6, out));
    TEST_ASSERT_EQUAL_INT(0x7F, out[0].sub_id);

    TEST_ASSERT_EQUAL_UINT(0, memsearch_spans_from_preset(MEMSEARCH_PRESET_RAMDISK, r, 6, out));
    TEST_ASSERT_EQUAL_UINT(2, memsearch_spans_from_preset(MEMSEARCH_PRESET_ALL_WRITABLE, r, 6, out));

    TEST_ASSERT_EQUAL_INT(MEMSEARCH_PRESET_ALL_WRITABLE, memsearch_preset_from_name("all_writable"));
    TEST_ASSERT_EQUAL_INT(-1, memsearch_preset_from_name("rom"));
    TEST_ASSERT_EQUAL_INT(MEMSEARCH_OP_DEC_BY, memsearch_op_from_name("dec_by"));
    TEST_ASSERT_EQUAL_STRING("changed_by", memsearch_op_name(MEMSEARCH_OP_CHANGED_BY));
    en_MEMSEARCH_TYPE t;
    unsigned w;
    TEST_ASSERT_TRUE(memsearch_type_from_name("s24", &t, &w));
    TEST_ASSERT_EQUAL_INT(MEMSEARCH_TYPE_SINT, t);
    TEST_ASSERT_EQUAL_UINT(3, w);
}


/** @brief Převod kandidáta na CPU adresu a write BP (RAM, VRAM 700, PEHU, Luftner, plane). */
void test_target(void)
{
    st_MEMSEARCH_TARGET t;
    TEST_ASSERT_TRUE(memsearch_target(REGION_KIND_RAM, 0, 0x502F, 1, NULL, &t));
    TEST_ASSERT_TRUE(t.has_z80);
    TEST_ASSERT_EQUAL_HEX16(0x502F, t.z80_addr);
    TEST_ASSERT_EQUAL_INT(MEMSEARCH_BPZONE_RAM, t.bp_zone);
    TEST_ASSERT_EQUAL_STRING("RAM", memsearch_bpzone_name(t.bp_zone));

    TEST_ASSERT_TRUE(memsearch_target(REGION_KIND_VRAM_700_ATTR, 0, 0x10, 2, NULL, &t));
    TEST_ASSERT_EQUAL_HEX16(0xD810, t.bp_addr);
    TEST_ASSERT_EQUAL_HEX16(0xD811, t.bp_addr_end);
    TEST_ASSERT_EQUAL_INT(MEMSEARCH_BPZONE_VRAM_FB, t.bp_zone);

    /* PEHU: kus 41h = horní půlka banky 20h, namapovaný v okně 4 (4000h). */
    st_MEMSEARCH_MEMEXT mx;
    memset(&mx, 0, sizeof(mx));
    mx.connected = true;
    mx.pehu = true;
    mx.map_valid = true;
    for (int i = 0; i < 16; i++) mx.map[i] = (uint32_t) i;
    mx.map[4] = 0x41;
    TEST_ASSERT_TRUE(memsearch_target(REGION_KIND_MEMEXT_RAM, 0x41, 0x123, 1, &mx, &t));
    TEST_ASSERT_TRUE(t.has_z80);
    TEST_ASSERT_EQUAL_HEX16(0x4123, t.z80_addr);
    TEST_ASSERT_EQUAL_INT(MEMSEARCH_BPZONE_MMEXT_BANK, t.bp_zone);
    TEST_ASSERT_EQUAL_INT(0x20, t.bp_bank);
    TEST_ASSERT_EQUAL_HEX16(0x1123, t.bp_addr);

    /* PEHU nenamapovaný kus: BP v bance platí dál, CPU adresa ne. */
    TEST_ASSERT_TRUE(memsearch_target(REGION_KIND_MEMEXT_RAM, 0x7E, 0x10, 1, &mx, &t));
    TEST_ASSERT_FALSE(t.has_z80);
    TEST_ASSERT_EQUAL_INT(0x3F, t.bp_bank);
    TEST_ASSERT_EQUAL_HEX16(0x0010, t.bp_addr);

    /* Luftner: jen namapovaný kus, CPU_VIEW. */
    mx.pehu = false;
    TEST_ASSERT_TRUE(memsearch_target(REGION_KIND_MEMEXT_RAM, 0x41, 0x1, 1, &mx, &t));
    TEST_ASSERT_EQUAL_INT(MEMSEARCH_BPZONE_CPU_VIEW, t.bp_zone);
    TEST_ASSERT_EQUAL_HEX16(0x4001, t.bp_addr);
    TEST_ASSERT_FALSE(memsearch_target(REGION_KIND_MEMEXT_RAM, 0x50, 0x1, 1, &mx, &t));
    TEST_ASSERT_EQUAL_INT(MEMSEARCH_BPZONE_NONE, t.bp_zone);

    /* VRAM plane a RAMdisk: nic. */
    TEST_ASSERT_FALSE(memsearch_target(REGION_KIND_VRAM_PHYS_PLANE, 0, 0x10, 1, &mx, &t));
    TEST_ASSERT_FALSE(memsearch_target(REGION_KIND_RAMDISK_STD, 3, 0x10, 1, NULL, &t));
}


/** @brief Informativně: start a filtr nad 16 MiB (čas do výstupu, bez pass/fail). */
void test_perf_16mib_informative(void)
{
    const uint32_t size = 16u * 1024u * 1024u;
    uint8_t *mem = g_malloc0(size);
    for (uint32_t i = 0; i < size; i++) mem[i] = (uint8_t) (i * 7u);
    st_MEMSEARCH_SPAN sp = { REGION_KIND_RAMDISK_STD, 0, 0, size };
    st_MEMSEARCH_FMT f = fmt_u8();
    st_MEMSEARCH_SESSION *s = memsearch_session_new(&sp, 1, &f, MEMSEARCH_HARD_MAX_SPACE, NULL);
    TEST_ASSERT_NOT_NULL(s);
    st_MEMSEARCH_SAMPLE *a = mk_sample(mem, size, 1);
    mem[12345] ^= 0xFF;
    st_MEMSEARCH_SAMPLE *b = mk_sample(mem, size, 1);

    gint64 t0 = g_get_monotonic_time();
    TEST_ASSERT_TRUE(memsearch_start(s, a, NULL, NULL));
    gint64 t1 = g_get_monotonic_time();
    st_MEMSEARCH_FILTER ch = flt(MEMSEARCH_OP_CHANGED, MEMSEARCH_CMP_PREVIOUS, 0);
    TEST_ASSERT_TRUE(memsearch_filter(s, b, &ch, NULL, NULL));
    gint64 t2 = g_get_monotonic_time();
    TEST_ASSERT_EQUAL_UINT64(1, memsearch_candidate_count(s));
    printf("memsearch 16 MiB: start %.1f ms, filter changed %.1f ms (informative)\n",
           (t1 - t0) / 1000.0, (t2 - t1) / 1000.0);

    memsearch_sample_unref(a);
    memsearch_sample_unref(b);
    memsearch_session_free(s);
    g_free(mem);
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_decode_integers);
    RUN_TEST(test_decode_bcd);
    RUN_TEST(test_decode_digits);
    RUN_TEST(test_encode_roundtrip);
    RUN_TEST(test_validate);
    RUN_TEST(test_flow_dec_by);
    RUN_TEST(test_ops_and_first);
    RUN_TEST(test_span_boundary_and_aligned);
    RUN_TEST(test_bcd_inc_over_99);
    RUN_TEST(test_digits_display_search);
    RUN_TEST(test_undo_redo_restores_reference);
    RUN_TEST(test_disconnected_span_keeps_candidates);
    RUN_TEST(test_eliminate_and_paging);
    RUN_TEST(test_limits);
    RUN_TEST(test_presets);
    RUN_TEST(test_target);
    RUN_TEST(test_perf_16mib_informative);
    int result = UNITY_END();

    mztest_teardown();
    return result;
}
