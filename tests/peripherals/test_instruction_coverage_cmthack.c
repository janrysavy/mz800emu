/* SPDX-License-Identifier: GPL-3.0-or-later
 * Synthetic component controls, never native gameplay evidence.
 * The normal OUT handler and production CMT services run in the headless
 * emulator core. Only fixture setup and observer inspection are synthetic.
 */
#include "mztest.h"
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "emulator/emulator.h"
#include "debugger/dbgapi_emu.h"
#include "hw-generic/cmt/cmthack.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/memory/memext.h"
#include "mzarch/mzarch.h"

static char *fixture_path;
static const uint8_t body[] = {0xd3, 0x02, 0x00, 0xc9};
static z80_pwrite_cb original_pwrite;
static void *original_pwrite_data;
static unsigned port_count;
static uint16_t observed_port;
static uint8_t observed_value;
static int observed_tstate;

typedef struct {
    uint8_t cpu[offsetof(z80_t, mread_cb)];
    uint8_t ram[MEMORY_SIZE_RAM];
    uint32_t clock;
    uint64_t gdg_ticks_delta, pc_units;
    uint32_t pc_unique;
    uint8_t pc_bitmap[8192];
    unsigned port_count;
    uint16_t port;
    uint8_t value;
    int tstate, cycles;
} observation;

static void observed_pwrite(z80_t *cpu, uint16_t port, uint8_t value, void *unused)
{
    (void)unused;
    port_count++;
    observed_port = port;
    observed_value = value;
    observed_tstate = cpu->op_tstate;
    original_pwrite(cpu, port, value, original_pwrite_data);
}

void setUp(void)
{
    if (!fixture_path) {
        char *name = g_strdup_printf("mztest_instruction_cmt_%u.mzf", (unsigned)g_random_int());
        fixture_path = g_build_filename(g_get_tmp_dir(), name, NULL);
        g_free(name);
    }
    uint8_t data[128 + sizeof(body)] = {0};
    data[0] = 1;
    memcpy(data + 1, "COVERAGE TEST\r", 14);
    data[0x12] = sizeof(body);
    data[0x15] = data[0x17] = 0x40; /* start and entry 4000h, plain RAM */
    memcpy(data + 128, body, sizeof(body));
    TEST_ASSERT_TRUE(g_file_set_contents(fixture_path, (const char *)data, sizeof(data), NULL));
    z80_t *cpu = g_mzarch_main.cpu;
    if (!cpu->instruction_coverage) cpu->instruction_coverage = zic_create(256);
    TEST_ASSERT_NOT_NULL(cpu->instruction_coverage);
    zic_reset(cpu->instruction_coverage);
    cpu->instruction_coverage->enabled = 1;
    g_emulator.paused = true;
    /* Test the MZ-800 with internal RAM; the default config may connect
     * expanded RAM and select the MZ-700 compatibility boot mode. */
    memext_disconnect();
    g_mzarch_main.mode_sw = MZ800_MODE_SW_MZ800;
}

void tearDown(void)
{
    cmthack_reset();
    if (fixture_path) g_remove(fixture_path);
    zic_reset(g_mzarch_main.cpu->instruction_coverage);
    g_mzarch_main.cpu->instruction_coverage->enabled = 0;
}

static JsonParser *detail_command(int action)
{
    st_DBGAPI_INSTRUCTION_COVERAGE_PARAM parameter = {0};
    parameter.action = action;
    parameter.limit = 256;
    st_DBGAPI_CMDRQ request = {0};
    request.cmd = DBGAPI_CMD_INSTRUCTION_COVERAGE;
    request.data_ptr = &parameter;
    dbgapi_emu_dispatch(&request);
    TEST_ASSERT_TRUE(request.success);
    TEST_ASSERT_NOT_NULL(parameter.json);
    JsonParser *parser = json_parser_new();
    TEST_ASSERT_TRUE(json_parser_load_from_data(parser, parameter.json, -1, NULL));
    g_free(parameter.json);
    return parser;
}

/* ready: 0 absent body, 1 successful body, 2 body read beyond available data. */
static void run_out(unsigned port, int ready, int enabled, observation *out)
{
    z80_t *cpu = g_mzarch_main.cpu;
    zic_state *s = cpu->instruction_coverage;
    zic_reset(s);
    s->enabled = enabled;
    memset(g_memory.RAM, 0, sizeof(g_memory.RAM));
    mzarch_main_reset_and_run_mzf(fixture_path);
    /* Bootstrap uses the real CMT helpers without a pending CPU unit. */
    TEST_ASSERT_EQUAL_UINT64(0, s->units);
    TEST_ASSERT_EQUAL_UINT64(0, s->semantic_override_units);
    TEST_ASSERT_FALSE(s->pending);
    if (ready) {
        cpu->hl.w = 0x5000;
        cmthack_load_mzf_filename(fixture_path);
        TEST_ASSERT_TRUE(g_cmthack.mzf_handler.status & HANDLER_STATUS_READY);
        TEST_ASSERT_EQUAL_UINT64(0, s->semantic_override_units);
    }
    uint8_t code[] = {0xd3, (uint8_t)port};
    memory_load_block(code, 0x4000, sizeof(code), MEMORY_LOAD_RAMONLY);
    cpu->pc = 0x4000;
    cpu->sp = 0x7f00;
    cpu->hl.w = 0x6000;
    cpu->bc.w = ready == 2 ? 0x100 : sizeof(body);
    cpu->af.w = 0x1200;
    cpu->iff1 = cpu->iff2 = 0;
    cpu->int_pending = cpu->nmi_pending = false;
    cpu->ei_delay = false;
    cpu->op_tstate = cpu->wait_cycles = 0;
    cpu->coverage_enabled = true;
    cpu->coverage_units = cpu->coverage_unique = 0;
    memset(cpu->coverage_bitmap, 0, sizeof(cpu->coverage_bitmap));
    g_mzarch_main.instruction_insideop_sync_ticks = 0;
    g_mzarch_main.instruction_wait_tstates = 0;
    original_pwrite = cpu->pwrite_cb;
    original_pwrite_data = cpu->pwrite_data;
    z80_set_pwrite(cpu, observed_pwrite, NULL);
    port_count = 0;
    /* GDG's lifetime clock intentionally survives the CPU reset above. */
    uint64_t gdg_before = gdg_get_total_ticks();
    out->cycles = z80_step(cpu);
    z80_set_pwrite(cpu, original_pwrite, original_pwrite_data);
    memcpy(out->cpu, cpu, sizeof(out->cpu));
    memcpy(out->ram, g_memory.RAM, sizeof(out->ram));
    out->clock = cpu->total_cycles;
    out->gdg_ticks_delta = gdg_get_total_ticks() - gdg_before;
    out->pc_units = cpu->coverage_units;
    out->pc_unique = cpu->coverage_unique;
    memcpy(out->pc_bitmap, cpu->coverage_bitmap, sizeof(out->pc_bitmap));
    out->port_count = port_count;
    out->port = observed_port;
    out->value = observed_value;
    out->tstate = observed_tstate;
    TEST_ASSERT_EQUAL_INT(11, out->cycles);
    TEST_ASSERT_EQUAL_HEX16(0x4002, cpu->pc);
    TEST_ASSERT_EQUAL_UINT(1, port_count);
    TEST_ASSERT_EQUAL_HEX16(0x1200 | port, observed_port);
    TEST_ASSERT_EQUAL_HEX8(0x12, observed_value);
    TEST_ASSERT_EQUAL_UINT64(1, cpu->coverage_units);
    TEST_ASSERT_EQUAL_UINT(1, cpu->coverage_unique);
    TEST_ASSERT_EQUAL_HEX16(port == 1 || !ready ? 0x0201 : ready == 2 ? 0x0101 : 0x1200, cpu->af.w);
    if (ready == 1) TEST_ASSERT_EQUAL_MEMORY(body, g_memory.RAM + 0x6000, sizeof(body));
    if (!enabled) {
        TEST_ASSERT_EQUAL_UINT64(0, s->units);
        return;
    }
    uint8_t kind = port == 1 ? ZIC_SEMANTIC_CMTHACK_FILE : ZIC_SEMANTIC_CMTHACK_BODY;
    TEST_ASSERT_TRUE(zic_complete(s));
    TEST_ASSERT_EQUAL_UINT64(1, s->units);
    TEST_ASSERT_EQUAL_UINT64(1, s->semantic_override_units);
    TEST_ASSERT_EQUAL_UINT64(1, s->semantic_kind_units[port == 1 ? 4 : 5]);
    TEST_ASSERT_EQUAL_UINT(1, s->unique);
    TEST_ASSERT_EQUAL_HEX8(ZIC_SEMANTIC_OVERRIDE, s->rows[0].key.flags);
    TEST_ASSERT_EQUAL_HEX8(kind, s->rows[0].key.semantic_kinds);
    TEST_ASSERT_EQUAL_MEMORY(code, s->rows[0].key.bytes, sizeof(code));
    TEST_ASSERT_EQUAL_INT(-1, s->rows[0].key.outcome);
    JsonParser *parser = detail_command(4);
    JsonObject *json = json_node_get_object(json_parser_get_root(parser));
    TEST_ASSERT_EQUAL_INT(2, json_object_get_int_member(json, "schema"));
    TEST_ASSERT_EQUAL_STRING("z80-dispatch-fetched-v2", json_object_get_string_member(json, "format"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(json, "complete"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(json, "contains_semantic_overrides"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(json, "native_semantics_only"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(json, "counter_overflow"));
    TEST_ASSERT_EQUAL_INT64(INT64_MAX, json_object_get_int_member(json, "counter_limit"));
    TEST_ASSERT_EQUAL_INT(0, json_object_get_int_member(json, "opcode_override_failures"));
    TEST_ASSERT_EQUAL_INT(1, json_object_get_int_member(json, "semantic_override_units"));
    static const char *kind_names[] = {
        "fuse_tape_save", "fuse_tape_load", "fuse_slt",
        "fuse_loader_acceleration", "mz_cmthack_file", "mz_cmthack_body"
    };
    JsonObject *kinds = json_object_get_object_member(json, "semantic_kind_units");
    for (unsigned i = 0; i < 6; i++) {
        TEST_ASSERT_EQUAL_INT((1u << i) == kind,
                             json_object_get_int_member(kinds, kind_names[i]));
    }
    JsonObject *row = json_array_get_object_element(json_object_get_array_member(json, "entries"), 0);
    TEST_ASSERT_EQUAL_INT(kind, json_object_get_int_member(row, "semantic_kinds"));
    g_object_unref(parser);
}

void test_actual_cmt_out_paths_preserve_guest_observations(void)
{
    static observation off, on;
    for (int scenario = 0; scenario < 4; scenario++) {
        unsigned port = scenario == 0 ? 1 : 2;
        int ready = scenario < 2 ? 0 : scenario - 1;
        memset(&off, 0, sizeof(off));
        memset(&on, 0, sizeof(on));
        run_out(port, ready, 0, &off);
        run_out(port, ready, 1, &on);
        TEST_ASSERT_EQUAL_MEMORY(&off, &on, sizeof(off));
    }
}

void test_direct_file_service_is_tagged_once_only_inside_pending_unit(void)
{
    z80_t *cpu = g_mzarch_main.cpu;
    zic_state *s = cpu->instruction_coverage;
    cpu->hl.w = 0x5000;
    cmthack_load_mzf_filename(fixture_path);
    TEST_ASSERT_EQUAL_UINT64(0, s->units);
    TEST_ASSERT_EQUAL_UINT64(0, s->semantic_override_units);
    /* Synthetic collector envelope around the actual file helper, separate
     * from the production OUT controls above. Repeated entry is idempotent. */
    zic_begin(s, 0x4000);
    zic_fetch(s, 0x4000, 0xd3, 1);
    zic_fetch(s, 0x4001, 1, 0);
    cmthack_load_mzf_filename(fixture_path);
    cmthack_load_mzf_filename(fixture_path);
    zic_finish(s, 0x4002);
    TEST_ASSERT_TRUE(zic_complete(s));
    TEST_ASSERT_EQUAL_UINT64(1, s->semantic_override_units);
    TEST_ASSERT_EQUAL_UINT64(1, s->semantic_kind_units[4]);
}

void test_v2_api_lifecycle_clock_wrap_and_loss_status(void)
{
    z80_t *cpu = g_mzarch_main.cpu;
    zic_state *s = cpu->instruction_coverage;
    JsonParser *parser;
    JsonObject *json;

    /* Never-started status remains safe and does not certify completeness. */
    cpu->instruction_coverage = NULL;
    parser = detail_command(3);
    json = json_node_get_object(json_parser_get_root(parser));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(json, "complete"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(json, "native_semantics_only"));
    TEST_ASSERT_EQUAL_INT(0, json_object_get_int_member(json, "semantic_override_units"));
    g_object_unref(parser);
    cpu->instruction_coverage = s;

    mzarch_main_reset_and_run_mzf(fixture_path);
    uint8_t nop = 0;
    memory_load_block(&nop, 0x4000, 1, MEMORY_LOAD_RAMONLY);
    cpu->pc = 0x4000;
    cpu->iff1 = cpu->iff2 = 0;
    cpu->int_pending = cpu->nmi_pending = cpu->ei_delay = false;
    cpu->total_cycles = UINT32_MAX - 1;
    TEST_ASSERT_EQUAL_INT(4, z80_step(cpu));
    TEST_ASSERT_EQUAL_UINT32(2, cpu->total_cycles);
    parser = detail_command(4);
    json = json_node_get_object(json_parser_get_root(parser));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(json, "native_semantics_only"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(json, "contains_semantic_overrides"));
    TEST_ASSERT_EQUAL_INT(32, json_object_get_int_member(json, "clock_bits"));
    TEST_ASSERT_EQUAL_INT(2, json_object_get_int_member(json, "clock_cycles"));
    TEST_ASSERT_EQUAL_INT(1, json_object_get_int_member(json, "dispatch_units"));
    g_object_unref(parser);

    /* Stop retains evidence; CPU reset affects the clock, not the table. */
    parser = detail_command(1);
    json = json_node_get_object(json_parser_get_root(parser));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(json, "enabled"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(json, "native_semantics_only"));
    g_object_unref(parser);
    z80_reset(cpu);
    parser = detail_command(4);
    json = json_node_get_object(json_parser_get_root(parser));
    TEST_ASSERT_EQUAL_INT(0, json_object_get_int_member(json, "clock_cycles"));
    TEST_ASSERT_EQUAL_INT(1, json_object_get_int_member(json, "dispatch_units"));
    g_object_unref(parser);

    /* Coverage reset is independent of the guest clock and enable state. */
    cpu->total_cycles = UINT32_MAX;
    parser = detail_command(2);
    json = json_node_get_object(json_parser_get_root(parser));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(json, "enabled"));
    TEST_ASSERT_EQUAL_INT(0, json_object_get_int_member(json, "dispatch_units"));
    TEST_ASSERT_EQUAL_INT64(UINT32_MAX, json_object_get_int_member(json, "clock_cycles"));
    g_object_unref(parser);
    parser = detail_command(0);
    g_object_unref(parser);

    /* Explicit unknown substitution loss must not be mislabeled as a read
     * failure, or certify native-only capture simply because no kind is set. */
    zic_begin(s, 0x4000);
    zic_fetch(s, 0x4000, 0, 1);
    zic_flag(s, ZIC_OVERRIDE);
    zic_finish(s, 0x4001);
    parser = detail_command(3);
    json = json_node_get_object(json_parser_get_root(parser));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(json, "complete"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(json, "native_semantics_only"));
    TEST_ASSERT_EQUAL_INT(1, json_object_get_int_member(json, "opcode_override_failures"));
    TEST_ASSERT_EQUAL_INT(0, json_object_get_int_member(json, "fetch_failures"));
    g_object_unref(parser);

    /* Seed only host observer state to test exact JSON limits cheaply. */
    zic_reset(s);
    s->units = ZIC_COUNTER_LIMIT;
    zic_begin(s, 0x4000);
    zic_fetch(s, 0x4000, 0, 1);
    zic_finish(s, 0x4001);
    parser = detail_command(3);
    json = json_node_get_object(json_parser_get_root(parser));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(json, "counter_overflow"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(json, "complete"));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(json, "native_semantics_only"));
    TEST_ASSERT_EQUAL_INT64(INT64_MAX, json_object_get_int_member(json, "dispatch_units"));
    g_object_unref(parser);
    parser = detail_command(2);
    json = json_node_get_object(json_parser_get_root(parser));
    TEST_ASSERT_FALSE(json_object_get_boolean_member(json, "counter_overflow"));
    TEST_ASSERT_TRUE(json_object_get_boolean_member(json, "native_semantics_only"));
    g_object_unref(parser);
}

int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();
    UNITY_BEGIN();
    RUN_TEST(test_actual_cmt_out_paths_preserve_guest_observations);
    RUN_TEST(test_direct_file_service_is_tagged_once_only_inside_pending_unit);
    RUN_TEST(test_v2_api_lifecycle_clock_wrap_and_loss_status);
    int result = UNITY_END();
    g_free(fixture_path);
    mztest_teardown();
    return result;
}
