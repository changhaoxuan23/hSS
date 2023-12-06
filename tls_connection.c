#include <common.h>
#include <errno.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509-ext.h>
#include <gnutls/x509.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tls_connection.h>

struct connection_underlying {
  // state of the TLS connection
  enum tls_state {
    TLS_STATE_Initialized, // the session is now initialized locally
    TLS_STATE_Handshaking, // handshaking in progress
    TLS_STATE_Established, // TLS connection established and fully functional
    TLS_STATE_Failed,      // the connection has failed
  } state;
  // session used for this connection
  gnutls_session_t session;
};

// make a GNUTLS call as specified in call with the following argument list
//  if such call succeed, no further cation is taken
//  otherwise, detailed information about the call and the error returned is logged, and failed_call is called
#define GNUTLS_HELPER(failed_call, call, ...)                                                                \
  do {                                                                                                       \
    int rtv = call(__VA_ARGS__);                                                                             \
    if (rtv != 0) {                                                                                          \
      fprintf(                                                                                               \
          stderr, "GNUTLS call " #call " failed" __VA_OPT__(" with arguments ") #__VA_ARGS__ ": %s\n",       \
          gnutls_strerror(rtv)                                                                               \
      );                                                                                                     \
      failed_call;                                                                                           \
    }                                                                                                        \
  } while (false)
// this is an internal helper, do not call directly
static void destroy_credential(void);
// this function shall ALWAYS be called with argument set to false
// get credential used for all sessions, which is exactly the certificate and key of the server
//  this function calls exit(3) in case a failure occurs since absent of credential shall prevent the server
//   from being functional ultimately
static gnutls_certificate_credentials_t get_credential(bool destroy) {
  static gnutls_certificate_credentials_t credential;
  static bool initialize = true;
  if (destroy) {
    if (initialize) {
      return NULL;
    }
    gnutls_certificate_free_credentials(credential);
  }
  if (initialize) {
    // allocate the structure for a certificate
    GNUTLS_HELPER(exit(EXIT_FAILURE), gnutls_certificate_allocate_credentials, &credential);
    // load certificate from file
    GNUTLS_HELPER(
        exit(EXIT_FAILURE), gnutls_certificate_set_x509_key_file2, credential, "keys/cnlab.cert",
        "keys/cnlab.prikey", GNUTLS_X509_FMT_PEM, NULL, GNUTLS_PKCS_PLAIN
    );
    // register automatic destroy of credential
    atexit(destroy_credential);
    initialize = true;
  }
  return credential;
}
void destroy_credential(void) {
  // free credential
  get_credential(true);
}

static int do_handshake(struct connection_information *connection) {
  struct connection_underlying *underlying = connection->underlying;
  logging_trace("handshaking with %s:%hu\n", get_address(connection), get_port(connection));
  int result = gnutls_handshake(underlying->session);
  if (result == 0) {
    logging_trace("handshake done with %s:%hu\n", get_address(connection), get_port(connection));
    underlying->state = TLS_STATE_Established;
  } else if (result == GNUTLS_E_FATAL_ALERT_RECEIVED || result == GNUTLS_E_WARNING_ALERT_RECEIVED) {
    void (*logging)(const char *, ...) =
        result == GNUTLS_E_FATAL_ALERT_RECEIVED ? logging_error : logging_warning;
    gnutls_alert_description_t alert = gnutls_alert_get(underlying->session);
    logging("received alert: %s\n", gnutls_alert_get_name(alert));
    if (result == GNUTLS_E_FATAL_ALERT_RECEIVED) {
      // fatal alert shall terminate the session
      underlying->state = TLS_STATE_Failed;
    }
  }
  return result;
}
// set corresponding errno from gnutls error code
static void set_errno(long gnutls_error_code) {
  switch (gnutls_error_code) {
  case GNUTLS_E_AGAIN:
  case GNUTLS_E_WARNING_ALERT_RECEIVED:
    errno = EAGAIN;
    break;
  case GNUTLS_E_INTERRUPTED:
    errno = EINTR;
    break;
  case GNUTLS_E_FATAL_ALERT_RECEIVED:
    errno = EBADF;
    break;
  default:
    errno = 0;
    break;
  }
  logging_trace(
      "set errno to %d(%s) by gnutls error code %d(%s)\n", errno, strerror(errno), gnutls_error_code,
      gnutls_strerror((int)gnutls_error_code)
  );
}

// recv/send wrapper for TLS connections
typedef ssize_t (*operation_t)(gnutls_session_t, void *, size_t);
static ssize_t
tls_recv_send(struct connection_information *connection, operation_t operation, void *buf, size_t nbytes) {
  struct connection_underlying *underlying = connection->underlying;
  if (underlying->state == TLS_STATE_Failed) {
    errno = EBADF;
    return -1;
  }
  if (underlying->state == TLS_STATE_Initialized || underlying->state == TLS_STATE_Handshaking) {
    int result = do_handshake(connection);
    if (result != 0) {
      set_errno(result);
      return -1;
    }
  }
  if (underlying->state == TLS_STATE_Established) {
    ssize_t size = operation(underlying->session, buf, nbytes);
    if (size < 0) {
      set_errno(size);
      return -1;
    }
    return size;
  } else {
    logging_error("invalid TLS session state: value = %d\n", underlying->state);
    errno = EBADF;
    return -1;
  }
}
static ssize_t tls_recv(struct connection_information *connection, void *buf, size_t nbytes) {
  ssize_t result = tls_recv_send(connection, gnutls_record_recv, buf, nbytes);
  // save errno
  int error = errno;
  logging_trace("%ld bytes received from %s:%hu\n", result, get_address(connection), get_port(connection));
  errno = error;
  return result;
}
static ssize_t tls_send(struct connection_information *connection, const void *buf, size_t n) {
  ssize_t result = tls_recv_send(connection, (operation_t)gnutls_record_send, (void *)buf, n);
  int error = errno;
  logging_trace("%ld bytes sent to %s:%hu\n", result, get_address(connection), get_port(connection));
  errno = error;
  return result;
}

static void tls_destroy_underlying(struct connection_information *connection) {
  struct connection_underlying *underlying = connection->underlying;
  // use blocking terminate here
  int result;
  logging_trace("tearing down TLS session with %s:%hu\n", get_address(connection), get_port(connection));
  do {
    result = gnutls_bye(underlying->session, GNUTLS_SHUT_RDWR);
  } while (result == GNUTLS_E_AGAIN);
  if (result == 0) {
    logging_trace("TLS session closed gracefully\n");
  } else {
    logging_debug("unclear close of TLS session: %s\n", gnutls_strerror(result));
  }
  gnutls_deinit(underlying->session);
  free(connection->underlying);
  connection->underlying = NULL;
}

void tls_initialize_underlying(struct connection_information *connection) {
  // we do need extra state here
  connection->underlying = malloc(sizeof(struct connection_underlying));
  struct connection_underlying *underlying = connection->underlying;
  underlying->state = TLS_STATE_Failed;
  // setup session
  GNUTLS_HELPER(return, gnutls_init, &underlying->session,
                      GNUTLS_SERVER          // this is a session for server side
                          | GNUTLS_NONBLOCK  // no-blocking mode is used since we use it with epoll ET
                          | GNUTLS_NO_SIGNAL // do not generate SIGPIPE
                          | GNUTLS_POST_HANDSHAKE_AUTH // enable for auto re-auth
                          | GNUTLS_AUTO_REAUTH         // let GNUTLS handle re-handshake automatically
  );
  // simply use the default settings
  GNUTLS_HELPER(return, gnutls_set_default_priority, underlying->session);
  // set the certificate/key pair
  GNUTLS_HELPER(return, gnutls_credentials_set, underlying->session, GNUTLS_CRD_CERTIFICATE,
                      get_credential(false));
  // setup socket file descriptor for communication
  gnutls_transport_set_int(underlying->session, connection->file_descriptor);
  // setup wrapper for recv/send
  connection->recv = tls_recv;
  connection->send = tls_send;
  // setup destructor
  connection->destroy_underlying = tls_destroy_underlying;
  // update state
  underlying->state = TLS_STATE_Initialized;
}