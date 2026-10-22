/**
 * @file   coverage_decode.c
 * @brief  Dekódování kroku Z80 pro pokrytí kódu - implementace (viz coverage_decode.h).
 *
 * Délky instrukcí jsou tabulka pro opkódy bez prefixu a dvě malé funkce pro
 * ED a pro instrukce s IX/IY. Správnost tabulek hlídá test
 * tests/debugger/test_coverage_decode.c proti disassembleru z80_dasm()
 * a proti skutečnému kroku jádra.
 *
 * @par Licence: GPLv3
 */

#include "coverage_decode.h"

#include <stddef.h>


/** @brief Prefix DD (IX). */
#define CD_PREFIX_DD 0xDD
/** @brief Prefix FD (IY). */
#define CD_PREFIX_FD 0xFD
/** @brief Prefix CB (bitové operace). */
#define CD_PREFIX_CB 0xCB
/** @brief Prefix ED (rozšířené instrukce). */
#define CD_PREFIX_ED 0xED
/** @brief Opkód HALT. */
#define CD_OP_HALT 0x76


/**
 * @brief Délka instrukce bez prefixu podle prvního bajtu.
 *
 * Hodnota 0 je u bajtů, které délku samy neurčují (CBh, DDh, EDh, FDh);
 * ty dekodér obsluhuje zvlášť.
 */
static const uint8_t s_base_len[ 256 ] = {
    /*        0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
    /* 0x */  1, 3, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 2, 1,
    /* 1x */  2, 3, 1, 1, 1, 1, 2, 1, 2, 1, 1, 1, 1, 1, 2, 1,
    /* 2x */  2, 3, 3, 1, 1, 1, 2, 1, 2, 1, 3, 1, 1, 1, 2, 1,
    /* 3x */  2, 3, 3, 1, 1, 1, 2, 1, 2, 1, 3, 1, 1, 1, 2, 1,
    /* 4x */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* 5x */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* 6x */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* 7x */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* 8x */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* 9x */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* Ax */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* Bx */  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    /* Cx */  1, 1, 3, 3, 3, 1, 2, 1, 1, 1, 3, 0, 3, 3, 2, 1,
    /* Dx */  1, 1, 3, 2, 3, 1, 2, 1, 1, 1, 3, 2, 3, 0, 2, 1,
    /* Ex */  1, 1, 3, 1, 3, 1, 2, 1, 1, 1, 3, 1, 3, 0, 2, 1,
    /* Fx */  1, 1, 3, 1, 3, 1, 2, 1, 1, 1, 3, 1, 3, 0, 2, 1,
};


/**
 * @brief Délka instrukce s IX/IY za prefixem DD/FD (bez prefixu).
 *
 * Seznam odpovídá větvím makra DDFD_PREFIX v z80_execute.inc; ostatní
 * bajty jádro provede jako opkód bez prefixu.
 *
 * @param op Bajt za posledním prefixem.
 * @return Počet bajtů od @p op do konce instrukce (CBh: CB d op = 3),
 *         0 = nejde o instrukci s IX/IY.
 */
static unsigned cd_index_len ( uint8_t op )
{
    switch ( op ) {
        /* (IX+d) a (IX+d),n */
        case 0x34: case 0x35:
        case 0x46: case 0x4E: case 0x56: case 0x5E: case 0x66: case 0x6E: case 0x7E:
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x77:
        case 0x86: case 0x8E: case 0x96: case 0x9E: case 0xA6: case 0xAE: case 0xB6: case 0xBE:
            return 2;
        case 0x36:
            return 3;
        /* LD IX,nn, LD (nn),IX, LD IX,(nn) */
        case 0x21: case 0x22: case 0x2A:
            return 3;
        /* LD IXH,n a LD IXL,n */
        case 0x26: case 0x2E:
            return 2;
        /* DD CB d op */
        case 0xCB:
            return 3;
        /* ADD IX,rr, INC/DEC IX, IXH a IXL v aritmetice a přesunech, POP, EX (SP),
         * PUSH, JP (IX), LD SP,IX */
        case 0x09: case 0x19: case 0x29: case 0x39:
        case 0x23: case 0x24: case 0x25: case 0x2B: case 0x2C: case 0x2D:
        case 0x44: case 0x45: case 0x4C: case 0x4D: case 0x54: case 0x55: case 0x5C: case 0x5D:
        case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x67:
        case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6F:
        case 0x7C: case 0x7D:
        case 0x84: case 0x85: case 0x8C: case 0x8D: case 0x94: case 0x95: case 0x9C: case 0x9D:
        case 0xA4: case 0xA5: case 0xAC: case 0xAD: case 0xB4: case 0xB5: case 0xBC: case 0xBD:
        case 0xE1: case 0xE3: case 0xE5: case 0xE9: case 0xF9:
            return 1;
        default:
            return 0;
    };
}


/**
 * @brief Délka instrukce s prefixem ED (včetně EDh).
 * @param op Bajt za EDh.
 * @return 4 u LD (nn),rr a LD rr,(nn), jinak 2 (i nedefinované ED = NOP).
 */
static unsigned cd_ed_len ( uint8_t op )
{
    switch ( op ) {
        case 0x43: case 0x4B: case 0x53: case 0x5B:
        case 0x63: case 0x6B: case 0x73: case 0x7B:
            return 4;
        default:
            return 2;
    };
}


/**
 * @brief Zjistí, zda je bajt za EDh opakující bloková instrukce.
 * @param op Bajt za EDh.
 * @return true u LDIR, CPIR, INIR, OTIR, LDDR, CPDR, INDR, OTDR.
 */
static bool cd_ed_is_repeat ( uint8_t op )
{
    return ( op >= 0xB0 && op <= 0xB3 ) || ( op >= 0xB8 && op <= 0xBB );
}


/**
 * @brief Doplní podmínku instrukce bez prefixu (DJNZ, JR cc, RET/JP/CALL cc).
 * @param op  Opkód.
 * @param out Výsledek; mění jen `cond` a `cc`.
 */
static void cd_base_cond ( uint8_t op, st_COVERAGE_INSN *out )
{
    if ( op == 0x10 ) {
        out->cond = COVERAGE_COND_DJNZ;
    } else if ( op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38 ) {
        out->cond = COVERAGE_COND_FLAG;
        out->cc = (uint8_t) ( ( op >> 3 ) & 0x03 );
    } else if ( ( op & 0xC7 ) == 0xC0 || ( op & 0xC7 ) == 0xC2 || ( op & 0xC7 ) == 0xC4 ) {
        out->cond = COVERAGE_COND_FLAG;
        out->cc = (uint8_t) ( ( op >> 3 ) & 0x07 );
    };
}


void coverage_decode ( coverage_decode_read_fn read, void *ctx, st_COVERAGE_INSN *out )
{
    *out = ( st_COVERAGE_INSN ) { 0 };

    unsigned p = 0;
    uint8_t op = read ( ctx, 0 );
    while ( op == CD_PREFIX_DD || op == CD_PREFIX_FD ) {
        p++;
        op = read ( ctx, p );
        if ( p == COVERAGE_DECODE_PREFIX_MAX && ( op == CD_PREFIX_DD || op == CD_PREFIX_FD ) ) {
            /* 65. prefix: jádro krok rozdělí, bajt se načte znovu v dalším kroku. */
            out->len = COVERAGE_DECODE_PREFIX_MAX;
            out->prefixes = COVERAGE_DECODE_PREFIX_MAX;
            out->op_len = COVERAGE_DECODE_PREFIX_MAX;
            out->prefix_split = true;
            return;
        };
    };
    out->prefixes = (uint8_t) p;

    if ( p > 0 ) {
        unsigned idx = cd_index_len ( op );
        if ( idx > 0 ) {
            out->len = (uint8_t) ( p + idx );
            out->op_len = (uint8_t) ( p + 1 );
            out->op_last = ( op == CD_PREFIX_CB );
            return;
        };
    };

    if ( op == CD_PREFIX_CB ) {
        out->len = (uint8_t) ( p + 2 );
        out->op_len = (uint8_t) ( p + 2 );
        return;
    };

    if ( op == CD_PREFIX_ED ) {
        uint8_t e = read ( ctx, p + 1 );
        out->len = (uint8_t) ( p + cd_ed_len ( e ) );
        out->op_len = (uint8_t) ( p + 2 );
        if ( cd_ed_is_repeat ( e ) ) {
            out->cond = COVERAGE_COND_REPEAT;
        };
        return;
    };

    out->len = (uint8_t) ( p + s_base_len[ op ] );
    out->op_len = (uint8_t) ( p + 1 );
    out->halt = ( op == CD_OP_HALT );
    cd_base_cond ( op, out );
}


/** @brief Kontext coverage_decode_bytes(): pole bajtů a jeho délka. */
typedef struct st_CD_BYTES {
    const uint8_t *bytes; /**< Bajty kroku (nevlastněné). */
    unsigned n;           /**< Počet platných bajtů. */
} st_CD_BYTES;


/**
 * @brief Čtení bajtu z pole pro coverage_decode_bytes().
 * @param ctx   st_CD_BYTES.
 * @param index Pozice bajtu.
 * @return Bajt, za koncem pole 00h.
 */
static uint8_t cd_read_bytes ( void *ctx, unsigned index )
{
    const st_CD_BYTES *b = (const st_CD_BYTES *) ctx;
    return ( index < b->n ) ? b->bytes[ index ] : 0x00;
}


void coverage_decode_bytes ( const uint8_t *bytes, unsigned n, st_COVERAGE_INSN *out )
{
    st_CD_BYTES b = { bytes, n };
    coverage_decode ( cd_read_bytes, &b, out );
}


bool coverage_decode_cc_true ( uint8_t cc, uint8_t f )
{
    switch ( cc & 0x07 ) {
        case 0: return ( f & 0x40 ) == 0;   /* NZ */
        case 1: return ( f & 0x40 ) != 0;   /* Z */
        case 2: return ( f & 0x01 ) == 0;   /* NC */
        case 3: return ( f & 0x01 ) != 0;   /* C */
        case 4: return ( f & 0x04 ) == 0;   /* PO */
        case 5: return ( f & 0x04 ) != 0;   /* PE */
        case 6: return ( f & 0x80 ) == 0;   /* P */
        default: return ( f & 0x80 ) != 0;  /* M */
    };
}


en_COVERAGE_OUTCOME coverage_decode_outcome ( const st_COVERAGE_INSN *insn,
                                              uint16_t pc, uint16_t next_pc,
                                              uint8_t f, uint8_t b )
{
    bool taken;
    switch ( insn->cond ) {
        case COVERAGE_COND_FLAG:
            taken = coverage_decode_cc_true ( insn->cc, f );
            break;
        case COVERAGE_COND_DJNZ:
            taken = ( b != 0 );
            break;
        case COVERAGE_COND_REPEAT:
            /* Opakování vrací PC na prefix EDh, tedy za prefixy DD/FD kroku. */
            taken = ( next_pc == (uint16_t) ( pc + insn->prefixes ) );
            break;
        default:
            return COVERAGE_OUTCOME_NONE;
    };
    return taken ? COVERAGE_OUTCOME_TAKEN : COVERAGE_OUTCOME_NOT_TAKEN;
}
