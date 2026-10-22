/**
 * @file   state_hash.c
 * @brief  Sdílený hash SHA-256 částí stavu stroje - viz state_hash.h.
 */

#include <string.h>
#include <glib.h>

#include "state_hash.h"
#include "mzarch/mzarch_config.h"
#include "hw-generic/memory/memory.h"
#include "hw-generic/memory/memext.h"
#include "hw-generic/gdg/framebuffer.h"
#include "mzarch/mzarch.h"
#include "libs/cpu-z80/z80.h"


void state_hash_buffer(const void *data, size_t len, char *out_hex)
{
    GChecksum *cs = g_checksum_new(G_CHECKSUM_SHA256);
    if (len)
    {
        g_checksum_update(cs, (const guchar *)data, (gssize)len);
    };
    /* g_checksum_get_string vrací malá hex písmena, 64 znaků. */
    g_strlcpy(out_hex, g_checksum_get_string(cs), STATE_HASH_HEX_LEN + 1);
    g_checksum_free(cs);
}


const char *state_hash_part_name(en_STATE_HASH_PART part)
{
    switch (part)
    {
        case STATE_HASH_RAM: return "ram";
        case STATE_HASH_MEMEXT: return "memext";
        case STATE_HASH_VRAM: return "vram";
        case STATE_HASH_PCG: return "pcg";
        case STATE_HASH_SCREEN: return "screen";
        case STATE_HASH_CPU: return "cpu";
        default: break;
    };
    return "unknown";
}


bool state_hash_part(en_STATE_HASH_PART part, char *out_hex)
{
    switch (part)
    {
        case STATE_HASH_RAM:
            state_hash_buffer(g_memory.RAM, sizeof(g_memory.RAM), out_hex);
            return true;

        case STATE_HASH_MEMEXT:
            if (!MEMEXT_TEST_CONNECTED) return false;
            state_hash_buffer(g_memext.RAM, sizeof(g_memext.RAM), out_hex);
            return true;

        case STATE_HASH_VRAM:
        {
#if MZARCH == 800
            /* Plány I+II (VRAM) a III+IV (EXVRAM) jedním hashem, v tomto pořadí. */
            GChecksum *cs = g_checksum_new(G_CHECKSUM_SHA256);
            g_checksum_update(cs, g_memory.VRAM, sizeof(g_memory.VRAM));
            g_checksum_update(cs, g_memory.EXVRAM, sizeof(g_memory.EXVRAM));
            g_strlcpy(out_hex, g_checksum_get_string(cs), STATE_HASH_HEX_LEN + 1);
            g_checksum_free(cs);
#else
            state_hash_buffer(g_memory.VRAM, sizeof(g_memory.VRAM), out_hex);
#endif
            return true;
        };

        case STATE_HASH_PCG:
#if MZARCH == 1500
            state_hash_buffer(g_memory.PCG, sizeof(g_memory.PCG), out_hex);
            return true;
#else
            return false;
#endif

        case STATE_HASH_SCREEN:
            if (!g_framebuffer.pixels) return false;
            state_hash_buffer(g_framebuffer.pixels, GDG_FRAMEBUFFER_PIXBUF_SIZE, out_hex);
            return true;

        case STATE_HASH_CPU:
        {
            if (!g_mzarch_main.cpu) return false;
            unsigned char bytes[STATE_HASH_CPU_BYTES];
            state_hash_cpu_bytes(bytes);
            state_hash_buffer(bytes, sizeof(bytes), out_hex);
            return true;
        };

        default:
            break;
    };
    return false;
}


void state_hash_cpu_bytes(unsigned char *out)
{
    static const z80_reg_t pairs[] = {
        Z80_REG_AF, Z80_REG_BC, Z80_REG_DE, Z80_REG_HL,
        Z80_REG_AF2, Z80_REG_BC2, Z80_REG_DE2, Z80_REG_HL2,
        Z80_REG_IX, Z80_REG_IY, Z80_REG_SP, Z80_REG_PC, Z80_REG_WZ,
    };
    z80_t *cpu = g_mzarch_main.cpu;
    size_t n = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(pairs); i++)
    {
        uint16_t v = z80_get_reg(cpu, pairs[i]);
        out[n++] = (unsigned char)(v & 0xFF);
        out[n++] = (unsigned char)(v >> 8);
    };
    out[n++] = cpu->i;
    out[n++] = cpu->r;
    out[n++] = cpu->iff1;
    out[n++] = cpu->iff2;
    out[n++] = cpu->im;
    out[n++] = cpu->halted ? 1 : 0;
    out[n++] = cpu->int_pending ? 1 : 0;
    out[n++] = cpu->nmi_pending ? 1 : 0;
    out[n++] = cpu->ei_delay ? 1 : 0;
    out[n++] = cpu->ld_a_ir ? 1 : 0;
    out[n++] = cpu->int_vector;
    out[n++] = cpu->q;
    g_assert(n == STATE_HASH_CPU_BYTES);
}
