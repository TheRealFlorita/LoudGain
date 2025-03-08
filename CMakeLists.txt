cmake_minimum_required(VERSION 3.10)

project(LoudGain LANGUAGES C CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(VERSION_MAJOR 1)
set(VERSION_MINOR 0)
set(VERSION_PATCH 0)

set(LoudGain_VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH})

set(INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/install")

file(GLOB SOURCES RELATIVE ${CMAKE_SOURCE_DIR} "src/*.cpp" "include/*.hpp" "include/argparse/*.hpp")

add_executable(LoudGain ${SOURCES})

configure_file("config.h.in" "config.h")

if (MSVC)
    add_definitions(-DTAGLIB_STATIC -D_OS_WIN32_)

    include_directories(
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/include/argparse
      ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ebur128/include
      ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/taglib/include
      ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ffmpeg/include)

    target_link_libraries(LoudGain
        ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/taglib/lib/tag.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ebur128/lib/ebur128.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ffmpeg/bin/swresample.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ffmpeg/bin/avformat.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ffmpeg/bin/avcodec.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ffmpeg/bin/avutil.lib)

    set_target_properties(LoudGain PROPERTIES COMPILE_FLAGS "/EHsc /W4 /O2 /MT")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /EHsc /W4 /O2 /MT")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc /W4 /O2 /MT")
    install(TARGETS LoudGain DESTINATION ${INSTALL_PREFIX})

elseif (MINGW)
    add_definitions(-DTAGLIB_STATIC -D_OS_WIN32_)

    include_directories(
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/include/argparse
      ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ebur128/include
      ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/taglib/include
      ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ffmpeg/include)

    target_link_libraries(LoudGain
        ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/taglib/lib/libtag.a
        ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ebur128/lib/libebur128.a
        ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ffmpeg/lib/libswresample.a
        ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ffmpeg/lib/libavformat.a
        ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ffmpeg/lib/libavcodec.a
        ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ffmpeg/lib/libavutil.a
        bcrypt)

    set_target_properties(LoudGain PROPERTIES COMPILE_FLAGS "-Wall -Wextra -g -O3 -static")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -static")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -static")
    install(TARGETS LoudGain DESTINATION ${INSTALL_PREFIX})

elseif (UNIX)
    add_definitions(-DTAGLIB_STATIC -D_OS_UNIX_)

    find_package(PkgConfig)
    find_package(Threads)
    PKG_CHECK_MODULES(LAVC libavcodec REQUIRED)
    PKG_CHECK_MODULES(LAVF libavformat REQUIRED)
    PKG_CHECK_MODULES(LAVR libswresample REQUIRED)
    PKG_CHECK_MODULES(LAVU libavutil REQUIRED)
    PKG_CHECK_MODULES(LTAG taglib REQUIRED)
    PKG_CHECK_MODULES(LEBU libebur128 REQUIRED)

    include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include/argparse
        ${LEBU_INCLUDE_DIRS}
        ${LAVC_INCLUDE_DIRS}
        ${LAVF_INCLUDE_DIRS}
        ${LAVR_INCLUDE_DIRS}
        ${LAVU_INCLUDE_DIRS}
        ${LTAG_INCLUDE_DIRS})

    target_link_libraries(LoudGain
        ${CMAKE_THREAD_LIBS_INIT}
        ${LEBU_LIBRARIES}
        ${LAVC_LIBRARIES}
        ${LAVF_LIBRARIES}
        ${LAVR_LIBRARIES}
        ${LAVU_LIBRARIES}
        ${LTAG_LIBRARIES})

    set_target_properties(LoudGain PROPERTIES COMPILE_FLAGS "-Wall -Wextra -g -O3")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3")
    install(TARGETS LoudGain DESTINATION ${CMAKE_INSTALL_PREFIX}/bin)
endif()
