/**
 * @file snapshot_io.h
 * @brief I/O vrstva snapshot systému — ZIP čtení/zápis přes minizip-ng
 *
 * Zapouzdřuje veškerou práci se ZIP archivem. Snapshot handlery
 * pracují výhradně přes toto API a nevidí minizip-ng přímo.
 */

#ifndef SNAPSHOT_IO_H
#define SNAPSHOT_IO_H

#include "snapshot.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle pro ZIP archiv */
typedef struct snapshot_io_t snapshot_io_t;


/**
 * Otevření archivu pro zápis (save)
 *
 * @param filepath Cesta k výstupnímu souboru
 * @param compression_level ZIP kompresní úroveň (0-9)
 * @return Handle nebo NULL při chybě
 */
snapshot_io_t *snapshot_io_open_write(const char *filepath, int compression_level);

/**
 * Otevření archivu pro čtení (load)
 *
 * @param filepath Cesta ke vstupnímu souboru
 * @return Handle nebo NULL při chybě
 */
snapshot_io_t *snapshot_io_open_read(const char *filepath);

/**
 * Otevření archivu pro zápis do paměťového bufferu (save)
 *
 * Místo zápisu do souboru se ZIP archiv buduje v paměti přes minizip-ng
 * mem stream. Pro získání hotových dat zavolat
 * @ref snapshot_io_close_to_buffer (NIKOLI @ref snapshot_io_close,
 * jinak data zaniknou).
 *
 * @param compression_level ZIP kompresní úroveň (0-9)
 * @return Handle nebo NULL při chybě
 *
 * @note Volající je vlastníkem handle - musí jej uzavřít přes
 *       @ref snapshot_io_close_to_buffer (úspěšná cesta) nebo
 *       @ref snapshot_io_close (cesta s chybou, bez extrakce dat).
 */
snapshot_io_t *snapshot_io_open_write_buffer(int compression_level);

/**
 * Otevření archivu pro čtení z paměťového bufferu (load)
 *
 * Vstupem jsou hotová .mzs data v paměti (např. dříve vygenerovaná přes
 * @ref snapshot_io_open_write_buffer, nebo přijatá po síti přes MCP).
 * Handle udržuje vnitřní mem stream nad volajícím dodanou pamětí -
 * volající MUSÍ udržet platnost @p data po celou dobu životnosti handle
 * (uvolnit až po @ref snapshot_io_close).
 *
 * @param data Ukazatel na ZIP data v paměti (vlastnictví zůstává volajícímu)
 * @param size Velikost dat v bajtech
 * @return Handle nebo NULL při chybě (neplatná data, prázdný buffer, atd.)
 */
snapshot_io_t *snapshot_io_open_read_buffer(const uint8_t *data, size_t size);

/**
 * Zavření archivu a uvolnění zdrojů
 *
 * Pro writer otevřený přes @ref snapshot_io_open_write_buffer tato funkce
 * zahodí nasbíraná data. Pokud chceš data získat, volej
 * @ref snapshot_io_close_to_buffer.
 */
void snapshot_io_close(snapshot_io_t *io);

/**
 * Zavření buffer-writeru a extrakce hotových .mzs dat
 *
 * Volat MÍSTO @ref snapshot_io_close pro handle otevřený přes
 * @ref snapshot_io_open_write_buffer. Po návratu je handle invalidní
 * (nepoužívat opakovaně).
 *
 * @param io Handle (musí být buffer-writer; jinak SNAPSHOT_ERR_IO)
 * @param out_data Výstupní ukazatel na nově alokovaný buffer s .mzs daty.
 *                 Volající uvolní přes g_free(). Při chybě bude *out_data = NULL.
 * @param out_size Výstupní velikost dat v bajtech. Při chybě bude *out_size = 0.
 * @return SNAPSHOT_OK při úspěchu; jinak chybový kód a handle je přesto
 *         uvolněn (volat znovu close není potřeba).
 */
en_SNAPSHOT_RESULT snapshot_io_close_to_buffer(snapshot_io_t *io,
                                              uint8_t **out_data,
                                              size_t *out_size);

/**
 * @brief Otevře handle v hashovacím režimu (bez ZIP).
 *
 * Zápisy (@ref snapshot_io_write_bin, @ref snapshot_io_write_xml) se
 * nikam neukládají, jen se z nich počítá SHA-256 v pořadí zápisu: jméno
 * entry včetně koncové nuly, délka (8 bajtů little endian) a data. Hlavička
 * ZIP (čas entry) se do hashe nedostane, takže stejný stav dá vždy stejný
 * hash. Slouží k porovnání stavu komponent (hash části `periph` záznamu
 * vstupů).
 *
 * @return Handle (nikdy NULL); uzavřít @ref snapshot_io_close_hash, nebo
 *         @ref snapshot_io_close (hash se zahodí). Čtecí funkce na něm
 *         vracejí chybu.
 */
snapshot_io_t *snapshot_io_open_hash(void);

/**
 * @brief Zavře handle z @ref snapshot_io_open_hash a vrátí hash.
 *
 * @param io       Handle v hashovacím režimu (po návratu neplatný).
 * @param out_hex  Výstup: SHA-256 jako 64 hex znaků malými písmeny a nula.
 * @param out_size Velikost @p out_hex (aspoň 65).
 * @return SNAPSHOT_OK; SNAPSHOT_ERR_IO, když handle není v hashovacím
 *         režimu nebo je @p out_hex malý (pak je @p out_hex prázdný).
 *         Handle se uvolní vždy.
 */
en_SNAPSHOT_RESULT snapshot_io_close_hash(snapshot_io_t *io, char *out_hex, size_t out_size);

/**
 * Zápis binárních dat do archivu
 *
 * @param io Handle
 * @param entry_name Cesta v archivu (např. "memory/ram.bin")
 * @param data Ukazatel na data
 * @param size Velikost dat v bajtech
 * @return SNAPSHOT_OK při úspěchu
 */
en_SNAPSHOT_RESULT snapshot_io_write_bin(snapshot_io_t *io,
                                        const char *entry_name,
                                        const uint8_t *data,
                                        size_t size);

/**
 * Zápis XML řetězce do archivu
 *
 * @param io Handle
 * @param entry_name Cesta v archivu (např. "cpu/z80.xml")
 * @param xml_string XML řetězec (null-terminated)
 * @return SNAPSHOT_OK při úspěchu
 */
en_SNAPSHOT_RESULT snapshot_io_write_xml(snapshot_io_t *io,
                                        const char *entry_name,
                                        const char *xml_string);

/**
 * Čtení binárních dat z archivu (alokuje paměť)
 *
 * @param io Handle
 * @param entry_name Cesta v archivu
 * @param out_data Výstupní ukazatel na data (volající uvolní přes g_free)
 * @param out_size Výstupní velikost přečtených dat
 * @return SNAPSHOT_OK při úspěchu
 */
en_SNAPSHOT_RESULT snapshot_io_read_bin(snapshot_io_t *io,
                                       const char *entry_name,
                                       uint8_t **out_data,
                                       size_t *out_size);

/**
 * Čtení binárních dat do existujícího bufferu
 *
 * @param io Handle
 * @param entry_name Cesta v archivu
 * @param buffer Cílový buffer
 * @param buffer_size Očekávaná velikost dat
 * @return SNAPSHOT_OK při úspěchu
 */
en_SNAPSHOT_RESULT snapshot_io_read_bin_into(snapshot_io_t *io,
                                            const char *entry_name,
                                            uint8_t *buffer,
                                            size_t buffer_size);

/**
 * Čtení XML řetězce z archivu
 *
 * @param io Handle
 * @param entry_name Cesta v archivu
 * @param out_xml_string Výstupní ukazatel na řetězec (volající uvolní přes g_free)
 * @return SNAPSHOT_OK při úspěchu
 */
en_SNAPSHOT_RESULT snapshot_io_read_xml(snapshot_io_t *io,
                                       const char *entry_name,
                                       char **out_xml_string);

/**
 * Zjistí zda entry v archivu existuje
 *
 * @param io Handle
 * @param entry_name Cesta v archivu
 * @return true pokud entry existuje
 */
bool snapshot_io_entry_exists(snapshot_io_t *io, const char *entry_name);

/**
 * Vypočítá SHA-256 hash všech entries v archivu kromě "manifest.xml"
 *
 * Entries se zpracovávají v abecedním pořadí názvů.
 *
 * @param io Handle (musí být otevřen pro čtení)
 * @return Hex string (64 znaků + null), volající uvolní přes g_free. NULL při chybě.
 */
char *snapshot_io_compute_checksum(snapshot_io_t *io);


#ifdef __cplusplus
}
#endif

#endif /* SNAPSHOT_IO_H */
