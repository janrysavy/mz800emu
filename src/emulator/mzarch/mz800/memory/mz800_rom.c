#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Lokalizace
#include "i18n.h"

#include "hw-generic/memory/rom_config.h"

extern uint8_t c_ROM_MZ800_0000 [];
extern uint8_t c_ROM_MZ800_CGROM [];
extern uint8_t c_ROM_MZ800_E000 [];

extern uint8_t c_ROM_JSS105C_0000 [];
extern uint8_t c_ROM_JSS105C_CGROM [];
extern uint8_t c_ROM_JSS105C_E000 [];

extern uint8_t c_ROM_JSS103_0000 [];
extern uint8_t c_ROM_JSS103_CGROM [];
extern uint8_t c_ROM_JSS103_E000 [];

extern uint8_t c_ROM_JSS108C_0000 [];
extern uint8_t c_ROM_JSS108C_CGROM [];
extern uint8_t c_ROM_JSS108C_E000 [];

extern uint8_t c_ROM_JSS106A_0000 [];
extern uint8_t c_ROM_JSS106A_CGROM [];
extern uint8_t c_ROM_JSS106A_E000 [];

extern uint8_t c_ROM_WILLY_0000 [];

extern uint8_t c_ROM_WILLY_en_CGROM [];
extern uint8_t c_ROM_WILLY_en_E000 [];

extern uint8_t c_ROM_WILLY_ge_CGROM [];
extern uint8_t c_ROM_WILLY_ge_E000 [];

extern uint8_t c_ROM_WILLY_jap_CGROM [];
extern uint8_t c_ROM_WILLY_jap_E000 [];

static st_ROM_CONFIG_ROW c_roms[] = {
    {ROM_CONFIG_TYPE_ROM, true, "STANDARD", N_("Standard Sharp MZ-800 ROM"), ROM_CONFIG_FLAG_NONE, c_ROM_MZ800_0000, c_ROM_MZ800_CGROM, c_ROM_MZ800_E000},
    {ROM_CONFIG_TYPE_SECTION, true, NULL, N_("JSS ROMs"), ROM_CONFIG_FLAG_NONE, NULL, NULL, NULL},
    {ROM_CONFIG_TYPE_ROM, true, "JSS105C", N_("JSS v1.05C - for MZ-1R18 (devel)"), ROM_CONFIG_FLAG_DEVELOPMENT, c_ROM_JSS105C_0000, c_ROM_JSS105C_CGROM, c_ROM_JSS105C_E000},
    {ROM_CONFIG_TYPE_ROM, true, "JSS103", N_("JSS v1.03 - for Pezik (devel)"), ROM_CONFIG_FLAG_DEVELOPMENT, c_ROM_JSS103_0000, c_ROM_JSS103_CGROM, c_ROM_JSS103_E000},
    {ROM_CONFIG_TYPE_SECTION, true, NULL, NULL, ROM_CONFIG_FLAG_NONE, NULL, NULL, NULL},
    {ROM_CONFIG_TYPE_ROM, true, "JSS108C", N_("JSS v1.08C - for MZ-1R18"), ROM_CONFIG_FLAG_NONE, c_ROM_JSS108C_0000, c_ROM_JSS108C_CGROM, c_ROM_JSS108C_E000},
    {ROM_CONFIG_TYPE_ROM, true, "JSS106A", N_("JSS v1.06A - for Pezik"), ROM_CONFIG_FLAG_NONE, c_ROM_JSS106A_0000, c_ROM_JSS106A_CGROM, c_ROM_JSS106A_E000},
    {ROM_CONFIG_TYPE_SECTION, true, NULL, N_("Willy ROMs"), ROM_CONFIG_FLAG_NONE, NULL, NULL, NULL},
    {ROM_CONFIG_TYPE_ROM, true, "WILLY_EN", N_("Willy - English"), ROM_CONFIG_FLAG_CMTHACK_INCOMP, c_ROM_WILLY_0000, c_ROM_WILLY_en_CGROM, c_ROM_WILLY_en_E000},
    {ROM_CONFIG_TYPE_ROM, true, "WILLY_GE", N_("Willy - German"), ROM_CONFIG_FLAG_CMTHACK_INCOMP, c_ROM_WILLY_0000, c_ROM_WILLY_ge_CGROM, c_ROM_WILLY_ge_E000},
    {ROM_CONFIG_TYPE_ROM, true, "WILLY_JAP", N_("Willy - Japanese"), ROM_CONFIG_FLAG_CMTHACK_INCOMP, c_ROM_WILLY_0000, c_ROM_WILLY_jap_CGROM, c_ROM_WILLY_jap_E000},
    {ROM_CONFIG_TYPE_SECTION, true, NULL, N_("User"), ROM_CONFIG_FLAG_NONE, NULL, NULL, NULL},
    {ROM_CONFIG_TYPE_ROM, true, "USER_DEFINED", N_("User Defined ROM"), ROM_CONFIG_FLAG_USER_DEFINED, NULL, NULL, NULL},
    {ROM_CONFIG_TYPE_NONE, false, NULL, NULL, ROM_CONFIG_FLAG_NONE, NULL, NULL, NULL}
};

static st_ROM_CONFIG c_rom_config = {
    .roms = c_roms,
    .default_rom = &c_roms[0]
};

st_ROM_CONFIG *g_rom_config = &c_rom_config;
