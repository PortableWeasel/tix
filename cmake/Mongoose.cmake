# Cesanta Mongoose (GPL-2.0-only), vendored at third_party/mongoose, pinned at 7.23.
# docs/architecture.md section 13.1. Only apps/web/src/http_mongoose.c may include
# mongoose.h: the header is on no other target's include path, and the check below
# fails the configure step if any other tix source names it.

set(TIX_MONGOOSE_SRC ${PROJECT_SOURCE_DIR}/third_party/mongoose)
if(NOT EXISTS ${TIX_MONGOOSE_SRC}/mongoose.c)
  message(FATAL_ERROR "third_party/mongoose missing: run `git submodule update --init --depth 1`")
endif()

add_library(tix_mongoose STATIC ${TIX_MONGOOSE_SRC}/mongoose.c)
add_library(tix::mongoose ALIAS tix_mongoose)
target_include_directories(tix_mongoose SYSTEM PUBLIC ${TIX_MONGOOSE_SRC})
# OS sockets with poll(); no TLS (loopback, or TLS at a reverse proxy), no built-in
# TCP/IP stack, no logging, directory listing or SSI. Most of Mongoose's recent CVEs
# are in code these switches leave out.
target_compile_definitions(tix_mongoose PUBLIC
  MG_ARCH=MG_ARCH_UNIX MG_TLS=MG_TLS_NONE MG_ENABLE_TCPIP=0 MG_ENABLE_POLL=1
  MG_ENABLE_IPV6=1 MG_ENABLE_LOG=0 MG_ENABLE_DIRLIST=0 MG_ENABLE_SSI=0
  MG_MAX_RECV_SIZE=262144 MG_MAX_HTTP_HEADERS=30)
target_compile_definitions(tix_mongoose PRIVATE _DEFAULT_SOURCE _POSIX_C_SOURCE=200809L)
# Third-party code: our warning set is not imposed (a bump must not break the build),
# but the sanitizers are.
target_compile_options(tix_mongoose PRIVATE -w)
if(TIX_SANITIZE)
  target_compile_options(tix_mongoose PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

function(tix_check_mongoose_quarantine)
  file(GLOB_RECURSE srcs ${PROJECT_SOURCE_DIR}/modules/*.[ch] ${PROJECT_SOURCE_DIR}/apps/*.[ch])
  foreach(f IN LISTS srcs)
    file(STRINGS "${f}" hits REGEX "^[ \t]*#[ \t]*include[ \t]*[\"<]mongoose\\.h")
    if(hits AND NOT f STREQUAL "${PROJECT_SOURCE_DIR}/apps/web/src/http_mongoose.c")
      file(RELATIVE_PATH rel ${PROJECT_SOURCE_DIR} ${f})
      message(FATAL_ERROR "${rel} includes mongoose.h; only apps/web/src/http_mongoose.c may")
    endif()
  endforeach()
endfunction()
tix_check_mongoose_quarantine()
