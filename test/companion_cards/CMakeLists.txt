add_executable(CompanionCardsTest
  CardManagerTest.cpp
  ScheduleTest.cpp
  ${REPO_ROOT}/src/companion/proto/Cbor.cpp
  ${REPO_ROOT}/src/companion/proto/Frame.cpp
  ${REPO_ROOT}/src/companion/proto/Messages.cpp
  ${REPO_ROOT}/src/companion/ble/ChunkBitmap.cpp
  ${REPO_ROOT}/src/companion/ble/Transfer.cpp
  ${REPO_ROOT}/src/companion/cards/CardManager.cpp
)

target_include_directories(CompanionCardsTest PRIVATE
  ${REPO_ROOT}/src
  # FakePorts.h is shared with the BLE suite rather than duplicated.
  ${REPO_ROOT}/test/companion_ble
)

# The companion sources are compiled out of every stock env; the tests opt in.
target_compile_definitions(CompanionCardsTest PRIVATE CROSSPOINT_COMPANION=1)

target_link_libraries(CompanionCardsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompanionCardsTest)
