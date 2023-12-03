CC = clang
CFLAGS += -I. -Wall -Wextra -std=c17
LD_FLAGS += -fuse-ld=lld
SRCS = $(wildcard *.c)
OBJS = $(SRCS:.c=.o)
TARGET = server
TESTS = test_http_parse

all: release
debug:
	@CFLAGS="-g3" LD_FLAGS="-fsanitize=address" make build
release:
	@CFLAGS="-O3 -DNDEBUG -s" LD_FLAGS="-flto" make build
build: $(OBJS) $(TARGET)
server: server.o http.o
	$(CC) -o $@ $(LD_FLAGS) $^
test:
	@CFLAGS="-g3" LD_FLAGS="-fsanitize=address" make _real_test
_real_test: $(TESTS)
test_http_parse: test_http_parse.o http.o
	$(CC) -o $@ $(LD_FLAGS) $^
clean:
	rm -f $(OBJS)
distclean: clean
	rm -f $(TARGET) $(TESTS)
%.o: %.c
	$(CC) -c $(CFLAGS) $<
.SUFFIXES:
.PHONY: all debug release build clean distclean test _real_test