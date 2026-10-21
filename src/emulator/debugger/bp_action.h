/*
 * File:   bp_action.h
 *
 * Action mini-DSL pro smart breakpointy (V1, fáze D.1.3).
 *
 * Po splnění condition (= bp_expr eval -> non-zero) se vykoná action
 * skript. Skript je line-based, řádky odděluje '\n' nebo ';'. Komentáře
 * začínají '#' a pokračují do konce řádku. Argumenty příkazů jsou
 * vyhodnocovány přes bp_expr.
 *
 * Implicit continue:
 *   action == NULL nebo prázdný  → BP zastaví emulátor (= klasický stop)
 *   action s libovolným non-empty obsahem → BP automaticky pokračuje
 *
 * Explicitní zastavení: příkaz `stop` (zastaví jako prázdná akce, volitelně
 * s hláškou) a `exit` (žádost o konec běhu s návratovým kódem, viz
 * automation.h). Oba ukončí provádění akce: příkazy za nimi se nevykonají.
 *
 * Příkazy:
 *   log "fmt" [args...]      - printf-style do stdout (TODO V1.5: TTY/Trace)
 *   continue                 - jen explicit marker, default už je continue
 *   disable_self             - tento BP se po hitu vypne
 *   enable <name>            - aktivuj jiný BP podle jména
 *   disable <name>           - deaktivuj jiný BP podle jména
 *   poke <addr> <value>      - zápis byte do RAM
 *   set <reg> <value>        - nastav Z80 registr
 *   mark "name"              - marker do trace-suite (V1.5: skutečný hook)
 *   $name = <expr>           - přiřazení do user var
 *   $name <op>= <expr>       - += -= *= /= <<= >>= &= |= ^=
 *   clear_vars               - reset všech existujících $vars na 0
 *   if <expr> then <stmt>    - jednořádkový conditional, žádný else
 *   cdl_start / cdl_stop / cdl_reset       - CDL (Memory Heatmap) lifecycle
 *   cdl_export "fmt" [args]  - export CDL do souboru (šablona jména přes %X..)
 *   trace_start <chan>       - spustit trace kanál (cputrack/iorqlog/intlog/hwlog/marklog)
 *   trace_stop  <chan>       - zastavit trace kanál
 *   trace_save  <chan>, "fmt" [args] - uložit/přesměrovat segment kanálu
 *   snapshot "fmt" [args]    - uložit .mzs snapshot (vyžaduje paused emu)
 *   ramlog_start [from, to]  - spustit záznam zápisů do paměti (bez argumentů
 *                              celá paměť; výrazy se vyhodnotí při hitu)
 *   ramlog_stop / ramlog_clear - zastavit / vyprázdnit záznam zápisů
 *   coverage_start [branch|detail] - spustit pokrytí kódu (bez argumentu branch,
 *                              pokračuje do stávajících dat)
 *   coverage_stop / coverage_mark - zastavit pokrytí / nová epocha (značka)
 *   stop ["fmt" [, args]]    - zastavit emulaci (BP stop), volitelná hláška
 *                              na stdout jako u log ("[BP-STOP] ...")
 *   exit <expr> [, "fmt" [, args]] - konec běhu s kódem <expr> (0-255,
 *                              jinak dolních 8 bitů); v GUI a s MCP jen pauza
 *
 * Forward příkazy (cdl_* / trace_* / snapshot) automatizují metodiku "jeden
 * chytrý BP místo desítek ručních MCP volání" (vize 0017) - při hitu BP
 * synchronně provedou tentýž efekt jako příslušné MCP / dbgapi volání. Šablona
 * jména souboru (cdl_export / trace_save / snapshot) podporuje stejné
 * specifikátory a $var argumenty jako "log" (= sdílený bp_action_render_fmt),
 * takže např. 'trace_save cputrack, "seg-%d.bin", $id' vytvoří číslované
 * segmenty.
 *
 * Format string `log` podporuje:
 *   %X / %x  - hex (upper / lower)
 *   %d       - dec (signed)
 *   %u       - dec (unsigned 32b)
 *   %b       - binary
 *   %c       - char (LSB ASCII)
 *   %s       - string literal (jen jako parametr-volné, NEpodporuje
 *              expr argument v V1)
 *   %%       - literál %
 *
 * Lifecycle:
 *   bp_action_t *act = bp_action_parse(src, errbuf, sizeof errbuf);
 *   if (!act) ... ;
 *   bool cont = bp_action_execute(act, &ctx);  // true = pokračuj
 *   bp_action_free(act);
 *
 * ----------------------------- License -------------------------------------
 * GPL-3.0-or-later, viz licence header v breakpoints.h.
 * ---------------------------------------------------------------------------
 */

#ifndef BP_ACTION_H
#define BP_ACTION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>


    /**
     * @brief Vestavěný default minimálního odstupu těžkých FWD akcí (0019 v2).
     *
     * Aplikuje se, pokud uživatel u BP nenastaví vlastní fwd_min_interval_ms
     * (= per-BP pole je 0). Bezpečný default NENÍ nula - jinak by saturace
     * opakovanými snapshoty/trace_save zůstala možná i bez konfigurace.
     * 250 ms = max ~4 těžké akce za sekundu na jeden BP.
     */
#define BP_ACTION_FWD_DEFAULT_MIN_INTERVAL_MS  250u

    /**
     * @brief Default práh kumulativního byte backstopu pro FWD akce (0019 v3).
     *
     * Po překročení tohoto počtu bajtů zapsaných FWD akcemi emulátor sám
     * zapauzuje. 256 MiB = ochrana proti zaplnění disku při run-away BP.
     * Nastavitelné za běhu přes @c breakpoints_fwd_set_byte_limit (0 = vypnuto).
     */
#define BP_ACTION_FWD_DEFAULT_BYTE_LIMIT  ( (uint64_t) 256u * 1024u * 1024u )


    /**
     * @brief Opaque handle reprezentující parsovaný action skript.
     *
     * Vlastněný caller-em, freed přes bp_action_free(). Threading: AST
     * je read-only po parse. Executor však provádí mutace (vars, BP
     * enable/disable, memory poke, register set), takže concurrent
     * execute mezi vlákny není povolený - typicky volá jen EMU vlákno.
     */
    typedef struct st_BP_ACTION bp_action_t;


    /* Forward decl - plný typ je v bp_expr.h. Caller používá ten samý
     * kontext jako pro bp_expr_eval(). */
    struct bp_expr_ctx_s;


    /* Forward decl výrazového AST uzlu (plný typ st_BP_EXPR v bp_expr.h).
     * Slouží jako prvek pole argumentů pro bp_action_render_fmt(). */
    typedef struct st_BP_EXPR bp_expr_t;


    /**
     * @brief Parse zdrojový text action skriptu.
     *
     * @param source       Zdrojový text (UTF-8, NULL-terminated).
     *                     NULL nebo prázdný řetězec → vrátí NULL bez chyby
     *                     (caller interpretuje jako "no action = stop").
     * @param errbuf       Buffer pro chybovou hlášku (může být NULL).
     * @param errbuf_size  Velikost errbuf.
     * @return Vlastněný handle, nebo NULL při syntax chybě (errbuf
     *         se naplní). Caller musí volat bp_action_free().
     *
     * Side effects: žádné. Funkce je čistá.
     *
     * Pozn.: parser je striktní - neznámý příkaz, špatná arita, neznámý
     *        operátor = chyba (= caller dostane NULL). Komentáře (#) a
     *        prázdné řádky jsou tiše ignorovány.
     */
    extern bp_action_t* bp_action_parse ( const char *source,
                                           char *errbuf, size_t errbuf_size );


    /**
     * @brief Vykoná parsovaný action skript.
     *
     * @param action  Parsovaný skript (musí být non-NULL).
     * @param ctx     Kontext pro vyhodnocení argumentů příkazů
     *                (musí být non-NULL, předává se identicky jako
     *                pro bp_expr_eval).
     * @return true   = emulátor má pokračovat (= continue / implicit
     *                  continue); BP nezastavuje
     *         false  = emulátor má zastavit (= action je prázdná nebo
     *                  vykonala `stop` / `exit`, viz bp_action_last_halt())
     *
     * Side effects:
     *   - Mutuje user $vars storage (assignment, clear_vars).
     *   - Mutuje state breakpointů (disable_self, enable/disable).
     *   - Volá memory_write_byte (poke) a CPU register zápis (set).
     *   - Píše do stdout (log) a trace markeru (mark, V1.5+).
     *
     * Threading: musí být voláno jen z EMU vlákna (mutace CPU/memory
     * stavu).
     */
    extern bool bp_action_execute ( const bp_action_t *action,
                                     const struct bp_expr_ctx_s *ctx );


    /**
     * @brief Jak skončilo poslední bp_action_execute() (příkaz `stop` / `exit`).
     */
    typedef enum en_BP_ACTION_HALT {
        BP_ACTION_HALT_NONE = 0, /**< Akce doběhla bez `stop` / `exit`. */
        BP_ACTION_HALT_STOP,     /**< Vykonal se `stop`: běžné zastavení BP. */
        BP_ACTION_HALT_EXIT      /**< Vykonal se `exit`: žádost o konec běhu s kódem. */
    } en_BP_ACTION_HALT;

    /**
     * @brief Výsledek `stop` / `exit` posledního bp_action_execute().
     *
     * @invariant kind == BP_ACTION_HALT_EXIT => code je 0-255.
     */
    typedef struct st_BP_ACTION_HALT {
        en_BP_ACTION_HALT kind; /**< Druh zastavení. */
        int code;               /**< Návratový kód (jen EXIT). */
        const char *message;    /**< Vykreslená hláška nebo NULL; platí do dalšího bp_action_execute(). */
    } st_BP_ACTION_HALT;

    /**
     * @brief Vrátí, zda poslední bp_action_execute() skončilo příkazem `stop` / `exit`.
     *
     * "Poslední" je to nejvnější volání, které právě skončilo (vnořená akce
     * spuštěná přes `poke` a MEM_W breakpoint výsledek vnější akce
     * nepřepíše). Volá se hned po bp_action_execute() (breakpoints_enforce).
     *
     * @param[out] out Výsledek (NULL = jen dotaz).
     * @return true = poslední akce skončila `stop` nebo `exit`.
     * @pre Jen EMU vlákno.
     */
    extern bool bp_action_last_halt ( st_BP_ACTION_HALT *out );


    /**
     * @brief Uvolní paměť asociovanou s parsovaným skriptem.
     *
     * @param action AST k uvolnění (NULL = no-op).
     *
     * Postcondition: pointer je po návratu invalidní.
     */
    extern void bp_action_free ( bp_action_t *action );


    /**
     * @brief Vyrenderuje printf-style format string s vyhodnocenými argumenty.
     *
     * Sdílený renderer, který oddeluje "fmt + args -> text" od konkrétního
     * sinku (stdout u `log`, jméno souboru u snapshot/trace_save). Logika
     * je byte-identická s tím, co dříve produkoval interní renderer příkazu
     * @c log - jen vrácená jako vlastněný řetězec místo zápisu do stdout.
     *
     * Podporované specifikátory (shodné s `log`):
     *   %X / %x  - hex (upper / lower, unsigned)
     *   %d       - dec (signed)
     *   %u       - dec (unsigned 32b)
     *   %b       - binary (trim leading nul)
     *   %c       - char (LSB ASCII)
     *   %s       - arg vyhodnocen jako 16b adresa, lookup v sym_db (bank 0);
     *              nalezené jméno symbolu, jinak "<no-symbol-at-0xADDR>"
     *   %%       - literál %
     * Neznámý specifikátor se opíše doslova ('%' + znak).
     *
     * Argumenty jsou konzumovány v pořadí výskytu specifikátoru (mimo %%).
     * Pokud je specifikátorů více než argumentů, chybějící se chovají jako 0.
     * $var resolver je zajištěn přes @p ctx (argumenty jsou výrazové AST
     * uzly, které mohou odkazovat na user $vars - vyhodnocují se přes
     * bp_expr_eval).
     *
     * @param fmt     Format string (NULL = vrátí prázdný řetězec "").
     * @param args    Pole výrazových argumentů (může být NULL, pokud
     *                @p n_args == 0). Prvky se nemodifikují.
     * @param n_args  Počet platných prvků v @p args.
     * @param ctx     Kontext pro vyhodnocení argumentů (NULL = argumenty
     *                se vyhodnotí jako 0, viz eval_arg).
     * @return Nově alokovaný NUL-terminated řetězec, vlastněný callerem.
     *         Caller jej musí uvolnit přes @c g_free(). Nikdy nevrací NULL.
     *
     * Side effects: čte sym_db (lookup u %s). Nemodifikuje args/ctx.
     * Threading: re-entrant vzhledem k vlastní alokaci, ale sym_db lookup
     * a bp_expr_eval podléhají stejným omezením jako u bp_action_execute.
     */
    extern char* bp_action_render_fmt ( const char *fmt,
                                         bp_expr_t *const *args, int n_args,
                                         const struct bp_expr_ctx_s *ctx );


    /**
     * @brief Klasifikace dominantního "primárního" účinku action skriptu.
     *
     * Slouží pro vrstvu logování / UI (= Event Viewer @c BP_FIRE subtype,
     * statistiky), kde chceme jedním enum číslem charakterizovat, "co BP
     * dělá při fire" bez procházení detailního AST. Klasifikace je
     * heuristická - skutečný behavior určuje až @ref bp_action_execute().
     *
     * Pravidla (vyhodnocují se v pořadí, vrací první match):
     *
     *  1. @c action == NULL nebo obsahuje @c stop / @c exit
     *                          -> @c BP_ACTION_SUB_HALT
     *  2. obsahuje @c STMT_MARK -> @c BP_ACTION_SUB_MARK
     *  3. obsahuje @c STMT_DISABLE nebo @c STMT_DISABLE_SELF
     *                          -> @c BP_ACTION_SUB_DISABLE
     *  4. obsahuje @c STMT_ENABLE -> @c BP_ACTION_SUB_ENABLE
     *  5. jinak                -> @c BP_ACTION_SUB_CONTINUE
     *
     * Klasifikace prochází i tělo @c STMT_IF (then + else větve), aby
     * `if cond then mark "x"` byla detekována jako @c MARK.
     *
     * Hodnoty enumu jsou stabilní (= persistované v Event Viewer cfg
     * filtrech a v eventlog ringu). Sjednoceno s
     * @c en_EVENTLOG_BP_FIRE_SUB (viz @c eventlog.h) - 1:1 mapping,
     * caller nemusí překládat.
     */
    typedef enum en_BP_ACTION_SUB {
        BP_ACTION_SUB_HALT     = 0, /**< action prázdná - emu se pozastaví. */
        BP_ACTION_SUB_MARK     = 1, /**< @c STMT_MARK je přítomen v skriptu. */
        BP_ACTION_SUB_CONTINUE = 2, /**< log / poke / set / var assign / continue ... */
        BP_ACTION_SUB_IGNORE   = 3, /**< Rezervováno (DSL aktuálně nemá "ignore" stmt). */
        BP_ACTION_SUB_ENABLE   = 4, /**< @c STMT_ENABLE přítomen. */
        BP_ACTION_SUB_DISABLE  = 5, /**< @c STMT_DISABLE / @c STMT_DISABLE_SELF přítomen. */
    } en_BP_ACTION_SUB;


    /**
     * @brief Klasifikuje dominantní primární účinek skriptu.
     *
     * @param action  Parsovaný AST (může být @c NULL = mapuje na
     *                @c BP_ACTION_SUB_HALT).
     * @return  Hodnota @ref en_BP_ACTION_SUB dle pravidel popsaných u
     *          enumu.
     *
     * Side effects: žádné. Čistá funkce, čte AST.
     * Threading: re-entrant, AST je read-only.
     */
    extern en_BP_ACTION_SUB bp_action_classify_subtype ( const bp_action_t *action );


    /**
     * @brief Typ hooku pro injektovatelný monotónní časový zdroj (0019 v2).
     *
     * Vrací počet mikrosekund monotónního času (stejná sémantika jako
     * @c g_get_monotonic_time). Slouží výhradně rate-limit gate pro těžké
     * FWD akce, aby ho šlo v testech učinit deterministickým (mock clock)
     * a odstranit timing-flakiness závislou na reálném wall-clocku.
     *
     * @return Monotónní čas v mikrosekundách (musí být neklesající mezi
     *         po sobě jdoucími voláními v rámci jednoho enforce kontextu).
     */
    typedef int64_t ( *bp_action_clock_fn ) ( void );


    /**
     * @brief Nastaví injektovatelný časový zdroj pro rate-limit gate (0019 v2).
     *
     * Produkce tento hook NENASTAVUJE - default je reálný @c g_get_monotonic_time
     * a chování rate-limitu je beze změny. Slouží JEN testům k determinismu:
     * test nastaví mock clock vracející řízený čas, takže rate-limit enforce
     * (min_interval / fire pacing) nezávisí na CPU zátěži ani preempci.
     *
     * @param fn  Hook funkce vracející monotónní čas v us, nebo @c NULL pro
     *            návrat na reálný čas (g_get_monotonic_time).
     *
     * Side effects: mutuje globální (process-wide) ukazatel na časový zdroj.
     * Threading: typicky voláno z test vlákna před/po enforce sekvenci; není
     * synchronizováno - nevolat souběžně s běžící emulací používající gate.
     */
    extern void bp_action_set_clock_hook ( bp_action_clock_fn fn );


    /**
     * @brief Resetuje baseline trace_save disk-byte accountingu (0019 v3).
     *
     * Trace_save BP akce countuje do byte backstopu DELTU kumulativního tlog
     * disk čítače (@ref tlog_common_disk_bytes_total) od posledního fire - tím
     * se do auto-pauzy započítá CELÝ disk footprint trace-suite (chunk segmenty
     * + initial-state dumpy), ne jen velikost hlavního index souboru (ten
     * undercount byl V19 disk-flood vektor).
     *
     * Tato funkce nastaví baseline na aktuální total, takže historický footprint
     * zapsaný PŘED resetem accountingu se nezapočítá. Volá se z @c breakpoints_init
     * (reset emulátoru) a z @c breakpoints_fwd_reset_byte_accounting (ruční /
     * testovací reset), aby trace accounting startoval z čisté nuly konzistentně
     * se snapshot accountingem (@c g_bp_action_total_bytes).
     *
     * Side effects: přepíše interní baseline (process-globální).
     * Threading: typicky z emu vlákna (reset) nebo test vlákna; bez locku.
     */
    extern void bp_action_reset_trace_disk_baseline ( void );


    /**
     * @brief Zaregistruje flush-side byte backstop guard do trace vrstvy (0019 v3).
     *
     * Zapojí interní hook bp_action do @ref tlog_common_set_disk_flush_hook, takže
     * tlog_common při každém inkrementu disk-byte čítače (chunk swap, meta.json,
     * initial-state dump) vyhodnotí byte backstop PRŮBĚŽNĚ na flush cestě - ne jen
     * na hraně trace_save BP fire. Tím auto-pauza ohraničí i inkrementální disk
     * rust trace-suite (cputrack/trace flood bez periodické trace_save akce).
     *
     * Guard sdílí baseline accountingu s trace_save fire cestou -> žádný
     * double-account (viz @ref bp_action_install_disk_flush_guard interní hook).
     *
     * Guard účtuje jen tehdy, když nahrává aspoň jeden kanál, který spustila
     * nebo ukládá akce BP (@ref bp_action_trace_channel_note). Trace spuštěný
     * jen z GUI nebo MCP backstop nezastaví (hlídá ho vlastní max_total_mb
     * kanálu); guard i tak posune baseline, takže jeho bajty nezaúčtuje ani
     * pozdější trace_save fire.
     *
     * Volá se z @c breakpoints_init (= jednou při (re-)init debuggeru, ze stejného
     * místa, kde se resetuje byte accounting). Idempotentní (přepíše hook stejnou
     * funkcí).
     *
     * Side effects: nastaví process-globální hook v trace vrstvě.
     * Threading: voláno při inicializaci; bez locku.
     */
    extern void bp_action_install_disk_flush_guard ( void );


    /**
     * @brief Označí kanál trace jako řízený akcí BP (pro flush-side guard).
     *
     * Volá se po akci BP @c trace_start a po úspěšné akci @c trace_save
     * daného kanálu. Dokud kanál nahrává, flush-side guard jeho zápisy na disk
     * účtuje do byte backstopu. Značka zmizí akcí @c trace_stop, příkazem
     * TRACE_START/STOP z GUI nebo MCP (@ref bp_action_trace_channel_forget),
     * nebo když guard zjistí, že kanál už nenahrává.
     *
     * @param channel en_DBGAPI_TRACE_CHANNEL (0-31); jiná hodnota se ignoruje.
     * Side effects: nastaví bit v process-globální masce.
     * Threading: jen emu vlákno (akce BP, dispatch dbgapi) nebo test.
     */
    extern void bp_action_trace_channel_note ( int channel );


    /**
     * @brief Zruší značku "řízený akcí BP" u kanálu trace.
     *
     * Volá se z akce BP @c trace_stop a z dispatch příkazů TRACE_START /
     * TRACE_STOP (GUI, MCP): uživatel převzal řízení kanálu, jeho zápisy
     * už do byte backstopu nepatří, dokud ho akce BP znovu nespustí nebo
     * neuloží.
     *
     * @param channel en_DBGAPI_TRACE_CHANNEL (0-31); jiná hodnota se ignoruje.
     * Side effects: vynuluje bit v process-globální masce.
     * Threading: jen emu vlákno nebo test.
     */
    extern void bp_action_trace_channel_forget ( int channel );


#ifdef __cplusplus
}
#endif

#endif /* BP_ACTION_H */
