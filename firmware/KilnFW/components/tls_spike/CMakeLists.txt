# WP7 TLS spike. SRCS is empty unless CONFIG_KILNCTL_TLS_SPIKE=y, so the default
# build links no new code. PRIV_REQUIRES must be unconditional: the early
# component-requirements pass runs before sdkconfig is visible, so a REQUIRES
# gated on CONFIG_* is silently empty there (found by the first spike build).
set(tls_spike_srcs "")
if(CONFIG_KILNCTL_TLS_SPIKE)
    set(tls_spike_srcs "tls_spike.c")
endif()
idf_component_register(SRCS ${tls_spike_srcs}
    PRIV_REQUIRES esp_http_client esp-tls esp_netif esp_system freertos heap mbedtls
    WHOLE_ARCHIVE)
