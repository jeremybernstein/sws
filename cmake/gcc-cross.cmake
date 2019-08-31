if(NOT DEFINED ENV{PLATFORM})
  message(FATAL_ERROR "The PLATFORM environment variable is not set.")
endif()

if(NOT DEFINED ENV{TOOLCHAIN_PREFIX})
  message(FATAL_ERROR "The TOOLCHAIN_PREFIX environment variable is not set.")
endif()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR $ENV{PLATFORM})

set(CMAKE_C_COMPILER $ENV{TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER $ENV{TOOLCHAIN_PREFIX}-g++)

set(CMAKE_FIND_ROOT_PATH /usr/$ENV{TOOLCHAIN_PREFIX})

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)