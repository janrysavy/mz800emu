/**
 * @file test_dbgapi_dequeue_matching.c
 * @brief Testy výběru příkazu z fronty CMDRQ mimo pořadí.
 *
 * dbgapi_emu_dequeue_matching() vybírá v drainu na hranici akce BP jen
 * řídicí příkazy, i když ve frontě čekají za jinými (drain běží uprostřed
 * instrukce, ostatní příkazy musí počkat na hranici snímku). Testy ověřují:
 *  - vybere se první vyhovující příkaz za nevyhovujícími, ostatní zůstanou
 *    v pořadí; dbgapi_emu_dequeue() vykonaný slot přeskočí,
 *  - vyhovující příkaz v hlavě fronty se vyjme normálně (posune head),
 *  - zrušený slot (CANCELLED) se nevybere a dequeue ho uvolní,
 *  - bez vyhovujícího příkazu se nic nevybere a fronta se nezmění.
 *
 * Fronta se plní přímo (bílá skříňka, jako zařazení v
 * dbgapi_ui_submit_cmd_sync_ex() bez čekání odesílatele), test běží
 * v jednom vlákně.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include "debugger/dbgapi_cmdrq.h"
#include "debugger/dbgapi_emu.h"

#include <glib.h>


/** @brief Fronta testu (inicializuje setUp). */
static st_DBGAPI_CMDRQ_QUEUE s_queue;


void setUp(void)
{
    dbgapi_init(&s_queue);
}


void tearDown(void)
{
    dbgapi_destroy(&s_queue);
}


/**
 * @brief Zařadí příkaz do fronty jako odesílatel (bez čekání na výsledek).
 * @param cmd Příkaz.
 * @return Slot příkazu.
 */
static st_DBGAPI_CMDRQ *put(en_DBGAPI_CMD cmd)
{
    st_DBGAPI_CMDRQ *slot = &s_queue.cmdrq[s_queue.tail];
    s_queue.tail = (s_queue.tail + 1) % DBGAPI_CMDRQ_QUEUE_SIZE;
    slot->cmd = cmd;
    slot->cmd_state = DBGAPI_CMDSTATE_PENDING;
    slot->dequeued = false;
    slot->done_early = false;
    slot->success = false;
    return slot;
}


/**
 * @brief Dokončí slot a uvolní ho jako odesílatel po přečtení výsledku.
 * @param rq Slot vyzvednutý emu stranou.
 */
static void finish(st_DBGAPI_CMDRQ *rq)
{
    dbgapi_emu_complete(rq);
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMDSTATE_PROCESSED, rq->cmd_state);
    rq->cmd_state = DBGAPI_CMDSTATE_NONE;
}


/** @brief Predikát drainu na hranici akce BP (pauza a stav běhu). */
static bool is_control(en_DBGAPI_CMD cmd)
{
    return cmd == DBGAPI_CMD_PAUSE || cmd == DBGAPI_CMD_IS_RUNNING;
}


void test_matching_picks_control_behind_others(void)
{
    st_DBGAPI_CMDRQ *a = put(DBGAPI_CMD_MEM_READ);
    st_DBGAPI_CMDRQ *p = put(DBGAPI_CMD_PAUSE);
    st_DBGAPI_CMDRQ *b = put(DBGAPI_CMD_GET_ALL_REGS);
    int head = s_queue.head;

    st_DBGAPI_CMDRQ *rq = dbgapi_emu_dequeue_matching(&s_queue, is_control);
    TEST_ASSERT_EQUAL_PTR(p, rq);
    TEST_ASSERT_TRUE(rq->dequeued);
    TEST_ASSERT_TRUE(rq->done_early);
    TEST_ASSERT_EQUAL_INT(head, s_queue.head);
    finish(rq);

    TEST_ASSERT_NULL(dbgapi_emu_dequeue_matching(&s_queue, is_control));

    rq = dbgapi_emu_dequeue(&s_queue);
    TEST_ASSERT_EQUAL_PTR(a, rq);
    finish(rq);
    rq = dbgapi_emu_dequeue(&s_queue);
    TEST_ASSERT_EQUAL_PTR(b, rq);
    TEST_ASSERT_FALSE(p->done_early);
    finish(rq);
    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&s_queue));
    TEST_ASSERT_FALSE(dbgapi_emu_has_pending(&s_queue));
}


void test_matching_at_head_advances_head(void)
{
    st_DBGAPI_CMDRQ *p = put(DBGAPI_CMD_IS_RUNNING);
    st_DBGAPI_CMDRQ *a = put(DBGAPI_CMD_MEM_READ);
    int head = s_queue.head;

    st_DBGAPI_CMDRQ *rq = dbgapi_emu_dequeue_matching(&s_queue, is_control);
    TEST_ASSERT_EQUAL_PTR(p, rq);
    TEST_ASSERT_FALSE(rq->done_early);
    TEST_ASSERT_EQUAL_INT((head + 1) % DBGAPI_CMDRQ_QUEUE_SIZE, s_queue.head);
    finish(rq);

    rq = dbgapi_emu_dequeue(&s_queue);
    TEST_ASSERT_EQUAL_PTR(a, rq);
    finish(rq);
    TEST_ASSERT_FALSE(dbgapi_emu_has_pending(&s_queue));
}


void test_matching_skips_cancelled(void)
{
    st_DBGAPI_CMDRQ *c = put(DBGAPI_CMD_PAUSE);
    c->cmd_state = DBGAPI_CMDSTATE_CANCELLED;
    st_DBGAPI_CMDRQ *p = put(DBGAPI_CMD_PAUSE);

    st_DBGAPI_CMDRQ *rq = dbgapi_emu_dequeue_matching(&s_queue, is_control);
    TEST_ASSERT_EQUAL_PTR(p, rq);
    finish(rq);

    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&s_queue));
    TEST_ASSERT_EQUAL_INT(DBGAPI_CMDSTATE_NONE, c->cmd_state);
    TEST_ASSERT_FALSE(dbgapi_emu_has_pending(&s_queue));
}


void test_matching_none_leaves_queue(void)
{
    st_DBGAPI_CMDRQ *a = put(DBGAPI_CMD_MEM_READ);
    int head = s_queue.head;
    int tail = s_queue.tail;

    TEST_ASSERT_NULL(dbgapi_emu_dequeue_matching(&s_queue, is_control));
    TEST_ASSERT_NULL(dbgapi_emu_dequeue_matching(&s_queue, NULL));
    TEST_ASSERT_EQUAL_INT(head, s_queue.head);
    TEST_ASSERT_EQUAL_INT(tail, s_queue.tail);
    TEST_ASSERT_FALSE(a->dequeued);

    st_DBGAPI_CMDRQ *rq = dbgapi_emu_dequeue(&s_queue);
    TEST_ASSERT_EQUAL_PTR(a, rq);
    finish(rq);
}


int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();
    RUN_TEST(test_matching_picks_control_behind_others);
    RUN_TEST(test_matching_at_head_advances_head);
    RUN_TEST(test_matching_skips_cancelled);
    RUN_TEST(test_matching_none_leaves_queue);
    int result = UNITY_END();

    mztest_teardown();
    return result;
}
