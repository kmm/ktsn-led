# Portable core: no ESP-IDF dependencies, also built by host/Makefile for unit tests.
idf_component_register(SRCS "src/arena.c" "src/compile.c" "src/natives.c" "src/channel.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src")
# The evaluator is the hot path of every frame (the rest of the firmware stays at the project's optimisation level).
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
