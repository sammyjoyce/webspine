find_package(GTest REQUIRED)
include(FetchContent)

set(HEGEL_REFLECTION OFF CACHE BOOL "" FORCE)
if(DEFINED ENV{HEGEL_CPP_SOURCE})
  set(FETCHCONTENT_SOURCE_DIR_HEGEL $ENV{HEGEL_CPP_SOURCE})
endif()
if(DEFINED ENV{HEGEL_LIBHEGEL_LIBRARY})
  set(HEGEL_LIBHEGEL_LIBRARY $ENV{HEGEL_LIBHEGEL_LIBRARY} CACHE FILEPATH "")
endif()
FetchContent_Declare(hegel GIT_REPOSITORY https://github.com/hegeldev/hegel-cpp.git GIT_TAG v0.13.0)
FetchContent_MakeAvailable(hegel)

add_executable(core_test core_test.cpp)
target_link_libraries(core_test PRIVATE webspine_lib GTest::gtest_main)
add_test(NAME core COMMAND core_test)

add_executable(property_test property_test.cpp)
target_link_libraries(property_test PRIVATE webspine_lib hegel GTest::gtest_main)
add_test(NAME properties COMMAND property_test)

add_executable(e2e_test e2e_test.cpp)
target_link_libraries(e2e_test PRIVATE webspine_lib GTest::gtest_main)
target_compile_definitions(e2e_test PRIVATE WEBSPINE_BINARY="$<TARGET_FILE:webspine>"
                                            WEBSPINE_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_dependencies(e2e_test webspine)
add_test(NAME e2e COMMAND e2e_test)
set_tests_properties(e2e PROPERTIES TIMEOUT 600)
