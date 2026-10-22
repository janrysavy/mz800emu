/**
 * @file snap_pio8255.c
 * @brief Snapshot handler: PIO 8255 — uložení a načtení stavu klávesnice a portů
 */

#include <stdio.h>
#include <glib.h>

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"
#include "hw-generic/pio8255/pio8255.h"

/* Názvy elementů pro klávesovou matici */
static const char *km_names[10] = {
    "km_0", "km_1", "km_2", "km_3", "km_4",
    "km_5", "km_6", "km_7", "km_8", "km_9"
};


static en_SNAPSHOT_RESULT snap_pio8255_save(st_SNAPSHOT_CONTEXT *ctx)
{
    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    snapshot_xml_open_element(w, "pio8255_state");

    /* Klávesová matice — 10 sloupců. Pro hash stavu výsledná matice, kterou
     * čte program (keyboard_matrix & vkbd_matrix): stejné klávesy mohou
     * držet různé zdroje (HID skript ve vkbd_matrix při záznamu vstupů,
     * keyboard_matrix při jeho přehrání). */
    snapshot_xml_open_element(w, "keyboard_matrix");
    for (int i = 0; i < 10; i++) {
        uint8_t v = g_pio8255.keyboard_matrix[i];
        if (ctx->for_hash) v &= g_pio8255.vkbd_matrix[i];
        snapshot_xml_write_hex8(w, km_names[i], v);
    }
    snapshot_xml_close_element(w); /* keyboard_matrix */

    /* Výstupní port A a jeho dekódované signály */
    snapshot_xml_open_element(w, "port_a");
    snapshot_xml_write_uint(w, "signal_PA", g_pio8255.signal_PA);
    snapshot_xml_write_uint(w, "signal_PA_keybord_column", g_pio8255.signal_PA_keybord_column);
    snapshot_xml_write_uint(w, "signal_PA_joy1_enabled", g_pio8255.signal_PA_joy1_enabled);
    snapshot_xml_write_uint(w, "signal_PA_joy2_enabled", g_pio8255.signal_PA_joy2_enabled);
    snapshot_xml_close_element(w); /* port_a */

    /* Port C — výstupní a vstupní signály */
    snapshot_xml_open_element(w, "port_c");
    snapshot_xml_write_uint(w, "signal_PC", g_pio8255.signal_PC);
    snapshot_xml_write_uint(w, "signal_pc00", g_pio8255.signal_pc00);
    snapshot_xml_write_uint(w, "signal_pc01", g_pio8255.signal_pc01);
    snapshot_xml_write_uint(w, "signal_pc02", g_pio8255.signal_pc02);
    snapshot_xml_write_uint(w, "signal_pc03", g_pio8255.signal_pc03);
    snapshot_xml_write_uint(w, "signal_pc04", g_pio8255.signal_pc04);
    snapshot_xml_close_element(w); /* port_c */

    snapshot_xml_close_element(w); /* pio8255_state */

    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, "hw/pio8255.xml", xml);
    g_free(xml);

    return res;
}


static en_SNAPSHOT_RESULT snap_pio8255_load(st_SNAPSHOT_CONTEXT *ctx)
{
    char *xml = NULL;
    en_SNAPSHOT_RESULT res = snapshot_io_read_xml(ctx->io, "hw/pio8255.xml", &xml);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("pio8255", "Cannot load hw/pio8255.xml");
        return res;
    }

    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    g_free(xml);

    if (!r) {
        SNAP_ERR("pio8255", "Parse error in hw/pio8255.xml");
        return SNAPSHOT_ERR_XML_PARSE;
    }

    if (!snapshot_xml_enter_element(r, "pio8255_state")) {
        SNAP_ERR("pio8255", "Missing element pio8255_state");
        snapshot_xml_reader_free(r);
        return SNAPSHOT_ERR_XML_PARSE;
    }

    /* Klávesová matice */
    if (snapshot_xml_enter_element(r, "keyboard_matrix")) {
        for (int i = 0; i < 10; i++) {
            snapshot_xml_read_hex8(r, km_names[i], &g_pio8255.keyboard_matrix[i]);
        }
        snapshot_xml_leave_element(r);
    }

    /* Port A */
    if (snapshot_xml_enter_element(r, "port_a")) {
        snapshot_xml_read_uint(r, "signal_PA", &g_pio8255.signal_PA);
        snapshot_xml_read_uint(r, "signal_PA_keybord_column", &g_pio8255.signal_PA_keybord_column);
        snapshot_xml_read_uint(r, "signal_PA_joy1_enabled", &g_pio8255.signal_PA_joy1_enabled);
        snapshot_xml_read_uint(r, "signal_PA_joy2_enabled", &g_pio8255.signal_PA_joy2_enabled);
        snapshot_xml_leave_element(r);
    }

    /* Port C */
    if (snapshot_xml_enter_element(r, "port_c")) {
        snapshot_xml_read_uint(r, "signal_PC", &g_pio8255.signal_PC);
        snapshot_xml_read_uint(r, "signal_pc00", &g_pio8255.signal_pc00);
        snapshot_xml_read_uint(r, "signal_pc01", &g_pio8255.signal_pc01);
        snapshot_xml_read_uint(r, "signal_pc02", &g_pio8255.signal_pc02);
        snapshot_xml_read_uint(r, "signal_pc03", &g_pio8255.signal_pc03);
        snapshot_xml_read_uint(r, "signal_pc04", &g_pio8255.signal_pc04);
        snapshot_xml_leave_element(r);
    }

    snapshot_xml_leave_element(r); /* pio8255_state */
    snapshot_xml_reader_free(r);

    return SNAPSHOT_OK;
}


void snap_pio8255_register(void)
{
    snapshot_register_component("pio8255",
                                SNAPSHOT_PRIORITY_HW_IO,
                                snap_pio8255_save,
                                snap_pio8255_load,
                                false);
}
