find_package(miniRPC CONFIG REQUIRED)
find_package(Protobuf REQUIRED)

protobuf_generate_cpp(KV_RPC_PROTO_SRCS KV_RPC_PROTO_HDRS proto/kv_service.proto)
add_library(kv_rpc_proto STATIC ${KV_RPC_PROTO_SRCS} ${KV_RPC_PROTO_HDRS})
target_include_directories(kv_rpc_proto PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(kv_rpc_proto PUBLIC protobuf::libprotobuf)

add_library(kv_rpc src/client.cc src/service.cc)
add_library(kv::rpc ALIAS kv_rpc)
target_include_directories(kv_rpc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(kv_rpc PUBLIC kv_core minirpc::minirpc kv_rpc_proto)
target_compile_options(kv_rpc PRIVATE -Wall -Wextra -Wpedantic)

foreach(app kv_server kv_client)
    add_executable(${app} apps/${app}.cc)
    target_link_libraries(${app} PRIVATE kv_rpc)
    target_compile_options(${app} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()

if(BUILD_TESTING)
    add_executable(kv_rpc_integration_test test/integration_test.cc)
    target_link_libraries(kv_rpc_integration_test PRIVATE kv_rpc)
    target_compile_options(kv_rpc_integration_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME kv_rpc_integration_test COMMAND kv_rpc_integration_test)
    set_tests_properties(kv_rpc_integration_test PROPERTIES
        TIMEOUT 60 LABELS "rpc;integration" RESOURCE_LOCK kv_rpc_listener)
endif()
