#ifdef ESP_PLATFORM

#include "doom_esp32_file.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "lib/oofatfs/ff.h"

extern FATFS* replay_get_fatfs(void);

typedef struct doom_file {
    FIL fil;
    int is_open;
} doom_file_t;

#define MAX_DOOM_FILES 8
static doom_file_t s_files[MAX_DOOM_FILES];

static const char* strip_root(const char* path) {
    if (path && path[0] == '/' && path[1] != '\0') return path + 1;
    return path;
}

static BYTE translate_mode(const char* mode) {
    if (!mode) return 0;
    if (strcmp(mode, "rb") == 0 || strcmp(mode, "r") == 0) return FA_READ;
    if (strcmp(mode, "wb") == 0 || strcmp(mode, "w") == 0) return FA_WRITE | FA_CREATE_ALWAYS;
    if (strcmp(mode, "r+b") == 0 || strcmp(mode, "rb+") == 0) return FA_READ | FA_WRITE;
    return FA_READ;
}

doom_file_t* doom_fopen(const char* path, const char* mode) {
    FATFS* fs = replay_get_fatfs();
    if (!fs || !path) return NULL;

    doom_file_t* f = NULL;
    for (int i = 0; i < MAX_DOOM_FILES; i++) {
        if (!s_files[i].is_open) { f = &s_files[i]; break; }
    }
    if (!f) return NULL;

    memset(f, 0, sizeof(*f));
    const char* fat_path = strip_root(path);
    FRESULT res = f_open(fs, &f->fil, fat_path, translate_mode(mode));
    if (res != FR_OK) {
        printf("[doom_fopen] FAIL '%s' fr=%d\n", fat_path, (int)res);
        return NULL;
    }

    f->is_open = 1;
    printf("[doom_fopen] OK '%s' (%lu bytes)\n", fat_path, (unsigned long)f_size(&f->fil));
    return f;
}

int doom_fclose(doom_file_t* f) {
    if (!f || !f->is_open) return -1;
    f_close(&f->fil);
    f->is_open = 0;
    return 0;
}

size_t doom_fread(void* buf, size_t size, size_t count, doom_file_t* f) {
    if (!f || !f->is_open || !buf) return 0;
    UINT total = (UINT)(size * count);
    UINT got = 0;
    FRESULT res = f_read(&f->fil, buf, total, &got);
    if (res != FR_OK) return 0;
    return (size > 0) ? (size_t)(got / size) : 0;
}

size_t doom_fwrite(const void* buf, size_t size, size_t count, doom_file_t* f) {
    if (!f || !f->is_open || !buf) return 0;
    UINT total = (UINT)(size * count);
    UINT written = 0;
    FRESULT res = f_write(&f->fil, buf, total, &written);
    if (res != FR_OK) return 0;
    return (size > 0) ? (size_t)(written / size) : 0;
}

int doom_fseek(doom_file_t* f, long offset, int whence) {
    if (!f || !f->is_open) return -1;
    FSIZE_t pos;
    switch (whence) {
        case SEEK_SET: pos = (FSIZE_t)offset; break;
        case SEEK_CUR: pos = f_tell(&f->fil) + offset; break;
        case SEEK_END: pos = f_size(&f->fil) + offset; break;
        default: return -1;
    }
    return (f_lseek(&f->fil, pos) == FR_OK) ? 0 : -1;
}

long doom_ftell(doom_file_t* f) {
    if (!f || !f->is_open) return -1;
    return (long)f_tell(&f->fil);
}

long doom_flength(doom_file_t* f) {
    if (!f || !f->is_open) return -1;
    return (long)f_size(&f->fil);
}

int doom_fexists(const char* path) {
    FATFS* fs = replay_get_fatfs();
    if (!fs || !path) return 0;
    FILINFO fno;
    return (f_stat(fs, strip_root(path), &fno) == FR_OK) ? 1 : 0;
}

int doom_fremove(const char* path) {
    FATFS* fs = replay_get_fatfs();
    if (!fs || !path) return -1;
    return (f_unlink(fs, strip_root(path)) == FR_OK) ? 0 : -1;
}

int doom_frename(const char* oldpath, const char* newpath) {
    FATFS* fs = replay_get_fatfs();
    if (!fs || !oldpath || !newpath) return -1;
    return (f_rename(fs, strip_root(oldpath), strip_root(newpath)) == FR_OK) ? 0 : -1;
}

int doom_mkdir(const char* path) {
    FATFS* fs = replay_get_fatfs();
    if (!fs || !path) return -1;
    FRESULT res = f_mkdir(fs, strip_root(path));
    return (res == FR_OK || res == FR_EXIST) ? 0 : -1;
}

void doom_fclose_all(void) {
    for (int i = 0; i < MAX_DOOM_FILES; i++) {
        if (s_files[i].is_open) {
            f_close(&s_files[i].fil);
            s_files[i].is_open = 0;
        }
    }
}

#endif // ESP_PLATFORM
