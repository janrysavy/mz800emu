/**
 * @file membrowser_io.h
 * @brief Emu backend wrapper kolem dbgapi_regions_* pro Memory Browser.
 *
 * Implementuje st_HEX_VIEW_BACKEND vtable nad debug API regiony. Tento
 * soubor je JEDINÉ místo v Memory Browser kódu, kde se volá
 * dbgapi_regions_read/write/enumerate. Core hex view + ostatní moduly
 * hex_view_backend abstrakci NESMÍ obejít.
 *
 * Page cache: V0 minimální - bez cache (každý read jde rovnou na dbgapi).
 * Pokud bude scroll Ramdisk 16 MB lagovat, V1 přidá 64 KB LRU chunk cache.
 *
 * Region enumerate cache: per-frame, invalidovaná při HW reconfigure.
 *
 * ----------------------------- License -------------------------------------
 *
 * GPL-3.0-or-later.
 *
 * ---------------------------------------------------------------------------
 */

#ifndef MEMBROWSER_IO_H
#define MEMBROWSER_IO_H

#include <stdint.h>
#include <stdbool.h>

#include "hex_view_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* dbgapi_regions.h forward typedef bez include do hlavičky. */
struct st_REGION_DESC;

/**
 * @brief Per-frame snapshot region listu.
 *
 * Drží výsledek dbgapi_regions_enumerate_dup (všechny regiony sestavy, bez
 * stropu na počet). UI ho čte read-only během jednoho framu. Po edit /
 * banking change se refreshne.
 */
typedef struct st_MEMBROWSER_REGION_SNAPSHOT
{
    int count;                              /**< Skutečný počet enumerated regionů. */
    struct st_REGION_DESC *items;           /**< Pole z dbgapi_regions_enumerate_dup (vlastní membrowser_io). */
} st_MEMBROWSER_REGION_SNAPSHOT;

/**
 * @brief Refresh region snapshot (volá se per frame nebo on demand).
 *
 * Naplní vnitřní buffer přes dbgapi_regions_enumerate_dup (všechny regiony,
 * bez stropu). Pointer vrácený v out zůstává platný do dalšího refresh
 * volání (refresh předchozí buffer uvolní).
 *
 * @param[out] out Snapshot struktura (NESmí být NULL).
 * @return Počet enumerated regionů (0 = žádný).
 */
int membrowser_io_refresh_regions ( st_MEMBROWSER_REGION_SNAPSHOT *out );

/**
 * @brief Najde region_id v aktuálním snapshotu dle (kind, sub_id).
 *
 * @param snap Snapshot z membrowser_io_refresh_regions().
 * @param kind en_REGION_KIND.
 * @param sub_id Disambiguator.
 * @return region_id (>=0) nebo -1 pokud nenalezen / disconnected.
 */
int membrowser_io_find_region ( const st_MEMBROWSER_REGION_SNAPSHOT *snap,
                                int kind, int sub_id );

/**
 * @brief Inicializuje st_HEX_VIEW_BACKEND vtable pro emu region.
 *
 * Vyplní function pointery a nastaví ctx = (void*)(intptr_t)region_id.
 * Backend deleguje na dbgapi_regions_read/write/enumerate.
 *
 * Lifetime: backend struct vlastní caller (typicky stack allokovaná
 * v render path); function pointery zůstávají platné po celou session.
 *
 * @param[out] be         Backend struct (NESmí být NULL).
 * @param[in]  region_id  Region ID z aktuálního enumerate snapshotu.
 * @param[in]  is_writable Writable flag z st_REGION_DESC.writable (false =
 *                         write_bytes bude no-op vracející -1).
 */
void membrowser_io_make_emu_backend ( st_HEX_VIEW_BACKEND *be,
                                      int region_id, bool is_writable );

/**
 * @brief Invalidate kompletní page cache (V0-leftovers F5).
 *
 * Volat při:
 *   - emu pause/resume (banking mohlo změnit fyzickou paměť)
 *   - explicit edit přes write_bytes (cache page by jinak vracela starou
 *     hodnotu - membrowser_io_write_bytes řeší auto-invalidate ovlivněné
 *     stránky, full flush je pro vnější změny)
 *   - encoding change (encoding sám o sobě page data nemění, ale uživatel
 *     očekává fresh view; per V0-leftovers brief explicit požadavek)
 *   - region snapshot refresh kdy se změnily region_id mapování
 *
 * Bezpečné volat z UI vlákna, no-op pokud cache prázdná.
 */
void membrowser_io_cache_invalidate_all ( void );

/**
 * @brief Page cache statistiky pro UI debug/status (V0-leftovers F5).
 *
 * @param[out] out_pages_used  Počet aktuálně držených stránek v cache.
 * @param[out] out_pages_max   Maximální počet stránek (LRU capacity).
 * @param[out] out_hits        Cumulative hit count od init/invalidate.
 * @param[out] out_misses      Cumulative miss count.
 */
void membrowser_io_cache_stats ( int *out_pages_used, int *out_pages_max,
                                 uint64_t *out_hits, uint64_t *out_misses );

#ifdef __cplusplus
}
#endif

#endif /* MEMBROWSER_IO_H */
