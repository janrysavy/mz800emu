/*
 * File:   sdlapp_options.h
 * Author: Michal Hucik <hucik@ordoz.com>
 *
 *
 * ----------------------------- License -------------------------------------
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * ---------------------------------------------------------------------------
 */

/**
 * @file sdlapp_options.h
 * @brief Distribuovaný parser CLI options pro emulátor.
 *
 * Storage argc/argv ze main(), aby si jednotlivé moduly mohly své options
 * vytáhnout samy (bez centralizované registrace). main() validuje vůči
 * whitelistu a hlásí neznámé options jako chybu.
 *
 * Konvence:
 *  - Long options s prefixem "--" (např. "--cdl-mode")
 *  - Hodnota za mezerou: "--cdl-mode always" (ne --cdl-mode=always)
 *  - Boolean (přítomnost = true): "--no-save-ini"
 *
 * Použití:
 *   main():
 *     sdlapp_options_init(argc, argv);
 *     if (!sdlapp_options_validate(known_options)) exit(1);
 *
 *   modul:
 *     const char *mode = sdlapp_option_value("--cdl-mode");
 *     if (mode) { ... }
 *     bool no_save = sdlapp_option_present("--no-save-ini");
 */

#ifndef SDLAPP_OPTIONS_H
#define SDLAPP_OPTIONS_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>


    /**
     * @brief Druh option dle přítomnosti hodnoty.
     */
    typedef enum en_SDLAPP_OPTION_KIND
    {
        SDLAPP_OPTION_FLAG = 0,    /**< boolean - přítomnost znamená true (např. "--no-save-ini") */
        SDLAPP_OPTION_VALUE,       /**< vyžaduje hodnotu za mezerou (např. "--cdl-mode always") */
    } en_SDLAPP_OPTION_KIND;


    /**
     * @brief Typ hodnoty VALUE option pro vstupní validaci.
     *
     * Slouží k centrální kontrole hodnot v @ref sdlapp_options_validate(),
     * aby se "tichá" akceptace nepovolených hodnot (např. "yes" místo "on")
     * neprosvištěla až do per-modul kódu.
     */
    typedef enum en_SDLAPP_OPTION_VALUE_TYPE
    {
        SDLAPP_OPTVAL_NONE = 0,        /**< Pro FLAG options - žádná hodnota není. */
        SDLAPP_OPTVAL_STRING,          /**< Libovolný neprázdný string (cesty, názvy). */
        SDLAPP_OPTVAL_ENUM,            /**< Hodnota musí být v @c allowed_values seznamu. */
        SDLAPP_OPTVAL_UINT,            /**< Nezáporné celé číslo (jen číslice 0-9). */
        SDLAPP_OPTVAL_BOOL_ON_OFF,     /**< Přesně "on" nebo "off" (case sensitive). */
    } en_SDLAPP_OPTION_VALUE_TYPE;


    /**
     * @brief Záznam ve whitelistu validních options.
     *
     * Pro @ref SDLAPP_OPTION_FLAG použijte @c value_type=SDLAPP_OPTVAL_NONE
     * a @c allowed_values=NULL. Pro @ref SDLAPP_OPTION_VALUE povinně
     * vyplňte @c value_type podle očekávaného typu hodnoty - to umožní
     * @ref sdlapp_options_validate() odhalit překlepy ("yes" místo "on")
     * a vypsat srozumitelnou chybu už před spuštěním zbytku aplikace.
     *
     * Pro @c SDLAPP_OPTVAL_ENUM musí být @c allowed_values NULL-terminated
     * pole stringů, např. @c {"off","window","always",NULL}. Hodnoty se
     * porovnávají case-sensitive přes @c strcmp.
     */
    typedef struct st_SDLAPP_OPTION_DEF
    {
        const char *name;                          /**< plný long-option včetně "--" prefixu */
        en_SDLAPP_OPTION_KIND kind;                /**< FLAG nebo VALUE */
        en_SDLAPP_OPTION_VALUE_TYPE value_type;    /**< Pro VALUE povinné, pro FLAG použít NONE */
        const char *const *allowed_values;         /**< Pro ENUM NULL-terminated pole, jinak NULL */
        const char *value_placeholder;             /**< pro --help, např. "<filepath>" nebo "<off|window|always>". NULL pro FLAG */
        const char *description;                   /**< pro --help, jednořádkový popis */
    } st_SDLAPP_OPTION_DEF;


    /**
     * @brief Uložit argc/argv pro pozdější dotazy modulů.
     *
     * Volat na začátku main() před jakýmkoliv option dotazem.
     *
     * @param argc Počet argumentů.
     * @param argv Pole argumentů (uložen pointer, neudělá kopii - argv musí
     *             žít po celý běh aplikace, což je u main() argv vždycky tak).
     */
    extern void sdlapp_options_init ( int argc, char **argv );


    /**
     * @brief Otestovat, zda je option v argv (jen pro FLAG typ).
     *
     * @param name Plný long option včetně "--" (např. "--no-save-ini").
     * @return true pokud je v argv, jinak false.
     */
    extern bool sdlapp_option_present ( const char *name );


    /**
     * @brief Vrátí hodnotu option (jen pro VALUE typ).
     *
     * @param name Plný long option včetně "--" (např. "--cdl-mode").
     * @return Pointer na argument za option v argv, nebo NULL pokud option není
     *         přítomen, nebo pokud následující argument chybí. Lifetime je
     *         shodný s argv (= živé po celý běh aplikace).
     */
    extern const char *sdlapp_option_value ( const char *name );


    /**
     * @brief Kolikrát je option v argv (opakovatelné VALUE volby).
     *
     * Počítá obě formy zápisu (`--name value` i `--name=value`).
     *
     * @param name Plný long option včetně "--".
     * @return Počet výskytů (0 = option chybí).
     */
    extern int sdlapp_option_count ( const char *name );


    /**
     * @brief Hodnota n-tého výskytu option (jen pro VALUE typ).
     *
     * @param name Plný long option včetně "--".
     * @param n Pořadí výskytu od 0 (v pořadí argv).
     * @return Hodnota stejně jako sdlapp_option_value(), nebo NULL, když
     *         výskyt @p n neexistuje nebo za ním hodnota chybí.
     */
    extern const char *sdlapp_option_value_nth ( const char *name, int n );


    /**
     * @brief Validovat argv vůči whitelistu.
     *
     * Projde všechny tokeny v argv (kromě argv[0]) začínající "--". Pokud
     * některý není ve @p known seznamu, vypíše error na stderr a vrátí false.
     * Také zkontroluje, že VALUE options mají následující argument k dispozici.
     *
     * @param known     Pole popisů známých options. Konec značí záznam s
     *                  @c name == NULL.
     * @return true pokud jsou všechny options v argv platné, jinak false.
     */
    extern bool sdlapp_options_validate ( const st_SDLAPP_OPTION_DEF *known );


    /**
     * @brief Vypsat usage / help text na stdout.
     *
     * Zobrazí seznam všech známých options s popisem (z @p known seznamu).
     * Volá se z main() při detekci "--help".
     *
     * @param prog_name Jméno binárky (typicky argv[0]).
     * @param known     Pole popisů známých options (terminate NULL).
     */
    extern void sdlapp_options_print_help ( const char *prog_name,
                                            const st_SDLAPP_OPTION_DEF *known );


#ifdef __cplusplus
}
#endif

#endif /* SDLAPP_OPTIONS_H */
