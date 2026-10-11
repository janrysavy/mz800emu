/* SPDX-License-Identifier: GPL-3.0-or-later
 * Synthetic CPU clock-boundary controls; no gameplay or hardware claim.
 */
#include "unity.h"
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "libs/cpu-z80/z80.h"

static uint8_t ram[65536];
static z80_t *cpu;
static int irq_on_fetch;
static unsigned port_count;

static uint8_t read_memory(z80_t *c, uint16_t address, int m1, void *data)
{
    (void)data;
    if (m1 && irq_on_fetch) {
        c->int_pending = true;
        irq_on_fetch = 0;
    }
    return ram[address];
}

static void write_memory(z80_t *c, uint16_t address, uint8_t value, void *data)
{
    (void)c; (void)data;
    ram[address] = value;
}

static void write_port(z80_t *c, uint16_t port, uint8_t value, void *data)
{
    (void)data;
    TEST_ASSERT_EQUAL_HEX16(0x22a5, port);
    TEST_ASSERT_EQUAL_HEX8(0x22, value);
    port_count++;
    z80_add_wait_states(c, 5);
}

void setUp(void)
{
    memset(ram, 0, sizeof(ram));
    irq_on_fetch = 0;
    port_count = 0;
    cpu = z80_create(read_memory, NULL, write_memory, NULL,
                     NULL, NULL, write_port, NULL, NULL, NULL);
    TEST_ASSERT_NOT_NULL(cpu);
}

void tearDown(void)
{
    z80_destroy(cpu);
    cpu = NULL;
}

static void boundary_cases(unsigned scenario)
{
    static const uint32_t clocks[] = {
        0, 0x7ffffffau, 0x7ffffffeu, 0x80000000u,
        0xfffffffau, 0xfffffffeu, 0xffffffffu
    };
    int expected = scenario == 1 ? 16 : scenario == 2 ? 17 : 4;
    for (unsigned n = 0; n < sizeof(clocks) / sizeof(clocks[0]); n++) {
        z80_reset(cpu);
        memset(ram, 0, sizeof(ram));
        port_count = 0;
        cpu->pc = 0x8000;
        cpu->sp = 0x9000;
        cpu->af.w = 0x2200;
        cpu->total_cycles = clocks[n];
        if (scenario == 1) {
            ram[0x8000] = 0xd3; /* OUT (a5h),A plus five callback WAITs */
            ram[0x8001] = 0xa5;
        } else if (scenario == 2) {
            cpu->iff1 = cpu->iff2 = 1;
            cpu->im = 1;
            irq_on_fetch = 1; /* NOP completes, then acknowledge IRQ */
        } else if (scenario == 3) {
            cpu->halted = true; /* refresh-only step */
        }
        TEST_ASSERT_EQUAL_INT(expected, z80_step(cpu));
        TEST_ASSERT_EQUAL_HEX32(clocks[n] + (uint32_t)expected, cpu->total_cycles);
        TEST_ASSERT_EQUAL_UINT32(expected, cpu->cycles);
        TEST_ASSERT_EQUAL_UINT(scenario == 1 ? 1 : 0, port_count);
        TEST_ASSERT_EQUAL_HEX16(scenario == 2 ? 0x0038 :
                               scenario == 3 ? 0x8000 :
                               scenario == 1 ? 0x8002 : 0x8001, cpu->pc);
        if (scenario == 2) {
            TEST_ASSERT_EQUAL_HEX16(0x8ffe, cpu->sp);
            TEST_ASSERT_EQUAL_HEX8(1, ram[0x8ffe]);
            TEST_ASSERT_EQUAL_HEX8(0x80, ram[0x8fff]);
        }
    }
}

static void test_nop_cycle_delta(void) { boundary_cases(0); }
static void test_waited_io_cycle_delta(void) { boundary_cases(1); }
static void test_post_instruction_irq_cycle_delta(void) { boundary_cases(2); }
static void test_halt_refresh_cycle_delta(void) { boundary_cases(3); }

/* With -ftrapv the old signed subtraction aborts. Suppress native crash UI
 * in this explicitly headless control and return a recognizable failure. */
static void arithmetic_abort(int signal_number)
{
    (void)signal_number;
    _Exit(77);
}

int main(void)
{
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    signal(SIGABRT, arithmetic_abort);
    UNITY_BEGIN();
    RUN_TEST(test_nop_cycle_delta);
    RUN_TEST(test_waited_io_cycle_delta);
    RUN_TEST(test_post_instruction_irq_cycle_delta);
    RUN_TEST(test_halt_refresh_cycle_delta);
    return UNITY_END();
}
