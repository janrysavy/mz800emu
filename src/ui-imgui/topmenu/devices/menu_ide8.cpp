#include "main.h"
#include <stdbool.h>
#include <stdint.h>
#include <glib.h>

#include "libs/imgui/imgui.h"

// Lokalizace
#include "i18n.h"

#include "ui-imgui/topmenu/topmenu.h"
#include "ui-imgui/bootstrap/myimgui.h"

#include "emulator/hw-generic/ide8/ide8.h"
#include "baseui/baseui_filechooser.h"

extern "C"
{
    void imgui_menu_ide8(void);
}

void imgui_ide8_gstring_add_filepath(GString *str, en_IDE8_DRIVE drive_id)
{
    const char *basename = g_path_get_basename(ide8_drive_get_filepath(drive_id));

    if (basename)
    {
        // pokud ma vice jak 20 znaku, tak zkratime a na konec pridame "..." - aby se veslo do menu
        if (strlen(basename) > 20)
        {
            g_string_append_printf(str, "%s...", g_utf8_substring(basename, 0, 20));
        }
        else
        {
            g_string_append(str, basename);
        };
        g_free((gpointer)basename);
    }
    else
    {
        g_string_append(str, _("No image"));
    };
}

void imgui_ide8_open_file_cb(baseui_fchooser_t *fch)
{
    if (fch->state != BASEUI_FCHOOSER_STATE_CLOSED_OK)
    {
        baseui_filechooser_destroy(fch);
        return;
    };

    int drive_id = GPOINTER_TO_INT(fch->user_data);

    if(drive_id < 0 || drive_id >= IDE8_DRIVES_COUNT)
    {
        fprintf(stderr, "%s(%d): invalid drive_id\n", __FILE__, __LINE__);
        baseui_filechooser_destroy(fch);
        return;
    };

    if(ide8_drive_get_connected(drive_id))
    {
        ide8_drive_close_image(&g_ide8.drive[drive_id]);
    };

    if (EXIT_SUCCESS == ide8_drive_open_image(&g_ide8.drive[drive_id], fch->selected_filePathName))
    {
        g_ide8.drive[drive_id].filepath = fch->selected_filePathName;
        fch->selected_filePathName = NULL;
    };

    baseui_filechooser_destroy(fch);
}

void imgui_ide8_open_file(en_IDE8_DRIVE drive_id)
{
    // pokud je drive pripojen, tak se nejprve odpoji
    if (ide8_drive_get_connected(drive_id))
    {
        ide8_drive_close_image(&g_ide8.drive[drive_id]);
    };

    const char *title = (drive_id == IDE8_DRIVE_MASTER) ? _("Select IDE8 HDD0 image file to open") : _("Select IDE8 HDD1 image file to open");

    baseui_fchooser_t *fch = baseui_filechooser_open_rw_file(BASEUI_FCHOOSER_CAT_HDD, title, ".img, .dat, .*", NULL, NULL, ide8_drive_get_filepath(drive_id), imgui_ide8_open_file_cb, (gpointer)drive_id);
    if (!fch)
    {
        fprintf(stderr, "%s(%d): filechooser error\n", __FILE__, __LINE__);
    };
}

void imgui_menu_ide8(void)
{
    if (ImGui::BeginMenu(_L("IDE8")))
    {
        GString *str0 = g_string_new(_("IDE8 Master: "));
        if (IDE8_TEST_MASTER_CONNECTED)
        {
            imgui_ide8_gstring_add_filepath(str0, IDE8_DRIVE_MASTER);
        }
        else
        {
            g_string_append(str0, _("Disconnected"));
        };

        if (ImGui::MenuItem(str0->str, NULL, IDE8_TEST_MASTER_CONNECTED))
        {
            ide8_drive_set_connected(IDE8_DRIVE_MASTER, (!IDE8_TEST_MASTER_CONNECTED) ? IDE8_STATE_CONNECTED : IDE8_STATE_DISCONNECTED);
        };
        g_string_free(str0, TRUE);

        if (ImGui::MenuItem(_L("Set HDD0 Image..."), NULL, false, !IDE8_TEST_MASTER_CONNECTED))
        {
            imgui_ide8_open_file(IDE8_DRIVE_MASTER);
        };

        ImGui::Separator();

        GString *str1 = g_string_new(_("IDE8 Slave: "));
        if (IDE8_TEST_SLAVE_CONNECTED)
        {
            imgui_ide8_gstring_add_filepath(str1, IDE8_DRIVE_SLAVE);
        }
        else
        {
            g_string_append(str1, _("Disconnected"));
        };

        if (ImGui::MenuItem(str1->str, NULL, IDE8_TEST_SLAVE_CONNECTED))
        {
            ide8_drive_set_connected(IDE8_DRIVE_SLAVE, (!IDE8_TEST_SLAVE_CONNECTED) ? IDE8_STATE_CONNECTED : IDE8_STATE_DISCONNECTED);
        };
        g_string_free(str1, TRUE);

        if (ImGui::MenuItem(_L("Set HDD1 Image..."), NULL, false, !IDE8_TEST_SLAVE_CONNECTED))
        {
            imgui_ide8_open_file(IDE8_DRIVE_SLAVE);
        };

#ifdef MZ800EMU_CFG_DEBUGGER_ENABLED
        /* memory-disk-state: debugger okno se stavem IDE8 radice. */
        ImGui::Separator();
        if (ImGui::MenuItem(_L("IDE8 State (debugger)..."), NULL,
                            g_gui->showIde8StateWindow, true))
        {
            g_gui->showIde8StateWindow = !g_gui->showIde8StateWindow;
        };
#endif

        ImGui::EndMenu();
    };
}
