#include <sys/socket.h>
#include <tcp_connection.h>
// these are just simple wrapper over recv and send
static ssize_t tcp_recv(struct connection_information *connection, void *buf, size_t nbytes) {
  return recv(connection->file_descriptor, buf, nbytes, 0);
}
static ssize_t tcp_send(struct connection_information *connection, const void *buf, size_t n) {
  // use MSG_NOSIGNAL to prevent a SIGPIPE signal from terminating our process
  //  just too lazy to set up a handler lol
  return send(connection->file_descriptor, buf, n, MSG_NOSIGNAL);
}
static void tcp_destroy_underlying(struct connection_information *connection) {
  logging_trace("closing TCP session with %s:%hu\n", get_address(connection), get_port(connection));
}
void tcp_initialize_underlying(struct connection_information *connection) {
  // there is no need for extra, hidden states
  connection->recv = tcp_recv;
  connection->send = tcp_send;
  connection->destroy_underlying = tcp_destroy_underlying;
}