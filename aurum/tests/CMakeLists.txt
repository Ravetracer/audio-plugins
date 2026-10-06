add_executable(geq_test geq_test.cpp)
target_link_libraries(geq_test PRIVATE aurum-dsp)

add_executable(engine_test engine_test.cpp WavFile.cpp)
target_link_libraries(engine_test PRIVATE aurum-dsp)
