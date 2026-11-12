cmake_minimum_required(VERSION 3.16)

project(linuxcnc_ctrl LANGUAGES CXX)

include(CTest)

option(LINUXCNC_BUILD_TRANSPORT "Build reusable native TLS transport" OFF)
if(LINUXCNC_BUILD_TRANSPORT)
  add_subdirectory(native/transport)
endif()

option(LINUXCNC_GRPC_BUILD_SERVER "Build the native LinuxCNC gRPC server target" ON)
if(LINUXCNC_GRPC_BUILD_SERVER)
  add_subdirectory(native/server)
endif()
