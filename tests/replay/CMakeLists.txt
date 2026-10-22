# Záznam a přehrání vstupů (replay).

# Formát .mzr: inputs.jsonl, hashes.csv, manifest a zápis/čtení souboru.
mz_add_test(replay-format
    TYPE     EMU
    SOURCES  test_replay_format.c
    LABELS   "emu;replay"
)

# Cesta vstupů záznamu bez GUI: čekající matice, reset na hranici snímku,
# zahození živého vstupu při přehrávání, přehrání bez rozjetí.
mz_add_test(replay-input
    TYPE     EMU
    SOURCES  test_replay_input.c
    LABELS   "emu;replay"
)

# Média v záznamu: obsah DSK a IDE8 při startu, přehrání nad kopiemi,
# originály a INI beze změny, návrat obrazů po konci přehrání.
mz_add_test(replay-media
    TYPE     EMU
    SOURCES  test_replay_media.c
    LABELS   "emu;replay"
)

# Checkpointy (rewind, seek): kruhový buffer, hledání do snímku, blokace
# zápisem na médium, zkrácení záznamu po rewindu.
mz_add_test(replay-ckpt
    TYPE     EMU
    SOURCES  test_replay_ckpt.c
    LABELS   "emu;replay"
)

# Rewind při záznamu a seek při přehrávání bez GUI: návrat stavu, zkrácení
# záznamu, odmítnutí po zápisu na médium, dohrání v MAX SPEED a restart.
mz_add_test(replay-rewind
    TYPE     EMU
    SOURCES  test_replay_rewind.c
    LABELS   "emu;replay"
)
