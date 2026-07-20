idf_component_register(
    SRCS
        "src/monocypher.c"
        "src/optional/monocypher-ed25519.c"
    INCLUDE_DIRS
        "src"
        "src/optional"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function)
