#define _GNU_SOURCE
#include "http.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

struct connection_information {};
struct file_descriptor_information {
  enum file_descriptor_type { LISTEN_SOCKET, CONNECTION_SOCKET } type;
  int file_descriptor;
  struct file_descriptor_information *next;
  struct file_descriptor_information **prev;
  struct connection_information connection[];
};

struct file_descriptor_information **get_file_descriptor_list(void) {
  static struct file_descriptor_information *head = NULL;
  return &head;
}
struct file_descriptor_information *register_file_descriptor(int file_descriptor,
                                                             enum file_descriptor_type type) {
  size_t allocate_size = sizeof(struct file_descriptor_information);
  if (type == CONNECTION_SOCKET) {
    allocate_size += sizeof(struct connection_information);
  }
  struct file_descriptor_information *information = malloc(allocate_size);
  information->file_descriptor = file_descriptor;
  information->type = type;
  struct file_descriptor_information **head = get_file_descriptor_list();
  information->next = *head;
  information->prev = head;
  *head = information;
  return information;
}
void destroy_file_information(struct file_descriptor_information *information) {
  close(information->file_descriptor);
  *information->prev = information->next;
  free(information);
}

void listen_address(int epoll_file_descriptor, const struct addrinfo *address) {
  int socket_file_descriptor = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
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
  struct file_descriptor_information *information =
      register_file_descriptor(connection_socket, CONNECTION_SOCKET);
  struct epoll_event event = {.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET, .data.ptr = information};
  epoll_ctl(epoll_file_descriptor, EPOLL_CTL_ADD, connection_socket, &event);
}
void handle_connection() {}
int main() {
  struct addrinfo hints;
  struct addrinfo *result;
  int return_value;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  return_value = getaddrinfo(NULL, "http", &hints, &result);
  assert(return_value == 0);
  int epoll_file_descriptor = epoll_create1(EPOLL_CLOEXEC);
  for (struct addrinfo *target = result; target != NULL; target = target->ai_next) {
    listen_address(epoll_file_descriptor, target);
  }
  freeaddrinfo(result);
  bool running = true;
  while (running) {
    struct epoll_event events[128];
    int event_count = epoll_wait(epoll_file_descriptor, events, 128, -1);
    for (int i = 0; i < event_count; i++) {
      struct file_descriptor_information *information = events[i].data.ptr;
      if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
        // error occurred or the connection is closed, free this connection
        destroy_file_information(information);
        if (*get_file_descriptor_list() == NULL) {
          // all connections are gone, exit
          running = false;
          break;
        }
      }
      if (information->type == LISTEN_SOCKET) {
        assert(events[i].events & EPOLLIN);
        accept_connection(epoll_file_descriptor, information->file_descriptor);
      } else {
        handle_connection();
      }
    }
  }
  return 0;
}