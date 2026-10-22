/**
 * @file   test_run_log.c
 * @brief  Unit testy čísel běhů a kruhu záznamů zastavení (run_log.h) bez emulátoru.
 *
 * Ověřuje:
 * - číslování běhů (begin zvedá číslo, end ukončí, current hlásí stav),
 * - čekání: hotový běh vrátí hned, běžící běh po timeoutu, konec běhu
 *   z jiného vlákna čekání probudí, budoucí běh čeká,
 * - přepsání kruhu (záznam vypadl: DONE bez záznamu), historii od nejstaršího,
 * - ukončení modulu probudí čekání a další čekání vrací hned SHUTDOWN.
 *
 * @par Licence: GPLv3
 */

#include "unity.h"

#include <string.h>
#include <glib.h>

#include "run_log.h"


void setUp(void)
{
    run_log_reset_for_tests();
}

void tearDown(void) {}


/** @brief Ukončí aktuální běh záznamem s důvodem a PC. */
static void end_run(const char *reason, uint16_t pc)
{
    st_RUN_LOG_RECORD r;
    run_log_record_clear(&r);
    r.reason = reason;
    r.pc = pc;
    run_log_end(&r);
}


/** @brief Číslování: běh 0 běží od startu, begin/end střídají stav. */
void test_run_log_numbering(void)
{
    bool running = false;
    TEST_ASSERT_EQUAL_UINT64(0, run_log_current(&running));
    TEST_ASSERT_TRUE(running);

    end_run("manual", 0x1000);
    TEST_ASSERT_EQUAL_UINT64(0, run_log_current(&running));
    TEST_ASSERT_FALSE(running);

    TEST_ASSERT_EQUAL_UINT64(1, run_log_begin());
    TEST_ASSERT_EQUAL_UINT64(1, run_log_current(&running));
    TEST_ASSERT_TRUE(running);
    end_run("step", 0x1001);

    st_RUN_LOG_RECORD out;
    bool found = false;
    TEST_ASSERT_EQUAL_INT(RUN_LOG_WAIT_DONE, run_log_wait(1, 0, &out, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_UINT64(1, out.run_seq);
    TEST_ASSERT_EQUAL_STRING("step", out.reason);
    TEST_ASSERT_EQUAL_HEX16(0x1001, out.pc);
    TEST_ASSERT_EQUAL_INT(-1, out.bp_id);
    TEST_ASSERT_EQUAL_STRING("", out.started_by);

    /* Starší běh skončil taky. */
    TEST_ASSERT_EQUAL_INT(RUN_LOG_WAIT_DONE, run_log_wait(0, 0, &out, &found));
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_STRING("manual", out.reason);
}


/** @brief Běžící a budoucí běh do timeoutu neskončí. */
void test_run_log_timeout(void)
{
    run_log_begin(); /* běh 1 běží */
    gint64 t0 = g_get_monotonic_time();
    TEST_ASSERT_EQUAL_INT(RUN_LOG_WAIT_TIMEOUT, run_log_wait(1, 50, NULL, NULL));
    gint64 dt = g_get_monotonic_time() - t0;
    TEST_ASSERT_TRUE(dt >= 45 * G_TIME_SPAN_MILLISECOND);

    end_run("breakpoint", 0x2000);
    /* Budoucí běh 2 ještě nezačal. */
    TEST_ASSERT_EQUAL_INT(RUN_LOG_WAIT_TIMEOUT, run_log_wait(2, 10, NULL, NULL));
}


/** @brief Data vlákna, které po prodlevě ukončí běh. */
typedef struct
{
    int delay_ms;   /**< Prodleva před koncem běhu. */
} st_ENDER;

/** @brief Vlákno: počká a ukončí aktuální běh. */
static gpointer ender_thread(gpointer user_data)
{
    st_ENDER *e = (st_ENDER *)user_data;
    g_usleep((gulong)e->delay_ms * 1000);
    end_run("deadline", 0x3000);
    return NULL;
}


/** @brief Konec běhu z jiného vlákna probudí čekání dřív než timeout. */
void test_run_log_wakeup(void)
{
    uint64_t seq = run_log_begin();
    st_ENDER e = { 50 };
    GThread *t = g_thread_new("ender", ender_thread, &e);
    st_RUN_LOG_RECORD out;
    bool found = false;
    gint64 t0 = g_get_monotonic_time();
    TEST_ASSERT_EQUAL_INT(RUN_LOG_WAIT_DONE, run_log_wait(seq, 5000, &out, &found));
    gint64 dt = g_get_monotonic_time() - t0;
    g_thread_join(t);
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_EQUAL_STRING("deadline", out.reason);
    TEST_ASSERT_TRUE(dt < 4000 * G_TIME_SPAN_MILLISECOND);
}


/** @brief Přepsaný kruh: DONE bez záznamu; historie nejnovějších od nejstaršího. */
void test_run_log_ring_and_history(void)
{
    end_run("manual", 0);                           /* běh 0 */
    for (unsigned i = 0; i < RUN_LOG_CAPACITY + 3; i++)
    {
        run_log_begin();                            /* běh i + 1 */
        end_run("step", (uint16_t)(i + 1));
    };
    bool found = true;
    TEST_ASSERT_EQUAL_INT(RUN_LOG_WAIT_DONE, run_log_wait(0, 0, NULL, &found));
    TEST_ASSERT_FALSE(found);

    st_RUN_LOG_RECORD hist[RUN_LOG_CAPACITY];
    TEST_ASSERT_EQUAL_UINT(RUN_LOG_CAPACITY, run_log_history(hist, RUN_LOG_CAPACITY));
    uint64_t last = RUN_LOG_CAPACITY + 3;
    TEST_ASSERT_EQUAL_UINT64(last - RUN_LOG_CAPACITY + 1, hist[0].run_seq);
    TEST_ASSERT_EQUAL_UINT64(last, hist[RUN_LOG_CAPACITY - 1].run_seq);

    TEST_ASSERT_EQUAL_UINT(2, run_log_history(hist, 2));
    TEST_ASSERT_EQUAL_UINT64(last - 1, hist[0].run_seq);
    TEST_ASSERT_EQUAL_UINT64(last, hist[1].run_seq);
}


/** @brief Detail se ořízne a uzavře nulou i při plném bufferu. */
void test_run_log_detail_terminated(void)
{
    st_RUN_LOG_RECORD r;
    run_log_record_clear(&r);
    r.reason = "exit";
    memset(r.detail, 'x', sizeof(r.detail));
    run_log_end(&r);
    st_RUN_LOG_RECORD out;
    TEST_ASSERT_EQUAL_INT(RUN_LOG_WAIT_DONE, run_log_wait(0, 0, &out, NULL));
    TEST_ASSERT_EQUAL_size_t(RUN_LOG_DETAIL_SIZE - 1, strlen(out.detail));
}


/** @brief Vlákno: po prodlevě ukončí modul. */
static gpointer shutdown_thread(gpointer user_data)
{
    (void)user_data;
    g_usleep(50 * 1000);
    run_log_shutdown();
    return NULL;
}


/** @brief Ukončení probudí čekání a další čekání vrací hned SHUTDOWN. */
void test_run_log_shutdown(void)
{
    uint64_t seq = run_log_begin();
    GThread *t = g_thread_new("shutdown", shutdown_thread, NULL);
    gint64 t0 = g_get_monotonic_time();
    TEST_ASSERT_EQUAL_INT(RUN_LOG_WAIT_SHUTDOWN, run_log_wait(seq, 5000, NULL, NULL));
    TEST_ASSERT_TRUE(g_get_monotonic_time() - t0 < 4000 * G_TIME_SPAN_MILLISECOND);
    g_thread_join(t);
    TEST_ASSERT_EQUAL_INT(RUN_LOG_WAIT_SHUTDOWN, run_log_wait(seq, 5000, NULL, NULL));

    /* Hotový běh se vrací i po ukončení. */
    end_run("manual", 0);
    TEST_ASSERT_EQUAL_INT(RUN_LOG_WAIT_DONE, run_log_wait(seq, 0, NULL, NULL));
}


int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_run_log_numbering);
    RUN_TEST(test_run_log_timeout);
    RUN_TEST(test_run_log_wakeup);
    RUN_TEST(test_run_log_ring_and_history);
    RUN_TEST(test_run_log_detail_terminated);
    RUN_TEST(test_run_log_shutdown);
    return UNITY_END();
}
