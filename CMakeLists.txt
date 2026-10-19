cmake_minimum_required(VERSION 3.13)
project(filexfer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(FILEXFER_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
if(FILEXFER_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Протокол и fault-injection транспорт не зависят от сокетов — общая часть для программы и юнит-тестов
add_library(filexfer_core STATIC
    src/util.cpp
    src/protocol.cpp
    src/fault_transport.cpp)
target_include_directories(filexfer_core PUBLIC src)

add_executable(filexfer
    src/main.cpp
    src/log.cpp
    src/fd_transport.cpp
    src/tcp_transport.cpp
    src/server.cpp
    src/client.cpp)
target_link_libraries(filexfer PRIVATE filexfer_core)

add_executable(unit_tests tests/unit_tests.cpp)
target_link_libraries(unit_tests PRIVATE filexfer_core)

enable_testing()
add_test(NAME unit COMMAND unit_tests)
add_test(NAME integration
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh $<TARGET_FILE:filexfer>)
