/*
 * Accuracy-focused Morse light decoder for QNX Camera API
 * Based on the QNX camera_example1_callback sample.
 *
 * Improvements over the fast-lock version:
 *   - local spot contrast instead of absolute brightness
 *   - two-frame transition confirmation with first-frame timestamps
 *   - separators are committed only after a valid pulse completes
 *   - midpoint gap thresholds tolerate 30 fps quantization
 *   - isolated invalid pulses do not immediately discard the lock
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <termios.h>
#include <sys/stat.h>
#include <errno.h>

#include <camera/camera_api.h>

#define OUTPUT_DIR             "/data/home/qnxuser/morse_output"
#define OUTPUT_FILE            OUTPUT_DIR "/output.txt"

#define TARGET_CAMERA_FPS      30.0      /* 3 frames/dot, 6 frames/dash */
#define PREFERRED_WIDTH        1536U     /* lower CPU load than 2304x1296 */
#define PREFERRED_HEIGHT       864U
#define MORSE_UNIT_US          100000LL  /* dot = 100 ms, dash = 200 ms */
#define DOT_EXPECTED_US        (MORSE_UNIT_US)
#define DASH_EXPECTED_US       (MORSE_UNIT_US * 2)
#define PULSE_TOLERANCE_US     70000LL
/* Midpoints between expected 1/3/7-unit dark gaps. At 30 fps, requiring
 * measured gaps to reach exactly 300/700 ms can lose one frame and merge
 * letters or words. */
#define LETTER_GAP_SPLIT_US    (MORSE_UNIT_US * 2)
#define WORD_GAP_SPLIT_US      (MORSE_UNIT_US * 5)

/* Search for a localized temporal rise, verify that it falls again after a
 * valid 100/200 ms pulse, and only then begin writing Morse output. */
#define TRACK_SAMPLE_STEP             4U
#define TRACK_SPOT_INNER_RADIUS       3U
#define TRACK_SPOT_OUTER_RADIUS       7U
#define TRACK_SPOT_TOP_K              8U
#define TRACK_RECENTER_RADIUS         3U
#define ACQUIRE_TOP_CANDIDATES        24U
#define ACQUIRE_EDGE_MARGIN_SAMPLES   8U
#define WARMUP_FRAMES                 6U
#define ACQUIRE_PIXEL_RISE            20.0
#define ACQUIRE_CLUSTER_RISE          12.0
#define ACQUIRE_SCORE_RISE            15.0
#define ACQUIRE_ABOVE_GLOBAL          10.0
#define LIGHT_ON_DELTA                10.0
#define LIGHT_OFF_DELTA               4.0
#define LIGHT_ON_FRACTION             0.42
#define LIGHT_OFF_FRACTION            0.22
#define BASELINE_ALPHA                0.04
#define AMPLITUDE_ALPHA               0.12
#define TRANSITION_CONFIRM_FRAMES     2U
#define MAX_INVALID_PULSES            3U
#define ACQUIRE_TIMEOUT_US            340000LL
#define REACQUIRE_DARK_US             5000000LL
#define MAX_ON_US                     400000LL
#define MIN_PULSE_US                  40000LL
#define MAX_PULSE_US                  280000LL
#define STATUS_INTERVAL_US            500000LL  /* reduce callback-side console I/O */

typedef struct {
    camera_frametype_t format;
    camera_res_t resolution;
    double fps;
    bool valid;
} camera_mode_choice_t;

typedef struct {
    bool initialized;
    bool have_symbol;
    bool pulse_active;
    int64_t dark_started_us;
    int64_t pulse_started_us;
    int64_t pending_gap_us;
} morse_decoder_t;

typedef struct {
    bool initialized;
    bool locked;
    bool light_on;
    bool candidate_active;
    bool pending_transition;
    bool pending_light_on;
    uint32_t pending_frames;
    uint32_t invalid_pulses;
    uint32_t warmup_remaining;
    uint32_t frame_width;
    uint32_t frame_height;
    uint32_t grid_width;
    uint32_t grid_height;
    uint8_t *previous;
    uint8_t *current;
    uint32_t roi_x;
    uint32_t roi_y;
    uint32_t candidate_x;
    uint32_t candidate_y;
    double candidate_off_score;
    double candidate_peak_score;
    int64_t candidate_started_us;
    double baseline;
    double amplitude;
    double peak_score;
    double score;
    double delta;
    double max_rise;
    double cluster_rise;
    double global_change;
    int64_t last_transition_us;
    int64_t pending_started_us;
} flash_tracker_t;

static FILE *g_output = NULL;
static morse_decoder_t g_morse;
static flash_tracker_t g_tracker;

static void list_available_cameras(void);
static void process_camera_data(camera_handle_t handle,
                                camera_buffer_t *buffer,
                                void *arg);
static void block_on_key_press(void);
static bool can_process_frametype(camera_frametype_t format);
static const char *frametype_name(camera_frametype_t format);
static int choose_best_camera_mode(camera_handle_t handle,
                                   camera_mode_choice_t *choice);
static bool get_frame_geometry(const camera_buffer_t *buffer,
                               uint32_t *width,
                               uint32_t *height);
static bool sample_luma_grid(const camera_buffer_t *buffer,
                             flash_tracker_t *tracker);
static bool update_flash_tracker(const camera_buffer_t *buffer,
                                 bool *light_on);
static void destroy_flash_tracker(void);
static void reset_morse_state_machine(int64_t timestamp_us);
static bool classify_pulse(int64_t duration_us, char *symbol);
static bool process_morse_state_machine(bool light_on, int64_t timestamp_us);
static int open_output_file(void);
static void write_morse_symbol(char symbol);

static bool can_process_frametype(camera_frametype_t format)
{
    switch (format) {
    case CAMERA_FRAMETYPE_NV12:
    case CAMERA_FRAMETYPE_YCBYCR:
    case CAMERA_FRAMETYPE_CBYCRY:
    case CAMERA_FRAMETYPE_RGB8888:
    case CAMERA_FRAMETYPE_BGR8888:
        return true;
    default:
        return false;
    }
}

static const char *frametype_name(camera_frametype_t format)
{
    switch (format) {
    case CAMERA_FRAMETYPE_NV12:
        return "NV12";
    case CAMERA_FRAMETYPE_YCBYCR:
        return "YCBYCR";
    case CAMERA_FRAMETYPE_CBYCRY:
        return "CBYCRY";
    case CAMERA_FRAMETYPE_RGB8888:
        return "RGB8888";
    case CAMERA_FRAMETYPE_BGR8888:
        return "BGR8888";
    default:
        return "OTHER";
    }
}

static int open_output_file(void)
{
    if (mkdir(OUTPUT_DIR, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "Failed to create %s: %s\n", OUTPUT_DIR, strerror(errno));
        return -1;
    }

    g_output = fopen(OUTPUT_FILE, "w");
    if (g_output == NULL) {
        fprintf(stderr, "Failed to open %s: %s\n", OUTPUT_FILE, strerror(errno));
        return -1;
    }

    return 0;
}

static void write_morse_symbol(char symbol)
{
    if (g_output == NULL) {
        return;
    }

    fputc(symbol, g_output);
    fflush(g_output);
}

static void reset_morse_state_machine(int64_t timestamp_us)
{
    memset(&g_morse, 0, sizeof(g_morse));
    g_morse.initialized = true;
    g_morse.dark_started_us = timestamp_us;
}

static int64_t abs_i64(int64_t value)
{
    return value < 0 ? -value : value;
}

static bool classify_pulse(int64_t duration_us, char *symbol)
{
    if (symbol == NULL || duration_us < MIN_PULSE_US ||
        duration_us > MAX_PULSE_US) {
        return false;
    }

    int64_t dot_error = abs_i64(duration_us - DOT_EXPECTED_US);
    int64_t dash_error = abs_i64(duration_us - DASH_EXPECTED_US);
    int64_t best_error = dot_error <= dash_error ? dot_error : dash_error;
    if (best_error > PULSE_TOLERANCE_US) {
        return false;
    }

    *symbol = dot_error <= dash_error ? '.' : '-';
    return true;
}

static bool process_morse_state_machine(bool light_on, int64_t timestamp_us)
{
    if (!g_morse.initialized) {
        reset_morse_state_machine(timestamp_us);
    }

    if (light_on) {
        if (g_morse.pulse_active) {
            return false;
        }

        g_morse.pending_gap_us = timestamp_us - g_morse.dark_started_us;
        if (g_morse.pending_gap_us < 0) {
            reset_morse_state_machine(timestamp_us);
            g_morse.pending_gap_us = 0;
        }
        g_morse.pulse_started_us = timestamp_us;
        g_morse.pulse_active = true;
        return true;
    }

    if (!g_morse.pulse_active) {
        return false;
    }

    int64_t duration_us = timestamp_us - g_morse.pulse_started_us;
    char symbol = '\0';
    g_morse.pulse_active = false;

    if (!classify_pulse(duration_us, &symbol)) {
        /* Do not emit the pending gap. Keeping dark_started_us unchanged
         * treats a rejected flicker as though the transmitter stayed off. */
        return false;
    }

    if (g_morse.have_symbol) {
        if (g_morse.pending_gap_us >= WORD_GAP_SPLIT_US) {
            write_morse_symbol('/');
        } else if (g_morse.pending_gap_us >= LETTER_GAP_SPLIT_US) {
            write_morse_symbol(' ');
        }
    }

    write_morse_symbol(symbol);
    g_morse.have_symbol = true;
    g_morse.dark_started_us = timestamp_us;
    g_morse.pending_gap_us = 0;
    return true;
}

static double pick_rate(const double *rates,
                        uint32_t count,
                        bool maxmin)
{
    if (rates == NULL || count == 0) {
        return 0.0;
    }

    if (maxmin && count >= 2) {
        /* QNX documents rates[0] as max and rates[1] as min.
         * Use comparisons anyway so the code is robust to ordering. */
        double max_rate = (rates[0] > rates[1]) ? rates[0] : rates[1];
        double min_rate = (rates[0] < rates[1]) ? rates[0] : rates[1];
        if (TARGET_CAMERA_FPS >= min_rate && TARGET_CAMERA_FPS <= max_rate) {
            return TARGET_CAMERA_FPS;
        }
        return max_rate;
    }

    double highest = rates[0];
    double closest_at_or_above = 0.0;

    for (uint32_t i = 0; i < count; ++i) {
        if (rates[i] > highest) {
            highest = rates[i];
        }
        if (rates[i] >= TARGET_CAMERA_FPS &&
            (closest_at_or_above == 0.0 || rates[i] < closest_at_or_above)) {
            closest_at_or_above = rates[i];
        }
    }

    return (closest_at_or_above > 0.0) ? closest_at_or_above : highest;
}

static int frametype_preference(camera_frametype_t format)
{
    switch (format) {
    case CAMERA_FRAMETYPE_NV12:
        return 0;  /* fastest luma access */
    case CAMERA_FRAMETYPE_YCBYCR:
    case CAMERA_FRAMETYPE_CBYCRY:
        return 1;
    case CAMERA_FRAMETYPE_RGB8888:
    case CAMERA_FRAMETYPE_BGR8888:
        return 2;
    default:
        return 3;
    }
}

static uint64_t resolution_distance(camera_res_t resolution)
{
    uint64_t dx = (resolution.width > PREFERRED_WIDTH)
                      ? (uint64_t)(resolution.width - PREFERRED_WIDTH)
                      : (uint64_t)(PREFERRED_WIDTH - resolution.width);
    uint64_t dy = (resolution.height > PREFERRED_HEIGHT)
                      ? (uint64_t)(resolution.height - PREFERRED_HEIGHT)
                      : (uint64_t)(PREFERRED_HEIGHT - resolution.height);
    return dx + dy;
}

static bool candidate_is_better(camera_frametype_t format,
                                camera_res_t resolution,
                                double fps,
                                const camera_mode_choice_t *best)
{
    if (!best->valid) {
        return true;
    }

    bool reaches_target = fps >= TARGET_CAMERA_FPS;
    bool best_reaches_target = best->fps >= TARGET_CAMERA_FPS;
    if (reaches_target != best_reaches_target) {
        return reaches_target;
    }

    if (!reaches_target && fps != best->fps) {
        return fps > best->fps;
    }

    int format_rank = frametype_preference(format);
    int best_format_rank = frametype_preference(best->format);
    if (format_rank != best_format_rank) {
        return format_rank < best_format_rank;
    }

    double fps_distance = fps > TARGET_CAMERA_FPS
                              ? fps - TARGET_CAMERA_FPS
                              : TARGET_CAMERA_FPS - fps;
    double best_fps_distance = best->fps > TARGET_CAMERA_FPS
                                   ? best->fps - TARGET_CAMERA_FPS
                                   : TARGET_CAMERA_FPS - best->fps;
    if (fps_distance != best_fps_distance) {
        return fps_distance < best_fps_distance;
    }

    uint64_t distance = resolution_distance(resolution);
    uint64_t best_distance = resolution_distance(best->resolution);
    if (distance != best_distance) {
        return distance < best_distance;
    }

    uint64_t pixels = (uint64_t)resolution.width * resolution.height;
    uint64_t best_pixels =
        (uint64_t)best->resolution.width * best->resolution.height;
    return pixels < best_pixels;
}

static int choose_best_camera_mode(camera_handle_t handle,
                                   camera_mode_choice_t *choice)
{
    int err;
    uint32_t num_formats = 0;
    camera_frametype_t *formats = NULL;

    memset(choice, 0, sizeof(*choice));

    err = camera_get_supported_vf_frame_types(handle, 0, &num_formats, NULL);
    if (err != CAMERA_EOK || num_formats == 0) {
        fprintf(stderr, "Failed to query viewfinder frame types: err=%d\n", err);
        return -1;
    }

    formats = calloc(num_formats, sizeof(*formats));
    if (formats == NULL) {
        fprintf(stderr, "Out of memory while reading frame types\n");
        return -1;
    }

    err = camera_get_supported_vf_frame_types(handle,
                                               num_formats,
                                               &num_formats,
                                               formats);
    if (err != CAMERA_EOK) {
        fprintf(stderr, "Failed to read viewfinder frame types: err=%d\n", err);
        free(formats);
        return -1;
    }

    printf("Camera reports %u viewfinder frame type(s):\n", num_formats);

    for (uint32_t fi = 0; fi < num_formats; ++fi) {
        camera_frametype_t format = formats[fi];
        uint32_t num_resolutions = 0;
        camera_res_t *resolutions = NULL;

        printf("  format %d (%s)%s\n",
               (int)format,
               frametype_name(format),
               can_process_frametype(format) ? "" : " [not handled by decoder]");

        if (!can_process_frametype(format)) {
            continue;
        }

        err = camera_get_specified_vf_resolutions(handle,
                                                   format,
                                                   0,
                                                   &num_resolutions,
                                                   NULL);
        if (err != CAMERA_EOK || num_resolutions == 0) {
            printf("    no resolutions reported (err=%d)\n", err);
            continue;
        }

        resolutions = calloc(num_resolutions, sizeof(*resolutions));
        if (resolutions == NULL) {
            fprintf(stderr, "Out of memory while reading resolutions\n");
            free(formats);
            return -1;
        }

        err = camera_get_specified_vf_resolutions(handle,
                                                   format,
                                                   num_resolutions,
                                                   &num_resolutions,
                                                   resolutions);
        if (err != CAMERA_EOK) {
            printf("    failed to read resolutions (err=%d)\n", err);
            free(resolutions);
            continue;
        }

        for (uint32_t ri = 0; ri < num_resolutions; ++ri) {
            camera_res_t resolution = resolutions[ri];
            uint32_t num_rates = 0;
            double *rates = NULL;
            bool maxmin = false;

            err = camera_get_specified_vf_framerates(handle,
                                                      format,
                                                      resolution,
                                                      0,
                                                      &num_rates,
                                                      NULL,
                                                      &maxmin);
            if (err != CAMERA_EOK || num_rates == 0) {
                printf("    %ux%u: no frame rates (err=%d)\n",
                       resolution.width, resolution.height, err);
                continue;
            }

            rates = calloc(num_rates, sizeof(*rates));
            if (rates == NULL) {
                fprintf(stderr, "Out of memory while reading frame rates\n");
                free(resolutions);
                free(formats);
                return -1;
            }

            err = camera_get_specified_vf_framerates(handle,
                                                      format,
                                                      resolution,
                                                      num_rates,
                                                      &num_rates,
                                                      rates,
                                                      &maxmin);
            if (err != CAMERA_EOK || num_rates == 0) {
                printf("    %ux%u: failed to read frame rates (err=%d)\n",
                       resolution.width, resolution.height, err);
                free(rates);
                continue;
            }

            double selected_rate = pick_rate(rates, num_rates, maxmin);
            printf("    %ux%u: ", resolution.width, resolution.height);
            if (maxmin && num_rates >= 2) {
                double max_rate = (rates[0] > rates[1]) ? rates[0] : rates[1];
                double min_rate = (rates[0] < rates[1]) ? rates[0] : rates[1];
                printf("%.2f..%.2f fps", min_rate, max_rate);
            } else {
                printf("%u discrete rate(s), best %.2f fps",
                       num_rates, selected_rate);
            }
            printf("\n");

            if (selected_rate > 0.0 &&
                candidate_is_better(format, resolution, selected_rate, choice)) {
                choice->format = format;
                choice->resolution = resolution;
                choice->fps = selected_rate;
                choice->valid = true;
            }

            free(rates);
        }

        free(resolutions);
    }

    free(formats);
    return choice->valid ? 0 : -1;
}

static bool get_frame_geometry(const camera_buffer_t *buffer,
                               uint32_t *width,
                               uint32_t *height)
{
    if (buffer == NULL || width == NULL || height == NULL) {
        return false;
    }

    switch (buffer->frametype) {
    case CAMERA_FRAMETYPE_NV12:
        *width = buffer->framedesc.nv12.width;
        *height = buffer->framedesc.nv12.height;
        return true;
    case CAMERA_FRAMETYPE_YCBYCR:
        *width = buffer->framedesc.ycbycr.width;
        *height = buffer->framedesc.ycbycr.height;
        return true;
    case CAMERA_FRAMETYPE_CBYCRY:
        *width = buffer->framedesc.cbycry.width;
        *height = buffer->framedesc.cbycry.height;
        return true;
    case CAMERA_FRAMETYPE_RGB8888:
        *width = buffer->framedesc.rgb8888.width;
        *height = buffer->framedesc.rgb8888.height;
        return true;
    case CAMERA_FRAMETYPE_BGR8888:
        *width = buffer->framedesc.bgr8888.width;
        *height = buffer->framedesc.bgr8888.height;
        return true;
    default:
        return false;
    }
}

static uint8_t luma_at(const camera_buffer_t *buffer, uint32_t x, uint32_t y)
{
    switch (buffer->frametype) {
    case CAMERA_FRAMETYPE_NV12:
    {
        uint32_t stride = buffer->framedesc.nv12.stride;
        return buffer->framebuf[(size_t)y * stride + x];
    }
    case CAMERA_FRAMETYPE_YCBYCR:
    {
        uint32_t stride = buffer->framedesc.ycbycr.stride;
        return buffer->framebuf[(size_t)y * stride + 2U * x];
    }
    case CAMERA_FRAMETYPE_CBYCRY:
    {
        uint32_t stride = buffer->framedesc.cbycry.stride;
        return buffer->framebuf[(size_t)y * stride + 2U * x + 1U];
    }
    case CAMERA_FRAMETYPE_RGB8888:
    {
        uint32_t stride = buffer->framedesc.rgb8888.stride;
        const uint8_t *pixel = buffer->framebuf + (size_t)y * stride + 4U * x;
        return (uint8_t)(((uint32_t)pixel[0] + pixel[1] + pixel[2]) / 3U);
    }
    case CAMERA_FRAMETYPE_BGR8888:
    {
        uint32_t stride = buffer->framedesc.bgr8888.stride;
        const uint8_t *pixel = buffer->framebuf + (size_t)y * stride + 4U * x;
        return (uint8_t)(((uint32_t)pixel[0] + pixel[1] + pixel[2]) / 3U);
    }
    default:
        return 0;
    }
}

static void destroy_flash_tracker(void)
{
    free(g_tracker.previous);
    free(g_tracker.current);
    memset(&g_tracker, 0, sizeof(g_tracker));
}

static bool prepare_flash_tracker(const camera_buffer_t *buffer)
{
    uint32_t width = 0;
    uint32_t height = 0;

    if (!get_frame_geometry(buffer, &width, &height) || width == 0 || height == 0) {
        return false;
    }

    uint32_t grid_width = (width + TRACK_SAMPLE_STEP - 1U) / TRACK_SAMPLE_STEP;
    uint32_t grid_height = (height + TRACK_SAMPLE_STEP - 1U) / TRACK_SAMPLE_STEP;

    if (g_tracker.initialized &&
        g_tracker.frame_width == width &&
        g_tracker.frame_height == height &&
        g_tracker.grid_width == grid_width &&
        g_tracker.grid_height == grid_height) {
        return true;
    }

    destroy_flash_tracker();

    size_t count = (size_t)grid_width * grid_height;
    g_tracker.previous = calloc(count, sizeof(*g_tracker.previous));
    g_tracker.current = calloc(count, sizeof(*g_tracker.current));
    if (g_tracker.previous == NULL || g_tracker.current == NULL) {
        fprintf(stderr, "Failed to allocate flash-tracking buffers\n");
        destroy_flash_tracker();
        return false;
    }

    g_tracker.frame_width = width;
    g_tracker.frame_height = height;
    g_tracker.grid_width = grid_width;
    g_tracker.grid_height = grid_height;
    g_tracker.warmup_remaining = WARMUP_FRAMES;
    return true;
}

static bool sample_luma_grid(const camera_buffer_t *buffer,
                             flash_tracker_t *tracker)
{
    if (buffer == NULL || buffer->framebuf == NULL || tracker == NULL ||
        tracker->current == NULL) {
        return false;
    }

    for (uint32_t gy = 0; gy < tracker->grid_height; ++gy) {
        uint32_t y = gy * TRACK_SAMPLE_STEP;
        if (y >= tracker->frame_height) {
            y = tracker->frame_height - 1U;
        }

        for (uint32_t gx = 0; gx < tracker->grid_width; ++gx) {
            uint32_t x = gx * TRACK_SAMPLE_STEP;
            if (x >= tracker->frame_width) {
                x = tracker->frame_width - 1U;
            }

            tracker->current[(size_t)gy * tracker->grid_width + gx] =
                luma_at(buffer, x, y);
        }
    }

    return true;
}

static double roi_spot_contrast_score(const uint8_t *grid,
                                      uint32_t grid_width,
                                      uint32_t grid_height,
                                      uint32_t center_x,
                                      uint32_t center_y)
{
    double top[TRACK_SPOT_TOP_K] = {0.0};
    uint32_t top_used = 0;
    double ring_sum = 0.0;
    uint32_t ring_count = 0;

    uint32_t min_x = center_x > TRACK_SPOT_OUTER_RADIUS
                         ? center_x - TRACK_SPOT_OUTER_RADIUS
                         : 0U;
    uint32_t min_y = center_y > TRACK_SPOT_OUTER_RADIUS
                         ? center_y - TRACK_SPOT_OUTER_RADIUS
                         : 0U;
    uint32_t max_x = center_x + TRACK_SPOT_OUTER_RADIUS;
    uint32_t max_y = center_y + TRACK_SPOT_OUTER_RADIUS;
    if (max_x >= grid_width) {
        max_x = grid_width - 1U;
    }
    if (max_y >= grid_height) {
        max_y = grid_height - 1U;
    }

    for (uint32_t y = min_y; y <= max_y; ++y) {
        for (uint32_t x = min_x; x <= max_x; ++x) {
            uint32_t dx = x > center_x ? x - center_x : center_x - x;
            uint32_t dy = y > center_y ? y - center_y : center_y - y;
            double value = grid[(size_t)y * grid_width + x];

            if (dx <= TRACK_SPOT_INNER_RADIUS &&
                dy <= TRACK_SPOT_INNER_RADIUS) {
                uint32_t pos;
                if (top_used < TRACK_SPOT_TOP_K) {
                    pos = top_used++;
                } else {
                    if (value <= top[TRACK_SPOT_TOP_K - 1U]) {
                        continue;
                    }
                    pos = TRACK_SPOT_TOP_K - 1U;
                }

                while (pos > 0U && value > top[pos - 1U]) {
                    top[pos] = top[pos - 1U];
                    --pos;
                }
                top[pos] = value;
            } else {
                ring_sum += value;
                ++ring_count;
            }
        }
    }

    if (top_used == 0U || ring_count == 0U) {
        return 0.0;
    }

    double inner_sum = 0.0;
    for (uint32_t i = 0; i < top_used; ++i) {
        inner_sum += top[i];
    }

    return inner_sum / top_used - ring_sum / ring_count;
}

static double local_positive_rise(const flash_tracker_t *tracker,
                                  uint32_t center_x,
                                  uint32_t center_y)
{
    const uint32_t radius = 2U;
    const uint32_t top_count = 5U;
    double top_rises[5] = {0.0};
    uint32_t used = 0;
    uint32_t min_x = center_x > radius ? center_x - radius : 0U;
    uint32_t min_y = center_y > radius ? center_y - radius : 0U;
    uint32_t max_x = center_x + radius;
    uint32_t max_y = center_y + radius;
    if (max_x >= tracker->grid_width) {
        max_x = tracker->grid_width - 1U;
    }
    if (max_y >= tracker->grid_height) {
        max_y = tracker->grid_height - 1U;
    }

    for (uint32_t y = min_y; y <= max_y; ++y) {
        for (uint32_t x = min_x; x <= max_x; ++x) {
            size_t index = (size_t)y * tracker->grid_width + x;
            int rise = (int)tracker->current[index] - (int)tracker->previous[index];
            if (rise <= 0) {
                continue;
            }

            double value = rise;
            uint32_t pos;
            if (used < top_count) {
                pos = used++;
            } else {
                if (value <= top_rises[top_count - 1U]) {
                    continue;
                }
                pos = top_count - 1U;
            }

            while (pos > 0U && value > top_rises[pos - 1U]) {
                top_rises[pos] = top_rises[pos - 1U];
                --pos;
            }
            top_rises[pos] = value;
        }
    }

    if (used == 0U) {
        return 0.0;
    }

    double sum = 0.0;
    for (uint32_t i = 0; i < used; ++i) {
        sum += top_rises[i];
    }
    return sum / used;
}

typedef struct {
    double rise;
    size_t index;
} rise_candidate_t;

static double max_double(double a, double b)
{
    return a > b ? a : b;
}

static void recenter_on_brightest_roi(const flash_tracker_t *tracker,
                                      uint32_t *center_x,
                                      uint32_t *center_y,
                                      uint32_t radius)
{
    uint32_t original_x = *center_x;
    uint32_t original_y = *center_y;
    uint32_t min_x = original_x > radius ? original_x - radius : 0U;
    uint32_t min_y = original_y > radius ? original_y - radius : 0U;
    uint32_t max_x = original_x + radius;
    uint32_t max_y = original_y + radius;
    if (max_x >= tracker->grid_width) {
        max_x = tracker->grid_width - 1U;
    }
    if (max_y >= tracker->grid_height) {
        max_y = tracker->grid_height - 1U;
    }

    double best_score = roi_spot_contrast_score(tracker->current,
                                                   tracker->grid_width,
                                                   tracker->grid_height,
                                                   original_x,
                                                   original_y);
    uint32_t best_x = original_x;
    uint32_t best_y = original_y;
    for (uint32_t y = min_y; y <= max_y; ++y) {
        for (uint32_t x = min_x; x <= max_x; ++x) {
            double score = roi_spot_contrast_score(tracker->current,
                                                      tracker->grid_width,
                                                      tracker->grid_height,
                                                      x,
                                                      y);
            /* Do not drift across a flat bright plateau. Move only when the
             * new center is meaningfully brighter than the current center. */
            if (score > best_score + 1.0) {
                best_score = score;
                best_x = x;
                best_y = y;
            }
        }
    }

    *center_x = best_x;
    *center_y = best_y;
}

static bool find_best_rising_candidate(flash_tracker_t *tracker,
                                       uint32_t *best_x,
                                       uint32_t *best_y,
                                       double *previous_score,
                                       double *current_score)
{
    rise_candidate_t top[ACQUIRE_TOP_CANDIDATES];
    uint32_t used = 0;
    size_t count = (size_t)tracker->grid_width * tracker->grid_height;
    double absolute_change_sum = 0.0;
    tracker->max_rise = 0.0;
    tracker->cluster_rise = 0.0;

    uint32_t margin = ACQUIRE_EDGE_MARGIN_SAMPLES;
    if (tracker->grid_width <= 2U * margin ||
        tracker->grid_height <= 2U * margin) {
        margin = 0U;
    }

    for (uint32_t y = 0; y < tracker->grid_height; ++y) {
        for (uint32_t x = 0; x < tracker->grid_width; ++x) {
            size_t index = (size_t)y * tracker->grid_width + x;
            int difference =
                (int)tracker->current[index] - (int)tracker->previous[index];
            absolute_change_sum += difference >= 0 ? difference : -difference;
            if ((double)difference > tracker->max_rise) {
                tracker->max_rise = difference;
            }

            if (difference <= 0 || x < margin || y < margin ||
                x + margin >= tracker->grid_width ||
                y + margin >= tracker->grid_height) {
                continue;
            }

            double rise = difference;
            uint32_t pos;
            if (used < ACQUIRE_TOP_CANDIDATES) {
                pos = used++;
            } else {
                if (rise <= top[ACQUIRE_TOP_CANDIDATES - 1U].rise) {
                    continue;
                }
                pos = ACQUIRE_TOP_CANDIDATES - 1U;
            }

            while (pos > 0U && rise > top[pos - 1U].rise) {
                top[pos] = top[pos - 1U];
                --pos;
            }
            top[pos].rise = rise;
            top[pos].index = index;
        }
    }

    tracker->global_change = count > 0U ? absolute_change_sum / count : 0.0;

    bool found = false;
    double best_quality = 0.0;
    for (uint32_t i = 0; i < used; ++i) {
        if (top[i].rise < ACQUIRE_PIXEL_RISE ||
            top[i].rise < tracker->global_change + ACQUIRE_ABOVE_GLOBAL) {
            continue;
        }

        uint32_t y = (uint32_t)(top[i].index / tracker->grid_width);
        uint32_t x = (uint32_t)(top[i].index % tracker->grid_width);
        double cluster = local_positive_rise(tracker, x, y);
        double before = roi_spot_contrast_score(tracker->previous,
                                                   tracker->grid_width,
                                                   tracker->grid_height,
                                                   x,
                                                   y);
        double now = roi_spot_contrast_score(tracker->current,
                                              tracker->grid_width,
                                              tracker->grid_height,
                                              x,
                                              y);
        double score_rise = now - before;
        if (cluster < ACQUIRE_CLUSTER_RISE ||
            score_rise < ACQUIRE_SCORE_RISE) {
            continue;
        }

        double quality = 2.0 * score_rise + cluster + 0.25 * top[i].rise;
        if (!found || quality > best_quality) {
            found = true;
            best_quality = quality;
            *best_x = x;
            *best_y = y;
            *previous_score = before;
            *current_score = now;
            tracker->cluster_rise = cluster;
        }
    }

    return found;
}

static void cancel_candidate(flash_tracker_t *tracker)
{
    tracker->candidate_active = false;
    tracker->candidate_off_score = 0.0;
    tracker->candidate_peak_score = 0.0;
    tracker->candidate_started_us = 0;
}

static bool begin_candidate(flash_tracker_t *tracker, int64_t timestamp_us)
{
    uint32_t x = 0;
    uint32_t y = 0;
    double before = 0.0;
    double now = 0.0;

    if (!find_best_rising_candidate(tracker, &x, &y, &before, &now)) {
        return false;
    }

    tracker->candidate_active = true;
    tracker->candidate_x = x;
    tracker->candidate_y = y;
    tracker->candidate_off_score = before;
    tracker->candidate_peak_score = now;
    tracker->candidate_started_us = timestamp_us;
    tracker->score = now;
    tracker->delta = now - before;

    printf("\nPotential flash near pixel (%u, %u); verifying pulse...\n",
           x * TRACK_SAMPLE_STEP,
           y * TRACK_SAMPLE_STEP);
    return true;
}

static bool verify_candidate(flash_tracker_t *tracker, int64_t timestamp_us)
{
    recenter_on_brightest_roi(tracker,
                              &tracker->candidate_x,
                              &tracker->candidate_y,
                              TRACK_RECENTER_RADIUS);

    tracker->score = roi_spot_contrast_score(tracker->current,
                                                tracker->grid_width,
                                                tracker->grid_height,
                                                tracker->candidate_x,
                                                tracker->candidate_y);
    if (tracker->score > tracker->candidate_peak_score) {
        tracker->candidate_peak_score = tracker->score;
    }

    double amplitude =
        tracker->candidate_peak_score - tracker->candidate_off_score;
    tracker->delta = tracker->score - tracker->candidate_off_score;
    int64_t duration_us = timestamp_us - tracker->candidate_started_us;
    double off_limit = tracker->candidate_off_score +
                       max_double(LIGHT_OFF_DELTA, 0.30 * amplitude);
    double fall = tracker->candidate_peak_score - tracker->score;
    bool returned_off =
        amplitude >= ACQUIRE_SCORE_RISE &&
        tracker->score <= off_limit &&
        fall >= max_double(8.0, 0.25 * amplitude);

    if (returned_off) {
        char acquired_symbol = '\0';
        if (classify_pulse(duration_us, &acquired_symbol)) {
            tracker->roi_x = tracker->candidate_x;
            tracker->roi_y = tracker->candidate_y;
            tracker->baseline =
                0.5 * (tracker->candidate_off_score + tracker->score);
            tracker->amplitude = amplitude;
            tracker->peak_score = tracker->candidate_peak_score;
            tracker->locked = true;
            tracker->light_on = false;
            tracker->pending_transition = false;
            tracker->pending_frames = 0U;
            tracker->invalid_pulses = 0U;
            tracker->last_transition_us = timestamp_us;

            /* Include the verified acquisition pulse as the first symbol. */
            reset_morse_state_machine(tracker->candidate_started_us);
            (void)process_morse_state_machine(true,
                                              tracker->candidate_started_us);
            (void)process_morse_state_machine(false, timestamp_us);

            printf("\nLocked onto verified flashing source near pixel (%u, %u) "
                   "(pulse %.0f ms = %c, contrast %.1f)\n",
                   tracker->roi_x * TRACK_SAMPLE_STEP,
                   tracker->roi_y * TRACK_SAMPLE_STEP,
                   (double)duration_us / 1000.0,
                   acquired_symbol,
                   tracker->amplitude);
            cancel_candidate(tracker);
            return true;
        }

        cancel_candidate(tracker);
        return false;
    }

    if (duration_us > ACQUIRE_TIMEOUT_US) {
        cancel_candidate(tracker);
    }
    return false;
}

static void lose_lock(flash_tracker_t *tracker, int64_t timestamp_us)
{
    printf("\nLost or rejected flashing source; searching again...\n");
    tracker->locked = false;
    tracker->light_on = false;
    tracker->amplitude = 0.0;
    tracker->peak_score = 0.0;
    tracker->pending_transition = false;
    tracker->pending_frames = 0U;
    tracker->invalid_pulses = 0U;
    cancel_candidate(tracker);
    reset_morse_state_machine(timestamp_us);
}

static void clear_pending_transition(flash_tracker_t *tracker)
{
    tracker->pending_transition = false;
    tracker->pending_frames = 0U;
    tracker->pending_started_us = 0;
}

static bool confirm_transition(flash_tracker_t *tracker,
                               bool desired_light_on,
                               int64_t timestamp_us,
                               int64_t *transition_us)
{
    if (desired_light_on == tracker->light_on) {
        clear_pending_transition(tracker);
        return false;
    }

    if (!tracker->pending_transition ||
        tracker->pending_light_on != desired_light_on) {
        tracker->pending_transition = true;
        tracker->pending_light_on = desired_light_on;
        tracker->pending_frames = 1U;
        tracker->pending_started_us = timestamp_us;
        return false;
    }

    ++tracker->pending_frames;
    if (tracker->pending_frames < TRANSITION_CONFIRM_FRAMES) {
        return false;
    }

    *transition_us = tracker->pending_started_us;
    clear_pending_transition(tracker);
    return true;
}

static bool update_locked_source(flash_tracker_t *tracker,
                                 int64_t timestamp_us,
                                 bool *light_on)
{
    tracker->score = roi_spot_contrast_score(tracker->current,
                                              tracker->grid_width,
                                              tracker->grid_height,
                                              tracker->roi_x,
                                              tracker->roi_y);
    tracker->delta = tracker->score - tracker->baseline;

    bool raw_light_on;
    if (!tracker->light_on) {
        double on_delta = max_double(LIGHT_ON_DELTA,
                                     LIGHT_ON_FRACTION * tracker->amplitude);
        raw_light_on = tracker->delta >= on_delta;

        if (!raw_light_on) {
            tracker->baseline +=
                BASELINE_ALPHA * (tracker->score - tracker->baseline);
        }
    } else {
        recenter_on_brightest_roi(tracker,
                                  &tracker->roi_x,
                                  &tracker->roi_y,
                                  TRACK_RECENTER_RADIUS);
        tracker->score = roi_spot_contrast_score(tracker->current,
                                                  tracker->grid_width,
                                                  tracker->grid_height,
                                                  tracker->roi_x,
                                                  tracker->roi_y);
        tracker->delta = tracker->score - tracker->baseline;
        if (tracker->score > tracker->peak_score) {
            tracker->peak_score = tracker->score;
        }

        double off_limit = tracker->baseline +
                           max_double(LIGHT_OFF_DELTA,
                                      LIGHT_OFF_FRACTION * tracker->amplitude);
        double fall = tracker->peak_score - tracker->score;
        bool looks_off =
            tracker->score <= off_limit &&
            fall >= max_double(6.0, 0.18 * tracker->amplitude);
        raw_light_on = !looks_off;
    }

    int64_t transition_us = 0;
    if (confirm_transition(tracker,
                           raw_light_on,
                           timestamp_us,
                           &transition_us)) {
        if (raw_light_on) {
            recenter_on_brightest_roi(tracker,
                                      &tracker->roi_x,
                                      &tracker->roi_y,
                                      TRACK_RECENTER_RADIUS);
            tracker->score = roi_spot_contrast_score(tracker->current,
                                                      tracker->grid_width,
                                                      tracker->grid_height,
                                                      tracker->roi_x,
                                                      tracker->roi_y);
            tracker->delta = tracker->score - tracker->baseline;
            tracker->light_on = true;
            tracker->peak_score = tracker->score;
            tracker->last_transition_us = transition_us;
            (void)process_morse_state_machine(true, transition_us);
        } else {
            int64_t pulse_us = transition_us - tracker->last_transition_us;
            tracker->light_on = false;
            tracker->last_transition_us = transition_us;

            bool valid = process_morse_state_machine(false, transition_us);
            if (valid) {
                double observed_amplitude =
                    tracker->peak_score - tracker->baseline;
                tracker->amplitude +=
                    AMPLITUDE_ALPHA *
                    (observed_amplitude - tracker->amplitude);
                tracker->invalid_pulses = 0U;
            } else {
                ++tracker->invalid_pulses;
                fprintf(stderr,
                        "\nRejected pulse %.0f ms (%u/%u); keeping lock.\n",
                        (double)pulse_us / 1000.0,
                        tracker->invalid_pulses,
                        MAX_INVALID_PULSES);
            }

            tracker->baseline +=
                BASELINE_ALPHA * (tracker->score - tracker->baseline);

            if (tracker->invalid_pulses >= MAX_INVALID_PULSES) {
                lose_lock(tracker, transition_us);
                *light_on = false;
                return false;
            }
        }
    }

    int64_t unchanged_us = timestamp_us - tracker->last_transition_us;
    if ((!tracker->light_on && unchanged_us >= REACQUIRE_DARK_US) ||
        (tracker->light_on && unchanged_us >= MAX_ON_US)) {
        lose_lock(tracker, timestamp_us);
        *light_on = false;
        return false;
    }

    *light_on = tracker->light_on;
    return true;
}

static bool update_flash_tracker(const camera_buffer_t *buffer,
                                 bool *light_on)
{
    if (light_on == NULL || !prepare_flash_tracker(buffer) ||
        !sample_luma_grid(buffer, &g_tracker)) {
        return false;
    }

    size_t count = (size_t)g_tracker.grid_width * g_tracker.grid_height;

    if (!g_tracker.initialized) {
        memcpy(g_tracker.previous, g_tracker.current, count);
        g_tracker.initialized = true;
        reset_morse_state_machine(buffer->frametimestamp);
        *light_on = false;
        return true;
    }

    if (g_tracker.warmup_remaining > 0U) {
        --g_tracker.warmup_remaining;
        memcpy(g_tracker.previous, g_tracker.current, count);
        *light_on = false;
        return true;
    }

    if (g_tracker.locked) {
        (void)update_locked_source(&g_tracker,
                                   buffer->frametimestamp,
                                   light_on);
    } else if (g_tracker.candidate_active) {
        (void)verify_candidate(&g_tracker, buffer->frametimestamp);
        *light_on = false;
    } else {
        (void)begin_candidate(&g_tracker, buffer->frametimestamp);
        *light_on = false;
    }

    if (g_tracker.locked) {
        *light_on = g_tracker.light_on;
    }

    memcpy(g_tracker.previous, g_tracker.current, count);
    return true;
}

int main(int argc, char *argv[])
{
    int err;
    int opt;
    camera_unit_t unit = CAMERA_UNIT_NONE;
    camera_handle_t handle = CAMERA_HANDLE_INVALID;
    camera_mode_choice_t choice;

    while ((opt = getopt(argc, argv, "u:")) != -1) {
        switch (opt) {
        case 'u':
            unit = (camera_unit_t)strtol(optarg, NULL, 10);
            break;
        default:
            fprintf(stderr, "Usage: %s -u CAMERA_UNIT\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    if ((unit == CAMERA_UNIT_NONE) || (unit >= CAMERA_UNIT_NUM_UNITS)) {
        list_available_cameras();
        printf("Please provide a camera unit with -u\n");
        return EXIT_SUCCESS;
    }

    err = camera_open(unit, CAMERA_MODE_RW, &handle);
    if (err != CAMERA_EOK || handle == CAMERA_HANDLE_INVALID) {
        fprintf(stderr, "Failed to open CAMERA_UNIT_%d: err=%d\n", (int)unit, err);
        return EXIT_FAILURE;
    }

    err = camera_set_vf_mode(handle, CAMERA_VFMODE_VIDEO);
    if (err != CAMERA_EOK) {
        fprintf(stderr, "Failed to set video viewfinder mode: err=%d\n", err);
        camera_close(handle);
        return EXIT_FAILURE;
    }

    if (choose_best_camera_mode(handle, &choice) != 0) {
        fprintf(stderr, "No camera mode usable by this decoder was found.\n");
        camera_close(handle);
        return EXIT_FAILURE;
    }

    printf("Selected: format=%d (%s), %ux%u @ %.2f fps\n",
           (int)choice.format,
           frametype_name(choice.format),
           choice.resolution.width,
           choice.resolution.height,
           choice.fps);

    camera_frametype_t current_format = CAMERA_FRAMETYPE_UNSPECIFIED;
    err = camera_get_vf_property(handle, CAMERA_IMGPROP_FORMAT, &current_format);
    if (err != CAMERA_EOK) {
        fprintf(stderr, "Failed to read current format: err=%d\n", err);
        camera_close(handle);
        return EXIT_FAILURE;
    }

    if (current_format != choice.format) {
        err = camera_set_vf_property(handle,
                                     CAMERA_IMGPROP_FORMAT,
                                     choice.format);
        if (err != CAMERA_EOK) {
            fprintf(stderr,
                    "Failed to set format %d (%s): err=%d\n",
                    (int)choice.format,
                    frametype_name(choice.format),
                    err);
            camera_close(handle);
            return EXIT_FAILURE;
        }
    }

    err = camera_set_vf_property(handle,
                                 CAMERA_IMGPROP_WIDTH,
                                 choice.resolution.width,
                                 CAMERA_IMGPROP_HEIGHT,
                                 choice.resolution.height,
                                 CAMERA_IMGPROP_FRAMERATE,
                                 choice.fps,
                                 CAMERA_IMGPROP_CREATEWINDOW,
                                 0);
    if (err != CAMERA_EOK) {
        fprintf(stderr, "Failed to set resolution/framerate: err=%d\n", err);
        camera_close(handle);
        return EXIT_FAILURE;
    }

    camera_frametype_t actual_format = CAMERA_FRAMETYPE_UNSPECIFIED;
    uint32_t actual_width = 0;
    uint32_t actual_height = 0;
    double actual_fps = 0.0;
    err = camera_get_vf_property(handle,
                                 CAMERA_IMGPROP_FORMAT, &actual_format,
                                 CAMERA_IMGPROP_WIDTH, &actual_width,
                                 CAMERA_IMGPROP_HEIGHT, &actual_height,
                                 CAMERA_IMGPROP_FRAMERATE, &actual_fps);
    if (err != CAMERA_EOK) {
        fprintf(stderr, "Failed to read configured mode: err=%d\n", err);
        camera_close(handle);
        return EXIT_FAILURE;
    }

    printf("Configured: format=%d (%s), %ux%u @ %.2f fps\n",
           (int)actual_format,
           frametype_name(actual_format),
           actual_width,
           actual_height,
           actual_fps);

    if (!can_process_frametype(actual_format)) {
        fprintf(stderr, "Configured frame type is not handled by the decoder.\n");
        camera_close(handle);
        return EXIT_FAILURE;
    }

    if (actual_fps < TARGET_CAMERA_FPS) {
        fprintf(stderr,
                "WARNING: %.2f fps is below the preferred %.2f fps for "
                "100 ms dots and 200 ms dashes. Decoding may be less reliable.\n",
                actual_fps,
                TARGET_CAMERA_FPS);
    }

    if (open_output_file() != 0) {
        camera_close(handle);
        return EXIT_FAILURE;
    }

    err = camera_start_viewfinder(handle, process_camera_data, NULL, NULL);
    if (err != CAMERA_EOK) {
        fprintf(stderr, "Failed to start CAMERA_UNIT_%d: err=%d\n", (int)unit, err);
        fclose(g_output);
        g_output = NULL;
        destroy_flash_tracker();
        camera_close(handle);
        return EXIT_FAILURE;
    }

    printf("Watching for Morse flashes. Press any key to stop.\n");
    block_on_key_press();

    err = camera_stop_viewfinder(handle);
    if (err != CAMERA_EOK) {
        fprintf(stderr, "Failed to stop CAMERA_UNIT_%d: err=%d\n", (int)unit, err);
    }

    if (g_output != NULL) {
        fputc('\n', g_output);
        fclose(g_output);
        g_output = NULL;
    }

    camera_close(handle);
    return (err == CAMERA_EOK) ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void process_camera_data(camera_handle_t handle,
                                camera_buffer_t *buffer,
                                void *arg)
{
    static int64_t last_status_us = 0;
    bool light_on = false;

    (void)handle;
    (void)arg;

    if (!update_flash_tracker(buffer, &light_on)) {
        return;
    }

    if (buffer->frametimestamp - last_status_us >= STATUS_INTERVAL_US) {
        if (g_tracker.locked) {
            printf("\rTRACKING (%u,%u) | contrast %.1f | base %.1f | amp %.1f | "
                   "delta %.1f | %s     ",
                   g_tracker.roi_x * TRACK_SAMPLE_STEP,
                   g_tracker.roi_y * TRACK_SAMPLE_STEP,
                   g_tracker.score,
                   g_tracker.baseline,
                   g_tracker.amplitude,
                   g_tracker.delta,
                   light_on ? "ON " : "OFF");
        } else if (g_tracker.candidate_active) {
            printf("\rVERIFYING (%u,%u) | score %.1f | rise %.1f     ",
                   g_tracker.candidate_x * TRACK_SAMPLE_STEP,
                   g_tracker.candidate_y * TRACK_SAMPLE_STEP,
                   g_tracker.score,
                   g_tracker.delta);
        } else if (g_tracker.warmup_remaining > 0U) {
            printf("\rWARMING UP | %u frame(s) remaining     ",
                   g_tracker.warmup_remaining);
        } else {
            printf("\rSEARCHING | max rise %.1f | cluster %.1f | global %.1f     ",
                   g_tracker.max_rise,
                   g_tracker.cluster_rise,
                   g_tracker.global_change);
        }
        fflush(stdout);
        last_status_us = buffer->frametimestamp;
    }
}

static void list_available_cameras(void)
{
    int err;
    uint32_t num_supported = 0;
    camera_unit_t *cameras = NULL;

    err = camera_get_supported_cameras(0, &num_supported, NULL);
    if (err != CAMERA_EOK) {
        fprintf(stderr, "Failed to get camera count: err=%d\n", err);
        return;
    }

    if (num_supported == 0) {
        printf("No supported cameras detected.\n");
        return;
    }

    cameras = calloc(num_supported, sizeof(*cameras));
    if (cameras == NULL) {
        fprintf(stderr, "Out of memory while listing cameras.\n");
        return;
    }

    err = camera_get_supported_cameras(num_supported,
                                       &num_supported,
                                       cameras);
    if (err == CAMERA_EOK) {
        printf("Available camera units:\n");
        for (uint32_t i = 0; i < num_supported; ++i) {
            printf("  CAMERA_UNIT_%d (use -u %d)\n",
                   (int)cameras[i],
                   (int)cameras[i]);
        }
    } else {
        fprintf(stderr, "Failed to read camera list: err=%d\n", err);
    }

    free(cameras);
}

static void block_on_key_press(void)
{
    struct termios oldterm;
    struct termios newterm;
    char key;

    if (tcgetattr(STDIN_FILENO, &oldterm) != 0) {
        (void)read(STDIN_FILENO, &key, 1);
        return;
    }

    newterm = oldterm;
    newterm.c_lflag &= (tcflag_t)~(ECHO | ICANON);
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &newterm);
    (void)read(STDIN_FILENO, &key, 1);
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &oldterm);
}
