# -*- mode: cmake -*-

#
# ATS
#       Root CMakeLists.txt file
#

# Require cmake 3.1.3 or higher for c++11 support
cmake_minimum_required(VERSION 3.1.3)

set(CMAKE_CXX_STANDARD 11)

# Enable testing, ctest needs this
# all add_test commands are ignored unless this is called!
enable_testing()

# Define the project name
# This command will define
#     ATS_SOURCE_DIR
#     ATS_BINARY_DIR
project(ATS)

set(ATS_MODULE_PATH "${ATS_SOURCE_DIR}/tools/cmake")
set(CMAKE_MODULE_PATH ${ATS_MODULE_PATH})

# --------------------------------------------------------------------------- #
# Search for Amanzi
#  Set Amanzi_DIR when invoking cmake
# --------------------------------------------------------------------------- #
find_package(Amanzi)

if ( Amanzi_FOUND )
  message(STATUS "Located Amanzi")
  message(STATUS "Amanzi_VERSION=${Amanzi_VERSION}")
  message(STATUS "Amanzi_DIR=${Amanzi_DIR}")
  message(STATUS "Amanzi_INCLUDE_DIR=${Amanzi_INCLUDE_DIR}")      
  message(STATUS "Amanzi_ENABLE_STRUCTURED=${Amanzi_ENABLE_STRUCTURED}")
  message(STATUS "Amanzi_ENABLE_UNSTRUCTURED=${Amanzi_ENABLE_UNSTRUCTURED}")
else()
  message(FATAL_ERROR "Failed to locate Amanzi")
endif()


# --------------------------------------------------------------------------- #
#  Define the compilers
# --------------------------------------------------------------------------- #
set(CMAKE_C_COMPILER ${Amanzi_C_COMPILER})
set(CMAKE_CXX_COMPILER ${Amanzi_CXX_COMPILER})
set(CMAKE_Fortran_COMPILER ${Amanzi_Fortran_COMPILER})
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${Amanzi_C_COMPILER_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${Amanzi_CXX_COMPILER_FLAGS}")
set(CMAKE_Fortran_FLAGS "${CMAKE_Fortran_FLAGS} ${Amanzi_Fortran_COMPILER_FLAGS}")

if (CMAKE_BUILD_TYPE MATCHES Debug)
  add_definitions(-DENABLE_DBC)
endif()

message(STATUS " --- CMAKE_C_COMPILER ${CMAKE_C_COMPILER} ")
message(STATUS " --- CMAKE_CXX_COMPILER ${CMAKE_CXX_COMPILER} ")
message(STATUS " --- CMAKE_Fortran_COMPILER ${CMAKE_Fortran_COMPILER} ")

if (Amanzi_DISABLE_MPI_CXX_BINDINGS)
  add_definitions("-DOMPI_SKIP_MPICXX -DMPICH_SKIP_MPICXX")
endif()

# Should pass this from Amanzi as well (but flags should be set already).
option(FORTRAN_SINGLE_UNDERSCORE "Use single-underscore in Fortran name-mangling" FALSE)
if (DISABLE_MPI_CXX_BINDINGS)
  set(FORTRAN_SINGLE_UNDERSCORE TRUE)
endif()

#find_package(MPI QUIET)

# --------------------------------------------------------------------------- #
#  Define the include paths
# --------------------------------------------------------------------------- #

# Amanzi install include path
include_directories(${Amanzi_INCLUDE_DIR})

# Amanzi TPLs
# The pattern for each variable is
# Amanzi_TPL_<tpl name>_<variable>
# Example: For the Teuchos package in Trilinos
# Amanzi_TPL_Teuchos_DIR          => Location of the TeuchosConfig.cmake file
# Amanzi_TPL_Teuchos_INCLUDE_DIR  => Include path for the Teuchos header files
# Amanzi_TPL_Teuchos_INCLUDE_DIRS => Include paths for Teuchos AND TPLs that Teuchos depends on
# Amanzi_TPL_Teuchos_LIBRARY_DIRS => Library paths for Teuchos library and TPL libraries Teuchos calls
# See AmanziConfigTPL.cmake for the entire list

include_directories(${Amanzi_TPL_Teuchos_INCLUDE_DIRS})
include_directories(${Amanzi_TPL_Epetra_INCLUDE_DIRS})
include_directories(${Amanzi_TPL_ASCEMIO_INCLUDE_DIRS})

# Each TPL is flagged as enabled ON(TRUE)|OFF(FALSE)

if ( Amanzi_TPL_CGNS_ENABLED )
  include_directories(${Amanzi_TPL_CGNS_INCLUDE_DIRS})
  add_definitions(-DENABLE_CGNS)
endif()

if ( Amanzi_TPL_STK_ENABLED )
  include_directories(${Amanzi_TPL_STK_INCLUDE_DIRS})
  add_definitions(-DENABLE_STK_Mesh)
endif()

# enable structured/unstructured
if ( Amanzi_ENABLE_UNSTRUCTURED )
  add_definitions(-DENABLE_Unstructured)
endif()

if ( Amanzi_ENABLE_STRUCTURED )
  add_definitions(-DENABLE_Structured)
endif()

#silo
if ( Amanzi_TPL_Silo_ENABLED )
  set(ENABLE_Silo ON)
  add_definitions(-DENABLE_Silo)
endif()

if (ENABLE_ALQUIMIA)
    # Use Alquimia as a chemistry engine.
  add_definitions(-DALQUIMIA_ENABLED)
  #add_subdirectory(common/alquimia)
endif()

message(STATUS "Silo Enabled?: ${Amanzi_TPL_Silo_ENABLED}")
message(STATUS "Silo Enabled?: ${ENABLE_Silo}")

# We make every effort in the Amanzi build system to include TPL libraries 
# in the target definitions. Avoid using Amanzi_TPL_*_LIBRARY or
# Amanzi_TPL_*_LIBRARIES variables. Using these variables when it
# is not neccessary may result in a command line length that exceeds
# the OS limits and will prevent the host code from compiling.
# Unfortunately Trilinos does not provide exported targets
# The host code must explicitly link against the Trilinos library
# path to link against the Trilinos libraries.
link_directories(${Amanzi_TPL_Trilinos_LIBRARY_DIRS})


# add the macros that help with registration of factories
include(${Amanzi_LIBRARY_DIR}/RegisterEvaluators.cmake)




# --------------------------------------------------------------------------- #
#  Building against Amanzi
# --------------------------------------------------------------------------- #

# Source files for all binaries and libraries found under src
add_subdirectory(src)



