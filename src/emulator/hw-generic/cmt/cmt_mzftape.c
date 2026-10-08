/* 
 * File:   cmt_mzftape.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 11. května 2018, 7:25
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

#include <stdio.h>
#include <string.h>

#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"
#include "libs/generic_driver/generic_driver.h"
#include "generic_driver/memory_driver.h"
#include "baseui/baseui.h"

#include "cmt.h"
#include "cmtext.h"
#include "cmt_mzf.h"
#include "cmt_mzftape.h"

static st_DRIVER *g_driver = &g_memory_driver_static;

char *g_cmt_mzftape_fileext[] = {
                                 "mzt",
                                 NULL
};

st_CMTEXT_INFO g_cmt_mzftape_info = {
                                     "MZT",
                                     g_cmt_mzftape_fileext,
                                     "MZT cmt extension",
                                     CMTEXT_TYPE_PLAYABLE
};

extern st_CMTEXT *g_cmt_mzftape;


static void cmtmzftape_container_close ( st_CMTEXT_CONTAINER *container ) {
    cmtext_container_destroy ( container );
}


static void cmtmzftape_eject ( void ) {
    cmtmzf_block_close ( g_cmt_mzftape->block );
    g_cmt_mzftape->block = (st_CMTEXT_BLOCK*) NULL;
    cmtmzftape_container_close ( g_cmt_mzftape->container );
    g_cmt_mzftape->container = (st_CMTEXT_CONTAINER*) NULL;
}


/**
 * @brief Projde MZT a sestaví index jeho MZF bloků.
 *
 * Hlavičky CMTSPEED zařízení UniCMT (viz mztape_unicmt_speed_marker())
 * se do indexu nedostanou: všem dalším blokům až do další hlavičky
 * nastaví pevnou rychlost (CMTEXT_BLOCK_SPEED_SET) CMTSPEED_CUSTOM
 * s délkami pulzů z hlavičky. Bloky před první hlavičkou mají výchozí
 * rychlost Virtual CMT. Hlavička s nulovou délkou pulzu se zahodí
 * s varováním a rychlost se nezmění.
 *
 * Na HW UniCMT pásku s hlavičkou CMTSPEED jako prvním blokem nepřehraje
 * (pozorováno, příčina neznámá, báze hw/25-unicmt.md kap. 2.1). Emulátor
 * to nenapodobuje, jen to vypíše.
 *
 * @param h Handler s celým MZT v paměti.
 * @param offset Offset první hlavičky.
 * @param cmtspeed Výchozí rychlost uložená do bloků bez hlavičky (informativní;
 *        bloky s výchozí rychlostí ji při otevření berou z g_cmt).
 * @param[out] count_blocks Počet bloků v indexu (bez hlaviček CMTSPEED).
 * @return Nový index (vlastník je volající), NULL při chybě čtení nebo
 *         alokace. Prázdný MZT (nebo jen hlavičky CMTSPEED) vrátí NULL
 *         s *count_blocks == 0.
 */
static st_CMTEXT_TAPE_INDEX* cmtmzftape_container_index_new ( st_HANDLER *h, uint32_t offset, en_CMTSPEED cmtspeed, int *count_blocks ) {

    *count_blocks = 0;

    st_MZF_HEADER *hdr = (st_MZF_HEADER*) baseui_tools_mem_alloc0 ( sizeof ( st_MZF_HEADER ) );
    if ( !hdr ) {
        fprintf ( stderr, "%s():%d - Can't alocate memory (%d)\n", __func__, __LINE__, (int) sizeof ( st_MZF_HEADER ) );
        return NULL;
    };

    st_CMTEXT_TAPE_INDEX *index = NULL;
    int i = 0;

    /* rychlost z poslední hlavičky CMTSPEED (UniCMT) */
    int marker_active = 0;
    st_MZTAPE_PULSES_LENGTH marker_pulses;
    memset ( &marker_pulses, 0x00, sizeof ( marker_pulses ) );

    while ( offset < h->spec.memspec.size ) {

        if ( EXIT_FAILURE == mzf_read_header_on_offset ( h, offset, hdr ) ) {
            fprintf ( stderr, "%s():%d - Can't read MZF header\n", __func__, __LINE__ );
            baseui_tools_mem_free ( hdr );
            cmtext_container_tapeindex_destroy ( index, i );
            return NULL;
        };

        uint8_t raw_hdr[sizeof ( st_MZF_HEADER )];
        if ( EXIT_SUCCESS != generic_driver_read ( h, offset, raw_hdr, sizeof ( raw_hdr ) ) ) {
            fprintf ( stderr, "%s():%d - Can't read MZF header\n", __func__, __LINE__ );
            baseui_tools_mem_free ( hdr );
            cmtext_container_tapeindex_destroy ( index, i );
            return NULL;
        };

        st_MZTAPE_PULSES_LENGTH pulses;
        en_MZTAPE_UNICMT_MARKER marker = mztape_unicmt_speed_marker ( raw_hdr, &pulses );
        if ( marker != MZTAPE_UNICMT_MARKER_NONE ) {
            if ( marker == MZTAPE_UNICMT_MARKER_VALID ) {
                marker_active = 1;
                marker_pulses = pulses;
                printf ( "%s: UniCMT CMTSPEED header at offset 0x%x: pulses %0.0f / %0.0f / %0.0f / %0.0f us (~%0.2f:1) for next blocks\n",
                         cmtext_get_description ( g_cmt_mzftape ), offset,
                         pulses.long_pulse.high * 1e6, pulses.long_pulse.low * 1e6,
                         pulses.short_pulse.high * 1e6, pulses.short_pulse.low * 1e6,
                         mztape_pulses_get_ratio ( CMTMZF_FORMATSET, &pulses ) );
                if ( i == 0 ) {
                    printf ( "%s: note: CMTSPEED header is the first block; the real UniCMT does not play such a tape\n", cmtext_get_description ( g_cmt_mzftape ) );
                };
            } else {
                fprintf ( stderr, "%s: warning: CMTSPEED header at offset 0x%x has a zero pulse length, ignored\n", cmtext_get_description ( g_cmt_mzftape ), offset );
            };
            /* hlavička CMTSPEED nemá tělo (délka 0 je podmínkou rozpoznání) */
            offset += sizeof ( st_MZF_HEADER );
            continue;
        };

        char ascii_filename[MZF_FNAME_FULL_LENGTH];
        mzf_tools_get_fname ( hdr, ascii_filename );

        index = cmtext_container_tape_index_aloc ( index, i );
        if ( !index ) {
            baseui_tools_mem_free ( hdr );
            return NULL;
        };

        st_CMTEXT_TAPE_INDEX *idx = &index[i];

        idx->block_id = i++;
        idx->offset = offset;
        idx->blspeed = ( marker_active ) ? CMTEXT_BLOCK_SPEED_SET : CMTEXT_BLOCK_SPEED_DEFAULT;
        idx->bltype = CMTEXT_BLOCK_TYPE_MZF;
        idx->pause_after = CMTMZFTAPE_DEFAULT_PAUSE_AFTER;

        st_CMTEXT_TAPE_ITEM_MZF *mzfitem = &idx->item.mzf;

        mzfitem->cmtspeed = ( marker_active ) ? CMTSPEED_CUSTOM : cmtspeed;
        mzfitem->pulses = marker_pulses;
        mzfitem->ftype = hdr->ftype;
        mzfitem->fsize = hdr->fsize;
        mzfitem->fstrt = hdr->fstrt;
        mzfitem->fexec = hdr->fexec;

        int name_length = strlen ( ascii_filename ) + 1;
        mzfitem->fname = (char*) baseui_tools_mem_alloc0 ( name_length );
        if ( !mzfitem->fname ) {
            fprintf ( stderr, "%s():%d - Can't alocate memory (%d)\n", __func__, __LINE__, name_length );
            baseui_tools_mem_free ( hdr );
            cmtext_container_tapeindex_destroy ( index, i );
            return NULL;
        };

        strncpy ( mzfitem->fname, ascii_filename, name_length );

        offset += sizeof ( st_MZF_HEADER ) + hdr->fsize;
    };

    if ( i != 0 ) {
        st_CMTEXT_TAPE_INDEX *idx = &index[i - 1];
        idx->pause_after = 0;
    };

    baseui_tools_mem_free ( hdr );

    *count_blocks = i;
    return index;
}


st_CMTEXT_BLOCK* cmtmzftape_block_open ( int block_id ) {

    assert ( g_cmt_mzftape->container != NULL );
    assert ( g_cmt_mzftape->container->tape != NULL );
    assert ( g_cmt_mzftape->container->tape->h != NULL );
    assert ( g_cmt_mzftape->container->tape->index != NULL );

    if ( ( block_id < 0 ) || ( block_id >= g_cmt_mzftape->container->count_blocks ) ) {
        baseui_error ( "%s: Can't open block '%d' (blocks = %d)\n", cmtext_get_description ( g_cmt_mzftape ), block_id, g_cmt_mzftape->container->count_blocks );
        return NULL;
    };

    st_CMTEXT_TAPE_INDEX *idx = &g_cmt_mzftape->container->tape->index[block_id];
    uint32_t offset = idx->offset;
    en_CMTEXT_BLOCK_SPEED blspeed = idx->blspeed;
    st_CMTEXT_TAPE_ITEM_MZF *mzfitem = &idx->item.mzf;
    // pokud neni SET, tak je DEFAULT
    en_CMTSPEED cmtspeed = ( blspeed == CMTEXT_BLOCK_SPEED_SET ) ? mzfitem->cmtspeed : g_cmt.mz_cmtspeed;
    const st_MZTAPE_PULSES_LENGTH *pulses = ( blspeed == CMTEXT_BLOCK_SPEED_SET ) ? &mzfitem->pulses : &g_cmt.mz_custom_pulses;
    uint16_t pause_after = idx->pause_after;

    st_CMTEXT_BLOCK *block = cmtmzf_block_open ( g_cmt_mzftape->container->tape->h, offset, block_id, pause_after, blspeed, cmtspeed, pulses );
    if ( !block ) {
        return NULL;
    };

    return block;
}


static int cmtmzftape_container_next_block ( void ) {
    assert ( g_cmt_mzftape->container != NULL );
    assert ( g_cmt_mzftape->block != NULL );
    int next_block_id = g_cmt_mzftape->block->block_id + 1;
    if ( next_block_id < g_cmt_mzftape->container->count_blocks ) {
        st_CMTEXT_BLOCK *block = cmtmzftape_block_open ( next_block_id );
        if ( !block ) return EXIT_FAILURE;
        cmtmzf_block_close ( g_cmt_mzftape->block );
        g_cmt_mzftape->block = block;
        return EXIT_SUCCESS;
    };
    return EXIT_FAILURE;
}


static int cmtmzftape_container_previous_block ( void ) {
    assert ( g_cmt_mzftape->container != NULL );
    assert ( g_cmt_mzftape->block != NULL );
    if ( g_cmt_mzftape->block->block_id == 0 ) return EXIT_FAILURE;
    int prev_block_id = g_cmt_mzftape->block->block_id - 1;
    st_CMTEXT_BLOCK *block = cmtmzftape_block_open ( prev_block_id );
    if ( !block ) return EXIT_FAILURE;
    cmtmzf_block_close ( g_cmt_mzftape->block );
    g_cmt_mzftape->block = block;
    return EXIT_SUCCESS;
}


static int cmtmzftape_container_open_block ( int block_id ) {
    assert ( g_cmt_mzftape->container != NULL );
    assert ( g_cmt_mzftape->block != NULL );
    st_CMTEXT_BLOCK *block = cmtmzftape_block_open ( block_id );
    if ( !block ) return EXIT_FAILURE;
    cmtmzf_block_close ( g_cmt_mzftape->block );
    g_cmt_mzftape->block = block;
    return EXIT_SUCCESS;
}


static int cmtmzftape_container_open ( char *filename ) {

    cmtmzftape_eject ( );

    printf ( "%s\nOpen: %s\n", cmtext_get_description ( g_cmt_mzftape ), filename );

    st_HANDLER *h = generic_driver_open_memory_from_file ( NULL, g_driver, filename );

    if ( !h ) {
        baseui_error ( "%s: Can't open file '%s'\n", cmtext_get_description ( g_cmt_mzftape ), filename );
        return EXIT_FAILURE;
    };

    int count_blocks = 0;

    st_CMTEXT_TAPE_INDEX *index = cmtmzftape_container_index_new ( h, 0, g_cmt.mz_cmtspeed, &count_blocks );

    /* MZT jen s hlavičkami CMTSPEED (nebo prázdný) nemá žádný blok */
    if ( ( !index ) && ( !count_blocks ) && ( h->spec.memspec.size > 0 ) ) {
        baseui_error ( "%s: No playable block in '%s'\n", cmtext_get_description ( g_cmt_mzftape ), filename );
        generic_driver_close ( h );
        return EXIT_FAILURE;
    };

    if ( !index ) {
        generic_driver_close ( h );
        return EXIT_FAILURE;
    };

    if ( !count_blocks ) {
        baseui_error ( "%s: Empty container '%s'\n", cmtext_get_description ( g_cmt_mzftape ), filename );
        generic_driver_close ( h );
        cmtext_container_tapeindex_destroy ( index, count_blocks );
        return EXIT_FAILURE;
    };

    st_CMTEXT_CONTAINER_TAPE *tape = cmtext_container_tape_new ( h, index );
    if ( !tape ) {
        generic_driver_close ( h );
        cmtext_container_tapeindex_destroy ( index, count_blocks );
        return EXIT_FAILURE;
    };

    st_CMTEXT_CONTAINER *container = cmtext_container_new ( CMTEXT_CONTAINER_TYPE_SIMPLE_TAPE, filename, count_blocks, tape, cmtmzftape_container_next_block, cmtmzftape_container_previous_block, cmtmzftape_container_open_block );
    if ( !container ) {
        cmtext_container_tape_destroy ( tape, count_blocks );
        return EXIT_FAILURE;
    };

    g_cmt_mzftape->container = container;

    st_CMTEXT_BLOCK *block = cmtmzftape_block_open ( 0 );
    if ( !block ) {
        baseui_error ( "%s: Can't open first block\n", cmtext_get_description ( g_cmt_mzftape ) );
        cmtmzftape_container_close ( container );
        g_cmt_mzftape->container = NULL;
        return EXIT_FAILURE;
    };

    g_cmt_mzftape->block = block;

    return EXIT_SUCCESS;
}


static void cmtmzftape_init ( void ) {
    return;
}


static void cmtmzftape_exit ( void ) {
    cmtmzftape_eject ( );
}


st_CMTEXT g_cmt_mzftape_extension = {
                                     &g_cmt_mzftape_info,
                                     (st_CMTEXT_CONTAINER*) NULL,
                                     (st_CMTEXT_BLOCK*) NULL,
                                     cmtmzftape_init,
                                     cmtmzftape_exit,
                                     cmtmzftape_container_open,
                                     (cmtext_cb_stop) NULL,
                                     cmtmzftape_eject,
                                     (cmtext_cb_write_data) NULL,
};

st_CMTEXT *g_cmt_mzftape = &g_cmt_mzftape_extension;
