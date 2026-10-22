/**
 * @file test_memsearch_window_queue.c
 * @brief Vzorek Memory Search pořizuje emu vlákno přes CMDRQ frontu.
 *
 * Okno Memory Search i MCP `memsearch_*` filtrují na svém vlákně, ale
 * vzorek paměti (kopie všech spanů prostoru v jednom okamžiku) pořizuje
 * emu vlákno v drainu fronty (DBGAPI_CMD_MEMSEARCH_SAMPLE, UI helper
 * dbg_ui_memsearch_sample z dbgapi_helpers.cpp).
 *
 * Ověřuje se přes skutečný helper:
 *  - bez vlákna, které frontu obsluhuje, se vzorek NEpořídí (helper vrátí
 *    false po timeoutu, vzorek beze změny, zrušený slot emu nevyzvedne),
 *  - se simulovaným emu vláknem se vzorek naplní z regionů (User RAM),
 *    odpojený span (MemExt bez připojení) má span_connected 0 a nulová
 *    data a vzorek nese číslo snímku,
 *  - vzorek s jiným rozměrem než spany emu vlákno odmítne.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <string.h>

#include "debugger/dbgapi_cmdrq.h"
#include "debugger/dbgapi_emu.h"
#include "debugger/dbgapi_ui.h"
#include "debugger/dbgapi_regions.h"
#include "debugger/memsearch/memsearch.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/memory/memext.h"
#include "hw-generic/gdg/gdg.h"
#include "../../src/ui-imgui/debugger/dbgapi_helpers.h"


/** @brief Začátek úseku User RAM ve vzorku. */
#define TEST_RAM_OFFSET 0x1000u

/** @brief Délka úseku User RAM ve vzorku. */
#define TEST_RAM_LEN    16u


/**
 * @brief Stav simulovaného emu vlákna.
 *
 * Vlákno obsluhuje frontu, dokud `stop` není nenulové, a počítá
 * vyzvednuté příkazy MEMSEARCH_SAMPLE.
 */
typedef struct {
    gint stop;          /**< 1 = ukončit smyčku (atomicky). */
    gint sample_seen;   /**< Počet vyzvednutých MEMSEARCH_SAMPLE. */
} st_FAKE_EMU;


/**
 * @brief Tělo simulovaného emu vlákna: drain fronty jako mzarch drain.
 * @param user_data st_FAKE_EMU*
 * @return NULL
 */
static gpointer fake_emu_thread(gpointer user_data)
{
    st_FAKE_EMU *fe = (st_FAKE_EMU *)user_data;
    while (!g_atomic_int_get(&fe->stop))
    {
        st_DBGAPI_CMDRQ *rq = dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue);
        if (!rq)
        {
            (void)dbgapi_emu_wait_for_cmd(&g_dbgapi_cmdrq_queue, 5);
            continue;
        };
        if ((rq->cmd & DBGAPI_CMD_MASK) == DBGAPI_CMD_MEMSEARCH_SAMPLE)
            g_atomic_int_inc(&fe->sample_seen);
        dbgapi_emu_dispatch(rq);
        dbgapi_emu_complete(rq);
    };
    return NULL;
}


/** @brief Spany testu: úsek User RAM a MemExt RAM kus 7Fh (odpojený). */
static const st_MEMSEARCH_SPAN s_spans[2] = {
    { REGION_KIND_RAM, 0, TEST_RAM_OFFSET, TEST_RAM_LEN },
    { REGION_KIND_MEMEXT_RAM, 0x7F, 0, 8 },
};


void setUp(void)
{
    dbgapi_init(&g_dbgapi_cmdrq_queue);
    memext_disconnect();
    for (uint32_t i = 0; i < TEST_RAM_LEN; i++)
        g_memory.RAM[TEST_RAM_OFFSET + i] = (uint8_t)(0xA0 + i);
}

void tearDown(void)
{
    dbgapi_destroy(&g_dbgapi_cmdrq_queue);
}


/**
 * @brief Bez emu vlákna se vzorek nepořídí (data beze změny).
 */
void test_sample_not_taken_without_emu_thread(void)
{
    st_MEMSEARCH_SAMPLE *s = memsearch_sample_new(TEST_RAM_LEN + 8, 2);
    TEST_ASSERT_NOT_NULL(s);
    memset(s->data, 0xEE, s->size);

    TEST_ASSERT_FALSE(dbg_ui_memsearch_sample(s_spans, 2, s));
    TEST_ASSERT_EQUAL_HEX8(0xEE, s->data[0]);
    TEST_ASSERT_EQUAL_UINT8(0, s->span_connected[0]);

    /* Zrušený příkaz emu nevyzvedne. */
    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue));
    memsearch_sample_unref(s);
}


/**
 * @brief Se simulovaným emu vláknem se vzorek naplní, odpojený span se
 *        označí a vzorek s jiným rozměrem se odmítne.
 */
void test_sample_taken_by_emu_thread(void)
{
    st_FAKE_EMU fe = { 0 };
    g_gdg.total_elapsed.screens = 1234;
    st_MEMSEARCH_SAMPLE *s = memsearch_sample_new(TEST_RAM_LEN + 8, 2);
    st_MEMSEARCH_SAMPLE *wrong = memsearch_sample_new(TEST_RAM_LEN + 9, 2);
    memset(s->data, 0xEE, s->size);

    GThread *t = g_thread_new("fake-emu", fake_emu_thread, &fe);
    bool ok = dbg_ui_memsearch_sample(s_spans, 2, s);
    bool ok_wrong = dbg_ui_memsearch_sample(s_spans, 2, wrong);
    g_atomic_int_set(&fe.stop, 1);
    g_thread_join(t);

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_FALSE(ok_wrong);
    TEST_ASSERT_EQUAL_INT(2, g_atomic_int_get(&fe.sample_seen));

    TEST_ASSERT_EQUAL_UINT8(1, s->span_connected[0]);
    for (uint32_t i = 0; i < TEST_RAM_LEN; i++)
        TEST_ASSERT_EQUAL_HEX8(0xA0 + i, s->data[i]);

    /* MemExt odpojený: span 1 odpojený a vynulovaný. */
    TEST_ASSERT_EQUAL_UINT8(0, s->span_connected[1]);
    for (uint32_t i = 0; i < 8; i++)
        TEST_ASSERT_EQUAL_HEX8(0x00, s->data[TEST_RAM_LEN + i]);

    TEST_ASSERT_EQUAL_UINT64(1234, s->frame);

    memsearch_sample_unref(s);
    memsearch_sample_unref(wrong);
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_sample_not_taken_without_emu_thread);
    RUN_TEST(test_sample_taken_by_emu_thread);

    int result = UNITY_END();

    mztest_teardown();
    return result;
}
