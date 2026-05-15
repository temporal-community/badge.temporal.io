// C-level native block device for MicroPython VFS FAT.
// Wraps ESP-IDF wear-levelling over the "ffat" flash partition so
// MicroPython's built-in VFS layer can mount a standard FAT filesystem.

// NO_QSTR is defined during the MicroPython QSTR generation step (gcc -E)
#ifndef NO_QSTR
#include <esp_partition.h>
#include <esp_system.h>
#include <wear_levelling.h>
#else
// Dummy types for QSTR generator
typedef int wl_handle_t;
typedef int esp_err_t;
typedef struct {
    unsigned int address;
    unsigned int size;
} esp_partition_t;
#define WL_INVALID_HANDLE -1
#define ESP_PARTITION_TYPE_DATA 0
#define ESP_PARTITION_SUBTYPE_DATA_FAT 0
#define ESP_OK 0
#endif

#include "extmod/vfs.h"
#include "extmod/vfs_fat.h"
#include "py/mphal.h"
#include "py/mperrno.h"
#include "py/obj.h"
#include "py/runtime.h"

// ── Flash block device state ────────────────────────────────────────────────

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static uint32_t s_sector_size = 0;
static uint32_t s_sector_count = 0;

// ── Native block device callbacks ───────────────────────────────────────────
// These are called directly by MicroPython's oofatfs disk I/O layer when
// MP_BLOCKDEV_FLAG_NATIVE is set.  Signature: int f(uint8_t*, uint32_t, uint32_t)

#ifndef NO_QSTR
static void yield_if_needed( void ) {
    static mp_uint_t s_last_yield_ms = 0;
    mp_uint_t now_ms = mp_hal_ticks_ms( );
    // VFS operations like f_mkfs can take many seconds on a 10MB partition.
    // Yield periodically to avoid starving background tasks/watchdogs.
    if ( now_ms - s_last_yield_ms > 200 ) {
        mp_hal_delay_ms( 10 );
        s_last_yield_ms = mp_hal_ticks_ms( );
    }
}
#endif

static mp_uint_t bdev_readblocks( uint8_t* dest, uint32_t block_num, uint32_t num_blocks ) {
#ifndef NO_QSTR
    yield_if_needed( );
    esp_err_t err = wl_read( s_wl_handle, (size_t)block_num * s_sector_size,
                             dest, (size_t)num_blocks * s_sector_size );
    return ( err == ESP_OK ) ? 0 : MP_EIO;
#else
    return MP_EIO;
#endif
}

static mp_uint_t bdev_writeblocks( const uint8_t* src, uint32_t block_num, uint32_t num_blocks ) {
#ifndef NO_QSTR
    yield_if_needed( );
    // Erase before write (FAT requires this for flash).
    size_t addr = (size_t)block_num * s_sector_size;
    size_t len = (size_t)num_blocks * s_sector_size;
    esp_err_t err = wl_erase_range( s_wl_handle, addr, len );
    if ( err != ESP_OK ) {
        return MP_EIO;
    }
    err = wl_write( s_wl_handle, addr, src, len );
    return ( err == ESP_OK ) ? 0 : MP_EIO;
#else
    return MP_EIO;
#endif
}

// ── MicroPython block device type ───────────────────────────────────────────
// Minimal Python-visible object so VfsFat can hold a reference to it.

typedef struct _replay_bdev_obj_t {
    mp_obj_base_t base;
} replay_bdev_obj_t;

// readblocks(block_num, buf) — Python-level fallback (not normally called with NATIVE flag)
static mp_obj_t bdev_readblocks_py( mp_obj_t self_in, mp_obj_t block_num_in, mp_obj_t buf_in ) {
    (void)self_in;
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise( buf_in, &bufinfo, MP_BUFFER_WRITE );
    uint32_t block_num = mp_obj_get_int( block_num_in );
    uint32_t num_blocks = bufinfo.len / s_sector_size;
    mp_uint_t ret = bdev_readblocks( (uint8_t*)bufinfo.buf, block_num, num_blocks );
    return MP_OBJ_NEW_SMALL_INT( ret == 0 ? 0 : -MP_EIO );
}
static MP_DEFINE_CONST_FUN_OBJ_3( bdev_readblocks_py_obj, bdev_readblocks_py );

// writeblocks(block_num, buf)
static mp_obj_t bdev_writeblocks_py( mp_obj_t self_in, mp_obj_t block_num_in, mp_obj_t buf_in ) {
    (void)self_in;
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise( buf_in, &bufinfo, MP_BUFFER_READ );
    uint32_t block_num = mp_obj_get_int( block_num_in );
    uint32_t num_blocks = bufinfo.len / s_sector_size;
    mp_uint_t ret = bdev_writeblocks( (const uint8_t*)bufinfo.buf, block_num, num_blocks );
    return MP_OBJ_NEW_SMALL_INT( ret == 0 ? 0 : -MP_EIO );
}
static MP_DEFINE_CONST_FUN_OBJ_3( bdev_writeblocks_py_obj, bdev_writeblocks_py );

// ioctl(cmd, arg)
static mp_obj_t bdev_ioctl_py( mp_obj_t self_in, mp_obj_t cmd_in, mp_obj_t arg_in ) {
    (void)self_in;
    (void)arg_in;
    int cmd = mp_obj_get_int( cmd_in );
    switch ( cmd ) {
    case MP_BLOCKDEV_IOCTL_INIT:
        return MP_OBJ_NEW_SMALL_INT( 0 ); // already initialised
    case MP_BLOCKDEV_IOCTL_DEINIT:
        return MP_OBJ_NEW_SMALL_INT( 0 );
    case MP_BLOCKDEV_IOCTL_SYNC:
        return MP_OBJ_NEW_SMALL_INT( 0 );
    case MP_BLOCKDEV_IOCTL_BLOCK_COUNT:
        return MP_OBJ_NEW_SMALL_INT( s_sector_count );
    case MP_BLOCKDEV_IOCTL_BLOCK_SIZE:
        return MP_OBJ_NEW_SMALL_INT( s_sector_size );
    case MP_BLOCKDEV_IOCTL_BLOCK_ERASE: {
#ifndef NO_QSTR
        uint32_t block = mp_obj_get_int( arg_in );
        esp_err_t err = wl_erase_range( s_wl_handle,
                                        (size_t)block * s_sector_size,
                                        s_sector_size );
        return MP_OBJ_NEW_SMALL_INT( err == ESP_OK ? 0 : -MP_EIO );
#else
        return MP_OBJ_NEW_SMALL_INT( -MP_EIO );
#endif
    }
    default:
        return mp_const_none;
    }
}
static MP_DEFINE_CONST_FUN_OBJ_3( bdev_ioctl_py_obj, bdev_ioctl_py );

static const mp_rom_map_elem_t bdev_locals_dict_table[] = {
    { MP_ROM_QSTR( MP_QSTR_readblocks ), MP_ROM_PTR( &bdev_readblocks_py_obj ) },
    { MP_ROM_QSTR( MP_QSTR_writeblocks ), MP_ROM_PTR( &bdev_writeblocks_py_obj ) },
    { MP_ROM_QSTR( MP_QSTR_ioctl ), MP_ROM_PTR( &bdev_ioctl_py_obj ) },
};
static MP_DEFINE_CONST_DICT( bdev_locals_dict, bdev_locals_dict_table );

static MP_DEFINE_CONST_OBJ_TYPE(
    replay_bdev_type,
    MP_QSTR_FlashBdev,
    MP_TYPE_FLAG_NONE,
    locals_dict, &bdev_locals_dict );

// Singleton instance.
static replay_bdev_obj_t s_bdev_obj = {
    .base = { &replay_bdev_type },
};

// ── Public API ──────────────────────────────────────────────────────────────

static FATFS* s_mounted_fatfs = NULL;

FATFS* replay_get_fatfs( void ) {
    return s_mounted_fatfs;
}

int replay_vfs_mount_fat( void ) {
#ifndef NO_QSTR
    // 1. Find the "ffat" partition.
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "ffat" );
    if ( part == NULL ) {
        mp_printf( &mp_plat_print, "[mpy] FAT partition 'ffat' not found\n" );
        return -MP_ENODEV;
    }
    mp_printf( &mp_plat_print, "[mpy] Found 'ffat' partition: offset=0x%x size=0x%x (%u KB)\n",
               (unsigned)part->address, (unsigned)part->size,
               (unsigned)( part->size / 1024 ) );

    // 2. Initialise wear-levelling on the partition.
    esp_err_t err = wl_mount( part, &s_wl_handle );
    if ( err != ESP_OK ) {
        mp_printf( &mp_plat_print, "[mpy] wl_mount failed: 0x%x\n", err );
        return -MP_EIO;
    }
    s_sector_size = wl_sector_size( s_wl_handle );
    s_sector_count = (uint32_t)( wl_size( s_wl_handle ) / s_sector_size );
    mp_printf( &mp_plat_print, "[mpy] Wear-levelling: %u sectors x %u bytes = %u KB\n",
               s_sector_count, s_sector_size,
               (unsigned)( (uint64_t)s_sector_count * s_sector_size / 1024 ) );

    // 3. Create the VfsFat filesystem object backed by our block device.
    //    This is equivalent to Python: vfs = VfsFat(bdev)
    fs_user_mount_t* vfs_fat = m_new_obj( fs_user_mount_t );
    vfs_fat->base.type = &mp_fat_vfs_type;
    vfs_fat->blockdev.flags = MP_BLOCKDEV_FLAG_NATIVE | MP_BLOCKDEV_FLAG_HAVE_IOCTL;
    vfs_fat->blockdev.block_size = s_sector_size;

    // Set up native read/write function pointers.
    // The VFS blockdev stores method+self in readblocks[0..2]:
    //   [0] = method obj (unused for native), [1] = self (unused for native),
    //   [2] = native function pointer
    vfs_fat->blockdev.readblocks[ 0 ] = MP_OBJ_FROM_PTR( &bdev_readblocks_py_obj );
    vfs_fat->blockdev.readblocks[ 1 ] = MP_OBJ_FROM_PTR( &s_bdev_obj );
    vfs_fat->blockdev.readblocks[ 2 ] = (mp_obj_t)(uintptr_t)bdev_readblocks;

    vfs_fat->blockdev.writeblocks[ 0 ] = MP_OBJ_FROM_PTR( &bdev_writeblocks_py_obj );
    vfs_fat->blockdev.writeblocks[ 1 ] = MP_OBJ_FROM_PTR( &s_bdev_obj );
    vfs_fat->blockdev.writeblocks[ 2 ] = (mp_obj_t)(uintptr_t)bdev_writeblocks;

    vfs_fat->blockdev.u.ioctl[ 0 ] = MP_OBJ_FROM_PTR( &bdev_ioctl_py_obj );
    vfs_fat->blockdev.u.ioctl[ 1 ] = MP_OBJ_FROM_PTR( &s_bdev_obj );

    // Connect the FATFS struct to our block device wrapper.
    // MicroPython's disk_get_device expects fs_user_mount_t* as the pdrv.
    vfs_fat->fatfs.drv = vfs_fat;

    // 4. Mount the FAT filesystem.
    mp_printf( &mp_plat_print, "[mpy] Registering f_mount...\n" );
    FRESULT res = f_mount( &vfs_fat->fatfs );

    // f_mount doesn't read the disk immediately. Use f_stat or f_getfree to force a check.
    mp_printf( &mp_plat_print, "[mpy] Probing filesystem...\n" );
    DWORD free_clusters;
    res = f_getfree( &vfs_fat->fatfs, &free_clusters );

    if ( res == FR_NO_FILESYSTEM ) {
        mp_printf( &mp_plat_print, "[mpy] No FAT filesystem found — formatting...\n" );
        // f_mkfs needs at least one cluster of working space. FF_MAX_SS (512) is too
        // small for FAT32 on a large partition — allocate a proper 4 KiB buffer on the heap.
        const size_t kWorkBufSize = 4096;
        uint8_t* working_buf = (uint8_t*)malloc( kWorkBufSize );
        if ( !working_buf ) {
            mp_printf( &mp_plat_print, "[mpy] f_mkfs: out of memory for working buffer\n" );
            return -MP_ENOMEM;
        }
        // FM_ANY: Let FatFS choose FAT12/16/32 based on partition size (10MB is too small for FAT32).
        // FM_SFD: Super-Floppy Disk format — no MBR/partition table, volume starts at sector 0.
        // Required for wear-levelled partitions which have no MBR header.
        res = f_mkfs( &vfs_fat->fatfs, FM_ANY | FM_SFD, 0, working_buf, kWorkBufSize );
        free( working_buf );
        if ( res != FR_OK ) {
            mp_printf( &mp_plat_print, "[mpy] f_mkfs failed: %d\n", res );
            return -MP_EIO;
        }
        // Remount after format.
        f_mount( NULL ); // Unmount
        res = f_mount( &vfs_fat->fatfs );
        if ( res == FR_OK ) {
            mp_printf( &mp_plat_print, "[mpy] Format complete. Re-checking free space...\n" );
            res = f_getfree( &vfs_fat->fatfs, &free_clusters );
        }
    }
    if ( res != FR_OK ) {
        mp_printf( &mp_plat_print, "[mpy] Mount/probe failed: %d\n", res );
        return -MP_EIO;
    }

    // 5. Register in MicroPython's VFS mount table at "/".
    mp_printf( &mp_plat_print, "[mpy] Registering VFS at /...\n" );
    mp_obj_t mount_point = MP_OBJ_NEW_QSTR( MP_QSTR__slash_ );
    mp_obj_t args[] = { MP_OBJ_FROM_PTR( vfs_fat ), mount_point };
    mp_map_t kw_args;
    mp_map_init( &kw_args, 0 );
    mp_vfs_mount( 2, args, &kw_args );
    mp_vfs_chdir( mount_point );

    s_mounted_fatfs = &vfs_fat->fatfs;
    mp_printf( &mp_plat_print, "[mpy] FAT filesystem mounted at /\n" );
    return 0;
#else
    return 0;
#endif
}

// ── Storage layout queries (used by the Firmware Update screen) ─────────────
//
// The badge can detect when the on-flash FAT volume is smaller than the
// partition that backs it (e.g. after we grew the ffat partition in a
// firmware update but the existing FAT header still describes the old
// 6 MB volume) and offer the user a one-tap "reformat to use all space"
// option. These helpers expose just enough state to make that comparison
// from C++ without dragging in the full ff.h.

size_t replay_bdev_partition_size( void ) {
#ifndef NO_QSTR
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "ffat" );
    return part ? part->size : 0;
#else
    return 0;
#endif
}

size_t replay_bdev_wl_size( void ) {
#ifndef NO_QSTR
    if ( s_wl_handle == WL_INVALID_HANDLE ) return 0;
    return (size_t)wl_size( s_wl_handle );
#else
    return 0;
#endif
}

size_t replay_bdev_fatfs_volume_bytes( void ) {
#ifndef NO_QSTR
    if ( !s_mounted_fatfs ) return 0;
    // Total cluster-addressable bytes (excludes reserved + FAT regions).
    // n_fatent is total FAT entries; clusters = n_fatent - 2 (entries 0,1
    // are reserved). csize is sectors per cluster.
    if ( s_mounted_fatfs->n_fatent <= 2 ) return 0;
    return (size_t)( s_mounted_fatfs->n_fatent - 2 )
         * (size_t)s_mounted_fatfs->csize
         * 512u;
#else
    return 0;
#endif
}

// True iff the currently mounted FAT volume is materially smaller than
// the wear-levelled partition area (>= 1 MB unused). Used to gate the
// "Reformat storage" option in the Firmware Update screen.
int replay_bdev_fatfs_is_undersized( void ) {
#ifndef NO_QSTR
    size_t wl = replay_bdev_wl_size( );
    size_t vol = replay_bdev_fatfs_volume_bytes( );
    if ( wl == 0 || vol == 0 ) return 0;
    if ( vol >= wl ) return 0;
    return ( wl - vol ) >= ( 1u * 1024u * 1024u );
#else
    return 0;
#endif
}

// Unmount, reformat the FAT volume across the full wear-levelled area,
// then reboot. Wipes everything on /. Returns 0 on success (in which
// case it doesn't return — esp_restart was called); negative on error.
//
// Caller is responsible for warning the user before invoking this; it's
// destructive and synchronous (a few seconds at minimum).
int replay_bdev_reformat_and_reboot( void ) {
#ifndef NO_QSTR
    if ( !s_mounted_fatfs ) {
        mp_printf( &mp_plat_print, "[mpy] reformat: no mounted fs\n" );
        return -MP_EIO;
    }
    mp_printf( &mp_plat_print, "[mpy] reformat: starting (this wipes /)...\n" );

    // Snapshot the FATFS pointer; f_mount(NULL) clears the global mount
    // slot but f_mkfs needs a valid fatfs struct to drive the bdev.
    FATFS* fs = s_mounted_fatfs;
    f_mount( NULL );

    const size_t kWorkBufSize = 4096;
    uint8_t* working_buf = (uint8_t*)malloc( kWorkBufSize );
    if ( !working_buf ) {
        mp_printf( &mp_plat_print, "[mpy] reformat: OOM\n" );
        return -MP_ENOMEM;
    }
    FRESULT res = f_mkfs( fs, FM_ANY | FM_SFD, 0, working_buf, kWorkBufSize );
    free( working_buf );
    if ( res != FR_OK ) {
        mp_printf( &mp_plat_print, "[mpy] reformat: f_mkfs failed %d\n", res );
        return -MP_EIO;
    }
    mp_printf( &mp_plat_print, "[mpy] reformat: done; rebooting\n" );
    // Give the serial line a moment to flush.
    mp_hal_delay_ms( 300 );
    esp_restart( );
    return 0;  // unreachable
#else
    return -MP_EIO;
#endif
}
