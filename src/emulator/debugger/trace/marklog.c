/**
 * @file   marklog.c
 * @brief  Implementace 5. trace-suite subsystému - Marker Log.
 *
 * Viz @ref marklog.h pro design & per-record formát.
 *
 * @author Michal Hucik <hucik@ordoz.com>
 */

#include "main.h"

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED

#include "marklog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <json-glib/json-glib.h>

#include "main.h"
#include "libs/sdlapp/sdlapp.h"
#include "libs/sdlapp/sdlapp_options.h"
#include "libs/cfgfile/cfgmodule.h"
#include "emulator/cfgmain.h"
#include "emulator/debugger/trace/tlog_common.h"
#include "emulator/debugger/trace/reclife.h"
#include "emulator/debugger/trace/eventlog.h"
#include "emulator/mzarch/mzarch.h"

st_MARKLOG_CONFIG g_marklog_config;
int g_marklog_active = 0;

/* Privátní instance writeru tohoto subsystému. */
static st_TLOG_WRITER s_writer;
static int s_writer_open = 0;

/* Marker name registry - perzistentní v rámci celého emu běhu.
 *
 * Invariant: `g_hash_table_size(s_name_to_id) == s_id_to_name->len`.
 * Lifetime: alokuje marklog_init(), uvolňuje marklog_finalize().
 *
 * Mapping `name -> id` v hashtable je kódován jako
 * `g_hash_table_insert(table, key, GUINT_TO_POINTER((guint)id + 1))`
 * aby valid id=0 byl rozlišitelný od "not found" (= NULL hodnota).
 * Lookup pak rozbaluje přes `GPOINTER_TO_UINT(v) - 1`.
 */
static GHashTable *s_name_to_id = NULL;   /**< char* (vlastní GPtrArray) -> id+1 */
static GPtrArray  *s_id_to_name = NULL;   /**< index = id, hodnota = g_strdup(name) */


/* ===========================================================================
 *  Marker registry
 * =========================================================================== */

/**
 * @brief Inicializovat (lazy) hashtable + array.
 */
static void ensure_registry ( void )
{
    if ( s_name_to_id != NULL ) return;
    /* Klíče vlastněné GPtrArray (přes g_ptr_array_set_free_func(g_free)). */
    s_name_to_id = g_hash_table_new ( g_str_hash, g_str_equal );
    s_id_to_name = g_ptr_array_new_with_free_func ( g_free );
}


unsigned marklog_registry_count ( void )
{
    if ( !s_id_to_name ) return 0;
    return s_id_to_name->len;
}


const char *marklog_get_name ( uint16_t marker_id )
{
    if ( marker_id == MARKLOG_INVALID_ID ) return NULL;
    if ( !s_id_to_name ) return NULL;
    if ( marker_id >= s_id_to_name->len ) return NULL;
    return (const char *) g_ptr_array_index ( s_id_to_name, marker_id );
}


uint16_t marklog_register ( const char *name )
{
    if ( !name ) return MARKLOG_INVALID_ID;

    ensure_registry ( );

    /* Existing entry? */
    gpointer v = g_hash_table_lookup ( s_name_to_id, name );
    if ( v != NULL ) {
        return (uint16_t) ( GPOINTER_TO_UINT ( v ) - 1u );
    }

    /* Truncate name pokud delší než limit (-1 pro NUL). */
    char buf[ MARKLOG_NAME_MAX ];
    size_t n = strlen ( name );
    int truncated = 0;
    if ( n >= MARKLOG_NAME_MAX ) {
        memcpy ( buf, name, MARKLOG_NAME_MAX - 1 );
        buf[ MARKLOG_NAME_MAX - 1 ] = '\0';
        truncated = 1;
        fprintf ( stderr,
                  "[trace-suite] marklog: marker name truncated to %u chars: '%s...'\n",
                  (unsigned) ( MARKLOG_NAME_MAX - 1 ), buf );
        name = buf;
    }

    /* Overflow check - id musí vejít do uint16 mimo INVALID. */
    if ( s_id_to_name->len >= MARKLOG_INVALID_ID ) {
        fprintf ( stderr,
                  "[trace-suite] marklog: registry overflow (>65535 markers), "
                  "dropping '%s'\n", name );
        return MARKLOG_INVALID_ID;
    }

    /* Truncate path - znovu kontrola pro případ, že truncate vytvořil duplicit. */
    if ( truncated ) {
        gpointer v2 = g_hash_table_lookup ( s_name_to_id, name );
        if ( v2 != NULL ) {
            return (uint16_t) ( GPOINTER_TO_UINT ( v2 ) - 1u );
        }
    }

    uint16_t new_id = (uint16_t) s_id_to_name->len;
    gchar *owned = g_strdup ( name );
    g_ptr_array_add ( s_id_to_name, owned );
    /* Klíč v hashtable je tentýž buffer (= owned by s_id_to_name). */
    g_hash_table_insert ( s_name_to_id, owned, GUINT_TO_POINTER ( (guint) new_id + 1u ) );

    return new_id;
}


/* ===========================================================================
 *  Hot path
 * =========================================================================== */

void marklog_record ( uint16_t marker_id )
{
    if ( marker_id == MARKLOG_INVALID_ID ) return;

    /* Cesta 1: existující tlog disk write. */
    if ( s_writer_open ) {
        uint64_t pxclk_total = tlog_common_get_pxclk_total ( );
        uint32_t screens = tlog_common_get_screens_total ( );
        uint32_t pxclk_in = tlog_common_get_pxclk_in_screen ( );

        uint8_t buf[ 24 ];
        for ( int i = 0; i < 8; i++ ) buf[ i ]       = (uint8_t) ( pxclk_total >> ( i * 8 ) );
        for ( int i = 0; i < 4; i++ ) buf[ 8 + i ]   = (uint8_t) ( screens     >> ( i * 8 ) );
        for ( int i = 0; i < 4; i++ ) buf[ 12 + i ]  = (uint8_t) ( pxclk_in    >> ( i * 8 ) );
        buf[ 16 ] = (uint8_t) ( marker_id & 0xFFu );
        buf[ 17 ] = (uint8_t) ( ( marker_id >> 8 ) & 0xFFu );
        /* Reserved padding bytes 18..23 = 0. */
        buf[ 18 ] = 0;
        buf[ 19 ] = 0;
        buf[ 20 ] = 0;
        buf[ 21 ] = 0;
        buf[ 22 ] = 0;
        buf[ 23 ] = 0;

        tlog_writer_append ( &s_writer, buf, sizeof ( buf ) );
    }

    /* Cesta 2: paralelní fan-out do in-memory eventlog ringu.
     * USER_MARK kategorie, subtype = 0 (rezerva pro budoucí typy),
     * payload = marker_id (low 16 b). */
    if ( TEST_TRACE_EVENTLOG_ACTIVE
         && ( g_eventlog_active_mask & ( 1ULL << EVENTLOG_CAT_USER_MARK ) ) ) {
        uint16_t pc = (uint16_t) g_mzarch_main.cpu->pc;
        eventlog_record ( EVENTLOG_CAT_USER_MARK, 0, pc, (uint32_t) marker_id );
    }
}


/* ===========================================================================
 *  Lifecycle
 * =========================================================================== */

/**
 * @brief Vybuduje JSON fragment s registrem markerů pro manifest <name>.json.
 *
 * Klíče = stringified id ("0", "1", ...), hodnoty = registered name.
 * Pokud je registr prázdný, vrací validní (prázdný) "markers" objekt.
 */
static char *build_subsys_header_json ( void )
{
    JsonBuilder *b = json_builder_new ( );
    json_builder_begin_object ( b );

    json_builder_set_member_name ( b, "event_record_size" );
    json_builder_add_int_value ( b, 24 );

    json_builder_set_member_name ( b, "marker_id_invalid" );
    json_builder_add_int_value ( b, MARKLOG_INVALID_ID );

    json_builder_set_member_name ( b, "marker_count" );
    json_builder_add_int_value ( b, (gint64) marklog_registry_count ( ) );

    json_builder_set_member_name ( b, "markers" );
    json_builder_begin_object ( b );
    if ( s_id_to_name ) {
        for ( guint i = 0; i < s_id_to_name->len; i++ ) {
            char key[ 12 ];
            g_snprintf ( key, sizeof ( key ), "%u", i );
            json_builder_set_member_name ( b, key );
            json_builder_add_string_value ( b, (const char *) g_ptr_array_index ( s_id_to_name, i ) );
        }
    }
    json_builder_end_object ( b );

    json_builder_set_member_name ( b, "start_pxclk" );
    json_builder_add_int_value ( b, (gint64) tlog_common_get_pxclk_total ( ) );

    json_builder_set_member_name ( b, "start_pxclk_in_screen" );
    json_builder_add_int_value ( b, (gint64) tlog_common_get_pxclk_in_screen ( ) );

    json_builder_set_member_name ( b, "start_screens" );
    json_builder_add_int_value ( b, (gint64) tlog_common_get_screens_total ( ) );

    json_builder_set_member_name ( b, "start_cpuclk" );
    json_builder_add_int_value ( b, (gint64) tlog_common_get_cpuclk_total ( ) );

    json_builder_end_object ( b );

    JsonGenerator *gen = json_generator_new ( );
    JsonNode *root = json_builder_get_root ( b );
    json_generator_set_root ( gen, root );
    json_generator_set_pretty ( gen, TRUE );
    json_generator_set_indent ( gen, 2 );

    gsize len = 0;
    gchar *out = json_generator_to_data ( gen, &len );

    g_object_unref ( gen );
    if ( root ) json_node_unref ( root );
    g_object_unref ( b );

    return out;
}


int marklog_start ( void )
{
    if ( s_writer_open ) return 0;
    if ( !g_marklog_config.dir || !g_marklog_config.dir[0]
         || !g_marklog_config.name || !g_marklog_config.name[0] ) {
        return -1;
    }
    char *resolved_dir = sdlapp_paths_resolve_work ( g_sdlapp->paths, g_marklog_config.dir );
    uint64_t init_px  = tlog_common_get_pxclk_total ( );
    uint64_t init_cpu = tlog_common_get_cpuclk_total ( );
    uint32_t init_sc  = tlog_common_get_screens_total ( );
    if ( tlog_writer_open ( &s_writer, "marklog", resolved_dir, g_marklog_config.name,
                            g_marklog_config.chunk_mb, g_marklog_config.max_total_mb,
                            init_px, init_cpu, init_sc ) != 0 ) {
        g_free ( resolved_dir );
        return -1;
    }
    s_writer_open = 1;
    tlog_writer_set_record_size ( &s_writer, TLOG_RECORD_SIZE_EVENT24 );

    char *hdr = build_subsys_header_json ( );
    tlog_writer_update_meta ( &s_writer, hdr );
    g_free ( hdr );
    g_free ( resolved_dir );
    fprintf ( stderr, "[trace-suite] marklog: recording started\n" );
    return 0;
}


void marklog_stop ( void )
{
    if ( !s_writer_open ) return;
    uint64_t now_px  = tlog_common_get_pxclk_total ( );
    uint64_t now_cpu = tlog_common_get_cpuclk_total ( );
    uint32_t now_sc  = tlog_common_get_screens_total ( );
    tlog_writer_close ( &s_writer, now_px, now_cpu, now_sc );
    s_writer_open = 0;
    fprintf ( stderr, "[trace-suite] marklog: recording stopped\n" );
}


void marklog_finalize ( void )
{
    if ( g_marklog_config.save_on_exit && s_writer_open ) marklog_stop ( );

    /* Registr - GPtrArray vlastní stringy přes free_func, hashtable jen
     * pointer na ně. Pořadí důležité: nejdřív hashtable (nepouští stringy),
     * pak array (uvolní stringy). */
    if ( s_name_to_id ) {
        g_hash_table_destroy ( s_name_to_id );
        s_name_to_id = NULL;
    }
    if ( s_id_to_name ) {
        g_ptr_array_free ( s_id_to_name, TRUE );
        s_id_to_name = NULL;
    }

    g_free ( g_marklog_config.dir );
    g_free ( g_marklog_config.name );
    g_marklog_config.dir = NULL;
    g_marklog_config.name = NULL;
}


int marklog_is_truncated ( void )
{
    if ( !s_writer_open ) return 0;
    return tlog_writer_is_truncated ( &s_writer );
}


int marklog_save_segment ( const char *path )
{
    /* Vzor sdílený s cputrack_save_segment / iorqlog_save_segment - viz tam
     * pro detaily semantiky. */
    int was_active = ( g_marklog_active != 0 );

    /* Prázdná cesta = "force save now": flush rozdělaného segmentu BEZ
     * uzavření/reopenu (segment pokračuje do téhož manifestu, mzdos 0022). */
    if ( !( path && path[ 0 ] ) ) {
        if ( s_writer_open ) {
            uint64_t now_px = tlog_common_get_pxclk_total ( );
            uint64_t now_cpu = tlog_common_get_cpuclk_total ( );
            uint32_t now_sc = tlog_common_get_screens_total ( );
            tlog_writer_flush_chunk ( &s_writer, now_px, now_cpu, now_sc );
        }
        return 0;
    }

    if ( s_writer_open ) {
        marklog_stop ( );
    }
    reclife_redirect_path ( &g_marklog_config.dir, &g_marklog_config.name, path );
    if ( was_active ) {
        return marklog_start ( );
    }
    return 0;
}


/* Sdílený lifecycle deskriptor (reclife) - viz cputrack.c pro detaily vzoru. */
static const st_RECLIFE_DESC s_marklog_reclife = {
    .subsys_name = "marklog",
    .mode_ptr    = (int *) &g_marklog_config.mode,
    .active_ptr  = &g_marklog_active,
    .fn_start    = marklog_start,
    .fn_stop     = marklog_stop,
    .fn_reset    = NULL,
    .fn_save     = marklog_save_segment,
};


void marklog_recompute_active ( int debugger_active )
{
    reclife_recompute_active ( &s_marklog_reclife, debugger_active );
}


void marklog_init ( void )
{
    memset ( &g_marklog_config, 0, sizeof ( g_marklog_config ) );
    g_marklog_config.mode = TLOG_MODE_OFF;
    g_marklog_config.chunk_mb = TLOG_DEFAULT_CHUNK_MB;
    g_marklog_config.max_total_mb = TLOG_DEFAULT_MAX_TOTAL_MB;
    g_marklog_config.save_on_exit = 1;
    g_marklog_config.stdout_enabled = 1;  /* back-compat default */

    /* Eager init registru - alokuje hashtable + array, aby parser mohl
     * volat marklog_register() i před prvním marklog_start(). */
    ensure_registry ( );

    CFGMOD *cmod = cfgroot_register_new_module ( g_cfgmain, "TRACE_MARKLOG" );
    CFGELM *elm;
    elm = cfgmodule_register_new_element ( cmod, "mode", CFGENTYPE_KEYWORD, TLOG_MODE_OFF,
                                           TLOG_MODE_OFF,         "OFF",
                                           TLOG_MODE_WITH_WINDOW, "WITH_WINDOW",
                                           TLOG_MODE_ALWAYS,      "ALWAYS",
                                           -1 );
    cfgelement_set_handlers ( elm, (void*) &g_marklog_config.mode, (void*) &g_marklog_config.mode );
    elm = cfgmodule_register_new_element ( cmod, "dir", CFGENTYPE_TEXT, "trace-suite" );
    cfgelement_bind ( elm, (void*) &g_marklog_config.dir );
    elm = cfgmodule_register_new_element ( cmod, "name", CFGENTYPE_TEXT, "marklog" );
    cfgelement_bind ( elm, (void*) &g_marklog_config.name );
    elm = cfgmodule_register_new_element ( cmod, "chunk_mb", CFGENTYPE_UNSIGNED, TLOG_DEFAULT_CHUNK_MB, 1, 4096 );
    cfgelement_set_handlers ( elm, (void*) &g_marklog_config.chunk_mb, (void*) &g_marklog_config.chunk_mb );
    elm = cfgmodule_register_new_element ( cmod, "max_total_mb", CFGENTYPE_UNSIGNED, TLOG_DEFAULT_MAX_TOTAL_MB, 0, 1048576 );
    cfgelement_set_handlers ( elm, (void*) &g_marklog_config.max_total_mb, (void*) &g_marklog_config.max_total_mb );
    elm = cfgmodule_register_new_element ( cmod, "save_on_exit", CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm, (void*) &g_marklog_config.save_on_exit, (void*) &g_marklog_config.save_on_exit );
    elm = cfgmodule_register_new_element ( cmod, "stdout_enabled", CFGENTYPE_BOOL, 1 );
    cfgelement_set_handlers ( elm, (void*) &g_marklog_config.stdout_enabled, (void*) &g_marklog_config.stdout_enabled );

    cfgmodule_parse ( cmod );
    cfgmodule_propagate ( cmod );
}


void marklog_apply_cli_options ( void )
{
    const char *v;
    v = sdlapp_option_value ( "--marklog-mode" );
    if ( v ) {
        if ( g_ascii_strcasecmp ( v, "off" ) == 0 ) g_marklog_config.mode = TLOG_MODE_OFF;
        else if ( g_ascii_strcasecmp ( v, "window" ) == 0 ) g_marklog_config.mode = TLOG_MODE_WITH_WINDOW;
        else if ( g_ascii_strcasecmp ( v, "always" ) == 0 ) g_marklog_config.mode = TLOG_MODE_ALWAYS;
    }
    v = sdlapp_option_value ( "--marklog-dir" );
    if ( v ) { g_free ( g_marklog_config.dir ); g_marklog_config.dir = g_strdup ( v ); }
    v = sdlapp_option_value ( "--marklog-name" );
    if ( v ) { g_free ( g_marklog_config.name ); g_marklog_config.name = g_strdup ( v ); }
    v = sdlapp_option_value ( "--marklog-chunk-mb" );
    if ( v ) g_marklog_config.chunk_mb = (unsigned) g_ascii_strtoull ( v, NULL, 10 );
    v = sdlapp_option_value ( "--marklog-max-total-mb" );
    if ( v ) g_marklog_config.max_total_mb = (unsigned) g_ascii_strtoull ( v, NULL, 10 );
    v = sdlapp_option_value ( "--marklog-save-on-exit" );
    if ( v ) {
        if ( g_ascii_strcasecmp ( v, "on" ) == 0 ) g_marklog_config.save_on_exit = 1;
        else if ( g_ascii_strcasecmp ( v, "off" ) == 0 ) g_marklog_config.save_on_exit = 0;
    }
    v = sdlapp_option_value ( "--marklog-stdout" );
    if ( v ) {
        if ( g_ascii_strcasecmp ( v, "on" ) == 0 ) g_marklog_config.stdout_enabled = 1;
        else if ( g_ascii_strcasecmp ( v, "off" ) == 0 ) g_marklog_config.stdout_enabled = 0;
    }
}

const st_TLOG_WRITER *marklog_get_writer ( void )
{
    return &s_writer;
}

#endif /* MZ800EMU_CFG_DEBUGGER_ENABLED */
