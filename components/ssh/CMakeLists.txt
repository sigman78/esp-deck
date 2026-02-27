idf_component_register(
    SRCS
        "ssh_client.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        mbedtls
        lwip
        terminal
        libssh2_esp
)

# TODO: Add libssh when ported
# PRIV_REQUIRES libssh
