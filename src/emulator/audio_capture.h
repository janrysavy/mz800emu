/**
 * @file   audio_capture.h
 * @brief  Zachycení zvuku do složky pro hledání chyb zvuku (MCP audio_capture_*).
 *
 * Po startu zapisuje do zvolené složky v emulovaném čase:
 *
 * | Soubor           | Obsah |
 * |------------------|-------|
 * | `events.csv`     | každá změna výstupní úrovně zvukového kanálu (CTC0 = reproduktor, kanály PSG) |
 * | `psg_writes.csv` | každý zápis do PSG s dekódováním (kanál, registr, hodnota po zápisu) |
 * | `reference.wav`  | zvuk z rendereru videa (videorec_audio, přesné převzorkování v emulovaném čase) |
 * | `sdl.wav`        | zvuk tak, jak ho vyrobila skutečná SDL cesta (iface_audio_wait_for_data()) |
 * | `capture.json`   | metadata (platforma, takt, kanály, počty, důvod konce); zapisuje se při konci |
 *
 * Časy v CSV jsou v taktech GDG od začátku zachycení (`ticks`) a v µs
 * (`time_us`); `frame` je pořadí zvukového snímku (20 ms PAL / 16,7 ms NTSC)
 * od začátku zachycení.
 *
 * Začátek: audio_capture_start() zachycení jen naplánuje, skutečně začne na
 * konci zvukového snímku (audio_capture_on_frame()), kdy jsou dopočítané
 * všechny kanály PSG - stejně jako začátek video záznamu. Se sdl.wav až po
 * rozjezdu SDL cesty (6 snímků, bloky se zahazují), bez něj na konci
 * nejbližšího snímku. Úrovně kanálů na začátku se berou z živého audio logu;
 * renderer reference.wav se zahřeje 100 ms se stejnými úrovněmi (trvalé
 * úrovně začnou zaparkované jako v SDL cestě).
 *
 * `reference.wav` a `sdl.wav` mají stejný formát (44100 Hz, 16 bit, stereo)
 * a lze je přímo porovnat. SDL cesta je ale jiný zdroj: zpracovává hotové
 * zvukové snímky se zpožděním (bez zařízení přesně o jeden snímek, se
 * zařízením podle jeho bufferu) a při MAX SPEED může snímky vynechat
 * (status: `frames` vs `sdl_blocks`).
 * V režimu bez zvukového zařízení (headless) si zachycení SDL bloky vyrábí
 * samo na každý snímek (iface_audio_20ms_sync()).
 *
 * Konec: audio_capture_stop(), dosažení `frames`, pojistka
 * AUDIO_CAPTURE_MAX_FRAMES_SAFETY, skok času zpět (reset emulátoru, načtení
 * snapshotu), chyba zápisu, nebo ukončení emulátoru (audio_capture_exit()).
 *
 * Ovládání: MCP (dbgapi DBGAPI_CMD_AUDIO_CAPTURE), GUI (Tools -> Audio
 * Capture, Alt+Q) a příkazová řádka (--audio-capture, --audio-capture-frames;
 * spouští mzarch po bootu). GUI a příkazová řádka berou výběr souborů
 * z g_audio_capture_settings (INI `[AUDIO_CAPTURE]`); bez zadané složky se
 * zachycení zakládá ve výstupní složce zachycení (výchozí `audio_captures`
 * vedle emulátoru).
 *
 * @par Vlákna
 * Všechny funkce kromě audio_capture_config_init() a
 * audio_capture_params_from_settings() lze volat z libovolného vlákna; stav
 * chrání jeden zámek modulu. Háčky audio_capture_on_*() volá emu vlákno,
 * audio_capture_sdl_output() vlákno SDL zvuku (GUI) nebo emu vlákno
 * (headless). g_audio_capture_settings patří UI vláknu (jako nastavení
 * screenshotu).
 *
 * @par Licence: GPLv3
 */

#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximální délka cesty ke složce. */
#define AUDIO_CAPTURE_PATH_MAX 1024
/** @brief Velikost bufferů s textem (chyba, důvod konce). */
#define AUDIO_CAPTURE_TEXT_MAX 256
/** @brief Vzorkovací frekvence obou WAV [Hz] (stejná jako výstup SDL cesty). */
#define AUDIO_CAPTURE_WAV_RATE 44100u
/** @brief Pojistka délky zachycení bez `frames` (1 hodina při 60 snímcích/s). */
#define AUDIO_CAPTURE_MAX_FRAMES_SAFETY 216000u

/** @brief Parametry zachycení. */
typedef struct st_AUDIO_CAPTURE_PARAMS {
    char dir[AUDIO_CAPTURE_PATH_MAX]; /**< Složka; "" = nová podsložka `<platforma>_audio_RRRRMMDD_HHMMSS` výstupní složky zachycení (audio_capture_resolve_output_dir()). */
    uint64_t frames;                  /**< Po tolika zvukových snímcích automatický stop (0 = do stop, nejvýš pojistka). */
    bool events;                      /**< Zapisovat events.csv. */
    bool psg_writes;                  /**< Zapisovat psg_writes.csv. */
    bool reference_wav;               /**< Zapisovat reference.wav. */
    bool sdl_wav;                     /**< Zapisovat sdl.wav. */
    bool quit_after_stop;             /**< Po dosažení `frames` ukončit emulátor (CLI v režimu --headless). */
} st_AUDIO_CAPTURE_PARAMS;

/**
 * @brief Výběr souborů pro GUI a příkazovou řádku (INI `[AUDIO_CAPTURE]`).
 *
 * MCP si soubory volí parametry a nastavení nepoužívá.
 */
typedef struct st_AUDIO_CAPTURE_SETTINGS {
    char output_dir[AUDIO_CAPTURE_PATH_MAX]; /**< INI `output_dir`: výstupní složka; "" = `audio_captures` vedle emulátoru. */
    bool events;         /**< INI `events`: zapisovat events.csv (výchozí 1). */
    bool psg_writes;     /**< INI `psg_writes`: zapisovat psg_writes.csv (výchozí 1). */
    bool reference_wav;  /**< INI `reference_wav`: zapisovat reference.wav (výchozí 1). */
    bool sdl_wav;        /**< INI `sdl_wav`: zapisovat sdl.wav (výchozí 1). */
} st_AUDIO_CAPTURE_SETTINGS;

/**
 * @brief Aktuální nastavení (UI vlákno; ukládá se do INI při ukončení).
 *
 * Bez zámku: output_dir čte i start z MCP v emu vlákně (audio_capture_resolve_output_dir()).
 * Zápis dělá jen dialog nastavení v UI vlákně; souběh se startem z MCP ve
 * stejném okamžiku je prakticky nedosažitelný (stejně jako u g_videorec_settings).
 */
extern st_AUDIO_CAPTURE_SETTINGS g_audio_capture_settings;

/** @brief Stav zachycení (běžícího nebo posledního). */
typedef struct st_AUDIO_CAPTURE_STATUS {
    bool pending;                /**< Start naplánovaný, čeká na konec zvukového snímku. */
    bool active;                 /**< Zachytává se. */
    uint64_t frames;             /**< Zachycené zvukové snímky. */
    uint64_t events;             /**< Řádky events.csv. */
    uint64_t psg_writes;         /**< Řádky psg_writes.csv. */
    uint64_t reference_samples;  /**< Vzorky (stereo páry) v reference.wav. */
    uint64_t sdl_blocks;         /**< Bloky SDL cesty zapsané do sdl.wav (ideálně = frames). */
    uint64_t sdl_samples;        /**< Vzorky (stereo páry) v sdl.wav. */
    char dir[AUDIO_CAPTURE_PATH_MAX];          /**< Složka ("" = dosud žádná). */
    char stop_reason[AUDIO_CAPTURE_TEXT_MAX];  /**< Anglicky: proč zachycení skončilo ("" = běží / nikdy). */
    char last_error[AUDIO_CAPTURE_TEXT_MAX];   /**< Anglicky: poslední chyba ("" = žádná). */
} st_AUDIO_CAPTURE_STATUS;

/**
 * @brief Příznak pro rychlé testy v háčcích (0 = nic se nezachytává ani nečeká).
 *
 * Bit 0 = start naplánovaný nebo zachycení běží (háčky emu vlákna),
 * bit 1 = zapisuje se sdl.wav (háček SDL cesty). Čte se atomicky.
 */
extern gint g_audio_capture_flags;

/** @brief Bit g_audio_capture_flags: zachycení naplánované nebo běží. */
#define AUDIO_CAPTURE_FLAG_ON  0x01
/** @brief Bit g_audio_capture_flags: zapisuje se sdl.wav. */
#define AUDIO_CAPTURE_FLAG_SDL 0x02

/** @brief Výchozí parametry: všechny soubory, bez limitu snímků, výchozí složka. */
static inline void audio_capture_params_init(st_AUDIO_CAPTURE_PARAMS *p)
{
    memset(p, 0, sizeof(*p));
    p->events = true;
    p->psg_writes = true;
    p->reference_wav = true;
    p->sdl_wav = true;
}

/** @brief Je potřeba volat háčky emu vlákna (start naplánovaný nebo běží)? */
static inline bool audio_capture_wanted(void)
{
    return (g_atomic_int_get(&g_audio_capture_flags) & AUDIO_CAPTURE_FLAG_ON) != 0;
}

/** @brief Zapisuje se sdl.wav (rozhoduje i o výrobě SDL bloků bez zařízení)? */
static inline bool audio_capture_wants_sdl(void)
{
    return (g_atomic_int_get(&g_audio_capture_flags) & AUDIO_CAPTURE_FLAG_SDL) != 0;
}

/**
 * @brief Naplánuje zachycení: vytvoří složku a soubory, začne na konci zvukového snímku.
 *
 * @param p        Parametry.
 * @param err      Buffer na anglický text chyby, nebo NULL.
 * @param err_size Velikost `err`.
 * @return false = zachycení už běží, žádný soubor není zvolený, nebo nejde
 *         vytvořit složka či soubor (text v `err`); nic se nespustí.
 * @post Při úspěchu status.pending == true a status.dir je složka.
 */
bool audio_capture_start(const st_AUDIO_CAPTURE_PARAMS *p, char *err, size_t err_size);

/**
 * @brief Ukončí zachycení (i naplánované), dopíše a zavře soubory a capture.json.
 * @return false = nic neběželo ani nečekalo.
 */
bool audio_capture_stop(void);

/**
 * @brief Stav běžícího nebo posledního zachycení.
 * @param out Výstup.
 */
void audio_capture_get_status(st_AUDIO_CAPTURE_STATUS *out);

/**
 * @brief Háček: změna výstupní úrovně zvukového kanálu (volá audio_changed()).
 * @param src_id Index kanálu (0 = CTC0, 1..4 = PSG0, 5..8 = PSG1).
 * @param value  Nová úroveň 0..15.
 * @param ticks  Absolutní čas změny v taktech GDG.
 * @pre Emu vlákno, audio_capture_wanted().
 */
void audio_capture_on_change(unsigned src_id, uint8_t value, uint64_t ticks);

/**
 * @brief Háček: zápis do PSG (volá psg_write_byte() po zápisu do registrů).
 * @param ticks Absolutní čas zápisu v taktech GDG.
 * @param mask  Cílové PSG: bit 0 = PSG0 (mono), bit 1 = PSG1.
 * @param value Zapsaný bajt.
 * @pre Emu vlákno, audio_capture_wanted().
 */
void audio_capture_on_psg_write(uint64_t ticks, unsigned mask, uint8_t value);

/**
 * @brief Háček: konec zvukového snímku (volá audiolog_finish_20ms_frame()).
 *
 * Naplánované zachycení zde začne; běžící vyrenderuje reference.wav do
 * `horizon_ticks` a zkontroluje konec (frames, pojistka, skok času).
 * Konec po `frames` s quit_after_stop ukončí emulátor (emulator_quit(),
 * nevrátí se), stejně jako video záznam s --record-frames.
 *
 * @param horizon_ticks Čas, do kterého jsou doručené všechny změny kanálů.
 * @pre Emu vlákno, audio_capture_wanted(); g_audio.log je log končícího snímku.
 */
void audio_capture_on_frame(uint64_t horizon_ticks);

/**
 * @brief Háček: blok výstupu SDL cesty (volá iface_audio_wait_for_data()).
 * @param stereo Prokládané L,R vzorky v rozsahu -1..1.
 * @param frames Počet stereo párů.
 * @note Libovolné vlákno; bez běžícího zachycení se nic nestane.
 */
void audio_capture_sdl_output(const float *stereo, size_t frames);

/**
 * @brief Ukončí zachycení při ukončení emulátoru (soubory se dopíší).
 * @note Libovolné vlákno (bere zámek modulu).
 */
void audio_capture_exit(void);

/**
 * @brief Zaregistruje sekci INI `[AUDIO_CAPTURE]` a načte g_audio_capture_settings.
 * @pre Voláno jednou z cfgmain_init() (vedle screenshot_config_init()).
 */
void audio_capture_config_init(void);

/**
 * @brief Výstupní složka zachycení (kořen, do kterého se zakládají podsložky jednotlivých zachycení).
 *
 * Nastavená `output_dir`, jinak `audio_captures` v domovském adresáři
 * emulátoru (vedle `screenshots`), jinak ".". Složku vytvoří, pokud chybí.
 *
 * @param out  Výstup.
 * @param size Velikost `out`.
 * @return false = nastavenou output_dir nelze vytvořit (out ji přesto obsahuje).
 */
bool audio_capture_resolve_output_dir(char *out, size_t size);

/**
 * @brief Výchozí parametry s výběrem souborů z g_audio_capture_settings.
 * @param p Výstup (dir "", frames 0, quit_after_stop false).
 */
void audio_capture_params_from_settings(st_AUDIO_CAPTURE_PARAMS *p);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_CAPTURE_H */
