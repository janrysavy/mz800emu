/**
 * @file test_ramlog_window_queue.c
 * @brief Okno RAM Write Log a Memory Browser ovládají záznam zápisů přes
 *        CMDRQ frontu (dbg_ui_ramlog), ne přímo z UI vlákna.
 *
 * Start a stop záznamu přepínají callbacky CPU a dotazy čtou ring, do kterého
 * zapisuje emu vlákno; proto UI helper dbg_ui_ramlog (dbgapi_helpers.cpp)
 * odešle DBGAPI_CMD_RAMLOG a operaci vykoná emu vlákno.
 *
 * Ověřuje se přes skutečný helper:
 *  - bez vlákna, které frontu obsluhuje, se start neprovede (helper vrátí
 *    false s prázdným out_error = timeout, záznam neběží, zrušený slot emu
 *    nevyzvedne),
 *  - se simulovaným emu vláknem (dequeue + dbgapi_emu_dispatch + complete)
 *    projde start, dotaz "kdo zapsal" podle regionu Memory Browseru
 *    (RAM i nepodporovaná ROM), nejnovější záznamy (RECENT) a stop.
 *
 * Zápis do paměti jde přes memory_write_byte_no_bp (zdroj DEBUGGER), vždy
 * jen když simulované emu vlákno neběží.
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
#include "debugger/trace/ramlog.h"
#include "hw-generic/memory/memory.h"
#include "../../src/ui-imgui/debugger/dbgapi_helpers.h"


/** @brief Stav simulovaného emu vlákna (vzor test_callstack_window_queue.c). */
typedef struct {
    gint stop;          /**< 1 = ukončit smyčku (atomicky). */
    gint ramlog_seen;   /**< Počet vyzvednutých DBGAPI_CMD_RAMLOG. */
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
        if ((rq->cmd & DBGAPI_CMD_MASK) == DBGAPI_CMD_RAMLOG)
        {
            g_atomic_int_inc(&fe->ramlog_seen);
        };
        dbgapi_emu_dispatch(rq);
        dbgapi_emu_complete(rq);
    };
    return NULL;
}


/** @brief Parametr operace s nulovými výstupy. */
static st_DBGAPI_RAMLOG_PARAM mk(en_DBGAPI_RAMLOG_OP op)
{
    st_DBGAPI_RAMLOG_PARAM p;
    memset(&p, 0, sizeof(p));
    p.op = op;
    return p;
}


void setUp(void)
{
    dbgapi_init(&g_dbgapi_cmdrq_queue);
}

void tearDown(void)
{
    ramlog_free();
    dbgapi_destroy(&g_dbgapi_cmdrq_queue);
}


/**
 * @brief Bez emu vlákna se start neprovede (timeout, záznam neběží).
 */
void test_start_not_applied_without_emu_thread(void)
{
    st_DBGAPI_RAMLOG_PARAM p = mk(DBGAPI_RAMLOG_OP_START);
    p.cfg.mode = RAMLOG_MODE_RING;
    p.cfg.capacity = 64;
    TEST_ASSERT_FALSE(dbg_ui_ramlog(&p));
    TEST_ASSERT_EQUAL_CHAR('\0', p.out_error[0]);
    TEST_ASSERT_EQUAL_INT(0, g_ramlog_active);
    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue));
}


/**
 * @brief Se simulovaným emu vláknem projde start, dotaz podle regionu,
 *        RECENT i stop.
 */
void test_ops_executed_by_emu_thread(void)
{
    st_FAKE_EMU fe = { 0 };
    GThread *t = g_thread_new("fake-emu", fake_emu_thread, &fe);
    st_DBGAPI_RAMLOG_PARAM p = mk(DBGAPI_RAMLOG_OP_START);
    p.cfg.mode = RAMLOG_MODE_RING;
    p.cfg.capacity = 64;
    p.cfg.n_ranges = 1;
    p.cfg.ranges[0].from = 0x4000;
    p.cfg.ranges[0].to = 0x4001;
    bool ok_start = dbg_ui_ramlog(&p);
    bool active = p.out_status.active;
    g_atomic_int_set(&fe.stop, 1);
    g_thread_join(t);
    TEST_ASSERT_TRUE(ok_start);
    TEST_ASSERT_TRUE(active);

    /* Zápisy bez běžícího simulovaného emu vlákna (debugger cesta). */
    memory_write_byte_no_bp(0x4000, 0x11);
    memory_write_byte_no_bp(0x4001, 0x22);
    memory_write_byte_no_bp(0x5000, 0x33);   /* mimo rozsah */

    g_atomic_int_set(&fe.stop, 0);
    t = g_thread_new("fake-emu", fake_emu_thread, &fe);

    /* Kdo zapsal bajt 4001h regionu RAM (Memory Browser). */
    st_RAMLOG_REC recs[4 * 8];
    uint64_t seqs[4 * 8];
    st_DBGAPI_RAMLOG_PARAM q = mk(DBGAPI_RAMLOG_OP_WHO_WROTE);
    q.by_region = true;
    q.region_kind = REGION_KIND_RAM;
    q.region_sub = 0;
    q.region_off = 0x4001;
    q.limit = 8;
    q.out_recs = recs;
    q.out_seqs = seqs;
    bool ok_q = dbg_ui_ramlog(&q);

    /* ROM region záznam nepokrývá: chyba s textem. */
    st_DBGAPI_RAMLOG_PARAM qr = mk(DBGAPI_RAMLOG_OP_WHO_WROTE);
    qr.by_region = true;
    qr.region_kind = REGION_KIND_ROM_LOWER;
    qr.region_off = 0x10;
    bool ok_rom = dbg_ui_ramlog(&qr);

    /* Nejnovější záznamy. */
    st_RAMLOG_REC rr[8];
    uint64_t rs[8];
    st_DBGAPI_RAMLOG_PARAM r = mk(DBGAPI_RAMLOG_OP_RECENT);
    r.limit = 8;
    r.out_recs = rr;
    r.out_seqs = rs;
    bool ok_recent = dbg_ui_ramlog(&r);

    st_DBGAPI_RAMLOG_PARAM s = mk(DBGAPI_RAMLOG_OP_STOP);
    bool ok_stop = dbg_ui_ramlog(&s);

    g_atomic_int_set(&fe.stop, 1);
    g_thread_join(t);

    TEST_ASSERT_TRUE(ok_q);
    TEST_ASSERT_EQUAL_UINT(1, q.out_n_cells);
    TEST_ASSERT_TRUE(q.out_cells[0].has_last);
    TEST_ASSERT_EQUAL_UINT8(RAMLOG_SRC_DEBUGGER, q.out_cells[0].last.src);
    TEST_ASSERT_EQUAL_UINT(1, q.out_cells[0].n_recs);
    TEST_ASSERT_EQUAL_UINT8(0x22, recs[0].new_value);
    TEST_ASSERT_EQUAL_UINT64(1, seqs[0]);

    TEST_ASSERT_FALSE(ok_rom);
    TEST_ASSERT_TRUE(qr.out_error[0] != '\0');

    TEST_ASSERT_TRUE(ok_recent);
    TEST_ASSERT_EQUAL_UINT(2, r.out_n_recs);
    TEST_ASSERT_EQUAL_UINT64(1, rs[0]);           /* od nejnovějšího */
    TEST_ASSERT_EQUAL_UINT16(0x4001, rr[0].cpu_addr);
    TEST_ASSERT_EQUAL_UINT16(0x4000, rr[1].cpu_addr);

    TEST_ASSERT_TRUE(ok_stop);
    TEST_ASSERT_FALSE(s.out_status.active);
    TEST_ASSERT_EQUAL_INT(5, g_atomic_int_get(&fe.ramlog_seen));
}


int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_start_not_applied_without_emu_thread);
    RUN_TEST(test_ops_executed_by_emu_thread);

    int result = UNITY_END();

    mztest_teardown();
    return result;
}
