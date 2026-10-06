/**
 * @file memext_save_dialog.cpp
 * @brief Dialog pro uložení obsahu MemExt RAM nebo FLASH do souboru
 */

#include "main.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "libs/imgui/imgui.h"
#include "libs/igfd/ImGuiFileDialog.h"
#include "ui-imgui/filechooser/imgui_filechooser.h"
#include "ui-imgui/bootstrap/myimgui.h"
#include "ui-imgui/auto_layout.h"
#include "memext_content.h"

// Lokalizace
#include "i18n.h"

extern "C"
{
#include "emulator/hw-generic/memory/memext.h"
}

/* MZF hlavička (128 bytů) */
#pragma pack(push, 1)
struct MzfHeader {
    uint8_t type;
    char filename[16];
    uint16_t size;
    uint16_t start_addr;
    uint16_t exec_addr;
    char comment[104];
};
#pragma pack(pop)

/* Stav save dialogu */
static bool s_save_open = false;
static en_MemextContentType s_save_type = MEMEXT_CONTENT_RAM;
static uint32_t s_save_mem_from = 0;
static uint32_t s_save_mem_size = MEMEXT_RAM_SIZE;
static bool s_save_add_mzf = false;
static uint8_t s_save_mzf_type = 0x01;
static uint16_t s_save_mzf_start = 0x0000;
static uint16_t s_save_mzf_exec = 0x0000;
static char s_save_mzf_filename[17] = "FILENAME";
static char s_save_mzf_comment[105] = "";
static bool s_save_fch_open = false;
static char s_save_error[256] = "";

/* Helper: Hex/Dec duální vstup */
static bool HexDecInput(const char *label, uint32_t *value, uint32_t max_value, const char *id_suffix) {
    bool changed = false;

    if (label) {
        ImGui::Text("%s", label);
        ImGui::SameLine(80.0f);
    }

    char hex_buf[16];
    snprintf(hex_buf, sizeof(hex_buf), "%05X", *value);
    char id_hex[64];
    snprintf(id_hex, sizeof(id_hex), "##hex_%s", id_suffix);
    ImGui::Text("0x");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    if (ImGui::InputText(id_hex, hex_buf, sizeof(hex_buf), ImGuiInputTextFlags_CharsHexadecimal)) {
        uint32_t v = (uint32_t)strtoul(hex_buf, NULL, 16);
        if (v > max_value) v = max_value;
        *value = v;
        changed = true;
    }

    ImGui::SameLine();

    int dec_val = (int)*value;
    char id_dec[64];
    snprintf(id_dec, sizeof(id_dec), "##dec_%s", id_suffix);
    ImGui::SetNextItemWidth(80);
    if (ImGui::InputInt(id_dec, &dec_val, 0, 0)) {
        if (dec_val < 0) dec_val = 0;
        if ((uint32_t)dec_val > max_value) dec_val = (int)max_value;
        *value = (uint32_t)dec_val;
        changed = true;
    }

    return changed;
}

extern "C" void imgui_memext_open_save(en_MemextContentType type) {
    s_save_type = type;
    s_save_open = true;
    s_save_mem_from = 0;
    s_save_mem_size = (type == MEMEXT_CONTENT_RAM) ? MEMEXT_RAM_SIZE : MEMEXT_FLASH_SIZE;
    s_save_add_mzf = false;
    s_save_error[0] = '\0';
}

extern "C" void imgui_memext_save_dialog(void) {
    if (!s_save_open) return;

    uint32_t max_mem = (s_save_type == MEMEXT_CONTENT_RAM) ? MEMEXT_RAM_SIZE : MEMEXT_FLASH_SIZE;
    const char *title = (s_save_type == MEMEXT_CONTENT_RAM) ?
        _("Save MemExt RAM Into File") : _("Save MemExt FLASH Into File");

    ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse;

    /* Auto-layout poblíž kurzoru pro modální dialog. */
    auto_layout_first_use_near_mouse(title, 500.0f, 400.0f);
    if (ImGui::Begin(title, &s_save_open, flags)) {

        /* Sekce: Memory Allocation */
        ImGui::SeparatorText(_("Memory Allocation"));

        HexDecInput(_("From:"), &s_save_mem_from, max_mem - 1, "sfrom");

        ImGui::SameLine(0, 20);
        uint32_t max_size = max_mem - s_save_mem_from;
        HexDecInput(_("Size:"), &s_save_mem_size, max_size, "ssize");

        /* Sekce: MZF Header */
        ImGui::SeparatorText(_("MZF Header"));

        ImGui::Checkbox(_L("Add MZF Header"), &s_save_add_mzf);

        if (!s_save_add_mzf) ImGui::BeginDisabled();

        /* MZF Type */
        int mzf_type_int = s_save_mzf_type;
        ImGui::Text("%s", _("Type:"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(50);
        char hex_type[4];
        snprintf(hex_type, sizeof(hex_type), "%02X", s_save_mzf_type);
        if (ImGui::InputText("##mzf_type", hex_type, sizeof(hex_type), ImGuiInputTextFlags_CharsHexadecimal)) {
            s_save_mzf_type = (uint8_t)strtoul(hex_type, NULL, 16);
        }

        /* MZF Start & Exec */
        ImGui::SameLine(0, 20);
        ImGui::Text("%s", _("Start:"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        char hex_start[8];
        snprintf(hex_start, sizeof(hex_start), "%04X", s_save_mzf_start);
        if (ImGui::InputText("##mzf_start", hex_start, sizeof(hex_start), ImGuiInputTextFlags_CharsHexadecimal)) {
            s_save_mzf_start = (uint16_t)strtoul(hex_start, NULL, 16);
        }

        ImGui::SameLine(0, 20);
        ImGui::Text("%s", _("Exec:"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        char hex_exec[8];
        snprintf(hex_exec, sizeof(hex_exec), "%04X", s_save_mzf_exec);
        if (ImGui::InputText("##mzf_exec", hex_exec, sizeof(hex_exec), ImGuiInputTextFlags_CharsHexadecimal)) {
            s_save_mzf_exec = (uint16_t)strtoul(hex_exec, NULL, 16);
        }

        /* MZF Filename */
        ImGui::Text("%s", _("Filename:"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180);
        ImGui::InputText("##mzf_fname", s_save_mzf_filename, sizeof(s_save_mzf_filename));

        /* MZF Comment */
        ImGui::Text("%s", _("Comment:"));
        ImGui::InputTextMultiline("##mzf_comment", s_save_mzf_comment, sizeof(s_save_mzf_comment),
                                   ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3));

        if (!s_save_add_mzf) ImGui::EndDisabled();

        /* Tlačítka */
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float total_btn_width = 120 * 2 + ImGui::GetStyle().ItemSpacing.x;
        float avail = ImGui::GetContentRegionAvail().x;
        float offset = (avail - total_btn_width) * 0.5f;
        if (offset > 0.0f)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);

        bool can_save = (s_save_mem_size > 0);
        if (!can_save) ImGui::BeginDisabled();

        if (ImGui::Button(_L("Save"), ImVec2(120, 0))) {
            /* Otevřít file chooser pro výběr cílového souboru */
            IGFD::FileDialogConfig config;
            config.path = ".";
            config.countSelectionMax = 1;
            config.flags = ImGuiFileDialogFlags_Modal |
                           ImGuiFileDialogFlags_DontShowHiddenFiles |
                           ImGuiFileDialogFlags_ShowDevicesButton |
                           ImGuiFileDialogFlags_ConfirmOverwrite;
            imgui_filechooser_prepare_config(BASEUI_FCHOOSER_CAT_MEMEXT, config);
            ImGuiFileDialog::Instance()->OpenDialog("MemextSaveFch", _("Save file"), ".dat,.bin,.mzf,.*", config);
            s_save_fch_open = true;
        }

        if (!can_save) ImGui::EndDisabled();

        ImGui::SameLine();

        if (ImGui::Button(_L("Cancel"), ImVec2(120, 0))) {
            s_save_open = false;
        }

        /* Chybová hláška */
        if (s_save_error[0] != '\0') {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s", s_save_error);
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();

    /* File chooser pro Save */
    if (s_save_fch_open) {
        ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);
        if (ImGuiFileDialog::Instance()->Display("MemextSaveFch")) {
            if (ImGuiFileDialog::Instance()->IsOk()) {
                std::string path = ImGuiFileDialog::Instance()->GetFilePathName();
                FILE *fp = fopen(path.c_str(), "wb");
                if (fp) {
                    /* Volitelně zapsat MZF hlavičku */
                    if (s_save_add_mzf) {
                        struct MzfHeader hdr;
                        memset(&hdr, 0, sizeof(hdr));
                        hdr.type = s_save_mzf_type;
                        strncpy(hdr.filename, s_save_mzf_filename, 16);
                        hdr.size = (uint16_t)(s_save_mem_size & 0xFFFF);
                        hdr.start_addr = s_save_mzf_start;
                        hdr.exec_addr = s_save_mzf_exec;
                        strncpy(hdr.comment, s_save_mzf_comment, 104);
                        fwrite(&hdr, 1, 128, fp);
                    }

                    /* Zapsat data z paměti */
                    uint8_t *src = (s_save_type == MEMEXT_CONTENT_RAM) ?
                        g_memext.RAM : g_memext.FLASH;
                    size_t written = fwrite(src + s_save_mem_from, 1, s_save_mem_size, fp);
                    fclose(fp);

                    if (written == s_save_mem_size) {
                        s_save_open = false;
                    } else {
                        snprintf(s_save_error, sizeof(s_save_error),
                                 "Written only %u of %u bytes", (unsigned)written, s_save_mem_size);
                    }
                } else {
                    snprintf(s_save_error, sizeof(s_save_error), "%s", _("Cannot open file for writing"));
                }
            }
            ImGuiFileDialog::Instance()->Close();
            s_save_fch_open = false;
        }
    }
}
