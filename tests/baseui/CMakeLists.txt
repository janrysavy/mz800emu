# Testy baseui vrstvy (čisté C části bez GUI).

# Paměť naposledy použitých adresářů dialogu pro výběr souboru
# (kategorie, seznam posledních, pravidla výchozího umístění, INI).
mz_add_test(fchooser_lastdir
    TYPE     EMU
    SOURCES  test_fchooser_lastdir.c
    LABELS   "emu;baseui"
)
