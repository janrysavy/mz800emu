/**
 * @file   test_sdlapp_options.c
 * @author Michal Hucik <hucik@ordoz.com>
 * @brief  Unit testy validace sdlapp_options framework.
 *
 * Pokrývá:
 *  - whitelist (neznámá option = chyba)
 *  - VALUE bez následující hodnoty = chyba
 *  - ENUM value type (povolené hodnoty + odmítnutí ostatních)
 *  - BOOL_ON_OFF (přesně "on"/"off", odmítnutí "yes"/"true"/"1")
 *  - UINT (jen číslice, odmítnutí "-1" / "abc" / prázdné)
 *  - STRING (libovolná hodnota projde)
 *  - FLAG nemá value (negativní + pozitivní zápis)
 *
 * Standalone test - linkuje s Unity + sdlapp_options.o.
 */

#include "unity.h"

#include "libs/sdlapp/sdlapp_options.h"

/* ========================================================================
 * Sdílené allowed-values seznamy pro testovací schémata
 * ======================================================================== */

static const char *const MODE_VALUES[] = { "off", "window", "always", NULL };

/** @brief Testovací schéma pokrývající všechny value_type varianty. */
static const st_SDLAPP_OPTION_DEF g_test_options[] = {
    { "--help",       SDLAPP_OPTION_FLAG,  SDLAPP_OPTVAL_NONE,        NULL,        NULL,
      "Help" },
    { "--no-save",    SDLAPP_OPTION_FLAG,  SDLAPP_OPTVAL_NONE,        NULL,        NULL,
      "No save flag" },
    { "--mode",       SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_ENUM,        MODE_VALUES, "<off|window|always>",
      "Mode enum" },
    { "--save-on",    SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_BOOL_ON_OFF, NULL,        "<on|off>",
      "On/off flag value" },
    { "--chunk-mb",   SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_UINT,        NULL,        "<N>",
      "Unsigned integer" },
    { "--dir",        SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<dirpath>",
      "Free-form string" },
    /* Testovací paralely k --all-traces-* shorthand options v src/main.c.
     * Validují, že parser akceptuje stejnou ENUM + STRING strukturu. */
    { "--all-traces-mode", SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_ENUM,        MODE_VALUES, "<off|window|always>",
      "Trace-suite shorthand mode" },
    { "--all-traces-dir",  SDLAPP_OPTION_VALUE, SDLAPP_OPTVAL_STRING,      NULL,        "<dirpath>",
      "Trace-suite shorthand dir" },
    { NULL, SDLAPP_OPTION_FLAG, SDLAPP_OPTVAL_NONE, NULL, NULL, NULL }
};


/* ========================================================================
 * setUp / tearDown
 * ======================================================================== */

void setUp(void)
{
    /* Reset interního stavu před každým testem - re-inicializace s prázdným
     * argv. Konkrétní test si pak nahradí přes sdlapp_options_init(). */
    sdlapp_options_init(0, NULL);
}

void tearDown(void)
{
    sdlapp_options_init(0, NULL);
}


/* ========================================================================
 * Pomocné helpery
 * ======================================================================== */

/**
 * @brief Spustit validate pro konkrétní argv, vrátit výsledek.
 *
 * @param argc Počet argumentů (včetně argv[0]).
 * @param argv Pole argumentů. Není modifikováno.
 * @return Výsledek sdlapp_options_validate().
 */
static bool run_validate(int argc, char **argv)
{
    sdlapp_options_init(argc, argv);
    return sdlapp_options_validate(g_test_options);
}


/* ========================================================================
 * Testy: whitelist
 * ======================================================================== */

void test_no_args_only_argv0_passes(void)
{
    char *argv[] = { (char *)"prog" };
    TEST_ASSERT_TRUE(run_validate(1, argv));
}

void test_known_flag_passes(void)
{
    char *argv[] = { (char *)"prog", (char *)"--help" };
    TEST_ASSERT_TRUE(run_validate(2, argv));
}

void test_unknown_option_fails(void)
{
    char *argv[] = { (char *)"prog", (char *)"--foobar" };
    TEST_ASSERT_FALSE(run_validate(2, argv));
}

void test_unknown_option_after_known_fails(void)
{
    char *argv[] = { (char *)"prog", (char *)"--no-save", (char *)"--xyz" };
    TEST_ASSERT_FALSE(run_validate(3, argv));
}


/* ========================================================================
 * Testy: VALUE bez hodnoty
 * ======================================================================== */

void test_value_option_without_value_fails(void)
{
    char *argv[] = { (char *)"prog", (char *)"--mode" };
    TEST_ASSERT_FALSE(run_validate(2, argv));
}

void test_value_option_followed_by_argv0_end_fails(void)
{
    /* --dir je VALUE, za ním už nic - missing value */
    char *argv[] = { (char *)"prog", (char *)"--no-save", (char *)"--dir" };
    TEST_ASSERT_FALSE(run_validate(3, argv));
}


/* ========================================================================
 * Testy: ENUM (value_type=ENUM)
 * ======================================================================== */

void test_enum_off_passes(void)
{
    char *argv[] = { (char *)"prog", (char *)"--mode", (char *)"off" };
    TEST_ASSERT_TRUE(run_validate(3, argv));
}

void test_enum_window_passes(void)
{
    char *argv[] = { (char *)"prog", (char *)"--mode", (char *)"window" };
    TEST_ASSERT_TRUE(run_validate(3, argv));
}

void test_enum_always_passes(void)
{
    char *argv[] = { (char *)"prog", (char *)"--mode", (char *)"always" };
    TEST_ASSERT_TRUE(run_validate(3, argv));
}

void test_enum_invalid_value_fails(void)
{
    /* "banana" není v MODE_VALUES - musí selhat */
    char *argv[] = { (char *)"prog", (char *)"--mode", (char *)"banana" };
    TEST_ASSERT_FALSE(run_validate(3, argv));
}

void test_enum_case_sensitive_fails(void)
{
    /* "Always" (capitalized) není v MODE_VALUES - case sensitive */
    char *argv[] = { (char *)"prog", (char *)"--mode", (char *)"Always" };
    TEST_ASSERT_FALSE(run_validate(3, argv));
}


/* ========================================================================
 * Testy: BOOL_ON_OFF (Michalův primární use-case "yes" místo "on")
 * ======================================================================== */

void test_bool_on_passes(void)
{
    char *argv[] = { (char *)"prog", (char *)"--save-on", (char *)"on" };
    TEST_ASSERT_TRUE(run_validate(3, argv));
}

void test_bool_off_passes(void)
{
    char *argv[] = { (char *)"prog", (char *)"--save-on", (char *)"off" };
    TEST_ASSERT_TRUE(run_validate(3, argv));
}

void test_bool_yes_fails(void)
{
    /* Klíčový bug: "yes" by tiše prošlo. Musí selhat. */
    char *argv[] = { (char *)"prog", (char *)"--save-on", (char *)"yes" };
    TEST_ASSERT_FALSE(run_validate(3, argv));
}

void test_bool_no_fails(void)
{
    char *argv[] = { (char *)"prog", (char *)"--save-on", (char *)"no" };
    TEST_ASSERT_FALSE(run_validate(3, argv));
}

void test_bool_true_fails(void)
{
    char *argv[] = { (char *)"prog", (char *)"--save-on", (char *)"true" };
    TEST_ASSERT_FALSE(run_validate(3, argv));
}

void test_bool_one_fails(void)
{
    char *argv[] = { (char *)"prog", (char *)"--save-on", (char *)"1" };
    TEST_ASSERT_FALSE(run_validate(3, argv));
}

void test_bool_capitalized_fails(void)
{
    /* "On" capitalized - case sensitive */
    char *argv[] = { (char *)"prog", (char *)"--save-on", (char *)"On" };
    TEST_ASSERT_FALSE(run_validate(3, argv));
}


/* ========================================================================
 * Testy: UINT (chunk-mb, max-total-mb)
 * ======================================================================== */

void test_uint_zero_passes(void)
{
    char *argv[] = { (char *)"prog", (char *)"--chunk-mb", (char *)"0" };
    TEST_ASSERT_TRUE(run_validate(3, argv));
}

void test_uint_positive_passes(void)
{
    char *argv[] = { (char *)"prog", (char *)"--chunk-mb", (char *)"64" };
    TEST_ASSERT_TRUE(run_validate(3, argv));
}

void test_uint_large_passes(void)
{
    char *argv[] = { (char *)"prog", (char *)"--chunk-mb", (char *)"4294967295" };
    TEST_ASSERT_TRUE(run_validate(3, argv));
}

void test_uint_negative_fails(void)
{
    /* "-1" není čistě číslice - musí selhat (jen 0-9 znaky). */
    char *argv[] = { (char *)"prog", (char *)"--chunk-mb", (char *)"-1" };
    TEST_ASSERT_FALSE(run_validate(3, argv));
}

void test_uint_alpha_fails(void)
{
    char *argv[] = { (char *)"prog", (char *)"--chunk-mb", (char *)"abc" };
    TEST_ASSERT_FALSE(run_validate(3, argv));
}

void test_uint_mixed_fails(void)
{
    /* "12abc" - začíná číslicí, ale obsahuje písmena */
    char *argv[] = { (char *)"prog", (char *)"--chunk-mb", (char *)"12abc" };
    TEST_ASSERT_FALSE(run_validate(3, argv));
}

void test_uint_empty_fails(void)
{
    /* prázdný string - vyloučení edge case */
    char *argv[] = { (char *)"prog", (char *)"--chunk-mb", (char *)"" };
    TEST_ASSERT_FALSE(run_validate(3, argv));
}

void test_uint_with_plus_sign_fails(void)
{
    /* "+10" - znaménko není akceptované */
    char *argv[] = { (char *)"prog", (char *)"--chunk-mb", (char *)"+10" };
    TEST_ASSERT_FALSE(run_validate(3, argv));
}


/* ========================================================================
 * Testy: STRING (libovolná hodnota projde)
 * ======================================================================== */

void test_string_anything_passes(void)
{
    char *argv[] = { (char *)"prog", (char *)"--dir", (char *)"./logs/run-1/" };
    TEST_ASSERT_TRUE(run_validate(3, argv));
}

void test_string_with_special_chars_passes(void)
{
    char *argv[] = { (char *)"prog", (char *)"--dir", (char *)"C:\\Users\\Test Path\\With Spaces" };
    TEST_ASSERT_TRUE(run_validate(3, argv));
}

void test_string_value_starting_with_dash_passes(void)
{
    /* Hodnota začínající '-' je legitimní (např. v cestě). Validate ji
     * nesmí odmítnout - explicitně skipne `i++` v parseru. */
    char *argv[] = { (char *)"prog", (char *)"--dir", (char *)"-some-weird-name" };
    TEST_ASSERT_TRUE(run_validate(3, argv));
}


/* ========================================================================
 * Testy: kombinace (více options za sebou)
 * ======================================================================== */

void test_combined_valid_options_pass(void)
{
    char *argv[] = {
        (char *)"prog",
        (char *)"--mode",     (char *)"always",
        (char *)"--save-on",  (char *)"on",
        (char *)"--chunk-mb", (char *)"32",
        (char *)"--dir",      (char *)"./logs/",
        (char *)"--no-save",
    };
    TEST_ASSERT_TRUE(run_validate(10, argv));
}

void test_combined_with_one_invalid_fails(void)
{
    /* První 3 jsou OK, "yes" v 4. má selhat */
    char *argv[] = {
        (char *)"prog",
        (char *)"--mode",     (char *)"window",
        (char *)"--chunk-mb", (char *)"64",
        (char *)"--save-on",  (char *)"yes",   /* BUG case */
    };
    TEST_ASSERT_FALSE(run_validate(7, argv));
}


/* ========================================================================
 * --all-traces-mode / --all-traces-dir shorthand
 * ======================================================================== */

void test_all_traces_mode_always_passes(void)
{
    char *argv[] = { (char *)"prog", (char *)"--all-traces-mode", (char *)"always" };
    TEST_ASSERT_TRUE(run_validate(3, argv));
}

void test_all_traces_mode_window_passes(void)
{
    char *argv[] = { (char *)"prog", (char *)"--all-traces-mode", (char *)"window" };
    TEST_ASSERT_TRUE(run_validate(3, argv));
}

void test_all_traces_mode_off_passes(void)
{
    char *argv[] = { (char *)"prog", (char *)"--all-traces-mode", (char *)"off" };
    TEST_ASSERT_TRUE(run_validate(3, argv));
}

void test_all_traces_mode_invalid_fails(void)
{
    char *argv[] = { (char *)"prog", (char *)"--all-traces-mode", (char *)"banana" };
    TEST_ASSERT_FALSE(run_validate(3, argv));
}

void test_all_traces_dir_passes(void)
{
    char *argv[] = { (char *)"prog", (char *)"--all-traces-dir", (char *)"./logs/run-1/" };
    TEST_ASSERT_TRUE(run_validate(3, argv));
}


/* ========================================================================
 * Testy: opakovaná VALUE option (sdlapp_option_count / _value_nth)
 * ======================================================================== */

void test_repeated_value_option_count_and_nth(void)
{
    /* Obě formy zápisu, mezi výskyty jiná volba; pouhý prefix jména
     * (--di) se za výskyt nepočítá. */
    char *argv[] = { (char *)"prog", (char *)"--dir", (char *)"a", (char *)"--no-save",
                     (char *)"--dir=b", (char *)"--dir", (char *)"c" };
    TEST_ASSERT_TRUE(run_validate(7, argv));
    TEST_ASSERT_EQUAL_INT(3, sdlapp_option_count("--dir"));
    TEST_ASSERT_EQUAL_STRING("a", sdlapp_option_value_nth("--dir", 0));
    TEST_ASSERT_EQUAL_STRING("b", sdlapp_option_value_nth("--dir", 1));
    TEST_ASSERT_EQUAL_STRING("c", sdlapp_option_value_nth("--dir", 2));
    TEST_ASSERT_NULL(sdlapp_option_value_nth("--dir", 3));
    TEST_ASSERT_NULL(sdlapp_option_value_nth("--dir", -1));
    /* sdlapp_option_value() vrací dál první výskyt. */
    TEST_ASSERT_EQUAL_STRING("a", sdlapp_option_value("--dir"));
    TEST_ASSERT_EQUAL_INT(0, sdlapp_option_count("--mode"));
    TEST_ASSERT_EQUAL_INT(0, sdlapp_option_count("--di"));
}

void test_all_traces_combined_with_per_subsystem_passes(void)
{
    /* Realistický CLI: shorthand + jeden per-subsystem override.
     * Parser musí akceptovat oba (validace neřeší semantic precedence,
     * jen syntax). */
    char *argv[] = {
        (char *)"prog",
        (char *)"--all-traces-mode", (char *)"always",
        (char *)"--all-traces-dir",  (char *)"./traces/",
        (char *)"--mode",            (char *)"off",   /* per-subsystem override */
    };
    TEST_ASSERT_TRUE(run_validate(7, argv));
}


/* ========================================================================
 * Test runner
 * ======================================================================== */

int main(void)
{
    UNITY_BEGIN();

    /* whitelist */
    RUN_TEST(test_no_args_only_argv0_passes);
    RUN_TEST(test_known_flag_passes);
    RUN_TEST(test_unknown_option_fails);
    RUN_TEST(test_unknown_option_after_known_fails);

    /* missing value */
    RUN_TEST(test_value_option_without_value_fails);
    RUN_TEST(test_value_option_followed_by_argv0_end_fails);

    /* enum */
    RUN_TEST(test_enum_off_passes);
    RUN_TEST(test_enum_window_passes);
    RUN_TEST(test_enum_always_passes);
    RUN_TEST(test_enum_invalid_value_fails);
    RUN_TEST(test_enum_case_sensitive_fails);

    /* bool on/off */
    RUN_TEST(test_bool_on_passes);
    RUN_TEST(test_bool_off_passes);
    RUN_TEST(test_bool_yes_fails);
    RUN_TEST(test_bool_no_fails);
    RUN_TEST(test_bool_true_fails);
    RUN_TEST(test_bool_one_fails);
    RUN_TEST(test_bool_capitalized_fails);

    /* uint */
    RUN_TEST(test_uint_zero_passes);
    RUN_TEST(test_uint_positive_passes);
    RUN_TEST(test_uint_large_passes);
    RUN_TEST(test_uint_negative_fails);
    RUN_TEST(test_uint_alpha_fails);
    RUN_TEST(test_uint_mixed_fails);
    RUN_TEST(test_uint_empty_fails);
    RUN_TEST(test_uint_with_plus_sign_fails);

    /* string */
    RUN_TEST(test_string_anything_passes);
    RUN_TEST(test_string_with_special_chars_passes);
    RUN_TEST(test_string_value_starting_with_dash_passes);

    /* combined */
    RUN_TEST(test_combined_valid_options_pass);
    RUN_TEST(test_combined_with_one_invalid_fails);

    /* --all-traces-* shorthand */
    RUN_TEST(test_all_traces_mode_always_passes);
    RUN_TEST(test_all_traces_mode_window_passes);
    RUN_TEST(test_all_traces_mode_off_passes);
    RUN_TEST(test_all_traces_mode_invalid_fails);
    RUN_TEST(test_all_traces_dir_passes);
    RUN_TEST(test_all_traces_combined_with_per_subsystem_passes);

    /* opakovaná volba */
    RUN_TEST(test_repeated_value_option_count_and_nth);

    return UNITY_END();
}
