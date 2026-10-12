# Virtual drop tuning (src/audio/DropTuner.cpp). JUCE-free: a unit test that
# checks pitch, latency bound, sample rates, block-size independence and
# clamping, plus an offline render tool (raw float32 in/out) for scoring the
# shifter on recorded takes.
set(DROP_TUNER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../src/audio/DropTuner.cpp")
set(AUDIO_INC "${CMAKE_CURRENT_SOURCE_DIR}/../../src/audio")

add_executable(drop_tuner_test drop_tuner_test.cpp "${DROP_TUNER_SRC}")
target_compile_features(drop_tuner_test PRIVATE cxx_std_17)
target_include_directories(drop_tuner_test PRIVATE "${AUDIO_INC}")
add_test(NAME drop_tuner COMMAND drop_tuner_test)

add_executable(drop_tuner_render drop_tuner_render.cpp "${DROP_TUNER_SRC}")
target_compile_features(drop_tuner_render PRIVATE cxx_std_17)
target_include_directories(drop_tuner_render PRIVATE "${AUDIO_INC}")
