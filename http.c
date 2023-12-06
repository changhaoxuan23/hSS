#include "http.h"
#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef NDEBUG
#define debug(fmt, ...) fprintf(stderr, fmt __VA_OPT__(, ) __VA_ARGS__)
#else
#define debug(fmt, ...)
#endif
struct header {
  char *key;
  char *value;
  struct header *next;
};
struct headers {
  struct header *header_list;
};

// lookup header with specified name from headers, return the pointer to which in *target if exists, the
// pointer to the pointer which, in the list of headers, points to or will point to target if exists in *prev
static void lookup_header(
    const struct headers *headers, const char *name, struct header **target, struct header ***prev
) {
  *prev = (struct header **)&headers->header_list;
  while (**prev != NULL && strcmp((**prev)->key, name) < 0) {
    *prev = &(**prev)->next;
  }
  if (**prev != NULL && strcmp((**prev)->key, name) == 0) {
    *target = **prev;
    return;
  }
  *target = NULL;
  return;
}

static int insert_header(struct headers *headers, struct header *header) {
  // we keep strict order in the list
  struct header **prev = NULL;
  struct header *target = NULL;
  lookup_header(headers, header->key, &target, &prev);
  if (target != NULL) {
    return HTTP_ERROR_CODE_DUPLICATE_HEADER_KEY;
  }
  header->next = *prev;
  *prev = header;
  return HTTP_ERROR_CODE_SUCCEED;
}

static void destroy_header(struct header *header) {
  if (header == NULL) {
    return;
  }
  free(header->key);
  free(header->value);
  header->key = NULL;
  header->value = NULL;
}

static void destroy_headers(struct headers *headers) {
  if (headers == NULL) {
    return;
  }
  struct header *target = headers->header_list;
  headers->header_list = NULL;
  while (target != NULL) {
    destroy_header(target);
    struct header *next = target->next;
    free(target);
    target = next;
  }
}

struct body {
  void *body;
  size_t length;
};

static void destroy_body(struct body *body) {
  if (body == NULL) {
    return;
  }
  if (body->body != NULL) {
    free(body->body);
    body->body = NULL;
  }
  body->length = 0;
}

struct request_start_line {
  enum http_request_method method;
  char *http_version;
  size_t http_version_length;
  char *url;
  size_t url_length;
};
struct http_request {
  enum http_request_state {
    // initialized, ready for parsing message
    HTTP_REQUEST_STATE_INITIALIZED,
    // message loaded, ready for queries
    HTTP_REQUEST_STATE_PARSED,
    // invalid request
    HTTP_REQUEST_STATE_INVALID,
  } state;
  struct request_start_line start_line;
  struct headers headers;
  struct body body;
};
const size_t http_request_size = sizeof(struct http_request);

#ifndef NDEBUG
static void dump_request(const struct http_request *_Nonnull request) {
  fprintf(stderr, "\n================================ request dump ================================\n");
  fprintf(stderr, "Method       = [GET]\n");
  fprintf(stderr, "HTTP version = [%s]\n", request->start_line.http_version);
  fprintf(stderr, "URL          = [%s]\n", request->start_line.url);
  fprintf(stderr, "Headers      = [\n");
  for (struct header *target = request->headers.header_list; target != NULL; target = target->next) {
    fprintf(stderr, "  Key          = [%s]\n", target->key);
    fprintf(stderr, "  Value        = [%s]\n", target->value);
  }
  fprintf(stderr, "]\n");
  fprintf(stderr, "Body         = []\n");
  fprintf(stderr, "================================ request dump ================================\n\n");
}
#endif

int http_request_initialize(struct http_request *_Nonnull request) {
  request->headers.header_list = NULL;
  request->start_line.http_version = NULL;
  request->start_line.http_version_length = 0;
  request->start_line.url = NULL;
  request->start_line.url_length = 0;
  request->body.body = NULL;
  request->body.length = 0;
  request->state = HTTP_REQUEST_STATE_INITIALIZED;
  return HTTP_ERROR_CODE_SUCCEED;
}

int http_request_from_buffer(
    struct http_request *_Nonnull restrict destination, const void *_Nonnull restrict buffer,
    const size_t length
) {
#define trigger_incomplete                                                                                   \
  do {                                                                                                       \
    destination->state = HTTP_REQUEST_STATE_INVALID;                                                         \
    debug("incomplete triggered at line %d\n", __LINE__);                                                    \
    return HTTP_ERROR_CODE_INCOMPLETE_REQUEST;                                                               \
  } while (false)
#define trigger_invalid                                                                                      \
  do {                                                                                                       \
    destination->state = HTTP_REQUEST_STATE_INVALID;                                                         \
    debug("invalid triggered at line %d\n", __LINE__);                                                       \
    return HTTP_ERROR_CODE_PARSE_INVALID_REQUEST_SYNTAX;                                                     \
  } while (false)
#define skip_whitespaces                                                                                     \
  do {                                                                                                       \
    while (isspace(((const char *)buffer)[end])) {                                                           \
      if (++end == length) {                                                                                 \
        trigger_incomplete;                                                                                  \
      }                                                                                                      \
    }                                                                                                        \
    start = end;                                                                                             \
  } while (false)
#define range_non_blank                                                                                      \
  do {                                                                                                       \
    while (end < length && !isspace(((const char *)buffer)[end])) {                                          \
      end++;                                                                                                 \
    }                                                                                                        \
  } while (false)
#define range_line                                                                                           \
  do {                                                                                                       \
    while (end + 1 < length &&                                                                               \
           (((const char *)buffer)[end] != '\r' || ((const char *)buffer)[end + 1] != '\n')) {               \
      end++;                                                                                                 \
    }                                                                                                        \
    if (end + 1 >= length) {                                                                                 \
      trigger_incomplete;                                                                                    \
    }                                                                                                        \
  } while (false)
  if (destination->state != HTTP_REQUEST_STATE_INITIALIZED) {
    http_request_destroy(destination);
  }
  size_t start = 0, end = 0;

  // parse METHOD
  range_non_blank;
  //  we only support GET
  if (strncmp("GET", buffer, end - start) != 0) {
    return HTTP_ERROR_CODE_UNSUPPORTED_METHOD;
  }
  destination->start_line.method = HTTP_REQUEST_METHOD_GET;
  skip_whitespaces;

  // parse url
  range_non_blank;
  if (start == end) {
    trigger_incomplete;
  }
  destination->start_line.url_length = end - start;
  destination->start_line.url = malloc(destination->start_line.url_length + 1);
  memcpy(destination->start_line.url, buffer + start, destination->start_line.url_length);
  destination->start_line.url[destination->start_line.url_length] = '\0';
  skip_whitespaces;

  // parse http version
  range_non_blank;
  //  well, since this value is not used, we just do not check it, just make sure it is not empty
  if (start == end) {
    trigger_incomplete;
  }
  destination->start_line.http_version_length = end - start;
  destination->start_line.http_version = malloc(destination->start_line.http_version_length + 1);
  memcpy(destination->start_line.http_version, buffer + start, destination->start_line.http_version_length);
  destination->start_line.http_version[destination->start_line.http_version_length] = '\0';

  //  assert we are facing a CRLF pair
  if (end + 1 >= length) {
    trigger_incomplete;
  }
  if (((const char *)buffer)[end] != '\r' || ((const char *)buffer)[end + 1] != '\n') {
    trigger_invalid;
  }
  end += 2;
  start = end;

  // parse headers
  while (true) {
    range_line;
    if (start == end) {
      end += 2;
      start = end;
      break;
    }
    // find the first colon
    const char *colon = strchr((const char *)buffer + start, ':');
    if (colon == NULL) {
      trigger_invalid;
    }
    //  no whitespace is allowed from the beginning of the line to the colon, we will restrict this
    for (const char *c = buffer + start; c < colon; c++) {
      if (isspace(*c)) {
        trigger_invalid;
      }
    }
    size_t key_end = colon - (const char *)buffer;
    //  the value shall not be empty, check it
    size_t value_start = key_end + 1;
    size_t value_end = end;
    //  there may be leading and/or tailing whitespaces
    while (value_start != value_end && isspace(((const char *)buffer)[value_start])) {
      value_start++;
    }
    while (value_end != value_start && isspace(((const char *)buffer)[value_end - 1])) {
      value_end--;
    }
    if (value_start == value_end) {
      trigger_invalid;
    }
    //  no more error is expected to be encountered from now on for this line, we can allocate space for it
    struct header *header = malloc(sizeof(struct header));
    header->key = malloc(key_end - start + 1);
    memcpy(header->key, buffer + start, key_end - start);
    header->key[key_end - start] = '\0';
    header->value = malloc(value_end - value_start + 1);
    memcpy(header->value, buffer + value_start, value_end - value_start);
    header->value[value_end - value_start] = '\0';
    //  insert the header to header list
    int result = insert_header(&destination->headers, header);
    if (result != HTTP_ERROR_CODE_SUCCEED) {
      destination->state = HTTP_REQUEST_STATE_INVALID;
      return result;
    }
    // update the index
    end += 2;
    start = end;
  }

  // parse body: since we only support HTTP GET, the request shall not contain body
  //  we assert such restriction
  if (end != length) {
    trigger_invalid;
  }

  destination->state = HTTP_REQUEST_STATE_PARSED;
  return HTTP_ERROR_CODE_SUCCEED;

#undef trigger_incomplete
#undef trigger_invalid
#undef skip_whitespaces
#undef range_non_blank
#undef range_line
}

int http_request_get_method(const struct http_request *_Nonnull restrict request) {
  if (request->state != HTTP_REQUEST_STATE_PARSED) {
    return HTTP_ERROR_CODE_REQUEST_NO_VALID_DATA;
  }
  return request->start_line.method;
}

int http_request_get_url(
    const struct http_request *_Nonnull restrict request, char *_Nullable restrict buffer,
    size_t *_Nonnull restrict length
) {
  if (request->state != HTTP_REQUEST_STATE_PARSED) {
    return HTTP_ERROR_CODE_REQUEST_NO_VALID_DATA;
  }
  if (buffer == NULL || (buffer != NULL && *length < request->start_line.url_length + 1)) {
    *length = request->start_line.url_length + 1;
    return buffer == NULL ? HTTP_ERROR_CODE_SUCCEED : HTTP_ERROR_CODE_INSUFFICIENT_BUFFER_SIZE;
  }
  strcpy(buffer, request->start_line.url);
  *length = request->start_line.url_length;
  return HTTP_ERROR_CODE_SUCCEED;
}

int http_request_get_header(
    const struct http_request *_Nonnull restrict request, const char *_Nonnull restrict name,
    char *_Nullable restrict buffer, size_t *_Nonnull restrict length
) {
  if (request->state != HTTP_REQUEST_STATE_PARSED) {
    return HTTP_ERROR_CODE_REQUEST_NO_VALID_DATA;
  }
  struct header *header = request->headers.header_list;
  while (header != NULL && strcmp(header->key, name) < 0) {
    header = header->next;
  }
  if (header == NULL || strcmp(header->key, name) != 0) {
    return HTTP_ERROR_CODE_NO_SUCH_HEADER;
  }
  size_t value_length = strlen(header->value);
  if (buffer == NULL || (buffer != NULL && *length < value_length + 1)) {
    *length = value_length + 1;
    return buffer == NULL ? HTTP_ERROR_CODE_SUCCEED : HTTP_ERROR_CODE_INSUFFICIENT_BUFFER_SIZE;
  }
  strcpy(buffer, header->value);
  *length = value_length;
  return HTTP_ERROR_CODE_SUCCEED;
}

int http_request_destroy(struct http_request *_Nonnull request) {
  if (request->state == HTTP_REQUEST_STATE_INITIALIZED) {
    return HTTP_ERROR_CODE_SUCCEED;
  }
  // free start line
  free(request->start_line.http_version);
  free(request->start_line.url);
  // free headers
  destroy_headers(&request->headers);
  // free body: we do not need to do this since no body is supported, but it does not harm to do so
  destroy_body(&request->body);

  // reinitialize this header to enable further usage
  return http_request_initialize(request);
}

struct state_line {
  enum http_response_code code;
  char *description;
  size_t description_length;
};
struct http_response {
  struct state_line state_line;
  struct headers headers;
  struct body body;
};
const size_t http_response_size = sizeof(struct http_response);

int http_response_initialize(struct http_response *_Nonnull response) {
  response->state_line.description = NULL;
  response->state_line.description_length = 0;
  response->headers.header_list = NULL;
  response->body.body = NULL;
  response->body.length = 0;
  return HTTP_ERROR_CODE_SUCCEED;
}

// get default description of a state code, NULL if such code is not matched
static const char *get_default_description(enum http_response_code code) {
  static char *descriptions[] = {
      "OK",        "No Content", "Partial Content",       "Moved Permanently", "Bad Request",
      "Forbidden", "Not Found",  "Internal Server Error", "Not Implemented",   "HTTP Version Not Supported",
  };
  assert(sizeof(descriptions) / sizeof(descriptions[0]) == HTTP_RESPONSE_CODE_MAX);
  if (code >= HTTP_RESPONSE_CODE_MAX || code < 0) {
    debug("unmapped code value %d for response state\n", code);
    return NULL;
  }
  return descriptions[code];
}
static const char *get_representative_state_code(enum http_response_code code) {
  static char *state[] = {"200", "204", "206", "301", "400", "403", "404", "500", "501", "505"};
  static char buffer[16];
  assert(sizeof(state) / sizeof(state[0]) == HTTP_RESPONSE_CODE_MAX);
  if (code >= HTTP_RESPONSE_CODE_MAX || code < 0) {
    debug("unmapped code value %d for response state\n", code);
    sprintf(buffer, "%03d", code);
    return buffer;
  }
  return state[code];
}

int http_response_set_code(
    struct http_response *_Nonnull restrict response, enum http_response_code code,
    const char *_Nullable restrict description
) {
  if (response->state_line.description != NULL) {
    free(response->state_line.description);
    response->state_line.description_length = 0;
  }
  if (description == NULL) {
    description = get_default_description(code);
  }
  response->state_line.code = code;
  if (description != NULL) {
    response->state_line.description_length = strlen(description);
    response->state_line.description = malloc(response->state_line.description_length + 1);
    strcpy(response->state_line.description, description);
  }
  return HTTP_ERROR_CODE_SUCCEED;
}

int http_response_set_header(
    struct http_response *_Nonnull restrict response, const char *_Nonnull restrict key,
    const char *_Nonnull restrict value
) {
  struct header **prev = NULL;
  struct header *target = NULL;
  lookup_header(&response->headers, key, &target, &prev);
  if (target != NULL) {
    free(target->value);
    target->value = malloc(strlen(value) + 1);
    strcpy(target->value, value);
  } else {
    target = malloc(sizeof(struct header));
    target->key = malloc(strlen(key) + 1);
    strcpy(target->key, key);
    target->value = malloc(strlen(value) + 1);
    strcpy(target->value, value);
    target->next = *prev;
    *prev = target;
  }
  return HTTP_ERROR_CODE_SUCCEED;
}

int http_response_set_body(
    struct http_response *_Nonnull restrict response, const void *_Nonnull restrict body,
    const size_t *_Nullable restrict length
) {
  destroy_body(&response->body);
  response->body.length = length == NULL ? strlen(body) : *length;
  response->body.body = malloc(response->body.length);
  memcpy(response->body.body, body, response->body.length);
  // regenerate Content-Length
  char buffer[64]; // such size shall be overwhelmingly large
  sprintf(buffer, "%lu", response->body.length);
  return http_response_set_header(response, "Content-Length", buffer);
}

// HTTP version used for response
static const char *const HTTP_VERSION = "HTTP/1.1";
static const size_t http_version_length = strlen(HTTP_VERSION);
static const size_t state_code_length = 3;
// measure the size of buffer required to render the response
static size_t measure_response_size(const struct http_response *response) {
  size_t result = 0;
  // start line: [<VERSION> <STATE_CODE>{ <DESCRIPTION>}\r\n]
  if (response->state_line.description_length != 0) {
    result += http_version_length + state_code_length + response->state_line.description_length + 4;
  } else {
    result += http_version_length + state_code_length + 3;
  }
  // each header: [<KEY>: <VALUE>\r\n]
  for (struct header *target = response->headers.header_list; target != NULL; target = target->next) {
    result += strlen(target->key) + strlen(target->value) + 4;
  }
  // empty line splitting headers and body
  result += 2;
  // body size
  result += response->body.length;
  return result;
}

static inline void copy_and_advance(void *restrict *destination, const void *restrict source, size_t size) {
  memcpy(*destination, source, size);
  *destination += size;
}

int http_response_render(
    struct http_response *_Nonnull restrict response, void *_Nullable restrict buffer,
    size_t *_Nonnull restrict length
) {
  // update Content-Length if body was not set
  if (response->body.length == 0) {
    assert(response->body.body == NULL);
    http_response_set_header(response, "Content-Length", "0");
  }
  size_t size = measure_response_size(response);
  if (buffer == NULL || *length < size) {
    *length = size;
    return buffer == NULL ? HTTP_ERROR_CODE_SUCCEED : HTTP_ERROR_CODE_INSUFFICIENT_BUFFER_SIZE;
  }
  // state line
  copy_and_advance(&buffer, HTTP_VERSION, http_version_length);
  copy_and_advance(&buffer, " ", 1);
  copy_and_advance(&buffer, get_representative_state_code(response->state_line.code), 3);
  if (response->state_line.description_length != 0) {
    copy_and_advance(&buffer, " ", 1);
    copy_and_advance(&buffer, response->state_line.description, response->state_line.description_length);
  }
  copy_and_advance(&buffer, "\r\n", 2);
  // headers
  for (struct header *target = response->headers.header_list; target != NULL; target = target->next) {
    copy_and_advance(&buffer, target->key, strlen(target->key));
    copy_and_advance(&buffer, ": ", 2);
    copy_and_advance(&buffer, target->value, strlen(target->value));
    copy_and_advance(&buffer, "\r\n", 2);
  }
  // empty line
  copy_and_advance(&buffer, "\r\n", 2);
  // body
  copy_and_advance(&buffer, response->body.body, response->body.length);
  *length = size;
  return HTTP_ERROR_CODE_SUCCEED;
}

int http_response_destroy(struct http_response *_Nonnull response) {
  free(response->state_line.description);
  destroy_headers(&response->headers);
  destroy_body(&response->body);
  return http_response_initialize(response);
}

const char *http_get_error_string(enum http_error_code error_code) {
  switch (error_code) {
  case HTTP_ERROR_CODE_REQUEST_NO_VALID_DATA:
    return "the request supplied does not hold valid information: it may not initialized with HTTP request "
           "message, or some error occurred when parsing the message";
  case HTTP_ERROR_CODE_PARSE_INVALID_REQUEST_SYNTAX:
    return "the syntax of HTTP request message to be parsed is invalid, therefore which may not be processed";
  case HTTP_ERROR_CODE_INSUFFICIENT_BUFFER_SIZE:
    return "the buffer supplied is insufficient to hold all the results";
  case HTTP_ERROR_CODE_INCOMPLETE_REQUEST:
    return "the HTTP request message to be processed is not complete";
  case HTTP_ERROR_CODE_UNSUPPORTED_METHOD:
    return "the HTTP request attempted a method that is not supported";
  case HTTP_ERROR_CODE_DUPLICATE_HEADER_KEY:
    return "the attempt to add multiple headers with the same key is rejected";
  case HTTP_ERROR_CODE_NO_SUCH_HEADER:
    return "the requested key does not exist in headers";
  case HTTP_ERROR_CODE_SUCCEED:
    return "not an error, the operation succeed";
  }
}