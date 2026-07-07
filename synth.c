// synth.c — small GM-flavored software synthesizer for DOOM's music.
//
// Consumes the standard packed MIDI messages produced by doom_tick_midi()
// (status|channel in byte 0, data1 in byte 1, data2 in byte 2) and renders
// 44100Hz interleaved stereo, additively mixed into an i16 buffer with
// saturation. Everything is synthesized procedurally — no samples, no
// soundfont — so it has no asset or filesystem dependencies.
//
// The sound model is deliberately simple ("recognizably DOOM" tier):
//   - Melodic voices: 1-2 detuned oscillators (saw/square/pulse/tri/sine/
//     noise) -> linear-attack / exponential-decay ADSR -> one-pole lowpass
//     with key tracking -> equal-power pan.
//   - The 128 GM programs map onto ~16 patch families (one per GM group of
//     8), with specific overrides for the instruments DOOM's soundtrack
//     leans on (distortion guitar, basses, saw lead, ensemble strings...).
//   - Percussion (MIDI channel 9): pitch-swept sine "body" plus filtered
//     noise, tuned per GM drum note (kick/snare/hats/toms/cymbals...).
//
// This file is designed to compile both under uvclang for UVM and natively
// (for offline testing). UVM's floating point is f32-only: keep every
// literal f-suffixed and all arithmetic in float. sinf/exp2f/sqrtf lower to
// single native UVM instructions, so they are cheap even per-sample.

#include <stdint.h>
#include <math.h>

#define SYNTH_SAMPLE_RATE 44100
#define SYNTH_MAX_VOICES  24
#define SYNTH_MAX_FRAMES  1024   // max frames per synth_render() call

// Final output scale for the voice mix (i16 units per 1.0 of mix amplitude).
// Tuned so a typical DOOM track peaks well below clipping while staying
// audible against the SFX mix.
#define SYNTH_MASTER 50000.0f

// Oscillator waveforms
enum
{
    W_SAW,
    W_SQUARE,
    W_PULSE,   // 25% duty pulse
    W_TRI,
    W_SINE,
    W_NOISE,
};

// Envelope stages. SUSTAIN is implicit: DECAY converges onto the sustain
// level and stays there until note-off moves the voice to RELEASE.
enum
{
    ENV_ATTACK,
    ENV_DECAY,
    ENV_RELEASE,
};

//-----------------------------------------------------------------------------
// Patches: how one GM program sounds.
//-----------------------------------------------------------------------------

typedef struct
{
    uint8_t wave;
    float attack;   // seconds, linear ramp 0 -> 1
    float decay;    // seconds, exponential fall 1 -> sustain
    float sustain;  // 0..1 level held while the note is down
    float release;  // seconds, exponential fall to 0 after note-off
    float cutoff;   // lowpass cutoff in Hz (key tracking is added on top)
    float gain;     // per-patch amplitude trim
    float detune;   // second-oscillator ratio; 0 = single oscillator
    float drive;    // waveshaper pre-gain; 0 = clean (no distortion stage)
    float sub;      // octave-down square sub-oscillator level; 0 = off
} Patch;

//-----------------------------------------------------------------------------
// Voices
//-----------------------------------------------------------------------------

typedef struct
{
    uint8_t active;
    uint8_t drum;      // 1 = percussion voice (channel 9)
    uint8_t chan;
    uint8_t note;
    uint8_t stage;     // ENV_*
    uint8_t pending;   // note-off arrived while sustain pedal was down
    uint8_t wave;
    uint32_t age;      // note-on order, for voice stealing

    float freq;        // base frequency in Hz (before pitch bend)
    float phase, phase2, phase3;
    float detune;      // osc2 freq ratio, 0 = single osc
    float drive;       // waveshaper pre-gain, 0 = clean
    float sub_amp;     // octave-down sub oscillator level, 0 = off

    float env;         // current envelope level
    float attack_inc;  // per-sample linear attack increment
    float decay_mult;  // per-sample multiplier toward sustain
    float sustain;
    float release_mult;

    float filt_c;      // one-pole lowpass coefficient
    float filt_y;      // filter state
    float amp;         // velocity * patch gain

    // Percussion-only: the sine body sweeps from freq down to drum_f1.
    float drum_f1;
    float drum_pmult;  // per-sample multiplier pulling freq toward drum_f1
    float tone_amp;    // level of the swept sine body
    float noise_amp;   // level of the noise component
    float noise_hp;    // 1 = highpass the noise (cymbals/hats)
    float noise_lp;    // filter state for the noise highpass

    uint32_t rng;      // per-voice noise generator state
} Voice;

//-----------------------------------------------------------------------------
// Channels
//-----------------------------------------------------------------------------

typedef struct
{
    uint8_t prog;
    uint8_t vol;       // CC7
    uint8_t expr;      // CC11
    uint8_t pan;       // CC10, 0=left 64=center 127=right
    uint8_t sustain;   // CC64 pedal down
    float bend;        // pitch-bend frequency ratio (1.0 = center)
    Patch patch;
} SynthChan;

static Voice g_voices[SYNTH_MAX_VOICES];
static SynthChan g_chans[16];
static uint32_t g_voice_age = 0;

static float g_note_freq[128];          // MIDI note -> Hz
static float g_mixl[SYNTH_MAX_FRAMES];  // per-block float accumulators
static float g_mixr[SYNTH_MAX_FRAMES];

// Exponential envelope coefficient: per-sample multiplier that decays to
// ~exp(-5) (effectively silence/settled) over `seconds`.
static float env_coef(float seconds)
{
    if (seconds <= 0.0f)
        return 0.0f;
    // exp(-5 / (seconds * sr)) = 2^(-5 * log2(e) / (seconds * sr))
    return exp2f(-7.213475f / (seconds * (float)SYNTH_SAMPLE_RATE));
}

// Cubic soft clipper: smooth saturation for the distortion stage.
// x - (4/27)x^3 maps [-1.5, 1.5] onto [-1, 1] with zero slope at the rails;
// beyond that it hard-limits. Driving a detuned saw stack into this is what
// produces the guitar grit.
static float softclip(float x)
{
    if (x > 1.5f) return 1.0f;
    if (x < -1.5f) return -1.0f;
    return x * (1.0f - x * x * (4.0f / 27.0f));
}

//-----------------------------------------------------------------------------
// GM program -> patch mapping.
//
// One base patch per GM family (program / 8), then overrides for specific
// programs that matter to DOOM's soundtrack. Values are ballpark tunings,
// not physical models.
//-----------------------------------------------------------------------------

static void gm_patch(int prog, Patch* p)
{
    // Family defaults
    switch (prog >> 3)
    {
        case 0: // 0-7 pianos: bright pluck, no sustain
            *p = (Patch){ W_PULSE, 0.002f, 1.1f, 0.0f, 0.25f, 3000.0f, 0.85f, 0.0f };
            break;
        case 1: // 8-15 chromatic percussion: bell-like sine pluck
            *p = (Patch){ W_SINE, 0.002f, 1.6f, 0.0f, 0.5f, 6000.0f, 1.0f, 0.0f };
            break;
        case 2: // 16-23 organs: sustained square
            *p = (Patch){ W_SQUARE, 0.01f, 0.1f, 0.9f, 0.06f, 2500.0f, 0.45f, 0.0f };
            break;
        case 3: // 24-31 guitars: plucked saw
            *p = (Patch){ W_SAW, 0.002f, 0.7f, 0.0f, 0.15f, 2200.0f, 0.8f, 0.0f };
            break;
        case 4: // 32-39 basses: dark plucked saw with body
            *p = (Patch){ W_SAW, 0.002f, 0.5f, 0.35f, 0.1f, 700.0f, 0.9f, 0.0f };
            break;
        case 5: // 40-47 solo strings: slow-ish bowed saw
            *p = (Patch){ W_SAW, 0.06f, 0.3f, 0.8f, 0.3f, 3200.0f, 0.5f, 1.004f };
            break;
        case 6: // 48-55 ensembles: thick detuned saw pad
            *p = (Patch){ W_SAW, 0.12f, 0.4f, 0.85f, 0.5f, 2800.0f, 0.45f, 1.006f };
            break;
        case 7: // 56-63 brass: bright saw swell
            *p = (Patch){ W_SAW, 0.03f, 0.2f, 0.8f, 0.15f, 2600.0f, 0.55f, 0.0f };
            break;
        case 8: // 64-71 reeds: hollow pulse
            *p = (Patch){ W_PULSE, 0.03f, 0.15f, 0.85f, 0.1f, 2200.0f, 0.55f, 0.0f };
            break;
        case 9: // 72-79 pipes/flutes: soft triangle
            *p = (Patch){ W_TRI, 0.04f, 0.15f, 0.85f, 0.15f, 2000.0f, 0.65f, 0.0f };
            break;
        case 10: // 80-87 synth leads: bright saw
            *p = (Patch){ W_SAW, 0.01f, 0.2f, 0.85f, 0.15f, 3500.0f, 0.5f, 0.0f };
            break;
        case 11: // 88-95 synth pads: slow detuned wash
            *p = (Patch){ W_SAW, 0.3f, 0.5f, 0.8f, 0.8f, 1400.0f, 0.4f, 1.007f };
            break;
        case 12: // 96-103 FX: pad-ish triangle
            *p = (Patch){ W_TRI, 0.2f, 0.4f, 0.7f, 1.0f, 1800.0f, 0.4f, 1.005f };
            break;
        case 13: // 104-111 ethnic: plucked
            *p = (Patch){ W_SAW, 0.002f, 0.6f, 0.0f, 0.2f, 2500.0f, 0.7f, 0.0f };
            break;
        case 14: // 112-119 percussive: sine pluck
            *p = (Patch){ W_SINE, 0.002f, 0.4f, 0.0f, 0.2f, 4000.0f, 0.9f, 0.0f };
            break;
        default: // 120-127 sound effects: noise burst
            *p = (Patch){ W_NOISE, 0.002f, 0.3f, 0.0f, 0.3f, 4000.0f, 0.5f, 0.0f };
            break;
    }

    // Program-specific overrides (the sounds DOOM's tracks lean on).
    switch (prog)
    {
        case 11: // Vibraphone: longer shimmer
            p->decay = 2.2f;
            break;
        case 18: // Rock organ: detuned for a rotary-ish thickness
            p->detune = 1.003f;
            break;
        case 28: // Electric guitar (muted): distorted palm-mute chug
            p->decay = 0.22f;
            p->cutoff = 1600.0f;
            p->gain = 0.9f;
            p->drive = 8.0f;
            p->sub = 0.5f;
            break;
        case 29: case 30: // Overdriven/distortion guitar: THE Doom riff sound.
            // Detuned saw pair + octave-down sub driven hard into the soft
            // clipper: the clipping compresses the attack into a sustained,
            // growling wall of sound (the detune beating modulates the
            // saturation, which is where the grit comes from).
            p->wave = W_SAW;
            p->decay = 1.2f;
            p->sustain = 0.7f;
            p->release = 0.15f;
            p->cutoff = 3000.0f;
            p->gain = 0.9f;
            p->detune = 1.009f;
            p->drive = 14.0f;
            p->sub = 0.6f;
            break;
        case 31: // Guitar harmonics: brighter, lighter overdrive
            p->wave = W_SAW;
            p->cutoff = 3500.0f;
            p->gain = 0.6f;
            p->drive = 5.0f;
            break;
        case 38: case 39: // Synth bass: square, a bit brighter
            p->wave = W_SQUARE;
            p->cutoff = 900.0f;
            p->gain = 0.75f;
            break;
        case 45: case 46: // Pizzicato / harp: pluck instead of bow
            p->attack = 0.002f;
            p->decay = 0.5f;
            p->sustain = 0.0f;
            p->release = 0.15f;
            p->detune = 0.0f;
            break;
        case 47: // Timpani: dark boomy sine hit
            p->wave = W_SINE;
            p->attack = 0.002f;
            p->decay = 0.7f;
            p->sustain = 0.0f;
            p->release = 0.4f;
            p->cutoff = 800.0f;
            p->gain = 1.0f;
            p->detune = 0.0f;
            break;
        case 52: case 53: case 54: // Choir/voice: softer triangle wash
            p->wave = W_TRI;
            p->cutoff = 1500.0f;
            p->gain = 0.5f;
            break;
        case 62: case 63: // Synth brass: detuned analog stab
            p->detune = 1.005f;
            break;
        case 80: // Square lead
            p->wave = W_SQUARE;
            break;
        case 81: // Saw lead: detuned (poor man's supersaw)
            p->detune = 1.004f;
            break;
        case 108: // Kalimba: rounder, longer
            p->wave = W_SINE;
            p->decay = 0.8f;
            break;
        case 119: // Reverse cymbal: noise swell while held
            p->wave = W_NOISE;
            p->attack = 0.9f;
            p->decay = 0.1f;
            p->sustain = 1.0f;
            p->release = 0.08f;
            p->cutoff = 6000.0f;
            p->gain = 0.5f;
            break;
    }
}

//-----------------------------------------------------------------------------
// Percussion (GM drum map, channel 9).
//-----------------------------------------------------------------------------

// Configure a voice as a drum hit for the given GM drum note.
static void drum_setup(Voice* v, int note)
{
    // Defaults: generic short noise tick.
    float f0 = 200.0f, f1 = 100.0f; // sine body sweep start/end (Hz)
    float pdecay = 0.03f;           // pitch sweep time constant (s)
    float tone = 0.0f, noise = 0.6f;
    float decay = 0.15f;            // amplitude decay (s)
    float hp = 0.0f;                // highpass the noise (metallic)
    float gain = 0.8f;

    switch (note)
    {
        case 35: case 36: // Kick
            f0 = 170.0f; f1 = 50.0f; pdecay = 0.03f;
            tone = 1.0f; noise = 0.12f; decay = 0.17f; gain = 1.1f;
            break;
        case 37: // Rimshot
            f0 = 450.0f; f1 = 350.0f; pdecay = 0.01f;
            tone = 0.4f; noise = 0.6f; decay = 0.06f; gain = 0.7f;
            break;
        case 38: case 40: // Snare
            f0 = 220.0f; f1 = 160.0f; pdecay = 0.02f;
            tone = 0.35f; noise = 0.85f; decay = 0.18f; gain = 0.9f;
            break;
        case 39: // Hand clap
            tone = 0.0f; noise = 0.9f; decay = 0.12f; gain = 0.8f;
            break;
        case 41: case 43: case 45: case 47: case 48: case 50: // Toms, low->high
        {
            // Tom pitch rises with the note.
            float base = 90.0f + ((float)note - 41.0f) * 15.0f;
            f0 = base * 2.0f; f1 = base; pdecay = 0.04f;
            tone = 1.0f; noise = 0.15f; decay = 0.28f; gain = 0.9f;
            break;
        }
        case 42: case 44: // Closed/pedal hi-hat
            tone = 0.0f; noise = 0.7f; decay = 0.045f; hp = 1.0f; gain = 0.6f;
            break;
        case 46: // Open hi-hat
            tone = 0.0f; noise = 0.7f; decay = 0.3f; hp = 1.0f; gain = 0.6f;
            break;
        case 49: case 57: // Crash cymbals
            tone = 0.0f; noise = 0.8f; decay = 1.1f; hp = 1.0f; gain = 0.8f;
            break;
        case 51: case 59: // Ride cymbals
            tone = 0.0f; noise = 0.5f; decay = 0.35f; hp = 1.0f; gain = 0.6f;
            break;
        case 52: // China cymbal
            tone = 0.0f; noise = 0.8f; decay = 0.8f; hp = 1.0f; gain = 0.7f;
            break;
        case 53: // Ride bell
            f0 = 540.0f; f1 = 520.0f; pdecay = 0.05f;
            tone = 0.5f; noise = 0.3f; decay = 0.25f; hp = 1.0f; gain = 0.6f;
            break;
        case 54: // Tambourine
            tone = 0.0f; noise = 0.6f; decay = 0.1f; hp = 1.0f; gain = 0.6f;
            break;
        case 55: // Splash cymbal
            tone = 0.0f; noise = 0.8f; decay = 0.5f; hp = 1.0f; gain = 0.7f;
            break;
        case 56: // Cowbell
            f0 = 560.0f; f1 = 540.0f; pdecay = 0.05f;
            tone = 0.9f; noise = 0.05f; decay = 0.12f; gain = 0.7f;
            break;
    }

    v->drum = 1;
    v->wave = W_SINE;
    v->freq = f0;
    v->drum_f1 = f1;
    v->drum_pmult = env_coef(pdecay);
    v->amp *= gain; // amp was set to velocity by the caller
    v->tone_amp = tone;
    v->noise_amp = noise;
    v->noise_hp = hp;

    // Percussion envelope: instant attack (with a tiny declick ramp), pure
    // exponential decay, no sustain. Drums ignore note-off.
    v->stage = ENV_ATTACK;
    v->env = 0.0f;
    v->attack_inc = 1.0f / (0.001f * (float)SYNTH_SAMPLE_RATE);
    v->decay_mult = env_coef(decay);
    v->sustain = 0.0f;
    v->release_mult = v->decay_mult;
    v->filt_c = 1.0f; // no lowpass on drums; hats/cymbals use the noise highpass
}

//-----------------------------------------------------------------------------
// Voice allocation
//-----------------------------------------------------------------------------

static Voice* alloc_voice(void)
{
    Voice* best = 0;

    // Free voice?
    for (int i = 0; i < SYNTH_MAX_VOICES; ++i)
        if (!g_voices[i].active)
            return &g_voices[i];

    // Steal the quietest releasing voice.
    for (int i = 0; i < SYNTH_MAX_VOICES; ++i)
    {
        Voice* v = &g_voices[i];
        if (v->stage == ENV_RELEASE && (!best || v->env < best->env))
            best = v;
    }
    if (best)
        return best;

    // Else steal the oldest note.
    best = &g_voices[0];
    for (int i = 1; i < SYNTH_MAX_VOICES; ++i)
        if (g_voices[i].age < best->age)
            best = &g_voices[i];
    return best;
}

static void note_on(int chan, int note, int vel)
{
    if (vel == 0)
        return; // handled as note-off by the caller

    SynthChan* c = &g_chans[chan];
    Voice* v = alloc_voice();

    float fvel = (float)vel * (1.0f / 127.0f);

    v->active = 1;
    v->drum = 0;
    v->chan = (uint8_t)chan;
    v->note = (uint8_t)note;
    v->pending = 0;
    v->age = ++g_voice_age;
    v->phase = 0.0f;
    v->phase2 = 0.0f;
    v->phase3 = 0.0f;
    v->filt_y = 0.0f;
    v->noise_lp = 0.0f;
    v->rng = 0x9E3779B9u ^ (g_voice_age * 2654435761u);
    v->amp = fvel * fvel; // quadratic velocity curve, punchier than linear

    if (chan == 9)
    {
        drum_setup(v, note);
        return;
    }

    Patch* p = &c->patch;
    v->wave = p->wave;
    v->freq = g_note_freq[note & 127];
    v->detune = p->detune;
    v->drive = p->drive;
    v->sub_amp = p->sub;
    v->amp *= p->gain;

    v->stage = ENV_ATTACK;
    v->env = 0.0f;
    v->attack_inc = 1.0f / ((p->attack > 0.0005f ? p->attack : 0.0005f) * (float)SYNTH_SAMPLE_RATE);
    v->decay_mult = env_coef(p->decay);
    v->sustain = p->sustain;
    v->release_mult = env_coef(p->release);

    // One-pole lowpass coefficient, with key tracking so high notes are not
    // muffled: c = 1 - exp(-2*pi*fc/sr).
    float fc = p->cutoff + 2.0f * v->freq;
    if (fc > 12000.0f) fc = 12000.0f;
    v->filt_c = 1.0f - exp2f(-9.064720f * fc / (float)SYNTH_SAMPLE_RATE);

    // Percussion-only fields unused for melodic voices.
    v->drum_f1 = 0.0f;
    v->drum_pmult = 1.0f;
    v->tone_amp = 0.0f;
    v->noise_amp = 0.0f;
    v->noise_hp = 0.0f;
}

static void note_off(int chan, int note)
{
    if (chan == 9)
        return; // GM drums decay on their own

    for (int i = 0; i < SYNTH_MAX_VOICES; ++i)
    {
        Voice* v = &g_voices[i];
        if (v->active && v->chan == chan && v->note == note && v->stage != ENV_RELEASE)
        {
            if (g_chans[chan].sustain)
                v->pending = 1; // release when the pedal comes up
            else
                v->stage = ENV_RELEASE;
        }
    }
}

// Release (or hard-kill) every voice on a channel.
static void channel_off(int chan, int hard)
{
    for (int i = 0; i < SYNTH_MAX_VOICES; ++i)
    {
        Voice* v = &g_voices[i];
        if (v->active && v->chan == chan)
        {
            if (hard)
                v->active = 0;
            else
                v->stage = ENV_RELEASE;
        }
    }
}

//-----------------------------------------------------------------------------
// Public API
//-----------------------------------------------------------------------------

void synth_init(void)
{
    // MIDI note -> frequency: 440 * 2^((n-69)/12).
    for (int n = 0; n < 128; ++n)
        g_note_freq[n] = 440.0f * exp2f(((float)n - 69.0f) * (1.0f / 12.0f));

    for (int c = 0; c < 16; ++c)
    {
        g_chans[c].prog = 0;
        g_chans[c].vol = 100;
        g_chans[c].expr = 127;
        g_chans[c].pan = 64;
        g_chans[c].sustain = 0;
        g_chans[c].bend = 1.0f;
        gm_patch(0, &g_chans[c].patch);
    }

    for (int i = 0; i < SYNTH_MAX_VOICES; ++i)
        g_voices[i].active = 0;
}

// Feed one packed MIDI message (as returned by doom_tick_midi()).
void synth_midi(uint32_t msg)
{
    int status = (int)(msg & 0xF0u);
    int chan = (int)(msg & 0x0Fu);
    int d1 = (int)((msg >> 8) & 0x7Fu);
    int d2 = (int)((msg >> 16) & 0x7Fu);
    SynthChan* c = &g_chans[chan];

    switch (status)
    {
        case 0x80: // note off
            note_off(chan, d1);
            break;

        case 0x90: // note on (velocity 0 == note off)
            if (d2 == 0)
                note_off(chan, d1);
            else
                note_on(chan, d1, d2);
            break;

        case 0xB0: // control change
            switch (d1)
            {
                case 7: c->vol = (uint8_t)d2; break;
                case 10: c->pan = (uint8_t)d2; break;
                case 11: c->expr = (uint8_t)d2; break;
                case 64: // sustain pedal
                    c->sustain = (uint8_t)(d2 >= 64);
                    if (!c->sustain)
                    {
                        // Pedal up: release notes that ended while it was down.
                        for (int i = 0; i < SYNTH_MAX_VOICES; ++i)
                        {
                            Voice* v = &g_voices[i];
                            if (v->active && v->chan == chan && v->pending)
                            {
                                v->pending = 0;
                                v->stage = ENV_RELEASE;
                            }
                        }
                    }
                    break;
                case 120: channel_off(chan, 1); break; // all sounds off
                case 121: // reset all controllers
                    c->expr = 127;
                    c->sustain = 0;
                    c->bend = 1.0f;
                    break;
                case 123: channel_off(chan, 0); break; // all notes off
                // Bank select / mod / reverb / chorus / soft pedal: ignored.
            }
            break;

        case 0xC0: // program change
            c->prog = (uint8_t)d1;
            gm_patch(d1, &c->patch);
            break;

        case 0xE0: // pitch bend, +/-2 semitones full scale
        {
            int bend14 = d1 | (d2 << 7);
            c->bend = exp2f((float)(bend14 - 8192) * (2.0f / (8192.0f * 12.0f)));
            break;
        }
    }
}

// Render `frames` frames of music and additively mix them into `out`
// (interleaved stereo i16) with saturation. frames <= SYNTH_MAX_FRAMES.
void synth_render(int16_t* out, int frames)
{
    if (frames > SYNTH_MAX_FRAMES)
        frames = SYNTH_MAX_FRAMES;

    for (int i = 0; i < frames; ++i)
    {
        g_mixl[i] = 0.0f;
        g_mixr[i] = 0.0f;
    }

    int any = 0;

    for (int vi = 0; vi < SYNTH_MAX_VOICES; ++vi)
    {
        Voice* v = &g_voices[vi];
        if (!v->active)
            continue;
        any = 1;

        SynthChan* c = &g_chans[v->chan];

        // Per-block (not per-sample) parameters: channel volume/expression,
        // equal-power pan, pitch bend. MIDI updates arrive at most at 140Hz
        // and synth_render is called in sub-block chunks between MIDI ticks,
        // so per-block resolution is sample-accurate enough.
        float cv = ((float)c->vol * (1.0f / 127.0f)) * ((float)c->expr * (1.0f / 127.0f));
        float gl = sqrtf((float)(127 - c->pan) * (1.0f / 127.0f)) * cv * v->amp;
        float gr = sqrtf((float)c->pan * (1.0f / 127.0f)) * cv * v->amp;

        float pinc = 0.0f, pinc2 = 0.0f;
        if (!v->drum)
        {
            float f = v->freq * c->bend;
            pinc = f * (1.0f / (float)SYNTH_SAMPLE_RATE);
            pinc2 = (v->detune > 0.0f) ? pinc * v->detune : 0.0f;
        }

        float phase = v->phase, phase2 = v->phase2, phase3 = v->phase3;
        float env = v->env;
        float fy = v->filt_y;
        float nlp = v->noise_lp;
        float freq = v->freq;
        uint32_t rng = v->rng;
        int stage = v->stage;

        for (int i = 0; i < frames; ++i)
        {
            // Envelope
            if (stage == ENV_ATTACK)
            {
                env += v->attack_inc;
                if (env >= 1.0f) { env = 1.0f; stage = ENV_DECAY; }
            }
            else if (stage == ENV_DECAY)
            {
                env = v->sustain + (env - v->sustain) * v->decay_mult;
            }
            else // ENV_RELEASE
            {
                env *= v->release_mult;
            }

            float s;
            if (v->drum)
            {
                // Pitch-swept sine body + noise.
                freq = v->drum_f1 + (freq - v->drum_f1) * v->drum_pmult;
                phase += freq * (1.0f / (float)SYNTH_SAMPLE_RATE);
                if (phase >= 1.0f) phase -= 1.0f;
                s = sinf(phase * 6.2831853f) * v->tone_amp;

                rng = rng * 1103515245u + 12345u;
                float n = (float)(int32_t)rng * (1.0f / 2147483648.0f);
                if (v->noise_hp > 0.0f)
                {
                    // One-pole highpass: keep only what the lowpass rejects.
                    nlp += 0.4f * (n - nlp);
                    n = n - nlp;
                }
                s += n * v->noise_amp;
            }
            else
            {
                phase += pinc;
                if (phase >= 1.0f) phase -= 1.0f;

                switch (v->wave)
                {
                    case W_SAW:    s = 2.0f * phase - 1.0f; break;
                    case W_SQUARE: s = (phase < 0.5f) ? 1.0f : -1.0f; break;
                    case W_PULSE:  s = (phase < 0.25f) ? 1.0f : -1.0f; break;
                    case W_TRI:    s = (phase < 0.5f) ? (4.0f * phase - 1.0f) : (3.0f - 4.0f * phase); break;
                    case W_SINE:   s = sinf(phase * 6.2831853f); break;
                    default: // W_NOISE
                        rng = rng * 1103515245u + 12345u;
                        s = (float)(int32_t)rng * (1.0f / 2147483648.0f);
                        break;
                }

                if (pinc2 > 0.0f)
                {
                    phase2 += pinc2;
                    if (phase2 >= 1.0f) phase2 -= 1.0f;
                    // Same waveform family; saw is what detune is used for.
                    float s2 = (v->wave == W_SQUARE) ? ((phase2 < 0.5f) ? 1.0f : -1.0f)
                                                     : (2.0f * phase2 - 1.0f);
                    s = 0.5f * (s + s2);
                }

                // Octave-down square sub oscillator (distorted guitars).
                if (v->sub_amp > 0.0f)
                {
                    phase3 += pinc * 0.5f;
                    if (phase3 >= 1.0f) phase3 -= 1.0f;
                    s += ((phase3 < 0.5f) ? 1.0f : -1.0f) * v->sub_amp;
                }

                // One-pole lowpass (tone shaping happens before the clipper)
                fy += v->filt_c * (s - fy);
                s = fy;

                // Distortion stage. Note this runs *before* the envelope is
                // applied, so the saturation depth stays constant while the
                // note decays — the compressed, singing sustain of a
                // distorted amp rather than a fading clean tone.
                // The 0.25 bias makes the clipping asymmetric, which adds
                // even harmonics (tube-style snarl) on top of the odd ones;
                // subtracting softclip(0.25) recenters the output around 0.
                if (v->drive > 0.0f)
                    s = softclip(s * v->drive + 0.25f) - 0.247685f;
            }

            float o = s * env;
            g_mixl[i] += o * gl;
            g_mixr[i] += o * gr;
        }

        v->phase = phase;
        v->phase2 = phase2;
        v->phase3 = phase3;
        v->env = env;
        v->filt_y = fy;
        v->noise_lp = nlp;
        v->freq = freq;
        v->rng = rng;
        v->stage = (uint8_t)stage;

        // Voice done? (Fully decayed with nothing left to sustain.)
        if (env < 0.0005f && stage != ENV_ATTACK && (stage == ENV_RELEASE || v->sustain < 0.001f))
            v->active = 0;
    }

    if (!any)
        return;

    // Saturating add into the i16 output.
    for (int i = 0; i < frames; ++i)
    {
        int32_t l = (int32_t)out[2 * i] + (int32_t)(g_mixl[i] * SYNTH_MASTER);
        int32_t r = (int32_t)out[2 * i + 1] + (int32_t)(g_mixr[i] * SYNTH_MASTER);
        if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
        out[2 * i] = (int16_t)l;
        out[2 * i + 1] = (int16_t)r;
    }
}
