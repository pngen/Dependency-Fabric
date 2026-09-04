# tools
if(WIN32)
  add_executable(df_app df_app_main.cpp)
  target_link_libraries(df_app PRIVATE dependency_fabric)
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/proof_output")
  add_test(NAME multiprocess_proof
           COMMAND df_app --role orchestrator --port 0 --dir "${CMAKE_BINARY_DIR}/proof_output")
endif()
add_executable(df_cli df_cli.cpp)
target_link_libraries(df_cli PRIVATE dependency_fabric)
