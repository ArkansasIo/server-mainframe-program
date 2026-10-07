/* ==========================================================================
 * tools/make_assets.c - Generate the RailControl audio and image assets.
 *
 * Writes into assets/audio and assets/images:
 *
 *   audio/  one .wav per sound in the palette (16-bit mono PCM, 22050 Hz)
 *   images/ one .bmp per icon (24-bit uncompressed, easy for the Win32 API
 *           to load with LoadImage) plus a colour swatch strip used as the
 *           panel legend
 *
 * Everything is synthesised from first principles - no sample files, no audio
 * library - so the repository stays self contained and the assets can be
 * regenerated on any platform with a plain C compiler.
 *
 * Build:   cc -O2 -o make_assets tools/make_assets.c -lm
 * Run:     ./make_assets assets
 *
 * Sound design notes
 * ------------------
 * Each tone is shaped so it is recognisable without being memorable music:
 *   - keypress      very short, low, unobtrusive click
 *   - refused       two descending beeps (the classic "no")
 *   - acknowledge   one clean mid tone
 *   - route set     three rising notes (progress)
 *   - emergency     a loud alternating two-tone siren
 *   - track fail    a slow pulsing low buzz
 *   - collision     a fast aggressive double pulse
 *
 * The tones are deliberately distinct in pitch AND rhythm, because a
 * signaller identifies them by ear while looking elsewhere on the panel.
 * ========================================================================== */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_RATE 22050

/* --------------------------------------------------------------------------
 * WAV writing
 * -------------------------------------------------------------------------- */
static void write_u32(FILE *f, uint32_t v)
{
    fputc((int)(v & 0xFF), f);
    fputc((int)((v >> 8) & 0xFF), f);
    fputc((int)((v >> 16) & 0xFF), f);
    fputc((int)((v >> 24) & 0xFF), f);
}

static void write_u16(FILE *f, uint16_t v)
{
    fputc((int)(v & 0xFF), f);
    fputc((int)((v >> 8) & 0xFF), f);
}

/* Write a complete 16-bit mono PCM WAV from an array of samples in [-1, 1]. */
static int write_wav(const char *path, const double *samples, size_t count)
{
    FILE *f = fopen(path, "wb");
    size_t i;

    if (f == NULL)
    {
        fprintf(stderr, "cannot write %s\n", path);
        return -1;
    }

    {
        const uint32_t data_bytes = (uint32_t)(count * 2);
        const uint32_t riff_size = 36 + data_bytes;

        fwrite("RIFF", 1, 4, f);
        write_u32(f, riff_size);
        fwrite("WAVE", 1, 4, f);

        fwrite("fmt ", 1, 4, f);
        write_u32(f, 16); /* PCM chunk size */
        write_u16(f, 1);  /* PCM */
        write_u16(f, 1);  /* mono */
        write_u32(f, SAMPLE_RATE);
        write_u32(f, SAMPLE_RATE * 2); /* byte rate */
        write_u16(f, 2);               /* block align */
        write_u16(f, 16);              /* bits per sample */

        fwrite("data", 1, 4, f);
        write_u32(f, data_bytes);

        for (i = 0; i < count; ++i)
        {
            double s = samples[i];
            if (s > 1.0)
            {
                s = 1.0;
            }
            if (s < -1.0)
            {
                s = -1.0;
            }
            write_u16(f, (uint16_t)(int16_t)(s * 32767.0));
        }
    }

    fclose(f);
    printf("  audio/%s  (%zu samples, %.2f s)\n",
           strrchr(path, '/') != NULL ? strrchr(path, '/') + 1 : path,
           count, (double)count / SAMPLE_RATE);
    return 0;
}

/* --------------------------------------------------------------------------
 * Synthesis helpers
 * -------------------------------------------------------------------------- */

/* Apply a short attack and a longer release so no tone clicks at the edges.
 * A click at the start of a loud alarm is unpleasant and, on a real panel,
 * indistinguishable from a fault. */
static void envelope(double *samples, size_t count, double attack_ms, double release_ms)
{
    size_t attack = (size_t)(attack_ms * SAMPLE_RATE / 1000.0);
    size_t release = (size_t)(release_ms * SAMPLE_RATE / 1000.0);
    size_t i;

    if (attack == 0)
    {
        attack = 1;
    }
    if (release == 0)
    {
        release = 1;
    }

    for (i = 0; i < attack && i < count; ++i)
    {
        samples[i] *= (double)i / (double)attack;
    }
    for (i = 0; i < release && i < count; ++i)
    {
        const size_t index = count - 1 - i;
        samples[index] *= (double)i / (double)release;
    }
}

/* A pure tone plus a touch of second harmonic, which reads as "electronic"
 * rather than "test signal". */
static double tone_sample(double phase)
{
    return sin(phase) * 0.85 + sin(phase * 2.0) * 0.15;
}

typedef struct
{
    double *samples;
    size_t count;
    size_t capacity;
} Buffer;

static void buffer_init(Buffer *b, size_t capacity)
{
    b->samples = (double *)calloc(capacity, sizeof(double));
    b->count = 0;
    b->capacity = capacity;
}

static void buffer_free(Buffer *b)
{
    free(b->samples);
    b->samples = NULL;
    b->count = 0;
    b->capacity = 0;
}

/* Append `ms` of tone at `frequency`, at `amplitude`, with a short envelope. */
static void buffer_append_tone(Buffer *b, double frequency, double ms, double amplitude)
{
    const size_t n = (size_t)(ms * SAMPLE_RATE / 1000.0);
    const double step = 2.0 * 3.14159265358979 * frequency / SAMPLE_RATE;
    size_t i;
    double phase = 0.0;

    for (i = 0; i < n && b->count < b->capacity; ++i)
    {
        b->samples[b->count++] = tone_sample(phase) * amplitude;
        phase += step;
    }
}

static void buffer_append_silence(Buffer *b, double ms)
{
    const size_t n = (size_t)(ms * SAMPLE_RATE / 1000.0);
    size_t i;

    for (i = 0; i < n && b->count < b->capacity; ++i)
    {
        b->samples[b->count++] = 0.0;
    }
}

/* A frequency sweep, used for the "refused" descending tone. */
static void buffer_append_sweep(Buffer *b, double f_start, double f_end, double ms,
                                double amplitude)
{
    const size_t n = (size_t)(ms * SAMPLE_RATE / 1000.0);
    size_t i;
    double phase = 0.0;

    for (i = 0; i < n && b->count < b->capacity; ++i)
    {
        const double t = (double)i / (double)n;
        const double frequency = f_start + (f_end - f_start) * t;
        b->samples[b->count++] = tone_sample(phase) * amplitude;
        phase += 2.0 * 3.14159265358979 * frequency / SAMPLE_RATE;
    }
}

/* Amplitude modulated buzz, used for the track circuit and collision alarms. */
static void buffer_append_pulse(Buffer *b, double frequency, double ms,
                                double amplitude, double pulse_hz)
{
    const size_t n = (size_t)(ms * SAMPLE_RATE / 1000.0);
    size_t i;
    double phase = 0.0;
    double pulse_phase = 0.0;

    for (i = 0; i < n && b->count < b->capacity; ++i)
    {
        const double gate = sin(pulse_phase) > 0.0 ? 1.0 : 0.15;
        b->samples[b->count++] = tone_sample(phase) * amplitude * gate;
        phase += 2.0 * 3.14159265358979 * frequency / SAMPLE_RATE;
        pulse_phase += 2.0 * 3.14159265358979 * pulse_hz / SAMPLE_RATE;
    }
}

/* --------------------------------------------------------------------------
 * The sound palette
 * -------------------------------------------------------------------------- */
static int make_sound(const char *directory, const char *name,
                      void (*build)(Buffer *))
{
    char path[512];
    Buffer buffer;

    buffer_init(&buffer, SAMPLE_RATE * 4); /* up to four seconds */
    build(&buffer);
    envelope(buffer.samples, buffer.count, 4.0, 12.0);

    snprintf(path, sizeof(path), "%s/%s.wav", directory, name);
    {
        const int result = write_wav(path, buffer.samples, buffer.count);
        buffer_free(&buffer);
        return result;
    }
}

static void build_keypress(Buffer *b)
{
    buffer_append_tone(b, 1200.0, 14.0, 0.22);
    buffer_append_silence(b, 6.0);
    buffer_append_tone(b, 1800.0, 10.0, 0.12);
}

static void build_refused(Buffer *b)
{
    buffer_append_sweep(b, 500.0, 340.0, 130.0, 0.55);
    buffer_append_silence(b, 50.0);
    buffer_append_sweep(b, 420.0, 250.0, 170.0, 0.55);
}

static void build_acknowledge(Buffer *b)
{
    buffer_append_tone(b, 880.0, 90.0, 0.42);
    buffer_append_silence(b, 30.0);
    buffer_append_tone(b, 1320.0, 70.0, 0.34);
}

static void build_signal_clear(Buffer *b)
{
    buffer_append_tone(b, 660.0, 55.0, 0.40);
    buffer_append_silence(b, 25.0);
    buffer_append_tone(b, 990.0, 110.0, 0.45);
}

static void build_signal_danger(Buffer *b)
{
    buffer_append_tone(b, 990.0, 55.0, 0.42);
    buffer_append_silence(b, 25.0);
    buffer_append_tone(b, 560.0, 120.0, 0.45);
}

static void build_point_move(Buffer *b)
{
    /* A slow low thrum: heavy machinery in motion. */
    buffer_append_pulse(b, 110.0, 260.0, 0.30, 22.0);
}

static void build_point_lock(Buffer *b)
{
    buffer_append_tone(b, 200.0, 40.0, 0.42);
    buffer_append_silence(b, 25.0);
    buffer_append_tone(b, 160.0, 60.0, 0.46);
}

static void build_route_set(Buffer *b)
{
    buffer_append_tone(b, 523.25, 80.0, 0.38); /* C5 */
    buffer_append_silence(b, 20.0);
    buffer_append_tone(b, 659.25, 80.0, 0.38); /* E5 */
    buffer_append_silence(b, 20.0);
    buffer_append_tone(b, 783.99, 140.0, 0.42); /* G5 */
}

static void build_route_release(Buffer *b)
{
    buffer_append_tone(b, 783.99, 80.0, 0.36);
    buffer_append_silence(b, 20.0);
    buffer_append_tone(b, 523.25, 150.0, 0.36);
}

static void build_train_depart(Buffer *b)
{
    buffer_append_tone(b, 440.0, 70.0, 0.36);
    buffer_append_silence(b, 20.0);
    buffer_append_tone(b, 587.33, 70.0, 0.36);
    buffer_append_silence(b, 20.0);
    buffer_append_tone(b, 880.0, 120.0, 0.40);
}

static void build_train_arrive(Buffer *b)
{
    buffer_append_tone(b, 880.0, 80.0, 0.38);
    buffer_append_silence(b, 25.0);
    buffer_append_tone(b, 587.33, 80.0, 0.36);
    buffer_append_silence(b, 25.0);
    buffer_append_tone(b, 440.0, 140.0, 0.36);
}

static void build_warning(Buffer *b)
{
    buffer_append_tone(b, 700.0, 130.0, 0.52);
    buffer_append_silence(b, 70.0);
    buffer_append_tone(b, 700.0, 130.0, 0.52);
}

static void build_alarm(Buffer *b)
{
    int i;
    for (i = 0; i < 3; ++i)
    {
        buffer_append_tone(b, 800.0, 110.0, 0.60);
        buffer_append_silence(b, 60.0);
    }
}

static void build_track_fail(Buffer *b)
{
    /* A slow pulsing low buzz: something is wrong underneath the train. */
    buffer_append_pulse(b, 190.0, 900.0, 0.50, 3.5);
}

static void build_collision(Buffer *b)
{
    buffer_append_pulse(b, 320.0, 500.0, 0.70, 9.0);
    buffer_append_silence(b, 90.0);
    buffer_append_pulse(b, 380.0, 500.0, 0.70, 9.0);
}

static void build_emergency(Buffer *b)
{
    /* The alternating two-tone siren. Deliberately the loudest and the most
     * insistent sound in the palette. */
    int i;
    for (i = 0; i < 6; ++i)
    {
        buffer_append_tone(b, 950.0, 210.0, 0.85);
        buffer_append_tone(b, 640.0, 210.0, 0.85);
    }
}

static void build_emergency_clear(Buffer *b)
{
    int i;
    for (i = 0; i < 2; ++i)
    {
        buffer_append_tone(b, 640.0, 160.0, 0.45);
        buffer_append_tone(b, 950.0, 160.0, 0.45);
    }
    buffer_append_silence(b, 60.0);
    buffer_append_tone(b, 1200.0, 220.0, 0.45);
}

static void build_critical(Buffer *b)
{
    int i;
    for (i = 0; i < 4; ++i)
    {
        buffer_append_tone(b, 1400.0, 90.0, 0.80);
        buffer_append_silence(b, 55.0);
        buffer_append_tone(b, 700.0, 90.0, 0.80);
        buffer_append_silence(b, 55.0);
    }
}

/* --------------------------------------------------------------------------
 * Bitmap writing
 * -------------------------------------------------------------------------- */
typedef struct
{
    unsigned char r, g, b;
} Pixel;

/* A 24-bit uncompressed BMP. Chosen because LoadImage() in the Win32 API
 * reads it with no dependency at all, unlike PNG or JPEG. */
static int write_bmp(const char *path, int width, int height, const Pixel *pixels)
{
    FILE *f = fopen(path, "wb");
    int x;
    int y;

    if (f == NULL)
    {
        fprintf(stderr, "cannot write %s\n", path);
        return -1;
    }

    {
        const int row_bytes = (width * 3 + 3) & ~3; /* rows padded to 4 bytes */
        const int image_bytes = row_bytes * height;
        const int file_bytes = 54 + image_bytes;

        /* BITMAPFILEHEADER */
        fputc('B', f);
        fputc('M', f);
        write_u32(f, (uint32_t)file_bytes);
        write_u16(f, 0);
        write_u16(f, 0);
        write_u32(f, 54);

        /* BITMAPINFOHEADER */
        write_u32(f, 40);
        write_u32(f, (uint32_t)width);
        write_u32(f, (uint32_t)height);
        write_u16(f, 1);
        write_u16(f, 24);
        write_u32(f, 0); /* BI_RGB */
        write_u32(f, (uint32_t)image_bytes);
        write_u32(f, 2835); /* 72 DPI */
        write_u32(f, 2835);
        write_u32(f, 0);
        write_u32(f, 0);

        /* Pixels, bottom row first, BGR order. */
        for (y = height - 1; y >= 0; --y)
        {
            int written = 0;
            for (x = 0; x < width; ++x)
            {
                const Pixel *p = &pixels[y * width + x];
                fputc(p->b, f);
                fputc(p->g, f);
                fputc(p->r, f);
                written += 3;
            }
            while (written < row_bytes)
            {
                fputc(0, f);
                written++;
            }
        }
    }

    fclose(f);
    printf("  images/%s  (%dx%d)\n",
           strrchr(path, '/') != NULL ? strrchr(path, '/') + 1 : path,
           width, height);
    return 0;
}

/* --------------------------------------------------------------------------
 * Icons. Each is drawn procedurally from a shape description so the set is
 * consistent in weight and colour.
 * -------------------------------------------------------------------------- */
typedef enum
{
    SHAPE_CIRCLE,
    SHAPE_TRIANGLE,
    SHAPE_SQUARE,
    SHAPE_DIAMOND,
    SHAPE_RING,
    SHAPE_CROSS,
    SHAPE_BAR
} Shape;

static Pixel blend(Pixel a, Pixel b, double t)
{
    Pixel out;
    if (t < 0.0)
    {
        t = 0.0;
    }
    if (t > 1.0)
    {
        t = 1.0;
    }
    out.r = (unsigned char)(a.r + (b.r - a.r) * t);
    out.g = (unsigned char)(a.g + (b.g - a.g) * t);
    out.b = (unsigned char)(a.b + (b.b - a.b) * t);
    return out;
}

static Pixel make_icon_pixel(Shape shape, double u, double v, Pixel fg, Pixel bg)
{
    /* u, v in [-1, 1], origin at the centre. */
    double d = 999.0;

    switch (shape)
    {
    case SHAPE_CIRCLE:
        d = sqrt(u * u + v * v);
        break;
    case SHAPE_TRIANGLE:
    {
        /* Simple upright triangle: inside when below the two edges. */
        const double top = 1.0 - fabs(u) * 1.1;
        d = (v + 1.0) * 0.5 - top * 0.9;
        d = (d < 0.0) ? -1.0 : 1.0;
        break;
    }
    case SHAPE_SQUARE:
        d = (fabs(u) > fabs(v) ? fabs(u) : fabs(v));
        break;
    case SHAPE_DIAMOND:
        d = fabs(u) + fabs(v);
        break;
    case SHAPE_RING:
        d = fabs(sqrt(u * u + v * v) - 0.62);
        break;
    case SHAPE_CROSS:
        d = (fabs(u) < 0.28 || fabs(v) < 0.28) ? 0.0 : 1.0;
        break;
    case SHAPE_BAR:
        d = (fabs(v) < 0.34) ? 0.0 : 1.0;
        break;
    }

    /* Anti-alias the boundary so the icons do not look like pixel art. */
    if (d <= 0.0)
    {
        return fg;
    }
    if (d <= 0.06)
    {
        return blend(fg, bg, d / 0.06);
    }
    return bg;
}

static int make_icon(const char *directory, const char *name, int size,
                     Shape shape, Pixel fg, Pixel bg)
{
    char path[512];
    Pixel *pixels = (Pixel *)malloc((size_t)size * (size_t)size * sizeof(Pixel));
    int x;
    int y;

    if (pixels == NULL)
    {
        return -1;
    }

    for (y = 0; y < size; ++y)
    {
        for (x = 0; x < size; ++x)
        {
            const double u = (x + 0.5) / size * 2.0 - 1.0;
            const double v = 1.0 - ((y + 0.5) / size * 2.0);
            pixels[y * size + x] = make_icon_pixel(shape, u, v, fg, bg);
        }
    }

    snprintf(path, sizeof(path), "%s/%s.bmp", directory, name);
    {
        const int result = write_bmp(path, size, size, pixels);
        free(pixels);
        return result;
    }
}

/* --------------------------------------------------------------------------
 * Panel legend: the colour swatch strip, which documents what every colour on
 * the diagram means. Keeping it as a generated asset means the legend can
 * never drift out of step with the renderer.
 * -------------------------------------------------------------------------- */
static int make_legend(const char *directory)
{
    const int swatch = 48;
    const int count = 12;
    char path[512];
    Pixel *pixels;
    int i;
    int x;
    int y;

    static const Pixel colours[12] = {
        {30, 30, 30},    /* background / plain line      */
        {90, 90, 90},    /* track outline                */
        {0, 170, 0},     /* signal green                 */
        {230, 180, 0},   /* signal yellow                */
        {200, 30, 30},   /* signal red                   */
        {200, 30, 30},   /* track occupied               */
        {150, 60, 200},  /* track unknown / failed       */
        {240, 240, 240}, /* train                        */
        {60, 120, 220},  /* point normal                 */
        {220, 140, 40},  /* point reverse                */
        {200, 30, 30},   /* emergency                    */
        {110, 110, 110}  /* out of service               */
    };
    static const char *names[12] = {
        "background", "track", "signal-clear", "signal-caution", "signal-danger",
        "occupied", "unknown", "train", "point-normal", "point-reverse",
        "emergency", "out-of-service"};

    pixels = (Pixel *)malloc((size_t)swatch * count * swatch * sizeof(Pixel));
    if (pixels == NULL)
    {
        return -1;
    }

    for (i = 0; i < count; ++i)
    {
        for (y = 0; y < swatch; ++y)
        {
            for (x = 0; x < swatch; ++x)
            {
                /* A thin darker border so adjacent swatches stay distinct. */
                const bool border = (x == 0 || y == 0 || x == swatch - 1 || y == swatch - 1);
                Pixel p = colours[i];
                if (border)
                {
                    p.r = (unsigned char)(p.r / 2);
                    p.g = (unsigned char)(p.g / 2);
                    p.b = (unsigned char)(p.b / 2);
                }
                pixels[i * swatch * swatch + y * swatch + x] = p;
            }
        }
    }

    snprintf(path, sizeof(path), "%s/legend.bmp", directory);
    {
        const int result = write_bmp(path, swatch, swatch * count, pixels);
        free(pixels);
        return result;
    }
    (void)names;
}

/* --------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    const char *root = (argc > 1) ? argv[1] : "assets";
    char audio_dir[512];
    char image_dir[512];
    char command[600];

    printf("RailControl asset generator\n");
    printf("----------------------------\n");

    snprintf(audio_dir, sizeof(audio_dir), "%s/audio", root);
    snprintf(image_dir, sizeof(image_dir), "%s/images", root);

    /* Create the directories. Using the shell here avoids a platform specific
     * mkdir and keeps the tool to a single portable file. */
#if defined(_WIN32)
    snprintf(command, sizeof(command), "if not exist \"%s\" mkdir \"%s\"", audio_dir, audio_dir);
    if (system(command) != 0)
    { /* best effort */
    }
    snprintf(command, sizeof(command), "if not exist \"%s\" mkdir \"%s\"", image_dir, image_dir);
    if (system(command) != 0)
    { /* best effort */
    }
#else
    snprintf(command, sizeof(command), "mkdir -p \"%s\" \"%s\"", audio_dir, image_dir);
    if (system(command) != 0)
    { /* best effort */
    }
#endif

    printf("\nAudio:\n");
    make_sound(audio_dir, "keypress", build_keypress);
    make_sound(audio_dir, "refused", build_refused);
    make_sound(audio_dir, "acknowledge", build_acknowledge);
    make_sound(audio_dir, "signal_clear", build_signal_clear);
    make_sound(audio_dir, "signal_danger", build_signal_danger);
    make_sound(audio_dir, "point_move", build_point_move);
    make_sound(audio_dir, "point_lock", build_point_lock);
    make_sound(audio_dir, "route_set", build_route_set);
    make_sound(audio_dir, "route_release", build_route_release);
    make_sound(audio_dir, "train_depart", build_train_depart);
    make_sound(audio_dir, "train_arrive", build_train_arrive);
    make_sound(audio_dir, "warning", build_warning);
    make_sound(audio_dir, "alarm", build_alarm);
    make_sound(audio_dir, "track_fail", build_track_fail);
    make_sound(audio_dir, "collision", build_collision);
    make_sound(audio_dir, "emergency", build_emergency);
    make_sound(audio_dir, "emergency_clear", build_emergency_clear);
    make_sound(audio_dir, "critical", build_critical);

    printf("\nImages:\n");
    {
        const Pixel transparent = {24, 24, 28}; /* matches the panel background */
        const Pixel green = {0, 190, 0};
        const Pixel yellow = {235, 185, 0};
        const Pixel red = {210, 40, 40};
        const Pixel blue = {70, 130, 230};
        const Pixel orange = {225, 145, 45};
        const Pixel white = {235, 235, 235};
        const Pixel grey = {120, 120, 120};

        make_icon(image_dir, "signal_clear", 32, SHAPE_CIRCLE, green, transparent);
        make_icon(image_dir, "signal_caution", 32, SHAPE_CIRCLE, yellow, transparent);
        make_icon(image_dir, "signal_danger", 32, SHAPE_CIRCLE, red, transparent);
        make_icon(image_dir, "point_normal", 32, SHAPE_SQUARE, blue, transparent);
        make_icon(image_dir, "point_reverse", 32, SHAPE_DIAMOND, orange, transparent);
        make_icon(image_dir, "train", 32, SHAPE_BAR, white, transparent);
        make_icon(image_dir, "track_clear", 32, SHAPE_RING, grey, transparent);
        make_icon(image_dir, "track_occupied", 32, SHAPE_RING, red, transparent);
        make_icon(image_dir, "track_unknown", 32, SHAPE_RING, orange, transparent);
        make_icon(image_dir, "warning", 32, SHAPE_TRIANGLE, yellow, transparent);
        make_icon(image_dir, "alarm", 32, SHAPE_TRIANGLE, red, transparent);
        make_icon(image_dir, "emergency", 32, SHAPE_CROSS, red, transparent);
        make_icon(image_dir, "route", 32, SHAPE_DIAMOND, green, transparent);
        make_icon(image_dir, "account", 32, SHAPE_CIRCLE, blue, transparent);
        make_icon(image_dir, "settings", 32, SHAPE_RING, white, transparent);
        make_icon(image_dir, "terminal", 32, SHAPE_SQUARE, green, transparent);

        printf("\nLegend:\n");
        make_legend(image_dir);
    }

    printf("\nDone. Point RailControl at this directory with --assets.\n");
    return 0;
}
