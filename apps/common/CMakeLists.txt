# Small pieces the frontends share (not a module: no business logic, no tix/ header).
add_library(tix_apps_common STATIC src/editor.c)
target_include_directories(tix_apps_common PUBLIC src)
target_link_libraries(tix_apps_common PUBLIC tix::base)
tix_apply_warnings(tix_apps_common)
