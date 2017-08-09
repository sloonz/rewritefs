#include <assert.h>
#include <errno.h>
#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

/* Recursively create all parent directories in `path`. */
int mkdir_parents(const char *path, mode_t mode) {
  int result = 0;

  // dirname() could clobber its argument.
  char *tmp = malloc(strlen(path) + 1);
  strcpy(tmp, path);
  char *path_ = tmp;

  // dirname() may statically allocate the result; we need to copy so we don’t
  // lose the result during recursion.
  tmp = dirname(path_);
  char *dir = malloc(strlen(tmp) + 1);
  strcpy(dir, tmp);

  struct stat dirstat;
  errno = 0;
  if ((stat(dir, &dirstat) == -1) && errno == ENOENT) {
    if (mkdir_parents(dir, mode)) {
      result = -1;
      goto done;
    }

    if (mkdir(path_, mode)) {
      result = -1;
      goto done;
    }
  }

done:
  free(dir);
  free(path_);
  return result;
}
