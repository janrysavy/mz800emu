/**
 * @file snapshot_config.h
 * @brief Konfigurace snapshotů — INI integrace
 */

#ifndef SNAPSHOT_CONFIG_H
#define SNAPSHOT_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Zaregistruje sekci [SNAPSHOT], načte ji z INI a propaguje do g_snapshot_settings.
 *
 * Globální cfgroot_propagate se v emulátoru nevolá, proto modul sám volá
 * cfgmodule_parse + cfgmodule_propagate. Chybí-li sekce nebo soubor, platí
 * výchozí hodnoty elementů.
 *
 * @pre g_cfgmain existuje (volá cfgmain_init()).
 * @post g_snapshot_settings odpovídá INI (resp. výchozím hodnotám).
 * @note Volat jednou; vlákno: hlavní, před startem emulace.
 */
void snapshot_config_init(void);

#ifdef __cplusplus
}
#endif

#endif /* SNAPSHOT_CONFIG_H */
