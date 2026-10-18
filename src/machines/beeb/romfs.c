/*
 * newlib file system calls over the files built into the firmware (and disc
 * images in RAM), so that beebjit's util_file_* (stdio) can open ROMs and
 * discs unchanged. Read only.
 * Handles 0-2 stay with the SDK's stdio (USB serial).
 *
 * Part of the RP2350 port of beebjit. beebjit is copyright Chris Evans and
 * licensed under the GNU GPL v3; this file is under the same licence.
 */
#include "romfs.h"

#include "pico/stdio.h"
#include "pico/time.h"

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

enum {
  k_romfs_first_fd = 3,
  k_romfs_max_open = 8,
};

struct romfs_open {
  const struct romfs_file* p_file;
  uint32_t pos;
};

static struct romfs_open s_open[k_romfs_max_open];

enum {
  k_romfs_max_ram_files = 2,
};

static struct romfs_file s_ram_files[k_romfs_max_ram_files];
static char s_ram_names[k_romfs_max_ram_files][32];
static uint8_t s_ram_writeable[k_romfs_max_ram_files];
static uint32_t s_ram_writes;

static void
ram_file_set(const char* p_name, const uint8_t* p_data, uint32_t size,
             int writeable) {
  unsigned i;
  unsigned free_i = k_romfs_max_ram_files;
  for (i = 0; i < k_romfs_max_ram_files; ++i) {
    if (s_ram_files[i].p_name == NULL) {
      if (free_i == k_romfs_max_ram_files) {
        free_i = i;
      }
    } else if (strcmp(s_ram_files[i].p_name, p_name) == 0) {
      free_i = i;
      break;
    }
  }
  if (free_i == k_romfs_max_ram_files) {
    free_i = 0;
  }
  (void) snprintf(s_ram_names[free_i], sizeof(s_ram_names[free_i]), "%s",
                  p_name);
  s_ram_files[free_i].p_name = s_ram_names[free_i];
  s_ram_files[free_i].p_data = p_data;
  s_ram_files[free_i].size = size;
  s_ram_writeable[free_i] = (uint8_t) writeable;
}

void
romfs_set_ram_file(const char* p_name, const uint8_t* p_data, uint32_t size) {
  ram_file_set(p_name, p_data, size, 0);
}

void
romfs_set_ram_file_writeable(const char* p_name,
                             uint8_t* p_data,
                             uint32_t size) {
  ram_file_set(p_name, p_data, size, 1);
}

uint32_t
romfs_ram_writes(void) {
  return s_ram_writes;
}

static int
is_writeable(const struct romfs_file* p_file) {
  unsigned i;
  for (i = 0; i < k_romfs_max_ram_files; ++i) {
    if (p_file == &s_ram_files[i]) {
      return s_ram_writeable[i];
    }
  }
  return 0;
}

const struct romfs_file*
romfs_find_builtin(const char* p_name) {
  unsigned i;
  for (i = 0; i < romfs_num_files; ++i) {
    if (strcmp(romfs_files[i].p_name, p_name) == 0) {
      return &romfs_files[i];
    }
  }
  return NULL;
}

static const struct romfs_file*
romfs_find(const char* p_name) {
  unsigned i;
  for (i = 0; i < k_romfs_max_ram_files; ++i) {
    if ((s_ram_files[i].p_name != NULL) &&
        (strcmp(s_ram_files[i].p_name, p_name) == 0)) {
      return &s_ram_files[i];
    }
  }
  return romfs_find_builtin(p_name);
}

static struct romfs_open*
romfs_get(int fd) {
  int i = (fd - k_romfs_first_fd);
  if ((i < 0) || (i >= k_romfs_max_open) || (s_open[i].p_file == NULL)) {
    return NULL;
  }
  return &s_open[i];
}

int
_open(const char* p_name, int flags, ...) {
  unsigned j;
  const struct romfs_file* p_file;
  p_file = romfs_find(p_name);
  if (p_file == NULL) {
    errno = ENOENT;
    return -1;
  }
  if (((flags & O_ACCMODE) != O_RDONLY) &&
      (!is_writeable(p_file) || (flags & (O_CREAT | O_TRUNC)))) {
    errno = EROFS;
    return -1;
  }
  for (j = 0; j < k_romfs_max_open; ++j) {
    if (s_open[j].p_file == NULL) {
      s_open[j].p_file = p_file;
      s_open[j].pos = 0;
      return (int) (j + k_romfs_first_fd);
    }
  }
  errno = EMFILE;
  return -1;
}

int
_close(int fd) {
  struct romfs_open* p_open = romfs_get(fd);
  if (p_open == NULL) {
    errno = EBADF;
    return -1;
  }
  p_open->p_file = NULL;
  return 0;
}

int
_read(int fd, char* p_buf, int len) {
  uint32_t left;
  struct romfs_open* p_open;
  if (fd == 0) {
    return stdio_get_until(p_buf, len, at_the_end_of_time);
  }
  p_open = romfs_get(fd);
  if (p_open == NULL) {
    errno = EBADF;
    return -1;
  }
  left = (p_open->p_file->size - p_open->pos);
  if ((uint32_t) len > left) {
    len = (int) left;
  }
  (void) memcpy(p_buf, (p_open->p_file->p_data + p_open->pos), len);
  p_open->pos += len;
  return len;
}

int
_write(int fd, const char* p_buf, int len) {
  struct romfs_open* p_open;
  uint32_t left;
  if ((fd == 1) || (fd == 2)) {
    stdio_put_string(p_buf, len, false, true);
    return len;
  }
  p_open = romfs_get(fd);
  if ((p_open == NULL) || !is_writeable(p_open->p_file)) {
    errno = EBADF;
    return -1;
  }
  left = ((p_open->pos < p_open->p_file->size) ?
          (p_open->p_file->size - p_open->pos) : 0);
  if ((uint32_t) len > left) {
    errno = ENOSPC;
    return -1;
  }
  (void) memcpy(((uint8_t*) p_open->p_file->p_data + p_open->pos), p_buf, len);
  p_open->pos += len;
  s_ram_writes++;
  return len;
}

off_t
_lseek(int fd, off_t pos, int whence) {
  int32_t new_pos;
  struct romfs_open* p_open = romfs_get(fd);
  if (p_open == NULL) {
    errno = EBADF;
    return -1;
  }
  switch (whence) {
  case SEEK_SET:
    new_pos = pos;
    break;
  case SEEK_CUR:
    new_pos = (p_open->pos + pos);
    break;
  case SEEK_END:
    new_pos = (p_open->p_file->size + pos);
    break;
  default:
    errno = EINVAL;
    return -1;
  }
  if (new_pos < 0) {
    errno = EINVAL;
    return -1;
  }
  p_open->pos = new_pos;
  return new_pos;
}

int
_fstat(int fd, struct stat* p_stat) {
  struct romfs_open* p_open;
  (void) memset(p_stat, '\0', sizeof(*p_stat));
  if (fd <= 2) {
    p_stat->st_mode = S_IFCHR;
    return 0;
  }
  p_open = romfs_get(fd);
  if (p_open == NULL) {
    errno = EBADF;
    return -1;
  }
  p_stat->st_mode = S_IFREG;
  p_stat->st_size = p_open->p_file->size;
  return 0;
}
