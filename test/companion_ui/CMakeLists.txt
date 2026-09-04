add_executable(CompanionUiTest
  ListFileTest.cpp
  ComposeTargetTest.cpp
  ChordTest.cpp
  OutboxEventsTest.cpp
  ${REPO_ROOT}/src/companion/proto/Cbor.cpp
  ${REPO_ROOT}/src/companion/proto/Frame.cpp
  ${REPO_ROOT}/src/companion/proto/Messages.cpp
  ${REPO_ROOT}/src/companion/store/Outbox.cpp
  ${REPO_ROOT}/src/companion/brain/ListFile.cpp
  ${REPO_ROOT}/src/companion/brain/ComposeTarget.cpp
  ${REPO_ROOT}/src/companion/chord/Chord.cpp
)

target_include_directories(CompanionUiTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/test
)

# The companion sources are compiled out of every stock env; the tests opt in.
target_compile_definitions(CompanionUiTest PRIVATE CROSSPOINT_COMPANION=1)

target_link_libraries(CompanionUiTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompanionUiTest)
