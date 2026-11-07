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

# Static files (apps/web/assets) compiled in: no file system serving (section 13.6).
set(_assets ${CMAKE_CURRENT_SOURCE_DIR}/assets/tix.css ${CMAKE_CURRENT_SOURCE_DIR}/assets/app.js)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/assets_gen.c
  COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/assets_gen.c
    "-DINPUTS=tix_asset_css=${CMAKE_CURRENT_SOURCE_DIR}/assets/tix.css$<SEMICOLON>tix_asset_js=${CMAKE_CURRENT_SOURCE_DIR}/assets/app.js"
    -P ${PROJECT_SOURCE_DIR}/cmake/Embed.cmake
  DEPENDS ${_assets} ${PROJECT_SOURCE_DIR}/cmake/Embed.cmake VERBATIM)

# Pages, sessions and request checks: everything above HTTP, testable on store_mem.
add_library(tix_web_app STATIC ${CMAKE_CURRENT_BINARY_DIR}/assets_gen.c src/app.c src/html.c src/assets.c src/util.c src/gantt.c
  src/pages_tickets.c src/pages_plan.c src/pages_res.c src/forms.c src/actions.c src/api.c)
target_link_libraries(tix_web_app PUBLIC tix_web_request tix::core tix::input tix::export)
tix_apply_warnings(tix_web_app)

add_executable(tix-web src/main.c)
target_link_libraries(tix-web PRIVATE tix_web_app tix_web_http tix::store_libgit tix::index_sqlite)
tix_apply_warnings(tix-web)
# Next to `tix`, which execs it for `tix web` (same directory first, then PATH).
set_target_properties(tix-web PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/cli)
install(TARGETS tix-web RUNTIME DESTINATION bin)

add_executable(test_web_app tests/app.c)
target_link_libraries(test_web_app PRIVATE tix_web_app tix::store_mem tix::testkit)
tix_apply_warnings(test_web_app)
add_test(NAME web.app COMMAND test_web_app)
set_tests_properties(web.app PROPERTIES LABELS web TIMEOUT 60)

# End to end: the real tix-web in a throwaway git repository, plus an ASan soak.
add_executable(test_web_smoke tests/smoke.c)
target_link_libraries(test_web_smoke PRIVATE tix::testkit tix::base)
tix_apply_warnings(test_web_smoke)
add_test(NAME web.smoke COMMAND test_web_smoke $<TARGET_FILE:tix> $<TARGET_FILE:tix-web>)
set_tests_properties(web.smoke PROPERTIES LABELS web TIMEOUT 300)

# app.js in a real browser (headless Chrome via Playwright, run with uv). Skipped (77) when
# uv, Playwright or Chrome isn't there; label "browser" to run or exclude it on its own.
find_program(TIX_UV uv)
if(TIX_UV)
  add_test(NAME web.browser
    COMMAND ${TIX_UV} run --quiet --with playwright python ${CMAKE_CURRENT_SOURCE_DIR}/tests/browser.py
      $<TARGET_FILE:tix> $<TARGET_FILE:tix-web>)
  set_tests_properties(web.browser PROPERTIES LABELS "web;browser" TIMEOUT 300 SKIP_RETURN_CODE 77)
endif()
