# tests/audio/CMakeLists.txt
# Zachycení zvuku do složky (audio_capture.h) nad emu core; háčky zvukové
# cesty se v testech volají přímo.

mz_add_test(audio_capture
    TYPE     EMU
    SOURCES  test_audio_capture.c
    LABELS   "emu;audio"
)

# Čisté funkce rozboru zvuku (výška tónu, noty, úseky, převzorkování, WAV,
# posun signálů) bez emu core.
mz_add_test(audio_analysis
    TYPE     STANDALONE
    SOURCES  test_audio_analysis.c
             ${CMAKE_SOURCE_DIR}/src/emulator/audio_analysis.c
    LIBS     m
    LABELS   "standalone;audio"
)
target_include_directories(test_audio_analysis PRIVATE ${CMAKE_SOURCE_DIR}/src/emulator)
