# NS7 Audio: CoreAudio plug-in (AudioServerPlugIn) for the Numark NS7.
#   cmake -S plugin -B build/plugin && cmake --build build/plugin
# Produces build/plugin/NS7Audio.driver, installed to /Library/Audio/Plug-Ins/HAL.
cmake_minimum_required(VERSION 3.15)
project(NS7Audio CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
# builds for this Mac's own architecture (arm64 on Apple Silicon), like coreaudiod

set(CODESIGN_ID "-" CACHE STRING "Code signing identity (- = ad-hoc)")

set(DRIVER_NAME "NS7Audio")
set(DRIVER_VERSION "0.1.0")
set(DRIVER_IDENTIFIER "com.ns7driver.audio")
set(DRIVER_UID "760DDBB9-8408-400A-A94E-FE4E4A7DC757")
set(DRIVER_ENTRYPOINT "NS7AudioEntryPoint")
set(DRIVER_MACH_SERVICE "com.ns7driver.daemon")

# libASPL (MIT), fetched and built as a static library
include(ExternalProject)
set(ASPL_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/libASPL-prefix)
ExternalProject_Add(libASPL
  GIT_REPOSITORY https://github.com/gavv/libASPL.git
  GIT_TAG v3.1.2
  GIT_SHALLOW TRUE
  CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
             -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
             -DCMAKE_BUILD_TYPE=Release
             -DBUILD_TESTING=OFF
  INSTALL_DIR ${ASPL_PREFIX}
  BUILD_BYPRODUCTS ${ASPL_PREFIX}/lib/libASPL.a
  LOG_CONFIGURE YES
  LOG_BUILD YES
  LOG_INSTALL YES
)

add_library(${DRIVER_NAME} MODULE NS7Audio.cpp)
add_dependencies(${DRIVER_NAME} libASPL)
target_include_directories(${DRIVER_NAME} PRIVATE ${ASPL_PREFIX}/include)
target_link_libraries(${DRIVER_NAME} PRIVATE ${ASPL_PREFIX}/lib/libASPL.a)
find_library(LIB_CoreFoundation CoreFoundation REQUIRED)
find_library(LIB_CoreAudio CoreAudio REQUIRED)
target_link_libraries(${DRIVER_NAME} PRIVATE ${LIB_CoreFoundation} ${LIB_CoreAudio})

set_target_properties(${DRIVER_NAME} PROPERTIES
  OUTPUT_NAME "${DRIVER_NAME}"
  BUNDLE TRUE
  BUNDLE_EXTENSION "driver"
  PREFIX ""
  SUFFIX ""
  MACOSX_BUNDLE ON
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
  MACOSX_BUNDLE_BUNDLE_NAME "${DRIVER_NAME}"
  MACOSX_BUNDLE_BUNDLE_VERSION "${DRIVER_VERSION}"
  MACOSX_BUNDLE_COPYRIGHT "MIT License"
  MACOSX_BUNDLE_GUI_IDENTIFIER "${DRIVER_IDENTIFIER}"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${DRIVER_VERSION}"
)

add_custom_command(TARGET ${DRIVER_NAME} POST_BUILD
  COMMAND codesign --force --sign "${CODESIGN_ID}" "$<TARGET_BUNDLE_DIR:${DRIVER_NAME}>"
  COMMENT "Signing ${DRIVER_NAME}.driver"
  VERBATIM)
