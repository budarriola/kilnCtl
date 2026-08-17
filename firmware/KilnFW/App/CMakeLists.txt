idf_component_register(SRCS "main.c"
                       "monitor_task.c"
                       INCLUDE_DIRS "."
                       REQUIRES drivers mdns app_update)
