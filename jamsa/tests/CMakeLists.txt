# Tests for the plug-in (JAMSA_TESTS=ON), run with ctest:
#
#   jamsa_unit_test  the saved state (also in a locale that writes 95,5, when one is installed),
#                    the names, following the host (the bar math, lining the intervals up with
#                    its bars, voting for its tempo, the editor's notes), the metronome's click,
#                    the editor's texts and BPM dragging, the text commands
#   jamsa_e2e_test   two engine wrappers in one process: one hosts a room, the other joins,
#                    a 997 Hz tone goes through process() with a simulated clock, in Opus and
#                    then in FLAC

add_executable(jamsa_unit_test jamsa_unit_test.cpp)
target_link_libraries(jamsa_unit_test PRIVATE jamsa_core)
add_test(NAME jamsa_unit_test COMMAND jamsa_unit_test)

add_executable(jamsa_e2e_test jamsa_e2e_test.cpp)
target_link_libraries(jamsa_e2e_test PRIVATE jamsa_core)
add_test(NAME jamsa_e2e_test COMMAND jamsa_e2e_test)
set_tests_properties(jamsa_e2e_test PROPERTIES TIMEOUT 300)
