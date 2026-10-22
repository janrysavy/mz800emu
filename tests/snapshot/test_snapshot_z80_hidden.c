/*
 * test_snapshot_z80_hidden.c - snapshot uloží a obnoví skrytý stav Z80
 *
 * Testuje handler snapshotu "z80" (cpu/z80.xml) přes buffer API
 * (snapshot_save_to_buffer / snapshot_load_from_buffer):
 *   - round-trip všech polí skrytého stavu, která jádro drží mezi
 *     instrukcemi: WZ (MEMPTR), Q, EI delay, čekající INT a NMI, vektor
 *     přerušení, příznak LD A,I/R a total_cycles.
 *
 * Pole se nastavují přímo ve struktuře z80_t, protože Q (v buildech MZ na
 * hranici instrukce vždy 0), vektor přerušení a příznak LD A,I/R přes MCP
 * nastavit ani přečíst nejdou. Načtení starého snapshotu bez prvku
 * hidden_state ověřuje e2e test tests/mcp/test_snapshot_z80_hidden.py.
 *
 * Licence: GPLv3
 */

#include "mztest.h"
#include <glib.h>
#include <string.h>

#include "emulator/snapshot/snapshot.h"
#include "emulator/emulator.h"
#include "mzarch/mzarch.h"

/** @brief Hodnoty skrytého stavu Z80, které test ukládá a porovnává. */
typedef struct {
    uint16_t wz;            /**< MEMPTR */
    uint8_t q;              /**< interní Q */
    bool ei_delay;          /**< EI delay */
    bool int_pending;       /**< čekající INT */
    bool nmi_pending;       /**< čekající NMI */
    uint8_t int_vector;     /**< vektor přerušení */
    bool ld_a_ir;           /**< příznak LD A,I/R */
    uint32_t total_cycles;  /**< celkové T-stavy */
} st_HIDDEN;

/** @brief Zapíše hodnoty do skrytého stavu CPU. */
static void hidden_set(const st_HIDDEN *h)
{
    z80_t *cpu = g_mzarch_main.cpu;
    cpu->wz.w = h->wz;
    cpu->q = h->q;
    cpu->ei_delay = h->ei_delay;
    cpu->int_pending = h->int_pending;
    cpu->nmi_pending = h->nmi_pending;
    cpu->int_vector = h->int_vector;
    cpu->ld_a_ir = h->ld_a_ir;
    cpu->total_cycles = h->total_cycles;
}

/** @brief Ověří, že skrytý stav CPU odpovídá h. */
static void hidden_assert(const st_HIDDEN *h)
{
    z80_t *cpu = g_mzarch_main.cpu;
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(h->wz, cpu->wz.w, "WZ");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(h->q, cpu->q, "Q");
    TEST_ASSERT_EQUAL_MESSAGE(h->ei_delay, cpu->ei_delay, "ei_delay");
    TEST_ASSERT_EQUAL_MESSAGE(h->int_pending, cpu->int_pending, "int_pending");
    TEST_ASSERT_EQUAL_MESSAGE(h->nmi_pending, cpu->nmi_pending, "nmi_pending");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(h->int_vector, cpu->int_vector, "int_vector");
    TEST_ASSERT_EQUAL_MESSAGE(h->ld_a_ir, cpu->ld_a_ir, "ld_a_ir");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(h->total_cycles, cpu->total_cycles, "total_cycles");
}

/** @brief Výchozí skrytý stav (jako po z80_reset), aby test nic nenechal. */
static const st_HIDDEN s_defaults = { 0, 0, false, false, false, 0, false, 0 };

void setUp(void) { }
void tearDown(void)
{
    hidden_set(&s_defaults);
    g_emulator.paused = false;
}

/* Uložený skrytý stav se po načtení obnoví, i když stroj mezitím
 * nastavil jiné hodnoty (dřív zůstaly hodnoty stroje). Dvě sady hodnot
 * pokrývají obě hodnoty bool polí. */
static void roundtrip(const st_HIDDEN *saved, const st_HIDDEN *dirty)
{
    g_emulator.paused = true;
    hidden_set(saved);

    uint8_t *buf = NULL;
    size_t size = 0;
    en_SNAPSHOT_RESULT res = snapshot_save_to_buffer("z80 hidden", &buf, &size);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SNAPSHOT_OK, res, snapshot_result_to_string(res));

    hidden_set(dirty);
    res = snapshot_load_from_buffer(buf, size);
    TEST_ASSERT_EQUAL_INT_MESSAGE(SNAPSHOT_OK, res, snapshot_result_to_string(res));

    hidden_assert(saved);
    g_free(buf);
}

/* Nenulové hodnoty přes stroj s výchozím skrytým stavem. */
void test_z80_hidden_roundtrip_set(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    const st_HIDDEN saved = { 0x1234, 0xA9, true, true, true, 0x5A, true, 0xFEDCBA98u };
    roundtrip(&saved, &s_defaults);
}

/* Výchozí hodnoty přes stroj s nenulovým skrytým stavem. */
void test_z80_hidden_roundtrip_clear(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    const st_HIDDEN dirty = { 0xBEEF, 0x28, true, true, true, 0xFF, true, 123456u };
    roundtrip(&s_defaults, &dirty);
}

/* === MAIN === */

int main(int argc, char *argv[])
{
    mztest_parse_args(argc, argv);
    mztest_init();

    UNITY_BEGIN();

    RUN_TEST(test_z80_hidden_roundtrip_set);
    RUN_TEST(test_z80_hidden_roundtrip_clear);

    int result = UNITY_END();
    mztest_teardown();
    return result;
}
