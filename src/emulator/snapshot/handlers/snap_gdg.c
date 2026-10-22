/**
 * @file snap_gdg.c
 * @brief Snapshot handler: GDG — zobrazovací řadič
 *
 * Čekající GDG událost (g_gdg.event) se do snapshotu neukládá - při loadu
 * se dopočítá z obnovené pozice paprsku, viz snap_gdg_event_resync().
 * Na MZ-800 ukládá i registry HW scrollu (g_hwscroll); snapshot bez nich
 * načte HW scroll ve stavu po resetu.
 */

#include <stdio.h>
#include <glib.h>

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/gdg/video.h"
#if MZARCH == 800
#include "mzarch/mz800/gdg/mz800_hwscroll.h"
#endif

static en_SNAPSHOT_RESULT snap_gdg_save(st_SNAPSHOT_CONTEXT *ctx)
{
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    snapshot_xml_open_element(w, "gdg_state");

    /* Časové razítko */
    snapshot_xml_open_element(w, "total_elapsed");
    snapshot_xml_write_uint(w, "screens", g_gdg.total_elapsed.screens);
    snapshot_xml_write_uint(w, "ticks", g_gdg.total_elapsed.ticks);
    snapshot_xml_close_element(w);

    /* Pozice paprsku */
    snapshot_xml_write_uint(w, "beam_row", g_gdg.beam_row);

    /* Registry */
    snapshot_xml_open_element(w, "registers");
    snapshot_xml_write_uint(w, "regDMD", g_gdg.regDMD);
    snapshot_xml_write_uint(w, "regBOR", g_gdg.regBOR);
#if MZARCH == 800
    snapshot_xml_write_uint(w, "regPALGRP", g_gdg.regPALGRP);
    snapshot_xml_write_uint(w, "regPAL0", g_gdg.regPAL0);
    snapshot_xml_write_uint(w, "regPAL1", g_gdg.regPAL1);
    snapshot_xml_write_uint(w, "regPAL2", g_gdg.regPAL2);
    snapshot_xml_write_uint(w, "regPAL3", g_gdg.regPAL3);
    snapshot_xml_write_uint(w, "cksw", g_gdg.cksw);
#endif
    snapshot_xml_write_uint(w, "regct53g7", g_gdg.regct53g7);
#if MZARCH == 800
    /* Model WAIT u VRAM - uvolnění WRITE FF posledního zápisu. Starší
     * snapshoty bez klíče se při načtení inicializují na 0 (žádný zápis);
     * dřívější klíče vram800_hot_phase_* se už nečtou. */
    snapshot_xml_write_uint64(w, "vram_wrff_release", g_gdg.vram_wrff_release);
#endif
    snapshot_xml_close_element(w); /* registers */

    /* Tempo */
    snapshot_xml_write_uint(w, "tempo", g_gdg.tempo);
    snapshot_xml_write_uint(w, "tempo_divider", g_gdg.tempo_divider);

    /* Synchronizační signály */
    snapshot_xml_open_element(w, "sync_signals");
    snapshot_xml_write_uint(w, "sts_vsync", g_gdg.sts_vsync);
    snapshot_xml_write_uint(w, "sts_hsync", g_gdg.sts_hsync);
    snapshot_xml_write_uint(w, "hbln", g_gdg.hbln);
    snapshot_xml_write_uint(w, "vbln", g_gdg.vbln);
    snapshot_xml_close_element(w); /* sync_signals */

    /* Stav renderování - průběh vykreslení obrazu pro hostitele, ne stav
     * stroje (posune ho např. překreslení v pauze); hash ho vynechá. */
    if (!ctx->for_hash) {
        snapshot_xml_write_uint(w, "screen_is_already_rendered_at_beam_pos",
                                g_gdg.screen_is_already_rendered_at_beam_pos);
        snapshot_xml_write_uint(w, "screen_need_update_from",
                                g_gdg.screen_need_update_from);
        snapshot_xml_write_uint(w, "last_updated_border_pixel",
                                g_gdg.last_updated_border_pixel);
    }

#if MZARCH == 1500
    /* MZ-1500 specifické: barevná paleta */
    snapshot_xml_open_element(w, "mode1500_colors");
    snapshot_xml_write_int(w, "mode1500_color_0", g_gdg.mode1500_color[0]);
    snapshot_xml_write_int(w, "mode1500_color_1", g_gdg.mode1500_color[1]);
    snapshot_xml_write_int(w, "mode1500_color_2", g_gdg.mode1500_color[2]);
    snapshot_xml_write_int(w, "mode1500_color_3", g_gdg.mode1500_color[3]);
    snapshot_xml_write_int(w, "mode1500_color_4", g_gdg.mode1500_color[4]);
    snapshot_xml_write_int(w, "mode1500_color_5", g_gdg.mode1500_color[5]);
    snapshot_xml_write_int(w, "mode1500_color_6", g_gdg.mode1500_color[6]);
    snapshot_xml_write_int(w, "mode1500_color_7", g_gdg.mode1500_color[7]);
    snapshot_xml_close_element(w); /* mode1500_colors */
#elif MZARCH == 700
    /* MZ-700 specifické: barevná paleta */
    snapshot_xml_open_element(w, "mode700_colors");
    snapshot_xml_write_int(w, "mode700_color_0", g_gdg.mode700_color[0]);
    snapshot_xml_write_int(w, "mode700_color_1", g_gdg.mode700_color[1]);
    snapshot_xml_write_int(w, "mode700_color_2", g_gdg.mode700_color[2]);
    snapshot_xml_write_int(w, "mode700_color_3", g_gdg.mode700_color[3]);
    snapshot_xml_write_int(w, "mode700_color_4", g_gdg.mode700_color[4]);
    snapshot_xml_write_int(w, "mode700_color_5", g_gdg.mode700_color[5]);
    snapshot_xml_write_int(w, "mode700_color_6", g_gdg.mode700_color[6]);
    snapshot_xml_write_int(w, "mode700_color_7", g_gdg.mode700_color[7]);
    snapshot_xml_close_element(w); /* mode700_colors */
#endif

#if MZARCH == 800
    /* HW scroll: registry překládají adresy CPU přístupů do VRAM. */
    snapshot_xml_open_element(w, "hwscroll");
    snapshot_xml_write_uint(w, "regSSA", g_hwscroll.regSSA);
    snapshot_xml_write_uint(w, "regSEA", g_hwscroll.regSEA);
    snapshot_xml_write_uint(w, "regSW", g_hwscroll.regSW);
    snapshot_xml_write_uint(w, "regSOF", g_hwscroll.regSOF);
    snapshot_xml_write_uint(w, "enabled", g_hwscroll.enabled);
    snapshot_xml_close_element(w); /* hwscroll */
#endif

    snapshot_xml_close_element(w); /* gdg_state */

    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, "hw/gdg.xml", xml);
    g_free(xml);

    return res;
}

/**
 * @brief Dopočítá čekající GDG událost (g_gdg.event) z obnovené pozice paprsku.
 *
 * g_gdg.event je další GDG událost, kterou plánovač (mzarch_main_queue_next_event,
 * gdg_event_set_next) čeká. Snapshot ji neukládá, a bez dopočtu by po loadu
 * zůstala z předchozího stroje: např. AFTER_LAST_SCREEN_PIXEL řádku 103
 * zpracovaná při obnoveném beam_row = 0 čte VRAM mimo rozsah (index
 * (beam_row - 46) * 40 přeteče) a emulátor spadne; jindy se přeskočí nebo
 * posunou události řádku (HBLN, VSYNC, hrana CTC1 z REAL_HSYNC_START).
 *
 * Mezi instrukcemi (jen tam se snapshot ukládá a načítá) mzarch_main_process_events
 * zpracoval všechny události s ticks <= total_elapsed.ticks, takže čekající
 * událost je první aktivní událost aktuálního řádku (v pořadí g_gdgevent[],
 * seřazeném vzestupně podle event_column) s ticks > total_elapsed.ticks -
 * tedy totéž, co by vybral gdg_event_set_next. V konzistentním stavu takovou
 * událost najdeme vždy: poslední událost řádku SCREEN_ROW_END leží na
 * (beam_row + 1) * VIDEO_SCREEN_WIDTH.
 *
 * Index do g_gdgevent[] je hodnota en_MZEVENT (MZEVENT_GDG_HBLN_END = 0 ..
 * MZEVENT_GDG_SCREEN_ROW_END), stejně jako v gdg_event_set_next.
 *
 * @pre g_gdg.beam_row a g_gdg.total_elapsed.ticks jsou už obnovené ze snapshotu
 *      a g_gdg.beam_row < VIDEO_SCREEN_HEIGHT.
 * @post g_gdg.event odpovídá obnovenému rastru. Pokud je total_elapsed.ticks
 *       za koncem řádku (nekonzistentní snapshot), nastaví se SCREEN_ROW_END
 *       aktuálního řádku - zpracuje se hned při dalším mzarch_main_process_events
 *       a plánovač se tím sám srovná.
 * @note Volá se na EMU vlákně v pauze (snapshot load), bez dalšího zamykání.
 *       Hlavní událost g_mzarch_main.event obnovuje snap_mzarch; z g_gdg.event
 *       ji plánovač přepočítá při nejbližším mzarch_main_queue_next_event.
 */
static void snap_gdg_event_resync(void)
{
    unsigned row = g_gdg.beam_row;
    unsigned row_ticks = row * VIDEO_SCREEN_WIDTH;

    for (int name = MZEVENT_GDG_HBLN_END; name <= MZEVENT_GDG_SCREEN_ROW_END; name++)
    {
        const st_GDGEVENT *ev = &g_gdgevent[name];
        unsigned ticks = row_ticks + ev->event_column;
        if (row >= ev->start_row && row < ev->start_row + ev->num_rows &&
            ticks > g_gdg.total_elapsed.ticks)
        {
            g_gdg.event.event_name = (en_MZEVENT)name;
            g_gdg.event.ticks = ticks;
            return;
        }
    }

    /* total_elapsed za koncem řádku - nekonzistentní stav, viz @post */
    g_gdg.event.event_name = MZEVENT_GDG_SCREEN_ROW_END;
    g_gdg.event.ticks = row_ticks + g_gdgevent[MZEVENT_GDG_SCREEN_ROW_END].event_column;
}

static en_SNAPSHOT_RESULT snap_gdg_load(st_SNAPSHOT_CONTEXT *ctx)
{
    char *xml = NULL;
    en_SNAPSHOT_RESULT res = snapshot_io_read_xml(ctx->io, "hw/gdg.xml", &xml);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("gdg", "Cannot load hw/gdg.xml");
        return res;
    }

    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    g_free(xml);

    if (!r) {
        SNAP_ERR("gdg", "Parse error in hw/gdg.xml");
        return SNAPSHOT_ERR_XML_PARSE;
    }

    if (!snapshot_xml_enter_element(r, "gdg_state")) {
        SNAP_ERR("gdg", "Missing element gdg_state");
        snapshot_xml_reader_free(r);
        return SNAPSHOT_ERR_XML_PARSE;
    }

    /* Časové razítko */
    if (snapshot_xml_enter_element(r, "total_elapsed")) {
        snapshot_xml_read_uint(r, "screens", &g_gdg.total_elapsed.screens);
        snapshot_xml_read_uint(r, "ticks", &g_gdg.total_elapsed.ticks);
        snapshot_xml_leave_element(r);
    }

    /* Pozice paprsku */
    snapshot_xml_read_uint(r, "beam_row", &g_gdg.beam_row);
    if (g_gdg.beam_row >= VIDEO_SCREEN_HEIGHT) {
        SNAP_ERR("gdg", "beam_row %u out of range", g_gdg.beam_row);
        snapshot_xml_reader_free(r);
        return SNAPSHOT_ERR_CORRUPTED;
    }

    /* Čekající GDG událost patří k obnovenému rastru, ne k předchozímu stroji. */
    snap_gdg_event_resync();

    /* Registry */
    if (snapshot_xml_enter_element(r, "registers")) {
        snapshot_xml_read_uint(r, "regDMD", &g_gdg.regDMD);
        snapshot_xml_read_uint(r, "regBOR", &g_gdg.regBOR);
#if MZARCH == 800
        snapshot_xml_read_uint(r, "regPALGRP", &g_gdg.regPALGRP);
        snapshot_xml_read_uint(r, "regPAL0", &g_gdg.regPAL0);
        snapshot_xml_read_uint(r, "regPAL1", &g_gdg.regPAL1);
        snapshot_xml_read_uint(r, "regPAL2", &g_gdg.regPAL2);
        snapshot_xml_read_uint(r, "regPAL3", &g_gdg.regPAL3);
        /* cksw - novy klic, starsi snapshoty ho nemaji -> default 0
         * (snapshot_xml_read_uint nesnaha pri chybejicim klici). */
        snapshot_xml_read_uint(r, "cksw", &g_gdg.cksw);
#endif
        snapshot_xml_read_uint(r, "regct53g7", &g_gdg.regct53g7);
#if MZARCH == 800
        /* Model WAIT u VRAM - klíč chybí ve starších snapshotech, pak 0
         * (první přístup do VRAM po načtení se chová jako bez předchozího
         * zápisu). */
        g_gdg.vram_wrff_release = 0;
        snapshot_xml_read_uint64(r, "vram_wrff_release", &g_gdg.vram_wrff_release);
#endif
        snapshot_xml_leave_element(r);
    }

    /* Tempo */
    snapshot_xml_read_uint(r, "tempo", &g_gdg.tempo);
    snapshot_xml_read_uint(r, "tempo_divider", &g_gdg.tempo_divider);

    /* Synchronizační signály */
    if (snapshot_xml_enter_element(r, "sync_signals")) {
        snapshot_xml_read_uint(r, "sts_vsync", &g_gdg.sts_vsync);
        snapshot_xml_read_uint(r, "sts_hsync", &g_gdg.sts_hsync);
        snapshot_xml_read_uint(r, "hbln", &g_gdg.hbln);
        snapshot_xml_read_uint(r, "vbln", &g_gdg.vbln);
        snapshot_xml_leave_element(r);
    }

    /* Stav renderování */
    snapshot_xml_read_uint(r, "screen_is_already_rendered_at_beam_pos",
                            &g_gdg.screen_is_already_rendered_at_beam_pos);
    snapshot_xml_read_uint(r, "screen_need_update_from",
                            &g_gdg.screen_need_update_from);
    snapshot_xml_read_uint(r, "last_updated_border_pixel",
                            &g_gdg.last_updated_border_pixel);

#if MZARCH == 1500
    /* MZ-1500 specifické: barevná paleta */
    if (snapshot_xml_enter_element(r, "mode1500_colors")) {
        snapshot_xml_read_int(r, "mode1500_color_0", &g_gdg.mode1500_color[0]);
        snapshot_xml_read_int(r, "mode1500_color_1", &g_gdg.mode1500_color[1]);
        snapshot_xml_read_int(r, "mode1500_color_2", &g_gdg.mode1500_color[2]);
        snapshot_xml_read_int(r, "mode1500_color_3", &g_gdg.mode1500_color[3]);
        snapshot_xml_read_int(r, "mode1500_color_4", &g_gdg.mode1500_color[4]);
        snapshot_xml_read_int(r, "mode1500_color_5", &g_gdg.mode1500_color[5]);
        snapshot_xml_read_int(r, "mode1500_color_6", &g_gdg.mode1500_color[6]);
        snapshot_xml_read_int(r, "mode1500_color_7", &g_gdg.mode1500_color[7]);
        snapshot_xml_leave_element(r);
    }
#elif MZARCH == 700
    /* MZ-700 specifické: barevná paleta */
    if (snapshot_xml_enter_element(r, "mode700_colors")) {
        snapshot_xml_read_int(r, "mode700_color_0", &g_gdg.mode700_color[0]);
        snapshot_xml_read_int(r, "mode700_color_1", &g_gdg.mode700_color[1]);
        snapshot_xml_read_int(r, "mode700_color_2", &g_gdg.mode700_color[2]);
        snapshot_xml_read_int(r, "mode700_color_3", &g_gdg.mode700_color[3]);
        snapshot_xml_read_int(r, "mode700_color_4", &g_gdg.mode700_color[4]);
        snapshot_xml_read_int(r, "mode700_color_5", &g_gdg.mode700_color[5]);
        snapshot_xml_read_int(r, "mode700_color_6", &g_gdg.mode700_color[6]);
        snapshot_xml_read_int(r, "mode700_color_7", &g_gdg.mode700_color[7]);
        snapshot_xml_leave_element(r);
    }
#endif

#if MZARCH == 800
    /* HW scroll; snapshot bez něj (starší verze) = stav po resetu. */
    hwscroll_reset();
    if (snapshot_xml_enter_element(r, "hwscroll")) {
        snapshot_xml_read_uint(r, "regSSA", &g_hwscroll.regSSA);
        snapshot_xml_read_uint(r, "regSEA", &g_hwscroll.regSEA);
        snapshot_xml_read_uint(r, "regSW", &g_hwscroll.regSW);
        snapshot_xml_read_uint(r, "regSOF", &g_hwscroll.regSOF);
        snapshot_xml_read_uint(r, "enabled", &g_hwscroll.enabled);
        snapshot_xml_leave_element(r);
    }
#endif

    snapshot_xml_leave_element(r); /* gdg_state */
    snapshot_xml_reader_free(r);

    return SNAPSHOT_OK;
}

void snap_gdg_register(void)
{
    snapshot_register_component("gdg",
                                SNAPSHOT_PRIORITY_HW_CORE,
                                snap_gdg_save,
                                snap_gdg_load,
                                false);
}
