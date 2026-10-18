/*
 * Read-only files built into the firmware (ROM images, discs), and files in
 * RAM, opened with fopen() through newlib's _open/_read/_write/_lseek/_close
 * (see romfs.c).
 *
 * Part of the RP2350 port of beebjit. beebjit is copyright Chris Evans and
 * licensed under the GNU GPL v3; this file is under the same licence.
 */
#ifndef PRESTO_BEEBJIT_ROMFS_H
#define PRESTO_BEEBJIT_ROMFS_H

#include <stdint.h>

struct romfs_file {
  const char* p_name;
  const uint8_t* p_data;
  uint32_t size;
};

/* Generated from the CMake file list (embed_files.cmake). */
extern const struct romfs_file romfs_files[];
extern const unsigned romfs_num_files;

/* A file in RAM (a disc image from the SD card or the web), opened like the
 * built-in ones and found before them; a later one of the same name replaces
 * it (files already open see the new data). p_data stays the caller's.
 */
void romfs_set_ram_file(const char* p_name,
                        const uint8_t* p_data,
                        uint32_t size);
/* The same, writeable: opened for writing, writes land in p_data (within its
 * size), and each one counts (romfs_ram_writes): drive 0's disc, whose writes
 * the save disc keeps.
 */
void romfs_set_ram_file_writeable(const char* p_name,
                                  uint8_t* p_data,
                                  uint32_t size);
uint32_t romfs_ram_writes(void);
/* The built-in file of that name, or NULL. */
const struct romfs_file* romfs_find_builtin(const char* p_name);

#endif
