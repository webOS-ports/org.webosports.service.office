# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.10)
project(org.webosports.service.office C CXX)

include(webOS/webOS)
webos_modules_init(1 0 0 QUALIFIER RC4)
webos_component(1 0 0)

include(FindPkgConfig)

pkg_check_modules(GLIB2 REQUIRED glib-2.0)
pkg_check_modules(LS2 REQUIRED luna-service2)
pkg_check_modules(PBNJSON_C REQUIRED pbnjson_c)

# LibreOffice installs the LibreOfficeKit headers (libreoffice-lok-dev) but no
# .pc file; office-convert dlopen()s the library itself.
find_path(LOK_INCLUDE_DIR LibreOfficeKit/LibreOfficeKitInit.h REQUIRED)
set(LO_PROGRAM_DIR "/usr/lib/libreoffice/program" CACHE STRING "LibreOffice's program directory")
set(OFFICE_HELPER "${WEBOS_INSTALL_LIBEXEC}/office-convert")

include_directories(${GLIB2_INCLUDE_DIRS})

webos_add_compiler_flags(ALL -Wall -Wextra -Wno-unused-parameter)

add_executable(officed src/officed.c src/convert.c)
target_include_directories(officed PRIVATE ${LS2_INCLUDE_DIRS} ${PBNJSON_C_INCLUDE_DIRS})
target_compile_definitions(officed PRIVATE OFFICE_HELPER="${OFFICE_HELPER}")
target_link_libraries(officed ${GLIB2_LDFLAGS} ${LS2_LDFLAGS} ${PBNJSON_C_LDFLAGS})

add_executable(office-convert src/office-convert.cxx src/convert.c)
target_include_directories(office-convert PRIVATE ${LOK_INCLUDE_DIR})
target_compile_definitions(office-convert PRIVATE LO_PROGRAM_DIR="${LO_PROGRAM_DIR}")
target_link_libraries(office-convert ${GLIB2_LDFLAGS} ${CMAKE_DL_LIBS})

install(TARGETS officed DESTINATION ${WEBOS_INSTALL_SBINDIR})
install(TARGETS office-convert DESTINATION ${WEBOS_INSTALL_LIBEXEC})
webos_build_system_bus_files()
