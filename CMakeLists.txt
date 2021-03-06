cmake_minimum_required(VERSION 2.8)

PROJECT(yandex-porto)

OPTION(ENABLE_PYTHON "Enables building python API" ON)
OPTION(USE_SYSTEM_LIBNL "Use system libnl" ON)
OPTION(ENABLE_ASAN "Enables address sanitizer" OFF)

if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE MATCHES None)
	set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -std=c++11 -g")
set(CMAKE_EXE_LINKER_FLAGS "-static-libstdc++ -static-libgcc")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -fno-omit-frame-pointer --coverage")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG "--coverage")
set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(CMAKE_INSTALL_PREFIX /usr)

if(ENABLE_ASAN)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer -fno-common")
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libasan")
endif()

if(USE_SYSTEM_LIBNL)
	find_library(LIBNL NAMES libnl-3.a libnl-3 nl-3)
	find_library(LIBNL_ROUTE NAMES libnl-route-3.a libnl-route-3 nl-route-3)
	find_path(LIBNL_INCLUDE_DIR NAMES netlink/netlink.h PATH_SUFFIXES libnl3)
	include_directories(${LIBNL_INCLUDE_DIR})
else()
	include(ExternalProject)

	if(NOT EXISTS "/usr/bin/autoreconf")
		message(FATAL_ERROR "dh-autoreconf is required to build libnl")
	endif()

	if(NOT EXISTS "/usr/bin/pkg-config")
		message(FATAL_ERROR "pkg-config is required to build libnl")
	endif()

	find_package(BISON REQUIRED)
	find_package(FLEX REQUIRED)

	ExternalProject_Add(libnl
		#GIT_REPOSITORY git://github.com/tgraf/libnl.git
		#GIT_TAG libnl3_2_25
		URL https://github.com/tgraf/libnl/archive/libnl3_2_25.tar.gz
		URL_HASH SHA256=e4d8f77f86c23eb9b1384230936bcf482227b8dc0154398fffaf7d2aea7f476d
		UPDATE_COMMAND autoreconf -fi
		CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --disable-cli --disable-pthreads
		BUILD_COMMAND $(MAKE)
		INSTALL_COMMAND $(MAKE) install)
	ExternalProject_Get_Property(libnl INSTALL_DIR)
	include_directories(${INSTALL_DIR}/include/libnl3)
	SET(LIBNL ${INSTALL_DIR}/lib/libnl-3.a)
	SET(LIBNL_ROUTE ${INSTALL_DIR}/lib/libnl-route-3.a)
endif()

if(EXISTS "/usr/bin/g++-4.7")
	set(CMAKE_CXX_COMPILER "g++-4.7")
endif()

enable_testing()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_subdirectory(src)
add_subdirectory(test)

if(ENABLE_PYTHON)
	add_subdirectory(src/api/python)
endif()

set_source_files_properties(tags PROPERTIES GENERATED true)
add_custom_target(tags COMMAND ctags -R --c++-kinds=+p --fields=+iaS --extra=+q . WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

set_source_files_properties(TAGS PROPERTIES GENERATED true)
add_custom_target(TAGS COMMAND ctags -R -e --c++-kinds=+p --fields=+iaS --extra=+q . WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
