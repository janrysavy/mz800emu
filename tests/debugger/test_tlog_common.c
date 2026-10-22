/*
 * test_tlog_common.c - unit testy pro trace-suite framework
 *
 * Testuje:
 *  - tlog_writer_open / append / flush_chunk / close lifecycle
 *  - chunk boundary - přesné naplnění bufferu vyvolá flush
 *  - max_total_mb cap - po dosažení truncated + meta.json flag
 *  - meta.json serializace - validní JSON s chunks[] po flush
 *  - tlog_common_get_* clock domain accessors
 *  - tlog_common_ensure_dir - vytvoření adresáře
 *
 * Testy pracují přímo s tlog_writer_* API. Buffer je malý (1 MB) aby
 * šly chunk boundary testy stihnout v rozumném čase a paměti.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "debugger/trace/tlog_common.h"


/* ================================================================
 * setUp / tearDown
 * ================================================================ */

/* Per-test pracovní adresář (pro chunk soubory + meta.json). */
static char *s_test_dir = NULL;


void setUp ( void )
{
    GError *err = NULL;
    s_test_dir = g_dir_make_tmp ( "mztest-tlog-XXXXXX", &err );
    if ( !s_test_dir ) {
        if ( err ) g_error_free ( err );
        TEST_FAIL_MESSAGE ( "Nepodařilo se vytvořit tmpdir" );
        return;
    }
}


/* Rekurzivní mazání adresáře (sdílené s test_unicard.c, ale lokální kopie). */
static int rmtree ( const char *path )
{
    if ( !path || !g_file_test ( path, G_FILE_TEST_EXISTS ) ) return 0;
    if ( g_file_test ( path, G_FILE_TEST_IS_DIR ) && !g_file_test ( path, G_FILE_TEST_IS_SYMLINK ) ) {
        GError *err = NULL;
        GDir *d = g_dir_open ( path, 0, &err );
        if ( !d ) {
            if ( err ) g_error_free ( err );
            return -1;
        }
        const gchar *name;
        while ( ( name = g_dir_read_name ( d ) ) != NULL ) {
            char *child = g_build_filename ( path, name, NULL );
            rmtree ( child );
            g_free ( child );
        }
        g_dir_close ( d );
        return g_rmdir ( path );
    }
    return g_remove ( path );
}


void tearDown ( void )
{
    if ( s_test_dir ) {
        rmtree ( s_test_dir );
        g_free ( s_test_dir );
        s_test_dir = NULL;
    }
}


/* ================================================================
 * Pomocné helpery
 * ================================================================ */

/* Načte celý soubor do nově alokovaného bufferu. Vrací size přes *out_size,
 * NULL při chybě. Caller g_free(). */
static uint8_t *read_whole_file ( const char *dir, const char *filename, size_t *out_size )
{
    char *path = g_build_filename ( dir, filename, NULL );
    FILE *fp = g_fopen ( path, "rb" );
    g_free ( path );
    if ( !fp ) {
        if ( out_size ) *out_size = 0;
        return NULL;
    }
    fseek ( fp, 0, SEEK_END );
    long size = ftell ( fp );
    fseek ( fp, 0, SEEK_SET );
    if ( size < 0 ) {
        fclose ( fp );
        if ( out_size ) *out_size = 0;
        return NULL;
    }
    uint8_t *buf = (uint8_t *) g_malloc ( (size_t) size + 1 );
    size_t n = fread ( buf, 1, (size_t) size, fp );
    buf[ n ] = 0;
    fclose ( fp );
    if ( out_size ) *out_size = n;
    return buf;
}


/* Vrátí TRUE pokud needle existuje v haystack jako substring. */
static int str_contains ( const char *haystack, const char *needle )
{
    if ( !haystack || !needle ) return 0;
    return ( strstr ( haystack, needle ) != NULL ) ? 1 : 0;
}


/* Tolerantní JSON klíč:hodnota substring check.
 *
 * Akceptuje obě varianty oddělovače:
 *   - "klíč": hodnota   (= python json.dumps(indent=N) styl)
 *   - "klíč" : hodnota  (= json-glib pretty print styl)
 *
 * Caller předá needle bez dvojtečky, např. ("subsys", "\"cputrack\"")
 * a interně se postaví obě formy a hledá strstr().
 */
static int json_kv_present ( const char *haystack,
                             const char *key, const char *value )
{
    if ( !haystack || !key || !value ) return 0;
    char buf1[ 256 ];
    char buf2[ 256 ];
    g_snprintf ( buf1, sizeof ( buf1 ), "\"%s\": %s",  key, value );
    g_snprintf ( buf2, sizeof ( buf2 ), "\"%s\" : %s", key, value );
    return ( strstr ( haystack, buf1 ) || strstr ( haystack, buf2 ) ) ? 1 : 0;
}


/* ================================================================
 * tlog_common_ensure_dir
 * ================================================================ */

void test_ensure_dir_creates_subdir ( void )
{
    char *sub = g_build_filename ( s_test_dir, "newdir", NULL );

    int rc = tlog_common_ensure_dir ( sub );
    TEST_ASSERT_EQUAL_INT_MESSAGE ( 0, rc, "ensure_dir musí vytvořit nový adresář" );
    TEST_ASSERT_TRUE_MESSAGE ( g_file_test ( sub, G_FILE_TEST_IS_DIR ),
                               "Vytvořený adresář musí existovat" );
    g_free ( sub );
}


void test_ensure_dir_existing_is_ok ( void )
{
    /* s_test_dir už existuje. */
    int rc = tlog_common_ensure_dir ( s_test_dir );
    TEST_ASSERT_EQUAL_INT_MESSAGE ( 0, rc, "ensure_dir na existující musí být OK" );
}


void test_ensure_dir_null_or_empty ( void )
{
    TEST_ASSERT_EQUAL_INT ( -1, tlog_common_ensure_dir ( NULL ) );
    TEST_ASSERT_EQUAL_INT ( -1, tlog_common_ensure_dir ( "" ) );
}


/* ================================================================
 * tlog_common_get_* - clock domain accessors
 * ================================================================ */

void test_clock_domain_constants ( void )
{
    /* Po mztest_init je g_gdg inicializované, tyto konstanty musí mít
     * smysluplné hodnoty. */
    TEST_ASSERT_TRUE_MESSAGE ( tlog_common_get_pxclk_freq ( ) > 1000000,
                               "pxclk_freq musí být > 1 MHz" );
    TEST_ASSERT_EQUAL_UINT32_MESSAGE ( 5, tlog_common_get_cpu_divider ( ),
                                       "cpu_divider pro MZ-800/700/1500 = 5" );
    TEST_ASSERT_TRUE_MESSAGE ( tlog_common_get_pxclk_per_screen ( ) > 0,
                               "pxclk_per_screen > 0" );

    const char *plat = tlog_common_get_platform_name ( );
    TEST_ASSERT_NOT_NULL ( plat );
    /* TEST suite buildí jen MZ-800 (mz_test_core_mz800) */
    TEST_ASSERT_EQUAL_STRING_MESSAGE ( "MZ-800", plat,
                                       "Test core je build pro MZ-800" );
}


void test_clock_domain_cpuclk_is_pxclk_div_5 ( void )
{
    uint64_t px = tlog_common_get_pxclk_total ( );
    uint64_t cpu = tlog_common_get_cpuclk_total ( );
    /* CPU clk = pxCLK / 5 (MZ-800 standard divider). */
    TEST_ASSERT_EQUAL_UINT64_MESSAGE ( px / 5, cpu,
                                       "cpuclk musí být pxclk / 5" );
}


/* ================================================================
 * tlog_writer - basic lifecycle
 * ================================================================ */

void test_writer_open_close_minimal ( void )
{
    st_TLOG_WRITER w = { 0 };
    int rc = tlog_writer_open ( &w, "test_subsys", s_test_dir, "out",
                                1, 0, 100, 20, 5 );
    TEST_ASSERT_EQUAL_INT_MESSAGE ( 0, rc, "writer_open musí uspět" );
    TEST_ASSERT_NOT_NULL ( w.buffer );
    TEST_ASSERT_EQUAL_size_t ( 0, w.buffer_used );
    TEST_ASSERT_EQUAL_UINT ( 0, w.chunk_index );
    TEST_ASSERT_FALSE ( w.truncated );
    TEST_ASSERT_EQUAL_size_t ( 1u * 1024 * 1024, w.chunk_size_bytes );

    tlog_writer_close ( &w, 200, 40, 6 );
    /* Po close je writer zeroed. */
    TEST_ASSERT_NULL_MESSAGE ( w.buffer, "Po close musí být buffer NULL" );
}


void test_writer_open_rejects_empty_args ( void )
{
    st_TLOG_WRITER w = { 0 };
    /* Empty subsys */
    TEST_ASSERT_EQUAL_INT ( -1, tlog_writer_open ( &w, "", s_test_dir, "n", 1, 0, 0, 0, 0 ) );
    /* NULL subsys */
    TEST_ASSERT_EQUAL_INT ( -1, tlog_writer_open ( &w, NULL, s_test_dir, "n", 1, 0, 0, 0, 0 ) );
    /* Empty dir */
    TEST_ASSERT_EQUAL_INT ( -1, tlog_writer_open ( &w, "s", "", "n", 1, 0, 0, 0, 0 ) );
    /* Empty name */
    TEST_ASSERT_EQUAL_INT ( -1, tlog_writer_open ( &w, "s", s_test_dir, "", 1, 0, 0, 0, 0 ) );
}


void test_writer_close_idempotent ( void )
{
    st_TLOG_WRITER w = { 0 };
    tlog_writer_open ( &w, "subsys", s_test_dir, "out", 1, 0, 0, 0, 0 );
    tlog_writer_close ( &w, 0, 0, 0 );
    /* Druhé volání NESMÍ spadnout. */
    tlog_writer_close ( &w, 0, 0, 0 );
    TEST_ASSERT_NULL ( w.buffer );
}


/* ================================================================
 * tlog_writer - append + manual flush
 * ================================================================ */

void test_writer_append_below_chunk_no_flush ( void )
{
    st_TLOG_WRITER w = { 0 };
    tlog_writer_open ( &w, "test", s_test_dir, "out", 1, 0, 0, 0, 0 );

    uint8_t evt[ 12 ] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    /* 100 events * 12 B = 1200 B - mnohem méně než 1 MB chunk. */
    for ( int i = 0; i < 100; i++ ) {
        TEST_ASSERT_EQUAL_INT ( 0, tlog_writer_append ( &w, evt, sizeof ( evt ) ) );
    }
    TEST_ASSERT_EQUAL_size_t_MESSAGE ( 1200, w.buffer_used,
                                       "Po 100 append musí být buffer_used = 1200" );
    TEST_ASSERT_EQUAL_UINT_MESSAGE ( 0, w.chunk_index,
                                     "Bez flush chunk_index musí zůstat 0" );

    tlog_writer_close ( &w, 0, 0, 0 );
}


void test_writer_manual_flush_writes_chunk_file ( void )
{
    st_TLOG_WRITER w = { 0 };
    tlog_writer_open ( &w, "test", s_test_dir, "out", 1, 0, 0, 0, 0 );

    uint8_t evt[ 24 ];
    for ( int i = 0; i < 24; i++ ) evt[ i ] = (uint8_t) i;
    tlog_writer_append ( &w, evt, sizeof ( evt ) );

    int rc = tlog_writer_flush_chunk ( &w, 1000, 200, 1 );
    TEST_ASSERT_EQUAL_INT_MESSAGE ( 0, rc, "manual flush musí uspět" );
    TEST_ASSERT_EQUAL_size_t_MESSAGE ( 0, w.buffer_used,
                                       "Po flush musí být buffer_used = 0" );
    TEST_ASSERT_EQUAL_UINT_MESSAGE ( 1, w.chunk_index,
                                     "Po flush chunk_index = 1" );

    /* Soubor musí existovat: out.000.bin */
    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "out.000.bin", &fsize );
    TEST_ASSERT_NOT_NULL_MESSAGE ( buf, "out.000.bin musí existovat" );
    TEST_ASSERT_EQUAL_size_t_MESSAGE ( 24, fsize, "Soubor musí mít 24 B" );
    /* Obsah musí odpovídat eventu. */
    for ( int i = 0; i < 24; i++ ) {
        TEST_ASSERT_EQUAL_UINT8 ( i, buf[ i ] );
    }
    g_free ( buf );

    tlog_writer_close ( &w, 1000, 200, 1 );
}


void test_writer_flush_empty_buffer_is_noop ( void )
{
    st_TLOG_WRITER w = { 0 };
    tlog_writer_open ( &w, "test", s_test_dir, "out", 1, 0, 0, 0, 0 );

    /* Bez append flush je no-op. */
    int rc = tlog_writer_flush_chunk ( &w, 0, 0, 0 );
    TEST_ASSERT_EQUAL_INT ( 0, rc );
    TEST_ASSERT_EQUAL_UINT ( 0, w.chunk_index );

    /* Soubor out.000.bin nesmí existovat. */
    char *path = g_build_filename ( s_test_dir, "out.000.bin", NULL );
    TEST_ASSERT_FALSE_MESSAGE ( g_file_test ( path, G_FILE_TEST_EXISTS ),
                                "Empty flush nesmí vytvořit chunk soubor" );
    g_free ( path );

    tlog_writer_close ( &w, 0, 0, 0 );
}


/* ================================================================
 * tlog_writer - automatický chunk boundary flush
 * ================================================================ */

void test_writer_chunk_boundary_auto_flush ( void )
{
    st_TLOG_WRITER w = { 0 };
    /* 1 MB chunk = 1048576 B. Naplníme ho přesně 12 B eventy.
     * 1048576 / 12 = 87381 events + 4 B remainder. */
    tlog_writer_open ( &w, "test", s_test_dir, "out", 1, 0, 0, 0, 0 );

    uint8_t evt[ 12 ];
    memset ( evt, 0xAB, sizeof ( evt ) );

    /* Naplnit buffer těsně k limitu (87381 events = 1048572 B, zbývají 4 B). */
    for ( int i = 0; i < 87381; i++ ) {
        TEST_ASSERT_EQUAL_INT ( 0, tlog_writer_append ( &w, evt, sizeof ( evt ) ) );
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE ( 0, w.chunk_index,
                                     "Před překročením limitu chunk_index musí být 0" );
    TEST_ASSERT_EQUAL_size_t ( 1048572, w.buffer_used );

    /* Další event (12 B) by způsobil přetečení -> auto flush. */
    TEST_ASSERT_EQUAL_INT ( 0, tlog_writer_append ( &w, evt, sizeof ( evt ) ) );
    TEST_ASSERT_EQUAL_UINT_MESSAGE ( 1, w.chunk_index,
                                     "Po přetečení chunk_index musí být 1" );
    TEST_ASSERT_EQUAL_size_t_MESSAGE ( 12, w.buffer_used,
                                       "Po flush musí buffer_used obsahovat jen nový event" );

    /* Soubor out.000.bin musí mít 1048572 B. */
    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "out.000.bin", &fsize );
    TEST_ASSERT_NOT_NULL ( buf );
    TEST_ASSERT_EQUAL_size_t_MESSAGE ( 1048572, fsize,
                                       "out.000.bin musí mít přesně 1048572 B" );
    g_free ( buf );

    tlog_writer_close ( &w, 0, 0, 0 );
}


void test_writer_event_larger_than_chunk_fails ( void )
{
    st_TLOG_WRITER w = { 0 };
    tlog_writer_open ( &w, "test", s_test_dir, "out", 1, 0, 0, 0, 0 );

    /* Single event 2 MB > 1 MB chunk - musí být odmítnut. */
    uint8_t *huge = g_malloc ( 2 * 1024 * 1024 );
    int rc = tlog_writer_append ( &w, huge, 2 * 1024 * 1024 );
    TEST_ASSERT_EQUAL_INT_MESSAGE ( -1, rc,
                                    "Event > chunk_size musí být odmítnut" );
    g_free ( huge );

    tlog_writer_close ( &w, 0, 0, 0 );
}


/* ================================================================
 * tlog_writer - max_total_mb cap
 * ================================================================ */

void test_writer_max_total_truncates ( void )
{
    st_TLOG_WRITER w = { 0 };
    /* chunk=1 MB, max_total=1 MB. Po jednom chunku se musí truncate. */
    tlog_writer_open ( &w, "test", s_test_dir, "out", 1, 1, 0, 0, 0 );
    TEST_ASSERT_EQUAL_size_t ( 1u * 1024 * 1024, w.max_total_bytes );

    uint8_t evt[ 12 ];
    memset ( evt, 0x5A, sizeof ( evt ) );

    int truncated_at = -1;
    /* Limit je 1 MB / 12 = 87381 events. Po něm append vrátí -1. */
    for ( int i = 0; i < 100000; i++ ) {
        int rc = tlog_writer_append ( &w, evt, sizeof ( evt ) );
        if ( rc < 0 ) {
            truncated_at = i;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE ( truncated_at > 0,
                               "Po dosažení max_total musí append vrátit -1" );
    TEST_ASSERT_TRUE_MESSAGE ( tlog_writer_is_truncated ( &w ),
                               "is_truncated musí vracet 1" );

    /* Po truncate další append musí být no-op (-1). */
    TEST_ASSERT_EQUAL_INT ( -1, tlog_writer_append ( &w, evt, sizeof ( evt ) ) );

    tlog_writer_close ( &w, 0, 0, 0 );
}


/* ================================================================
 * meta.json
 * ================================================================ */

void test_meta_json_basic_structure ( void )
{
    st_TLOG_WRITER w = { 0 };
    tlog_writer_open ( &w, "cputrack", s_test_dir, "out", 1, 0,
                       12345, 2469, 0 );

    int rc = tlog_writer_update_meta ( &w, NULL );
    TEST_ASSERT_EQUAL_INT_MESSAGE ( 0, rc, "update_meta bez chunků musí uspět" );

    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "out.json", &fsize );
    TEST_ASSERT_NOT_NULL_MESSAGE ( buf, "out.json musí existovat" );

    const char *s = (const char *) buf;
    /* json-glib pretty print formátuje "klíč" : hodnota (= mezera před
     * i za dvojtečkou); helper json_kv_present toleruje oba formáty. */
    TEST_ASSERT_TRUE_MESSAGE ( json_kv_present ( s, "subsys", "\"cputrack\"" ),
                               "meta.json musí obsahovat subsys field" );
    TEST_ASSERT_TRUE_MESSAGE ( json_kv_present ( s, "platform", "\"MZ-800\"" ),
                               "meta.json musí obsahovat platform" );
    TEST_ASSERT_TRUE_MESSAGE ( json_kv_present ( s, "cpu_divider", "5" ),
                               "meta.json musí obsahovat cpu_divider=5" );
    TEST_ASSERT_TRUE_MESSAGE ( json_kv_present ( s, "chunks", "[" ),
                               "meta.json musí mít chunks pole" );
    TEST_ASSERT_TRUE_MESSAGE ( json_kv_present ( s, "truncated", "false" ),
                               "Bez max-total musí být truncated=false" );

    g_free ( buf );
    tlog_writer_close ( &w, 0, 0, 0 );
}


void test_meta_json_records_chunks_after_flush ( void )
{
    st_TLOG_WRITER w = { 0 };
    tlog_writer_open ( &w, "iorqlog", s_test_dir, "out", 1, 0,
                       100, 20, 0 );

    uint8_t evt[ 24 ];
    memset ( evt, 0x77, sizeof ( evt ) );
    tlog_writer_append ( &w, evt, sizeof ( evt ) );
    tlog_writer_flush_chunk ( &w, 200, 40, 0 );

    /* Po flush se update_meta zavolal automaticky. */
    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "out.json", &fsize );
    TEST_ASSERT_NOT_NULL ( buf );

    const char *s = (const char *) buf;
    TEST_ASSERT_TRUE_MESSAGE ( json_kv_present ( s, "index", "0" ),
                               "chunks[0].index musí být v meta" );
    TEST_ASSERT_TRUE_MESSAGE ( json_kv_present ( s, "file", "\"out.000.bin\"" ),
                               "chunks[0].file musí být v meta" );
    TEST_ASSERT_TRUE_MESSAGE ( json_kv_present ( s, "bytes", "24" ),
                               "chunks[0].bytes musí být 24" );

    g_free ( buf );
    tlog_writer_close ( &w, 200, 40, 0 );
}


void test_meta_json_truncated_flag_after_max_total ( void )
{
    st_TLOG_WRITER w = { 0 };
    tlog_writer_open ( &w, "test", s_test_dir, "out", 1, 1, 0, 0, 0 );

    uint8_t evt[ 12 ];
    memset ( evt, 0xCC, sizeof ( evt ) );
    /* Naplňovat dokud truncate. */
    for ( int i = 0; i < 100000; i++ ) {
        if ( tlog_writer_append ( &w, evt, sizeof ( evt ) ) < 0 ) break;
    }
    TEST_ASSERT_TRUE ( tlog_writer_is_truncated ( &w ) );

    /* Force update meta - truncate flag musí být v JSON. */
    tlog_writer_update_meta ( &w, NULL );

    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "out.json", &fsize );
    TEST_ASSERT_NOT_NULL ( buf );
    const char *s = (const char *) buf;

    TEST_ASSERT_TRUE_MESSAGE ( json_kv_present ( s, "truncated", "true" ),
                               "Po max-total musí být truncated=true v meta" );
    TEST_ASSERT_TRUE_MESSAGE ( json_kv_present ( s, "truncated_reason", "\"max_total_mb\"" ),
                               "truncated_reason musí být max_total_mb" );

    g_free ( buf );
    tlog_writer_close ( &w, 0, 0, 0 );
}


void test_meta_json_subsys_header_extra ( void )
{
    st_TLOG_WRITER w = { 0 };
    tlog_writer_open ( &w, "cputrack", s_test_dir, "out", 1, 0, 0, 0, 0 );

    const char *extra = "{\n    \"my_field\": 42,\n    \"another\": \"value\"\n  }";
    tlog_writer_update_meta ( &w, extra );

    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "out.json", &fsize );
    TEST_ASSERT_NOT_NULL ( buf );
    const char *s = (const char *) buf;

    TEST_ASSERT_TRUE_MESSAGE ( strstr ( s, "\"subsys_header\":" )
                               || strstr ( s, "\"subsys_header\" :" ),
                               "subsys_header klíč musí být v meta" );
    TEST_ASSERT_TRUE_MESSAGE ( json_kv_present ( s, "my_field", "42" ),
                               "Caller fragment musí být embedded" );

    g_free ( buf );
    tlog_writer_close ( &w, 0, 0, 0 );
}


/* ================================================================
 * Close - final flush
 * ================================================================ */

void test_writer_close_flushes_remaining ( void )
{
    st_TLOG_WRITER w = { 0 };
    tlog_writer_open ( &w, "test", s_test_dir, "out", 1, 0, 0, 0, 0 );

    uint8_t evt[ 12 ];
    memset ( evt, 0x42, sizeof ( evt ) );
    tlog_writer_append ( &w, evt, sizeof ( evt ) );

    /* Bez explicit flush - close ho musí udělat. */
    tlog_writer_close ( &w, 100, 20, 0 );

    /* out.000.bin musí existovat s 12 B. */
    size_t fsize = 0;
    uint8_t *buf = read_whole_file ( s_test_dir, "out.000.bin", &fsize );
    TEST_ASSERT_NOT_NULL_MESSAGE ( buf, "Close musí flushnout zbytek bufferu" );
    TEST_ASSERT_EQUAL_size_t ( 12, fsize );
    g_free ( buf );
}


/* ================================================================
 * Čtení proudu: segment a snímek (devdoc/trace/stream-read.md)
 * ================================================================ */

/** @brief Záznam 12 B s pořadovým číslem v prvních 4 bajtech. */
static void make_rec ( uint8_t rec[ 12 ], uint32_t seq )
{
    memset ( rec, 0, 12 );
    memcpy ( rec, &seq, sizeof ( seq ) );
}

/** @brief Pořadové číslo uložené v záznamu. */
static uint32_t rec_seq ( const uint8_t *rec )
{
    uint32_t seq;
    memcpy ( &seq, rec, sizeof ( seq ) );
    return seq;
}

/* Segment roste s každým open, close ho ponechá. */
void test_segment_increments_and_survives_close ( void )
{
    st_TLOG_WRITER w = { 0 };
    TEST_ASSERT_EQUAL_UINT ( 0, tlog_writer_segment ( &w ) );
    TEST_ASSERT_EQUAL_INT ( 0, tlog_writer_open ( &w, "test", s_test_dir, "out", 1, 0, 0, 0, 0 ) );
    unsigned s1 = tlog_writer_segment ( &w );
    TEST_ASSERT_TRUE ( s1 > 0 );
    tlog_writer_close ( &w, 0, 0, 0 );
    TEST_ASSERT_EQUAL_UINT_MESSAGE ( s1, tlog_writer_segment ( &w ), "close ponechá segment" );
    TEST_ASSERT_EQUAL_INT ( 0, tlog_writer_open ( &w, "test", s_test_dir, "out", 1, 0, 0, 0, 0 ) );
    TEST_ASSERT_TRUE_MESSAGE ( tlog_writer_segment ( &w ) > s1, "nový open = vyšší segment" );
    tlog_writer_close ( &w, 0, 0, 0 );
}

/* Nikdy neotevřený writer: snímek se segmentem 0. */
void test_snapshot_never_opened ( void )
{
    st_TLOG_WRITER w = { 0 };
    st_TLOG_READ_SNAPSHOT snap;
    TEST_ASSERT_EQUAL_INT ( 0, tlog_writer_read_snapshot ( &w, 0, 10, 4096, &snap ) );
    TEST_ASSERT_EQUAL_UINT ( 0, snap.segment );
    TEST_ASSERT_NULL ( snap.ram_copy );
    tlog_read_snapshot_clear ( &snap );
}

/* Živý segment: next_seq, hranice RAM/disk a kopie z RAM bufferu. */
void test_snapshot_live_ram_part ( void )
{
    st_TLOG_WRITER w = { 0 };
    tlog_writer_open ( &w, "test", s_test_dir, "out", 1, 0, 0, 0, 0 );
    tlog_writer_set_record_size ( &w, 12 );

    /* 1 MB chunk = 87381 záznamů po 12 B; 87381 + 100 = jeden chunk na
     * disku a 100 záznamů v RAM. */
    uint8_t rec[ 12 ];
    for ( uint32_t i = 0; i < 87381 + 100; i++ ) {
        make_rec ( rec, i );
        TEST_ASSERT_EQUAL_INT ( 0, tlog_writer_append ( &w, rec, sizeof ( rec ) ) );
    }

    st_TLOG_READ_SNAPSHOT snap;
    TEST_ASSERT_EQUAL_INT ( 0, tlog_writer_read_snapshot ( &w, 87381 + 10, 50, 1u << 20, &snap ) );
    TEST_ASSERT_EQUAL_UINT ( tlog_writer_segment ( &w ), snap.segment );
    TEST_ASSERT_EQUAL_INT ( 1, snap.live );
    TEST_ASSERT_EQUAL_size_t ( 12, snap.record_size );
    TEST_ASSERT_EQUAL_UINT64 ( 87381 + 100, snap.next_seq );
    TEST_ASSERT_EQUAL_UINT64 ( 87381, snap.ram_first_seq );
    TEST_ASSERT_EQUAL_UINT ( 1, snap.chunks->len );
    TEST_ASSERT_EQUAL_STRING ( "out", snap.name );
    TEST_ASSERT_EQUAL_UINT64 ( 87381 + 10, snap.ram_copy_seq );
    TEST_ASSERT_EQUAL_UINT64 ( 50, snap.ram_copy_count );
    for ( uint64_t k = 0; k < snap.ram_copy_count; k++ ) {
        TEST_ASSERT_EQUAL_UINT32 ( (uint32_t) ( snap.ram_copy_seq + k ), rec_seq ( snap.ram_copy + k * 12 ) );
    }
    tlog_read_snapshot_clear ( &snap );

    /* Rozsah přes hranici disk/RAM: kopie začne na ram_first_seq;
     * strop max_copy_bytes ořízne počet; rozsah za next_seq nic. */
    tlog_writer_read_snapshot ( &w, 87000, 1000, 24 * 12, &snap );
    TEST_ASSERT_EQUAL_UINT64 ( 87381, snap.ram_copy_seq );
    TEST_ASSERT_EQUAL_UINT64 ( 24, snap.ram_copy_count );
    tlog_read_snapshot_clear ( &snap );
    tlog_writer_read_snapshot ( &w, 87381 + 100, 10, 1u << 20, &snap );
    TEST_ASSERT_NULL ( snap.ram_copy );
    TEST_ASSERT_EQUAL_UINT64 ( 0, snap.ram_copy_count );
    tlog_read_snapshot_clear ( &snap );

    tlog_writer_close ( &w, 0, 0, 0 );
}

/* Uzavřený segment zůstane čitelný (vše na disku) do dalšího open. */
void test_snapshot_after_close ( void )
{
    st_TLOG_WRITER w = { 0 };
    tlog_writer_open ( &w, "test", s_test_dir, "out", 1, 0, 0, 0, 0 );
    tlog_writer_set_record_size ( &w, 12 );
    uint8_t rec[ 12 ];
    for ( uint32_t i = 0; i < 30; i++ ) {
        make_rec ( rec, i );
        tlog_writer_append ( &w, rec, sizeof ( rec ) );
    }
    unsigned seg = tlog_writer_segment ( &w );
    tlog_writer_close ( &w, 0, 0, 0 );

    st_TLOG_READ_SNAPSHOT snap;
    TEST_ASSERT_EQUAL_INT ( 0, tlog_writer_read_snapshot ( &w, 0, 30, 1u << 20, &snap ) );
    TEST_ASSERT_EQUAL_UINT ( seg, snap.segment );
    TEST_ASSERT_EQUAL_INT ( 0, snap.live );
    TEST_ASSERT_EQUAL_UINT64 ( 30, snap.next_seq );
    TEST_ASSERT_EQUAL_UINT64 ( 30, snap.ram_first_seq );
    TEST_ASSERT_NULL ( snap.ram_copy );
    TEST_ASSERT_EQUAL_UINT ( 1, snap.chunks->len );
    TEST_ASSERT_EQUAL_size_t ( 30 * 12, g_array_index ( snap.chunks, st_TLOG_CHUNK_META, 0 ).bytes );
    TEST_ASSERT_EQUAL_STRING ( s_test_dir, snap.dir );
    tlog_read_snapshot_clear ( &snap );

    /* Nový open popis uzavřeného segmentu zahodí. */
    tlog_writer_open ( &w, "test", s_test_dir, "out", 1, 0, 0, 0, 0 );
    tlog_writer_read_snapshot ( &w, 0, 30, 1u << 20, &snap );
    TEST_ASSERT_TRUE ( snap.segment > seg );
    TEST_ASSERT_EQUAL_UINT64 ( 0, snap.next_seq );
    TEST_ASSERT_EQUAL_UINT ( 0, snap.chunks->len );
    tlog_read_snapshot_clear ( &snap );
    tlog_writer_close ( &w, 0, 0, 0 );
}


/* ================================================================
 * MAIN
 * ================================================================ */

int main ( int argc, char *argv[] )
{
    mztest_parse_args ( argc, argv );
    mztest_init ( );

    UNITY_BEGIN ( );

    /* ensure_dir */
    RUN_TEST ( test_ensure_dir_creates_subdir );
    RUN_TEST ( test_ensure_dir_existing_is_ok );
    RUN_TEST ( test_ensure_dir_null_or_empty );

    /* Clock domain accessors */
    RUN_TEST ( test_clock_domain_constants );
    RUN_TEST ( test_clock_domain_cpuclk_is_pxclk_div_5 );

    /* Writer lifecycle */
    RUN_TEST ( test_writer_open_close_minimal );
    RUN_TEST ( test_writer_open_rejects_empty_args );
    RUN_TEST ( test_writer_close_idempotent );

    /* Append + manual flush */
    RUN_TEST ( test_writer_append_below_chunk_no_flush );
    RUN_TEST ( test_writer_manual_flush_writes_chunk_file );
    RUN_TEST ( test_writer_flush_empty_buffer_is_noop );

    /* Chunk boundary */
    RUN_TEST ( test_writer_chunk_boundary_auto_flush );
    RUN_TEST ( test_writer_event_larger_than_chunk_fails );

    /* Max total truncate */
    RUN_TEST ( test_writer_max_total_truncates );

    /* meta.json */
    RUN_TEST ( test_meta_json_basic_structure );
    RUN_TEST ( test_meta_json_records_chunks_after_flush );
    RUN_TEST ( test_meta_json_truncated_flag_after_max_total );
    RUN_TEST ( test_meta_json_subsys_header_extra );

    /* Close final flush */
    RUN_TEST ( test_writer_close_flushes_remaining );

    /* Čtení proudu */
    RUN_TEST ( test_segment_increments_and_survives_close );
    RUN_TEST ( test_snapshot_never_opened );
    RUN_TEST ( test_snapshot_live_ram_part );
    RUN_TEST ( test_snapshot_after_close );

    int result = UNITY_END ( );

    mztest_teardown ( );
    return result;
}
