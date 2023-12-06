#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <common.h>
#include <errno.h>
#include <fcntl.h>
#include <http.h>
#include <http_hl.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <tcp_connection.h>
#include <tls_connection.h>
#include <unistd.h>

// constants
enum {
  AuthorizationCodeLength = 32,
  HTTPPort = 8080,
  HTTPSPort = 8843,
};

static bool *get_running(void) {
  static bool running = true;
  return &running;
}

static const char *get_authorization_code() {
  static char map[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F',
                       'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
                       'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V',
                       'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '~', '@'};
  static bool initialize = true;
  static char code[AuthorizationCodeLength];
  if (initialize) {
    assert(AuthorizationCodeLength % 32 == 0);
    initialize = false;
    int buffer_length = AuthorizationCodeLength / 32 * 3;
    uint64_t buffer[buffer_length];
    getrandom(buffer, sizeof(buffer), 0);
    char *target = code;
    for (int i = 0; i < buffer_length; i++) {
      for (int j = 0; j < 10; j++) {
        *target++ = map[buffer[i] & 0x3f];
        buffer[i] >>= 6;
      }
    }
    for (int i = 0; i < buffer_length; i += 3) {
      *target++ = map[buffer[i] | ((buffer[i + 1] & 0xc) << 2)];
      *target++ = map[buffer[i + 2] | ((buffer[i + 1] & 0x3) << 4)];
    }
    assert(target - code == AuthorizationCodeLength);
    char output_buffer[sizeof(code) + 1];
    memcpy(output_buffer, code, sizeof(code));
    output_buffer[sizeof(code)] = '\0';
    logging_information("authorization_code: %s\n", output_buffer);
  }
  return code;
}

static const char *current_working_directory(void) {
  static bool initialize = true;
  static char current_directory[1028];
  if (initialize) {
    if (getcwd(current_directory + 4, 1024) == NULL) {
      fprintf(stderr, "cannot get current working directory\n");
      exit(EXIT_FAILURE);
    }
    *((uint32_t *)current_directory) = strlen(current_directory + 4);
    // ensure this path ends with a '/' therefore avoid potential bypass to the filesystem tree restriction
    //  when we are at
    //   /foo/bar
    //  while an attempt is made to access
    //   /foo/bar[^/]+/*
    if (current_directory[4 + *((uint32_t *)current_directory) - 1] != '/') {
      current_directory[4 + *((uint32_t *)current_directory)] = '/';
      current_directory[4 + *((uint32_t *)current_directory) + 1] = '\0';
      *((uint32_t *)current_directory) += 1;
    }
    initialize = false;
  }
  return current_directory;
}

struct file_descriptor_information {
  enum file_descriptor_type {
    LISTEN_SOCKET, // socket that represent a listening point
    TCP_SOCKET,    // socket that represent a plain TCP connection
    TLS_SOCKET     // yes, TLS is on TCP, but we use this term in contrast to plain TCP here
  } type;
  int file_descriptor;
  struct file_descriptor_information *next;
  struct file_descriptor_information **prev;
  struct connection_information *connection;
};

void initialize_connection_information(struct file_descriptor_information *information) {
  information->connection = malloc(sizeof(struct connection_information));
  struct connection_information *connection = information->connection;
  connection->buffer.buffer = NULL;
  connection->buffer.capability = 0;
  connection->buffer.start = 0;
  connection->buffer.end = 0;
  connection->state = ConnectionStatusWaitingRequest;
  connection->file_descriptor = information->file_descriptor;
  connection->request = malloc(http_request_size);
  http_request_initialize(connection->request);
  connection->response = malloc(http_response_size);
  http_response_initialize(connection->response);
  connection->underlying = NULL;

  // get remote address and save into context
  socklen_t length = sizeof(connection->address);
  getpeername(connection->file_descriptor, (struct sockaddr *)&connection->address, &length);

  // initialize underlying structure
  if (information->type == TCP_SOCKET) {
    tcp_initialize_underlying(connection);
    logging_trace("TCP connection established with %s:%hu\n", get_address(connection), get_port(connection));
  } else {
    assert(information->type == TLS_SOCKET);
    tls_initialize_underlying(connection);
    logging_trace("TLS connection initialized with %s:%hu\n", get_address(connection), get_port(connection));
  }

  // initialize the abstract recv/send functions
}
void destroy_connection_information(struct connection_information *information) {
  free(information->buffer.buffer);
  http_request_destroy(information->request);
  http_response_destroy(information->response);
  free(information->request);
  free(information->response);
  // free underlying
  information->destroy_underlying(information);
}

struct file_descriptor_information **get_file_descriptor_list(void) {
  static struct file_descriptor_information *head = NULL;
  return &head;
}
struct file_descriptor_information *
register_file_descriptor(int file_descriptor, enum file_descriptor_type type) {
  size_t allocate_size = sizeof(struct file_descriptor_information);
  if (type != LISTEN_SOCKET) {
    allocate_size += sizeof(struct connection_information);
  }
  struct file_descriptor_information *information = malloc(allocate_size);
  information->file_descriptor = file_descriptor;
  information->type = type;
  if (type != LISTEN_SOCKET) {
    initialize_connection_information(information);
  }
  struct file_descriptor_information **head = get_file_descriptor_list();
  information->next = *head;
  information->prev = head;
  *head = information;
  return information;
}
void destroy_file_information(struct file_descriptor_information *information) {
  close(information->file_descriptor);
  *information->prev = information->next;
  if (information->type != LISTEN_SOCKET) {
    destroy_connection_information(information->connection);
    free(information->connection);
  }
  free(information);
}
void close_all_file_descriptors(void) {
  while (true) {
    struct file_descriptor_information **head = get_file_descriptor_list();
    if (*head == NULL) {
      break;
    }
    destroy_file_information(*head);
  }
}

void listen_address(int epoll_file_descriptor, const struct addrinfo *address) {
  static const int yes = 1;
  int socket_file_descriptor = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
  setsockopt(socket_file_descriptor, SOL_SOCKET, SO_REUSEADDR, (void *)&yes, sizeof(int));
  if (socket_file_descriptor == -1) {
    return;
  }
  int result = 0;
  result |= bind(socket_file_descriptor, address->ai_addr, address->ai_addrlen);
  result |= listen(socket_file_descriptor, SOMAXCONN);
  if (result != 0) {
    close(socket_file_descriptor);
    return;
  }
  struct file_descriptor_information *information =
      register_file_descriptor(socket_file_descriptor, LISTEN_SOCKET);
  struct epoll_event event = {.events = EPOLLIN, .data.ptr = information};
  epoll_ctl(epoll_file_descriptor, EPOLL_CTL_ADD, socket_file_descriptor, &event);
}
void accept_connection(int epoll_file_descriptor, int listen_file_descriptor) {
  int connection_socket = accept(listen_file_descriptor, NULL, NULL);
  if (connection_socket == -1) {
    return;
  }
  // set the socket as non-blocking
  int flags = fcntl(connection_socket, F_GETFL);
  flags |= O_NONBLOCK;
  fcntl(connection_socket, F_SETFL, flags);
  // decide from which port are we receiving such connection
  struct sockaddr_storage address;
  socklen_t length = sizeof(address);
  getsockname(connection_socket, (struct sockaddr *)&address, &length);
  if (address.ss_family != AF_INET && address.ss_family != AF_INET6) {
    // unrecognized address, where did it come from?
    close(connection_socket);
    return;
  }
  uint16_t port = *(uint16_t *)(((void *)&address) + sizeof(address.ss_family));
  // shift the byte order
  port = ntohs(port);
  if (port != HTTPPort && port != HTTPSPort) {
    // unrecognized port number, where on earth did it come from?
    close(connection_socket);
    return;
  }
  enum file_descriptor_type type = port == HTTPPort ? TCP_SOCKET : TLS_SOCKET;
  struct file_descriptor_information *information = register_file_descriptor(connection_socket, type);
  struct epoll_event event = {.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET, .data.ptr = information};
  epoll_ctl(epoll_file_descriptor, EPOLL_CTL_ADD, connection_socket, &event);
}
// get value of a header, NULL if which does not exist; the returned buffer must be freed after use
char *get_request_header(struct http_request *request, const char *name) {
  size_t length;
  if (http_request_get_header(request, name, NULL, &length) != HTTP_ERROR_CODE_SUCCEED) {
    return NULL;
  }
  char *buffer = malloc(length);
  http_request_get_header(request, name, buffer, &length);
  return buffer;
}
void generate_forbidden(struct connection_information *information) {
  http_response_set_code(information->response, HTTP_RESPONSE_CODE_FORBIDDEN, NULL);
}
void generate_not_found(struct connection_information *information) {
  http_response_set_code(information->response, HTTP_RESPONSE_CODE_NOT_FOUND, NULL);
}
// the request is now ready and accessible from the supplied structure, generate response accordingly
void handle_http_transaction(struct file_descriptor_information *information) {
  struct connection_information *connection = information->connection;
  char *canonicalized_url = NULL;
  char *range_value = NULL;
  size_t url_length;
  // get the url of request
  http_request_get_url(connection->request, NULL, &url_length);
  char *url = malloc(url_length);
  http_request_get_url(connection->request, url, &url_length);

  // handle magic calls
  if (strncmp(url, "/magic-call/", 12) == 0) {
    char *code = get_request_header(connection->request, "Authorization");
    bool forbidden = false;
    if (code == NULL) {
      generate_not_found(connection);
      forbidden = true;
    } else if (memcmp(code, get_authorization_code(), AuthorizationCodeLength) != 0) {
      generate_forbidden(connection);
      forbidden = true;
    }
    free(code);
    code = NULL;
    if (forbidden) {
      goto cleanup;
    }
    if (strcmp(url + 12, "shutdown") == 0) {
      http_response_set_code(connection->response, HTTP_RESPONSE_CODE_NO_CONTENT, NULL);
      *get_running() = false;
    } else {
      http_response_set_code(connection->response, HTTP_RESPONSE_CODE_NOT_IMPLEMENTED, NULL);
    }
    goto cleanup;
  }

  // handle common requests
  // make sure that the url never goes beyond current root
  assert(*url == '/');
  canonicalized_url = realpath(url + 1, NULL);
  if (canonicalized_url == NULL) {
    generate_not_found(connection);
    goto cleanup;
  }
  const char *cwd = current_working_directory();
  if (memcmp(cwd + 4, canonicalized_url, *(uint32_t *)cwd) != 0) {
    generate_not_found(connection);
    goto cleanup;
  }

  // we do not check if the file exist if we are on TCP session: redirect directly to TLS address
  if (information->type == TCP_SOCKET) {
    http_response_set_code(connection->response, HTTP_RESPONSE_CODE_MOVE_MOVED_PERMANENTLY, NULL);
    struct sockaddr_storage address;
    socklen_t length = sizeof(address);
    getsockname(information->file_descriptor, (struct sockaddr *)&address, &length);
    // length = 8(https://) + INET6_ADDRSTRLEN(address) + 6(:PORT) + url_length + 1(\0)
    char buffer[INET6_ADDRSTRLEN + url_length + 15];
    char address_buffer[INET6_ADDRSTRLEN];
    void *target = NULL;
    if (address.ss_family == AF_INET) {
      target = &((struct sockaddr_in *)&address)->sin_addr;
    } else {
      target = &((struct sockaddr_in6 *)&address)->sin6_addr;
    }
    inet_ntop(address.ss_family, target, address_buffer, INET6_ADDRSTRLEN);
    sprintf(buffer, "https://%s:%hu%s", address_buffer, HTTPSPort, url);
    http_response_set_header(connection->response, "Location", buffer);
    goto cleanup;
  }

  // try to open the file
  int file = open(canonicalized_url, O_RDONLY);
  if (file == -1) {
    generate_not_found(connection);
    goto cleanup;
  }

  // get length of the file
  struct stat status;
  fstat(file, &status);

  // calculate Range information
  range_value = get_request_header(connection->request, "Range");
  struct range range = parse_range(range_value, status.st_size);
  // the only case that range.start == range.end is that both of which is 0, which indicates a full range
  size_t real_length = range.start == range.end ? status.st_size : range.end - range.start;

  // map the file
  //  since offset must be a page-aligned value, we cannot map only the range requested but the full file
  void *content = mmap(NULL, status.st_size, PROT_READ, MAP_PRIVATE, file, 0);
  close(file);
  http_response_set_body(connection->response, content + range.start, &real_length);
  munmap(content, status.st_size);
  if (range.start != range.end) {
    char buffer[100];
    sprintf(buffer, "bytes %lu-%lu/%lu", range.start, range.end - 1, status.st_size);
    http_response_set_code(connection->response, HTTP_RESPONSE_CODE_PARTIAL_CONTENT, NULL);
    http_response_set_header(connection->response, "Content-Range", buffer);
  } else {
    http_response_set_code(connection->response, HTTP_RESPONSE_CODE_OK, NULL);
  }

cleanup:
  // free all
  free(url);
  free(canonicalized_url);
  free(range_value);
}

void handle_connection(uint32_t event, struct file_descriptor_information *information) {
  if ((event & EPOLLIN) == 0 && information->connection->state == ConnectionStatusWaitingRequest) {
    return;
  }
  if ((event & EPOLLOUT) == 0 && information->connection->state == ConnectionStatusWritingResponse) {
    return;
  }
  if (information->connection->state == ConnectionStatusWaitingRequest) {
    const size_t buffer_page_size = 4096;
    void *buffer_list[128];
    size_t total_size = 0;
    size_t buffer_page = 0;
    size_t offset = 0;
    buffer_list[buffer_page] = malloc(buffer_page_size);
    while (true) {
      ssize_t size = information->connection->recv(
          information->connection, buffer_list[buffer_page] + offset, buffer_page_size - offset
      );
      if (size == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          int error = errno;
          logging_error(
              "receiving from %s:%hu returned errno %d(%s)\n", get_address(information->connection),
              get_port(information->connection), error, strerror(error)
          );
          destroy_file_information(information);
          information = NULL;
          break;
        } else {
          // break directly since we cannot get any further data for now
          break;
        }
      } else if (size == 0) {
        // this identifies EOF from peer, a (half-)closed TCP connection
        //  if no data received in this round, we shall close the connection
        if (total_size == 0) {
          destroy_file_information(information);
          information = NULL;
          break;
        }
      }
      total_size += size;
      if ((size_t)size < buffer_page_size - offset) {
        offset += size;
      } else {
        offset = 0;
        buffer_page++;
        buffer_list[buffer_page] = malloc(buffer_page_size);
      }
    }
    if (information != NULL && total_size != 0) {
      // shift effective part to beginning
      if (information->connection->buffer.start != 0) {
        memmove(
            information->connection->buffer.buffer,
            information->connection->buffer.buffer + information->connection->buffer.start,
            information->connection->buffer.end - information->connection->buffer.start
        );
        information->connection->buffer.end -= information->connection->buffer.start;
        information->connection->buffer.start = 0;
      }
      // allocate enough space for newly read data
      size_t available = information->connection->buffer.capability - information->connection->buffer.end;
      size_t extra = total_size - available;
      void *new_buffer =
          realloc(information->connection->buffer.buffer, information->connection->buffer.capability + extra);
      if (new_buffer == NULL) {
        destroy_file_information(information);
        information = NULL;
      } else {
        // update capability
        information->connection->buffer.capability += extra;
        information->connection->buffer.buffer = new_buffer;
        // move data
        for (size_t i = 0; i < buffer_page; i++) {
          // whole page
          memcpy(
              information->connection->buffer.buffer + information->connection->buffer.end, buffer_list[i],
              buffer_page_size
          );
          information->connection->buffer.end += buffer_page_size;
        }
        // last page, possibly partially filled
        if (offset > 0) {
          memcpy(
              information->connection->buffer.buffer + information->connection->buffer.end,
              buffer_list[buffer_page], offset
          );
          information->connection->buffer.end += offset;
        }
      }
    }
    // either succeed or not, clean the allocated memory up
    for (size_t i = 0; i <= buffer_page; i++) {
      free(buffer_list[i]);
    }
    // we can stop here if failed
    if (information == NULL || total_size == 0) {
      return;
    }
    // try to parse it
    int return_value;
    return_value = http_request_from_buffer(
        information->connection->request,
        information->connection->buffer.buffer + information->connection->buffer.start,
        information->connection->buffer.end - information->connection->buffer.start
    );
    if (return_value == HTTP_ERROR_CODE_INCOMPLETE_REQUEST) {
      // we shall wait for further data
      return;
    } else if (return_value != HTTP_ERROR_CODE_SUCCEED) {
      // we shall return a BAD REQUEST for this
      http_response_set_code(information->connection->response, HTTP_RESPONSE_CODE_BAD_REQUEST, NULL);
    } else {
      handle_http_transaction(information);
    }
    // free request since no which is no longer used
    http_request_destroy(information->connection->request);
    // set common headers
    http_response_set_header(information->connection->response, "Server", "hSS/0.0.1-alpha");
    // render the response for sending
    size_t size = information->connection->buffer.capability;
    if (http_response_render(
            information->connection->response, information->connection->buffer.buffer, &size
        ) == HTTP_ERROR_CODE_INSUFFICIENT_BUFFER_SIZE) {
      // avoid copying unused data
      free(information->connection->buffer.buffer);
      information->connection->buffer.buffer = malloc(size);
      information->connection->buffer.capability = size;
      http_response_render(information->connection->response, information->connection->buffer.buffer, &size);
    }
    information->connection->buffer.start = 0;
    information->connection->buffer.end = size;
    // mark for sending
    information->connection->state = ConnectionStatusWritingResponse;
  }
  if (information->connection->state == ConnectionStatusWritingResponse) {
    while (true) {
      size_t to_be_write = information->connection->buffer.end - information->connection->buffer.start;
      ssize_t size = information->connection->send(
          information->connection,
          information->connection->buffer.buffer + information->connection->buffer.start, to_be_write
      );
      if (size == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          destroy_file_information(information);
          return;
        } else {
          return;
        }
      }
      information->connection->buffer.start += size;
      if ((size_t)size == to_be_write) {
        information->connection->state = ConnectionStatusWaitingRequest;
        return;
      }
    }
  }
}
void listen_addresses(int epoll_file_descriptor, int port) {
  struct addrinfo hints;
  struct addrinfo *result;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  char service_buffer[10];
  sprintf(service_buffer, "%d", port);
  int return_value = getaddrinfo(NULL, service_buffer, &hints, &result);
  if (return_value != 0) {
    logging_error("failed to build address on passive port %d: %s\n", port, gai_strerror(return_value));
    return;
  }
  for (struct addrinfo *target = result; target != NULL; target = target->ai_next) {
    listen_address(epoll_file_descriptor, target);
  }
  freeaddrinfo(result);
}
int main() {
  // generate and print authorization code
  get_authorization_code();
  // create epoll handle
  int epoll_file_descriptor = epoll_create1(EPOLL_CLOEXEC);
  // listen HTTP port
  listen_addresses(epoll_file_descriptor, HTTPPort);
  // listen HTTPS port
  listen_addresses(epoll_file_descriptor, HTTPSPort);

  while (*get_running()) {
    struct epoll_event events[128];
    int event_count = epoll_wait(epoll_file_descriptor, events, 128, -1);
    for (int i = 0; i < event_count; i++) {
      struct file_descriptor_information *information = events[i].data.ptr;
      if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
        // error occurred or the connection is closed, free this connection
        destroy_file_information(information);
        if (*get_file_descriptor_list() == NULL) {
          // all connections are gone, exit
          *get_running() = false;
          break;
        }
        continue;
      }
      if (information->type == LISTEN_SOCKET) {
        assert(events[i].events & EPOLLIN);
        accept_connection(epoll_file_descriptor, information->file_descriptor);
      } else {
        handle_connection(events[i].events, information);
      }
    }
  }
  close_all_file_descriptors();
  close(epoll_file_descriptor);
  return 0;
}