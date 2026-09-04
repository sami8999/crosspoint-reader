add_executable(CompanionProtoTest
  CompanionProtoTest.cpp
  ${REPO_ROOT}/src/companion/proto/Cbor.cpp
  ${REPO_ROOT}/src/companion/proto/Frame.cpp
  ${REPO_ROOT}/src/companion/proto/Messages.cpp
)

target_include_directories(CompanionProtoTest PRIVATE
  ${REPO_ROOT}/src
)

# The companion sources are compiled out of every stock env; the tests opt in.
target_compile_definitions(CompanionProtoTest PRIVATE CROSSPOINT_COMPANION=1)

target_link_libraries(CompanionProtoTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CompanionProtoTest)
