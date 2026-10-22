/**
 * @file snap_z80.c
 * @brief Snapshot handler: Z80 CPU — uložení a načtení registrů a skrytého
 *        stavu jádra (`cpu/z80.xml`).
 *
 * Kromě viditelných registrů ukládá prvek `hidden_state` se stavem, který
 * jádro drží mezi instrukcemi a který ovlivňuje další běh: WZ (MEMPTR),
 * Q, EI delay, čekající INT a NMI, vektor přerušení, příznak LD A,I/R
 * a `total_cycles`. Snapshot ze starší verze bez `hidden_state` nastaví
 * skrytý stav na výchozí hodnoty resetu (@ref z80_reset) a `total_cycles`
 * ponechá ze stroje, aby výsledek nezávisel na stavu před načtením.
 */

#include <stdio.h>
#include <glib.h>

#include "snapshot/snapshot_mgr.h"
#include "snapshot/snapshot_xml.h"
#include "mzarch/mzarch.h"

/** @brief Jméno prvku se skrytým stavem Z80 v `cpu/z80.xml`. */
#define SNAP_Z80_HIDDEN_ELEMENT "hidden_state"

/**
 * @brief Zapíše prvek `hidden_state` se skrytým stavem Z80.
 *
 * @param w   Otevřený XML writer uvnitř prvku `z80_state`.
 * @param cpu Instance CPU (v pauze, mimo @ref z80_execute).
 */
static void snap_z80_save_hidden(snapshot_xml_writer_t *w, const z80_t *cpu)
{
    snapshot_xml_open_element(w, SNAP_Z80_HIDDEN_ELEMENT);
    snapshot_xml_write_hex16(w, "WZ", cpu->wz.w);
    snapshot_xml_write_hex8(w, "Q", cpu->q);
    snapshot_xml_write_bool(w, "ei_delay", cpu->ei_delay);
    snapshot_xml_write_bool(w, "int_pending", cpu->int_pending);
    snapshot_xml_write_bool(w, "nmi_pending", cpu->nmi_pending);
    snapshot_xml_write_hex8(w, "int_vector", cpu->int_vector);
    snapshot_xml_write_bool(w, "ld_a_ir", cpu->ld_a_ir);
    snapshot_xml_write_uint(w, "total_cycles", (unsigned)cpu->total_cycles);
    snapshot_xml_close_element(w);
}

/**
 * @brief Načte skrytý stav Z80 z prvku `hidden_state`.
 *
 * Pole, které v prvku chybí (nebo chybí celý prvek, snapshot ze starší
 * verze), dostane výchozí hodnotu resetu jako v @ref z80_reset: WZ = 0,
 * Q = 0, nic nečeká, vektor 0. `total_cycles` zůstane v takovém případě
 * ze stroje (reset by posunul emulovaný čas CPU proti GDG, který snapshot
 * obnovuje zvlášť).
 *
 * @param r   XML reader uvnitř prvku `z80_state`.
 * @param cpu Instance CPU (v pauze, mimo @ref z80_execute).
 *
 * @post Skrytý stav CPU nezávisí na stavu stroje před načtením (kromě
 *       `total_cycles` u snapshotu bez tohoto pole).
 * @note Bez prvku vypíše varování na stderr.
 */
static void snap_z80_load_hidden(snapshot_xml_reader_t *r, z80_t *cpu)
{
    bool present = snapshot_xml_enter_element(r, SNAP_Z80_HIDDEN_ELEMENT);
    uint16_t wz = 0;
    uint8_t q = 0, int_vector = 0;
    bool ei_delay = false, int_pending = false, nmi_pending = false, ld_a_ir = false;
    unsigned total_cycles = 0;
    bool have_total_cycles = false;

    if (present) {
        snapshot_xml_read_hex16(r, "WZ", &wz);
        snapshot_xml_read_hex8(r, "Q", &q);
        snapshot_xml_read_bool(r, "ei_delay", &ei_delay);
        snapshot_xml_read_bool(r, "int_pending", &int_pending);
        snapshot_xml_read_bool(r, "nmi_pending", &nmi_pending);
        snapshot_xml_read_hex8(r, "int_vector", &int_vector);
        snapshot_xml_read_bool(r, "ld_a_ir", &ld_a_ir);
        have_total_cycles = snapshot_xml_read_uint(r, "total_cycles", &total_cycles);
        snapshot_xml_leave_element(r);
    } else {
        SNAP_WARN("z80", "no %s in cpu/z80.xml (older snapshot), "
                  "hidden CPU state set to reset defaults", SNAP_Z80_HIDDEN_ELEMENT);
    }

    cpu->wz.w = wz;
    cpu->q = q;
    cpu->ei_delay = ei_delay;
    cpu->int_pending = int_pending;
    cpu->nmi_pending = nmi_pending;
    cpu->int_vector = int_vector;
    cpu->ld_a_ir = ld_a_ir;
    if (have_total_cycles) cpu->total_cycles = (uint32_t)total_cycles;
}

/**
 * @brief Uloží stav Z80 do `cpu/z80.xml` (registry + skrytý stav).
 *
 * @param ctx Kontext snapshotu (zápis).
 * @return @ref SNAPSHOT_OK nebo chyba zápisu XML do archivu.
 * @pre Emulátor je v pauze nebo v safe-pointu snapshotu (mezi instrukcemi).
 */
static en_SNAPSHOT_RESULT snap_z80_save(st_SNAPSHOT_CONTEXT *ctx)
{
    z80_t *cpu = g_mzarch_main.cpu;

    snapshot_xml_writer_t *w = snapshot_xml_writer_new();
    snapshot_xml_write_header(w);

    snapshot_xml_open_element(w, "z80_state");

    /* Hlavní registry */
    snapshot_xml_open_element(w, "registers");
    snapshot_xml_write_hex16(w, "AF", (uint16_t)z80_get_reg(cpu, Z80_REG_AF));
    snapshot_xml_write_hex16(w, "BC", (uint16_t)z80_get_reg(cpu, Z80_REG_BC));
    snapshot_xml_write_hex16(w, "DE", (uint16_t)z80_get_reg(cpu, Z80_REG_DE));
    snapshot_xml_write_hex16(w, "HL", (uint16_t)z80_get_reg(cpu, Z80_REG_HL));
    snapshot_xml_close_element(w);

    /* Alternativní registry */
    snapshot_xml_open_element(w, "alt_registers");
    snapshot_xml_write_hex16(w, "AF_", (uint16_t)z80_get_reg(cpu, Z80_REG_AF2));
    snapshot_xml_write_hex16(w, "BC_", (uint16_t)z80_get_reg(cpu, Z80_REG_BC2));
    snapshot_xml_write_hex16(w, "DE_", (uint16_t)z80_get_reg(cpu, Z80_REG_DE2));
    snapshot_xml_write_hex16(w, "HL_", (uint16_t)z80_get_reg(cpu, Z80_REG_HL2));
    snapshot_xml_close_element(w);

    /* Indexové, PC, SP registry */
    snapshot_xml_write_hex16(w, "IX", (uint16_t)z80_get_reg(cpu, Z80_REG_IX));
    snapshot_xml_write_hex16(w, "IY", (uint16_t)z80_get_reg(cpu, Z80_REG_IY));
    snapshot_xml_write_hex16(w, "PC", (uint16_t)z80_get_reg(cpu, Z80_REG_PC));
    snapshot_xml_write_hex16(w, "SP", (uint16_t)z80_get_reg(cpu, Z80_REG_SP));

    /* I, R, R7 registry */
    snapshot_xml_write_hex8(w, "I", (uint8_t)cpu->i);
    snapshot_xml_write_hex8(w, "R", (uint8_t)(cpu->r & 0x7F));
    snapshot_xml_write_hex8(w, "R7", (uint8_t)(cpu->r & 0x80));

    /* Přerušovací mód a flagy */
    snapshot_xml_write_int(w, "interrupt_mode", (int)cpu->im);
    snapshot_xml_write_bool(w, "IFF1", cpu->iff1 != 0);
    snapshot_xml_write_bool(w, "IFF2", cpu->iff2 != 0);
    snapshot_xml_write_bool(w, "halted", cpu->halted != 0);

    /* Skrytý stav jádra mezi instrukcemi */
    snap_z80_save_hidden(w, cpu);

    snapshot_xml_close_element(w); /* z80_state */

    char *xml = snapshot_xml_writer_finish(w);
    en_SNAPSHOT_RESULT res = snapshot_io_write_xml(ctx->io, "cpu/z80.xml", xml);
    g_free(xml);

    return res;
}

/**
 * @brief Načte stav Z80 z `cpu/z80.xml` (registry + skrytý stav).
 *
 * Chybějící registr nechá hodnotu stroje (dosavadní chování), skrytý stav
 * se nastavuje vždy (viz @ref snap_z80_load_hidden).
 *
 * @param ctx Kontext snapshotu (čtení).
 * @return @ref SNAPSHOT_OK; chyba čtení archivu, nebo
 *         @ref SNAPSHOT_ERR_XML_PARSE při nečitelném XML či chybějícím
 *         prvku `z80_state`.
 * @pre Emulátor je v pauze (mimo @ref z80_execute).
 */
static en_SNAPSHOT_RESULT snap_z80_load(st_SNAPSHOT_CONTEXT *ctx)
{
    char *xml = NULL;
    en_SNAPSHOT_RESULT res = snapshot_io_read_xml(ctx->io, "cpu/z80.xml", &xml);
    if (res != SNAPSHOT_OK) {
        SNAP_ERR("z80", "Cannot load cpu/z80.xml");
        return res;
    }

    snapshot_xml_reader_t *r = snapshot_xml_reader_new(xml);
    g_free(xml);

    if (!r) {
        SNAP_ERR("z80", "Parse error in cpu/z80.xml");
        return SNAPSHOT_ERR_XML_PARSE;
    }

    z80_t *cpu = g_mzarch_main.cpu;

    if (!snapshot_xml_enter_element(r, "z80_state")) {
        SNAP_ERR("z80", "Missing element z80_state");
        snapshot_xml_reader_free(r);
        return SNAPSHOT_ERR_XML_PARSE;
    }

    /* Hlavní registry */
    if (snapshot_xml_enter_element(r, "registers")) {
        uint16_t val;
        if (snapshot_xml_read_hex16(r, "AF", &val)) z80_set_reg(cpu, Z80_REG_AF, val);
        if (snapshot_xml_read_hex16(r, "BC", &val)) z80_set_reg(cpu, Z80_REG_BC, val);
        if (snapshot_xml_read_hex16(r, "DE", &val)) z80_set_reg(cpu, Z80_REG_DE, val);
        if (snapshot_xml_read_hex16(r, "HL", &val)) z80_set_reg(cpu, Z80_REG_HL, val);
        snapshot_xml_leave_element(r);
    }

    /* Alternativní registry */
    if (snapshot_xml_enter_element(r, "alt_registers")) {
        uint16_t val;
        if (snapshot_xml_read_hex16(r, "AF_", &val)) z80_set_reg(cpu, Z80_REG_AF2, val);
        if (snapshot_xml_read_hex16(r, "BC_", &val)) z80_set_reg(cpu, Z80_REG_BC2, val);
        if (snapshot_xml_read_hex16(r, "DE_", &val)) z80_set_reg(cpu, Z80_REG_DE2, val);
        if (snapshot_xml_read_hex16(r, "HL_", &val)) z80_set_reg(cpu, Z80_REG_HL2, val);
        snapshot_xml_leave_element(r);
    }

    /* Indexové, PC, SP registry */
    {
        uint16_t val;
        if (snapshot_xml_read_hex16(r, "IX", &val)) z80_set_reg(cpu, Z80_REG_IX, val);
        if (snapshot_xml_read_hex16(r, "IY", &val)) z80_set_reg(cpu, Z80_REG_IY, val);
        if (snapshot_xml_read_hex16(r, "PC", &val)) z80_set_reg(cpu, Z80_REG_PC, val);
        if (snapshot_xml_read_hex16(r, "SP", &val)) z80_set_reg(cpu, Z80_REG_SP, val);
    }

    /* I, R, R7 registry */
    {
        uint8_t val8;
        if (snapshot_xml_read_hex8(r, "I", &val8)) cpu->i = val8;
        if (snapshot_xml_read_hex8(r, "R", &val8)) cpu->r = (val8 & 0x7F) | (cpu->r & 0x80);
        if (snapshot_xml_read_hex8(r, "R7", &val8)) cpu->r = (cpu->r & 0x7F) | (val8 & 0x80);
    }

    /* Přerušovací mód a flagy */
    {
        int im;
        bool bval;
        if (snapshot_xml_read_int(r, "interrupt_mode", &im)) cpu->im = (uint8_t)im;
        if (snapshot_xml_read_bool(r, "IFF1", &bval)) cpu->iff1 = bval ? 1 : 0;
        if (snapshot_xml_read_bool(r, "IFF2", &bval)) cpu->iff2 = bval ? 1 : 0;
        /* HALT stav. Během HALT drží z80 jádro PC = X = adresa instrukce
         * HALT (op_76 v z80_execute.inc dělá rPC-- zpět na X), takže obnova
         * halted=1 spolu s načteným PC=X reprodukuje přesně stav, který CPU
         * drží za běhu během HALT (čeká na IRQ na adrese HALT). Dříve se
         * halted vůbec nečetl - CPU sice HALT re-fetchnul z PC a vrátil se
         * do něj (funkčně skoro stejné), ale obnova je HW-věrná a bez extra
         * instrukčního cyklu. */
        if (snapshot_xml_read_bool(r, "halted", &bval)) cpu->halted = bval ? 1 : 0;
    }

    /* Skrytý stav jádra (starší snapshot bez něj = výchozí hodnoty resetu) */
    snap_z80_load_hidden(r, cpu);

    snapshot_xml_leave_element(r); /* z80_state */
    snapshot_xml_reader_free(r);

    return SNAPSHOT_OK;
}

/**
 * @brief Zaregistruje povinnou komponentu snapshotu "z80" (priorita CPU).
 */
void snap_z80_register(void)
{
    snapshot_register_component("z80",
                                SNAPSHOT_PRIORITY_CPU,
                                snap_z80_save,
                                snap_z80_load,
                                false);
}
