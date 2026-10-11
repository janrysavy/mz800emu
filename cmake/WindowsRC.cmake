# cmake/WindowsRC.cmake
#
# Helper pro kompilaci Windows .rc souborů přes windres do COFF .o.
# CMake má vlastní enable_language(RC), ale tady chceme přesnou parity
# s Makefile (windres -O coff).

if(WIN32)
    enable_language(RC)
    # Použít windres a generovat COFF výstup.
    # Záměrně nepředáváme <DEFINES> ani <INCLUDES> - .rc potřebuje jen path k .ico
    # (vedle .rc) a CMake by jinak protlačil tranzitivní include paths z všech
    # libů, což překročí Windows command-line limit (8191 znaků).
    set(CMAKE_RC_COMPILER_INIT windres)
    set(CMAKE_RC_COMPILE_OBJECT
        "<CMAKE_RC_COMPILER> -O coff -o <OBJECT> <SOURCE>"
    )
endif()

# Helper funkce: přidá .rc soubor jako zdroj k targetu (jen na Windows)
#
# Použití:
#   mz_add_windows_rc(mz800emu src/windows_rc/mz800emu.rc)
function(mz_add_windows_rc target rc_file)
    if(WIN32)
        target_sources(${target} PRIVATE ${rc_file})
        set(_dpi_rc "${CMAKE_SOURCE_DIR}/src/windows_rc/dpi_awareness.rc")
        target_sources(${target} PRIVATE ${_dpi_rc})
        set_source_files_properties(${_dpi_rc} PROPERTIES OBJECT_DEPENDS
            "${CMAKE_SOURCE_DIR}/src/windows_rc/dpi_awareness.manifest")
    endif()
endfunction()
