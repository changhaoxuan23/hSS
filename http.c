#include "http.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#ifndef NDEBUG
#include <stdio.h>
#endif
struct header {
  char *key;
  char *value;
  struct header *next;
};
struct headers {
  struct header *header_list;
};
static int insert_header(struct headers *headers, struct header *header) {
  // we keep strict order in the list
  if (headers->header_list == NULL) {
    header->next = headers->header_list;
    headers->header_list = header;
    return HTTP_ERROR_CODE_SUCCEED;
  }
  struct header *target = headers->header_list;
  while (target->next != NULL && strcmp(target->key, header->key) < 0) {
    target = target->next;
  }
  if (strcmp(target->key, header->key) == 0) {
    return HTTP_ERROR_CODE_DUPLICATE_HEADER_KEY;
  }
  header->next = target->next;
  target->next = header;
  return HTTP_ERROR_CODE_SUCCEED;
}
struct body {
  void *body;
  size_t length;
};

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

int http_request_from_buffer(struct http_request *_Nonnull restrict destination,
                             const void *_Nonnull restrict buffer, const size_t length) {
#define trigger_incomplete                                                                                   \
  do {                                                                                                       \
    destination->state = HTTP_REQUEST_STATE_INVALID;                                                         \
    return HTTP_ERROR_CODE_INCOMPLETE_REQUEST;                                                               \
  } while (false)
#define trigger_invalid                                                                                      \
  do {                                                                                                       \
    destination->state = HTTP_REQUEST_STATE_INVALID;                                                         \
    return HTTP_ERROR_CODE_PARSE_INVALID_REQUEST_SYNTAX;                                                     \
  } while (false)
#define skip_whitespaces                                                                                     \
  do {                                                                                                       \
    while (isblank(((const char *)buffer)[end])) {                                                           \
      if (++end == length) {                                                                                 \
        trigger_incomplete;                                                                                  \
      }                                                                                                      \
    }                                                                                                        \
    start = end;                                                                                             \
  } while (false)
#define range_non_blank                                                                                      \
  do {                                                                                                       \
    while (end < length && !isblank(((const char *)buffer)[end])) {                                          \
      end++;                                                                                                 \
    }                                                                                                        \
  } while (false)
#define range_line                                                                                           \
  do {                                                                                                       \
    while (end + 1 < length &&                                                                               \
           (((const char *)buffer)[end] != '\r' || ((const char *)buffer)[end + 1] != '\n')) {               \
      end++;                                                                                                 \
    }                                                                                                        \
    if (end + 1 == length) {                                                                                 \
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
      if (isblank(*c)) {
        trigger_invalid;
      }
    }
    size_t key_end = start + (colon - (const char *)buffer + start) + 1;
    //  the value shall not be empty, check it
    size_t value_start = key_end + 1;
    size_t value_end = end;
    //  there may be leading and/or tailing whitespaces
    while (value_start != value_end && isblank(((const char *)buffer)[value_start])) {
      value_start++;
    }
    while (value_end != value_start && isblank(((const char *)buffer)[value_end - 1])) {
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

#ifndef NDEBUG
  dump_request(destination);
#endif
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

int http_request_get_url(const struct http_request *_Nonnull restrict request,
                         char *_Nullable restrict buffer, size_t *_Nonnull restrict length) {
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

int http_request_get_header(const struct http_request *_Nonnull restrict request,
                            const char *_Nonnull restrict name, char *_Nullable restrict buffer,
                            size_t *_Nonnull restrict length) {
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
  // free headers
  {
    struct header *target = request->headers.header_list;
    while (target != NULL) {
      struct header *next = target->next;
      free(target->key);
      free(target->value);
      free(target);
      target = next;
    }
  }
  // free body: we do not need to do this since no body is supported
  // reinitialize this header to enable further usage
  return http_request_initialize(request);
}

int http_response_initialize(struct http_response *_Nonnull response) {}

int http_response_set_code(struct http_response *_Nonnull restrict response, enum http_response_code code,
                           const char *_Nullable restrict description) {}

int http_response_set_header(struct http_response *_Nonnull restrict response,
                             const char *_Nonnull restrict key, const char *_Nonnull restrict value) {}

int http_response_set_body(struct http_response *_Nonnull restrict response,
                           const void *_Nonnull restrict body, const size_t *_Nullable restrict length) {}

int http_response_render(struct http_response *_Nonnull restrict response, void *_Nullable restrict buffer,
                         const size_t *_Nonnull restrict length) {}

int http_response_destroy(struct http_request *_Nonnull request) {}