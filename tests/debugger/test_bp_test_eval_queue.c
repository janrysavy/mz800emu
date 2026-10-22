/**
 * @file test_bp_test_eval_queue.c
 * @brief Test Eval z editačního panelu breakpointu jde přes CMDRQ frontu
 *        a znak `!` ve výrazu ignoruje.
 *
 * Regresní test pro souběh v editačním panelu BP (bpt_edit_panel.cpp,
 * tlačítko Test Eval): UI vlákno dřív vyhodnocovalo výraz samo
 * (bp_expr_parse_eval s breakpoints_fill_global_ctx), tedy četlo registry,
 * paměť a porty souběžně s emu vláknem. `[addr]!` navíc vyhodnotilo MEM_R
 * breakpointy z UI vlákna (zásah = hits++ a pauza) a `port[N]!` provedlo
 * skutečné čtení portu, přestože dokumentace Test Eval slibuje, že se `!`
 * ignoruje. Nově panel volá dbg_ui_bp_test_eval(), které odešle
 * DBGAPI_CMD_BP_TEST_EVAL; výraz vyhodnotí emu vlákno s příznakem
 * bp_expr_ctx_t.no_side_effects.
 *
 * Ověřuje se přes skutečný helper z dbgapi_helpers.cpp:
 *  - bez vlákna, které frontu obsluhuje, se výraz nevyhodnotí (helper vrátí
 *    false po timeoutu, zrušený slot emu nevyzvedne),
 *  - se simulovaným emu vláknem výraz projde, chyba parseru se vrátí,
 *  - `[addr]!` nad MEM_R breakpointem breakpoint nespustí (hits 0, bez pauzy).
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <string.h>

#include "debugger/dbgapi_cmdrq.h"
#include "debugger/dbgapi_emu.h"
#include "debugger/dbgapi_ui.h"
#include "debugger/breakpoints.h"
#include "debugger/bptmap.h"
#include "emulator/emulator.h"
#include "../../src/ui-imgui/debugger/dbgapi_helpers.h"


/** @brief Adresa MEM_R breakpointu v testu side effectu. */
#define TEST_MEMR_ADDR 0x4000u


/**
 * @brief Stav simulovaného emu vlákna.
 *
 * Vlákno obsluhuje frontu, dokud `stop` není nenulové, a počítá vyzvednuté
 * příkazy Test Eval. Žije na zásobníku testu, test na vlákno čeká přes
 * g_thread_join.
 */
typedef struct {
    gint stop;          /**< 1 = ukončit smyčku (atomicky). */
    gint eval_seen;     /**< Počet vyzvednutých BP_TEST_EVAL. */
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
        if ((rq->cmd & DBGAPI_CMD_MASK) == DBGAPI_CMD_BP_TEST_EVAL)
            g_atomic_int_inc(&fe->eval_seen);
        dbgapi_emu_dispatch(rq);
        dbgapi_emu_complete(rq);
    };
    return NULL;
}


/**
 * @brief Vyhodnotí výraz přes helper se simulovaným emu vláknem.
 * @param expr Výraz.
 * @param p Výstup (vyplní helper / emu vlákno).
 * @param seen Výstup: kolikrát emu vlákno vyzvedlo BP_TEST_EVAL.
 * @return Návratová hodnota dbg_ui_bp_test_eval().
 */
static bool eval_with_emu(const char *expr, st_DBGAPI_BP_TEST_EVAL_PARAM *p, int *seen)
{
    st_FAKE_EMU fe = { 0 };
    GThread *t = g_thread_new("fake-emu", fake_emu_thread, &fe);
    memset(p, 0, sizeof(*p));
    p->expr_text = expr;
    bool ok = dbg_ui_bp_test_eval(p);
    g_atomic_int_set(&fe.stop, 1);
    g_thread_join(t);
    *seen = g_atomic_int_get(&fe.eval_seen);
    return ok;
}


void setUp(void)
{
    dbgapi_init(&g_dbgapi_cmdrq_queue);
    breakpoints_clear_all();
    g_breakpoints.next_id = 1;
    g_emulator.paused = false;
}

void tearDown(void)
{
    breakpoints_clear_all();
    dbgapi_destroy(&g_dbgapi_cmdrq_queue);
    g_emulator.paused = false;
}


/**
 * @brief Bez emu vlákna se výraz nevyhodnotí a příkaz zůstane zrušený.
 */
void test_eval_not_applied_without_emu_thread(void)
{
    st_DBGAPI_BP_TEST_EVAL_PARAM p;
    memset(&p, 0, sizeof(p));
    p.expr_text = "2 + 3";
    p.out_value = 0x1234;

    TEST_ASSERT_FALSE(dbg_ui_bp_test_eval(&p));
    TEST_ASSERT_FALSE(p.out_ok);
    TEST_ASSERT_EQUAL_INT32(0x1234, p.out_value);

    /* Zrušený příkaz emu nevyzvedne. */
    TEST_ASSERT_NULL(dbgapi_emu_dequeue(&g_dbgapi_cmdrq_queue));
}


/**
 * @brief Se simulovaným emu vláknem se výraz vyhodnotí, chyba parseru se vrátí.
 */
void test_eval_executed_by_emu_thread(void)
{
    st_DBGAPI_BP_TEST_EVAL_PARAM p;
    int seen = 0;

    TEST_ASSERT_TRUE(eval_with_emu("2 + 3 * 4", &p, &seen));
    TEST_ASSERT_EQUAL_INT(1, seen);
    TEST_ASSERT_TRUE(p.out_ok);
    TEST_ASSERT_EQUAL_INT32(14, p.out_value);
    TEST_ASSERT_EQUAL_STRING("", p.out_error);

    TEST_ASSERT_TRUE(eval_with_emu("2 +", &p, &seen));
    TEST_ASSERT_EQUAL_INT(1, seen);
    TEST_ASSERT_FALSE(p.out_ok);
    TEST_ASSERT_TRUE(p.out_error[0] != '\0');
}


/**
 * @brief `[addr]!` v Test Eval nespustí MEM_R breakpoint na téže adrese.
 *
 * Dokumentace Test Eval: znak `!` se ignoruje (čtení bez vedlejšího
 * efektu). Bez příznaku no_side_effects by memory_read_byte() vyhodnotil
 * MEM_R breakpoint: hits by vzrostlo a emulátor by se pozastavil.
 */
void test_eval_ignores_side_effect_read(void)
{
    int id = breakpoints_add(TEST_MEMR_ADDR, "MemR", -1);
    TEST_ASSERT_GREATER_OR_EQUAL(1, id);
    breakpoints_set_type(id, BPT_TYPE_MEM_R);
    TEST_ASSERT_TRUE(g_bptmap.per_type_active[BPTMAP_IDX_MEM_R]);

    st_DBGAPI_BP_TEST_EVAL_PARAM p;
    int seen = 0;
    TEST_ASSERT_TRUE(eval_with_emu("[0x4000]! + 1", &p, &seen));
    TEST_ASSERT_TRUE(p.out_ok);

    st_BPT *bpt = breakpoints_find_by_id(id);
    TEST_ASSERT_NOT_NULL(bpt);
    TEST_ASSERT_EQUAL_UINT64(0, bpt->hits);
    TEST_ASSERT_FALSE(g_emulator.paused);
}


int main(void)
{
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_eval_not_applied_without_emu_thread);
    RUN_TEST(test_eval_executed_by_emu_thread);
    RUN_TEST(test_eval_ignores_side_effect_read);

    int result = UNITY_END();

    mztest_teardown();
    return result;
}
