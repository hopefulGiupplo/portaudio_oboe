# - Find Oboe
# Find the Oboe includes and libraries
#  OBOE_LIBRARIES   - List of libraries when using Oboe.
#  OBOE_FOUND       - True if Oboe found.

if (NOT DEFINED PA_DIRECTORY)
	set(PA_DIRECTORY ${CMAKE_SOURCE_DIR})
endif ()


set (OBOE_DIRECTORY ${PA_DIRECTORY}/oboe-main)

add_subdirectory(${OBOE_DIRECTORY} ./oboe-bin)


set(OBOE_INCLUDE_DIR ${OBOE_DIRECTORY}/include)

set(OBOE_LIBRARY ${PA_DIRECTORY}/Build_${ANDROID_ABI}/oboe-bin/liboboe.a)
set(OBOE_LIBRARY_DIRS ${PA_DIRECTORY}/Build_${ANDROID_ABI}/oboe-bin/)

if(OBOE_INCLUDE_DIR)
	# Already in cache, be silent
	set(OBOE_FIND_QUIETLY TRUE)
else()
	find_package(PkgConfig)
	pkg_check_modules(PC_OBOE QUIET Oboe)
endif(OBOE_INCLUDE_DIR)

find_path(OBOE_INCLUDE_DIR oboe/Oboe.h
		HINTS ${PC_OBOE_INCLUDE_DIR})

find_library(OBOE_LIBRARY NAMES liboboe.a
		HINTS ${OBOE_LIBRARY_DIRS})

# Handle the QUIETLY and REQUIRED arguments and set OPENSL_FOUND to TRUE if
# all listed variables are TRUE.
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Oboe DEFAULT_MSG
		OBOE_INCLUDE_DIR OBOE_LIBRARY)

if(OBOE_FOUND)
	set(OBOE_LIBRARIES ${OBOE_LIBRARY})
else(OBOE_FOUND)
	if (Oboe_FIND_REQUIRED)
		message(FATAL_ERROR "Could NOT find OBOE")
	endif()
endif(OBOE_FOUND)

mark_as_advanced(OBOE_INCLUDE_DIR OBOE_LIBRARY)
