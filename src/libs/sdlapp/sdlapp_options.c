/*
 * File:   sdlapp_options.c
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
 * @file sdlapp_options.c
 * @brief Implementace - viz sdlapp_options.h.
 */

#include <stdio.h>
#include <string.h>

#include "sdlapp_options.h"


/* === Stav modulu === */

static int s_argc = 0;
static char **s_argv = NULL;


/* === Helper === */

/**
 * @brief Porovnat argv token s názvem option.
 *
 * Podporujeme dvě formy zápisu hodnoty:
 *  - space-separated: `--name value` (tok je přesně rovno `name`)
 *  - equals-separated: `--name=value` (tok začíná `name=`, hodnota je
 *    bezprostředně za rovnítkem)
 *
 * @param tok argv token (např. `--fdc-impl`, `--fdc-impl=new`).
 * @param name kanonický název option (např. `--fdc-impl`).
 * @return 0 = přesná shoda (space form), 1 = shoda `name=` (equals form),
 *         -1 = neshoda.
 */
static int sdlapp_options_match_token ( const char *tok, const char *name )
{
    if ( !tok || !name ) return -1;
    size_t n = strlen ( name );
    if ( strncmp ( tok, name, n ) != 0 ) return -1;
    if ( tok[n] == '\0' ) return 0;
    if ( tok[n] == '=' ) return 1;
    return -1;
}


/**
 * @brief Najít index argv tokenu odpovídajícího @p name (space i equals forma).
 *
 * @return Index v argv (>= 1) nebo -1 pokud nenalezen.
 */
static int sdlapp_options_find_index ( const char *name )
{
    if ( !name || !s_argv ) return -1;
    int i;
    for ( i = 1; i < s_argc; i++ )
    {
        if ( s_argv[i] && sdlapp_options_match_token ( s_argv[i], name ) >= 0 ) return i;
    }
    return -1;
}


/* === Public API === */

void sdlapp_options_init ( int argc, char **argv )
{
    s_argc = argc;
    s_argv = argv;
}


bool sdlapp_option_present ( const char *name )
{
    return sdlapp_options_find_index ( name ) > 0;
}


/**
 * @brief Hodnota option z argv tokenu na indexu @p idx (obě formy zápisu).
 *
 * @return Hodnota, nebo NULL, když u space formy chybí další token.
 */
static const char *sdlapp_options_value_at ( int idx, const char *name )
{
    const char *tok = s_argv[ idx ];
    size_t n = strlen ( name );
    /* Equals form: `--name=value` - hodnota začíná za rovnítkem. */
    if ( tok[n] == '=' ) return tok + n + 1;
    /* Space form: `--name value` - hodnota je v dalším argv tokenu. */
    if ( idx + 1 >= s_argc ) return NULL;
    return s_argv[ idx + 1 ];
}


int sdlapp_option_count ( const char *name )
{
    if ( !name || !s_argv ) return 0;
    int count = 0;
    int i;
    for ( i = 1; i < s_argc; i++ )
    {
        if ( s_argv[i] && sdlapp_options_match_token ( s_argv[i], name ) >= 0 ) count++;
    }
    return count;
}


const char *sdlapp_option_value_nth ( const char *name, int n )
{
    if ( !name || !s_argv || n < 0 ) return NULL;
    int i;
    for ( i = 1; i < s_argc; i++ )
    {
        if ( s_argv[i] && sdlapp_options_match_token ( s_argv[i], name ) >= 0 )
        {
            if ( n == 0 ) return sdlapp_options_value_at ( i, name );
            n--;
        }
    }
    return NULL;
}


const char *sdlapp_option_value ( const char *name )
{
    int idx = sdlapp_options_find_index ( name );
    if ( idx < 0 ) return NULL;
    return sdlapp_options_value_at ( idx, name );
}


/**
 * @brief Validovat jednu hodnotu VALUE option vůči deklarovanému typu.
 *
 * Vytiskne chybu na stderr a vrátí false při neúspěchu. Pro
 * @c SDLAPP_OPTVAL_NONE / @c SDLAPP_OPTVAL_STRING vždy true (string je
 * neprázdný garantován callerem - i+1 < s_argc).
 *
 * @param def Definice option (pro name + value_type + allowed_values).
 * @param val Hodnota z argv (nesmí být NULL).
 * @return true při úspěchu, jinak false.
 */
static bool sdlapp_options_validate_value ( const st_SDLAPP_OPTION_DEF *def,
                                            const char *val )
{
    switch ( def->value_type )
    {
        case SDLAPP_OPTVAL_NONE:
        case SDLAPP_OPTVAL_STRING:
            return true;

        case SDLAPP_OPTVAL_ENUM:
        {
            if ( !def->allowed_values ) return true; /* schema nedeklaruje - tichý průchod */
            const char *const *p;
            for ( p = def->allowed_values; *p != NULL; p++ )
            {
                if ( strcmp ( *p, val ) == 0 ) return true;
            }
            fprintf ( stderr, "Invalid value for %s: '%s'. Allowed: ", def->name, val );
            for ( p = def->allowed_values; *p != NULL; p++ )
            {
                fprintf ( stderr, "%s%s", ( p == def->allowed_values ) ? "" : "|", *p );
            }
            fprintf ( stderr, "\n" );
            return false;
        }

        case SDLAPP_OPTVAL_BOOL_ON_OFF:
            if ( strcmp ( val, "on" ) == 0 || strcmp ( val, "off" ) == 0 ) return true;
            fprintf ( stderr, "Invalid value for %s: '%s'. Allowed: on|off\n", def->name, val );
            return false;

        case SDLAPP_OPTVAL_UINT:
        {
            const char *p = val;
            if ( !*p )
            {
                fprintf ( stderr, "Invalid value for %s: empty (expected non-negative integer)\n", def->name );
                return false;
            }
            for ( ; *p; p++ )
            {
                if ( *p < '0' || *p > '9' )
                {
                    fprintf ( stderr, "Invalid value for %s: '%s' (expected non-negative integer)\n",
                              def->name, val );
                    return false;
                }
            }
            return true;
        }
    }
    /* Neznámý value_type v deklaraci = bug v tabulce, hlas a pokračuj. */
    fprintf ( stderr, "Internal: unknown value_type %d for %s\n",
              (int) def->value_type, def->name );
    return false;
}


bool sdlapp_options_validate ( const st_SDLAPP_OPTION_DEF *known )
{
    if ( !s_argv ) return true; /* sdlapp_options_init nebyl zavolán - nic k validaci */

    int i;
    for ( i = 1; i < s_argc; i++ )
    {
        const char *tok = s_argv[i];
        if ( !tok ) continue;

        /* Token který nezačíná '-' nebo je to samotné "-" není option,
         * skipneme (typicky to nikdy nenastane, protože hodnoty za VALUE
         * option jsou explicitně skipnuty `i++` níže). */
        if ( tok[0] != '-' || tok[1] == '\0' ) continue;

        /* Cokoli začínající '-' (long "--xxx" i short "-x") musí být ve
         * whitelistu. Aktuálně podporujeme jen long options, takže short
         * options budou vždy unknown. Token může být buď přesné jméno
         * (space form: `--name value`) nebo `name=value` (equals form). */
        const st_SDLAPP_OPTION_DEF *def = NULL;
        int match_kind = -1; /* 0 = exact (space form), 1 = name= (equals form) */
        const st_SDLAPP_OPTION_DEF *p;
        for ( p = known; p && p->name != NULL; p++ )
        {
            int m = sdlapp_options_match_token ( tok, p->name );
            if ( m >= 0 )
            {
                def = p;
                match_kind = m;
                break;
            }
        }
        if ( !def )
        {
            fprintf ( stderr, "Unknown option: %s\n", tok );
            fprintf ( stderr, "Use --help for list of valid options.\n" );
            return false;
        }

        /* Pokud VALUE option, vytáhni hodnotu podle formy, validuj typ. */
        if ( def->kind == SDLAPP_OPTION_VALUE )
        {
            const char *val;
            if ( match_kind == 1 )
            {
                /* Equals form: hodnota je za rovnítkem ve stejném tokenu. */
                val = tok + strlen ( def->name ) + 1;
                if ( !*val )
                {
                    fprintf ( stderr, "Option %s requires a value.\n", def->name );
                    return false;
                }
            }
            else
            {
                /* Space form: hodnota je v dalším argv tokenu (může začínat
                 * '-' jako součást cesty - skipneme i++). */
                if ( i + 1 >= s_argc )
                {
                    fprintf ( stderr, "Option %s requires a value.\n", def->name );
                    return false;
                }
                val = s_argv[ i + 1 ];
                i++;
            }
            if ( !sdlapp_options_validate_value ( def, val ) )
            {
                return false;
            }
        }
    }

    return true;
}


void sdlapp_options_print_help ( const char *prog_name, const st_SDLAPP_OPTION_DEF *known )
{
    printf ( "Usage: %s [OPTIONS]\n\n", prog_name ? prog_name : "mz800emu" );
    printf ( "Options:\n" );

    if ( !known ) return;

    /* Spočítat max šířku jména + placeholder pro zarovnání popisu. */
    size_t max_w = 0;
    const st_SDLAPP_OPTION_DEF *p;
    for ( p = known; p->name != NULL; p++ )
    {
        size_t w = strlen ( p->name );
        if ( p->kind == SDLAPP_OPTION_VALUE && p->value_placeholder )
        {
            w += 1 + strlen ( p->value_placeholder );
        }
        if ( w > max_w ) max_w = w;
    }

    for ( p = known; p->name != NULL; p++ )
    {
        char buf[ 128 ];
        if ( p->kind == SDLAPP_OPTION_VALUE && p->value_placeholder )
        {
            snprintf ( buf, sizeof ( buf ), "%s %s", p->name, p->value_placeholder );
        }
        else
        {
            snprintf ( buf, sizeof ( buf ), "%s", p->name );
        }
        printf ( "  %-*s  %s\n", (int)max_w, buf,
                 p->description ? p->description : "" );
    }
}
