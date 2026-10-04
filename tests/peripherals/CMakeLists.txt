mz_add_test(ctc8253
    TYPE     EMU
    SOURCES  test_ctc8253.c
    LABELS   "emu;peripherals"
)

mz_add_test(pio8255
    TYPE     EMU
    SOURCES  test_pio8255.c
    LABELS   "emu;peripherals"
)

mz_add_test(psg
    TYPE     EMU
    SOURCES  test_psg.c
    LABELS   "emu;peripherals"
)

mz_add_test(unicard
    TYPE     EMU
    SOURCES  test_unicard.c
    LABELS   "emu;peripherals"
)

mz_add_test(wd279x
    TYPE     EMU
    SOURCES  test_wd279x.c
    LABELS   "emu;peripherals"
)

mz_add_test(gdg_mirror
    TYPE     EMU
    SOURCES  test_gdg_mirror.c
    LABELS   "emu;peripherals"
)

mz_add_test(printer
    TYPE     EMU
    SOURCES  test_printer.c
    LABELS   "emu;peripherals"
)

mz_add_test(mz1p16_emu
    TYPE     EMU
    SOURCES  test_mz1p16_emu.c
    LABELS   "emu;peripherals;mz1p16"
)

# Stav transportu CMT: invariant "bez pásky STOP", snapshot bez pásky,
# eject/stop bez pásky a přehrání nové pásky od začátku.
mz_add_test(cmt_state
    TYPE     EMU
    SOURCES  test_cmt_state.c
    LABELS   "emu;peripherals;cmt"
)
