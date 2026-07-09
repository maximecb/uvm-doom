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
//   - Music: UVM has no MIDI synthesizer primitive, so DOOM's MIDI stream is
//     rendered by our own small GM software synth (see synth.c).
//   - UVM audio output is 44100Hz, i16 only, so DOOM's 11025Hz stereo mix is
//     upsampled 4x (keeping left/right separation) in the audio callback.
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

// Upscaled BGRA frame handed to window_draw_frame each frame.
static uint32_t g_win_pixels[WIN_HEIGHT * WIN_WIDTH];

static int g_quit = 0; // Set when the user closes the window or DOOM asks to quit

// Set when running a -timedemo benchmark. In this mode there is no window, so
// vm_present_frame() still does the convert/upscale work (so it's measured)
// but skips the final window_draw_frame blit.
static int g_benchmark = 0;

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
// Video: convert + upscale DOOM's 320x200 indexed frame into our BGRA window
// buffer in a single pass.
//-----------------------------------------------------------------------------

// `indexed` is width*height bytes, one 8-bit palette index per pixel (the frame
// returned by doom_get_framebuffer(1), with the crosshair already drawn in). We
// fold the palette->BGRA conversion into the upscale: for each source pixel we
// look up its BGRA word once and splat it SCALE times horizontally, then copy
// the finished row down SCALE-1 times. This drops the intermediate 320x200 BGRA
// buffer (and its per-frame memcpy) that a separate conversion pass would need.
void vm_present_frame(const unsigned char* indexed, int width, int height)
{
    const uint32_t* lut = doom_get_bgra_lut();

    for (int y = 0; y < height; ++y)
    {
        // Build the first output row for this source row: convert each source
        // pixel through the LUT and splat it SCALE times horizontally.
        uint32_t* row0 = &g_win_pixels[(y * SCALE) * WIN_WIDTH];
        const unsigned char* srow = &indexed[y * width];
        uint32_t* dst = row0;
        for (int x = 0; x < width; ++x)
        {
            uint32_t c = lut[*srow++];
#if SCALE == 3
            // Unrolled horizontal splat for the fixed 3x scale (no inner loop
            // control per pixel).
            dst[0] = c;
            dst[1] = c;
            dst[2] = c;
#else
            for (int dx = 0; dx < SCALE; ++dx)
                dst[dx] = c;
#endif
            dst += SCALE;
        }

        // Vertical upscale: the other SCALE-1 rows are identical to row0, so
        // copy the finished row with a native memcpy instead of recomputing and
        // re-writing every pixel (which was SCALE-1 redundant passes).
        for (int dy = 1; dy < SCALE; ++dy)
            memcpy(&g_win_pixels[(y * SCALE + dy) * WIN_WIDTH],
                   row0, WIN_WIDTH * sizeof(uint32_t));
    }

    // In benchmark mode there is no window; we still ran the upscale above so
    // it's included in the timing, but there is nothing to blit to.
    if (!g_benchmark)
        window_draw_frame(g_window_id, (const uint8_t*)g_win_pixels);
}

//-----------------------------------------------------------------------------
// Music (MIDI): rendered by our own GM software synthesizer (synth.c).
//
// The original SDL port forwarded DOOM's MIDI stream to a platform synth
// (CoreAudio's DLS synth on macOS). UVM has no MIDI synthesizer primitive,
// so we synthesize the music ourselves: doom_tick_midi() is drained at
// DOOM's 140Hz MIDI rate from the audio callback (sample-accurately
// interleaved with rendering), and synth.c turns the messages into audio
// that is additively mixed over the SFX.
//
// synth.c is #included rather than compiled separately because the build
// hands uvclang a single translation unit (main.c), mirroring how PureDOOM.h
// itself is included.
//-----------------------------------------------------------------------------
#include "synth.c"

// Frames between 140Hz MIDI pumps (44100 / 140 == 315 exactly), and the
// count of frames left until the next pump. Only touched on the audio thread.
#define MIDI_PUMP_FRAMES (44100 / DOOM_MIDI_RATE)
static int g_midi_countdown = 0;

// Scratch schedule of MIDI events for one audio callback: message + the sample
// offset within the block where it should be applied. We drain DOOM's music
// stream into this array up front (each doom_tick_midi() briefly takes DOOM's
// internal g_music_lock), then do the synth work (applying events + rendering)
// entirely lock-free — see the music section of audio_cb(). Sized for the worst
// realistic burst: a song change drains the whole async queue
// (MAX_QUEUED_MIDI_MSGS == 256) plus a clutch of simultaneous note events, so
// give it generous headroom.
#define MAX_BLOCK_MIDI 512
static uint32_t g_block_midi_msg[MAX_BLOCK_MIDI];
static int g_block_midi_off[MAX_BLOCK_MIDI];

//-----------------------------------------------------------------------------
// Audio: bridge DOOM's 11025Hz stereo mix to UVM's 44100Hz stereo output.
//
// UVM's audio output supports 44100Hz, i16, with a fixed 1024-sample-per-channel
// callback block. DOOM produces 512 stereo frames per doom_get_sound_buffer()
// call at 11025Hz. We upsample 4x (44100 / 11025 == 4 exactly) by frame
// replication, preserving DOOM's left/right separation, buffering one DOOM block
// (512 * 4 = 2048 stereo frames) and serving it 1024 frames at a time. Output
// samples are interleaved L R L R, as UVM expects for stereo.
//-----------------------------------------------------------------------------

#define DOOM_FRAMES 512               // stereo frames per doom_get_sound_buffer()
#define UPSAMPLE 4                    // 44100 / 11025
#define MIX_FRAMES (DOOM_FRAMES * UPSAMPLE) // 2048 stereo frames @ 44100Hz

// Cross-thread synchronization between this audio callback thread and the game
// thread lives *inside* DOOM's sound layer now (PureDOOM.h's g_sfx_lock guards
// the SFX mixer, g_music_lock guards the MUS/MIDI sequencer). Each guards only
// its small shared data, so neither is held across doom_update()'s render. The
// callback therefore takes no lock of its own — it just calls into DOOM's sound
// functions, which lock briefly as needed.

// Set once doom_init() has run, so the audio thread never touches DOOM state
// before it exists.
static volatile int g_audio_ready = 0;

static int16_t g_mix[MIX_FRAMES * 2]; // upsampled interleaved L R frames from one DOOM block
static int g_mix_pos = MIX_FRAMES;    // read cursor in frames; == MIX_FRAMES forces a refill
static int16_t g_out[1024 * 2];       // buffer returned to UVM (interleaved L R, 1024-frame block)

// Called by UVM on a dedicated audio thread to fill a block of output samples.
// num_samples is the number of frames (samples per channel); output is
// interleaved across num_channels (2 for stereo).
int16_t* audio_cb(uint64_t num_channels, uint64_t num_samples)
{
    (void)num_channels; // always 2 (stereo output opened below)

    if (!g_audio_ready)
    {
        memset((uint8_t*)g_out, 0, sizeof(g_out));
        return g_out;
    }

    // Budget check: a 1024-frame block at 44100Hz is ~23.2ms of audio, so the
    // whole callback must finish well under that or the audio thread underruns
    // and the output glitches. Time the full callback, and separately tally the
    // time spent inside DOOM's sound/music calls (doom_get_sound_buffer and
    // doom_tick_midi) — the only places the callback can now block on the game
    // thread, since that's where DOOM's g_sfx_lock / g_music_lock live. That
    // split tells us whether an over-budget block was lost to cross-thread
    // contention or to our own synth/SFX compute.
    uint64_t cb_start_ms = time_current_ms();
    uint64_t doom_ms = 0;

    for (uint64_t i = 0; i < num_samples; ++i)
    {
        if (g_mix_pos >= MIX_FRAMES)
        {
            // Pull the next 512-frame stereo block from DOOM and upsample 4x
            // into g_mix, keeping the left and right channels separate.
            // PureDOOM's mixer level is restored to the engine's intended
            // scale in I_StartSound (see the [UVM] note there); on top of
            // that, boost 2x with saturation so SFX sit clearly above the
            // music synth (point-blank blasts will clip a little; fine).
            // doom_get_sound_buffer() runs the SFX mixer under g_sfx_lock.
            uint64_t doom_t0 = time_current_ms();
            int16_t* db = doom_get_sound_buffer();
            doom_ms += time_current_ms() - doom_t0;
            for (int f = 0; f < DOOM_FRAMES; ++f)
            {
                int32_t l = 2 * (int32_t)db[2 * f];
                int32_t r = 2 * (int32_t)db[2 * f + 1];
                if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
                if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
                for (int u = 0; u < UPSAMPLE; ++u)
                {
                    int m = (f * UPSAMPLE + u) * 2;
                    g_mix[m]     = (int16_t)l;
                    g_mix[m + 1] = (int16_t)r;
                }
            }
            g_mix_pos = 0;
        }

        g_out[2 * i]     = g_mix[2 * g_mix_pos];
        g_out[2 * i + 1] = g_mix[2 * g_mix_pos + 1];
        ++g_mix_pos;
    }

    // Music: drain DOOM's MIDI stream and render the synth on top of the SFX.
    //
    // Only doom_tick_midi() touches DOOM's music state (the game thread mutates
    // it on song changes / volume); it takes DOOM's g_music_lock internally per
    // call. synth_midi() and synth_render() touch only synth state, which lives
    // entirely on this audio thread. So we first drain the raw MIDI messages for
    // the whole block — tagging each with the sample offset where it lands
    // (140Hz pumps, MIDI_PUMP_FRAMES apart) — then apply them and render, all
    // lock-free. The render never blocks on the game thread's doom_update().
    int nev = 0;
    uint64_t doom_t1 = time_current_ms();
    {
        uint64_t p = 0;
        int countdown = g_midi_countdown;
        while (p < num_samples)
        {
            if (countdown <= 0)
            {
                unsigned long midi_msg;
                while ((midi_msg = doom_tick_midi()) != 0)
                {
                    if (nev < MAX_BLOCK_MIDI)
                    {
                        g_block_midi_msg[nev] = (uint32_t)midi_msg;
                        g_block_midi_off[nev] = (int)p;
                        nev++;
                    }
                    else
                    {
                        // Overflow (pathological burst): apply immediately rather
                        // than drop it, so no note gets stuck.
                        synth_midi((uint32_t)midi_msg);
                    }
                }
                countdown = MIDI_PUMP_FRAMES;
            }

            uint64_t n = num_samples - p;
            if (n > (uint64_t)countdown)
                n = (uint64_t)countdown;
            countdown -= (int)n;
            p += n;
        }
        g_midi_countdown = countdown;
    }
    doom_ms += time_current_ms() - doom_t1;

    // Apply the scheduled events and render the synth, all lock-free.
    uint64_t pos = 0;
    int ev = 0;
    while (pos < num_samples)
    {
        // Apply every event scheduled at or before this sample offset.
        while (ev < nev && (uint64_t)g_block_midi_off[ev] <= pos)
            synth_midi(g_block_midi_msg[ev++]);

        // Render up to the next event (or the end of the block).
        uint64_t next = num_samples;
        if (ev < nev && (uint64_t)g_block_midi_off[ev] < next)
            next = (uint64_t)g_block_midi_off[ev];
        synth_render(g_out + 2 * pos, (int)(next - pos));
        pos = next;
    }

    uint64_t cb_ms = time_current_ms() - cb_start_ms;
    if (cb_ms > 23)
    {
        print_str("[uvm-doom] WARNING: audio callback took ");
        print_str(doom_itoa((int)cb_ms, 10));
        print_str("ms (");
        print_str(doom_itoa((int)doom_ms, 10));
        print_str("ms in DOOM sound/music calls) for ");
        print_str(doom_itoa((int)num_samples, 10));
        print_str(" frames (budget ~23ms)\n");
    }

    return g_out;
}

// When non-NULL, the name of a demo lump to benchmark under -timedemo (set by
// build_doom_argv). Points into the argv the VM handed us, which lives for the
// whole run, so DOOM can keep the pointer.
static char* g_timedemo_name = NULL;

// DOOM's argv, filtered from our own argv in build_doom_argv().
#define MAX_DOOM_ARGS 32
static char* doom_argv[MAX_DOOM_ARGS];

// Build DOOM's argv from our command-line arguments. argv[0] is the .asm
// program path (the usual argv[0] convention); DOOM only scans argv from index
// 1, so we forward "doom" as argv[0] and pass the rest through.
//
// `-timedemo <name>` is intercepted here rather than forwarded: PureDOOM's
// command-line handler for it is broken (it calls D_DoomLoop() expecting an
// infinite loop, but PureDOOM made D_DoomLoop run a single tic, so it falls
// through to D_StartTitle() and drops into the attract loop). We record the
// demo name instead and drive the timedemo ourselves after doom_init().
//
// Returns the number of entries written to doom_argv.
static int build_doom_argv(int argc, char** argv)
{
    doom_argv[0] = "doom";
    int n = 1;

    for (int i = 1; i < argc && n < MAX_DOOM_ARGS; ++i)
    {
        // Intercept `-timedemo <name>`: capture the operand and don't forward
        // either token to DOOM (see the note above).
        if (strcmp(argv[i], "-timedemo") == 0 && i + 1 < argc)
        {
            g_timedemo_name = argv[i + 1];
            ++i; // also skip the demo-name operand
            continue;
        }

        doom_argv[n++] = argv[i];
    }

    return n;
}

int main(int argc, char** argv)
{
    int doom_argc = build_doom_argv(argc, argv);
    int benchmark = (g_timedemo_name != NULL);
    g_benchmark = benchmark; // visible to vm_present_frame()

    // Bring up the window. It stays hidden until the first frame is drawn. In
    // benchmark mode we never draw, so skip the window entirely (see the main
    // loop for why presenting/polling is skipped when benchmarking).
    if (!benchmark)
        g_window_id = window_create(WIN_WIDTH, WIN_HEIGHT, "PureDOOM - UVM", 0);

    // Wire up DOOM's platform callbacks.
    doom_set_exit(doom_exit_override);
    doom_set_malloc(vm_malloc, vm_free);
    doom_set_print(vm_print);
    doom_set_file_io(vm_open, vm_close, vm_read, vm_write, vm_seek, vm_tell, vm_eof);
    doom_set_gettime(vm_gettime);

    doom_set_resolution(WIDTH, HEIGHT);
    doom_init(doom_argc, doom_argv, DOOM_FLAG_MENU_DARKEN_BG);

    // Kick off the benchmark demo. G_TimeDemo() sets timingdemo so that, when
    // the demo finishes, DOOM prints "timed <gametics> in <realtics> realtics"
    // and quits (via our exit override). We drive it below with
    // doom_force_update().
    //
    // doom_init() already started the attract loop (D_StartTitle), which leaves
    // advancedemo=true — a pending request to cycle the title/demo sequence. If
    // we don't clear it, the first D_DoomLoop tic runs D_DoAdvanceDemo() before
    // G_Ticker acts on our ga_playdemo, resetting gameaction and replaying the
    // demos through the attract path (normal speed, no timingdemo, looping
    // forever). Clearing it lets our timed demo run instead.
    if (benchmark)
    {
        extern doom_boolean advancedemo;
        G_TimeDemo(g_timedemo_name);
        advancedemo = false;
    }

    // Only now that DOOM is initialized is it safe for the audio thread to call
    // into it. Open the stereo 44100Hz output, which spawns the audio callback
    // thread that mixes SFX and the music synth (see synth.c). Skip audio when
    // benchmarking so the audio thread's lock contention doesn't skew timings.
    if (!benchmark)
    {
        synth_init();
        g_audio_ready = 1;
        audio_open_output(44100, 2, AUDIO_FORMAT_I16, (void*)audio_cb);
    }

    // Target ~60 FPS. doom_update() additionally self-throttles game logic to
    // DOOM's native 35Hz tic rate.
    const uint64_t frame_ms = 1000 / 60;

    // Benchmark timing: wall-clock start and frame count, used for the FPS
    // report printed after the loop.
    uint64_t bench_start_ms = time_current_ms();
    uint64_t bench_frames = 0;

    while (!g_quit)
    {
        if (benchmark)
        {
            // Benchmark: run one gametic per iteration as fast as possible.
            // doom_force_update() ignores doom_update()'s real-time throttle,
            // so the demo plays back flat-out. When the demo ends DOOM sets
            // g_quit (via I_Error/our exit override), breaking the loop.
            //
            // We still call vm_present_frame() so the convert / upscale path
            // is exercised and counted, but with no window it skips the final
            // blit (see vm_present_frame). We deliberately do NOT poll input:
            // vm_poll_input() would feed window events to DOOM, and any input
            // during demo playback pops up the menu (G_Responder), aborting the
            // demo back into the attract loop instead of hitting the timingdemo
            // exit -- so it would "start over" forever rather than reporting a
            // result.
            doom_force_update();
            vm_present_frame(doom_get_framebuffer(1), WIDTH, HEIGHT);
            bench_frames++;
            continue;
        }

        uint64_t start = time_current_ms();

        vm_poll_input();

        // No coarse lock here anymore: doom_update() starts SFX and changes
        // music deep in the game tic, but those mutations now take DOOM's
        // fine-grained g_sfx_lock / g_music_lock only for the brief moments they
        // touch shared sound state (see PureDOOM.h). The bulk of doom_update()
        // — the software renderer — touches no sound state and so no longer
        // blocks the audio callback.
        doom_update();

        vm_present_frame(doom_get_framebuffer(1), WIDTH, HEIGHT);

        // Cap to ~60 FPS in normal play.
        uint64_t elapsed = time_current_ms() - start;
        if (elapsed < frame_ms)
            thread_sleep(frame_ms - elapsed);
    }

    // Report the benchmark result: DOOM already printed its "timed N gametics
    // in M realtics" line from I_Error; add a wall-clock frames/sec figure
    // measured over the whole run (game logic + render + upscale).
    if (benchmark)
    {
        uint64_t elapsed_ms = time_current_ms() - bench_start_ms;
        if (elapsed_ms == 0) elapsed_ms = 1; // avoid divide-by-zero

        // fps to two decimals via integer math (fps * 100).
        uint64_t fps100 = (bench_frames * 100000) / elapsed_ms;

        print_str("\n[uvm-doom] benchmark: ");
        print_str(doom_itoa((int)bench_frames, 10));
        print_str(" frames in ");
        print_str(doom_itoa((int)elapsed_ms, 10));
        print_str(" ms = ");
        print_str(doom_itoa((int)(fps100 / 100), 10));
        print_str(".");
        // Zero-pad the fractional part to two digits.
        {
            int frac = (int)(fps100 % 100);
            if (frac < 10) print_str("0");
            print_str(doom_itoa(frac, 10));
        }
        print_str(" fps\n");
    }

    return 0;
}
