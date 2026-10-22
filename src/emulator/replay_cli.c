/**
 * @file   replay_cli.c
 * @brief  Volby příkazové řádky záznamu a přehrání vstupů (viz replay_cli.h).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#include "replay_cli.h"
#include "replay.h"
#include "emulator.h"
#include "libs/sdlapp/sdlapp_options.h"


/** @brief Ověřený interval hashů z `--replay-hash-interval` (0 = výchozí). */
static uint32_t s_cli_hash_interval = 0;


/**
 * @brief Hodnota volby, nebo NULL bez volby; prázdná hodnota = chyba.
 * @param[out] bad true = volba je zadaná bez hodnoty.
 */
static const char *replay_cli_opt(const char *name, bool *bad)
{
    if (!sdlapp_option_present(name)) return NULL;
    const char *v = sdlapp_option_value(name);
    if (!v || !*v)
    {
        fprintf(stderr, "Option %s needs a value\n", name);
        *bad = true;
        return NULL;
    };
    return v;
}


bool replay_cli_validate(void)
{
    bool bad = false;
    const char *rec = replay_cli_opt("--replay-record", &bad);
    const char *play = replay_cli_opt("--replay", &bad);
    const char *interval = replay_cli_opt("--replay-hash-interval", &bad);
    bool exit_at_end = sdlapp_option_present("--exit-at-replay-end");
    bool verify = sdlapp_option_present("--replay-verify");

    if (rec && play)
    {
        fprintf(stderr, "Use only one of --replay-record and --replay\n");
        bad = true;
    };
    if (interval)
    {
        char *end = NULL;
        guint64 n = g_ascii_strtoull(interval, &end, 10);
        if (!end || *end != '\0' || n < 1 || n > 100000)
        {
            fprintf(stderr, "Invalid value for --replay-hash-interval: '%s' (expected 1..100000)\n", interval);
            bad = true;
        };
        s_cli_hash_interval = (uint32_t)n;
        if (!rec)
        {
            fprintf(stderr, "--replay-hash-interval needs --replay-record\n");
            bad = true;
        };
    };
    if ((exit_at_end || verify) && !play)
    {
        fprintf(stderr, "%s needs --replay\n", verify ? "--replay-verify" : "--exit-at-replay-end");
        bad = true;
    };
    if (play && !g_file_test(play, G_FILE_TEST_IS_REGULAR))
    {
        fprintf(stderr, "Replay file not found: '%s'\n", play);
        bad = true;
    };
    return !bad;
}


void replay_cli_start(void)
{
    const char *rec = sdlapp_option_present("--replay-record") ? sdlapp_option_value("--replay-record") : NULL;
    const char *play = sdlapp_option_present("--replay") ? sdlapp_option_value("--replay") : NULL;
    bool verify = sdlapp_option_present("--replay-verify");

    if (rec && *rec)
    {
        (void)replay_request_record(rec, s_cli_hash_interval, NULL);
    }
    else if (play && *play)
    {
        replay_set_exit_at_end(verify || sdlapp_option_present("--exit-at-replay-end"));
        if (verify) emulator_max_speed(true);
        (void)replay_request_play(play, false);
    };
}
