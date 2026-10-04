mz_add_test(breakpoints
    TYPE     EMU
    SOURCES  test_breakpoints.c
    LABELS   "emu;debugger"
)

mz_add_test(breakpoints_v2
    TYPE     EMU
    SOURCES  test_breakpoints_v2.c
    LABELS   "emu;debugger"
)

mz_add_test(bp_expr
    TYPE     EMU
    SOURCES  test_bp_expr.c
    LABELS   "emu;debugger"
)

mz_add_test(bp_vars
    TYPE     EMU
    SOURCES  test_bp_vars.c
    LABELS   "emu;debugger"
)

mz_add_test(watch
    TYPE     EMU
    SOURCES  test_watch.c
    LABELS   "emu;debugger"
)

mz_add_test(bookmarks
    TYPE     EMU
    SOURCES  test_bookmarks.c
    LABELS   "emu;debugger"
)

mz_add_test(freeze
    TYPE     EMU
    SOURCES  test_freeze.c
    LABELS   "emu;debugger"
)

mz_add_test(bp_action
    TYPE     EMU
    SOURCES  test_bp_action.c
    LABELS   "emu;debugger"
)

mz_add_test(bp_enforce
    TYPE     EMU
    SOURCES  test_bp_enforce.c
    LABELS   "emu;debugger"
)

mz_add_test(bp_match
    TYPE     EMU
    SOURCES  test_bp_match.c
    LABELS   "emu;debugger"
)

mz_add_test(bp_ratelimit
    TYPE     EMU
    SOURCES  test_bp_ratelimit.c
    LABELS   "emu;debugger"
)

mz_add_test(bp_persist_bc
    TYPE     EMU
    SOURCES  test_bp_persist_bc.c
    LABELS   "emu;debugger"
)
# if(TARGET): test_bp_persist_bc se při MZ_NO_DEBUGGER neregistruje (debugger
# label), pak target neexistuje a tato customizace by spadla configure chybou.
if(TARGET test_bp_persist_bc)
    target_compile_definitions(test_bp_persist_bc PRIVATE
        MZTEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    )
endif()

mz_add_test(bp_integration
    TYPE     EMU
    SOURCES  test_bp_integration.c
    LABELS   "emu;debugger;integration"
)

mz_add_test(bp_hw_event
    TYPE     EMU
    SOURCES  test_bp_hw_event.c
    LABELS   "emu;debugger"
)

mz_add_test(bp_sp_threshold
    TYPE     EMU
    SOURCES  test_bp_sp_threshold.c
    LABELS   "emu;debugger"
)

mz_add_test(bp_zone
    TYPE     EMU
    SOURCES  test_bp_zone.c
    LABELS   "emu;debugger"
)

mz_add_test(sym_db
    TYPE     EMU
    SOURCES  test_sym_db.c
    LABELS   "emu;debugger"
)

mz_add_test(dbgapi
    TYPE     EMU
    SOURCES  test_dbgapi.c
    LABELS   "emu;debugger"
)

mz_add_test(dbgapi_cmd_origin
    TYPE     EMU
    SOURCES  test_dbgapi_cmd_origin.c
    LABELS   "emu;debugger"
)

mz_add_test(dbgapi_submit_timeout
    TYPE     EMU
    SOURCES  test_dbgapi_submit_timeout.c
    LABELS   "emu;debugger"
)

# Zápis Memory Browseru přes CMDRQ frontu (ne přímo z UI vlákna).
# Linkuje skutečný backend membrowser_io.cpp (bez ImGui závislostí).
mz_add_test(membrowser_io_queue
    TYPE     EMU
    SOURCES  test_membrowser_io_queue.c
             ${CMAKE_SOURCE_DIR}/src/ui-imgui/debugger/membrowser/membrowser_io.cpp
    LABELS   "emu;debugger;ui-membrowser"
)

mz_add_test(tlog_common
    TYPE     EMU
    SOURCES  test_tlog_common.c
    LABELS   "emu;debugger;trace-suite"
)

mz_add_test(cputrack
    TYPE     EMU
    SOURCES  test_cputrack.c
    LABELS   "emu;debugger;trace-suite"
)

mz_add_test(iorqlog
    TYPE     EMU
    SOURCES  test_iorqlog.c
    LABELS   "emu;debugger;trace-suite"
)

mz_add_test(intlog
    TYPE     EMU
    SOURCES  test_intlog.c
    LABELS   "emu;debugger;trace-suite"
)

mz_add_test(hwlog
    TYPE     EMU
    SOURCES  test_hwlog.c
    LABELS   "emu;debugger;trace-suite"
)

mz_add_test(marklog
    TYPE     EMU
    SOURCES  test_marklog.c
    LABELS   "emu;debugger;trace-suite"
)

mz_add_test(eventlog
    TYPE     EMU
    SOURCES  test_eventlog.c
    LABELS   "emu;debugger;event-viewer"
)

mz_add_test(eventlog_filter
    TYPE     EMU
    SOURCES  test_eventlog_filter.c
    LABELS   "emu;debugger;event-viewer"
)

mz_add_test(eventlog_decoder
    TYPE     EMU
    SOURCES  test_eventlog_decoder.c
    LABELS   "emu;debugger;event-viewer"
)

mz_add_test(io_catalog
    TYPE     EMU
    SOURCES  test_io_catalog.c
    LABELS   "emu;debugger"
)

mz_add_test(io_activity
    TYPE     EMU
    SOURCES  test_io_activity.c
    LABELS   "emu;debugger"
)

mz_add_test(io_history
    TYPE     EMU
    SOURCES  test_io_history.c
    LABELS   "emu;debugger"
)

mz_add_test(io_history_filter
    TYPE     EMU
    SOURCES  test_io_history_filter.c
    LABELS   "emu;debugger"
)

mz_add_test(io_filter_andor_group
    TYPE     EMU
    SOURCES  test_io_filter_andor_group.c
    LABELS   "emu;debugger"
)

mz_add_test(io_ports_cfg
    TYPE     EMU
    SOURCES  test_io_ports_cfg.c
    LABELS   "emu;debugger"
)

mz_add_test(io_tracking_e2e
    TYPE     EMU
    SOURCES  test_io_tracking_e2e.c
    LABELS   "emu;debugger;integration"
)

mz_add_test(callstack
    TYPE     EMU
    SOURCES  test_callstack.c
    LABELS   "emu;debugger"
)

mz_add_test(profiler
    TYPE     EMU
    SOURCES  test_profiler.c
    LABELS   "emu;debugger"
)

mz_add_test(mhmap_resolve
    TYPE     EMU
    SOURCES  test_mhmap_resolve.c
    LABELS   "emu;debugger"
)
