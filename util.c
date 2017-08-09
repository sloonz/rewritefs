#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

/* Replace the first occurrence of `from` in `source` by `to` and return the
   result as a newly allocated string. If `from` is not found in `source` return
   NULL. */
char *string_replace(const char *source, const char *from, const char *to) {
  char *found = strstr(source, from);
  if (!found) {
    errno = EINVAL;
    return NULL;
  }

  size_t source_len = strlen(source);
  size_t from_len = strlen(from);
  size_t to_len = strlen(to);

  size_t substring_begin = found - source;
  size_t substring_end = substring_begin + from_len;

  size_t before_len = substring_begin;
  size_t after_len = source_len - substring_end;
  size_t dest_len = before_len + to_len + after_len;

  // Part preceeding `from`.
  char *before = malloc(before_len + 1);
  strncpy(before, source, before_len);
  *(before + before_len) = 0;

  // Part succeeding `from`.
  const char *after = source + substring_end;

  char *dest = malloc(dest_len + 1);
  int written = snprintf(dest, dest_len + 1, "%s%s%s", before, to, after);
  assert(written == dest_len);

  free(before);
  return dest;
}
