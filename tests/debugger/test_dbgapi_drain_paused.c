/**
 * @file test_dbgapi_drain_paused.c
 * @brief Drain fronty v pauze nezpracuje příkaz za krokem dřív, než se krok provede.
 *
 * Paused smyčka emulátoru vybírá frontu dbgapi (dbgapi_emu_drain_paused()).
 * Když klient pošle krok (STEP_INTO) a hned po jeho dokončení další příkaz,
 * může druhý příkaz dorazit, ještě než drain skončí. Drain bez kontroly
 * kroku by ho zpracoval ve stejném průchodu, tedy PŘED provedením kroku:
 * druhý krok by se ztratil (debugger_step_call(1) při nastaveném kroku nic
 * neudělá) a čtení by vidělo stav před krokem. Okno je krátké (ze zdroje,
 * v e2e neměřeno); změřenou ztrátu kroku na hranici snímku pokrývá
 * tests/mcp/test_step_frame_boundary.py.
 *
 * Test to modeluje deterministicky: dvě vlákna zařadí STEP_INTO a NONE,
 * hlavní vlákno (v roli emu vlákna v pauze) zavolá drain. Drain musí po
 * STEP_INTO skončit a NONE nechat ve frontě; po "provedení kroku" ho
 * zpracuje další drain.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>

#include "emulator.h"
#include "debugger/debugger.h"
#include "debugger/dbgapi_cmdrq.h"
#include "debugger/dbgapi_emu.h"
#include "debugger/dbgapi_ui.h"


/** @brief Parametr vlákna, které synchronně odešle jeden příkaz. */
typedef struct
{
    en_DBGAPI_CMD cmd;  /**< Odesílaný příkaz. */
    gint done;          /**< 1 = submit se vrátil (atomicky). */
    gint ok;            /**< Výsledek submitu (atomicky). */
} st_SUBMITTER;


/** @brief Vlákno: odešle příkaz a počká na jeho dokončení. */
static gpointer submitter_thread(gpointer user_data)
{
    st_SUBMITTER *s = (st_SUBMITTER *)user_data;
    bool ok = dbgapi_ui_submit_cmd_sync(&g_dbgapi_cmdrq_queue, s->cmd, NULL, NULL, 5000);
    g_atomic_int_set(&s->ok, ok ? 1 : 0);
    g_atomic_int_set(&s->done, 1);
    return NULL;
}


/** @brief Počet čekajících slotů ve frontě (jen pro test, čte indexy fronty pod jejím zámkem). */
static int pending_count(void)
{
    APP_MUTEX_LOCK(g_dbgapi_cmdrq_queue.queue_mutex);
    int n = (g_dbgapi_cmdrq_queue.tail - g_dbgapi_cmdrq_queue.head + DBGAPI_CMDRQ_QUEUE_SIZE)
            % DBGAPI_CMDRQ_QUEUE_SIZE;
    APP_MUTEX_UNLOCK(g_dbgapi_cmdrq_queue.queue_mutex);
    return n;
}


/** @brief Počká, až fronta obsahuje aspoň @p n slotů (nejvýš 2 s). */
static bool wait_pending(int n)
{
    for (int i = 0; i < 2000; i++)
    {
        if (pending_count() >= n) return true;
        g_usleep(1000);
    };
    return false;
}


void setUp(void)
{
    dbgapi_init(&g_dbgapi_cmdrq_queue);
    g_emulator.paused = true;
    g_debugger.step_call = 0;
}

void tearDown(void)
{
    g_debugger.step_call = 0;
    dbgapi_destroy(&g_dbgapi_cmdrq_queue);
}


/**
 * @brief Po STEP_INTO drain skončí, další příkaz počká na dobu po kroku.
 */
void test_drain_stops_after_step_request(void)
{
    st_SUBMITTER step = { DBGAPI_CMD_STEP_INTO, 0, 0 };
    st_SUBMITTER ping = { DBGAPI_CMD_NONE, 0, 0 };

    GThread *ts = g_thread_new("step", submitter_thread, &step);
    TEST_ASSERT_TRUE(wait_pending(1));
    GThread *tp = g_thread_new("ping", submitter_thread, &ping);
    TEST_ASSERT_TRUE(wait_pending(2));

    /* První drain: jen STEP_INTO, krok vyžádán, NONE zůstává ve frontě. */
    TEST_ASSERT_EQUAL_UINT(1, dbgapi_emu_drain_paused(&g_dbgapi_cmdrq_queue, NULL));
    TEST_ASSERT_TRUE(TEST_DEBUGGER_STEP_CALL);
    TEST_ASSERT_TRUE(dbgapi_emu_has_pending(&g_dbgapi_cmdrq_queue));
    TEST_ASSERT_EQUAL_INT(0, g_atomic_int_get(&ping.done));

    /* Krok proveden (paused smyčka po kroku shodí příznak). */
    debugger_step_call(0);
    TEST_ASSERT_EQUAL_UINT(1, dbgapi_emu_drain_paused(&g_dbgapi_cmdrq_queue, NULL));
    TEST_ASSERT_FALSE(dbgapi_emu_has_pending(&g_dbgapi_cmdrq_queue));

    g_thread_join(ts);
    g_thread_join(tp);
    TEST_ASSERT_EQUAL_INT(1, g_atomic_int_get(&step.ok));
    TEST_ASSERT_EQUAL_INT(1, g_atomic_int_get(&ping.ok));
}


/**
 * @brief Bez vyžádaného kroku drain vybere celou frontu.
 */
void test_drain_without_step_takes_all(void)
{
    st_SUBMITTER a = { DBGAPI_CMD_NONE, 0, 0 };
    st_SUBMITTER b = { DBGAPI_CMD_NONE, 0, 0 };
    GThread *ta = g_thread_new("a", submitter_thread, &a);
    TEST_ASSERT_TRUE(wait_pending(1));
    GThread *tb = g_thread_new("b", submitter_thread, &b);
    TEST_ASSERT_TRUE(wait_pending(2));

    TEST_ASSERT_EQUAL_UINT(2, dbgapi_emu_drain_paused(&g_dbgapi_cmdrq_queue, NULL));
    TEST_ASSERT_FALSE(dbgapi_emu_has_pending(&g_dbgapi_cmdrq_queue));

    g_thread_join(ta);
    g_thread_join(tb);
    TEST_ASSERT_EQUAL_INT(1, g_atomic_int_get(&a.ok));
    TEST_ASSERT_EQUAL_INT(1, g_atomic_int_get(&b.ok));
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_drain_stops_after_step_request);
    RUN_TEST(test_drain_without_step_takes_all);

    int result = UNITY_END();

    mztest_teardown();
    return result;
}
