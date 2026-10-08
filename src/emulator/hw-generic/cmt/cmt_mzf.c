/* 
 * File:   cmt_mzf.c
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 * Created on 19. května 2018, 16:55
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
#include <assert.h>
#include <math.h>
#include <string.h>

#include "libs/mzf/mzf.h"
#include "libs/mzf/mzf_tools.h"
#include "libs/mztape/mztape.h"
#include "libs/cmt_stream/cmt_stream.h"
#include "libs/generic_driver/generic_driver.h"
#include "generic_driver/memory_driver.h"
#include "baseui/baseui.h"

#include "cmt.h"
#include "cmtext.h"
#include "cmt_mzf.h"

static st_DRIVER *g_driver = &g_memory_driver_static;
//static st_DRIVER *g_driver_realloc = &g_ui_memory_driver_realloc;

char *g_cmt_mzf_fileext[] = {
                             "mzf",
                             "m12",
                             NULL
};

st_CMTEXT_INFO g_cmt_mzf_info = {
                                 "MZF",
                                 g_cmt_mzf_fileext,
                                 "MZF cmt extension",
                                 CMTEXT_TYPE_PLAYABLE
};

extern st_CMTEXT *g_cmt_mzf;


st_MZF_HEADER* cmtmzf_block_get_spec_mzfheader ( st_CMTEXT_BLOCK *block ) {
    assert ( block != NULL );
    assert ( block->spec != NULL );
    st_CMTMZF_BLOCKSPEC *blspec = (st_CMTMZF_BLOCKSPEC*) block->spec;
    return blspec->hdr;
}


void cmtmzf_blockspec_destroy ( st_CMTMZF_BLOCKSPEC *blspec ) {
    if ( !blspec ) return;
    if ( blspec->hdr ) baseui_tools_mem_free ( blspec->hdr );
    mztape_mztmzf_destroy ( blspec->mztmzf );
}


st_CMTMZF_BLOCKSPEC* cmtmzf_blockspec_new ( st_HANDLER *h, uint32_t offset, en_CMTSPEED cmtspeed, const st_MZTAPE_PULSES_LENGTH *pulses ) {

    if ( cmtspeed_is_custom ( cmtspeed ) && !pulses ) {
        fprintf ( stderr, "%s():%d - Custom speed without pulses\n", __func__, __LINE__ );
        return NULL;
    };

    st_MZF_HEADER *hdr = (st_MZF_HEADER*) baseui_tools_mem_alloc0 ( sizeof ( st_MZF_HEADER ) );
    if ( !hdr ) {
        fprintf ( stderr, "%s():%d - Can't alocate memory (%d)\n", __func__, __LINE__, (int) sizeof ( st_MZF_HEADER ) );
        return NULL;
    };

    if ( EXIT_SUCCESS == mzf_read_header_on_offset ( h, offset, hdr ) ) {
        char ascii_filename[MZF_FNAME_FULL_LENGTH];
        mzf_tools_get_fname ( hdr, ascii_filename );
        printf ( "%s fname: %s\n", cmtext_get_name ( g_cmt_mzf ), ascii_filename );
        printf ( "%s ftype: 0x%02x\n", cmtext_get_name ( g_cmt_mzf ), hdr->ftype );
        printf ( "%s fstrt: 0x%04x\n", cmtext_get_name ( g_cmt_mzf ), hdr->fstrt );
        printf ( "%s fsize: 0x%04x\n", cmtext_get_name ( g_cmt_mzf ), hdr->fsize );
        printf ( "%s fexec: 0x%04x\n", cmtext_get_name ( g_cmt_mzf ), hdr->fexec );
    } else {
        fprintf ( stderr, "%s():%d - Can't read MZF header\n", __func__, __LINE__ );
        baseui_tools_mem_free ( hdr );
        return NULL;
    };

    st_MZTAPE_MZF *mztmzf = mztape_create_mztapemzf ( h, offset );
    if ( !mztmzf ) {
        fprintf ( stderr, "%s() - %d: Can't create mztmzf\n", __func__, __LINE__ );
        baseui_tools_mem_free ( hdr );
        return NULL;
    };

    st_CMTMZF_BLOCKSPEC *blspec = (st_CMTMZF_BLOCKSPEC*) baseui_tools_mem_alloc0 ( sizeof ( st_CMTMZF_BLOCKSPEC ) );
    if ( !blspec ) {
        fprintf ( stderr, "%s():%d - Can't alocate memory (%d)\n", __func__, __LINE__, (int) sizeof ( st_CMTMZF_BLOCKSPEC ) );
        baseui_tools_mem_free ( hdr );
        mztape_mztmzf_destroy ( mztmzf );
        return NULL;
    };

    blspec->hdr = hdr;
    blspec->mztmzf = mztmzf;
    blspec->cmtspeed = cmtspeed;
    if ( cmtspeed_is_custom ( cmtspeed ) ) blspec->pulses = *pulses;

    return blspec;
}


void cmtmzf_block_close ( st_CMTEXT_BLOCK *block ) {
    if ( !block ) return;
    cmtmzf_blockspec_destroy ( (st_CMTMZF_BLOCKSPEC*) block->spec );
    block->spec = (void*) NULL;
    cmtext_block_destroy ( block );
}


static void cmtmzf_container_close ( st_CMTEXT_CONTAINER *container ) {
    cmtext_container_destroy ( container );
}


static void cmtmzf_eject ( void ) {
    cmtmzf_block_close ( g_cmt_mzf->block );
    g_cmt_mzf->block = (st_CMTEXT_BLOCK*) NULL;
    cmtmzf_container_close ( g_cmt_mzf->container );
    g_cmt_mzf->container = (st_CMTEXT_CONTAINER*) NULL;
}


/**
 * @brief Vygeneruje CMT stream bloku MZF (formát MZ-800 SANE).
 *
 * Vstream se generuje s frekvencí GDGCLK_BASE: každý půlpulz se zaokrouhlí
 * na celý takt GDG (u MZ-800 asi 56 ns), hrany pásky tedy leží prakticky
 * přesně podle délek pulzů z mztape a nekvantizují se na mřížku vzorkovací
 * frekvence. Přehrávání (cmtext_block_get_output) převádí takty GDG na
 * vzorky poměrem rate / GDGCLK_BASE, tj. 1:1.
 *
 * Bitstream se generuje s CMTSTREAM_DEFAULT_RATE (44,1 kHz) - jen jako
 * kompilační alternativa (CMTMZF_DEFAULT_STREAM_BITSTREAM) a pro případný
 * export do WAV. Půlpulzy se při něm zaokrouhlí na celé vzorky; při 3:1
 * z krátkého pulzu 81,9/92,7 us vznikne 90,7/90,7 us a zavaděče s pevným
 * okamžikem vzorkování bitu (např. Interkarate, díly 2-3) pak načítají
 * nespolehlivě.
 *
 * Rychlost dává buď poměr (délky pulzů Intercopy vydělené poměrem), nebo
 * CMTSPEED_CUSTOM s vlastními délkami pulzů (např. z hlavičky CMTSPEED
 * zařízení UniCMT); ty se na takt zaokrouhlují stejně.
 *
 * @param mztapemzf MZF data bloku (vlastní je blockspec, funkce je jen čte).
 * @param cmtspeed Rychlost záznamu: poměr nebo CMTSPEED_CUSTOM.
 * @param pulses Vlastní délky pulzů (povinné pro CMTSPEED_CUSTOM, jinak se ignoruje).
 * @param type Typ streamu: CMT_STREAM_TYPE_VSTREAM nebo CMT_STREAM_TYPE_BITSTREAM.
 * @return Nový stream (vlastník je volající, uvolnit cmt_stream_destroy),
 *         NULL při chybě (chybu hlásí mztape přes g_mztape_error_cb).
 * @note Vedlejší efekt: vypíše parametry streamu na stdout.
 */
static st_CMT_STREAM* cmtmzf_generate_stream_from_mztapemzf ( st_MZTAPE_MZF *mztapemzf, en_CMTSPEED cmtspeed, const st_MZTAPE_PULSES_LENGTH *pulses, en_CMT_STREAM_TYPE type ) {

    uint32_t rate = ( type == CMT_STREAM_TYPE_VSTREAM ) ? GDGCLK_BASE : CMTSTREAM_DEFAULT_RATE;
    st_CMT_STREAM *stream;
    if ( cmtspeed_is_custom ( cmtspeed ) ) {
        stream = mztape_create_stream_from_mztapemzf_pulses ( mztapemzf, pulses, type, CMTMZF_FORMATSET, rate );
    } else {
        stream = mztape_create_stream_from_mztapemzf ( mztapemzf, cmtspeed, type, CMTMZF_FORMATSET, rate );
    };
    if ( !stream ) {
        return NULL;
    };

    if ( cmtspeed_is_custom ( cmtspeed ) ) {
        printf ( "%s speed: custom pulses %0.3f / %0.3f / %0.3f / %0.3f us (~%0.2f:1)\n", cmtext_get_name ( g_cmt_mzf ),
                 pulses->long_pulse.high * 1e6, pulses->long_pulse.low * 1e6,
                 pulses->short_pulse.high * 1e6, pulses->short_pulse.low * 1e6,
                 mztape_pulses_get_ratio ( CMTMZF_FORMATSET, pulses ) );
    } else {
        char buff[100];
        cmtspeed_get_speedtxt ( buff, sizeof ( buff ), cmtspeed, MZTAPE_DEFAULT_BDSPEED );
        printf ( "%s speed: %s\n", cmtext_get_name ( g_cmt_mzf ), buff );
    };
    printf ( "%s stream type: %s\n", cmtext_get_name ( g_cmt_mzf ), cmt_stream_get_stream_type_txt ( stream ) );
    printf ( "%s stream size: %0.2f kB\n", cmtext_get_name ( g_cmt_mzf ), (float) cmt_stream_get_size ( stream ) / 1024 );
    printf ( "%s rate: %d Hz\n", cmtext_get_name ( g_cmt_mzf ), cmt_stream_get_rate ( stream ) );
    printf ( "%s length: %1.2f s\n", cmtext_get_name ( g_cmt_mzf ), cmt_stream_get_length ( stream ) );

    return stream;
}


/**
 * @brief Callback bloku: přegeneruje stream na novou výchozí rychlost Virtual CMT.
 *
 * Volá ho cmt_change_speed() / cmt_change_custom_pulses() jen pro blok
 * s CMTEXT_BLOCK_SPEED_DEFAULT. Pro CMTSPEED_CUSTOM se použijí výchozí
 * vlastní délky pulzů g_cmt.mz_custom_pulses (volající je nastaví předem).
 *
 * @param cmtext Rozšíření, jehož aktuální blok se mění (st_CMTEXT*).
 * @param cmtspeed Nová rychlost: poměr nebo CMTSPEED_CUSTOM.
 * @return EXIT_SUCCESS (i když se nic nezměnilo); EXIT_FAILURE, pokud blok
 *         nemá výchozí rychlost nebo se stream nepodařilo vytvořit (blok
 *         pak zůstane beze změny).
 */
static int cmtmzf_set_speed ( void *cmtext, en_CMTSPEED cmtspeed ) {
    assert ( cmtext != NULL );
    st_CMTEXT *cext = (st_CMTEXT*) cmtext;
    st_CMTEXT_BLOCK *block = cext->block;
    assert ( block != NULL );
    if ( block->block_speed != CMTEXT_BLOCK_SPEED_DEFAULT ) return EXIT_FAILURE;
    st_CMTMZF_BLOCKSPEC *blspec = (st_CMTMZF_BLOCKSPEC*) block->spec;
    const st_MZTAPE_PULSES_LENGTH *pulses = &g_cmt.mz_custom_pulses;
    if ( blspec->cmtspeed == cmtspeed ) {
        if ( !cmtspeed_is_custom ( cmtspeed ) ) return EXIT_SUCCESS;
        if ( 0 == memcmp ( &blspec->pulses, pulses, sizeof ( *pulses ) ) ) return EXIT_SUCCESS;
    };

    assert ( block->stream != NULL );

    st_CMT_STREAM *stream = cmtmzf_generate_stream_from_mztapemzf ( blspec->mztmzf, cmtspeed, pulses, block->stream->stream_type );
    if ( !stream ) return EXIT_FAILURE;
    cmt_stream_destroy ( block->stream );
    block->stream = stream;

    blspec->cmtspeed = cmtspeed;
    if ( cmtspeed_is_custom ( cmtspeed ) ) blspec->pulses = *pulses;
    return EXIT_SUCCESS;
}


/**
 * @brief Callback bloku: přenosová rychlost aktuálního bloku v Bd.
 *
 * U vlastních délek pulzů jde o ekvivalent vůči 1:1 (1200 Bd krát
 * mztape_pulses_get_ratio()), tedy jen orientační údaj.
 *
 * @param cmtext Rozšíření s otevřeným MZF blokem (st_CMTEXT*).
 * @return Rychlost v Bd (zaokrouhlená).
 */
static uint16_t cmtmzf_get_bdspeed ( void *cmtext ) {
    assert ( cmtext != NULL );
    st_CMTEXT *cext = (st_CMTEXT*) cmtext;
    st_CMTMZF_BLOCKSPEC *blspec = (st_CMTMZF_BLOCKSPEC*) cext->block->spec;
    assert ( blspec != NULL );
    if ( cmtspeed_is_custom ( blspec->cmtspeed ) ) {
        return (uint16_t) round ( MZTAPE_DEFAULT_BDSPEED * mztape_pulses_get_ratio ( CMTMZF_FORMATSET, &blspec->pulses ) );
    };
    return (uint16_t) round ( MZTAPE_DEFAULT_BDSPEED * g_cmtspeed_divisor[blspec->cmtspeed] );
}


/*
 * Typ streamu, ze kterého se MZF přehrává (právě jedno z maker):
 * CMTMZF_DEFAULT_STREAM_VSTREAM - vstream s přesností taktu GDG (výchozí),
 * CMTMZF_DEFAULT_STREAM_BITSTREAM - bitstream 44,1 kHz (pulzy zaokrouhlené
 * na vzorky, viz cmtmzf_generate_stream_from_mztapemzf).
 */
//#define CMTMZF_DEFAULT_STREAM_BITSTREAM
#define CMTMZF_DEFAULT_STREAM_VSTREAM


st_CMTEXT_BLOCK* cmtmzf_block_open ( st_HANDLER *h, uint32_t offset, int block_id, int pause_after, en_CMTEXT_BLOCK_SPEED block_speed, en_CMTSPEED cmtspeed, const st_MZTAPE_PULSES_LENGTH *pulses ) {

    printf ( "%s block id: %d\n", cmtext_get_name ( g_cmt_mzf ), block_id );

    st_CMTMZF_BLOCKSPEC *blspec = cmtmzf_blockspec_new ( h, offset, cmtspeed, pulses );
    if ( !blspec ) {
        return NULL;
    }

#ifdef CMTMZF_DEFAULT_STREAM_BITSTREAM
    st_CMT_STREAM *stream = cmtmzf_generate_stream_from_mztapemzf ( blspec->mztmzf, cmtspeed, &blspec->pulses, CMT_STREAM_TYPE_BITSTREAM );
#if 0
    st_HANDLER *hwav = generic_driver_open_memory ( NULL, g_driver_realloc, 1 );
    if ( !hwav ) {
        fprintf ( stderr, "%s():%d - Can't open handler\n", __func__, __LINE__ );
    } else {
        generic_driver_set_handler_readonly_status ( hwav, 0 );
        hwav->spec.memspec.swelling_enabled = 1;

        if ( EXIT_FAILURE == cmt_bitstream_create_wav ( hwav, stream->str.bitstream ) ) {
            fprintf ( stderr, "%s():%d - Can't create WAV from bitstream\n", __func__, __LINE__ );
            generic_driver_close ( hwav );
        } else {
            char filename[100];
            snprintf ( filename, sizeof (filename ), "mzf2wav_block_%d.wav", block_id );
            printf ( "Save WAV: %s\n", filename );
            if ( EXIT_FAILURE == generic_driver_save_memory ( hwav, filename ) ) {
                fprintf ( stderr, "%s():%d - Can't write WAV\n", __func__, __LINE__ );
            };
            generic_driver_close ( hwav );
        };
    };
#endif

#endif
#ifdef CMTMZF_DEFAULT_STREAM_VSTREAM
    st_CMT_STREAM *stream = cmtmzf_generate_stream_from_mztapemzf ( blspec->mztmzf, cmtspeed, &blspec->pulses, CMT_STREAM_TYPE_VSTREAM );
#endif
    if ( !stream ) {
        cmtmzf_blockspec_destroy ( blspec );
        return NULL;
    };

    st_CMTEXT_BLOCK *block = cmtext_block_new ( block_id, CMTEXT_BLOCK_TYPE_MZF, stream, block_speed, pause_after, blspec );
    if ( !block ) {
        cmtmzf_blockspec_destroy ( blspec );
        cmt_stream_destroy ( stream );
    };

    block->cb_play = cmtext_block_play;
    block->cb_get_playname = cmtext_block_get_playname;
    block->cb_set_polarity = (cmtext_block_cb_set_polarity) NULL;
    block->cb_set_speed = cmtmzf_set_speed;
    block->cb_get_bdspeed = cmtmzf_get_bdspeed;

    return block;
}


static int cmtmzf_container_open ( char *filename ) {

    cmtmzf_eject ( );

    st_CMTEXT_CONTAINER *container = cmtext_container_new ( CMTEXT_CONTAINER_TYPE_SINGLE, filename, 1, NULL, NULL, NULL, NULL );
    if ( !container ) {
        return EXIT_FAILURE;
    };

    printf ( "%s\nOpen: %s\n", cmtext_get_description ( g_cmt_mzf ), filename );

    st_HANDLER *h = generic_driver_open_memory_from_file ( NULL, g_driver, filename );

    if ( !h ) {
        baseui_error ( "%s: Can't open file '%s'\n", cmtext_get_description ( g_cmt_mzf ), filename );
        cmtmzf_container_close ( container );
        return EXIT_FAILURE;
    };

    st_CMTEXT_BLOCK *block = cmtmzf_block_open ( h, 0, 0, 0, CMTEXT_BLOCK_SPEED_DEFAULT, g_cmt.mz_cmtspeed, &g_cmt.mz_custom_pulses );
    if ( !block ) {
        baseui_error ( "%s: Can't create cmt block\n", cmtext_get_description ( g_cmt_mzf ) );
        cmtmzf_container_close ( container );
        generic_driver_close ( h );
        return EXIT_FAILURE;
    };

    generic_driver_close ( h );

    g_cmt_mzf->container = container;
    g_cmt_mzf->block = block;

    return EXIT_SUCCESS;
}


static void cmtmzf_init ( void ) {
    return;
}


static void cmtmzf_exit ( void ) {
    cmtmzf_eject ( );
}


st_CMTEXT g_cmt_mzf_extension = {
                                 &g_cmt_mzf_info,
                                 (st_CMTEXT_CONTAINER*) NULL,
                                 (st_CMTEXT_BLOCK*) NULL,
                                 cmtmzf_init,
                                 cmtmzf_exit,
                                 cmtmzf_container_open,
                                 (cmtext_cb_stop) NULL,
                                 cmtmzf_eject,
                                 (cmtext_cb_write_data) NULL,
};

st_CMTEXT *g_cmt_mzf = &g_cmt_mzf_extension;
