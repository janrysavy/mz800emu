/* Independent physical and automation joystick sources. GPLv3. */
#include "mztest.h"
#include "hw-generic/joy/joy.h"

void setUp(void) { }
void tearDown(void) { }

void test_joy_sources_merge_without_releasing_each_other(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_JOY_DEV saved = g_joy.dev[0];
    g_joy.dev[0].type = JOY_TYPE_NUM_KEYPAD;
    for (unsigned physical = 0; physical < 256; ++physical) {
        for (unsigned automation = 0; automation < 256; ++automation) {
            g_joy.dev[0].state = (uint8_t)physical;
            g_joy.dev[0].virtual_state = (uint8_t)automation;
            TEST_ASSERT_EQUAL_HEX8(physical & automation, joy_read_byte(JOY_DEVID_0));
        }
    }
    /* Physical reset/poll may release physical keys only. */
    g_joy.dev[0].virtual_state = 0xef;
    joy_reset_dev_state(JOY_DEVID_0);
    TEST_ASSERT_EQUAL_HEX8(0xef, joy_read_byte(JOY_DEVID_0));
    /* Releasing automation cannot release a physical left switch. */
    g_joy.dev[0].state = 0xfb;
    g_joy.dev[0].virtual_state = 0xff;
    TEST_ASSERT_EQUAL_HEX8(0xfb, joy_read_byte(JOY_DEVID_0));
    g_joy.dev[0] = saved;
}

void test_joy_automation_needs_no_host_device(void)
{
    MZTEST_REQUIRE_LEVEL(MZTEST_LEVEL_UNIT);
    st_JOY_DEV saved = g_joy.dev[0];
    g_joy.dev[0].type = JOY_TYPE_NONE;
    g_joy.dev[0].state = 0;
    for (unsigned automation = 0; automation < 256; ++automation) {
        g_joy.dev[0].virtual_state = (uint8_t)automation;
        TEST_ASSERT_EQUAL_HEX8(automation, joy_read_byte(JOY_DEVID_0));
    }
    g_joy.dev[0] = saved;
}

int main(int argc, char **argv)
{
    mztest_parse_args(argc, argv);
    mztest_init();
    UNITY_BEGIN();
    RUN_TEST(test_joy_sources_merge_without_releasing_each_other);
    RUN_TEST(test_joy_automation_needs_no_host_device);
    int result = UNITY_END();
    mztest_teardown();
    return result;
}
