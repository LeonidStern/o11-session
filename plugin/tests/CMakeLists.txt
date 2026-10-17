# Tests for the plug-in (O11_TESTS=ON), run with ctest:
#
#   o11_unit_test  the saved state (also in a locale that writes 95,5, when one is installed),
#                    the names, following the host (the bar math, lining the intervals up with
#                    its bars, voting for its tempo, the editor's notes), the metronome's click,
#                    the editor's texts and BPM dragging, the text commands, the built-in
#                    server's addresses (the port mapper's lines, the public address lookup
#                    against a server on localhost, the host bar)
#   o11_e2e_test   two engine wrappers in one process: one hosts a room, the other joins,
#                    a 997 Hz tone goes through process() with a simulated clock, in Opus and
#                    then in FLAC

add_executable(o11_unit_test o11_unit_test.cpp)
target_link_libraries(o11_unit_test PRIVATE o11_core)
o11_warnings(o11_unit_test)
add_test(NAME o11_unit_test COMMAND o11_unit_test)

add_executable(o11_e2e_test o11_e2e_test.cpp)
target_link_libraries(o11_e2e_test PRIVATE o11_core)
o11_warnings(o11_e2e_test)
add_test(NAME o11_e2e_test COMMAND o11_e2e_test)
# The room is on this computer, on any free port (another test or program may hold 2049): the
# router isn't asked to forward it, nor a service on the internet for the public address.
set_tests_properties(o11_e2e_test PROPERTIES TIMEOUT 300 ENVIRONMENT "O11_HOST_PORT=0;O11_PORTMAP=off;O11_WHATSMYIP=off")
