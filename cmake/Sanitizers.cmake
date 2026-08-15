set(KV_SANITIZERS "" CACHE STRING
    "Comma-separated sanitizers to enable (address, undefined, thread)")

if(NOT KV_SANITIZERS STREQUAL "")
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang|GNU)$")
    message(FATAL_ERROR
            "KV_SANITIZERS requires a Clang, AppleClang, or GNU compiler")
  endif()

  string(REPLACE "," ";" _kv_sanitizers "${KV_SANITIZERS}")
  list(TRANSFORM _kv_sanitizers STRIP)
  list(REMOVE_DUPLICATES _kv_sanitizers)

  set(_kv_supported_sanitizers address undefined thread)
  foreach(_kv_sanitizer IN LISTS _kv_sanitizers)
    if(_kv_sanitizer STREQUAL "")
      message(FATAL_ERROR "KV_SANITIZERS contains an empty entry")
    endif()
    if(NOT _kv_sanitizer IN_LIST _kv_supported_sanitizers)
      message(FATAL_ERROR
              "Unsupported sanitizer '${_kv_sanitizer}'. "
              "Choose from: address, undefined, thread")
    endif()
  endforeach()

  if("address" IN_LIST _kv_sanitizers AND "thread" IN_LIST _kv_sanitizers)
    message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer cannot be combined")
  endif()

  list(JOIN _kv_sanitizers "," _kv_sanitizer_flags)
  message(STATUS "Enabled sanitizers: ${_kv_sanitizer_flags}")

  add_compile_options(
      "-fsanitize=${_kv_sanitizer_flags}"
      -fno-omit-frame-pointer
      -fno-sanitize-recover=all)
  add_link_options("-fsanitize=${_kv_sanitizer_flags}")
endif()
