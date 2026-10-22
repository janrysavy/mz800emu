/**
 * @file   media_writes.h
 * @brief  Počítadlo zápisů emulovaného stroje na média (diskety, Quick Disk,
 *         IDE8, nahrávání na kazetu) a vysunutí pásky.
 *
 * Checkpoint záznamu vstupů (replay.h) obsah médií nenese. Načíst ho jde
 * jen tehdy, když se od jeho pořízení na médium nic nezapsalo; to pozná
 * podle tohoto počítadla (hodnota při pořízení proti hodnotě teď).
 *
 * Počítá se v obsluze zařízení při zápisu dat na médium (sektor, stopa,
 * bajt Quick Disku, začátek nahrávání na kazetu) a při vysunutí pásky
 * (checkpoint ji zpět nevloží), ne v instrukční smyčce ani při čtení
 * a zápisu paměti. Opravy obrazu při připojení
 * (hlavička DSK) se nepočítají - nejsou zápisem programu.
 */

#ifndef MEDIA_WRITES_H
#define MEDIA_WRITES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Počet zápisů na média od startu procesu (přetéká, porovnávat na rovnost).
     *
     * Zvyšuje jen emu vlákno (obsluha zařízení), čte emu vlákno (replay).
     */
    extern uint32_t g_media_writes;

    /**
     * @brief Zaznamená zápis na médium.
     *
     * @note Volá obsluha zařízení v emu vlákně; bez vedlejších efektů mimo
     *       počítadlo.
     */
#define MEDIA_WRITES_NOTE() (g_media_writes++)

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_WRITES_H */
