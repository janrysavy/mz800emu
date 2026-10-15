# tests/audio/CMakeLists.txt
# Zachycení zvuku do složky (audio_capture.h) nad emu core; háčky zvukové
# cesty se v testech volají přímo.

mz_add_test(audio_capture
    TYPE     EMU
    SOURCES  test_audio_capture.c
    LABELS   "emu;audio"
)
