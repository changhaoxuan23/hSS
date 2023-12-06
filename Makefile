CC = clang
CFLAGS += -I. -Wall -Wextra -std=c17
LD_FLAGS += -fuse-ld=lld -lgnutls
SRCS = $(wildcard *.c)
TARGET = server
TESTS = test_http_parse test_http_respond
TEST_SRCS = $(wildcard test_*.c)
TEST_OBJS = $(TEST_SRCS:.c=.o)
BUILD_SRCS = $(filter-out $(TEST_SRCS),$(SRCS))
OBJS = $(BUILD_SRCS:.c=.o)

all: release
debug:
	@CFLAGS="-g3 -DLOGGING_LOG_LEVEL=0" LD_FLAGS="-fsanitize=address" make build
release:
	@CFLAGS="-O3 -DNDEBUG" LD_FLAGS="-flto -s" make build
build: $(OBJS) $(TARGET)
server: server.o http.o http_hl.o common.o tcp_connection.o tls_connection.o
	$(CC) -o $@ $(LD_FLAGS) $^
#	sudo setcap cap_net_bind_service+ep $@
test:
	@CFLAGS="-g3" LD_FLAGS="-fsanitize=address" make _real_test
_real_test: $(TEST_OBJS) $(TESTS)
$(TESTS): http.o
	$(CC) -o $@ $(LD_FLAGS) $@.o $^
clean:
	rm -f $(OBJS)
distclean: clean
	rm -f $(TARGET) $(TESTS)
%.o: %.c
	$(CC) -c $(CFLAGS) $<
.SUFFIXES:
.PHONY: all debug release build clean distclean test _real_test