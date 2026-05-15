#ifndef DOOM_ESP32_FILE_H
#define DOOM_ESP32_FILE_H

#ifdef ESP_PLATFORM

#include <stddef.h>

typedef struct doom_file doom_file_t;

doom_file_t* doom_fopen(const char* path, const char* mode);
int          doom_fclose(doom_file_t* f);
size_t       doom_fread(void* buf, size_t size, size_t count, doom_file_t* f);
size_t       doom_fwrite(const void* buf, size_t size, size_t count, doom_file_t* f);
int          doom_fseek(doom_file_t* f, long offset, int whence);
long         doom_ftell(doom_file_t* f);
long         doom_flength(doom_file_t* f);
int          doom_fexists(const char* path);
int          doom_fremove(const char* path);
int          doom_frename(const char* oldpath, const char* newpath);
int          doom_mkdir(const char* path);
void         doom_fclose_all(void);

#endif // ESP_PLATFORM
#endif // DOOM_ESP32_FILE_H
