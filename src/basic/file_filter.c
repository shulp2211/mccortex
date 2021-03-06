#include "global.h"
#include "file_filter.h"
#include "range.h"
#include "util.h"
#include "file_util.h"

#define is_range_char(c) (((c) >= '0' && (c) <= '9') || (c) == '-' || (c) == ',')

// Get pointers to start and end of actual path
// (\d+(-\d+)?(,\d+(-\d+)?)*:)?path.ctx(:\d+(-\d+)?(,\d+(-\d+)?)*)?
static inline void file_filter_deconstruct_path(const char *path,
                                                const char **start,
                                                const char **end)
{
  const char *ptr = path;
  *start = path;
  while(is_range_char(*ptr)) ptr++;
  if(ptr > path && *ptr == ':') { ptr++; *start = ptr; }
  // Count backwards to match /:[-,0123456789]*$/
  ptr = *end = path + strlen(path);
  while(ptr > (*start)+1) {
    ptr--;
    if(*ptr == ':') { *end = ptr; break; }
    else if(!is_range_char(*ptr)) break;
  }
}

// Parse path and create FileFilter, calls die() with msg on error
// fltr should be zero'd before call
void file_filter_open(FileFilter *fltr, const char *path)
{
  const char *path_start, *path_end;
  size_t path_len;

  // Duplicate input string and file path
  strbuf_set(&fltr->input, path);
  file_filter_deconstruct_path(path, &path_start, &path_end);
  path_len = path_end - path_start;
  strbuf_ensure_capacity(&fltr->path, path_len);
  memcpy(fltr->path.b, path_start, path_len);
  fltr->path.b[fltr->path.end = path_len] = '\0';
}

// Create a direct filter, ready to be used on return
void file_filter_create_direct(FileFilter *fltr, size_t srcncols, size_t ncols)
{
  assert(srcncols >= ncols);
  size_t i;
  strbuf_set(&fltr->input, "");
  strbuf_set(&fltr->path, "");
  fltr->srcncols = srcncols;
  filter_buf_capacity(&fltr->filter, ncols);
  for(i = 0; i < ncols; i++) fltr->filter.b[i].from = fltr->filter.b[i].into = i;
  file_filter_num(fltr) = ncols;
  file_filter_update(fltr);
}

// Close file, release memory
void file_filter_close(FileFilter *fltr)
{
  strbuf_dealloc(&fltr->input);
  strbuf_dealloc(&fltr->path);
  filter_buf_dealloc(&fltr->filter);
  memset(fltr, 0, sizeof(FileFilter));
}

// Recalculate fromncols, intoncols
// Needs to be called after each modification to the filter
void file_filter_update(FileFilter *fltr)
{
  fltr->fromncols = file_filter_max_from(fltr->filter.b, fltr->filter.len) + 1;
  fltr->intoncols = file_filter_max_into(fltr->filter.b, fltr->filter.len) + 1;
}

// If there is no 'into' filter (e.g. 0,1:in.ctx 0,1 is 'into filter'),
// load into `into_offset..into_offset+N-1`
void file_filter_set_cols(FileFilter *fltr, size_t srcncols, size_t into_offset)
{
  size_t i;
  const char *path_start_c, *path_end_c;
  file_filter_deconstruct_path(fltr->input.b, &path_start_c, &path_end_c);

  // This is a hack to get non-const pointer
  size_t offset_start = path_start_c - fltr->input.b;
  size_t offset_end = path_end_c - fltr->input.b;
  char *path_start = fltr->input.b + offset_start;
  char *path_end = fltr->input.b + offset_end;

  char *from_fltr = (*path_end == ':' ? path_end+1 : NULL);
  char *into_fltr = (path_start > fltr->input.b ? fltr->input.b : NULL);

  size_t ncols;

  if(from_fltr) {
    int s = range_get_num(from_fltr, srcncols-1);
    if(s < 0)
      die("Invalid filter path: %s (from size: %zu)", fltr->input.b, srcncols);
    ncols = s;
  } else {
    ncols = srcncols;
  }

  if(into_fltr) {
    *(path_start-1) = '\0';
    int s = range_get_num(into_fltr, SIZE_MAX);
    *(path_start-1) = ':';
    if(s < 0 || (s != 1 && (size_t)s != ncols))
      die("Invalid filter path: %s (s:%i ncols:%zu)", fltr->input.b, s, ncols);
  }

  fltr->srcncols = srcncols;
  filter_buf_capacity(&fltr->filter, ncols);
  file_filter_num(fltr) = ncols;

  size_t *tmp = ctx_calloc(ncols, sizeof(size_t));

  if(from_fltr)
  {
    if(range_parse_array(from_fltr, tmp, srcncols-1) == -1)
      die("Invalid filter path: %s", fltr->input.b);
    for(i = 0; i < ncols; i++)
      file_filter_fromcol(fltr, i) = tmp[i];
  }
  else {
    for(i = 0; i < ncols; i++)
      file_filter_fromcol(fltr, i) = i;
  }

  if(into_fltr)
  {
    *(path_start-1) = '\0';
    int s = range_parse_array_fill(into_fltr, tmp, SIZE_MAX, ncols);
    *(path_start-1) = ':';
    if(s < 0 || (size_t)s != ncols)
      die("Invalid filter path: %s (s:%i ncols:%zu)", fltr->input.b, s, ncols);
    for(i = 0; i < ncols; i++)
      file_filter_intocol(fltr, i) = tmp[i];
  }
  else {
    for(i = 0; i < ncols; i++)
      file_filter_intocol(fltr, i) = into_offset + i;
  }

  ctx_free(tmp);

  filters_sort_by_into(fltr->filter.b, file_filter_num(fltr));
  file_filter_update(fltr);

  // for(i = 0; i < file_filter_num(fltr); i++)
  //   status("%u -> %u", file_filter_fromcol(fltr,i), file_filter_intocol(fltr,i));
}

// @add amount to add to each value of intocols
void file_filter_shift_cols(FileFilter *fltr, size_t add)
{
  size_t i;
  for(i = 0; i < file_filter_num(fltr); i++)
    file_filter_intocol(fltr, i) += add;

  file_filter_update(fltr);
}

// Returns true if each colour is loaded directly into the same colour
// 0->0, ... N-1->N-1 for some N
bool file_filter_direct(const FileFilter *fltr)
{
  size_t i = 0;
  while(i < file_filter_num(fltr) &&
        file_filter_fromcol(fltr, i) == i &&
        file_filter_intocol(fltr,i) == i) i++;
  return (i == file_filter_num(fltr));
}

// Returns true if the specifed filter `fltr` updates colour `col`
bool file_filter_iscolloaded(const FileFilter *fltr, size_t col)
{
  size_t i;
  for(i = 0; i < file_filter_num(fltr); i++) {
    if(file_filter_intocol(fltr, i) == col)
      return true;
  }
  return false;
}

// Print file filter description
void file_filter_status(const FileFilter *fltr, bool writing)
{
  size_t i;

  pthread_mutex_lock(&ctx_biglock);
  timestamp();
  message("[FileFilter] %s %s %s [%u src colour%s]",
          writing ? "Writing" : "Reading",
          strcmp(fltr->path.b,"")==0 ? "graph" : "file",
          file_filter_path(fltr),
          fltr->srcncols, util_plural_str(fltr->srcncols));

  if(!file_filter_direct(fltr))
  {
    message(" with filter: %u->%u", file_filter_fromcol(fltr, 0),
                                    file_filter_intocol(fltr, 0));

    for(i = 1; i < file_filter_num(fltr); i++)
      message(",%u->%u", file_filter_fromcol(fltr,i), file_filter_intocol(fltr,i));
  }
  message("\n");
  pthread_mutex_unlock(&ctx_biglock);
}

// Copy FileFilter src into dst
FileFilter* file_filter_copy(FileFilter *dst, const FileFilter *src)
{
  strbuf_set_buff(&dst->input, &src->input);
  strbuf_set_buff(&dst->path, &src->path);
  filter_buf_reset(&dst->filter);
  filter_buf_push(&dst->filter, src->filter.b, src->filter.len);
  dst->srcncols = src->srcncols;
  file_filter_update(dst);
  return dst;
}

// @intocols value to set all intocols to
void file_filter_flatten(FileFilter *fltr, size_t intocol)
{
  size_t i;
  ctx_assert(fltr->filter.b != NULL);
  for(i = 0; i < file_filter_num(fltr); i++)
    file_filter_intocol(fltr,i) = intocol;

  file_filter_update(fltr);
}

void file_filter_add(FileFilter *fltr, uint32_t from, uint32_t into)
{
  Filter f = {.from = from, .into = into};
  filter_buf_push(&fltr->filter, &f, 1);
  file_filter_update(fltr);
}

//
// Sort array of Filter structs
//

// Updates filter a using filter b (push a through b: a->b)
// Note: sorts filters in @b
void file_filter_merge(FileFilter *a, FileFilter *b)
{
  // Update a->intocols[i] = b->intocols[j] where a->intocols[i]==b->fromcols[j]
  // if no value of j such that a->intocols[i]==b->fromcols[j], remove j

  filters_sort_by_into(a->filter.b, file_filter_num(a));
  filters_sort_by_from(b->filter.b, file_filter_num(b));

  size_t i, j, k, n = file_filter_num(a), m = file_filter_num(b);

  // Append onto the end of a, then shift down
  for(i = j = 0; i < n; i++) {
    while(j < m && file_filter_intocol(a,i) > file_filter_fromcol(b,j)) j++;
    for(k = j; k < m && file_filter_intocol(a,i) == file_filter_fromcol(b,k); k++)
      file_filter_add(a, file_filter_fromcol(a,i), file_filter_intocol(b,k));
  }

  // Remove n items from the start of the array
  filter_buf_shift(&a->filter, NULL, n);
  file_filter_update(a);
}
