idf_component_register(SRCS "main.c"
                       "main_boot_early.c"
                       "main_control_bringup.c"
                       "main_network_http.c"
                       "main_bridges_bringup.c"
                       "monitor_task.c"
                       INCLUDE_DIRS "."
                       REQUIRES drivers mdns app_update espcoredump)

# Same hard-error policy as App/drivers/CMakeLists.txt -- see the long
# rationale there (a missing prototype wrote the Pico safety ceiling as 0 and
# only warned). PRIVATE: first-party sources only, never vendored components.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Werror=implicit-function-declaration
    -Werror=implicit-int
    -Werror=int-conversion
)
