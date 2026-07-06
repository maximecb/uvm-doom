// PureDOOM front-end for UVM (https://github.com/maximecb/uvm).
//
// This wires PureDOOM's platform callbacks to UVM's host syscalls: the `window`
// subsystem for video and input, the `audio` subsystem for sound, C stdio
// (uvclang's <stdio.h>, backed by the `fs` syscalls) for file access, and the
// `time` subsystem for timing. It is meant
// to be built with uvclang and run on the UVM VM. There is no SDL, CoreAudio,
// or any other host-OS dependency here.
//
// Known limitations of the UVM backend (see the notes at each call site):
//   - Music is disabled: UVM has no MIDI synthesizer primitive, so the MIDI
//     code from the original SDL port is commented out below.
//   - UVM audio output is mono, 44100Hz, i16 only, so DOOM's 11025Hz stereo mix
//     is downmixed to mono and upsampled 4x in the audio callback.
//   - UVM has no relative-mouse / pointer-lock mode, so mouse look is derived
//     from absolute cursor deltas and stops at the window edges.
//   - UVM exposes no CTRL/ALT/function keys, so use the mouse buttons to fire
//     and the number keys to switch weapons.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define DOOM_IMPLEMENTATION
#include "PureDOOM.h"

// The UVM headers are included *after* PureDOOM.h on purpose. PureDOOM.h defines
// its own KEY_* macros (e.g. KEY_BACKSPACE) for DOOM's internal keymap; including
// <uvm/syscalls.h> afterwards makes UVM's KEY_* values win for the input-mapping
// code below, while PureDOOM's implementation above keeps its own.
#include <uvm/syscalls.h>
#include <uvm/window.h>
#include <pthread.h>

// Resolution DOOM renders at. This is fixed: PureDOOM's renderer is hardwired
// to 320x200 (doom_set_resolution is a no-op), so we always render at this size.
#define WIDTH 320
#define HEIGHT 200

// Integer upscale factor. UVM's window_draw_frame requires the frame to exactly
// match the window size (there is no GPU stretch), so we nearest-neighbor
// upscale into a WIN_WIDTH x WIN_HEIGHT buffer ourselves. 3x gives 960x600.
#define SCALE 3
#define WIN_WIDTH (WIDTH * SCALE)
#define WIN_HEIGHT (HEIGHT * SCALE)

// Id of the window we draw into (window_create returns 0 for the first window).
static uint32_t g_window_id = 0;

// Upscaled, byte-swapped frame handed to window_draw_frame each frame.
static uint32_t g_win_pixels[WIN_HEIGHT * WIN_WIDTH];

static int g_quit = 0; // Set when the user closes the window or DOOM asks to quit

// Last absolute mouse position, used to derive relative motion for look
// controls. -1 means "no previous position yet".
static int g_last_mouse_x = -1;
static int g_last_mouse_y = -1;

void doom_exit_override(int code)
{
    (void)code;
    // DOOM wants to quit (e.g. from the menu). Flag the main loop to stop
    // instead of calling exit(), so we get a chance to shut down cleanly.
    g_quit = 1;
}

void* vm_malloc(int size)
{
    return malloc(size);
}

void vm_free(void* ptr)
{
    free(ptr);
}

void vm_print(const char* str)
{
    print_str(str);
}

//-----------------------------------------------------------------------------
// File I/O, via uvclang's C stdio (<stdio.h>), which wraps UVM's `fs` syscalls
// and provides the FILE* stream, SEEK_CUR/SEEK_END, and feof() semantics DOOM
// expects. DOOM's opaque file handle is just a FILE*.
//-----------------------------------------------------------------------------

// Needed to load doom1.wad, default.cfg, and savegames.
void* vm_open(const char* filename, const char* mode)
{
    return (void*)fopen(filename, mode);
}

void vm_close(void* handle)
{
    fclose((FILE*)handle);
}

int vm_read(void* handle, void* buf, int count)
{
    return (int)fread(buf, 1, count, (FILE*)handle);
}

int vm_write(void* handle, const void* buf, int count)
{
    return (int)fwrite(buf, 1, count, (FILE*)handle);
}

int vm_seek(void* handle, int offset, doom_seek_t origin)
{
    int whence = SEEK_SET;
    switch (origin)
    {
        case DOOM_SEEK_SET: whence = SEEK_SET; break;
        case DOOM_SEEK_CUR: whence = SEEK_CUR; break;
        case DOOM_SEEK_END: whence = SEEK_END; break;
    }
    return fseek((FILE*)handle, offset, whence);
}

int vm_tell(void* handle)
{
    return (int)ftell((FILE*)handle);
}

int vm_eof(void* handle)
{
    return feof((FILE*)handle);
}

// Needed for frame pacing (doom_update()'s internal 35Hz throttle) and as a
// random seed.
void vm_gettime(int* sec, int* usec)
{
    uint64_t ms = time_current_ms();
    *sec = (int)(ms / 1000);
    *usec = (int)((ms % 1000) * 1000);
}

//-----------------------------------------------------------------------------
// Input: translate UVM window events into DOOM key/button/motion events.
//-----------------------------------------------------------------------------

// Translate a UVM key code (see <uvm/syscalls.h>) to a DOOM key. UVM exposes a
// smaller key set than SDL did: there is no CTRL, ALT, or function-key code, so
// those DOOM bindings are simply unreachable from the keyboard here.
static doom_key_t uvm_key_to_doom_key(uint16_t key)
{
    switch (key)
    {
        case KEY_TAB:       return DOOM_KEY_TAB;
        case KEY_RETURN:    return DOOM_KEY_ENTER;
        case KEY_ESCAPE:    return DOOM_KEY_ESCAPE;
        case KEY_SPACE:     return DOOM_KEY_SPACE;
        case KEY_COMMA:     return DOOM_KEY_COMMA;
        case KEY_PERIOD:    return DOOM_KEY_PERIOD;
        case KEY_SLASH:     return DOOM_KEY_SLASH;
        case KEY_SEMICOLON: return DOOM_KEY_SEMICOLON;
        case KEY_EQUALS:    return DOOM_KEY_EQUALS;
        case KEY_BACKSPACE: return DOOM_KEY_BACKSPACE;
        case KEY_NUM0:      return DOOM_KEY_0;
        case KEY_NUM1:      return DOOM_KEY_1;
        case KEY_NUM2:      return DOOM_KEY_2;
        case KEY_NUM3:      return DOOM_KEY_3;
        case KEY_NUM4:      return DOOM_KEY_4;
        case KEY_NUM5:      return DOOM_KEY_5;
        case KEY_NUM6:      return DOOM_KEY_6;
        case KEY_NUM7:      return DOOM_KEY_7;
        case KEY_NUM8:      return DOOM_KEY_8;
        case KEY_NUM9:      return DOOM_KEY_9;
        case KEY_LEFT:      return DOOM_KEY_LEFT_ARROW;
        case KEY_RIGHT:     return DOOM_KEY_RIGHT_ARROW;
        case KEY_UP:        return DOOM_KEY_UP_ARROW;
        case KEY_DOWN:      return DOOM_KEY_DOWN_ARROW;
        case KEY_SHIFT:     return DOOM_KEY_SHIFT;
        default: break;
    }

    // Letters A-Z map contiguously in both key spaces.
    if (key >= KEY_A && key <= KEY_Z)
        return (doom_key_t)(DOOM_KEY_A + (key - KEY_A));

    return DOOM_KEY_UNKNOWN;
}

// UVM mouse buttons: 0 = left, 1 = middle, 2 = right (see vm/src/window.rs).
static doom_button_t uvm_button_to_doom_button(uint16_t button)
{
    switch (button)
    {
        case 0: return DOOM_LEFT_BUTTON;
        case 1: return DOOM_MIDDLE_BUTTON;
        case 2: return DOOM_RIGHT_BUTTON;
    }
    return (doom_button_t)0;
}

// Drain all pending UVM window events and feed them to DOOM. DOOM queues these
// and consumes them on the next doom_update().
void vm_poll_input(void)
{
    while (window_poll_event(&__event__))
    {
        switch (__event__.kind)
        {
            case EVENT_QUIT:
                g_quit = 1;
                break;

            case EVENT_KEYDOWN:
                doom_key_down(uvm_key_to_doom_key(__event__.key));
                break;

            case EVENT_KEYUP:
                doom_key_up(uvm_key_to_doom_key(__event__.key));
                break;

            case EVENT_MOUSEDOWN:
                doom_button_down(uvm_button_to_doom_button(__event__.button));
                break;

            case EVENT_MOUSEUP:
                doom_button_up(uvm_button_to_doom_button(__event__.button));
                break;

            case EVENT_MOUSEMOVE:
            {
                // UVM reports absolute cursor positions; DOOM wants relative
                // motion. Derive a delta from the previous position. There is no
                // pointer lock, so turning is bounded by the window edges.
                if (g_last_mouse_x >= 0)
                {
                    int dx = __event__.x - g_last_mouse_x;
                    int dy = __event__.y - g_last_mouse_y;
                    if (dx || dy)
                        doom_mouse_move(dx * 4, dy * 4);
                }
                g_last_mouse_x = __event__.x;
                g_last_mouse_y = __event__.y;
                break;
            }
        }
    }
}

//-----------------------------------------------------------------------------
// Video: upscale DOOM's 320x200 RGBA frame into our BGRA window buffer.
//-----------------------------------------------------------------------------

// `framebuffer` is width*height*4 bytes. DOOM's byte order is RGBA (R at the
// lowest address); UVM's window_draw_frame wants BGRA (B at the lowest address),
// so we swap the R and B bytes of every pixel while nearest-neighbor upscaling
// by SCALE.
void vm_present_frame(const unsigned char* framebuffer, int width, int height)
{
    const uint32_t* src = (const uint32_t*)framebuffer;

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            // DOOM word is 0xAABBGGRR (RGBA bytes); swap R and B to get the
            // 0xAARRGGBB (BGRA bytes) that UVM expects.
            uint32_t p = src[y * width + x];
            uint32_t c = (p & 0xFF00FF00u)
                       | ((p & 0x000000FFu) << 16)
                       | ((p & 0x00FF0000u) >> 16);

            // Splat this source pixel into its SCALE x SCALE destination block.
            for (int dy = 0; dy < SCALE; ++dy)
            {
                uint32_t* row = &g_win_pixels[(y * SCALE + dy) * WIN_WIDTH + x * SCALE];
                for (int dx = 0; dx < SCALE; ++dx)
                    row[dx] = c;
            }
        }
    }

    window_draw_frame(g_window_id, (const uint8_t*)g_win_pixels);
}

//-----------------------------------------------------------------------------
// Audio: bridge DOOM's 11025Hz stereo mix to UVM's 44100Hz mono output.
//
// UVM's audio output only supports 44100Hz, mono, i16, with a fixed 1024-sample
// callback block. DOOM produces 512 stereo frames per doom_get_sound_buffer()
// call at 11025Hz. We downmix each stereo frame to mono and upsample 4x
// (44100 / 11025 == 4 exactly) by sample replication, buffering one DOOM block
// (512 * 4 = 2048 mono samples) and serving it 1024 samples at a time.
//-----------------------------------------------------------------------------

#define DOOM_FRAMES 512               // stereo frames per doom_get_sound_buffer()
#define UPSAMPLE 4                    // 44100 / 11025
#define MIX_SAMPLES (DOOM_FRAMES * UPSAMPLE) // 2048 mono samples @ 44100Hz

// doom_get_sound_buffer() and doom_update() both touch DOOM's sound state, and
// the audio callback runs on its own UVM thread, so serialize them with a lock.
static pthread_mutex_t g_doom_lock = PTHREAD_MUTEX_INITIALIZER;

// Set once doom_init() has run, so the audio thread never touches DOOM state
// before it exists.
static volatile int g_audio_ready = 0;

static int16_t g_mix[MIX_SAMPLES];    // upsampled mono samples from one DOOM block
static int g_mix_pos = MIX_SAMPLES;   // read cursor; == MIX_SAMPLES forces a refill
static int16_t g_out[1024];           // buffer returned to UVM (fixed 1024 block)

// Called by UVM on a dedicated audio thread to fill a block of output samples.
int16_t* audio_cb(uint64_t num_channels, uint64_t num_samples)
{
    (void)num_channels; // always 1 (UVM output is mono)

    if (!g_audio_ready)
    {
        memset((uint8_t*)g_out, 0, sizeof(g_out));
        return g_out;
    }

    for (uint64_t i = 0; i < num_samples; ++i)
    {
        if (g_mix_pos >= MIX_SAMPLES)
        {
            // Pull the next 512-frame stereo block from DOOM, downmix to mono
            // and upsample 4x into g_mix.
            pthread_mutex_lock(&g_doom_lock);
            int16_t* db = doom_get_sound_buffer();
            for (int f = 0; f < DOOM_FRAMES; ++f)
            {
                int32_t mono = ((int32_t)db[2 * f] + (int32_t)db[2 * f + 1]) / 2;
                for (int u = 0; u < UPSAMPLE; ++u)
                    g_mix[f * UPSAMPLE + u] = (int16_t)mono;
            }
            pthread_mutex_unlock(&g_doom_lock);
            g_mix_pos = 0;
        }

        g_out[i] = g_mix[g_mix_pos++];
    }

    return g_out;
}

//-----------------------------------------------------------------------------
// Music (MIDI) — DISABLED on UVM.
//
// The original SDL port turned DOOM's MIDI stream into music using a
// platform software synth (CoreAudio's DLS synth on macOS). UVM has no MIDI
// synthesizer primitive, so there is nothing to forward the MIDI stream to.
// The code is kept here, commented out, for reference / a future UVM MIDI path.
//-----------------------------------------------------------------------------
#if 0
void send_midi_msg(uint32_t midi_msg) { (void)midi_msg; }
void setup_midi(void) {}

// Would be called ~140 times/sec to drain DOOM's pending MIDI messages:
//   unsigned long midi_msg;
//   while ((midi_msg = doom_tick_midi()) != 0)
//       send_midi_msg((uint32_t)midi_msg);
#endif

int main(int argc, char** argv)
{
    // Bring up the window. It stays hidden until the first frame is drawn.
    g_window_id = window_create(WIN_WIDTH, WIN_HEIGHT, "PureDOOM - UVM", 0);

    // Wire up DOOM's platform callbacks.
    doom_set_exit(doom_exit_override);
    doom_set_malloc(vm_malloc, vm_free);
    doom_set_print(vm_print);
    doom_set_file_io(vm_open, vm_close, vm_read, vm_write, vm_seek, vm_tell, vm_eof);
    doom_set_gettime(vm_gettime);

    doom_set_resolution(WIDTH, HEIGHT);
    doom_init(argc, argv, DOOM_FLAG_MENU_DARKEN_BG);

    // Only now that DOOM is initialized is it safe for the audio thread to call
    // into it. Open the mono 44100Hz output, which spawns the audio callback
    // thread. (Music/MIDI is disabled; see the block above.)
    g_audio_ready = 1;
    audio_open_output(44100, 1, AUDIO_FORMAT_I16, (void*)audio_cb);

    // Target ~60 FPS. doom_update() additionally self-throttles game logic to
    // DOOM's native 35Hz tic rate.
    const uint64_t frame_ms = 1000 / 60;

    while (!g_quit)
    {
        uint64_t start = time_current_ms();

        vm_poll_input();

        // The audio thread also calls into DOOM's sound code; hold the lock
        // around doom_update() so the two never run concurrently.
        pthread_mutex_lock(&g_doom_lock);
        doom_update();
        pthread_mutex_unlock(&g_doom_lock);

        vm_present_frame(doom_get_framebuffer(4), WIDTH, HEIGHT);

        uint64_t elapsed = time_current_ms() - start;
        if (elapsed < frame_ms)
            thread_sleep(frame_ms - elapsed);
    }

    return 0;
}
