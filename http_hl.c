#include <assert.h>
#include <ctype.h>
#include <http_hl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct range parse_single_range(const char *representation, size_t size) {
  // no space is allowed here which simplifies the procedure
  struct range range = {.start = 0, .end = 0};
  if (*representation == '-') {
    // suffix-range
    range.start = size - strtoll(representation + 1, NULL, 10);
    range.end = size;
  } else {
    // int-range
    // get first-pos
    char *mid = NULL;
    range.start = strtoll(representation, &mid, 10);
    assert(*mid == '-');
    if (isdigit(mid[1])) {
      // get last-pos
      range.end = strtoll(mid + 1, NULL, 10) + 1;
    } else {
      // no end-pos
      range.end = size;
    }
  }
  if (range.start >= range.end) {
    range.start = 0;
    range.end = 0;
  }
  return range;
}
static int compare_range(const void *lhs_, const void *rhs_) {
  const struct range *lhs = lhs_;
  const struct range *rhs = rhs_;
  if (lhs->start != rhs->start) {
    return lhs->start < rhs->start ? -1 : 1;
  }
  if (lhs->end != rhs->end) {
    return lhs->end < rhs->end ? -1 : 1;
  }
  return 0;
}
struct range parse_range(const char *representation, size_t size) {
  struct range range = {.start = 0, .end = 0};
  if (representation == NULL) {
    return range;
  }
  // check the unit
  while (isspace(*representation)) {
    representation++;
  }
  if (strncmp(representation, "bytes=", 6) != 0) {
    return range;
  }
  representation += 6;
  // count number of ranges
  size_t n_ranges = 1;
  for (const char *target = representation; *target != '\0'; target++) {
    if (*target == ',') {
      n_ranges++;
    }
  }
  struct range *ranges = malloc(sizeof(struct range) * n_ranges);
  for (size_t i = 0; i < n_ranges; i++) {
    while (isspace(*representation)) {
      representation++;
    }
    ranges[i] = parse_single_range(representation, size);
    do {
      representation++;
    } while (representation[-1] != ',' && representation[-1] != '\0');
  }
  struct range partial_result = ranges[0];
  qsort(ranges, n_ranges, sizeof(ranges[0]), compare_range);
  for (size_t i = 0; i < n_ranges; i++) {
    // if a full range is found, it shall always be returned since it represents an error
    if (ranges[i].start == 0 && ranges[i].end == 0) {
      partial_result.start = 0;
      partial_result.end = 0;
      break;
    }
    if (i != 0) {
      // detect overlapping ranges
      //  due to the way we sort the ranges, we have
      //    ranges[i].start >= ranges[i-1].start
      //   and
      //    ranges[i].end >= ranges[i-1].end
      if (ranges[i].start < ranges[i - 1].end) {
        partial_result.start = 0;
        partial_result.end = 0;
        break;
      }
    }
    // try to merge with current partial result
    if (ranges[i].end == partial_result.start) {
      partial_result.start = ranges[i].start;
    } else if (ranges[i].start == partial_result.end) {
      partial_result.end = ranges[i].end;
    }
  }
  range = partial_result;
  free(ranges);
  return range;
}