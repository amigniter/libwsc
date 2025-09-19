#
# Author: Milan M.
# Copyright (c) 2025 AMSOFTSWITCH LTD. All rights reserved.
#

cmake_minimum_required(VERSION 3.10)
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake;${CMAKE_MODULE_PATH}")
project(libwsc 
  VERSION 1.0.0
  LANGUAGES C CXX
)

set (CMAKE_CXX_STANDARD 11)
set (CXX_STANDARD_REQUIRED ON)

option(USE_TLS "Enable TLS/SSL support using OpenSSL" OFF)
option(LIBWSC_USE_DEBUG "Enable debug (verbose) output" OFF)
option(BUILD_SHARED_LIBS "Build shared libraries instead of static ones" OFF)

set(LIBEVENT_COMPONENTS libevent pthreads)

if(USE_TLS)
    list(APPEND LIBEVENT_COMPONENTS openssl)
    find_package(OpenSSL REQUIRED)
endif()

find_package(Libevent REQUIRED COMPONENTS ${LIBEVENT_COMPONENTS})
find_package(ZLIB REQUIRED)

if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

if (UNIX)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -pedantic")
endif()

set (LIBWSC_SOURCES 
  libwsc/WebSocketClient.cpp 
  libwsc/Utf8Validator.cpp 
  libwsc/base64.cpp)
set (LIBWSC_HEADERS 
  libwsc/WebSocketClient.h
  libwsc/Logger.h
  libwsc/Utf8Validator.h
  libwsc/base64.h
  libwsc/WebSocketHeaders.h
  libwsc/WebSocketTLSOptions.h)

add_library(libwsc 
    ${LIBWSC_SOURCES}
    ${LIBWSC_HEADERS}
)

target_include_directories(libwsc
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/libwsc>
  PRIVATE
    ${LIBEVENT_INCLUDE_DIRS}
)

target_link_libraries(libwsc
  PRIVATE
    ${LIBEVENT_LIBRARIES}
    ZLIB::ZLIB
)

if (LIBWSC_USE_DEBUG)
    target_compile_definitions(libwsc PRIVATE LIBWSC_USE_DEBUG)
endif()

if (USE_TLS)
    target_compile_definitions(libwsc PRIVATE USE_TLS)
    target_link_libraries(libwsc PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

set_target_properties(libwsc PROPERTIES 
  LINK_FLAGS_RELEASE -s
  PREFIX ""
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
)

# Build example executables
add_executable(autobahn examples/autobahn.cpp)
target_include_directories(autobahn PRIVATE libwsc)
target_link_libraries(autobahn libwsc pthread)

add_executable(threads examples/threads.cpp)
target_include_directories(threads PRIVATE libwsc)
target_link_libraries(threads libwsc pthread)

install(TARGETS libwsc
  EXPORT libwscTargets
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libwsc/
  DESTINATION include/libwsc
  FILES_MATCHING PATTERN "*.h"
)

install(EXPORT libwscTargets
  FILE libwscConfig.cmake
  NAMESPACE libwsc::
  DESTINATION lib/cmake/libwsc
)