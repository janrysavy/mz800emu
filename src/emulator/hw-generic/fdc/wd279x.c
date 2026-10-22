/**
 * @file wd279x.c
 * @brief Nová implementace WD279x - vrstva chip (true-bus), fáze B.
 *
 * Implementuje:
 *  - Type I příkazy (RESTORE, SEEK, STEP, STEP-IN, STEP-OUT)
 *  - Type II příkazy (READ SECTOR, WRITE SECTOR)
 *  - Type III: READ ADDRESS (READ TRACK, WRITE TRACK = stub NOT_READY)
 *  - Type IV: FORCE INTERRUPT (0xD0 terminace, 0xD8 sticky INTRQ)
 *  - Status registr s command-type specific sémantikou (Type I vs II/III)
 *  - Live bity v Type I: TRACK0 (S2) podle regTRACK==0, INDEX (S1) osciluje
 *  - Multiblock R/W (přechod na další sektor po posledním bajtu)
 *  - 5x čtení STATUS bez DATA = simulovaný konec sektoru ("BASIC verify")
 *
 * ## Port-offset mapping
 *
 * | offset | port  | název    | význam                                  |
 * |--------|-------|----------|-----------------------------------------|
 * |   0    | 0xD8h | CMDSTS   | R: Status, W: Command                   |
 * |   1    | 0xD9h | TRACK    | Track registr                           |
 * |   2    | 0xDAh | SECTOR   | Sector registr                          |
 * |   3    | 0xDBh | DATA     | Data registr                            |
 * |   4    | 0xDCh | MOTOR    | Motor / drive select                    |
 * |   5    | 0xDDh | SIDE     | Side select                             |
 * |   6    | 0xDEh | DENSITY  | Density select                          |
 * |   7    | 0xDFh | EINT     | HD Patch INT enable (Sharp specific)    |
 *
 * ## Timing
 *
 * Fáze B: všechny příkazy proběhnou OKAMŽITĚ. BUSY bit se nikdy nedrží
 * mezi dvěma čteními status registru - tj. ROM uvidí BUSY=0 ihned po
 * zápisu příkazu. To je v rozporu s reálným HW, ale pro funkční smoke
 * test (boot CP/M z DSK) postačí. Reálný timing přichází ve fázi C.
 *
 * License: GPLv3.
 */

#include <stdint.h>
#include <string.h>

#include "wd279x.h"
#include "wd279x_internal.h"
#include "fdc.h"
#include "libs/dsk/dsk.h"
#include "media_writes.h"

/* Konstanty WDST_*, FDCPORT_* a deklarace helperů (current_drive,
 * drive_is_ready, compute_abstrack, set_track_position) jsou sdílené přes
 * wd279x_internal.h - umožňuje split do wd279x_write_track.c
 * a wd279x_read_track.c. */

/* ====================================================================
 * Pomocné funkce - drive / DSK přístup
 * ==================================================================== */

/**
 * @brief Vrátí aktuálně vybranou mechaniku (z MOTOR registru).
 *
 * @param chip ukazatel na chip.
 * @return ukazatel na st_FDDrive nebo NULL pokud chip nemá drives
 *         napojené.
 */
struct st_FDDrive *current_drive(st_WD279X *chip)
{
    if (!chip->drives)
        return NULL;
    unsigned drv_id = chip->MOTOR & 0x03;
    return &chip->drives[drv_id];
}

/**
 * @brief Test zda je aktuálně vybraná mechanika ready (mounted s DSK).
 *
 * Ready = mechanika má namountovaný DSK obraz s platnou geometrií.
 * Pozn.: motor stav (bit 7 MOTOR) zatím neuvažujeme - reálný WD279x
 * vyžaduje běžící motor pro ready=1, ale Sharp ROM motor zapíná těsně
 * před operacemi a očekává okamžitou reakci. Fáze C upraví na timing-
 * aware ready (spin-up delay).
 *
 * @param chip ukazatel na chip.
 * @return 1 = ready, 0 = not ready.
 */
int drive_is_ready(st_WD279X *chip)
{
    struct st_FDDrive *drv = current_drive(chip);
    if (!drv)
        return 0;
    return (drv->mounted && drv->handler_valid && drv->geometry_valid) ? 1 : 0;
}

/**
 * @brief Vypočítá absolutní stopu pro abstrack v Extended DSK formátu.
 *
 * Pořadí: side-interleaved per track - abstrack = track * sides + side.
 * Pro single-sided disketu je side vždy 0 a sides=1, takže abstrack=track.
 *
 * @param drv mechanika (musí mít geometry_valid=1).
 * @param track logická stopa (0..tracks-1).
 * @param side strana (0 nebo 1).
 * @return absolutní stopa (0..total_tracks-1), nebo 0xFF pokud
 *         přesahuje rozsah.
 */
uint8_t compute_abstrack(struct st_FDDrive *drv, uint8_t track, uint8_t side)
{
    if (!drv || !drv->geometry_valid)
        return 0xFF;

    uint8_t sides = drv->geometry.sides ? drv->geometry.sides : 1;
    if (side >= sides)
        return 0xFF;
    if (track >= drv->geometry.tracks)
        return 0xFF;

    return (uint8_t)((track * sides) + side);
}

/**
 * @brief Synchronizuje "drive head position" (positioned_track/side) s chip
 *        registry (regTRACK/SIDE). Reset positioned_sector pokud track/side
 *        došlo ke změně. Volá se před Type II/III commandy.
 *
 * Symetrie s _old_ wd279x_old_set_track() ř. 470: porovnává drive.TRACK
 * s regTRACK a drive.SIDE s FDC->SIDE; pokud se liší, reset drive.SECTOR=0
 * a update cache. Bez tohoto by READ ADDRESS po side-change nesprávně
 * vrátila stale positioned_sector v buffer[1].
 *
 * @return 1 pokud track existuje (= compute_abstrack OK), 0 jinak.
 */
int set_track_position(st_WD279X *chip, struct st_FDDrive *drv)
{
    uint8_t side = chip->SIDE & 0x01;
    if (chip->positioned_track != chip->regTRACK
        || chip->positioned_side != side)
    {
        chip->positioned_sector = 0;
        chip->positioned_track = chip->regTRACK;
        chip->positioned_side = side;
    }
    return (compute_abstrack(drv, chip->regTRACK, side) != 0xFF) ? 1 : 0;
}

/* ====================================================================
 * Status build - sestavení regSTATUS pro Type I s live bity
 * ==================================================================== */

/**
 * @brief Vrátí Type I status s aktualizovanými "live" bity TRACK0 a INDEX.
 *
 * Bit S2 (TRACK0) je nastaven pokud regTRACK == 0.
 * Bit S1 (INDEX) osciluje - jednoduchá implementace: vrátíme každé Nté
 * čtení status registru INDEX=1, jinak 0. Reálný HW: INDEX je aktivní
 * cca 1-5 ms z 200 ms otáčky (300 RPM 5,25") nebo 60 ms (3,5"). Zde
 * jen krátké "ťuknutí" v sekvenci status čtení, aby případný polling
 * software detekoval hranu.
 *
 * @param chip ukazatel na chip.
 * @return složený status registr v Type I sémantice.
 */
static uint8_t build_type1_status(st_WD279X *chip)
{
    uint8_t status = chip->regSTATUS;

    /* Symetrie s _old_: TRACK0 a INDEX nejsou "live" počítané v každém
     * status read - jsou sticky v regSTATUS, nastavované pouze v do_type1()
     * po úspěšném dokončení Type I příkazu. Po Force Interrupt _old_
     * resetuje regSTATUS = 0 a vrací clean status, NIKOLIV recomputovaný
     * TRACK0 z aktuální polohy hlavy. Sharp ROM tohle clean status očekává.
     *
     * Pouze NOT_READY je dynamický (drive readiness se zjišťuje live)
     * a přepisuje sticky hodnotu v regSTATUS.
     *
     * Reference: wd279x_old.c ř. 1594 (default case: `*io_data = ~regSTATUS`
     * bez live computation), ř. 561 (TRACK0 set pouze v do_command po Type I). */
    status &= (uint8_t)~WDST_NOT_READY;

    if (!drive_is_ready(chip))
    {
        /* Drive není ready (žádný DSK obraz). */
        status |= WDST_NOT_READY;
    }

    return status;
}

/* ====================================================================
 * Type I příkazy (Restore, Seek, Step, Step-In, Step-Out)
 * ==================================================================== */

/**
 * @brief Provede Type I příkaz.
 *
 * Změna regTRACK probíhá podle příkazu (RESTORE = 0, SEEK = regDATA,
 * STEP/STEP-IN/STEP-OUT = ±1 podle direction_latch resp. směru).
 * Bit U (update Track, bit 4 příkazu) určuje, zda STEP modifikuje
 * regTRACK; pokud U=0, regTRACK zůstává nezměněn (ale pohyb hlavy se
 * provede - fáze C bude rozlišovat).
 *
 * Status mode po dokončení: Type I, BUSY=0, INTRQ aktivní (sticky).
 *
 * @param chip ukazatel na chip.
 * @param command true-bus hodnota příkazu (0x00..0x7F).
 */
static void do_type1(st_WD279X *chip, uint8_t command)
{
    uint8_t high = (uint8_t)(command >> 4);
    chip->status_mode = WD279X_STATUS_MODE_TYPE_I;
    chip->data_counter = 0;
    chip->buffer_pos = 0;

    if (!drive_is_ready(chip))
    {
        chip->regSTATUS = WDST_NOT_READY;
        chip->intrq_active = 1;
        return;
    }

    /* Reset status, postupně doplníme bity. */
    chip->regSTATUS = 0;

    if (high == 0x00)
    {
        /* RESTORE: hlava na track 0. */
        chip->regTRACK = 0;
        chip->direction_latch = -1;
    }
    else if (high == 0x01)
    {
        /* SEEK: track <- data. */
        if (chip->regDATA > chip->regTRACK)
            chip->direction_latch = +1;
        else if (chip->regDATA < chip->regTRACK)
            chip->direction_latch = -1;
        chip->regTRACK = chip->regDATA;
    }
    else if ((command & 0xE0) == 0x20)
    {
        /* STEP (0x20-0x3F): ve směru direction_latch. */
        uint8_t update = (command & 0x10) ? 1 : 0;
        if (update)
        {
            if (chip->direction_latch >= 0)
            {
                /* Krok dovnitř (+1). */
                if (chip->regTRACK < 0xFF)
                    chip->regTRACK++;
            }
            else
            {
                /* Krok ven (-1). */
                if (chip->regTRACK > 0)
                    chip->regTRACK--;
            }
        }
    }
    else if ((command & 0xE0) == 0x40)
    {
        /* STEP-IN (0x40-0x5F): track +1, direction = +1. */
        chip->direction_latch = +1;
        uint8_t update = (command & 0x10) ? 1 : 0;
        if (update && chip->regTRACK < 0xFF)
            chip->regTRACK++;
    }
    else if ((command & 0xE0) == 0x60)
    {
        /* STEP-OUT (0x60-0x7F): track -1, direction = -1. */
        chip->direction_latch = -1;
        uint8_t update = (command & 0x10) ? 1 : 0;
        if (update && chip->regTRACK > 0)
            chip->regTRACK--;
    }

    /* Pozn.: positioned_sector reset NEděláme zde - dělá to
     * set_track_position() called před Type II/III. To matchuje _old_
     * set_track() ř. 470 který je volaný v Type II/III command path,
     * NE v Type I path. */

    /* Po Type I: BUSY=0, INTRQ aktivní (sticky).
     *
     * HEAD_LOAD bit (S5) NEsetujeme - reálný chip ho propaguje z HLD pin
     * a v Type I status je odraz HLT (head load timing) signálu, který
     * je timing-řízený. _old_ implementace tento bit také neaktivuje
     * (jen TRACK0 pokud regTRACK==0). Sharp ROM po Type I příkazu testuje
     * konkrétní bity statusu a HEAD_LOAD=1 ji vyhodí z očekávaného stavu
     * - viz iorqlog porovnání OLD (status 0xFB = ~0x04) vs NEW
     * (dříve 0xDB = ~0x24). */
    chip->intrq_active = 1;

    /* TRACK0 sticky bit - nastavíme do regSTATUS pokud hlava skončila
     * na stopě 0. Symetrie s _old_ ř. 560-562. Force Interrupt regSTATUS
     * resetuje na 0, takže TRACK0 nepřežije přerušení (záměr - Sharp
     * ROM po FI očekává clean status). */
    if (chip->regTRACK == 0)
    {
        chip->regSTATUS |= WDST_T1_TRACK0;
    }

    /* One-shot BUSY pro první čtení STATUS po Type I. Sharp ROM po
     * vyslání RESTORE/SEEK/STEP očekává sekvenci "first read BUSY=1,
     * next reads BUSY=0 + result" - signál že příkaz reálně proběhl.
     * Bez tohoto se ROM zacyklí (čeká BUSY→0 přechod který nikdy
     * nepřijde). Symetrie s _old_ STATUS_SCRIPT case 1
     * (wd279x_old.c ř. 563 + ř. 1559). */
    chip->pending_busy_status = 1;
}

/* ====================================================================
 * Type II R/W sektor
 * ==================================================================== */

/**
 * @brief Najde geometrii a načte sektor do bufferu (pro READ SECTOR).
 *
 * Použije libs/dsk: získá per-sector velikost přes
 * dsk_read_short_sector_info() a načte sektor přes dsk_rw_sector().
 *
 * @param chip ukazatel na chip.
 * @param drv mechanika (musí být ready).
 * @return 1 = OK (buffer obsahuje data, current_sector_size nastaveno),
 *         0 = chyba (RNF nebo I/O error).
 */
static int load_sector_into_buffer(st_WD279X *chip, struct st_FDDrive *drv)
{
    uint8_t side = chip->SIDE & 0x01;
    uint8_t abstrack = compute_abstrack(drv, chip->regTRACK, side);
    if (abstrack == 0xFF)
    {
#ifdef FDC_DIAG
        fprintf(stderr, "fdc: RNF compute_abstrack(track=%u, side=%u) failed "
                        "(geometry: tracks=%u sides=%u total=%u)\n",
                chip->regTRACK, side,
                drv->geometry.tracks, drv->geometry.sides,
                drv->geometry.total_tracks);
#endif
        return 0;
    }

    /* Zjisti velikost sektoru - bez ní nevíme kolik bajtů číst. */
    uint32_t sec_offset = 0;
    uint16_t ssize_bytes = 0;
    if (EXIT_SUCCESS != dsk_read_short_sector_info(&drv->handler, NULL, NULL,
                                                    abstrack, chip->regSECTOR,
                                                    &sec_offset, &ssize_bytes))
    {
#ifdef FDC_DIAG
        fprintf(stderr, "fdc: RNF dsk_read_short_sector_info(abstrack=%u "
                        "track=%u side=%u sector=0x%02X) failed\n",
                abstrack, chip->regTRACK, side, chip->regSECTOR);
#endif
        return 0;
    }
    if (ssize_bytes == 0 || ssize_bytes > WD279X_BUFFER_SIZE)
    {
#ifdef FDC_DIAG
        fprintf(stderr, "fdc: RNF bad ssize=%u (max %u) abstrack=%u sector=0x%02X\n",
                ssize_bytes, WD279X_BUFFER_SIZE, abstrack, chip->regSECTOR);
#endif
        return 0;
    }

    if (EXIT_SUCCESS != dsk_rw_sector(&drv->handler, DSK_RWOP_READ, NULL, NULL,
                                       abstrack, chip->regSECTOR, chip->buffer))
    {
#ifdef FDC_DIAG
        fprintf(stderr, "fdc: dsk_rw_sector(READ abstrack=%u sector=0x%02X) failed\n",
                abstrack, chip->regSECTOR);
#endif
        return 0;
    }

    chip->current_sector_size = ssize_bytes;
    chip->data_counter = ssize_bytes;
    chip->buffer_pos = 0;

    /* Pozice hlavy = sektor co jsme právě načetli. Symetrie s _old_
     * seek_to_sector ř. 234 (`drive.SECTOR = sector`). */
    chip->positioned_sector = chip->regSECTOR;

#ifdef FDC_DIAG
    /* DIAG trace: loaded sector content (first 8 bytes for identification). */
    fprintf(stderr, "fdc: LOADED abstrack=%u track=%u side=%u sector=0x%02X size=%u "
                    "buffer[0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
            abstrack, chip->regTRACK, side, chip->regSECTOR, ssize_bytes,
            chip->buffer[0], chip->buffer[1], chip->buffer[2], chip->buffer[3],
            chip->buffer[4], chip->buffer[5], chip->buffer[6], chip->buffer[7]);
#endif
    return 1;
}

/**
 * @brief Zapíše obsah bufferu jako sektor do DSK obrazu.
 *
 * Volá se po obdržení posledního bajtu sektoru ve WRITE SECTOR.
 *
 * @param chip ukazatel na chip.
 * @param drv mechanika.
 * @return 1 = OK, 0 = chyba (RNF, WP nebo I/O).
 */
static int flush_buffer_to_sector(st_WD279X *chip, struct st_FDDrive *drv)
{
    if (drv->readonly)
    {
#ifdef FDC_DIAG
        fprintf(stderr, "fdc: WRITE rejected - drive R/O (track=%u side=%u sector=0x%02X)\n",
                chip->regTRACK, chip->SIDE & 0x01, chip->regSECTOR);
#endif
        return 0;
    }

    uint8_t side = chip->SIDE & 0x01;
    uint8_t abstrack = compute_abstrack(drv, chip->regTRACK, side);
    if (abstrack == 0xFF)
    {
#ifdef FDC_DIAG
        fprintf(stderr, "fdc: WRITE compute_abstrack failed (track=%u side=%u sector=0x%02X)\n",
                chip->regTRACK, side, chip->regSECTOR);
#endif
        return 0;
    }

    if (EXIT_SUCCESS != dsk_rw_sector(&drv->handler, DSK_RWOP_WRITE, NULL, NULL,
                                       abstrack, chip->regSECTOR, chip->buffer))
    {
#ifdef FDC_DIAG
        fprintf(stderr, "fdc: dsk_rw_sector(WRITE abstrack=%u sector=0x%02X) failed\n",
                abstrack, chip->regSECTOR);
#endif
        return 0;
    }
    /* Zápis na médium (checkpoint záznamu vstupů, media_writes.h). */
    MEDIA_WRITES_NOTE();
#ifdef FDC_DIAG
    fprintf(stderr, "fdc: FLUSHED abstrack=%u track=%u side=%u sector=0x%02X "
                    "buffer[0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
            abstrack, chip->regTRACK, side, chip->regSECTOR,
            chip->buffer[0], chip->buffer[1], chip->buffer[2], chip->buffer[3],
            chip->buffer[4], chip->buffer[5], chip->buffer[6], chip->buffer[7]);
#endif
    return 1;
}

/**
 * @brief Provede Type II příkaz (READ SECTOR / WRITE SECTOR).
 *
 * @param chip ukazatel na chip.
 * @param command true-bus hodnota příkazu (0x80..0xBF).
 */
static void do_type2(st_WD279X *chip, uint8_t command)
{
    chip->status_mode = WD279X_STATUS_MODE_TYPE_II_III;
    chip->multiblock_rw = (command & 0x10) ? 1 : 0;
    chip->data_counter = 0;
    chip->buffer_pos = 0;
    chip->regSTATUS = 0;

    struct st_FDDrive *drv = current_drive(chip);
    if (!drive_is_ready(chip))
    {
        chip->regSTATUS = WDST_NOT_READY;
        chip->intrq_active = 1;
        return;
    }

    /* set_track_position: sync drive head position s regTRACK/SIDE
     * (= _old_ set_track). Reset positioned_sector pokud došlo k track/side
     * změně. Volá se před Type II/III commandy. */
    if (!set_track_position(chip, drv))
    {
        chip->regSTATUS = WDST_T2_RNF;
        chip->intrq_active = 1;
        return;
    }

    if ((command & 0xE0) == 0x80)
    {
        /* READ SECTOR: načti sektor do bufferu, nastav DRQ + BUSY. */
        if (!load_sector_into_buffer(chip, drv))
        {
            chip->regSTATUS = WDST_T2_RNF;
            chip->intrq_active = 1;
            return;
        }
        chip->regSTATUS = WDST_BUSY | WDST_T2_DRQ;
    }
    else /* 0xA0..0xBF = WRITE SECTOR */
    {
        if (drv->readonly)
        {
            chip->regSTATUS = WDST_T2_WP;
            chip->intrq_active = 1;
            return;
        }

        /* Pro WRITE potřebujeme znát velikost sektoru, abychom věděli
         * kdy je buffer plný. Zjisti přes sector info. */
        uint8_t abstrack = compute_abstrack(drv, chip->regTRACK, chip->SIDE & 0x01);
        uint32_t sec_offset = 0;
        uint16_t ssize_bytes = 0;
        if (abstrack == 0xFF
            || EXIT_SUCCESS != dsk_read_short_sector_info(&drv->handler, NULL, NULL,
                                                          abstrack, chip->regSECTOR,
                                                          &sec_offset, &ssize_bytes)
            || ssize_bytes == 0
            || ssize_bytes > WD279X_BUFFER_SIZE)
        {
            chip->regSTATUS = WDST_T2_RNF;
            chip->intrq_active = 1;
            return;
        }
        chip->current_sector_size = ssize_bytes;
        chip->data_counter = ssize_bytes;
        chip->buffer_pos = 0;
        chip->regSTATUS = WDST_BUSY | WDST_T2_DRQ;
    }
}

/* ====================================================================
 * Type III - READ ADDRESS, READ TRACK, WRITE TRACK
 * ==================================================================== */

/**
 * @brief Provede READ ADDRESS (0xC0..0xCF).
 *
 * Vrátí 6 bajtů ID pole posledního sektoru pod hlavou: Track, Side,
 * Sector, ssize, CRC1, CRC2. Pozn.: WD279x quirk - Track z ID pole se
 * uloží do regSECTOR (ne regTRACK!).
 *
 * Pro emulaci: vybereme první sektor stopy (nemáme rotaci simulovanou).
 *
 * @param chip ukazatel na chip.
 */
static void do_read_address(st_WD279X *chip)
{
    chip->status_mode = WD279X_STATUS_MODE_TYPE_II_III;
    chip->data_counter = 0;
    chip->buffer_pos = 0;
    chip->regSTATUS = 0;

    struct st_FDDrive *drv = current_drive(chip);
    if (!drive_is_ready(chip))
    {
        chip->regSTATUS = WDST_NOT_READY;
        chip->intrq_active = 1;
        return;
    }

    /* set_track_position: sync drive head position s regTRACK/SIDE.
     * Reset positioned_sector po track/side změně - matchuje _old_
     * set_track() volaný v READ ADDRESS path (ř. 685). */
    if (!set_track_position(chip, drv))
    {
        chip->regSTATUS = WDST_T2_RNF;
        chip->intrq_active = 1;
        return;
    }

    uint8_t abstrack = compute_abstrack(drv, chip->regTRACK, chip->SIDE & 0x01);
    st_DSK_SHORT_TRACK_INFO tinfo;
    if (EXIT_SUCCESS != dsk_read_short_track_info(&drv->handler, NULL, abstrack, &tinfo))
    {
        chip->regSTATUS = WDST_T2_RNF;
        chip->intrq_active = 1;
        return;
    }
    if (tinfo.sectors == 0)
    {
        chip->regSTATUS = WDST_T2_RNF;
        chip->intrq_active = 1;
        return;
    }

    /* Seek-to-first-sector logic (symetrie s _old_ ř. 691-697):
     * Pokud nemáme aktuální pozici hlavy (positioned_sector == 0 = po
     * track change), seekneme na první sektor stopy (sinfo[0]). To
     * mapuje na _old_ `wd279x_old_seek_to_sector(drive_id, 1)`. */
    uint8_t sec_id = chip->positioned_sector;
    uint16_t ssize_bytes = 0;
    if (sec_id == 0)
    {
        sec_id = tinfo.sinfo[0];
        chip->positioned_sector = sec_id;
    }

    /* Najdi velikost sektoru pro positioned_sector. Symetrie s _old_
     * `drive.sector_size` po seek_to_sector. */
    for (int i = 0; i < tinfo.sectors; i++)
    {
        if (tinfo.sinfo[i] == sec_id)
        {
            uint8_t ssize_code = (tinfo.sector_sizes[i] != 0)
                                   ? tinfo.sector_sizes[i] : tinfo.ssize;
            ssize_bytes = dsk_decode_sector_size(ssize_code);
            break;
        }
    }
    if (ssize_bytes == 0)
    {
        ssize_bytes = 256; /* fallback */
    }

    /* Sharp/MB8876A quirk pro READ ADDRESS - symetrie s _old_ ř. 699-707:
     *   regSECTOR = drive.SECTOR (= positioned_sector)
     *   buffer[0] = TRACK
     *   buffer[1] = SECTOR (= positioned_sector, ne první sector ID)
     *   buffer[2] = SIDE
     *   buffer[3] = sector_size / 0x100 (raw blocks count, NE WD279x kód)
     *
     * Datasheet "track → regSECTOR" NEimplementujeme - _old_ ho nemá. */
    chip->regSECTOR = sec_id;
    chip->buffer[0] = chip->regTRACK;
    chip->buffer[1] = sec_id;
    chip->buffer[2] = chip->SIDE & 0x01;
    chip->buffer[3] = (uint8_t)(ssize_bytes / 0x100);
    chip->buffer[4] = 0x00; /* CRC1 - emulace nedělá CRC */
    chip->buffer[5] = 0x00; /* CRC2 */

    chip->data_counter = 6;
    chip->current_sector_size = 6;
    chip->buffer_pos = 0;

    chip->regSTATUS = WDST_BUSY | WDST_T2_DRQ;
}

/* ====================================================================
 * Type IV - FORCE INTERRUPT
 * ==================================================================== */

/**
 * @brief Provede FORCE INTERRUPT (0xD0..0xDF).
 *
 * Ukončí aktuální příkaz, přejde do Type I status mode.
 *  - 0xD0: terminace BEZ INTRQ (datasheet quirk, časté místo bugů).
 *  - 0xD8: okamžitý INTRQ (sticky - nezruší se čtením STATUS).
 *  - 0xD1..0xD7, 0xD9..0xDF: další masky (index pulse, ready transition)
 *    - zjednodušeně: jakákoliv hodnota s bit 3 nastaveným = INTRQ.
 *
 * Direction latch zůstává (přežívá Force Interrupt).
 *
 * @param chip ukazatel na chip.
 * @param command true-bus hodnota příkazu (0xD0..0xDF).
 */
static void do_force_interrupt(st_WD279X *chip, uint8_t command)
{
    chip->status_mode = WD279X_STATUS_MODE_TYPE_I;
    chip->data_counter = 0;
    chip->buffer_pos = 0;
    chip->multiblock_rw = 0;
    chip->reading_status_counter = 0;
    chip->pending_busy_status = 0;
    chip->pending_drq = 0;
    chip->pending_rnf_end = 0;
    /* Pozn.: waitForInt NEresetujeme - _old_ Force INT handler ho také
     * nereset (ř. 741-750 NEobsahuje `FDC->waitForInt = 0`). Reset by
     * způsobil race: CP/M dělá Force INT před novým commandem, OLD si
     * pamatuje throttle counter z předchozí sekvence → další INT fire
     * RYCHLEJI; NEW reset → musí čekat dalších 3 IORQs → INT fire pozdě
     * → CPU minul očekávaný IRQ → CP/M 4.1 HD vykolejí. */
    /* Reset regCOMMAND - symetrie s _old_ ř. 747 (FDC->COMMAND = 0x00).
     * Bez resetu by zbylá hodnota (např. WRITE SECTOR 0xA2) mohla aktivovat
     * HD Patch INT path v get_interrupt_state pokud by se data_counter
     * později (např. v jiné operaci) nastavil na > 0. */
    chip->regCOMMAND = 0x00;

    /* Status po Force Interrupt: vyčistíme všechny "operation" bity
     * (BUSY, DRQ, RNF, ...). HEAD_LOAD neseutjeme - viz do_type1. */
    chip->regSTATUS = 0x00;

    /* 0xD0 NEGENERUJE INTRQ. Pro ostatní hodnoty 0xD1..0xDF generuje. */
    if (command == 0xD0)
    {
        /* Sticky INTRQ se ruší přes 0xD0. */
        chip->intrq_active = 0;
    }
    else
    {
        /* 0xD8 a další = sticky INTRQ. */
        chip->intrq_active = 1;
    }
}

/* ====================================================================
 * Command dispatcher
 * ==================================================================== */

/**
 * @brief Rozhodne typ příkazu podle high nibble a zavolá příslušnou
 *        do_typeN() funkci.
 *
 * Vyhodnocení podle datasheet hodnot (true-bus):
 *  - 0x00..0x7F: Type I
 *  - 0x80..0xBF: Type II (READ/WRITE SECTOR)
 *  - 0xC0..0xCF: READ ADDRESS
 *  - 0xD0..0xDF: FORCE INTERRUPT
 *  - 0xE0..0xEF: READ TRACK (stub - vrátí NOT_READY)
 *  - 0xF0..0xFF: WRITE TRACK (formátování stopy)
 *
 * @param chip ukazatel na chip.
 * @param command true-bus hodnota příkazu.
 */
static void dispatch_command(st_WD279X *chip, uint8_t command)
{
    /* Force Interrupt může přerušit běžící příkaz - dej ho jako první. */
    if ((command & 0xF0) == 0xD0)
    {
        do_force_interrupt(chip, command);
        return;
    }

    /* Reset INTRQ při příjmu non-FORCE příkazu (datasheet: zápis Command
     * resetuje INTRQ). Vyčistíme i pending DRQ flag - ten je vázán na
     * předchozí multi-block transition. */
    chip->intrq_active = 0;
    chip->reading_status_counter = 0;
    chip->pending_drq = 0;
    chip->pending_rnf_end = 0;

    if ((command & 0x80) == 0x00)
    {
        /* Type I (0x00..0x7F). */
        do_type1(chip, command);
    }
    else if ((command & 0xC0) == 0x80)
    {
        /* Type II R/W SECTOR (0x80..0xBF). */
        do_type2(chip, command);
    }
    else if ((command & 0xF0) == 0xC0)
    {
        /* READ ADDRESS (0xC0..0xCF). */
        do_read_address(chip);
    }
    else if ((command & 0xF0) == 0xF0)
    {
        /* WRITE TRACK (0xF0..0xFF) - formátování stopy. */
        do_write_track_setup(chip, command);
    }
    else
    {
        /* READ TRACK (0xE0..0xEF) - syntetický raw track stream.
         * Sharp software ho v praxi nepoužívá (FORMAT/COPY/INIT používají
         * WRITE TRACK + READ SECTOR), ale implementujeme pro kompletnost. */
        do_read_track_setup(chip, command);
    }
}

/* ====================================================================
 * Lifecycle
 * ==================================================================== */

void wd279x_init(st_WD279X *chip)
{
    if (!chip)
        return;
    struct st_FDDrive *saved_drives = chip->drives;
    const int *saved_hd_patch_ref = chip->hd_patch_ref;
    memset(chip, 0, sizeof(*chip));
    chip->drives = saved_drives; /* attach se volá zvlášť, init je idempotentní. */
    chip->hd_patch_ref = saved_hd_patch_ref; /* HW config ref přežívá init (jako drives). */
    wd279x_reset(chip);
}

void wd279x_reset(st_WD279X *chip)
{
    if (!chip)
        return;

    /* Reset registrů na poweron hodnoty (true-bus).
     * regSTATUS = 0: NOT_READY není sticky bit v regSTATUS, počítá se
     * live v build_type1_status() podle drive_is_ready(). */
    chip->regSTATUS = 0x00;
    chip->regCOMMAND = 0x00;
    chip->regTRACK = 0x00;
    chip->regSECTOR = 0x00;
    chip->regDATA = 0x00;

    chip->MOTOR = 0x00;
    chip->SIDE = 0x00;
    chip->DENSITY = 0x00;
    chip->EINT = 0x00;

    chip->buffer_pos = 0;
    chip->data_counter = 0;
    chip->current_sector_size = 0;

    chip->status_mode = WD279X_STATUS_MODE_TYPE_I;
    chip->multiblock_rw = 0;
    chip->direction_latch = +1; /* default po power-on: step-in. */
    chip->intrq_active = 0;
    chip->reading_status_counter = 0;
    chip->pending_busy_status = 0;
    chip->pending_drq = 0;
    chip->pending_rnf_end = 0;
    chip->waitForInt = 0;
    chip->positioned_sector = 0;
    chip->positioned_track = 0;
    chip->positioned_side = 0;

    chip->write_track_stage = 0;
    chip->write_track_counter = 0;

    chip->rt_sectors = 0;
    chip->rt_sector_bytes = 0;
    chip->rt_ssize_code = 0;
    chip->rt_cached_sec_idx = -1;
    memset(chip->rt_sinfo, 0, sizeof(chip->rt_sinfo));

    /* buffer obsah zachováváme - reset neresetuje datovou paměť. */
    /* drives pointer zachováme - attach je nezávislý na resetu. */
}

void wd279x_attach_drives(st_WD279X *chip, struct st_FDDrive *drives)
{
    if (!chip)
        return;
    chip->drives = drives;
}

/* ====================================================================
 * I/O handlers
 * ==================================================================== */

int wd279x_read_byte(st_WD279X *chip, int i_addroffset, uint8_t *io_data)
{
    if (!chip || !io_data)
        return 0;

    en_FDCPORT_OFFSET off = (en_FDCPORT_OFFSET)(i_addroffset & 0x07);

    switch (off)
    {
    case FDCPORT_CMDSTS:
    {
        /* Status read. Reset INTRQ (kromě sticky 0xD8 - tam zůstává). */

        uint8_t status;
        if (chip->status_mode == WD279X_STATUS_MODE_TYPE_I)
        {
            status = build_type1_status(chip);
        }
        else
        {
            /* Type II/III: vrátíme regSTATUS jak je. */
            status = chip->regSTATUS;
        }

        /* One-shot BUSY signaling po Type I příkazu - symetrie s _old_
         * STATUS_SCRIPT case 1. Sharp ROM po RESTORE/SEEK/STEP očekává
         * že první čtení status vrátí BUSY=1 (potvrzení že příkaz běží),
         * další čtení vrátí real status. Bez tohoto by ROM viděla BUSY=0
         * hned a interpretovala by to jako "command nezačal". */
        if (chip->pending_busy_status)
        {
            status |= WDST_BUSY;
            chip->pending_busy_status = 0;
        }

        /* One-shot "BUSY only" pro Type II multi-block sector transition.
         * Tento status read vrátí status jak je (= BUSY bez DRQ - byl tak
         * nastaven v DATA handleru), DALŠÍ status read uvidí BUSY+DRQ.
         * Symetrie s _old_ STATUS_SCRIPT case 2. Kritické pro CP/M 4.1 HD. */
        if (chip->pending_drq)
        {
            chip->pending_drq = 0;
            chip->regSTATUS |= WDST_T2_DRQ;
        }

        /* One-shot konec stopy v multi-block R/W: toto čtení vrátí
         * BUSY+RNF (0x11, už nastaveno v regSTATUS), všechna další čtení
         * uvidí čistý 0x00. Symetrie s _old_ STATUS_SCRIPT case 4. */
        if (chip->pending_rnf_end)
        {
            chip->pending_rnf_end = 0;
            chip->regSTATUS = 0x00;
        }

        *io_data = status;

        /* "10x čtení STATUS bez čtení DATA" pravidlo - simuluje konec
         * sektoru. Symetrie s _old_ wd279x_old.c ř. 1481:
         *   `if ((DATA_COUNTER == sector_size) && (COMMAND>>5 == 0x03))`
         *
         * Spouští se POUZE při startu READ SECTOR (data_counter ještě
         * == sector_size, nic nepřečteno) a POUZE pro READ SECTOR command
         * (true-bus 0x80..0x9F → high nibble 0x8/0x9 → >>5 == 0x04).
         *
         * MZ-800 BASIC po zápisu provádí ověření čtením sektoru v multi-
         * blokovém režimu, ale samotná data nečte a sleduje jen status -
         * timeout přepne na další sektor. cp/m 4.1 format4.com totéž
         * v single-bloku.
         *
         * Pozor: dříve NEW měl širší podmínku (= jakýkoli Type II/III
         * s DRQ) → fire mid-sector / pro WRITE → CP/M state corruption. */
        if (chip->data_counter == chip->current_sector_size
            && chip->current_sector_size > 0
            && (chip->regCOMMAND >> 5) == 0x04 /* READ SECTOR 0x80..0x9F */)
        {
            chip->reading_status_counter++;
            if (chip->reading_status_counter > 10)
            {
                chip->reading_status_counter = 0;
                /* Předčasné ukončení sektoru. */
                if (chip->multiblock_rw)
                {
                    /* Přechod na další sektor jako v multiblock R/W. */
                    chip->regSECTOR++;
                    struct st_FDDrive *drv = current_drive(chip);
                    if (!drv || !load_sector_into_buffer(chip, drv))
                    {
                        /* Konec stopy - přechodný RNF (1x BUSY+RNF, pak 0x00).
                         * Symetrie s _old_ STATUS_SCRIPT case 4. Viz též
                         * stejná cesta v DATA handleru a pole pending_rnf_end. */
                        chip->regSECTOR--;
                        chip->regSTATUS = WDST_BUSY | WDST_T2_RNF;
                        chip->pending_rnf_end = 1;
                        chip->intrq_active = 1;
                    }
                    else
                    {
                        chip->regSTATUS = WDST_BUSY | WDST_T2_DRQ;
                    }
                }
                else
                {
                    chip->regSTATUS = 0;
                    chip->data_counter = 0;
                    chip->intrq_active = 1;
                }
            }
        }
        /* Pozn.: counter se inkrementuje POUZE v conditional path
         * (start READ SECTOR + cmd match). Symetrie s _old_ ř. 1483
         * (kde increment je uvnitř `if ((DATA_COUNTER == sector_size)
         * && (COMMAND >> 5 == 0x03))`). Bez tohoto NEW akumuloval counter
         * i mid-sector → při následném sector startu premature transition. */
        break;
    }

    case FDCPORT_TRACK:
        /* Track read - reset HD Patch INT throttle (CPU obsluhuje request).
         * Symetrie s _old_ ř. 1347. */
        chip->waitForInt = 0;
        *io_data = chip->regTRACK;
        break;

    case FDCPORT_SECTOR:
        *io_data = chip->regSECTOR;
        break;

    case FDCPORT_DATA:
    {
        /* Data read. V běhu Type II READ vrací byty ze bufferu;
         * jinak echo regDATA. Reset HD Patch INT throttle (CPU obsluhuje
         * request). Symetrie s _old_ ř. 1679. */
        chip->reading_status_counter = 0;
        chip->waitForInt = 0;

        /* READ TRACK (0xE0..0xEF) - syntetizovaný byte stream z rt_synth_byte. */
        if ((chip->regCOMMAND & 0xF0) == 0xE0
            && chip->data_counter > 0)
        {
            uint8_t b = rt_synth_byte(chip);
            *io_data = b;
            chip->regDATA = b;
            chip->buffer_pos++;
            chip->data_counter--;
            if (chip->data_counter == 0)
            {
                /* End of track stream. */
                chip->regSTATUS = 0;
                chip->intrq_active = 1;
            }
            else
            {
                chip->regSTATUS = WDST_BUSY | WDST_T2_DRQ;
            }
            break;
        }

        if (chip->status_mode == WD279X_STATUS_MODE_TYPE_II_III
            && chip->data_counter > 0)
        {
            *io_data = chip->buffer[chip->buffer_pos];
            chip->regDATA = chip->buffer[chip->buffer_pos];
            chip->buffer_pos++;
            chip->data_counter--;

            if (chip->data_counter == 0)
            {
                /* Konec sektoru. */
                if (chip->multiblock_rw)
                {
                    /* Přechod na další sektor. */
                    chip->regSECTOR++;
                    struct st_FDDrive *drv = current_drive(chip);
                    if (!drv || !load_sector_into_buffer(chip, drv))
                    {
                        /* Konec stopy - další sektor v pořadí neexistuje.
                         * Vrať RNF PŘECHODNĚ: 1x BUSY+RNF (0x11), pak 0x00.
                         * Symetrie s _old_ STATUS_SCRIPT case 4. Trvalý RNF
                         * (BUSY=0, sticky) by MZ-800 disk BASIC interpretoval
                         * jako "command complete + Record Not Found" =
                         * FD1:Unformat error při čtení adresáře multi-blokem. */
                        chip->regSECTOR--;
                        chip->regSTATUS = WDST_BUSY | WDST_T2_RNF;
                        chip->pending_rnf_end = 1;
                        chip->intrq_active = 1;
                    }
                    else
                    {
                        /* Multi-block sector transition: BUSY only první
                         * status read, pak BUSY+DRQ. Symetrie s _old_
                         * STATUS_SCRIPT case 2. Bez toho CP/M 4.1 HD
                         * boot se vykolejí hned na začátku. */
                        chip->regSTATUS = WDST_BUSY;
                        chip->pending_drq = 1;
                    }
                }
                else
                {
                    /* Single sector - konec přenosu. */
                    chip->regSTATUS = 0;
                    chip->intrq_active = 1;
                }
            }
            else
            {
                /* Stále data k odběru. */
                chip->regSTATUS = WDST_BUSY | WDST_T2_DRQ;
            }
        }
        else
        {
            /* Echo posledního zápisu. */
            *io_data = chip->regDATA;
        }
        break;
    }

    case FDCPORT_MOTOR:
    case FDCPORT_SIDE:
    case FDCPORT_DENSITY:
    case FDCPORT_EINT:
        /* Latch ports - return floating bus 0xFF.
         * [neověřeno] - WD279x datasheet popisuje pouze 4 registry. */
        *io_data = 0xFF;
        break;

    default:
        *io_data = 0xFF;
        break;
    }

    return 0;
}

int wd279x_write_byte(st_WD279X *chip, int i_addroffset, uint8_t *io_data)
{
    if (!chip || !io_data)
        return 0;

    en_FDCPORT_OFFSET off = (en_FDCPORT_OFFSET)(i_addroffset & 0x07);

    switch (off)
    {
    case FDCPORT_CMDSTS:
        /* Command write - reset HD Patch INT throttle (CPU právě
         * obslouží request), pak dispatch. Symetrie s _old_ ř. 1288. */
        chip->waitForInt = 0;
        chip->regCOMMAND = *io_data;
        dispatch_command(chip, *io_data);
        break;

    case FDCPORT_TRACK:
        /* CP/M 1.4 quirk: CP/M testuje aktuální stopu tak, že do Track
         * registru zapíše 0x00 a hned ho čte zpět - očekává, že tam
         * uvidí aktuální stopu mechaniky. Pokud bychom zápis 0x00
         * akceptovali, CP/M by Track 0x00 viděla jako "stopa 0" a
         * neustále seekovala. Sharp ROM tento test obchází tak, že
         * zápis cílového Track Reg = 0 zakážeme.
         *
         * V _new_ chipu pracujeme s true-bus hodnotami (BUS xlate
         * invertuje offsety 0..3), takže true 0x00 odpovídá zápisu
         * 0xFF na fyzické sběrnici - což je přesně sekvence kterou
         * CP/M 1.4 generuje.
         *
         * Reference: wd279x_old.c řádky 1308-1325, knowledge base
         * 16-floppy.md. Symetrie s _old_ filtrem
         * `if (*io_data != 0xff)`. */
        if (*io_data == 0x00)
        {
            /* CP/M 1.4 quirk - zápis ignorován, regTRACK beze změny. */
            break;
        }
        chip->regTRACK = *io_data;
        break;

    case FDCPORT_SECTOR:
        chip->regSECTOR = *io_data;
        break;

    case FDCPORT_DATA:
    {
        chip->reading_status_counter = 0;
        /* DATA write - reset HD Patch INT throttle. Symetrie s _old_
         * ř. 1347-1351: deaktivace /INT po obsluze data byte. Bez tohoto
         * waitForInt zůstane > 2, INT fire HNED po EI → CP/M ISR loop
         * trapuje (race condition popisovaná uživatelem). */
        chip->waitForInt = 0;

        /* Pokud běží WRITE TRACK (formátování), předáme byte state machine.
         * Symetrie s _old_ ř. 1357-1359 (FDC->COMMAND == 0x0f || 0x0b). */
        if ((chip->regCOMMAND & 0xF0) == 0xF0)
        {
            do_write_track_byte(chip, *io_data);
            break;
        }

        /* Pokud běží Type II WRITE SECTOR, routuj do bufferu. */
        if (chip->status_mode == WD279X_STATUS_MODE_TYPE_II_III
            && chip->data_counter > 0
            && (chip->regCOMMAND & 0xE0) == 0xA0) /* WRITE SECTOR 0xA0..0xBF */
        {
            chip->buffer[chip->buffer_pos] = *io_data;
            chip->buffer_pos++;
            chip->data_counter--;

            if (chip->data_counter == 0)
            {
                /* Buffer plný - zapiš sektor. */
                struct st_FDDrive *drv = current_drive(chip);
                if (!drv || !flush_buffer_to_sector(chip, drv))
                {
                    chip->regSTATUS = WDST_T2_RNF;
                    chip->intrq_active = 1;
                }
                else if (chip->multiblock_rw)
                {
                    /* Další sektor. */
                    chip->regSECTOR++;
                    /* Pro WRITE potřebujeme jen velikost nového sektoru,
                     * data zapíše ROM. */
                    uint8_t abstrack = compute_abstrack(drv, chip->regTRACK, chip->SIDE & 0x01);
                    uint32_t sec_offset = 0;
                    uint16_t ssize_bytes = 0;
                    if (abstrack == 0xFF
                        || EXIT_SUCCESS != dsk_read_short_sector_info(&drv->handler, NULL, NULL,
                                                                       abstrack, chip->regSECTOR,
                                                                       &sec_offset, &ssize_bytes)
                        || ssize_bytes == 0
                        || ssize_bytes > WD279X_BUFFER_SIZE)
                    {
                        chip->regSECTOR--;
                        chip->regSTATUS = WDST_T2_RNF;
                        chip->intrq_active = 1;
                    }
                    else
                    {
                        chip->current_sector_size = ssize_bytes;
                        chip->data_counter = ssize_bytes;
                        chip->buffer_pos = 0;
                        /* Multi-block WRITE sector transition: BUSY only
                         * první status read, pak BUSY+DRQ. Symetrie s READ. */
                        chip->regSTATUS = WDST_BUSY;
                        chip->pending_drq = 1;
                    }
                }
                else
                {
                    chip->regSTATUS = 0;
                    chip->intrq_active = 1;
                }
            }
            else
            {
                chip->regSTATUS = WDST_BUSY | WDST_T2_DRQ;
            }
        }
        else
        {
            /* Mimo aktivní WRITE - jen ulož do regDATA (SEEK target, atd.). */
            chip->regDATA = *io_data;
        }
        break;
    }

    case FDCPORT_MOTOR:
        /* Replikace _old_ chování:
         *  - bit 2 (EDR) = 1 → bity 0,1 (drive ID) se přijmou + bit 7
         *  - bit 2 = 0 → drive ID se nemění, mění se jen motor (bit 7) */
        if (*io_data & 0x04)
        {
            chip->MOTOR = (uint8_t)(*io_data & 0x83);
        }
        else
        {
            if (*io_data & 0x80)
                chip->MOTOR |= 0x80;
            else
                chip->MOTOR &= 0x03;
        }
        break;

    case FDCPORT_SIDE:
        chip->SIDE = *io_data & 0x01;
        break;

    case FDCPORT_DENSITY:
        chip->DENSITY = *io_data & 0x01;
        break;

    case FDCPORT_EINT:
        /* HD Patch obvod buď je nebo není osazen v reálném HW. Pokud
         * hd_patch == 0 (= raw / unpatched řadič) nebo ref není napojen,
         * port 0xDF nemá efekt - chip->EINT zůstává 0, čímž
         * wd279x_get_interrupt_state vždy vrátí 0 (= žádný HD Patch INT
         * nikdy nefire). hd_patch je per-instance (vlastní st_FDC). */
        if (!chip->hd_patch_ref || !*chip->hd_patch_ref)
        {
            chip->EINT = 0;
            chip->intrq_active = 0;
            chip->waitForInt = 0;
            break;
        };
        chip->EINT = *io_data & 0x01;
        /* Pokud HD Patch INT režim zakázán, zruš sticky INTRQ + throttle. */
        if (!chip->EINT)
        {
            chip->intrq_active = 0;
            chip->waitForInt = 0;
        }
        break;

    default:
        break;
    }

    return 0;
}

int wd279x_get_interrupt_state(st_WD279X *chip)
{
    if (!chip)
        return 0;

    /* HD Patch INT logika: aktivuje /INT signál když je EINT režim
     * povolen (port 0xDFh = 1) a chip má pendingí data transfer
     * vyžadující obsluhu CPU.
     *
     * Symetrie s wd279x_old_check_interrupt (wd279x_old.c ř. 1871+):
     *  - bez EINT režimu: žádný INT (waitForInt reset)
     *  - během Type II R/W SECTOR (true-bus 0x80..0xBF) s pending DATA
     *  - během READ ADDRESS (true-bus 0xC0..0xCF) s pending DATA
     *  - během WRITE TRACK 0xF0 / 0xF4 (= _old_ COMMAND 0x0F / 0x0B)
     *
     * Throttling waitForInt: INT se vystaví až po >2 po sobě jdoucích
     * checks - radič nesignalizuje konstantně, ale občasným pulsem.
     * Reset waitForInt se děje při Command write, Track read, Data read
     * a EINT off (viz wd279x_write_byte / wd279x_read_byte). */
    /* HD Patch obvod absent v HW (hd_patch == 0 nebo ref nenapojen) -
     * žádný HD Patch INT nikdy nefire. Tato kontrola je defensive proti
     * race kdy user vypne hd_patch za běhu po předchozím nastavení EINT=1
     * - chip->EINT by zůstal 1 do dalšího reset, INT by stále fire.
     * hd_patch je per-instance (vlastní st_FDC). */
    if (!chip->hd_patch_ref || !*chip->hd_patch_ref)
    {
        return 0;
    };

    if (!chip->EINT)
    {
        /* Pozn.: waitForInt NEresetujeme - musíme být symetrický s _old_
         * (wd279x_old.c ř. 1873 `if (!FDC->EINT) return 0;` bez resetu).
         * Reset by způsobil race: CP/M 4.1 dělá EINT off→on toggle pro
         * reset stavu, OLD si pamatuje waitForInt → další INT fire IHNED,
         * NEW reset → musí čekat dalších 3 IORQs → INT pozdě → CPU
         * pokračuje bez očekávaného IRQ → drift +10900 pxclk → vykolejí
         * CP/M 4.1 HD boot. Reset waitForInt se musí dít EXPLICITNĚ při:
         * WRITE EINT=0, WRITE CMDSTS, READ TRACK, READ DATA - tam to máme. */
        return 0;
    }

    int needs_int = 0;

    if (chip->data_counter)
    {
        uint8_t cmd_high = (uint8_t)(chip->regCOMMAND >> 5);
        /* 100x = READ SECTOR 0x80..0x9F, 101x = WRITE SECTOR 0xA0..0xBF */
        if (cmd_high == 0x04 || cmd_high == 0x05)
        {
            needs_int = 1;
        }
        /* READ ADDRESS 0xC0..0xCF (high nibble == 0xC) */
        else if ((chip->regCOMMAND & 0xF0) == 0xC0)
        {
            needs_int = 1;
        }
        /* READ TRACK 0xE0..0xEF (high nibble == 0xE) - streaming raw bytes,
         * INT pro každý byte ready event jako u Type II. */
        else if ((chip->regCOMMAND & 0xF0) == 0xE0)
        {
            needs_int = 1;
        }
    }

    /* WRITE TRACK varianty - vždy (i bez DATA_COUNTER, ROM se opírá
     * o INT pro obsluhu formátování stopy). */
    if (chip->regCOMMAND == 0xF0 || chip->regCOMMAND == 0xF4)
    {
        needs_int = 1;
    }

    if (needs_int)
    {
        chip->waitForInt++;
        /* Threshold: počet po sobě jdoucích check_interrupt() volání před
         * aktivací /INT. Symetrie s _old_ ř. 1882 (`waitForInt > 2`).
         * CP/M 4.1 HD patch race condition: ROM dělá `EI` PŘED tím, než
         * dokončí instalaci ISR rutiny. Pokud INT fire moc rychle (= před
         * dokončením ISR setup), CP/M crashne. Threshold dává delay. */
        if (chip->waitForInt > 2)
        {
            return 1;
        }
    }

    return 0;
}

/* ========================================================================= */
/* Debug/UI mirror API - side-effect free čtení registrů.                    */
/*                                                                           */
/* Použité v io_catalog.c read_value() callbacích pro IO Ports debug okno.   */
/* Mirror funkce NESMÍ modifikovat žádný field st_WD279X. Konvence:          */
/*  - status: rebuild přes build_type1_status() pro Type I, jinak regSTATUS  */
/*  - track / sector: surová hodnota registru                                */
/*  - data registr NEMÁ mirror - real read posouvá buffer stream             */
/* ========================================================================= */

uint8_t wd279x_mirror_status_get(st_WD279X *chip)
{
    if (chip->status_mode == WD279X_STATUS_MODE_TYPE_I)
    {
        /* build_type1_status() je side-effect free - čte regSTATUS a
         * drive_is_ready() (= jen flagy mounted/handler_valid/geometry_valid
         * z current_drive()). */
        return build_type1_status(chip);
    }
    /* Type II/III: vrátíme regSTATUS jak je (raw). Pending one-shot bity
     * (pending_busy_status, pending_drq) NEjsou aplikované - mirror ukazuje
     * "klidový" stav bez vyvolání side-effect cesty. */
    return chip->regSTATUS;
}

uint8_t wd279x_mirror_track_get(st_WD279X *chip)
{
    return chip->regTRACK;
}

uint8_t wd279x_mirror_sector_get(st_WD279X *chip)
{
    return chip->regSECTOR;
}


/* ========================================================================= */
/* Public API - dekódování command opcode (debugger / hwlog detail).         */
/*                                                                           */
/* Funkce nejsou závislé na žádném chip state (čisté lookup). Volajícím      */
/* je hwlog detail decoder (eventlog_decoder.c) a FDC State debugger okno.   */
/* ========================================================================= */

en_WD279X_COMMAND_TYPE wd279x_decode_command_type(uint8_t cmd)
{
    /* Type IV Force Interrupt má dedikovaný 0xDX rozsah - umístit ho
     * dřív než generický 0xC..0xF rozhodovací strom by stejně fungovalo,
     * ale switch nad top nibble je přehlednější. */
    uint8_t top = (uint8_t)(cmd >> 4);
    switch (top)
    {
    case 0x0: return WD279X_CMD_RESTORE;
    case 0x1: return WD279X_CMD_SEEK;
    case 0x2:
    case 0x3:
        /* Step: bit 4 (= 0x10) rozhoduje update Track registr. */
        return (cmd & 0x10) ? WD279X_CMD_STEP_UPDATE : WD279X_CMD_STEP;
    case 0x4:
    case 0x5:
        return (cmd & 0x10) ? WD279X_CMD_STEP_IN_UPDATE : WD279X_CMD_STEP_IN;
    case 0x6:
    case 0x7:
        return (cmd & 0x10) ? WD279X_CMD_STEP_OUT_UPDATE : WD279X_CMD_STEP_OUT;
    case 0x8:
    case 0x9:
        /* Read Sector single/multi (bit 4) - typ je společný. */
        return WD279X_CMD_READ_SECTOR;
    case 0xA:
    case 0xB:
        return WD279X_CMD_WRITE_SECTOR;
    case 0xC: return WD279X_CMD_READ_ADDRESS;
    case 0xD: return WD279X_CMD_FORCE_INTERRUPT;
    case 0xE: return WD279X_CMD_READ_TRACK;
    case 0xF: return WD279X_CMD_WRITE_TRACK;
    default:  return WD279X_CMD_UNKNOWN;
    }
}


const char *wd279x_command_type_name(en_WD279X_COMMAND_TYPE t)
{
    switch (t)
    {
    case WD279X_CMD_RESTORE:         return "Restore";
    case WD279X_CMD_SEEK:             return "Seek";
    case WD279X_CMD_STEP:             return "Step";
    case WD279X_CMD_STEP_UPDATE:      return "Step+U";
    case WD279X_CMD_STEP_IN:          return "Step In";
    case WD279X_CMD_STEP_IN_UPDATE:   return "Step In+U";
    case WD279X_CMD_STEP_OUT:         return "Step Out";
    case WD279X_CMD_STEP_OUT_UPDATE:  return "Step Out+U";
    case WD279X_CMD_READ_SECTOR:      return "Read Sector";
    case WD279X_CMD_WRITE_SECTOR:     return "Write Sector";
    case WD279X_CMD_READ_ADDRESS:     return "Read Address";
    case WD279X_CMD_READ_TRACK:       return "Read Track";
    case WD279X_CMD_WRITE_TRACK:      return "Write Track";
    case WD279X_CMD_FORCE_INTERRUPT:  return "Force INT";
    case WD279X_CMD_UNKNOWN:
    default:                          return "Unknown";
    }
}
