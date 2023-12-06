#ifndef HTTP_H_
#define HTTP_H_
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
// all interfaces returns negative value on failure

//////////////////////////////////////////////////////////////////////////////////////////////////////////////

enum http_error_code {
  // the request structure is not supplied with data with http_request_from_buffer or is invalid
  HTTP_ERROR_CODE_REQUEST_NO_VALID_DATA = INT_MIN,
  // the request does not complies with the RFC standard
  HTTP_ERROR_CODE_PARSE_INVALID_REQUEST_SYNTAX,
  // the supplied buffer has insufficient space to hold all the content
  HTTP_ERROR_CODE_INSUFFICIENT_BUFFER_SIZE,
  // the request to be parsed is not complete
  HTTP_ERROR_CODE_INCOMPLETE_REQUEST,
  // the request method is not HTTP GET
  HTTP_ERROR_CODE_UNSUPPORTED_METHOD,
  // attempting to add multiple headers with the same key
  HTTP_ERROR_CODE_DUPLICATE_HEADER_KEY,
  // the specified key does not exist in headers
  HTTP_ERROR_CODE_NO_SUCH_HEADER,
  // indicates a successful call
  HTTP_ERROR_CODE_SUCCEED = 0
};

//////////////////////////////////////////////////////////////////////////////////////////////////////////////

// HTTP request
struct http_request;
extern const size_t http_request_size;
// initialize the request structure
//  this method is called (at least) once before the same structure is supplied as an argument to any other
//  method of HTTP request
//  reinitialize an initialized request causes undefined behaviour
int http_request_initialize(struct http_request *_Nonnull request);

// parse http request in buffer
int http_request_from_buffer(
    struct http_request *_Nonnull restrict destination, const void *_Nonnull restrict buffer,
    const size_t length
);

// get request method, return the method code defined as the following enumerate if succeed
enum http_request_method {
  HTTP_REQUEST_METHOD_GET,
};
int http_request_get_method(const struct http_request *_Nonnull restrict request);

// get url of the request
//  if buffer is not NULL, the result is saved into the buffer, while length is updated to the actual length
//   of the content in case the space available, indicated by the origin value of length, is sufficient to
//   hold all the content, including the null-terminator while excluding the carriage return and line feed,
//   otherwise a error is returned while length is modified to the minimal space required to save the content
//  if buffer is NULL, only the length is updated as if buffer is not NULL while the space of buffer is not
//   sufficient, yet which is not treated as an error
//  that is, if the space is sufficient, when the buffer is filled by
//     0 1 2 3 4 5 6  7
//     e x a m p l e \0
//   then the length is updated to 7
int http_request_get_url(
    const struct http_request *_Nonnull restrict request, char *_Nullable restrict buffer,
    size_t *_Nonnull restrict length
);

// get header content
//  see also http_request_get_url
int http_request_get_header(
    const struct http_request *_Nonnull restrict request, const char *_Nonnull restrict name,
    char *_Nullable restrict buffer, size_t *_Nonnull restrict length
);

// cleanup HTTP request, free any dynamically allocated resource held by the structure
//  A call to this method may make the supplied structure at the same state as one after it is supplied to
//  http_request_initialize which may be used in subsequent procedure, or make it invalid for further usage,
//  which may be recovered into an usable state after reinitialized by http_request_initialize.
//  Either design is appropriate, but make it clear in documentation
int http_request_destroy(struct http_request *_Nonnull request);

//////////////////////////////////////////////////////////////////////////////////////////////////////////////

// HTTP response
struct http_response;
extern const size_t http_response_size;
// initialize a HTTP response, same restriction as http_request_initialize also applies to this method
int http_response_initialize(struct http_response *_Nonnull response);

// set response code
//  if NULL is passed to the nullable argument description, use a default description for it
//  otherwise, use the supplied description directly while assuming it is null-terminated
// NOTE: if you modified this enumerate here, update the corresponding mapping in http.c
enum http_response_code {
  HTTP_RESPONSE_CODE_OK,                         // 200
  HTTP_RESPONSE_CODE_NO_CONTENT,                 // 204
  HTTP_RESPONSE_CODE_PARTIAL_CONTENT,            // 206
  HTTP_RESPONSE_CODE_MOVE_MOVED_PERMANENTLY,     // 301
  HTTP_RESPONSE_CODE_BAD_REQUEST,                // 400
  HTTP_RESPONSE_CODE_FORBIDDEN,                  // 403
  HTTP_RESPONSE_CODE_NOT_FOUND,                  // 404
  HTTP_RESPONSE_CODE_INTERNAL_SERVER_ERROR,      // 500
  HTTP_RESPONSE_CODE_NOT_IMPLEMENTED,            // 501
  HTTP_RESPONSE_CODE_HTTP_VERSION_NOT_SUPPORTED, // 505
  HTTP_RESPONSE_CODE_MAX                         // keep this line at the bottom
};
int http_response_set_code(
    struct http_response *_Nonnull restrict response, enum http_response_code code,
    const char *_Nullable restrict description
);

// set response header
//  both key and value is null-terminated
//  if a header with the same key already exists, its value will be replaced
//  User NOTE: there is no need to care about Content-Length since which will be automatically calculated add
//   added to the response
int http_response_set_header(
    struct http_response *_Nonnull restrict response, const char *_Nonnull restrict key,
    const char *_Nonnull restrict value
);

// set body of response
//  if NULL is passed to the nullable argument length, treat body as null-terminated
//  otherwise, body may not be null-terminated, whose length shall be determined by the argument
//  if body is already set, it will be replaced
int http_response_set_body(
    struct http_response *_Nonnull restrict response, const void *_Nonnull restrict body,
    const size_t *_Nullable restrict length
);

// render the structure to a buffer
//  see also http_request_get_url
int http_response_render(
    struct http_response *_Nonnull restrict response, void *_Nullable restrict buffer,
    size_t *_Nonnull restrict length
);

// cleanup HTTP response, see also http_request_destroy
int http_response_destroy(struct http_response *_Nonnull response);

//////////////////////////////////////////////////////////////////////////////////////////////////////////////

// error interface
// get an description of the error code
const char *_Nonnull http_get_error_string(enum http_error_code error_code);
#endif