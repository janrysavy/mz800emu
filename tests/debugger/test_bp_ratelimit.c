/*
 * test_bp_ratelimit.c — testy 0019 vrstva 2 (per-BP rate-limit) + vrstva 3
 *                       (kumulativní byte backstop) pro forward BP akce
 *
 * Pokrytí:
 *   Vrstva 3 (byte backstop, veřejné API):
 *     - akumulace přes breakpoints_fwd_account_bytes
 *     - auto-pauza na hraně překročení prahu (emulator_pause)
 *     - vypnutý backstop (limit 0) = žádná pauza
 *     - reset akumulátoru
 *   Vrstva 2 (per-BP rate-limit):
 *     - reset runtime stavu při (re-)enable BP
 *     - rate-limit zaškrtí opakovanou snapshot akci (default interval)
 *     - per-BP override interval umožní vyšší frekvenci
 *     - max_fires strop vypne vlastní BP (disable_self)
 *
 * Snapshot e2e: mztest_init() registruje snapshot komponenty, snapshot_save
 * tak zapíše reálný .mzs do tests/data/tmp/.
 *
 * Licence: GPLv3
 */

#include "mztest.h"

#include "debugger/breakpoints.h"
#include "debugger/bptmap.h"
#include "debugger/bp_expr.h"
#include "debugger/bp_action.h"
#include "debugger/bp_vars.h"
#include "debugger/dbgapi_cmdrq.h"
#include "debugger/dbgapi_emu.h"
#include "debugger/trace/tlog_common.h"   /* I19BYTE: tlog disk-byte čítač */
#include "debugger/trace/cputrack.h"      /* mode kanálu cputrack (trace řízený akcí BP) */
#include "emulator/emulator.h"

#include <string.h>
#include <stdio.h>
#include <glib.h>
#include <glib/gstdio.h>
#ifdef _WIN32
#  include <process.h>   /* getpid na MSVCRT/MinGW */
#else
#  include <unistd.h>
#endif


/*
 * Cesta k dočasnému snapshot souboru je per-PROCES unikátní (PID v názvu).
 * Bez toho by paralelní kopie testu (CI / stres) zapisovaly do stejného
 * souboru a kolidovaly při zápisu/rename i v tearDown g_remove - to byl
 * druhý zdroj flakiness vedle časové závislosti (viz mock clock níže).
 * Cesta je naplněna v setUp do g_snap_path (dopředná lomítka = OK pro
 * BP-DSL akci i g_remove/g_file_test).
 */
static char g_snap_path[640] = { 0 };


/**
 * @brief Naplní g_snap_path unikátní cestou v TMP/TEMP (per-PID).
 *
 * Konvence shodná s test_bp_action.c (make_tmp_mzs_path). Používá dopředná
 * lomítka i na Windows - akceptuje je jak BP-DSL parser, tak g_fopen.
 */
static void build_snap_path ( void ) {
    const char *tmpdir = getenv ( "TMP" );
    if ( !tmpdir ) tmpdir = getenv ( "TEMP" );
    if ( !tmpdir ) tmpdir = "/tmp";

    char raw[640];
    snprintf ( raw, sizeof ( raw ), "%s/test_bp_ratelimit_%d.mzs",
               tmpdir, (int) getpid ( ) );

    /* Normalizace zpětných lomítek na dopředná (Windows TMP). */
    size_t j = 0;
    for ( size_t i = 0; raw[i] != '\0' && j + 1 < sizeof ( g_snap_path ); i++ ) {
        g_snap_path[j++] = ( raw[i] == '\\' ) ? '/' : raw[i];
    };
    g_snap_path[j] = '\0';
}


/**
 * @brief Nastaví BP akci "snapshot \"<g_snap_path>\"" (per-PID cesta).
 */
static void set_snapshot_action ( int id ) {
    char src[760];
    snprintf ( src, sizeof ( src ), "snapshot \"%s\"", g_snap_path );
    breakpoints_set_action ( id, src );
}


/**
 * @brief Nastaví BP akci "trace_save cputrack, \"<cesta>\"".
 *
 * Recording NENÍ aktivní (g_cputrack_active==0), takže dbgapi_trace_lifecycle
 * SAVE jen přesměruje dir/name a vrátí 0 (úspěch) bez reálného disk zápisu.
 * To stačí pro test accountingu: fwd_record_fire_trace čte SOUČASNÝ tlog disk
 * čítač (který v testu naplníme přes tlog_common_disk_bytes_add), ne velikost
 * právě uloženého segmentu. Tím je test deterministický a nezávislý na živé
 * trace recording infrastruktuře.
 */
static void set_trace_save_action ( int id ) {
    char src[760];
    snprintf ( src, sizeof ( src ),
               "trace_save cputrack, \"%s.trace\"", g_snap_path );
    breakpoints_set_action ( id, src );
}


/* ========================================================================= */
/*  Deterministický mock clock pro rate-limit gate                           */
/* ========================================================================= */

/*
 * Rate-limit enforce (bp_action.c) čte čas přes injektovatelný hook
 * bp_action_now_us(). Testy ho zde přebijí mock clockem řízeným ručně, takže
 * čas neplyne reálně - tím jsou rate-limit testy deterministické a nezávislé
 * na CPU zátěži / preempci (dříve spoléhaly na g_usleep vs reálný interval =
 * timing-flaky pod paralelní zátěží).
 *
 * Mock čas startuje na nenulové hodnotě (MOCK_CLOCK_START_US): fwd_last_fire_us
 * == 0 znamená "ještě nikdy nefiroval", takže by čas 0 maskoval první fire.
 */

#define MOCK_CLOCK_START_US   1000000   /* 1 s - libovolný nenulový základ */

static int64_t g_mock_now_us = MOCK_CLOCK_START_US;


/**
 * @brief Mock časový zdroj - vrací ručně řízený monotónní čas v us.
 */
static int64_t mock_clock_now_us ( void ) {
    return g_mock_now_us;
}


/**
 * @brief Posune mock čas o zadaný počet milisekund (= deterministický "spánek").
 */
static void mock_clock_advance_ms ( int64_t ms ) {
    g_mock_now_us += ms * 1000;
}


/* ========================================================================= */
/*  setUp / tearDown                                                         */
/* ========================================================================= */


void setUp ( void ) {
    breakpoints_clear_all ( );
    g_breakpoints.next_id = 1;
    bp_vars_clear_storage ( );
    g_emulator.paused = false;
    g_emulator.snapshot_safepoint = false;
    /* I19BYTE: vynuluj kumulativní tlog disk čítač PŘED resetem accountingu,
     * aby trace_save baseline (resetovaný uvnitř reset_byte_accounting)
     * startoval z čisté nuly = deterministická delta v testech vrstvy 3. */
    tlog_common_disk_bytes_reset ( );
    /* I19FLUSH: flush-side guard NEzaregistrovaný (default = jako bez
     * breakpoints_init). Testy, které ho chtějí, si ho instalují samy. */
    tlog_common_set_disk_flush_hook ( NULL );
    /* Default byte backstop práh pro deterministické testy vrstvy 3. */
    breakpoints_fwd_set_byte_limit ( BP_ACTION_FWD_DEFAULT_BYTE_LIMIT );
    breakpoints_fwd_reset_byte_accounting ( );
    /* Globální default rate-limit interval na vestavěnou hodnotu (= jako po
     * breakpoints_init bez INI override). */
    breakpoints_fwd_set_default_min_interval_ms ( BP_ACTION_FWD_DEFAULT_MIN_INTERVAL_MS );
    /* Deterministický mock clock pro rate-limit gate (reset na základ). */
    g_mock_now_us = MOCK_CLOCK_START_US;
    bp_action_set_clock_hook ( mock_clock_now_us );
    /* Per-PID unikátní snapshot cesta (TMP/TEMP) = bez kolize paralelních kopií. */
    build_snap_path ( );
    g_remove ( g_snap_path );
}


void tearDown ( void ) {
    /* Návrat na reálný čas - mock clock nesmí přežít test. */
    bp_action_set_clock_hook ( NULL );
    g_remove ( g_snap_path );
    /* I19FLUSH: odregistruj flush-side guard - nesmí přežít do dalšího testu. */
    tlog_common_set_disk_flush_hook ( NULL );
    breakpoints_fwd_set_byte_limit ( BP_ACTION_FWD_DEFAULT_BYTE_LIMIT );
    tlog_common_disk_bytes_reset ( );
    breakpoints_fwd_reset_byte_accounting ( );
    breakpoints_fwd_set_default_min_interval_ms ( BP_ACTION_FWD_DEFAULT_MIN_INTERVAL_MS );
}


/* ========================================================================= */
/*  Pomocné funkce - produkční cesta (dbgapi dispatch, NE přímá mutace)      */
/* ========================================================================= */


/**
 * @brief Vytvoří BP přes produkční dbgapi cestu (CMD_BP_CREATE_WITH_INIT).
 *
 * Simuluje cestu, kterou používá MCP / GUI: naplní st_DBGAPI_BP_UPDATE_PARAM
 * s update_mask a předá dispatcheru. Žádná přímá mutace struct st_BPT.
 *
 * @param addr         Adresa BP.
 * @param interval_ms  fwd_min_interval_ms override (aplikováno přes UM bit).
 * @param max_fires    fwd_max_fires override (aplikováno přes UM bit).
 * @return ID vytvořeného BP nebo -1 při selhání.
 */
static int create_bp_with_ratelimit ( uint16_t addr,
                                       uint32_t interval_ms,
                                       uint32_t max_fires ) {
    st_DBGAPI_BP_UPDATE_PARAM p;
    memset ( &p, 0, sizeof ( p ) );
    p.id = -1;
    p.addr = addr;
    p.fwd_min_interval_ms = interval_ms;
    p.fwd_max_fires = max_fires;
    p.update_mask = DBGAPI_BP_UM_ADDR
                  | DBGAPI_BP_UM_FWD_MIN_INTERVAL_MS
                  | DBGAPI_BP_UM_FWD_MAX_FIRES;

    st_DBGAPI_CMDRQ rq;
    memset ( &rq, 0, sizeof ( rq ) );
    rq.cmd = DBGAPI_CMD_BP_CREATE_WITH_INIT;
    rq.cmd_state = DBGAPI_CMDSTATE_PENDING;
    rq.data_ptr = &p;
    rq.success = false;
    dbgapi_emu_dispatch ( &rq );
    return rq.success ? p.id : -1;
}


/**
 * @brief Updatuje rate-limit pole existujícího BP přes produkční cestu.
 *
 * @return true při úspěchu dispatch (success flag).
 */
static bool update_bp_ratelimit ( int id, uint32_t interval_ms,
                                   uint32_t max_fires ) {
    st_DBGAPI_BP_UPDATE_PARAM p;
    memset ( &p, 0, sizeof ( p ) );
    p.id = id;
    p.fwd_min_interval_ms = interval_ms;
    p.fwd_max_fires = max_fires;
    p.update_mask = DBGAPI_BP_UM_FWD_MIN_INTERVAL_MS
                  | DBGAPI_BP_UM_FWD_MAX_FIRES;

    st_DBGAPI_CMDRQ rq;
    memset ( &rq, 0, sizeof ( rq ) );
    rq.cmd = DBGAPI_CMD_BP_UPDATE;
    rq.cmd_state = DBGAPI_CMDSTATE_PENDING;
    rq.data_ptr = &p;
    rq.success = false;
    dbgapi_emu_dispatch ( &rq );
    return rq.success;
}


/* ========================================================================= */
/*  Vrstva 3 - kumulativní byte backstop (veřejné API)                       */
/* ========================================================================= */


/**
 * Akumulátor sčítá zapsané bajty; pod prahem žádná pauza.
 */
void test_byte_backstop_accumulates ( void ) {
    breakpoints_fwd_set_byte_limit ( 0 );   /* vypni auto-pauzu pro čistou akumulaci */
    breakpoints_fwd_reset_byte_accounting ( );

    TEST_ASSERT_EQUAL_UINT64 ( 0, breakpoints_fwd_get_total_bytes ( ) );

    breakpoints_fwd_account_bytes ( 1, 1000 );
    breakpoints_fwd_account_bytes ( 1, 2000 );
    breakpoints_fwd_account_bytes ( 2, 500 );

    TEST_ASSERT_EQUAL_UINT64 ( 3500, breakpoints_fwd_get_total_bytes ( ) );
    TEST_ASSERT_FALSE ( g_emulator.paused );
}


/**
 * Překročení prahu = emulátor se sám zapauzuje (auto-pauza backstopu).
 */
void test_byte_backstop_auto_pause_on_threshold ( void ) {
    breakpoints_fwd_set_byte_limit ( 10000 );
    breakpoints_fwd_reset_byte_accounting ( );

    /* Pod prahem - žádná pauza. */
    breakpoints_fwd_account_bytes ( 5, 9999 );
    TEST_ASSERT_FALSE ( g_emulator.paused );

    /* Překročení prahu (>= 10000) - auto-pauza. */
    breakpoints_fwd_account_bytes ( 5, 2 );
    TEST_ASSERT_TRUE ( g_emulator.paused );
    TEST_ASSERT_EQUAL_UINT64 ( 10001, breakpoints_fwd_get_total_bytes ( ) );
}


/**
 * Auto-pauza nastane jen jednou na hraně překročení (ne s každým dalším
 * zápisem nad prahem) - další zápis sám o sobě paused znovu nenastaví.
 */
void test_byte_backstop_pause_only_on_edge ( void ) {
    breakpoints_fwd_set_byte_limit ( 1000 );
    breakpoints_fwd_reset_byte_accounting ( );

    breakpoints_fwd_account_bytes ( 7, 1500 );   /* hrana - pauza */
    TEST_ASSERT_TRUE ( g_emulator.paused );

    /* Uživatel pauzu zrušil a pokračuje; další zápis nad prahem už NEemituje
     * další auto-pauzu (jsme za hranou before>=limit). */
    g_emulator.paused = false;
    breakpoints_fwd_account_bytes ( 7, 1000 );
    TEST_ASSERT_FALSE ( g_emulator.paused );
}


/**
 * Limit 0 = backstop vypnut, žádná auto-pauza ani při velkém objemu.
 */
void test_byte_backstop_disabled_no_pause ( void ) {
    breakpoints_fwd_set_byte_limit ( 0 );
    breakpoints_fwd_reset_byte_accounting ( );

    breakpoints_fwd_account_bytes ( 1, 1024ULL * 1024ULL * 1024ULL );  /* 1 GB */
    TEST_ASSERT_FALSE ( g_emulator.paused );
}


/**
 * Reset vynuluje akumulátor (= chování breakpoints_init / ruční reset).
 */
void test_byte_backstop_reset ( void ) {
    breakpoints_fwd_set_byte_limit ( 0 );
    breakpoints_fwd_account_bytes ( 1, 12345 );
    TEST_ASSERT_EQUAL_UINT64 ( 12345, breakpoints_fwd_get_total_bytes ( ) );

    breakpoints_fwd_reset_byte_accounting ( );
    TEST_ASSERT_EQUAL_UINT64 ( 0, breakpoints_fwd_get_total_bytes ( ) );
}


/**
 * get/set byte limit roundtrip.
 */
void test_byte_backstop_limit_getset ( void ) {
    breakpoints_fwd_set_byte_limit ( 42ULL * 1024 * 1024 );
    TEST_ASSERT_EQUAL_UINT64 ( 42ULL * 1024 * 1024,
                               breakpoints_fwd_get_byte_limit ( ) );
}


/* ========================================================================= */
/*  I19BYTE - trace_save countuje CELÝ disk footprint (vrstva 3 dokončení)   */
/* ========================================================================= */


/**
 * tlog disk-byte čítač (add/total/reset) - základní kontrakt API I19BYTE.
 */
void test_tlog_disk_counter_api ( void ) {
    tlog_common_disk_bytes_reset ( );
    TEST_ASSERT_EQUAL_UINT64 ( 0, tlog_common_disk_bytes_total ( ) );

    tlog_common_disk_bytes_add ( 64ULL * 1024 * 1024 );   /* 1 chunk segment */
    tlog_common_disk_bytes_add ( 640ULL * 1024 );         /* initial-state dump */
    tlog_common_disk_bytes_add ( 0 );                     /* no-op */

    TEST_ASSERT_EQUAL_UINT64 ( 64ULL * 1024 * 1024 + 640ULL * 1024,
                               tlog_common_disk_bytes_total ( ) );

    tlog_common_disk_bytes_reset ( );
    TEST_ASSERT_EQUAL_UINT64 ( 0, tlog_common_disk_bytes_total ( ) );
}


/**
 * E2E: trace_save fire accountuje CELÝ disk footprint trace-suite (delta tlog
 * disk čítače), NE jen velikost hlavního index souboru.
 *
 * Toto je jádro I19BYTE: V19 zjistil, že byte backstop u trace_save
 * podpočítával (g_stat jen na index ~tisíce B, chunk segmenty 64 MB + initial
 * dumpy se nepočítaly -> disk-flood ochrana nezafungovala). Test simuluje
 * objem, který trace recording reálně zapsala na disk (přes
 * tlog_common_disk_bytes_add), a ověřuje, že trace_save fire ho celý
 * naúčtuje do backstopu -> auto-pauza zafunguje i pro trace_save flood.
 */
void test_trace_save_accounts_full_disk_footprint ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    /* Práh 100 MB; baseline accountingu je po setUp na nule. */
    breakpoints_fwd_set_byte_limit ( 100ULL * 1024 * 1024 );
    breakpoints_fwd_reset_byte_accounting ( );

    int id = breakpoints_add ( 0x1234, "TRSAVE", -1 );
    breakpoints_set_type ( id, BPT_TYPE_PC_EXEC );
    set_trace_save_action ( id );

    /* Simuluj footprint, který trace recording zapsala na disk od posledního
     * accountingu: 2x 64 MB chunk = 128 MB (> práh 100 MB). */
    tlog_common_disk_bytes_add ( 128ULL * 1024 * 1024 );
    TEST_ASSERT_FALSE ( g_emulator.paused );   /* samotný zápis pauzu nespouští */

    /* trace_save fire -> accountuje deltu (128 MB) -> překročení prahu -> auto-pauza. */
    breakpoints_enforce_pc_exec ( 0x1234 );

    TEST_ASSERT_TRUE ( g_emulator.paused );
    TEST_ASSERT_EQUAL_UINT64 ( 128ULL * 1024 * 1024,
                               breakpoints_fwd_get_total_bytes ( ) );
}


/**
 * Trace_save fire accountuje JEN deltu disk čítače od posledního accountingu -
 * footprint zapsaný PŘED breakpoints_fwd_reset_byte_accounting se nezapočítá
 * (baseline reset). Tím se po ručním/emu resetu nestrhne stará historie.
 */
void test_trace_save_baseline_reset ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    breakpoints_fwd_set_byte_limit ( 100ULL * 1024 * 1024 );

    /* Historický footprint zapsaný PŘED resetem accountingu. */
    tlog_common_disk_bytes_add ( 500ULL * 1024 * 1024 );   /* 500 MB historie */
    breakpoints_fwd_reset_byte_accounting ( );             /* baseline = 500 MB */

    int id = breakpoints_add ( 0x1234, "TRBASE", -1 );
    breakpoints_set_type ( id, BPT_TYPE_PC_EXEC );
    set_trace_save_action ( id );

    /* Po resetu přibude jen 10 MB (< práh 100 MB). */
    tlog_common_disk_bytes_add ( 10ULL * 1024 * 1024 );
    breakpoints_enforce_pc_exec ( 0x1234 );

    /* Accountuje se jen delta od baseline (10 MB), historie 500 MB ne. */
    TEST_ASSERT_EQUAL_UINT64 ( 10ULL * 1024 * 1024,
                               breakpoints_fwd_get_total_bytes ( ) );
    TEST_ASSERT_FALSE ( g_emulator.paused );   /* 10 MB < 100 MB práh */
}


/* ========================================================================= */
/*  Vrstva 3 (flush-side) - I19FLUSH: backstop i na flush cestě              */
/* ========================================================================= */


/**
 * I19FLUSH jádro: flush-side guard accountuje disk-bajty PRŮBĚŽNĚ na flush
 * cestě (tlog_common_disk_bytes_add), tj. i BEZ jakéhokoliv trace_save BP fire.
 *
 * Toto je root-cause oprava V19B disk-floodu: trace flooduje disk inkrementálně
 * po chunkách (64 MB) mezi fire eventy, takže per-fire accounting by tu stopu
 * chytil pozdě. Po zaregistrování guardu (bp_action_install_disk_flush_guard)
 * každý chunk swap kanálu řízeného akcí BP (zde cputrack, jako po akci
 * trace_start) rovnou naroste backstop a na hraně prahu auto-zapauzuje -
 * žádný trace_save fire není potřeba.
 */
void test_flush_guard_accounts_without_fire ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    /* Zaregistruj flush-side guard (= co dělá breakpoints_init v produkci). */
    bp_action_install_disk_flush_guard ( );
    /* Srovnej baseline na aktuální (nulový) total - guard countuje deltu od něj. */
    bp_action_reset_trace_disk_baseline ( );

    breakpoints_fwd_set_byte_limit ( 100ULL * 1024 * 1024 );
    breakpoints_fwd_reset_byte_accounting ( );

    /* cputrack nahrává a spustila ho akce BP (bez skutečného writeru). */
    g_cputrack_config.mode = TLOG_MODE_ALWAYS;
    bp_action_trace_channel_note ( DBGAPI_TRACE_CHANNEL_CPUTRACK );

    /* První chunk swap (64 MB) - pod prahem 100 MB, žádná pauza, ale účtuje se. */
    tlog_common_disk_bytes_add ( 64ULL * 1024 * 1024 );
    TEST_ASSERT_EQUAL_UINT64 ( 64ULL * 1024 * 1024,
                               breakpoints_fwd_get_total_bytes ( ) );
    TEST_ASSERT_FALSE ( g_emulator.paused );

    /* Druhý chunk swap (64 MB) -> kumulativně 128 MB > 100 MB -> auto-pauza
     * i BEZ trace_save fire. Toto by bez flush-side guardu nikdy nenastalo. */
    tlog_common_disk_bytes_add ( 64ULL * 1024 * 1024 );
    TEST_ASSERT_EQUAL_UINT64 ( 128ULL * 1024 * 1024,
                               breakpoints_fwd_get_total_bytes ( ) );
    TEST_ASSERT_TRUE ( g_emulator.paused );

    g_cputrack_config.mode = TLOG_MODE_OFF;
    bp_action_trace_channel_forget ( DBGAPI_TRACE_CHANNEL_CPUTRACK );
}


/**
 * Trace bez akcí BP (spuštěný z GUI nebo MCP) se do byte backstopu nepočítá.
 *
 * Dřív guard účtoval každý zápis trace na disk: samotný trace bez BP po
 * 256 MB zapauzoval emulaci s "BP #-1" a headless běh s --exit-after-frames
 * visel. Teď ho hlídá jen max_total_mb kanálu. Bajty zapsané bez řízení
 * akcí BP nezapočítá ani pozdější zúčtování (baseline se posouvá). Kanál,
 * který přestal nahrávat, ztratí značku řízení akcí BP.
 */
void test_flush_guard_ignores_trace_without_bp ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    bp_action_install_disk_flush_guard ( );
    bp_action_reset_trace_disk_baseline ( );
    breakpoints_fwd_set_byte_limit ( 100ULL * 1024 * 1024 );
    breakpoints_fwd_reset_byte_accounting ( );

    /* Trace spuštěný uživatelem: nahrává, ale bez značky akce BP. */
    g_cputrack_config.mode = TLOG_MODE_ALWAYS;
    bp_action_trace_channel_forget ( DBGAPI_TRACE_CHANNEL_CPUTRACK );

    tlog_common_disk_bytes_add ( 256ULL * 1024 * 1024 );
    TEST_ASSERT_EQUAL_UINT64 ( 0, breakpoints_fwd_get_total_bytes ( ) );
    TEST_ASSERT_FALSE ( g_emulator.paused );

    /* Akce BP kanál převezme: účtuje se jen to, co přibude potom. */
    bp_action_trace_channel_note ( DBGAPI_TRACE_CHANNEL_CPUTRACK );
    tlog_common_disk_bytes_add ( 32ULL * 1024 * 1024 );
    TEST_ASSERT_EQUAL_UINT64 ( 32ULL * 1024 * 1024,
                               breakpoints_fwd_get_total_bytes ( ) );

    /* Kanál přestal nahrávat (vypnutý z GUI): značka zmizí, další zápisy
     * (jiný kanál spuštěný uživatelem) se nepočítají. */
    g_cputrack_config.mode = TLOG_MODE_OFF;
    tlog_common_disk_bytes_add ( 32ULL * 1024 * 1024 );
    TEST_ASSERT_EQUAL_UINT64 ( 32ULL * 1024 * 1024,
                               breakpoints_fwd_get_total_bytes ( ) );
    g_cputrack_config.mode = TLOG_MODE_ALWAYS;
    tlog_common_disk_bytes_add ( 32ULL * 1024 * 1024 );
    TEST_ASSERT_EQUAL_UINT64 ( 32ULL * 1024 * 1024,
                               breakpoints_fwd_get_total_bytes ( ) );
    TEST_ASSERT_FALSE ( g_emulator.paused );

    g_cputrack_config.mode = TLOG_MODE_OFF;
}


/**
 * I19FLUSH: bez zaregistrovaného guardu zůstává chování beze změny (pojistka
 * proti regресi existujících testů, které breakpoints_init nevolají).
 *
 * Samotný tlog_common_disk_bytes_add bez hooku jen inkrementuje čítač, NEsahá
 * na backstop akumulátor ani na pauzu.
 */
void test_flush_guard_noop_when_unregistered ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    /* setUp už hook odregistroval; pro jistotu explicitně. */
    tlog_common_set_disk_flush_hook ( NULL );
    breakpoints_fwd_set_byte_limit ( 100ULL * 1024 * 1024 );
    breakpoints_fwd_reset_byte_accounting ( );

    tlog_common_disk_bytes_add ( 256ULL * 1024 * 1024 );   /* 256 MB > práh */

    /* Bez hooku se backstop akumulátor nezvýší a nedojde k pauze. */
    TEST_ASSERT_EQUAL_UINT64 ( 0, breakpoints_fwd_get_total_bytes ( ) );
    TEST_ASSERT_FALSE ( g_emulator.paused );
    /* Disk čítač přesto naběhl (počítá se nezávisle). */
    TEST_ASSERT_EQUAL_UINT64 ( 256ULL * 1024 * 1024,
                               tlog_common_disk_bytes_total ( ) );
}


/**
 * I19FLUSH: žádný double-account mezi flush-side guardem a trace_save fire.
 *
 * Guard a fwd_record_fire_trace sdílejí baseline g_fwd_trace_disk_seen. Flush-side
 * cesta deltu zúčtuje průběžně a posune baseline; následný trace_save fire pak
 * uvidí jen zbytek delty (zde 0). Každý disk-bajt se tedy do backstopu dostane
 * právě jednou.
 */
void test_flush_guard_no_double_account_with_fire ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    bp_action_install_disk_flush_guard ( );
    bp_action_reset_trace_disk_baseline ( );

    breakpoints_fwd_set_byte_limit ( 0 );   /* vypni pauzu - sledujeme jen součet */
    breakpoints_fwd_reset_byte_accounting ( );

    int id = breakpoints_add ( 0x1234, "TRDBL", -1 );
    breakpoints_set_type ( id, BPT_TYPE_PC_EXEC );
    set_trace_save_action ( id );

    /* Kanál cputrack nahrává a ukládá ho akce BP (trace_save). */
    g_cputrack_config.mode = TLOG_MODE_ALWAYS;
    bp_action_trace_channel_note ( DBGAPI_TRACE_CHANNEL_CPUTRACK );

    /* Flush-side guard zaúčtuje 80 MB (chunk swapy mezi fire). */
    tlog_common_disk_bytes_add ( 80ULL * 1024 * 1024 );
    TEST_ASSERT_EQUAL_UINT64 ( 80ULL * 1024 * 1024,
                               breakpoints_fwd_get_total_bytes ( ) );

    /* Teď firne trace_save - delta od baseline je už 0 (guard ji posunul),
     * takže se NEZAPOČÍTÁ podruhé. Součet zůstane 80 MB, ne 160 MB. */
    breakpoints_enforce_pc_exec ( 0x1234 );
    TEST_ASSERT_EQUAL_UINT64 ( 80ULL * 1024 * 1024,
                               breakpoints_fwd_get_total_bytes ( ) );

    g_cputrack_config.mode = TLOG_MODE_OFF;
    bp_action_trace_channel_forget ( DBGAPI_TRACE_CHANNEL_CPUTRACK );
}


/* ========================================================================= */
/*  Vrstva 2 - per-BP rate-limit (runtime stav + reset)                      */
/* ========================================================================= */


/**
 * (Re-)enable BP vynuluje runtime stav rate-limitu (fire_count, last_fire).
 *
 * Bez resetu by max_fires zůstal trvale vyčerpaný napříč session.
 */
void test_ratelimit_reset_on_reenable ( void ) {
    int id = breakpoints_add ( 0x1234, "RL", -1 );
    breakpoints_set_type ( id, BPT_TYPE_PC_EXEC );

    st_BPT *bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_NOT_NULL ( bpt );

    /* Simuluj naběhlý runtime stav. */
    bpt->fwd_fire_count  = 5;
    bpt->fwd_last_fire_us = 123456;

    /* Disable -> enable: enable musí stav vynulovat. */
    breakpoints_set_enabled ( id, false );
    breakpoints_set_enabled ( id, true );

    bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_EQUAL_UINT32 ( 0, bpt->fwd_fire_count );
    TEST_ASSERT_EQUAL_INT64 ( 0, bpt->fwd_last_fire_us );
}


/* ========================================================================= */
/*  Vrstva 2 - rate-limit e2e přes snapshot forward akci                     */
/* ========================================================================= */


/**
 * Default rate-limit zaškrtí opakovanou snapshot akci na horké smyčce:
 * první hit zapíše, těsně následující hity se v rámci default intervalu
 * (250 ms) tiše skipnou. Ověřujeme přes per-BP fire_count == 1.
 *
 * Determinismus: mock clock se mezi hity neposouvá, takže elapsed == 0 <
 * 250 ms je garantováno nezávisle na reálné CPU zátěži.
 */
void test_ratelimit_throttles_repeated_snapshot ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    int id = breakpoints_add ( 0x1234, "SNAP", -1 );
    breakpoints_set_type ( id, BPT_TYPE_PC_EXEC );
    set_snapshot_action ( id );

    /* 5 hitů za sebou bez posunu mock času (= horká smyčka v rámci jednoho
     * okamžiku). Default interval 250 ms propustí jen první. */
    for ( int i = 0; i < 5; i++ ) {
        breakpoints_enforce_pc_exec ( 0x1234 );
    };

    st_BPT *bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_NOT_NULL ( bpt );
    TEST_ASSERT_EQUAL_UINT32 ( 1, bpt->fwd_fire_count );

    /* Soubor reálně vznikl (jeden zápis proběhl). */
    TEST_ASSERT_TRUE ( g_file_test ( g_snap_path, G_FILE_TEST_EXISTS ) );
}


/**
 * Per-BP override min_interval umožní vyšší frekvenci než default.
 * Override = velmi malý interval (1 ms) -> všech N hitů projde (po každém
 * posuneme mock čas o víc než override okno).
 */
void test_ratelimit_override_allows_more ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    int id = breakpoints_add ( 0x1234, "SNAP2", -1 );
    breakpoints_set_type ( id, BPT_TYPE_PC_EXEC );
    set_snapshot_action ( id );

    st_BPT *bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_NOT_NULL ( bpt );
    bpt->fwd_min_interval_ms = 1;   /* override: 1 ms místo default 250 ms */

    const int N = 3;
    for ( int i = 0; i < N; i++ ) {
        breakpoints_enforce_pc_exec ( 0x1234 );
        /* Posun mock času o 10 ms (>1 ms override okno) = další fire projde. */
        mock_clock_advance_ms ( 10 );
    };

    bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_EQUAL_UINT32 ( (uint32_t) N, bpt->fwd_fire_count );
}


/**
 * max_fires strop: po dosažení limitu se BP sám vypne (disable_self).
 *
 * Override interval = 1 ms (aby rate-limit nemaskoval max_fires), max_fires=2.
 * 1. hit fire (count 1), spin, 2. hit fire (count 2), spin, 3. hit narazí na
 * strop -> disable + skip. BP je pak disabled a další hit nic nedělá.
 */
void test_ratelimit_max_fires_disables_self ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    int id = breakpoints_add ( 0x1234, "SNAP3", -1 );
    breakpoints_set_type ( id, BPT_TYPE_PC_EXEC );
    set_snapshot_action ( id );

    st_BPT *bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_NOT_NULL ( bpt );
    bpt->fwd_min_interval_ms = 1;
    bpt->fwd_max_fires       = 2;

    breakpoints_enforce_pc_exec ( 0x1234 );   /* fire 1 */
    mock_clock_advance_ms ( 10 );
    breakpoints_enforce_pc_exec ( 0x1234 );   /* fire 2 */
    mock_clock_advance_ms ( 10 );
    breakpoints_enforce_pc_exec ( 0x1234 );   /* strop -> disable_self */

    bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_NOT_NULL ( bpt );
    TEST_ASSERT_EQUAL_UINT32 ( 2, bpt->fwd_fire_count );
    TEST_ASSERT_FALSE ( bpt->enabled );
}


/* ========================================================================= */
/*  Vrstva 2 - per-BP override z PRODUKČNÍ cesty (MCP/dbgapi dispatch)        */
/* ========================================================================= */


/**
 * Override fwd_min_interval_ms / fwd_max_fires nastavený přes produkční
 * dbgapi cestu (CMD_BP_CREATE_WITH_INIT) se reálně zapíše do st_BPT - ne
 * jen přímou mutací struct v testu.
 */
void test_ratelimit_override_via_mcp_create ( void ) {
    int id = create_bp_with_ratelimit ( 0x1234, 7, 3 );
    TEST_ASSERT_GREATER_THAN_INT ( 0, id );

    st_BPT *bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_NOT_NULL ( bpt );
    TEST_ASSERT_EQUAL_UINT32 ( 7, bpt->fwd_min_interval_ms );
    TEST_ASSERT_EQUAL_UINT32 ( 3, bpt->fwd_max_fires );
}


/**
 * Override nastavený přes produkční dbgapi cestu (CMD_BP_UPDATE) na
 * existující BP se reálně zapíše.
 */
void test_ratelimit_override_via_mcp_update ( void ) {
    int id = breakpoints_add ( 0x2000, "UPD", -1 );
    TEST_ASSERT_GREATER_THAN_INT ( 0, id );

    bool ok = update_bp_ratelimit ( id, 13, 9 );
    TEST_ASSERT_TRUE ( ok );

    st_BPT *bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_NOT_NULL ( bpt );
    TEST_ASSERT_EQUAL_UINT32 ( 13, bpt->fwd_min_interval_ms );
    TEST_ASSERT_EQUAL_UINT32 ( 9, bpt->fwd_max_fires );
}


/**
 * E2E: override nastavený PRODUKČNÍ cestou je respektován enforce gate.
 *
 * BP vytvořený přes CMD_BP_CREATE_WITH_INIT s malým intervalem (1 ms) +
 * snapshot akcí. Po každém hitu spinneme >1 ms, takže všech N firů projde
 * (= override umožní vyšší frekvenci než vestavěný default 250 ms).
 */
void test_ratelimit_override_via_mcp_enforced ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    int id = create_bp_with_ratelimit ( 0x1234, /*interval*/ 1, /*max_fires*/ 0 );
    TEST_ASSERT_GREATER_THAN_INT ( 0, id );
    breakpoints_set_type ( id, BPT_TYPE_PC_EXEC );
    set_snapshot_action ( id );

    const int N = 3;
    for ( int i = 0; i < N; i++ ) {
        breakpoints_enforce_pc_exec ( 0x1234 );
        mock_clock_advance_ms ( 10 );   /* >1 ms override okno uplyne */
    };

    st_BPT *bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_NOT_NULL ( bpt );
    TEST_ASSERT_EQUAL_UINT32 ( (uint32_t) N, bpt->fwd_fire_count );
}


/* ========================================================================= */
/*  Vrstva 2 - globální INI default                                          */
/* ========================================================================= */


/**
 * get/set globálního default rate-limit intervalu (roundtrip).
 */
void test_ratelimit_global_default_getset ( void ) {
    breakpoints_fwd_set_default_min_interval_ms ( 500 );
    TEST_ASSERT_EQUAL_UINT32 ( 500,
                               breakpoints_fwd_get_default_min_interval_ms ( ) );
}


/**
 * E2E: globální default (= INI fwd_default_min_interval_ms) se aplikuje na
 * BP BEZ vlastního per-BP override (fwd_min_interval_ms == 0).
 *
 * Default nastavíme velmi nízko (1 ms) - BP bez override pak propustí
 * všech N rychlých firů (po každém spin >1 ms). Bez globálního defaultu by
 * platil vestavěný 250 ms a propustil by jen první.
 */
void test_ratelimit_global_default_applied ( void ) {
    MZTEST_REQUIRE_LEVEL ( MZTEST_LEVEL_UNIT );

    /* Simuluje načtený INI klíč fwd_default_min_interval_ms = 1. */
    breakpoints_fwd_set_default_min_interval_ms ( 1 );

    int id = breakpoints_add ( 0x1234, "GDEF", -1 );
    TEST_ASSERT_GREATER_THAN_INT ( 0, id );
    breakpoints_set_type ( id, BPT_TYPE_PC_EXEC );
    set_snapshot_action ( id );

    st_BPT *bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_NOT_NULL ( bpt );
    TEST_ASSERT_EQUAL_UINT32 ( 0, bpt->fwd_min_interval_ms );  /* žádný per-BP override */

    const int N = 3;
    for ( int i = 0; i < N; i++ ) {
        breakpoints_enforce_pc_exec ( 0x1234 );
        mock_clock_advance_ms ( 10 );   /* >1 ms global-default okno uplyne */
    };

    bpt = breakpoints_find_by_id ( id );
    TEST_ASSERT_EQUAL_UINT32 ( (uint32_t) N, bpt->fwd_fire_count );
}


/* ========================================================================= */
/*  MAIN                                                                     */
/* ========================================================================= */


int main ( int argc, char *argv[] ) {
    mztest_parse_args ( argc, argv );
    mztest_init ( );

    UNITY_BEGIN ( );

    /* Vrstva 3 - byte backstop */
    RUN_TEST ( test_byte_backstop_accumulates );
    RUN_TEST ( test_byte_backstop_auto_pause_on_threshold );
    RUN_TEST ( test_byte_backstop_pause_only_on_edge );
    RUN_TEST ( test_byte_backstop_disabled_no_pause );
    RUN_TEST ( test_byte_backstop_reset );
    RUN_TEST ( test_byte_backstop_limit_getset );

    /* I19BYTE - trace_save countuje celý disk footprint (vrstva 3 dokončení) */
    RUN_TEST ( test_tlog_disk_counter_api );
    RUN_TEST ( test_trace_save_accounts_full_disk_footprint );
    RUN_TEST ( test_trace_save_baseline_reset );

    /* I19FLUSH - flush-side byte backstop guard (0019 v3 dokončení) */
    RUN_TEST ( test_flush_guard_accounts_without_fire );
    RUN_TEST ( test_flush_guard_ignores_trace_without_bp );
    RUN_TEST ( test_flush_guard_noop_when_unregistered );
    RUN_TEST ( test_flush_guard_no_double_account_with_fire );

    /* Vrstva 2 - per-BP rate-limit (stav + reset) */
    RUN_TEST ( test_ratelimit_reset_on_reenable );

    /* Vrstva 2 - rate-limit e2e přes snapshot */
    RUN_TEST ( test_ratelimit_throttles_repeated_snapshot );
    RUN_TEST ( test_ratelimit_override_allows_more );
    RUN_TEST ( test_ratelimit_max_fires_disables_self );

    /* Vrstva 2 - per-BP override z produkční (MCP/dbgapi) cesty */
    RUN_TEST ( test_ratelimit_override_via_mcp_create );
    RUN_TEST ( test_ratelimit_override_via_mcp_update );
    RUN_TEST ( test_ratelimit_override_via_mcp_enforced );

    /* Vrstva 2 - globální INI default */
    RUN_TEST ( test_ratelimit_global_default_getset );
    RUN_TEST ( test_ratelimit_global_default_applied );

    int result = UNITY_END ( );

    mztest_teardown ( );
    return result;
}
