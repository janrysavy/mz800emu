/**
 * @file   ramlog.c
 * @brief  Záznam zápisů do paměti - jádro (viz ramlog.h).
 *
 * @par Licence: GPLv3
 */

#include "ramlog.h"

#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(st_RAMLOG_REC) == 32, "st_RAMLOG_REC musí mít 32 B (kanál ramlog trace_read)");
_Static_assert(sizeof(st_RAMLOG_MAPENT) == 8, "st_RAMLOG_MAPENT musí mít 8 B");

int g_ramlog_active = 0;
uint8_t g_ramlog_src = RAMLOG_SRC_INSN;
uint16_t g_ramlog_src_pc = 0;

/**
 * @brief Stav modulu (jen emu vlákno).
 *
 * @invariant ring != NULL => capacity >= RAMLOG_MIN_CAPACITY.
 * @invariant first_seq = (mode == RING && next_seq > capacity) ? next_seq - capacity : 0.
 */
static struct {
    st_RAMLOG_REC *ring;      /**< Ring záznamů (capacity položek), NULL = nic. */
    uint32_t capacity;        /**< Kapacita ringu. */
    en_RAMLOG_MODE mode;      /**< Režim. */
    uint64_t next_seq;        /**< Příští seq v ringu. */
    uint64_t total;           /**< Všechny zaznamenané zápisy (seq posledního + 1 i mimo ring). */
    uint64_t dropped;         /**< Nepřijaté v režimu fill. */
    unsigned segment;         /**< Pořadí segmentu. */
    unsigned n_ranges;        /**< Počet rozsahů filtru. */
    st_RAMLOG_RANGE ranges[RAMLOG_MAX_RANGES]; /**< Rozsahy filtru. */
    uint8_t cpu_bitmap[0x10000 / 8]; /**< Bitmapa CPU adres z CPU rozsahů. */
    bool any_cpu;             /**< Aspoň jeden CPU rozsah. */
    bool any_phys_ram;        /**< Fyzický rozsah v RAM nebo MemExt (stránky nelze předpočítat). */
    st_RAMLOG_MAPENT *map[RAMLOG_SPACE_COUNT]; /**< Mapa posledního zapisovatele po prostorech. */
    uint32_t map_size[RAMLOG_SPACE_COUNT];     /**< Velikost prostorů [buňky]. */
} s_rl;

static unsigned s_segment_counter = 0;


uint32_t ramlog_source_push(en_RAMLOG_SRC src, uint16_t pc)
{
    uint32_t saved = (uint32_t) g_ramlog_src | ((uint32_t) g_ramlog_src_pc << 16);
    g_ramlog_src = (uint8_t) src;
    g_ramlog_src_pc = pc;
    return saved;
}


void ramlog_source_pop(uint32_t saved)
{
    g_ramlog_src = (uint8_t) (saved & 0xFFu);
    g_ramlog_src_pc = (uint16_t) (saved >> 16);
}


/** @brief Uvolní ring a mapy. */
static void rl_release(void)
{
    free(s_rl.ring);
    s_rl.ring = NULL;
    for (unsigned i = 0; i < RAMLOG_SPACE_COUNT; i++) {
        free(s_rl.map[i]);
        s_rl.map[i] = NULL;
    }
}


/** @brief Nejstarší seq v ringu. */
static uint64_t rl_first(void)
{
    if (s_rl.mode == RAMLOG_MODE_RING && s_rl.next_seq > s_rl.capacity) return s_rl.next_seq - s_rl.capacity;
    return 0;
}


bool ramlog_start(const st_RAMLOG_CONFIG *cfg, const uint32_t space_size[RAMLOG_SPACE_COUNT])
{
    g_ramlog_active = 0;
    rl_release();
    uint32_t cap = cfg->capacity;
    if (cap < RAMLOG_MIN_CAPACITY) cap = RAMLOG_MIN_CAPACITY;
    if (cap > RAMLOG_MAX_CAPACITY) cap = RAMLOG_MAX_CAPACITY;
    s_rl.ring = (st_RAMLOG_REC *) malloc((size_t) cap * sizeof(st_RAMLOG_REC));
    bool ok = (s_rl.ring != NULL);
    for (unsigned i = 0; i < RAMLOG_SPACE_COUNT && ok; i++) {
        s_rl.map_size[i] = (i == RAMLOG_SPACE_ROM) ? 0 : space_size[i];
        /* MemExt až při prvním zápisu do něj. */
        if (i != RAMLOG_SPACE_MEMEXT && s_rl.map_size[i]) {
            s_rl.map[i] = (st_RAMLOG_MAPENT *) calloc(s_rl.map_size[i], sizeof(st_RAMLOG_MAPENT));
            ok = (s_rl.map[i] != NULL);
        }
    }
    if (!ok) {
        rl_release();
        return false;
    }
    s_rl.capacity = cap;
    s_rl.mode = cfg->mode;
    s_rl.next_seq = 0;
    s_rl.total = 0;
    s_rl.dropped = 0;
    s_rl.segment = ++s_segment_counter;
    s_rl.n_ranges = (cfg->n_ranges > RAMLOG_MAX_RANGES) ? RAMLOG_MAX_RANGES : cfg->n_ranges;
    memcpy(s_rl.ranges, cfg->ranges, sizeof(s_rl.ranges));
    memset(s_rl.cpu_bitmap, 0, sizeof(s_rl.cpu_bitmap));
    s_rl.any_cpu = false;
    s_rl.any_phys_ram = false;
    for (unsigned i = 0; i < s_rl.n_ranges; i++) {
        const st_RAMLOG_RANGE *r = &s_rl.ranges[i];
        if (!r->phys) {
            uint32_t to = (r->to > 0xFFFFu) ? 0xFFFFu : r->to;
            for (uint32_t a = r->from; a <= to; a++) s_rl.cpu_bitmap[a >> 3] |= (uint8_t) (1u << (a & 7u));
            s_rl.any_cpu = true;
        } else if (r->space == RAMLOG_SPACE_RAM || r->space == RAMLOG_SPACE_MEMEXT) {
            s_rl.any_phys_ram = true;
        }
    }
    g_ramlog_active = 1;
    return true;
}


void ramlog_stop(void)
{
    g_ramlog_active = 0;
}


void ramlog_clear(void)
{
    if (!s_rl.ring) return;
    s_rl.next_seq = 0;
    s_rl.total = 0;
    s_rl.dropped = 0;
    s_rl.segment = ++s_segment_counter;
    for (unsigned i = 0; i < RAMLOG_SPACE_COUNT; i++) {
        if (s_rl.map[i]) memset(s_rl.map[i], 0, (size_t) s_rl.map_size[i] * sizeof(st_RAMLOG_MAPENT));
    }
}


void ramlog_free(void)
{
    g_ramlog_active = 0;
    rl_release();
}


bool ramlog_match(uint16_t cpu_addr, bool has_cpu, en_RAMLOG_SPACE space, uint32_t offset)
{
    if (s_rl.n_ranges == 0) return true;
    if (has_cpu && s_rl.any_cpu && (s_rl.cpu_bitmap[cpu_addr >> 3] & (1u << (cpu_addr & 7u)))) return true;
    for (unsigned i = 0; i < s_rl.n_ranges; i++) {
        const st_RAMLOG_RANGE *r = &s_rl.ranges[i];
        if (r->phys && r->space == (uint8_t) space && offset >= r->from && offset <= r->to) return true;
    }
    return false;
}


bool ramlog_page_watched(unsigned page)
{
    if (s_rl.n_ranges == 0 || s_rl.any_phys_ram) return true;
    if (!s_rl.any_cpu || page > 15) return false;
    const uint8_t *b = &s_rl.cpu_bitmap[(page << 12) >> 3];
    for (unsigned i = 0; i < (0x1000u >> 3); i++) {
        if (b[i]) return true;
    }
    return false;
}


/** @brief Mapa prostoru (MemExt alokuje líně), NULL = prostor nemá mapu. */
static st_RAMLOG_MAPENT *rl_map(unsigned space)
{
    if (space >= RAMLOG_SPACE_COUNT || s_rl.map_size[space] == 0) return NULL;
    if (!s_rl.map[space] && space == RAMLOG_SPACE_MEMEXT) {
        s_rl.map[space] = (st_RAMLOG_MAPENT *) calloc(s_rl.map_size[space], sizeof(st_RAMLOG_MAPENT));
    }
    return s_rl.map[space];
}


void ramlog_record(const st_RAMLOG_REC *r)
{
    if (!s_rl.ring) return;
    uint64_t seq = s_rl.total++;
    if (s_rl.mode == RAMLOG_MODE_FILL && s_rl.next_seq >= s_rl.capacity) {
        s_rl.dropped++;
    } else {
        s_rl.ring[s_rl.next_seq % s_rl.capacity] = *r;
        s_rl.next_seq++;
    }
    if (r->flags & (RAMLOG_FLAG_SUPPRESSED | RAMLOG_FLAG_NO_MAP)) return;
    st_RAMLOG_MAPENT *m = rl_map(r->space);
    if (!m) return;
    st_RAMLOG_MAPENT e = { (uint32_t) (seq + 1u), r->insn_pc, r->src, r->flags };
    uint32_t n = (r->flags & RAMLOG_FLAG_SUMMARY) ? (r->count ? r->count : 0x10000u) : 1u;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t off = r->offset + i;
        if (off >= s_rl.map_size[r->space]) break;
        m[off] = e;
    }
}


bool ramlog_lastwriter(en_RAMLOG_SPACE space, uint32_t offset, st_RAMLOG_MAPENT *out)
{
    memset(out, 0, sizeof(*out));
    if ((unsigned) space >= RAMLOG_SPACE_COUNT || !s_rl.map[space] || offset >= s_rl.map_size[space]) return false;
    *out = s_rl.map[space][offset];
    return out->seq1 != 0;
}


void ramlog_get_status(st_RAMLOG_STATUS *out)
{
    memset(out, 0, sizeof(*out));
    out->active = g_ramlog_active != 0;
    out->allocated = s_rl.ring != NULL;
    out->segment = s_rl.ring ? s_rl.segment : 0;
    out->mode = s_rl.mode;
    out->capacity = s_rl.capacity;
    out->first_seq = rl_first();
    out->next_seq = s_rl.next_seq;
    out->total = s_rl.total;
    out->dropped = s_rl.dropped;
    out->overwritten = rl_first();
    out->n_ranges = s_rl.n_ranges;
    memcpy(out->ranges, s_rl.ranges, sizeof(out->ranges));
    if (s_rl.ring) out->memory_bytes = (uint64_t) s_rl.capacity * sizeof(st_RAMLOG_REC);
    for (unsigned i = 0; i < RAMLOG_SPACE_COUNT; i++) {
        if (s_rl.map[i]) out->memory_bytes += (uint64_t) s_rl.map_size[i] * sizeof(st_RAMLOG_MAPENT);
    }
}


bool ramlog_get(uint64_t seq, st_RAMLOG_REC *out)
{
    if (!s_rl.ring || seq < rl_first() || seq >= s_rl.next_seq) return false;
    *out = s_rl.ring[seq % s_rl.capacity];
    return true;
}


/** @brief Pokrývá záznam buňku [space, offset]? */
static bool rl_covers(const st_RAMLOG_REC *r, unsigned space, uint32_t offset)
{
    if (r->space != space) return false;
    if (r->flags & RAMLOG_FLAG_SUMMARY) {
        uint32_t n = r->count ? r->count : 0x10000u;
        return offset >= r->offset && offset - r->offset < n;
    }
    return r->offset == offset;
}


size_t ramlog_recent(uint64_t *out_seq, st_RAMLOG_REC *out, size_t max,
                     bool skip_unchanged, uint64_t scan_max)
{
    size_t n = 0;
    if (!s_rl.ring) return 0;
    uint64_t first = rl_first();
    uint64_t stop = (scan_max && s_rl.next_seq - first > scan_max) ? s_rl.next_seq - scan_max : first;
    for (uint64_t seq = s_rl.next_seq; seq > stop && n < max; seq--) {
        const st_RAMLOG_REC *r = &s_rl.ring[(seq - 1) % s_rl.capacity];
        if (skip_unchanged && (r->flags & (RAMLOG_FLAG_UNCHANGED | RAMLOG_FLAG_SUPPRESSED))) continue;
        if (out_seq) out_seq[n] = seq - 1;
        out[n++] = *r;
    }
    return n;
}


size_t ramlog_who_wrote(en_RAMLOG_SPACE space, uint32_t offset, uint64_t *out_seq,
                        st_RAMLOG_REC *out, size_t max, bool skip_unchanged)
{
    size_t n = 0;
    if (!s_rl.ring) return 0;
    uint64_t first = rl_first();
    for (uint64_t seq = s_rl.next_seq; seq > first && n < max; seq--) {
        const st_RAMLOG_REC *r = &s_rl.ring[(seq - 1) % s_rl.capacity];
        if (!rl_covers(r, (unsigned) space, offset)) continue;
        if (skip_unchanged && (r->flags & (RAMLOG_FLAG_UNCHANGED | RAMLOG_FLAG_SUPPRESSED))) continue;
        if (out_seq) out_seq[n] = seq - 1;
        out[n++] = *r;
    }
    return n;
}


/** @brief Řazení zapisovatelů podle počtu sestupně, pak podle insn_pc. */
static int rl_cmp_writer(const void *a, const void *b)
{
    const st_RAMLOG_WRITER *x = (const st_RAMLOG_WRITER *) a, *y = (const st_RAMLOG_WRITER *) b;
    if (x->count != y->count) return (x->count < y->count) ? 1 : -1;
    if (x->insn_pc != y->insn_pc) return (int) x->insn_pc - (int) y->insn_pc;
    return (int) x->src - (int) y->src;
}


/**
 * @brief Souhrn zapisovatelů záznamů, které vybere predikát.
 *
 * @param by_cpu true = rozsah CPU adres záznamu (cpu_addr), false = buňky prostoru.
 */
static size_t rl_writers(bool by_cpu, unsigned space, uint32_t from, uint32_t to,
                         st_RAMLOG_WRITER *out, size_t max, size_t *total, bool skip_unchanged)
{
    if (total) *total = 0;
    if (!s_rl.ring) return 0;
    /* Klíč insn_pc + zdroj: 64 K x RAMLOG_SRC_COUNT indexů do pole souhrnů. */
    size_t cap = 256, n = 0;
    st_RAMLOG_WRITER *acc = (st_RAMLOG_WRITER *) malloc(cap * sizeof(*acc));
    uint32_t *idx = (uint32_t *) calloc(0x10000u * RAMLOG_SRC_COUNT, sizeof(uint32_t));
    if (!acc || !idx) {
        free(acc);
        free(idx);
        return 0;
    }
    uint64_t first = rl_first();
    for (uint64_t seq = first; seq < s_rl.next_seq; seq++) {
        const st_RAMLOG_REC *r = &s_rl.ring[seq % s_rl.capacity];
        uint32_t n_cells = (r->flags & RAMLOG_FLAG_SUMMARY) ? (r->count ? r->count : 0x10000u) : 1u;
        if (by_cpu) {
            if (r->flags & RAMLOG_FLAG_NO_CPU) continue;
            if (r->cpu_addr > to || (uint32_t) r->cpu_addr + n_cells - 1u < from) continue;
        } else {
            if (r->space != (uint8_t) space) continue;
            if (r->offset > to || r->offset + n_cells - 1u < from) continue;
        }
        if (skip_unchanged && (r->flags & (RAMLOG_FLAG_UNCHANGED | RAMLOG_FLAG_SUPPRESSED))) continue;
        unsigned src = (r->src < RAMLOG_SRC_COUNT) ? r->src : 0;
        uint32_t key = (uint32_t) r->insn_pc * RAMLOG_SRC_COUNT + src;
        if (!idx[key]) {
            if (n == cap) {
                cap *= 2;
                st_RAMLOG_WRITER *na = (st_RAMLOG_WRITER *) realloc(acc, cap * sizeof(*acc));
                if (!na) break;
                acc = na;
            }
            st_RAMLOG_WRITER w = { r->insn_pc, (uint8_t) src, 0, seq, seq };
            acc[n] = w;
            idx[key] = (uint32_t) ++n;
        }
        st_RAMLOG_WRITER *w = &acc[idx[key] - 1];
        w->count++;
        w->last_seq = seq;
    }
    qsort(acc, n, sizeof(*acc), rl_cmp_writer);
    size_t k = (n < max) ? n : max;
    if (k) memcpy(out, acc, k * sizeof(*acc));
    if (total) *total = n;
    free(acc);
    free(idx);
    return k;
}


size_t ramlog_writers(en_RAMLOG_SPACE space, uint32_t from, uint32_t to,
                      st_RAMLOG_WRITER *out, size_t max, size_t *total, bool skip_unchanged)
{
    return rl_writers(false, (unsigned) space, from, to, out, max, total, skip_unchanged);
}


size_t ramlog_writers_cpu(uint16_t from, uint16_t to,
                          st_RAMLOG_WRITER *out, size_t max, size_t *total, bool skip_unchanged)
{
    return rl_writers(true, 0, from, to, out, max, total, skip_unchanged);
}


uint64_t ramlog_copy(uint64_t from_seq, uint64_t count, st_RAMLOG_REC *dst, uint64_t *out_seq)
{
    uint64_t first = rl_first();
    uint64_t lo = (from_seq < first) ? first : from_seq;
    *out_seq = lo;
    if (!s_rl.ring || lo >= s_rl.next_seq) return 0;
    uint64_t n = s_rl.next_seq - lo;
    if (n > count) n = count;
    for (uint64_t i = 0; i < n; i++) dst[i] = s_rl.ring[(lo + i) % s_rl.capacity];
    return n;
}


const char *ramlog_space_name(unsigned space)
{
    static const char *const names[RAMLOG_SPACE_COUNT] = { "ram", "vram", "pcg", "memext", "rom" };
    return (space < RAMLOG_SPACE_COUNT) ? names[space] : "?";
}


const char *ramlog_src_name(unsigned src)
{
    static const char *const names[RAMLOG_SRC_COUNT] = {
        "insn", "int_ack", "nmi_ack", "debugger", "poke", "freeze", "cmt_hack", "load"
    };
    return (src < RAMLOG_SRC_COUNT) ? names[src] : "?";
}


int ramlog_space_from_name(const char *name)
{
    for (unsigned i = 0; name && i < RAMLOG_SPACE_COUNT; i++) {
        if (strcmp(name, ramlog_space_name(i)) == 0) return (int) i;
    }
    return -1;
}
