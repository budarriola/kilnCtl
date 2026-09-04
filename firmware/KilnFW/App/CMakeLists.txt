idf_component_register(SRCS "main.c"
                       "main_boot_early.c"
                       "main_control_bringup.c"
                       "main_network_http.c"
                       "main_bridges_bringup.c"
                       "monitor_task.c"
                       INCLUDE_DIRS "."
                       REQUIRES drivers mdns app_update espcoredump)
