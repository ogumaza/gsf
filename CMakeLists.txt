# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
project(gsf VERSION 0.1.0 LANGUAGES C CXX)

# xsflib is a library for players and rippers of the PSF family. It's a submodule. Its XsflibProject.cmake sets what a
# project built on it starts with: C++20, a release build by default, the static C runtime, the options that keep the
# output the same on every platform, and the warnings for gsf's targets (GSF_WARNINGS). It refuses the compilers whose
# output would differ from other builds.
if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/externals/xsflib/CMakeLists.txt")
    message(FATAL_ERROR "externals/xsflib is missing. Clone with git clone --recursive, or run "
                        "git submodule update --init in a clone.")
endif()
include(externals/xsflib/cmake/XsflibProject.cmake)
xsflib_project(CHECK_COMPILERS NAME gsf PREFIX GSF)

# seqlib writes MIDI files and SoundFonts. agblib's conversions require it, and gsf uses them for the songs' lengths.
# It comes first, so that xsflib takes it in place of the copy in xsflib's submodule. xsflib gives the PSF container,
# with zlib, and the frame of the rip tool. ntdlib gives the ARM interpreter that agblib builds on. agblib holds the
# GBA's code: the cartridge image, the model of the console that runs a game's sound driver, and the drivers.
set(XSFLIB_PSF ON)
set(XSFLIB_CLI OFF)
set(NTDLIB_SEQUENCE_EXPORT ON)
set(NTDLIB_DISC OFF)
set(AGBLIB_SEQUENCE_EXPORT ON)
xsflib_add_libraries(seqlib xsflib ntdlib agblib)

# The GSF format: its files and the loading of a set.
add_library(gsf_format STATIC
    src/gsf/gsf_file.cpp
    src/gsf/gsf_file.h
)
target_include_directories(gsf_format PUBLIC src)
target_link_libraries(gsf_format PUBLIC xsflib::psf)
target_compile_options(gsf_format PRIVATE ${GSF_WARNINGS})

# The ripper: the ARM assembler, the stub, the model's runs of a stub's plan, the drivers and the pieces of a set.
add_library(gsf_rip STATIC
    src/rip/arm.cpp
    src/rip/arm.h
    src/rip/driver.cpp
    src/rip/driver.h
    src/rip/drivers/alphadream.cpp
    src/rip/drivers/logikstate.cpp
    src/rip/drivers/logikstate.h
    src/rip/drivers/mp2k.cpp
    src/rip/drivers/quintet.cpp
    src/rip/drivers/rd1.cpp
    src/rip/drivers/rd2.cpp
    src/rip/model.cpp
    src/rip/model.h
    src/rip/rip.cpp
    src/rip/rip.h
    src/rip/stub.cpp
    src/rip/stub.h
)
target_link_libraries(gsf_rip PUBLIC gsf_format xsflib::rip agblib::agb_system agblib::alphadream_game
                      agblib::sequence_export_alphadream agblib::sequence_export_logikstate agblib::rd1_game
                      agblib::sequence_export_rd1 agblib::sequence_export_quintet
                      agblib::sequence_export_rd2
                      agblib::mp2k_game agblib::sequence_export_mp2k)
target_compile_options(gsf_rip PRIVATE ${GSF_WARNINGS})

# gsfrip rips a game's music into a GSF set. On Windows, its code page is UTF-8.
add_executable(gsfrip src/tools/gsfrip.cpp)
target_link_libraries(gsfrip PRIVATE gsf_rip xsflib::console)
target_compile_options(gsfrip PRIVATE ${GSF_WARNINGS})
xsflib_add_version(gsfrip VERSION ${gsf_VERSION} DEFINE GSF_VERSION DESCRIPTION "GSF ripper" PRODUCT gsf)
xsflib_add_utf8_manifest(gsfrip)

option(GSF_TESTS "Build gsf's tests and register them with CTest." ON)
if (GSF_TESTS)
    enable_testing()

    # The unit tests: gsf_<name>_test from tests/<name>_test.cpp, registered as gsf_<name>.
    xsflib_add_project_test(gsf gsf_file gsf_format)
    xsflib_add_project_test(gsf arm gsf_rip)
    xsflib_add_project_test(gsf stub gsf_rip)
    xsflib_add_project_test(gsf rip gsf_rip)
    xsflib_add_project_test(gsf logikstate gsf_rip)

    # gsfrip reports the version it was built as.
    string(REPLACE "." "\\." version_pattern ${gsf_VERSION})
    add_test(NAME gsfrip_version COMMAND gsfrip --version)
    set_tests_properties(gsfrip_version PROPERTIES PASS_REGULAR_EXPRESSION "^gsfrip ${version_pattern}[\r\n]*$")

    # Load local_tests.cmake from each immediate subdirectory to register tests that use locally supplied files.
    xsflib_include_local_tests()
endif()
