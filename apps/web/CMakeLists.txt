# tix-web: the web UI (docs/architecture.md section 13), a composition root like apps/cli.
# Mongoose is linked only into tix_web_http (http_mongoose.c); everything else sees
# src/web_http.h.

add_library(tix_web_request STATIC src/request.c)
target_include_directories(tix_web_request PUBLIC src)
target_link_libraries(tix_web_request PUBLIC tix::base)
tix_apply_warnings(tix_web_request)

add_library(tix_web_http STATIC src/http_mongoose.c)
target_link_libraries(tix_web_http PUBLIC tix_web_request PRIVATE tix::mongoose)
tix_apply_warnings(tix_web_http)

foreach(t request http)
  add_executable(test_web_${t} tests/${t}.c)
  target_link_libraries(test_web_${t} PRIVATE tix_web_http tix::testkit)
  tix_apply_warnings(test_web_${t})
  add_test(NAME web.${t} COMMAND test_web_${t})
  set_tests_properties(web.${t} PROPERTIES LABELS web TIMEOUT 60)
endforeach()
