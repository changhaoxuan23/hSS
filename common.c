#include <arpa/inet.h>
#include <assert.h>
#include <common.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

const char *get_address(struct connection_information *connection) {
  static char buffer[INET6_ADDRSTRLEN];
  void *target = NULL;
  if (connection->address.ss_family == AF_INET) {
    target = &((struct sockaddr_in *)&connection->address)->sin_addr;
  } else {
    target = &((struct sockaddr_in6 *)&connection->address)->sin6_addr;
  }
  inet_ntop(connection->address.ss_family, target, buffer, sizeof(buffer));
  return buffer;
}
uint16_t get_port(struct connection_information *connection) {
  uint16_t port = *(uint16_t *)(((void *)&connection->address) + sizeof(connection->address.ss_family));
  return ntohs(port);
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////

// logging
#define forward_log(level)                                                                                   \
  do {                                                                                                       \
    va_list args;                                                                                            \
    va_start(args, format);                                                                                  \
    logging_implementation(level, format, args);                                                             \
    va_end(args);                                                                                            \
  } while (false)

static enum logging_log_level *log_level(void) {
#ifdef LOGGING_LOG_LEVEL
  static enum logging_log_level log_level = LOGGING_LOG_LEVEL;
#else
  static enum logging_log_level log_level = LOGGING_LOG_LEVEL_INFORMATION;
#endif
  return &log_level;
}
static void logging_implementation(enum logging_log_level level, const char *format, va_list args) {
  // this implementation is not thread-safe, but this will not harm since we are single-threaded
  if (*log_level() > level) {
    return;
  }
  // keep this as the same length
  static char *name[] = {"PLACEHOLDER", "TRACE", "DEBUG", "INFOR", "WARNI", "ERROR", "FATAL"};
  static char *before_output[] = {
      "PLACEHOLDER", "\e[2m", "", "\e[36m", "\e[33m", "\e[35m", "\e[1;4;31m",
  };
  static char *after_output[] = {
      "PLACEHOLDER", "\e[0m", "\e[0m", "\e[0m", "\e[0m", "\e[0m", "\e[0m",
  };
  assert(sizeof(name) / sizeof(name[0]) == LOGGING_LOG_LEVEL_OFF);
  assert(sizeof(before_output) / sizeof(before_output[0]) == LOGGING_LOG_LEVEL_OFF);
  assert(sizeof(after_output) / sizeof(after_output[0]) == LOGGING_LOG_LEVEL_OFF);
  fprintf(stderr, "%s", before_output[level]);
  // add time and level
  fprintf(stderr, "[%04.06f][%s] ", (double)clock() / CLOCKS_PER_SEC, name[level]);
  vfprintf(stderr, format, args);
  fprintf(stderr, "%s", after_output[level]);
}
void logging_trace(const char *format, ...) { forward_log(LOGGING_LOG_LEVEL_TRACE); }
void logging_debug(const char *format, ...) { forward_log(LOGGING_LOG_LEVEL_DEBUG); }
void logging_information(const char *format, ...) { forward_log(LOGGING_LOG_LEVEL_INFORMATION); }
void logging_warning(const char *format, ...) { forward_log(LOGGING_LOG_LEVEL_WARNING); }
void logging_error(const char *format, ...) { forward_log(LOGGING_LOG_LEVEL_ERROR); }
void logging_fatal(const char *format, ...) { forward_log(LOGGING_LOG_LEVEL_FATAL); }