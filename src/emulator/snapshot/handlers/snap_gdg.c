/**
 * @file snap_gdg.c
 * @brief Snapshot handler: GDG — zobrazovací řadič
 */

#include <stdio.h>
#include <glib.h>

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"
#include "hw-generic/gdg/gdg.h"
#include "hw-generic/gdg/video.h"

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

#if MZARCH == 800
    /* The pending GDG event belongs to this raster position. Keeping the
     * event from the pre-load machine can render a canvas row in the border. */
    snapshot_xml_open_element(w, "event");
    snapshot_xml_write_int(w, "event_name", (int)g_gdg.event.event_name);
    snapshot_xml_write_uint(w, "ticks", g_gdg.event.ticks);
    snapshot_xml_close_element(w);
#endif

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
    /* HDL-presny WAIT model 800 grafickych rezimu - stav horke faze.
     * Starsi snapshoty bez techto klicu se pri loadu defaultne zinicializuji
     * na 0 (= zadna horka faze aktivni). */
    snapshot_xml_write_uint64(w, "vram800_hot_phase_end_total_ticks",
                              g_gdg.vram800_hot_phase_end_total_ticks);
    snapshot_xml_write_uint(w, "vram800_hot_phase_clk0_phase",
                            g_gdg.vram800_hot_phase_clk0_phase);
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

    /* Stav renderování */
    snapshot_xml_write_uint(w, "screen_is_already_rendered_at_beam_pos",
                            g_gdg.screen_is_already_rendered_at_beam_pos);
    snapshot_xml_write_uint(w, "screen_need_update_from",
                            g_gdg.screen_need_update_from);
    snapshot_xml_write_uint(w, "last_updated_border_pixel",
                            g_gdg.last_updated_border_pixel);

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

    snapshot_xml_close_element(w); /* gdg_state */

    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, "hw/gdg.xml", xml);
    g_free(xml);

    return res;
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

#if MZARCH == 800
    /* Older snapshots omitted this state. Reconstruct their next GDG event
     * from the saved raster, never from the machine being replaced. */
    bool have_event = false;
    if (snapshot_xml_enter_element(r, "event")) {
        int event_name = -1;
        unsigned ticks = 0;
        have_event = snapshot_xml_read_int(r, "event_name", &event_name)
                  && snapshot_xml_read_uint(r, "ticks", &ticks);
        snapshot_xml_leave_element(r);
        if (!have_event || event_name < MZEVENT_GDG_HBLN_END
                        || event_name > MZEVENT_GDG_SCREEN_ROW_END) {
            SNAP_ERR("gdg", "Invalid pending GDG event");
            snapshot_xml_reader_free(r);
            return SNAPSHOT_ERR_XML_PARSE;
        }
        const st_GDGEVENT *event = &g_gdgevent[event_name];
        if (g_gdg.beam_row < event->start_row
            || g_gdg.beam_row >= event->start_row + event->num_rows
            || ticks != g_gdg.beam_row * VIDEO_SCREEN_WIDTH + event->event_column
            || ticks <= g_gdg.total_elapsed.ticks) {
            SNAP_ERR("gdg", "Pending GDG event does not match saved raster");
            snapshot_xml_reader_free(r);
            return SNAPSHOT_ERR_XML_PARSE;
        }
        g_gdg.event.event_name = (en_MZEVENT)event_name;
        g_gdg.event.ticks = ticks;
    }
    if (!have_event) {
        for (int index = MZEVENT_GDG_HBLN_END;
             index <= MZEVENT_GDG_SCREEN_ROW_END; ++index) {
            const st_GDGEVENT *event = &g_gdgevent[index];
            unsigned ticks = g_gdg.beam_row * VIDEO_SCREEN_WIDTH + event->event_column;
            if (g_gdg.beam_row >= event->start_row
                && g_gdg.beam_row < event->start_row + event->num_rows
                && ticks > g_gdg.total_elapsed.ticks) {
                g_gdg.event.event_name = (en_MZEVENT)index;
                g_gdg.event.ticks = ticks;
                have_event = true;
                break;
            }
        }
        if (!have_event) {
            SNAP_ERR("gdg", "Cannot reconstruct pending GDG event");
            snapshot_xml_reader_free(r);
            return SNAPSHOT_ERR_XML_PARSE;
        }
    }
#endif

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
        /* HDL-presny WAIT model - novy klic, default 0 pro starsi snapshoty
         * (= zadna horka faze aktivni, prvni VRAM pristup po loadu se chova
         *  jako bez predchoziho WRITE). */
        g_gdg.vram800_hot_phase_end_total_ticks = 0;
        g_gdg.vram800_hot_phase_clk0_phase = 0;
        snapshot_xml_read_uint64(r, "vram800_hot_phase_end_total_ticks",
                                 &g_gdg.vram800_hot_phase_end_total_ticks);
        snapshot_xml_read_uint(r, "vram800_hot_phase_clk0_phase",
                               &g_gdg.vram800_hot_phase_clk0_phase);
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
