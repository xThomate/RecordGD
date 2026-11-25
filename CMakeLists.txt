cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(RecordGD VERSION 1.0.0)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

setup_geode_mod(${PROJECT_NAME})

# Windows Media Foundation (video/audio encoding), part of Windows.
if (WIN32)
    target_link_libraries(${PROJECT_NAME} mfplat mfreadwrite mfuuid ole32)
endif()

# After packaging, copy the .geode into release/ so it can be committed and downloaded from the repo.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/mod.json" RECORDGD_MOD_JSON)
string(JSON RECORDGD_MOD_ID GET "${RECORDGD_MOD_JSON}" id)
if (TARGET ${PROJECT_NAME}_PACKAGE)
    add_custom_target(${PROJECT_NAME}_RELEASE_COPY ALL
        DEPENDS ${PROJECT_NAME}_PACKAGE
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/release"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_BINARY_DIR}/${RECORDGD_MOD_ID}.geode"
            "${CMAKE_CURRENT_SOURCE_DIR}/release/${RECORDGD_MOD_ID}.geode"
        COMMENT "Copying ${RECORDGD_MOD_ID}.geode to release/"
        VERBATIM
    )
endif()
