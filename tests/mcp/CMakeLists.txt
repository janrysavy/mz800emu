# tests/mcp/CMakeLists.txt - MCP backend testy
#
# V0.A.2: standalone test JSONL parseru.
# V0.A.3: standalone test dispatch vrstvy s mock dbgapi (stub).
#
# Oba testy jsou STANDALONE (= Unity bez emu core), linkují přímo
# zdrojové soubory z src/emulator/mcp/ s definicí MZ800EMU_MCP_TEST_BUILD
# která vypne include `mzarch_config.h`.
#
# Pokud MZ_NO_MCP=ON, testy nemají smysl (zdroje by se v guard-blockovaném
# režimu zkompilovaly jako prázdné TU) - guard navíc výslovně přeskočíme
# registraci.

if(MZ_NO_MCP)
    message(STATUS "  tests/mcp: skipped (MZ_NO_MCP=ON)")
    return()
endif()

# ---------------------------------------------------------------
# Test 1: JSONL parser (V0.A.2)
# ---------------------------------------------------------------
mz_add_test(mcp_jsonl_parser
    TYPE     STANDALONE
    SOURCES
        test_jsonl_parser.c
        ${CMAKE_SOURCE_DIR}/src/emulator/mcp/jsonl_io.c
    LABELS   "standalone;mcp"
)

# Override compile definitions na samotný test target - potřebuje:
#   MZ800EMU_MCP_TEST_BUILD - vypne include mzarch_config.h v jsonl_io.c
#   MZ800EMU_CFG_MCP_SERVER_ENABLED - aktivuje tělo jsonl_io.c
target_compile_definitions(test_mcp_jsonl_parser PRIVATE
    MZ800EMU_MCP_TEST_BUILD
    MZ800EMU_CFG_MCP_SERVER_ENABLED
)

# Linkneme json-glib + glib (jsonl_io.c je na nich závislý).
target_link_libraries(test_mcp_jsonl_parser PRIVATE
    mz::glib
    mz::json_glib
)

# ---------------------------------------------------------------
# Test 2: Dispatch vrstva (V0.A.3)
# ---------------------------------------------------------------
# Linkuje dispatch.c + jsonl_io.c + stub pro dbgapi
# (test_dispatch_dbgapi_stub.c poskytuje g_dbgapi_cmdrq_queue symbol
# a mock implementaci dbgapi_ui_submit_cmd_sync_with_origin).
mz_add_test(mcp_dispatch
    TYPE     STANDALONE
    SOURCES
        test_dispatch.c
        test_dispatch_dbgapi_stub.c
        test_hid_keymap_stub.c
        ${CMAKE_SOURCE_DIR}/src/emulator/mcp/dispatch.c
        ${CMAKE_SOURCE_DIR}/src/emulator/mcp/hid_keymap.c
        ${CMAKE_SOURCE_DIR}/src/emulator/mcp/jsonl_io.c
        ${CMAKE_SOURCE_DIR}/src/emulator/mcp/cooperation.c
        ${CMAKE_SOURCE_DIR}/src/emulator/mcp/event_bus.c
        ${CMAKE_SOURCE_DIR}/src/emulator/mcp/trap_manager.c
    LABELS   "standalone;mcp"
)

target_compile_definitions(test_mcp_dispatch PRIVATE
    MZ800EMU_MCP_TEST_BUILD
    MZ800EMU_CFG_MCP_SERVER_ENABLED
)

target_link_libraries(test_mcp_dispatch PRIVATE
    mz::glib
    mz::json_glib
)


# ---------------------------------------------------------------
# Test 3: MCP konfigurace - INI persist + validace (V0.B.4)
# ---------------------------------------------------------------
# LIB test - linkuje cfgmain + cfgfile + mcp_config pres emu core.
# Vyzaduje MCP TCP backend (NO_MCP_TCP=0).
if(NOT MZ_NO_MCP_TCP)
    mz_add_test(mcp_config
        TYPE     LIB
        SOURCES  test_mcp_config.c
        LABELS   "lib;mcp;config"
    )
else()
    message(STATUS "  tests/mcp: test_mcp_config skipped (MZ_NO_MCP_TCP=ON)")
endif()


# ---------------------------------------------------------------
# Test 4: Pipe transport E2E (V0.A.4)
# ---------------------------------------------------------------
# Python E2E test - spawns the built mz800emu(.exe) with --mcp-pipe and
# exercises the dispatch over JSONL stdin/stdout. Requires the mz800emu
# target to be built and present in CMAKE_SOURCE_DIR (= repo root, kam
# AddMzEmu.cmake kopíruje binárku via cp -f).
#
# Test sám má 10 podtestů (hello, ping, pause, get_state, get_registers,
# mem_read, reset, bp_add, unknown_cmd, shutdown). Pokud kterýkoliv
# selže, Python skript vrátí exit 1 a ctest test FAIL.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(
        NAME    mcp_pipe_e2e
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_pipe.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_pipe_e2e PROPERTIES
        LABELS  "e2e;mcp;pipe"
        TIMEOUT 60
    )
    # Pojistka pro paralelní ctest - test pipe sám spawnuje mz800emu
    # subprocess, takže by neměl běžet souběžně se sám sebou. Žádný
    # RESOURCE_LOCK protože různé MZARCH targets nesdílíme.
    message(STATUS "  tests/mcp: registered mcp_pipe_e2e "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Test 4: MCP Python wrapper E2E přes JSON-RPC stdio (V0.B.3)
    # -----------------------------------------------------------
    # Spawne mcp_server.py jako subprocess, simuluje MCP klienta
    # (initialize + tools/list + tools/call), tím end-to-end ověří
    # celý stack Python wrapper -> JSONL -> emu backend. Vyžaduje
    # mcp[cli] v ``mcp-server/.venv`` (auto-detekce, fallback na
    # systémový Python).
    #
    # Test sám spustí mcp_server.py přes jeho vlastní venv interpret,
    # který má FastMCP. Ctest jen použije systémový Python jako
    # launcher (cesty venv si test resolvuje sám).
    add_test(
        NAME    mcp_server_stdio_e2e
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_server_stdio.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_server_stdio_e2e PROPERTIES
        LABELS  "e2e;mcp;stdio"
        TIMEOUT 90
    )
    message(STATUS "  tests/mcp: registered mcp_server_stdio_e2e "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Test 5: TCP transport region wedge regrese (bug 0011)
    # -----------------------------------------------------------
    # Mock TCP server (BEZ emu) ověří, že velký region_read response
    # (>64 KiB JSONL řádek) nezasekne bridge, že error response se
    # surfacuje místo holého {}, a že _send_request self-healuje
    # off-by-one desync. Test importuje mcp_server in-process, takže
    # potřebuje FastMCP venv - sám se re-execne pod venv interpretem,
    # nebo skipne (exit 77) když venv chybí.
    add_test(
        NAME    mcp_tcp_region_wedge
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_tcp_region_wedge.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_tcp_region_wedge PROPERTIES
        LABELS  "e2e;mcp;tcp"
        TIMEOUT 60
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_tcp_region_wedge "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Test 6: video záznam přes MCP (video-capture Task 15)
    # -----------------------------------------------------------
    # Spawne mz800emu v pipe módu (izolovaný --cfg-dir + INI v dočasném
    # adresáři) a projde postup agenta: videorec_start, emu run frames,
    # marker, snapshot retake, record-pause, stop -> ověří AVI (počet snímků
    # přes ffprobe, jinak hlavička AVI) a sidecar; plus auto-stop po N
    # snímcích s vygenerovaným jménem. Video záznam je jen na MZ-800, test
    # spouští binárku mz800emu z kořene repa (nebo MZ_EMU).
    add_test(
        NAME    mcp_videorec_e2e
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_videorec_e2e.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_videorec_e2e PROPERTIES
        LABELS  "e2e;mcp;pipe;videorec"
        TIMEOUT 120
    )
    message(STATUS "  tests/mcp: registered mcp_videorec_e2e "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Test 7: tempo emulace v headless režimu (video-capture Task 16)
    # -----------------------------------------------------------
    # Spawne mz800emu v pipe módu bez programu (headless = bez audio
    # zařízení) a ověří, že při 100 % jde emulace reálným časem (cca 50
    # snímků/s, ne cca 1 snímek/s ani MAX SPEED), 200 % je dvojnásobné,
    # MAX SPEED není brzděný a návrat z MAX SPEED nedohání zameškaný čas.
    # RUN_SERIAL: měří reálný čas, souběh s jinými testy by ho zkresloval.
    add_test(
        NAME    mcp_headless_pacing
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_headless_pacing.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_headless_pacing PROPERTIES
        LABELS  "e2e;mcp;pipe"
        TIMEOUT 120
        RUN_SERIAL TRUE
    )
    message(STATUS "  tests/mcp: registered mcp_headless_pacing "
                   "(Python ${Python3_VERSION})")

    # -----------------------------------------------------------
    # Test 8: GUI bez audio zařízení (video-capture Task 23)
    # -----------------------------------------------------------
    # Spustí GUI mz800emu (otevře okno) s neexistujícím SDL_AUDIO_DRIVER
    # a MCP TCP portem 23877; ověří start, varování v logu, tempo cca 50
    # snímků/s, MAX SPEED a zvuk video záznamu podle reality bez zařízení.
    # Kvůli oknu jen na vyžádání: bez MZ_GUI_TESTS=1 vrací 77 (SKIP).
    add_test(
        NAME    mcp_gui_no_audio
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/test_gui_no_audio.py
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(mcp_gui_no_audio PROPERTIES
        LABELS  "e2e;mcp;tcp;gui"
        TIMEOUT 180
        RUN_SERIAL TRUE
        SKIP_RETURN_CODE 77
    )
    message(STATUS "  tests/mcp: registered mcp_gui_no_audio "
                   "(Python ${Python3_VERSION}, needs MZ_GUI_TESTS=1)")
else()
    message(STATUS "  tests/mcp: mcp_pipe_e2e SKIPPED "
                   "(Python3 interpreter not found)")
    message(STATUS "  tests/mcp: mcp_server_stdio_e2e SKIPPED "
                   "(Python3 interpreter not found)")
endif()
