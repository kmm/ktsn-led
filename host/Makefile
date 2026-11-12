# Host-side unit tests for the portable core (no ESP-IDF needed).
CC ?= gcc
SRCS = ../main/ws_parallel.c ../main/gamma.c ../main/wire.c ../main/astro.c ../components/dsl/src/arena.c ../components/dsl/src/compile.c ../components/dsl/src/natives.c ../components/dsl/src/channel.c
CFLAGS = -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I../components/dsl/include -I../components/dsl/src -I../main
test: test_dsl.c $(SRCS)
	$(CC) $(CFLAGS) $^ -lm -o test_dsl && ./test_dsl
clean:
	rm -f test_dsl
