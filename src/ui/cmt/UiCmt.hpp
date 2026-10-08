#ifndef UI_CMT_HPP
#define UI_CMT_HPP

#include <vector>
#include <string>

#include "libs/cmtspeed/cmtspeed.h"
#include "libs/mztape/mztape.h"
#include "cmt/cmtext_block.h"

typedef enum en_PLAY_STATE
{
    PLAY_STATE_NONE = 0,
    PLAY_STATE_STOPPED,
    PLAY_STATE_PLAYING,
    PLAY_STATE_PAUSED,
    PLAY_STATE_COUNT_ITEMS
} en_PLAY_STATE;

typedef enum en_CMT_SPEED_TYPE
{
    CMT_SPEED_TYPE_NONE = 0,
    CMT_SPEED_TYPE_MZ,
    CMT_SPEED_TYPE_ZX,
} en_CMT_SPEED_TYPE;

typedef struct TapeFileEntry_t
{
    int id;
    en_PLAY_STATE play_state;
    bool ready;
    bool played;
    bool paused;
    std::string name;
    std::string ftype_txt;
    std::string fsize_txt;
    std::string fstrt_txt;
    std::string fexec_txt;
    int block_type;
    en_CMTEXT_BLOCK_SPEED block_speed; // CMTEXT_BLOCK_SPEED_NONE, CMTEXT_BLOCK_SPEED_DEFAULT, CMTEXT_BLOCK_SPEED_SET
    en_CMTSPEED cmtspeed;              // CMTSPEED_NONE, CMTSPEED_1_1, ..., CMTSPEED_25_14, CMTSPEED_CUSTOM
    std::string cmtspeed_txt;          // "1:1 - 1200 Bd", ..., u vlastních pulzů "156/164/80/92 µs (~3.03:1)"
    en_CMT_SPEED_TYPE cmtspeed_type;   // CMT_SPEED_TYPE_NONE, CMT_SPEED_TYPE_MZ, CMT_SPEED_TYPE_ZX
    bool has_pulses;                   // pulses platí (MZ blok, cmt_tape_get_block_pulses uspěl)
    st_MZTAPE_PULSES_LENGTH pulses;    // efektivní délky pulzů MZ bloku (výchozí nebo vlastní rychlost)
    std::string length_txt;
} TapeFileEntry_t;

extern std::vector<TapeFileEntry_t> g_tapeFileList;

typedef struct UiCmtSpeedList_t
{
    en_CMTSPEED cmtspeed;
    std::string cmtspeed_txt;
} UiCmtSpeedList_t;

/**
 * @brief Předvolba vlastních délek pulzů (sada hlavičky CMTSPEED zařízení UniCMT).
 *
 * Délky v µs v pořadí hlavičky CMTSPEED: LONG high, LONG low, SHORT high,
 * SHORT low. Hodnoty souborů 1x/2x/3xspeed.mzf z USB disku UniCMT FW 0.5
 * (báze hw/25-unicmt.md, kap. 2.1).
 */
typedef struct UiCmtPulsesPreset_t
{
    const char *name; /**< zobrazovaný název (není lokalizovaný - jméno zařízení a sady) */
    double us[4];     /**< délky pulzů v µs */
} UiCmtPulsesPreset_t;

class UiCmt
{
public:
    /// Zakázání vytváření instancí.
    UiCmt() = delete;

    /// Zakázání kopírování a přiřazování.
    UiCmt(const UiCmt &) = delete;
    UiCmt &operator=(const UiCmt &) = delete;

    /**
     * @brief Text rychlosti bloku pro seznam bloků a výběr rychlosti.
     *
     * Pro CMTSPEED_CUSTOM vrátí jen obecný název ("Custom pulses"); text
     * s konkrétními délkami dává getPulsesTxt().
     */
    static std::string getSpeedTxt(en_CMTEXT_BLOCK_TYPE bltype, en_CMTEXT_BLOCK_SPEED blspeed, en_CMTSPEED cmtspeed, bool get_rateiospeed);

    /**
     * @brief Text vlastních délek pulzů, např. "156/164/80/92 µs (~3.03:1)".
     *
     * @param pulses Délky pulzů (nesmí být NULL).
     * @return Délky LONG high/low, SHORT high/low v µs (celá čísla bez
     *         desetin, jinak nejvýš 3 desetinná místa) a ekvivalentní poměr.
     */
    static std::string getPulsesTxt(const st_MZTAPE_PULSES_LENGTH *pulses);

    /**
     * @brief Předvolby vlastních délek pulzů (UniCMT 1x, 2x, 3x).
     *
     * @param[out] count Počet předvoleb.
     * @return Statické pole předvoleb.
     */
    static const UiCmtPulsesPreset_t *getPulsesPresets(int *count);

    // ziskani pole s rychlostmi pro MZ (posledni polozka je CMTSPEED_CUSTOM = "Custom pulses...")
    static std::vector<UiCmtSpeedList_t> getMzSpeedList(void);
    static std::vector<UiCmtSpeedList_t> getMzSpeedListDefault(void);

    // ziskani pole s rychlostmi pro ZX
    static std::vector<UiCmtSpeedList_t> getZxSpeedList(void);

    /**
     * @brief Odhad délky bloku v sekundách (9 bitů na bajt těla).
     *
     * @param bltype Typ bloku.
     * @param blspeed Režim rychlosti bloku.
     * @param cmtspeed Rychlost bloku (CMTSPEED_NONE = 1:1).
     * @param fsize Délka těla v bajtech.
     * @param pulses Délky pulzů pro CMTSPEED_CUSTOM (jinak se ignoruje; NULL = délku nelze určit).
     * @return Délka v sekundách, -1 pokud ji nelze určit.
     */
    static int getLengthInSec(en_CMTEXT_BLOCK_TYPE bltype, en_CMTEXT_BLOCK_SPEED blspeed, en_CMTSPEED cmtspeed, int fsize, const st_MZTAPE_PULSES_LENGTH *pulses = nullptr);

    // aktualizace seznamu souboru na pasce
    static void updateTapeFilelist(void);
};

#endif // UI_CMT_HPP
