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

# Actual native raster prefix must retain the mapping before a register write.
mz_add_test(gdg_scroll_causality
    TYPE     EMU
    SOURCES  test_gdg_scroll_causality.c
    LABELS   "emu;peripherals;gdg"
)

# Bootstrap --run-mzf (MZ-800 mód) nastavuje DMD cestou OUT CEh
# (GATE0 CTC0, latch MZ-700), ne přímým zápisem g_gdg.regDMD.
mz_add_test(mz800_bootstrap_dmd
    TYPE     EMU
    SOURCES  test_mz800_bootstrap_dmd.c
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

# Vlastní délky pulzů Virtual CMT a hlavičky CMTSPEED zařízení UniCMT v MZT.
mz_add_test(cmt_unicmt
    TYPE     EMU
    SOURCES  test_cmt_unicmt.c
    LABELS   "emu;peripherals;cmt"
)

# Synthetic actual-OUT CMT shortcut provenance and v2 API noninterference.
mz_add_test(instruction_coverage_cmthack
    TYPE     EMU
    SOURCES  test_instruction_coverage_cmthack.c
    LABELS   "emu;peripherals;cmt;coverage;debugger"
)
