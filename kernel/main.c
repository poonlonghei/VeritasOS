
#include <stdint.h>
#include <stddef.h>
#include <limine.h>

/* =========================================
   VeritasOS Identity
   ========================================= */

#define OS_NAME    "VeritasOS"
#define OS_VERSION "0.1.0"

/* =========================================
   Limine Boot Protocol Requests
   ========================================= */

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t requests_start[] =
    LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t requests_end[] =
    LIMINE_REQUESTS_END_MARKER;

/* =========================================
   Framebuffer
   ========================================= */

static struct limine_framebuffer *fb;

static void halt(void)
{
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

/* Convert an 8-bit color component to the
   framebuffer channel's supported bit width. */

static uint32_t channel(uint8_t value,
                        uint8_t size,
                        uint8_t shift)
{
    if (size == 0 || size > 8 || shift >= 32 ||
        size + shift > 32) {
        return 0;
    }

    uint32_t max_value = (1u << size) - 1u;

    uint32_t scaled =
        ((uint32_t)value * max_value + 127u) / 255u;

    return scaled << shift;
}

/* Draw one pixel. */

static void pixel(uint64_t x, uint64_t y,
                  uint8_t r, uint8_t g, uint8_t b)
{
    if (x >= fb->width || y >= fb->height) {
        return;
    }

    uint32_t color =
        channel(r, fb->red_mask_size, fb->red_mask_shift) |
        channel(g, fb->green_mask_size, fb->green_mask_shift) |
        channel(b, fb->blue_mask_size, fb->blue_mask_shift);

    volatile uint32_t *row =
        (volatile uint32_t *)(
            (uint8_t *)fb->address + y * fb->pitch
        );

    row[x] = color;
}

/* Draw a filled rectangle. */

static void rectangle(uint64_t x, uint64_t y,
                      uint64_t width, uint64_t height,
                      uint8_t r, uint8_t g, uint8_t b)
{
    for (uint64_t dy = 0; dy < height; dy++) {
        for (uint64_t dx = 0; dx < width; dx++) {
            pixel(x + dx, y + dy, r, g, b);
        }
    }
}

/* =========================================
   Built-in 5x7 Bitmap Font
   Supports uppercase letters, digits,
   spaces, and selected punctuation.
   ========================================= */

static const uint8_t font[][7] = {
    ['A'] = {14,17,17,31,17,17,17},
    ['B'] = {30,17,17,30,17,17,30},
    ['C'] = {14,17,16,16,16,17,14},
    ['D'] = {30,17,17,17,17,17,30},
    ['E'] = {31,16,16,30,16,16,31},
    ['F'] = {31,16,16,30,16,16,16},
    ['G'] = {14,17,16,23,17,17,15},
    ['H'] = {17,17,17,31,17,17,17},
    ['I'] = {14,4,4,4,4,4,14},
    ['J'] = {7,2,2,2,18,18,12},
    ['K'] = {17,18,20,24,20,18,17},
    ['L'] = {16,16,16,16,16,16,31},
    ['M'] = {17,27,21,21,17,17,17},
    ['N'] = {17,25,21,19,17,17,17},
    ['O'] = {14,17,17,17,17,17,14},
    ['P'] = {30,17,17,30,16,16,16},
    ['Q'] = {14,17,17,17,21,18,13},
    ['R'] = {30,17,17,30,20,18,17},
    ['S'] = {15,16,16,14,1,1,30},
    ['T'] = {31,4,4,4,4,4,4},
    ['U'] = {17,17,17,17,17,17,14},
    ['V'] = {17,17,17,17,17,10,4},
    ['W'] = {17,17,17,21,21,21,10},
    ['X'] = {17,17,10,4,10,17,17},
    ['Y'] = {17,17,10,4,4,4,4},
    ['Z'] = {31,1,2,4,8,16,31},

    ['0'] = {14,17,19,21,25,17,14},
    ['1'] = {4,12,4,4,4,4,14},
    ['2'] = {14,17,1,2,4,8,31},
    ['3'] = {30,1,1,14,1,1,30},
    ['4'] = {2,6,10,18,31,2,2},
    ['5'] = {31,16,16,30,1,1,30},
    ['6'] = {14,16,16,30,17,17,14},
    ['7'] = {31,1,2,4,8,8,8},
    ['8'] = {14,17,17,14,17,17,14},
    ['9'] = {14,17,17,15,1,1,14},

    ['.'] = {0,0,0,0,0,12,12},
    ['-'] = {0,0,0,31,0,0,0},
    [':'] = {0,12,12,0,12,12,0},
    ['/'] = {1,2,2,4,8,8,16},
    [' '] = {0,0,0,0,0,0,0}
};

/* Draw one character. */

static void draw_char(uint64_t x, uint64_t y, char c,
                      uint8_t r, uint8_t g, uint8_t b)
{
    unsigned char character = (unsigned char)c;

    if (character >= sizeof(font) / sizeof(font[0])) {
        return;
    }

    for (uint64_t row = 0; row < 7; row++) {
        for (uint64_t col = 0; col < 5; col++) {
            if (font[character][row] &
                (1u << (4 - col))) {
                pixel(x + col, y + row, r, g, b);
            }
        }
    }
}

/* Draw a string. Lowercase letters are converted
   to uppercase because the built-in font is uppercase. */

static void draw_text(uint64_t x, uint64_t y,
                      const char *text,
                      uint8_t r, uint8_t g, uint8_t b)
{
    while (*text != '\0') {
        char c = *text++;

        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }

        draw_char(x, y, c, r, g, b);
        x += 6;
    }
}

/* =========================================
   VeritasOS Desktop
   ========================================= */

void kmain(void)
{
    /* Check whether Limine provided a framebuffer. */

    if (framebuffer_request.response == NULL ||
        framebuffer_request.response->framebuffer_count < 1) {
        halt();
    }

    fb = framebuffer_request.response->framebuffers[0];

    /* This renderer currently supports 32-bit RGB only. */

    if (fb == NULL ||
        fb->address == NULL ||
        fb->bpp != 32 ||
        fb->memory_model != LIMINE_FRAMEBUFFER_RGB) {
        halt();
    }

    uint64_t w = fb->width;
    uint64_t h = fb->height;

    if (w == 0 || h < 120) {
        halt();
    }

    /* Desktop background */

    rectangle(0, 0, w, h, 22, 29, 48);

    /* Top panel */

    rectangle(0, 0, w, 42, 13, 18, 32);

    /* Operating system name */

    draw_text(16, 17,
              OS_NAME,
              235, 240, 255);

    /* Version label */

    if (w > 120) {
        draw_text(w - 100, 17,
                  OS_VERSION,
                  160, 180, 210);
    }

    /* Decorative desktop accent */

    if (w > 200) {
        rectangle(w / 2 - 100, h / 2 - 3,
                  200, 6,
                  80, 150, 255);
    }

    /* =====================================
       Main Application Window
       ===================================== */

    uint64_t win_w = w * 2 / 3;
    uint64_t win_h = h / 2;

    if (win_w < 180) {
        win_w = w > 20 ? w - 20 : w;
    }

    if (win_h < 100) {
        win_h = h - 80;
    }

    uint64_t win_x = (w - win_w) / 2;
    uint64_t win_y = (h - win_h) / 2;

    /* Window body */

    rectangle(win_x, win_y,
              win_w, win_h,
              55, 65, 86);

    /* Window title bar */

    rectangle(win_x, win_y,
              win_w, 36,
              35, 44, 64);

    /* Window controls */

    rectangle(win_x + 14, win_y + 13,
              10, 10, 255, 100, 110);

    rectangle(win_x + 32, win_y + 13,
              10, 10, 255, 195, 90);

    rectangle(win_x + 50, win_y + 13,
              10, 10, 100, 220, 150);

    /* Application title */

    draw_text(win_x + 76, win_y + 15,
              "VERITASOS",
              235, 240, 255);

    /* Welcome message */

    draw_text(win_x + 24, win_y + 65,
              "WELCOME TO VERITASOS",
              235, 240, 255);

    draw_text(win_x + 24, win_y + 90,
              "VERSION 0.1.0",
              160, 180, 210);

    /* =====================================
       Bottom Application Dock
       ===================================== */

    uint64_t dock_w = 240;
    uint64_t dock_h = 42;
    uint64_t dock_x = (w > dock_w)
                    ? (w - dock_w) / 2
                    : 0;
    uint64_t dock_y = (h > dock_h + 20)
                    ? h - dock_h - 20
                    : 0;

    rectangle(dock_x, dock_y,
              (w < dock_w) ? w : dock_w,
              dock_h,
              13, 18, 32);

    /* Placeholder application icons */

    rectangle(dock_x + 16, dock_y + 11,
              20, 20, 90, 150, 255);

    rectangle(dock_x + 50, dock_y + 11,
              20, 20, 100, 220, 160);

    rectangle(dock_x + 84, dock_y + 11,
              20, 20, 255, 185, 90);

    /* Keep the desktop visible.
       Keyboard, mouse and window management
       will be implemented in later versions. */

    halt();
}
