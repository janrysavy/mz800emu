/**
 * @file run_log.c
 * @brief Čísla běhů a kruh záznamů zastavení - implementace (viz run_log.h).
 *
 * Stav chrání jeden statický GMutex. Na konec běhu se čeká na GCond
 * s absolutním časem (g_cond_wait_until), takže falešná probuzení
 * prodlužují čekání nejvýš do zadaného timeoutu.
 */

#include "run_log.h"

#include <string.h>
#include <glib.h>


/** @brief Zámek celého stavu modulu (statická inicializace GLib nulami). */
static GMutex s_lock;

/** @brief Signál konce běhu a ukončení modulu (chráněno s_lock). */
static GCond s_cond;

/**
 * @brief Stav modulu (chráněno s_lock).
 *
 * Invarianty: `count` <= RUN_LOG_CAPACITY; nejnovější záznam je na indexu
 * (`head` + RUN_LOG_CAPACITY - 1) % RUN_LOG_CAPACITY; běh s číslem menším
 * než `seq` vždy skončil (další běh začíná až po konci předchozího).
 */
static struct
{
    uint64_t seq;               /**< Číslo aktuálního (nebo posledního) běhu. */
    bool running;               /**< Běh `seq` právě probíhá. */
    bool shutdown;              /**< run_log_shutdown() proběhlo. */
    st_RUN_LOG_RECORD ring[RUN_LOG_CAPACITY]; /**< Kruh záznamů. */
    unsigned head;              /**< Index, kam se zapíše další záznam. */
    unsigned count;             /**< Počet platných záznamů v kruhu. */
} s_rl = {
    .seq = 0,
    .running = true,
};


void run_log_record_clear(st_RUN_LOG_RECORD *rec)
{
    memset(rec, 0, sizeof(*rec));
    rec->reason = "";
    rec->started_by = "";
    rec->start_cmd = "";
    rec->stopped_by = "";
    rec->bp_id = -1;
    rec->exit_code = -1;
    rec->exit_reason = "";
}


uint64_t run_log_begin(void)
{
    g_mutex_lock(&s_lock);
    s_rl.seq++;
    s_rl.running = true;
    uint64_t seq = s_rl.seq;
    g_mutex_unlock(&s_lock);
    return seq;
}


void run_log_end(const st_RUN_LOG_RECORD *rec)
{
    g_mutex_lock(&s_lock);
    st_RUN_LOG_RECORD *slot = &s_rl.ring[s_rl.head];
    *slot = *rec;
    slot->run_seq = s_rl.seq;
    slot->detail[RUN_LOG_DETAIL_SIZE - 1] = '\0';
    s_rl.head = (s_rl.head + 1) % RUN_LOG_CAPACITY;
    if (s_rl.count < RUN_LOG_CAPACITY) s_rl.count++;
    s_rl.running = false;
    g_cond_broadcast(&s_cond);
    g_mutex_unlock(&s_lock);
}


uint64_t run_log_current(bool *running)
{
    g_mutex_lock(&s_lock);
    uint64_t seq = s_rl.seq;
    if (running) *running = s_rl.running;
    g_mutex_unlock(&s_lock);
    return seq;
}


/**
 * @brief Najde v kruhu záznam běhu @p run_seq.
 *
 * @return Ukazatel do kruhu, nebo NULL (záznam už přepsán nebo neexistuje).
 * @pre Volající drží s_lock.
 */
static const st_RUN_LOG_RECORD *run_log_find_locked(uint64_t run_seq)
{
    for (unsigned i = 0; i < s_rl.count; i++)
    {
        unsigned idx = (s_rl.head + RUN_LOG_CAPACITY - 1 - i) % RUN_LOG_CAPACITY;
        if (s_rl.ring[idx].run_seq == run_seq) return &s_rl.ring[idx];
    };
    return NULL;
}


/**
 * @brief Skončil už běh @p run_seq?
 *
 * Běh skončil, pokud je menší než aktuální číslo (každý další běh začíná
 * až po konci předchozího), nebo je to aktuální běh a neprobíhá.
 *
 * @pre Volající drží s_lock.
 */
static bool run_log_is_stopped_locked(uint64_t run_seq)
{
    if (run_seq < s_rl.seq) return true;
    if (run_seq == s_rl.seq) return !s_rl.running;
    return false;
}


en_RUN_LOG_WAIT run_log_wait(uint64_t run_seq, int timeout_ms,
                             st_RUN_LOG_RECORD *out, bool *found)
{
    if (timeout_ms < 0) timeout_ms = 0;
    if (timeout_ms > RUN_LOG_WAIT_MAX_MS) timeout_ms = RUN_LOG_WAIT_MAX_MS;
    if (found) *found = false;

    gint64 end_time = g_get_monotonic_time() + (gint64)timeout_ms * G_TIME_SPAN_MILLISECOND;
    en_RUN_LOG_WAIT result;

    g_mutex_lock(&s_lock);
    for (;;)
    {
        if (run_log_is_stopped_locked(run_seq))
        {
            const st_RUN_LOG_RECORD *rec = run_log_find_locked(run_seq);
            if (rec)
            {
                if (out) *out = *rec;
                if (found) *found = true;
            };
            result = RUN_LOG_WAIT_DONE;
            break;
        };
        if (s_rl.shutdown)
        {
            result = RUN_LOG_WAIT_SHUTDOWN;
            break;
        };
        if (!g_cond_wait_until(&s_cond, &s_lock, end_time))
        {
            /* Timeout: ještě jednou zkontrolovat stav (konec mohl přijít
             * těsně před vypršením). */
            if (run_log_is_stopped_locked(run_seq)) continue;
            result = s_rl.shutdown ? RUN_LOG_WAIT_SHUTDOWN : RUN_LOG_WAIT_TIMEOUT;
            break;
        };
    };
    g_mutex_unlock(&s_lock);
    return result;
}


unsigned run_log_history(st_RUN_LOG_RECORD *out, unsigned max)
{
    if (!out || max == 0) return 0;
    g_mutex_lock(&s_lock);
    unsigned n = (s_rl.count < max) ? s_rl.count : max;
    /* Nejnovějších n záznamů, vrácených od nejstaršího. */
    for (unsigned i = 0; i < n; i++)
    {
        unsigned idx = (s_rl.head + RUN_LOG_CAPACITY - n + i) % RUN_LOG_CAPACITY;
        out[i] = s_rl.ring[idx];
    };
    g_mutex_unlock(&s_lock);
    return n;
}


void run_log_shutdown(void)
{
    g_mutex_lock(&s_lock);
    s_rl.shutdown = true;
    g_cond_broadcast(&s_cond);
    g_mutex_unlock(&s_lock);
}


void run_log_reset_for_tests(void)
{
    g_mutex_lock(&s_lock);
    memset(&s_rl, 0, sizeof(s_rl));
    s_rl.seq = 0;
    s_rl.running = true;
    g_mutex_unlock(&s_lock);
}
