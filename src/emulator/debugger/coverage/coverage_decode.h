/**
 * @file   coverage_decode.h
 * @brief  Dekódování kroku Z80 pro pokrytí kódu: délky, podmíněnost a výsledek podmínky.
 *
 * Pokrytí kódu potřebuje po každém kroku CPU vědět, kolik bajtů krok
 * zabral, které z nich tvoří opkód (klíč verze kódu) a zda je instrukce
 * podmíněná. U podmíněné instrukce pak ze stavu CPU po kroku odvodí, kterou
 * větví šla. Obojí počítá tento modul bez textového disassembleru a bez
 * zásahu do jádra Z80.
 *
 * Krok odpovídá jednomu volání z80_step() v jádře `libs/cpu-z80`:
 * - řetěz prefixů DDh/FDh a instrukce za ním jsou jeden krok (platí poslední
 *   prefix); řetěz má strop 64 prefixů, 65 prefixů za sebou jádro rozdělí
 *   a krok skončí za 64. prefixem (DDFD_CHAIN_MAX v z80_execute.inc),
 * - za prefixem DD/FD, který nepatří k instrukci s IX/IY, se provede
 *   následující bajt jako běžný opkód včetně EDh a CBh (prefix se ignoruje),
 * - DD/FD CB d op je vždy instrukce s (IX+d)/(IY+d).
 * Předpokládá variantu jádra s computed goto (GCC, clang); bez ní jádro
 * krok za ignorovaným prefixem rozdělí jinak.
 *
 * Výsledek podmínky (coverage_decode_outcome()):
 * - JR cc, JP cc, CALL cc, RET cc nemění F, takže rozhoduje F po kroku,
 * - DJNZ skočí, když je B po kroku nenulové,
 * - LDIR, LDDR, CPIR, CPDR, INIR, INDR, OTIR, OTDR se opakují, když jádro
 *   vrátí PC na prefix EDh (u kroku s prefixy DD/FD je to adresa za nimi).
 * Porovnání "PC po kroku == adresa za instrukcí" nestačí: `JR Z,$+2` se
 * splněnou podmínkou skončí na stejném PC jako nesplněná.
 *
 * @par Licence: GPLv3
 */

#ifndef COVERAGE_DECODE_H
#define COVERAGE_DECODE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Nejvyšší počet prefixů DD/FD v jednom kroku (DDFD_CHAIN_MAX jádra). */
#define COVERAGE_DECODE_PREFIX_MAX 64

/** @brief Nejvyšší délka kroku v bajtech (64 prefixů + instrukce o 4 bajtech). */
#define COVERAGE_DECODE_LEN_MAX ( COVERAGE_DECODE_PREFIX_MAX + 4 )


/** @brief Druh podmínky instrukce. */
typedef enum en_COVERAGE_COND {
    COVERAGE_COND_NONE = 0, /**< Nepodmíněná instrukce (i nepřímé skoky, RST, HALT). */
    COVERAGE_COND_FLAG,     /**< JR cc, JP cc, CALL cc, RET cc: podmínka `cc` nad F po kroku. */
    COVERAGE_COND_DJNZ,     /**< DJNZ: skok, když je B po kroku nenulové. */
    COVERAGE_COND_REPEAT,   /**< Opakující bloková instrukce: "skok" = další iterace. */
} en_COVERAGE_COND;


/** @brief Výsledek podmíněné instrukce v jednom kroku. */
typedef enum en_COVERAGE_OUTCOME {
    COVERAGE_OUTCOME_NONE = 0,  /**< Instrukce není podmíněná. */
    COVERAGE_OUTCOME_TAKEN,     /**< Podmínka splněna (skok, volání, návrat, další iterace). */
    COVERAGE_OUTCOME_NOT_TAKEN, /**< Podmínka nesplněna (pokračuje se za instrukcí). */
} en_COVERAGE_OUTCOME;


/**
 * @brief Výsledek dekódování jednoho kroku.
 *
 * @invariant 1 <= `len` <= COVERAGE_DECODE_LEN_MAX.
 * @invariant `prefixes` <= `op_len` <= `len`.
 * @invariant `prefix_split` => `prefixes` == `len` == COVERAGE_DECODE_PREFIX_MAX,
 *            `cond` == COVERAGE_COND_NONE (krok bez instrukce).
 * @invariant `op_last` jen u DD/FD CB d op: opkódem je i poslední bajt
 *            kroku, bajt d mezi nimi je operand.
 * @invariant `cc` má význam jen u COVERAGE_COND_FLAG.
 */
typedef struct st_COVERAGE_INSN {
    uint8_t len;          /**< Délka kroku v bajtech (prefixy + instrukce). */
    uint8_t prefixes;     /**< Počet prefixů DD/FD na začátku kroku (0..64). */
    uint8_t op_len;       /**< Počet úvodních bajtů opkódu: prefixy, případně CBh/EDh a opkód. */
    bool op_last;         /**< Poslední bajt kroku je také opkód (DD/FD CB d op). */
    bool prefix_split;    /**< 65 prefixů za sebou: krok obsahuje jen 64 prefixů. */
    bool halt;            /**< Instrukce HALT (76h, i za ignorovaným prefixem). */
    uint8_t cond;         /**< Druh podmínky (en_COVERAGE_COND). */
    uint8_t cc;           /**< Kód podmínky 0-7: NZ, Z, NC, C, PO, PE, P, M. */
} st_COVERAGE_INSN;


/**
 * @brief Čtení bajtu kroku pro dekodér.
 *
 * @param ctx   Kontext volajícího (nevlastněný, dekodér ho jen předává).
 * @param index Pozice bajtu od začátku kroku (0 = první bajt).
 * @return Bajt na pozici @p index.
 */
typedef uint8_t ( *coverage_decode_read_fn ) ( void *ctx, unsigned index );


/**
 * @brief Dekóduje krok začínající bajtem 0.
 *
 * Čte jen bajty prefixů a opkódu (u kroku bez prefixu typicky jeden nebo
 * dva), operandy nečte: délka plyne z opkódu.
 *
 * @param read Čtení bajtu kroku; volá se pro indexy 0 .. `len`-1 (nejvýš
 *             COVERAGE_DECODE_PREFIX_MAX + 2), každý index nejvýš jednou.
 * @param ctx  Kontext pro @p read.
 * @param out  Výsledek.
 *
 * @pre @p read a @p out nejsou NULL.
 * @post @p out splňuje invarianty st_COVERAGE_INSN.
 * @note Bez vedlejších efektů mimo @p out a volání @p read; volatelné
 *       z libovolného vlákna.
 */
void coverage_decode ( coverage_decode_read_fn read, void *ctx, st_COVERAGE_INSN *out );


/**
 * @brief Dekóduje krok z pole bajtů.
 *
 * Obálka nad coverage_decode(): bajty za koncem pole čte jako 00h (NOP).
 *
 * @param bytes Bajty kroku od začátku.
 * @param n     Počet platných bajtů v @p bytes.
 * @param out   Výsledek.
 *
 * @pre @p bytes (při @p n > 0) a @p out nejsou NULL.
 */
void coverage_decode_bytes ( const uint8_t *bytes, unsigned n, st_COVERAGE_INSN *out );


/**
 * @brief Vyhodnotí podmínku `cc` nad registrem F.
 *
 * @param cc Kód podmínky 0-7 (NZ, Z, NC, C, PO, PE, P, M); vyšší bity se
 *           ignorují.
 * @param f  Registr F.
 * @return true, když je podmínka splněna.
 */
bool coverage_decode_cc_true ( uint8_t cc, uint8_t f );


/**
 * @brief Odvodí výsledek podmíněné instrukce ze stavu CPU po kroku.
 *
 * @param insn    Dekódovaný krok (coverage_decode()).
 * @param pc      Adresa začátku kroku.
 * @param next_pc PC po kroku (před případným přijetím přerušení).
 * @param f       Registr F po kroku.
 * @param b       Registr B po kroku.
 * @return COVERAGE_OUTCOME_NONE u nepodmíněné instrukce, jinak TAKEN nebo
 *         NOT_TAKEN.
 *
 * @pre Krok vykonalo jádro `libs/cpu-z80` a mezi krokem a voláním se F, B
 *      ani PC nezměnily (žádné přijetí přerušení, zásah debuggeru).
 */
en_COVERAGE_OUTCOME coverage_decode_outcome ( const st_COVERAGE_INSN *insn,
                                              uint16_t pc, uint16_t next_pc,
                                              uint8_t f, uint8_t b );

#ifdef __cplusplus
}
#endif

#endif /* COVERAGE_DECODE_H */
