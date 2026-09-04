add_executable(CompanionBleTest
  ChunkBitmapTest.cpp
  OutboxTest.cpp
  TransferTest.cpp
  SessionTest.cpp
  ${REPO_ROOT}/src/companion/proto/Cbor.cpp
  ${REPO_ROOT}/src/companion/proto/Frame.cpp
  ${REPO_ROOT}/src/companion/proto/Messages.cpp
  ${REPO_ROOT}/src/companion/ble/ChunkBitmap.cpp
  ${REPO_ROOT}/src/companion/ble/Transfer.cpp
  ${REPO_ROOT}/src/companion/ble/Session.cpp
  ${REPO_ROOT}/src/companion/cards/CardManager.cpp
  ${REPO_ROOT}/src/companion/store/Outbox.cpp
)

target_include_directories(CompanionBleTest PRIVATE
  ${REPO_ROOT}/src
)

# The companion sources are compiled out of every stock env; the tests opt in.
target_compile_definitions(CompanionBleTest PRIVATE CROSSPOINT_COMPANION=1)

target_link_libraries(CompanionBleTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompanionBleTest)
