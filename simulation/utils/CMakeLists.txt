if(ENABLE_TESTS)
  add_executable(test-runner test-runner.cc)
  # Test suites register through static constructors; do not drop their libraries.
  target_link_libraries(test-runner ${LIB_AS_NEEDED_PRE}
    "${ns3-libs-tests}" "${ns3-libs}" "${ns3-contrib-libs}"
    "${ns3-external-libs}" ${LIB_AS_NEEDED_POST})
  set_runtime_outputdirectory(test-runner "${CMAKE_OUTPUT_DIRECTORY}/utils/" "")
endif()
