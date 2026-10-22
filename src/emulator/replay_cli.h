/**
 * @file   replay_cli.h
 * @brief  Volby příkazové řádky záznamu a přehrání vstupů.
 *
 * | Volba | Význam |
 * |-------|--------|
 * | `--replay-record FILE` | při startu začít záznam do FILE, uložit při konci záznamu (konec procesu) |
 * | `--replay-hash-interval N` | interval hashů záznamu ve snímcích (1..100000, výchozí 50) |
 * | `--replay FILE` | při startu přehrát FILE |
 * | `--exit-at-replay-end` | konec přehrávání ukončí běh: kód 0 (`replay_end`), rozjetí kód 4 (`replay_desync`), chyba startu kód 3 |
 * | `--replay-verify` | `--exit-at-replay-end` + MAX SPEED (ověření záznamu, typicky s `--headless`) |
 *
 * Validace (replay_cli_validate()) běží v main() před startem emulátoru,
 * start (replay_cli_start()) v emulátorovém vlákně po automation_start().
 * Konec procesu vyřizuje automatizace (automation.h), proto
 * `--exit-at-replay-end` a `--replay-verify` patří mezi automatizační volby.
 */

#ifndef REPLAY_CLI_H
#define REPLAY_CLI_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Ověří kombinaci voleb a hodnoty (hlášky na stderr anglicky).
     *
     * Pravidla: `--replay-record` a `--replay` se vylučují;
     * `--replay-hash-interval` jen s `--replay-record`; `--exit-at-replay-end`
     * a `--replay-verify` jen s `--replay`; soubor `--replay` musí existovat.
     *
     * @return true = v pořádku (i bez voleb); false = chyba (main skončí).
     * @pre Hlavní vlákno, po sdlapp_options_init(), před startem emulace.
     */
    bool replay_cli_validate(void);

    /**
     * @brief Zahájí záznam nebo přehrání podle voleb (požadavek do fronty).
     *
     * Požadavek vyřídí emulátor v první hranici snímku (krátká pauza
     * s důvodem `replay`). `--replay-verify` zapne MAX SPEED.
     *
     * @pre Emulátorové vlákno, po resetu a automation_start(), před hlavní smyčkou.
     */
    void replay_cli_start(void);

#ifdef __cplusplus
}
#endif

#endif /* REPLAY_CLI_H */
