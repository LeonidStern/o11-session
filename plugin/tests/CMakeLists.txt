# Tests for the plug-in (O11_TESTS=ON), run with ctest:
#
#   o11_unit_test  the saved state (also in a locale that writes 95,5, when one is installed),
#                    the names, following the host (the bar math, lining the intervals up with
#                    its bars, voting for its tempo, the editor's notes), the metronome's click,
#                    the editor's texts and BPM dragging, the text commands, the built-in
#                    server's addresses (the port mapper's lines, the public address lookup
#                    against a server on localhost, the host bar); Ableton Link: the beat,
#                    phase and tempo logic, one instance leading Link in real time
#   o11_e2e_test   two engine wrappers in one process: one hosts a room, the other joins,
#                    a 997 Hz tone goes through process() with a simulated clock, in Opus and
#                    then in FLAC; one of them leads Link, and a Link peer (as Live with its
#                    Link on) takes the room's tempo

add_executable(o11_unit_test o11_unit_test.cpp)
target_link_libraries(o11_unit_test PRIVATE o11_core)
o11_warnings(o11_unit_test)
add_test(NAME o11_unit_test COMMAND o11_unit_test)
# Link apps on one machine see each other: the tests that use Link run one at a time
set_tests_properties(o11_unit_test PROPERTIES RESOURCE_LOCK ableton_link)

add_executable(o11_e2e_test o11_e2e_test.cpp)
target_link_libraries(o11_e2e_test PRIVATE o11_core Ableton::Link) # a Link peer, as Live with its Link on
o11_warnings(o11_e2e_test)
add_test(NAME o11_e2e_test COMMAND o11_e2e_test)
# The room is on this computer, on any free port (another test or program may hold 2049): the
# router isn't asked to forward it, nor a service on the internet for the public address.
set_tests_properties(o11_e2e_test PROPERTIES TIMEOUT 300 RESOURCE_LOCK ableton_link ENVIRONMENT "O11_HOST_PORT=0;O11_PORTMAP=off;O11_WHATSMYIP=off")

# o11_editor_shots (O11_EDITOR_SHOTS=ON): not a test, it needs a display (xvfb-run gives one).
# The editor in a room on this computer, one instance hosting and others joining, painted to PNGs
# for looking at the layout:
#   O11_HOST_PORT=0 O11_PORTMAP=off O11_WHATSMYIP=off xvfb-run -a ./o11_editor_shots <folder>
option(O11_EDITOR_SHOTS "Build o11_editor_shots, pictures of the editor" OFF)
if(O11_EDITOR_SHOTS)
    juce_add_console_app(o11_editor_shots PRODUCT_NAME "o11_editor_shots")
    target_sources(o11_editor_shots PRIVATE
        o11_editor_shots.cpp
        ../source/processor.cpp
        ../source/editor.cpp
        ../source/views.cpp
    )
    target_compile_definitions(o11_editor_shots PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(o11_editor_shots
        PRIVATE o11_core o11_fonts juce::juce_audio_processors juce::juce_gui_basics
        PUBLIC juce::juce_recommended_config_flags
    )
    o11_warnings(o11_editor_shots)
endif()
