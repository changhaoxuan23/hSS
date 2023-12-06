#ifndef HTTP_HL_H_
#define HTTP_HL_H_
#include <stddef.h>
#include <stdint.h>
// representation of a range, treat start as inclusive while end as exclusive
//  start == 0 and end == 0 represents a full range regardless the real size of underlying target
struct range {
  size_t start;
  size_t end;
};
// parse the Range header in HTTP request, generating a structure range
//  this will select the first continues range of the specifier
//  if there are adjacent yet non-overleaping specifiers, they will be silently merged
//  if overleaping specifiers exists, or other error(s) detected, a full range will be returned to effectively
//   ignore the Range Request, allowing the server to respond with an 200(OK) without further adjustment
//  such range is also returned if the representation is NULL
struct range parse_range(const char *representation, size_t size);
#endif