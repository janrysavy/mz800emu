/* 
 * File:   cmt_mzf.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 19. května 2018, 16:54
 * 
 * 
 * ----------------------------- License -------------------------------------
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 * ---------------------------------------------------------------------------
 */


#ifndef CMT_MZF_H
#define CMT_MZF_H

#ifdef __cplusplus
extern "C" {
#endif

#include "cmtext.h"
#include "libs/mztape/mztape.h"

    extern st_CMTEXT g_cmt_mzf_extension;


/** @brief Formátová varianta, ve které Virtual CMT přehrává MZF a MZT. */
#define CMTMZF_FORMATSET MZTAPE_FORMATSET_MZ800_SANE


    /**
     * @brief Data otevřeného MZF bloku (st_CMTEXT_BLOCK->spec).
     *
     * Invariant: cmtspeed a pulses popisují rychlost, se kterou byl
     * vygenerován stream bloku; pulses je platné jen pro CMTSPEED_CUSTOM.
     * Ownership: hdr a mztmzf vlastní blockspec (cmtmzf_blockspec_destroy).
     */
    typedef struct st_CMTMZF_BLOCKSPEC {
        st_MZF_HEADER *hdr;             /**< hlavička MZF (v nativní endianitě) */
        st_MZTAPE_MZF *mztmzf;          /**< data pro generování streamu */
        en_CMTSPEED cmtspeed;           /**< rychlost streamu: poměr nebo CMTSPEED_CUSTOM */
        st_MZTAPE_PULSES_LENGTH pulses; /**< vlastní délky pulzů (jen pro CMTSPEED_CUSTOM) */
    } st_CMTMZF_BLOCKSPEC;

    extern void cmtmzf_blockspec_destroy ( st_CMTMZF_BLOCKSPEC *blspec );

    /**
     * @brief Načte MZF blok z handleru a připraví jeho blockspec.
     *
     * @param h Handler s MZF/MZT daty.
     * @param offset Offset hlavičky MZF v handleru.
     * @param cmtspeed Rychlost bloku (poměr nebo CMTSPEED_CUSTOM).
     * @param pulses Vlastní délky pulzů; povinné (ne NULL) pro CMTSPEED_CUSTOM,
     *        jinak se ignoruje. Kopíruje se.
     * @return Nový blockspec (vlastník je volající), NULL při chybě.
     */
    extern st_CMTMZF_BLOCKSPEC* cmtmzf_blockspec_new ( st_HANDLER *h, uint32_t offset, en_CMTSPEED cmtspeed, const st_MZTAPE_PULSES_LENGTH *pulses );

    /**
     * @brief Otevře MZF blok pásky a vygeneruje jeho stream.
     *
     * @param h Handler s MZF/MZT daty.
     * @param offset Offset hlavičky MZF v handleru.
     * @param block_id Index bloku v kontejneru.
     * @param pause_after Pauza za blokem.
     * @param block_speed Režim rychlosti bloku (DEFAULT = řídí ji výchozí rychlost Virtual CMT).
     * @param cmtspeed Rychlost streamu (poměr nebo CMTSPEED_CUSTOM).
     * @param pulses Vlastní délky pulzů; povinné pro CMTSPEED_CUSTOM, jinak se ignoruje.
     * @return Nový blok (vlastník je volající, cmtmzf_block_close), NULL při chybě.
     */
    extern st_CMTEXT_BLOCK* cmtmzf_block_open ( st_HANDLER *h, uint32_t offset, int block_id, int pause_after, en_CMTEXT_BLOCK_SPEED block_speed, en_CMTSPEED cmtspeed, const st_MZTAPE_PULSES_LENGTH *pulses );
    extern void cmtmzf_block_close ( st_CMTEXT_BLOCK *block );

    extern st_MZF_HEADER* cmtmzf_block_get_spec_mzfheader ( st_CMTEXT_BLOCK *block );

#ifdef __cplusplus
}
#endif

#endif /* CMT_MZF_H */

