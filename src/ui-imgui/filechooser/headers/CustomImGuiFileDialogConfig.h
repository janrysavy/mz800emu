#pragma once

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

// #include <imgui.h>
#include "ui-imgui/filechooser/res/CustomFont.h"
#include "libs/imwidgets/ImWidgets.h"
#include "ui-imgui/filechooser/igfd_ToggleButton.h"
#include "i18n.h"

// uncomment and modify defines under for customize ImGuiFileDialog

// if you need to use your FileSystem Interface, you msut define the include of your FileSystem Interface file
// if commented, you have two default interface, std::filesystem or dirent
// #define CUSTOM_FILESYSTEM_INCLUDE

// this options need c++17
//  in this app its defined in CMakeLists.txt
// #define USE_STD_FILESYSTEM

// #define MAX_FILE_DIALOG_NAME_BUFFER 1024
// #define MAX_PATH_BUFFER_SIZE 1024

// the spacing between button path's can be customized.
// if disabled the spacing is defined by the imgui theme
// define the space between path buttons
// #define CUSTOM_PATH_SPACING 2

//#define USE_THUMBNAILS
// the thumbnail generation use the stb_image and stb_resize lib who need to define the implementation
// btw if you already use them in your app, you can have compiler error due to "implementation found in double"
// so uncomment these line for prevent the creation of implementation of these libs again in ImGuiFileDialog
// #define DONT_DEFINE_AGAIN__STB_IMAGE_IMPLEMENTATION
// #define DONT_DEFINE_AGAIN__STB_IMAGE_RESIZE_IMPLEMENTATION
// #define IMGUI_RADIO_BUTTON RadioButton
// #define DisplayMode_ThumbailsList_ImageHeight 32.0f
// #define tableHeaderFileThumbnailsString " Thumbnails"
#define DisplayMode_FilesList_ButtonString ICON_IGFD_FILE_LIST
// #define DisplayMode_FilesList_ButtonHelp "File List"
#define DisplayMode_ThumbailsList_ButtonString ICON_IGFD_FILE_LIST_THUMBNAILS
// #define DisplayMode_ThumbailsList_ButtonHelp "Thumbnails List"
#define DisplayMode_ThumbailsGrid_ButtonString ICON_IGFD_FILE_GRID_THUMBNAILS
// #define DisplayMode_ThumbailsGrid_ButtonHelp "Thumbnails Grid"
//  sleep time in millisedonce to apply in the thread loop, when no datas to extract
//  the sleep time can be ssen jsut after the opening of a directory who contain pictures
#define DisplayMode_ThumbailsGrid_ThreadSleepTimeInMS 50

#define USE_EXPLORATION_BY_KEYS
// Up key for explore to the top
#define IGFD_KEY_UP ImGuiKey_UpArrow
// Down key for explore to the bottom
#define IGFD_KEY_DOWN ImGuiKey_DownArrow
// Enter key for open directory
#define IGFD_KEY_ENTER ImGuiKey_Enter
// BackSpace for comming back to the last directory
#define IGFD_KEY_BACKSPACE ImGuiKey_Backspace

// by ex you can quit the dialog by pressing the key excape
#define USE_DIALOG_EXIT_WITH_KEY
#define IGFD_EXIT_KEY ImGuiKey_Escape

// widget
// begin combo widget
#define IMGUI_BEGIN_COMBO ImGui::BeginContrastedCombo
// uncomment if you want to have the combo resized by its content
// when auto resized, FILTER_COMBO_MIN_WIDTH will be considered has minimum width
#define FILTER_COMBO_AUTO_SIZE 1
// filter combobox minimal width
#define FILTER_COMBO_MIN_WIDTH 50.0f
// button widget use for compose path
#define IMGUI_PATH_BUTTON ImGui::ContrastedButton_For_Dialogs
// standard button
#define IMGUI_BUTTON ImGui::ContrastedButton_For_Dialogs

// locales string — ikonové labely (nepřekládají se)
#define createDirButtonString ICON_IGFD_ADD
#define resetButtonString ICON_IGFD_RESET
#define devicesButtonString ICON_IGFD_DRIVES
#define editPathButtonString ICON_IGFD_EDIT
#define searchString ICON_IGFD_SEARCH
#define dirEntryString ICON_IGFD_FOLDER
#define linkEntryString ICON_IGFD_LINK
#define fileEntryString ICON_IGFD_FILE

// Tooltipy — N_() registruje pro xgettext, _() překládá za běhu v ImGuiFileDialog.cpp
#define buttonResetSearchString N_("Reset search")
#define buttonDriveString N_("Devices")
#define buttonEditPathString N_("Edit path\nYou can also right click on path buttons")
#define buttonResetPathString N_("Reset to current directory")
#define buttonCreateDirString N_("Create Directory")

// Labely
#define fileNameString N_("File Name:")
#define dirNameString N_("Directory Path:")

// Overwrite dialog — ikona a text oddělené pro lokalizaci
#define OverWriteDialogTitleString N_("The selected file already exists!")
#define OverWriteDialogMessageString N_("Are you sure you want to overwrite it?")
#define OverWriteDialogConfirmButtonIcon ICON_IGFD_OK
#define OverWriteDialogConfirmButtonString N_("Confirm")
#define OverWriteDialogCancelButtonIcon ICON_IGFD_CANCEL
#define OverWriteDialogCancelButtonString N_("Cancel")

// Validation buttons — ikona a text oddělené pro lokalizaci
// Šířka se počítá dynamicky, aby se OK a Cancel vešly a měly stejnou šířku
#define okButtonIcon ICON_IGFD_OK
#define okButtonString N_("OK")
#define cancelButtonIcon ICON_IGFD_CANCEL
#define cancelButtonString N_("Cancel")
// alignement [0:1], 0.0 is left, 0.5 middle, 1.0 right, and other ratios
#define okCancelButtonAlignement 0.0f
// #define invertOkAndCancelButtons 0

// see strftime functionin <ctime> for customize
// "%Y/%m/%d %H:%M:%S" give 2021:01:22 11:47:10
// "%Y/%m/%d %i:%M%p" give 2021:01:22 11:45PM
// #define DateTimeFormat "%Y/%m/%d %i:%M%p"

// jednotky velikosti souborů — defaultní hodnoty v IGFD jsou francouzské
#define fileSizeBytes N_("B")
#define fileSizeKiloBytes N_("KB")
#define fileSizeMegaBytes N_("MB")
#define fileSizeGigaBytes N_("GB")

// hlavičky tabulky
#define USE_CUSTOM_SORTING_ICON
#define tableHeaderAscendingIcon ICON_IGFD_CHEVRON_UP
#define tableHeaderDescendingIcon ICON_IGFD_CHEVRON_DOWN
#define tableHeaderFileNameString N_("File name")
#define tableHeaderFileTypeString N_("Type")
#define tableHeaderFileSizeString N_("Size")
#define tableHeaderFileDateString N_("Date")

#define USE_PLACES_FEATURE
#define PLACES_PANE_DEFAULT_SHOWN true
// #define placesPaneWith 150.0f
// #define IMGUI_TOGGLE_BUTTON ToggleButton
#define IMGUI_TOGGLE_BUTTON igfd_ToggleButton
#define placesButtonString ICON_IGFD_PLACES
#define placesButtonHelpString N_("Places")
#define addPlaceButtonString ICON_IGFD_ADD
#define removePlaceButtonString ICON_IGFD_REMOVE
#define validatePlaceButtonString ICON_IGFD_OK
#define editPlaceButtonString ICON_IGFD_EDIT

// a group for bookmarks will be added by default, but you can also create it yourself and many more
// Záložky uživatele (+/- v panelu Places). Ukládají se do INI emulátoru
// (sekce [FILECHOOSER], klíč bookmarks) přes SerializePlaces(), viz
// imgui_filechooser.cpp. Jméno skupiny je klíčem serializace - neměnit,
// jinak se uložené záložky nenačtou.
#define USE_PLACES_BOOKMARKS
#define PLACES_BOOKMARK_DEFAULT_OPEPEND false
#define placesBookmarksGroupName ICON_IGFD_BOOKMARK " Bookmarks"
#define placesBookmarksDisplayOrder 0  // to the first

// a group for system devices (returned by IFileSystem), but you can also add yours
#define USE_PLACES_DEVICES
#define PLACES_DEVICES_DEFAULT_OPEPEND true
#define placesDevicesGroupName ICON_IGFD_DRIVES " Devices"
#define placesDevicesDisplayOrder 10  // to the end
