/**
 * @file memsearch_sample.c
 * @brief Vzorek prostoru hledání z regionů dbgapi (emu vlákno).
 *
 * Jediná část modulu memsearch, která čte stav emulátoru. Volá se
 * z obsluhy DBGAPI_CMD_MEMSEARCH_SAMPLE v drainu fronty dbgapi, tedy na
 * hranici snímku nebo v pauze; všechny spany se tak zkopírují ve stejném
 * okamžiku emulace. Horké cesty emulace se nedotýká.
 *
 * Licence: GPLv3
 */

#include "mzarch/mzarch_config.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include <string.h>

#include "memsearch.h"
#include "emulator/debugger/dbgapi_regions.h"
#include "hw-generic/gdg/gdg.h"


bool memsearch_sample_fill(const st_MEMSEARCH_SPAN *spans, unsigned n,
                           st_MEMSEARCH_SAMPLE *sample)
{
    if ( !spans || !sample || n == 0 || n != sample->span_count ) return false;

    uint64_t total = 0;
    for ( unsigned i = 0; i < n; i++ ) total += spans[i].length;
    if ( total != sample->size ) return false;

    /* Čerstvé příznaky connected a ID podle aktuální sestavy HW. */
    dbgapi_regions_refresh ( );

    uint32_t pos = 0;
    for ( unsigned i = 0; i < n; i++ ) {
        const st_MEMSEARCH_SPAN *sp = &spans[i];
        st_REGION_DESC d;
        bool ok = dbgapi_regions_find_kind ( (en_REGION_KIND) sp->region_kind,
                                             sp->sub_id, &d )
                  && d.connected
                  && sp->offset < d.size
                  && sp->length <= d.size - sp->offset;
        if ( ok ) {
            int r = dbgapi_regions_read ( d.id, sp->offset, &sample->data[pos],
                                          sp->length );
            ok = ( r == (int) sp->length );
        };
        if ( !ok ) memset ( &sample->data[pos], 0, sp->length );
        sample->span_connected[i] = ok ? 1 : 0;
        pos += sp->length;
    };

    sample->frame = (uint64_t) g_gdg.total_elapsed.screens;
    return true;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
