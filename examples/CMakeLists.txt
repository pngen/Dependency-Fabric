add_executable(df_basic basic.cpp)
target_link_libraries(df_basic PRIVATE dependency_fabric)
add_executable(df_cascade cascade.cpp)
target_link_libraries(df_cascade PRIVATE dependency_fabric)
