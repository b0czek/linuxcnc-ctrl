cmake_minimum_required(VERSION 3.16)
project(linuxcnc_transport LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
find_package(OpenSSL 3.0 REQUIRED)
find_package(Threads REQUIRED)
option(LINUXCNC_TRANSPORT_BUILD_GRPC "Build gRPC server credentials" ON)
option(LINUXCNC_TRANSPORT_BUILD_TESTS "Build native TLS transport tests" ${BUILD_TESTING})

add_library(linuxcnc-transport STATIC
    src/external_key.cc src/signer_protocol.cc src/signer_client.cc
    src/credentials.cc)
add_library(linuxcnc::transport ALIAS linuxcnc-transport)
set_target_properties(linuxcnc-transport PROPERTIES
    POSITION_INDEPENDENT_CODE ON EXPORT_NAME transport)
target_compile_features(linuxcnc-transport PUBLIC cxx_std_17)
target_include_directories(linuxcnc-transport PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(linuxcnc-transport PUBLIC OpenSSL::SSL OpenSSL::Crypto)
target_compile_options(linuxcnc-transport PRIVATE -Wall -Wextra -Wpedantic)

add_library(linuxcnc-external-key MODULE src/provider.cc)
set_target_properties(linuxcnc-external-key PROPERTIES
    PREFIX "" EXPORT_NAME external-key)
target_link_libraries(linuxcnc-external-key PRIVATE linuxcnc-transport)
target_compile_options(linuxcnc-external-key PRIVATE -Wall -Wextra -Wpedantic)

if(LINUXCNC_TRANSPORT_BUILD_GRPC)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(TRANSPORT_GRPC REQUIRED IMPORTED_TARGET grpc++)
  add_library(linuxcnc-transport-grpc STATIC src/grpc_credentials.cc)
  add_library(linuxcnc::transport-grpc ALIAS linuxcnc-transport-grpc)
  set_target_properties(linuxcnc-transport-grpc PROPERTIES EXPORT_NAME transport-grpc)
  target_link_libraries(linuxcnc-transport-grpc PUBLIC
      linuxcnc-transport PkgConfig::TRANSPORT_GRPC Threads::Threads)
  # gRPC 1.80 replaced the static certificate API; retain the qualified host
  # 1.51 API while using the new checked construction on the target.
  if(TRANSPORT_GRPC_VERSION VERSION_GREATER_EQUAL "1.80")
    target_compile_definitions(linuxcnc-transport-grpc PRIVATE
        LINUXCNC_TRANSPORT_GRPC_MODERN=1)
  else()
    target_compile_definitions(linuxcnc-transport-grpc PRIVATE
        LINUXCNC_TRANSPORT_GRPC_MODERN=0)
  endif()
  target_compile_options(linuxcnc-transport-grpc PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(LINUXCNC_TRANSPORT_BUILD_TESTS)
  add_executable(transport-contract-tests tests/contracts.cc)
  target_link_libraries(transport-contract-tests PRIVATE
      linuxcnc-transport Threads::Threads)
  add_test(NAME transport-contracts COMMAND transport-contract-tests)
  set_tests_properties(transport-contracts PROPERTIES TIMEOUT 30)
  if(LINUXCNC_TRANSPORT_BUILD_GRPC)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(transport-handshake-server tests/handshake_server.cc)
    target_link_libraries(transport-handshake-server PRIVATE linuxcnc-transport-grpc)
    add_test(NAME transport-handshakes COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/handshakes.py
        --server $<TARGET_FILE:transport-handshake-server>
        --provider $<TARGET_FILE:linuxcnc-external-key>
        --grpc-version ${TRANSPORT_GRPC_VERSION}
        --runner ${CMAKE_CROSSCOMPILING_EMULATOR})
    set_tests_properties(transport-handshakes PROPERTIES TIMEOUT 60)
  endif()
endif()

install(TARGETS linuxcnc-transport linuxcnc-external-key
    EXPORT linuxcnc-transport-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/linuxcnc/transport)
if(TARGET linuxcnc-transport-grpc)
  install(TARGETS linuxcnc-transport-grpc EXPORT linuxcnc-transport-targets
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
install(DIRECTORY include/linuxcnc_transport DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
set(TRANSPORT_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/linuxcnc-transport")
configure_package_config_file(cmake/linuxcnc-transport-config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/linuxcnc-transport-config.cmake"
    INSTALL_DESTINATION "${TRANSPORT_CMAKE_DIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/linuxcnc-transport-config.cmake"
    DESTINATION "${TRANSPORT_CMAKE_DIR}")
install(EXPORT linuxcnc-transport-targets NAMESPACE linuxcnc::
    DESTINATION "${TRANSPORT_CMAKE_DIR}")
