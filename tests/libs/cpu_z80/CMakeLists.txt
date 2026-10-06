# tests/libs/cpu_z80/CMakeLists.txt
# Standalone test - mzlib_cpu_z80 + Unity (zadny emu core).
#
# Pokriva IFF1/IFF2 change callback (z80_set_iff_change) - V17+ debugger
# BP event cpu:iff1_change source pripravy. Viz emu-experiments/
# z80-iff1-hook/README.md.

mz_add_test(cpu_z80_iff_change
    TYPE     STANDALONE
    SOURCES  test_iff_change.c
    LIBS     mzlib_cpu_z80
    LABELS   "standalone;libs"
)

# Pokriva cpu_ctrl_event_cb (HALT enter/exit + RST nn) - event-viewer
# mutant Vlna 1 Commit 4. Viz emu-experiments/event-viewer/README.md.
mz_add_test(cpu_z80_cpu_ctrl_event
    TYPE     STANDALONE
    SOURCES  test_cpu_ctrl_event.c
    LIBS     mzlib_cpu_z80
    LABELS   "standalone;libs"
)

# Pokriva call_cb / ret_cb (CALL / RET dispatch hook) - callstack mutant
# Faze 1A. Viz emu-experiments/callstack/README.md.
mz_add_test(cpu_z80_call_ret_hooks
    TYPE     STANDALONE
    SOURCES  test_call_ret_hooks.c
    LIBS     mzlib_cpu_z80
    LABELS   "standalone;libs"
)

# Pokriva EI-delay v per-step variante jadra (regrese v2.0.2 - hra Gun Runner).
# INT po EI se smi prijmout az po nasledujici instrukci; jinak "EI; LD SP,nn"
# pushne na stary zasobnik a poskodi pamet. Viz emu-experiments/gun-runner/.
mz_add_test(cpu_z80_ei_delay
    TYPE     STANDALONE
    SOURCES  test_ei_delay.c
    LIBS     mzlib_cpu_z80
    LABELS   "standalone;libs"
)

# Pokriva inkrement R pri opakovanem fetchi HALT a pri prijeti INT/NMI
# (regrese v2.0.2 - loader hry Interkarate desifruje klicem z R po HALT).
# Viz emu-experiments/interkarate-cmt/.
mz_add_test(cpu_z80_r_register
    TYPE     STANDALONE
    SOURCES  test_r_register.c
    LIBS     mzlib_cpu_z80
    LABELS   "standalone;libs"
)
