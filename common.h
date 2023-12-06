#ifndef COMMON_H_
#define COMMON_H_
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>
struct buffer {
  void *buffer;      // the real buffer
  size_t capability; // total space allocated for the buffer
  size_t start;      // starting index of effective part (i.e. inclusive)
  size_t end;        // exclusive ending index of effective part
};
struct connection_information {
  enum connection_status {
    ConnectionStatusWaitingRequest,
    ConnectionStatusWritingResponse,
  } state;
  int file_descriptor;
  struct buffer buffer;
  struct http_request *request;
  struct http_response *response;

  // abstract recv/send functions unify plain TCP and TLS connections
  ssize_t (*recv)(struct connection_information *connection, void *buf, size_t nbytes);
  ssize_t (*send)(struct connection_information *connection, const void *buf, size_t n);
  // destructor of underlying structures
  void (*destroy_underlying)(struct connection_information *connection);

  // address information of peer
  struct sockaddr_storage address;

  // extra fields for underlying
  void *underlying;
};

// get the (IPv4/IPv6) address of the peer on connection supplied
//  the returned buffer is statically allocated and shall be overwritten with subsequent call to this function
const char *get_address(struct connection_information *connection);

// get the port number of the peer on connection supplied
//  the byte order is shifted properly
uint16_t get_port(struct connection_information *connection);

// logging utilities
enum logging_log_level {
  LOGGING_LOG_LEVEL_FULL, // keep this at top
  LOGGING_LOG_LEVEL_TRACE,
  LOGGING_LOG_LEVEL_DEBUG,
  LOGGING_LOG_LEVEL_INFORMATION, // this is the default level
  LOGGING_LOG_LEVEL_WARNING,
  LOGGING_LOG_LEVEL_ERROR,
  LOGGING_LOG_LEVEL_FATAL,
  LOGGING_LOG_LEVEL_OFF // and this at bottom
};

void logging_trace(const char *format, ...);
void logging_debug(const char *format, ...);
void logging_information(const char *format, ...);
void logging_warning(const char *format, ...);
void logging_error(const char *format, ...);
void logging_fatal(const char *format, ...);

void logging_set_level(enum logging_log_level level);
#endif