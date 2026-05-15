// Minimal MicroPython HAL port for Replay badge.
// Provides stdio hooks that call into MicroPythonBridge.cpp for actual Serial I/O.

#include "py/mphal.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "py/ringbuf.h"
#include "esp_system.h"
#include "esp_random.h"
#include <string.h>
#include <sys/time.h>

static uint8_t stdin_ringbuf_array[260];
ringbuf_t stdin_ringbuf = { stdin_ringbuf_array, sizeof( stdin_ringbuf_array ), 0, 0 };

// Implemented in MicroPythonBridge.cpp (C++ side, has access to Arduino Serial).
extern void mpy_hal_stdout_write( const char* str, size_t len );
extern int mpy_hal_stdin_read( void );
extern int mpy_hal_stdin_available( void );
extern void mpy_hal_delay_ms( unsigned int ms );
extern unsigned long mpy_hal_ticks_ms( void );

// ── Interrupt character storage ─────────────────────────────────────────────

int mp_interrupt_char = -1;

void mp_hal_set_interrupt_char( int c ) {
    mp_interrupt_char = c;
}

// ── stdout ──────────────────────────────────────────────────────────────────

mp_uint_t mp_hal_stdout_tx_strn( const char* str, size_t len ) {
    mpy_hal_stdout_write( str, len );
    return len;
}

void mp_hal_stdout_tx_strn_cooked( const char* str, size_t len ) {
    // Expand \n -> \r\n for terminal display.
    const char* p = str;
    size_t remaining = len;
    while ( remaining > 0 ) {
        const char* nl = p;
        const char* end = p + remaining;
        while ( nl < end && *nl != '\n' )
            ++nl;

        size_t chunk = (size_t)( nl - p );
        if ( chunk > 0 )
            mpy_hal_stdout_write( p, chunk );

        if ( nl < end ) {
            mpy_hal_stdout_write( "\r\n", 2 );
            remaining -= chunk + 1;
            p = nl + 1;
        } else {
            break;
        }
    }
}

void mp_hal_stdout_tx_str( const char* str ) {
    mp_hal_stdout_tx_strn( str, strlen( str ) );
}

// ── stdin ───────────────────────────────────────────────────────────────────

uintptr_t mp_hal_stdio_poll( uintptr_t poll_flags ) {
    uintptr_t ret = 0;
    if ( ( poll_flags & MP_STREAM_POLL_RD ) &&
         ( ringbuf_peek( &stdin_ringbuf ) != -1 || mpy_hal_stdin_available( ) ) ) {
        ret |= MP_STREAM_POLL_RD;
    }
    if ( poll_flags & MP_STREAM_POLL_WR ) {
        ret |= MP_STREAM_POLL_WR; // stdout always writable
    }
    return ret;
}

int mp_hal_stdin_rx_chr( void ) {
    for ( ;; ) {
        int c = ringbuf_get( &stdin_ringbuf );
        if ( c != -1 ) {
            return c;
        }
        c = mpy_hal_stdin_read( );
        if ( c >= 0 ) {
            return c;
        }
        mpy_hal_delay_ms( 1 );
    }
}

// ── Timing ──────────────────────────────────────────────────────────────────

void mp_hal_delay_ms( mp_uint_t ms ) {
    mpy_hal_delay_ms( (unsigned int)ms );
}

void mp_hal_delay_us( mp_uint_t us ) {
    // For sub-ms delays, spin. For longer, delegate to ms delay.
    if ( us >= 1000 ) {
        mpy_hal_delay_ms( (unsigned int)( us / 1000 ) );
    }
    // Remaining sub-ms portion is dropped (acceptable for embedded).
}

mp_uint_t mp_hal_ticks_ms( void ) {
    return (mp_uint_t)mpy_hal_ticks_ms( );
}

mp_uint_t mp_hal_ticks_us( void ) {
    return mp_hal_ticks_ms( ) * 1000;
}

uint64_t mp_hal_time_ns( void ) {
    struct timeval tv;
    gettimeofday( &tv, NULL );
    return (uint64_t)tv.tv_sec * 1000000000ULL + (uint64_t)tv.tv_usec * 1000ULL;
}

// ── Random ───────────────────────────────────────────────────────────────────

void mp_hal_get_random( size_t n, uint8_t* buf ) {
    esp_fill_random( buf, n );
}

uint32_t replay_random_seed_init( void ) {
    uint32_t seed = 0;
    mp_hal_get_random( sizeof( seed ), (uint8_t*)&seed );
    return seed;
}

void mp_hal_wake_main_task( void ) {
}

void mp_hal_wake_main_task_from_isr( void ) {
}
