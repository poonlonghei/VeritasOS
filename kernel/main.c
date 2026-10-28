#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>

/* VeritasOS 0.2.0: framebuffer desktop + polled PS/2 input. */
#define OS_NAME    "VeritasOS"
#define OS_VERSION "0.2.0"

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;
__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);
__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};
__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

static struct limine_framebuffer *fb;
static uint64_t screen_w, screen_h;
static int mouse_x, mouse_y;
static bool mouse_initialized;
#define CURSOR_SIZE 13
static uint32_t cursor_background[CURSOR_SIZE * CURSOR_SIZE];
static int old_cursor_x, old_cursor_y;
static bool cursor_visible;
static bool previous_left_button;
static uint8_t mouse_packet[3];
static unsigned mouse_packet_index;
static char input_line[48];
static size_t input_length;
static const char *status_message = "READY - TYPE ON YOUR KEYBOARD OR CLICK THE DESKTOP";

static void halt_cpu(void) {
    for (;;) __asm__ volatile ("hlt");
}

static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void io_wait(void) {
    outb(0x80, 0);
}

static bool wait_input_clear(void) {
    for (unsigned i = 0; i < 100000; ++i) {
        if ((inb(0x64) & 0x02u) == 0) return true;
    }
    return false;
}

static bool wait_output_full(void) {
    for (unsigned i = 0; i < 100000; ++i) {
        if (inb(0x64) & 0x01u) return true;
    }
    return false;
}

static bool keyboard_read(uint8_t *value) {
    uint8_t status = inb(0x64);
    if ((status & 0x01u) == 0 || (status & 0x20u) != 0) return false;
    *value = inb(0x60);
    return true;
}

static bool mouse_read(uint8_t *value) {
    uint8_t status = inb(0x64);
    if ((status & 0x01u) == 0 || (status & 0x20u) == 0) return false;
    *value = inb(0x60);
    return true;
}

static bool controller_read_config(uint8_t *value) {
    if (!wait_input_clear()) return false;
    outb(0x64, 0x20); /* Read PS/2 controller configuration byte. */
    if (!wait_output_full()) return false;
    *value = inb(0x60);
    return true;
}

static bool controller_write_config(uint8_t value) {
    if (!wait_input_clear()) return false;
    outb(0x64, 0x60); /* Write PS/2 controller configuration byte. */
    if (!wait_input_clear()) return false;
    outb(0x60, value);
    io_wait();
    return true;
}

static bool mouse_write(uint8_t value) {
    if (!wait_input_clear()) return false;
    outb(0x64, 0xD4); /* Next byte goes to the auxiliary device. */
    if (!wait_input_clear()) return false;
    outb(0x60, value);

    /* Ignore unrelated bytes until the mouse acknowledges or times out. */
    for (unsigned i = 0; i < 100000; ++i) {
        if (inb(0x64) & 0x01u) {
            uint8_t response = inb(0x60);
            if (response == 0xFA) return true;
            if (response == 0xFE) return false; /* Resend requested. */
        }
    }
    return false;
}

static bool initialize_ps2_controller(void) {
    /* Disable both ports while configuring the controller. */
    if (!wait_input_clear()) return false;
    outb(0x64, 0xAD); /* Disable first (keyboard) port. */
    if (!wait_input_clear()) return false;
    outb(0x64, 0xA7); /* Disable second (mouse) port. */

    /* Drain stale output bytes before issuing controller commands. */
    for (unsigned i = 0; i < 32 && (inb(0x64) & 0x01u); ++i)
        (void)inb(0x60);

    uint8_t config;
    if (!controller_read_config(&config)) return false;

    /* Use polling: disable IRQ bits, enable both ports, enable set-1 translation. */
    config &= (uint8_t)~0x03u; /* IRQ1 and IRQ12 off; this kernel polls. */
    config &= (uint8_t)~0x30u; /* Clear keyboard/mouse clock-disable bits. */
    config |= 0x40u;           /* Translate keyboard scan codes to set 1. */
    if (!controller_write_config(config)) return false;

    if (!wait_input_clear()) return false;
    outb(0x64, 0xAE); /* Enable keyboard port. */
    if (!wait_input_clear()) return false;
    outb(0x64, 0xA8); /* Enable mouse port. */
    io_wait();
    return true;
}

static void initialize_ps2_mouse(void) {
    if (!initialize_ps2_controller()) return;

    /* Reset defaults and enable mouse packet reporting. */
    if (mouse_write(0xF6) && mouse_write(0xF4))
        mouse_initialized = true;
}

static uint32_t channel(uint8_t value, uint8_t size, uint8_t shift) {
    if (size == 0 || size > 8 || shift >= 32 || size + shift > 32) return 0;
    uint32_t max_value = (1u << size) - 1u;
    return (((uint32_t)value * max_value + 127u) / 255u) << shift;
}

static void pixel(uint64_t x, uint64_t y, uint8_t r, uint8_t g, uint8_t b) {
    if (!fb || x >= fb->width || y >= fb->height) return;
    uint32_t color =
        channel(r, fb->red_mask_size, fb->red_mask_shift) |
        channel(g, fb->green_mask_size, fb->green_mask_shift) |
        channel(b, fb->blue_mask_size, fb->blue_mask_shift);
    volatile uint32_t *row =
        (volatile uint32_t *)((uint8_t *)fb->address + y * fb->pitch);
    row[x] = color;
}

static void rectangle(int x, int y, int width, int height,
                      uint8_t r, uint8_t g, uint8_t b) {
    if (width <= 0 || height <= 0) return;
    for (int dy = 0; dy < height; ++dy)
        for (int dx = 0; dx < width; ++dx)
            if (x + dx >= 0 && y + dy >= 0)
                pixel((uint64_t)(x + dx), (uint64_t)(y + dy), r, g, b);
}

/* Small uppercase 5x7 bitmap font. */
static const uint8_t font[128][7] = {
    ['A']={14,17,17,31,17,17,17}, ['B']={30,17,17,30,17,17,30},
    ['C']={14,17,16,16,16,17,14}, ['D']={30,17,17,17,17,17,30},
    ['E']={31,16,16,30,16,16,31}, ['F']={31,16,16,30,16,16,16},
    ['G']={14,17,16,23,17,17,15}, ['H']={17,17,17,31,17,17,17},
    ['I']={14,4,4,4,4,4,14}, ['J']={7,2,2,2,18,18,12},
    ['K']={17,18,20,24,20,18,17}, ['L']={16,16,16,16,16,16,31},
    ['M']={17,27,21,21,17,17,17}, ['N']={17,25,21,19,17,17,17},
    ['O']={14,17,17,17,17,17,14}, ['P']={30,17,17,30,16,16,16},
    ['Q']={14,17,17,17,21,18,13}, ['R']={30,17,17,30,20,18,17},
    ['S']={15,16,16,14,1,1,30}, ['T']={31,4,4,4,4,4,4},
    ['U']={17,17,17,17,17,17,14}, ['V']={17,17,17,17,17,10,4},
    ['W']={17,17,17,21,21,21,10}, ['X']={17,17,10,4,10,17,17},
    ['Y']={17,17,10,4,4,4,4}, ['Z']={31,1,2,4,8,16,31},
    ['0']={14,17,19,21,25,17,14}, ['1']={4,12,4,4,4,4,14},
    ['2']={14,17,1,2,4,8,31}, ['3']={30,1,1,14,1,1,30},
    ['4']={2,6,10,18,31,2,2}, ['5']={31,16,16,30,1,1,30},
    ['6']={14,16,16,30,17,17,14}, ['7']={31,1,2,4,8,8,8},
    ['8']={14,17,17,14,17,17,14}, ['9']={14,17,17,15,1,1,14},
    ['.']={0,0,0,0,0,12,12}, [',']={0,0,0,0,0,12,8},
    ['-']={0,0,0,31,0,0,0}, [':']={0,12,12,0,12,12,0},
    ['/']={1,2,2,4,8,8,16}, ['!']={4,4,4,4,4,0,4},
    ['?']={14,17,1,2,4,0,4}, ['>']={16,8,4,8,16,0,0},
    ['<']={1,2,4,2,1,0,0}, ['_']={0,0,0,0,0,0,31},
    [' ']={0,0,0,0,0,0,0}
};

static void draw_char(int x, int y, char c, uint8_t r, uint8_t g, uint8_t b) {
    unsigned char ch = (unsigned char)c;
    if (ch >= 128) return;
    if (ch >= 'a' && ch <= 'z') ch = (unsigned char)(ch - 'a' + 'A');
    for (int row = 0; row < 7; ++row)
        for (int col = 0; col < 5; ++col)
            if (font[ch][row] & (1u << (4 - col)))
                pixel((uint64_t)(x + col), (uint64_t)(y + row), r, g, b);
}

static void draw_text(int x, int y, const char *text,
                      uint8_t r, uint8_t g, uint8_t b) {
    while (*text) {
        draw_char(x, y, *text++, r, g, b);
        x += 6;
        if (x > (int)screen_w - 6) break;
    }
}

static void restore_cursor(void) {
    if (!cursor_visible) return;

    for (int dy = 0; dy < CURSOR_SIZE; ++dy) {
        for (int dx = 0; dx < CURSOR_SIZE; ++dx) {
            int px = old_cursor_x + dx;
            int py = old_cursor_y + dy;
            if (px < 0 || py < 0 ||
                px >= (int)fb->width || py >= (int)fb->height) continue;

            volatile uint32_t *row = (volatile uint32_t *)(
                (uint8_t *)fb->address + (uint64_t)py * fb->pitch);
            row[px] = cursor_background[dy * CURSOR_SIZE + dx];
        }
    }
    cursor_visible = false;
}

static void draw_cursor(int x, int y) {
    old_cursor_x = x;
    old_cursor_y = y;

    /* Save the framebuffer pixels beneath the cursor before drawing it. */
    for (int dy = 0; dy < CURSOR_SIZE; ++dy) {
        for (int dx = 0; dx < CURSOR_SIZE; ++dx) {
            int px = x + dx;
            int py = y + dy;
            uint32_t saved = 0;
            if (px >= 0 && py >= 0 &&
                px < (int)fb->width && py < (int)fb->height) {
                volatile uint32_t *row = (volatile uint32_t *)(
                    (uint8_t *)fb->address + (uint64_t)py * fb->pitch);
                saved = row[px];
            }
            cursor_background[dy * CURSOR_SIZE + dx] = saved;
        }
    }

    /* A small arrow-shaped pointer. */
    for (int i = 0; i < CURSOR_SIZE; ++i) {
        if (x >= 0 && x < (int)screen_w && y + i >= 0 && y + i < (int)screen_h)
            pixel((uint64_t)x, (uint64_t)(y + i), 15, 20, 32);
        if (i < 8) {
            for (int j = 0; j <= i; ++j) {
                int px = x + j, py = y + i;
                if (px >= 0 && py >= 0 && px < (int)screen_w && py < (int)screen_h)
                    pixel((uint64_t)px, (uint64_t)py, 245, 248, 255);
            }
        } else if (x + 1 >= 0 && x + 1 < (int)screen_w &&
                   y + i >= 0 && y + i < (int)screen_h) {
            pixel((uint64_t)(x + 1), (uint64_t)(y + i), 245, 248, 255);
        }
    }
    cursor_visible = true;
}

static int win_x, win_y, win_w, win_h;
static int dock_x, dock_y, dock_w, dock_h;

static void draw_desktop(void) {
    int w = (int)screen_w, h = (int)screen_h;

    /* Deep navy background with broad horizontal color bands. */
    rectangle(0, 0, w, h, 17, 25, 44);
    rectangle(0, 42, w, h - 42, 20, 30, 53);
    rectangle(0, h / 2, w, h / 2, 22, 32, 57);

    /* Top panel. */
    rectangle(0, 0, w, 42, 11, 17, 31);
    rectangle(0, 41, w, 1, 49, 73, 111);
    draw_text(18, 17, OS_NAME, 240, 244, 255);
    draw_text(112, 17, "DESKTOP", 120, 157, 205);
    if (w > 150) draw_text(w - 105, 17, OS_VERSION, 180, 199, 225);

    /* Main window with a subtle shadow. */
    win_w = w * 2 / 3;
    win_h = h / 2;
    if (win_w < 240) win_w = w > 24 ? w - 24 : w;
    if (win_h < 130) win_h = h > 90 ? h - 90 : h;
    win_x = (w - win_w) / 2;
    win_y = (h - win_h) / 2;
    rectangle(win_x + 5, win_y + 6, win_w, win_h, 10, 15, 28);
    rectangle(win_x, win_y, win_w, win_h, 54, 68, 96);
    rectangle(win_x, win_y, win_w, 38, 33, 44, 66);
    rectangle(win_x, win_y + 37, win_w, 1, 82, 101, 135);

    rectangle(win_x + 14, win_y + 14, 10, 10, 255, 103, 119);
    rectangle(win_x + 32, win_y + 14, 10, 10, 255, 195, 91);
    rectangle(win_x + 50, win_y + 14, 10, 10, 95, 222, 155);
    draw_text(win_x + 76, win_y + 16, "VERITASOS DESKTOP", 242, 245, 255);

    draw_text(win_x + 24, win_y + 64, "WELCOME TO VERITASOS", 247, 249, 255);
    draw_text(win_x + 24, win_y + 86, "A DESKTOP BUILT FROM C", 183, 205, 237);
    draw_text(win_x + 24, win_y + 108, "VERSION 0.2.0", 183, 205, 237);

    rectangle(win_x + 24, win_y + 132, win_w > 60 ? win_w - 48 : 10, 1,
              91, 111, 147);
    draw_text(win_x + 24, win_y + 150, "INPUT STATUS", 242, 245, 255);
    draw_text(win_x + 24, win_y + 170, status_message, 174, 208, 190);
    if (input_length > 0) {
        draw_text(win_x + 24, win_y + 192, "KEYS:", 183, 205, 237);
        draw_text(win_x + 60, win_y + 192, input_line, 255, 215, 140);
    }

    /* Bottom dock. */
    dock_w = 202;
    dock_h = 48;
    dock_x = (w - dock_w) / 2;
    dock_y = h - dock_h - 18;
    rectangle(dock_x, dock_y, dock_w, dock_h, 10, 15, 27);
    rectangle(dock_x, dock_y, dock_w, 1, 60, 79, 111);
    rectangle(dock_x + 17, dock_y + 13, 22, 22, 89, 157, 255);
    rectangle(dock_x + 51, dock_y + 13, 22, 22, 100, 221, 163);
    rectangle(dock_x + 85, dock_y + 13, 22, 22, 255, 190, 92);
    rectangle(dock_x + 119, dock_y + 13, 22, 22, 186, 133, 255);
    rectangle(dock_x + 153, dock_y + 13, 22, 22, 255, 112, 133);
}

static void redraw(void) {
    /* Full redraws are reserved for actual UI changes, such as keyboard input. */
    cursor_visible = false;
    draw_desktop();
    draw_cursor(mouse_x, mouse_y);
}

static char scancode_to_char(uint8_t sc) {
    static const char map[128] = {
        [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',
        [0x07]='6',[0x08]='7',[0x09]='8',[0x0A]='9',[0x0B]='0',
        [0x0C]='-',[0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',
        [0x14]='t',[0x15]='y',[0x16]='u',[0x17]='i',[0x18]='o',
        [0x19]='p',[0x1E]='a',[0x1F]='s',[0x20]='d',[0x21]='f',
        [0x22]='g',[0x23]='h',[0x24]='j',[0x25]='k',[0x26]='l',
        [0x2C]='z',[0x2D]='x',[0x2E]='c',[0x2F]='v',[0x30]='b',
        [0x31]='n',[0x32]='m',[0x39]=' '
    };
    return sc < 128 ? map[sc] : 0;
}

static void handle_key(uint8_t sc) {
    if (sc & 0x80u) return; /* Ignore key-release codes. */

    if (sc == 0x1C) {
        status_message = "ENTER PRESSED - INPUT RECEIVED";
        input_length = 0;
        input_line[0] = '\0';
    } else if (sc == 0x0E) {
        if (input_length) input_line[--input_length] = '\0';
        status_message = "BACKSPACE PRESSED";
    } else {
        char c = scancode_to_char(sc);
        if (c && input_length < sizeof(input_line) - 1) {
            input_line[input_length++] = c;
            input_line[input_length] = '\0';
            status_message = "KEYBOARD INPUT RECEIVED";
        }
    }
    redraw();
}

static void handle_mouse_packet(void) {
    int old_x = mouse_x;
    int old_y = mouse_y;
    int dx = (int)(int8_t)mouse_packet[1];
    int dy = (int)(int8_t)mouse_packet[2];

    /* Ignore overflow packets; their movement deltas are not reliable. */
    if ((mouse_packet[0] & 0xC0u) == 0) {
        mouse_x += dx;
        mouse_y -= dy;
    }

    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= (int)screen_w) mouse_x = (int)screen_w - 1;
    if (mouse_y >= (int)screen_h) mouse_y = (int)screen_h - 1;

    bool left_button = (mouse_packet[0] & 0x01u) != 0;
    bool left_click = left_button && !previous_left_button;
    previous_left_button = left_button;

    if (left_click) {
        if (mouse_x >= dock_x && mouse_x < dock_x + dock_w &&
            mouse_y >= dock_y && mouse_y < dock_y + dock_h) {
            status_message = "DOCK ICON CLICKED";
        } else if (mouse_x >= win_x && mouse_x < win_x + win_w &&
                   mouse_y >= win_y && mouse_y < win_y + 38) {
            status_message = "WINDOW TITLE BAR CLICKED";
        } else {
            status_message = "DESKTOP CLICKED";
        }
        restore_cursor();
        draw_desktop();
        draw_cursor(mouse_x, mouse_y);
        return;
    }

    /* On movement, restore only the old cursor area and draw the new cursor. */
    if (mouse_x != old_x || mouse_y != old_y) {
        restore_cursor();
        draw_cursor(mouse_x, mouse_y);
    }
}

void kmain(void) {
    if (!framebuffer_request.response ||
        framebuffer_request.response->framebuffer_count < 1) halt_cpu();

    fb = framebuffer_request.response->framebuffers[0];
    if (!fb || !fb->address || fb->bpp != 32 ||
        fb->memory_model != LIMINE_FRAMEBUFFER_RGB) halt_cpu();

    screen_w = fb->width;
    screen_h = fb->height;
    if (screen_w < 320 || screen_h < 200) halt_cpu();

    mouse_x = (int)(screen_w / 2);
    mouse_y = (int)(screen_h / 2);
    input_line[0] = '\0';

    /* Draw even if the VM has no PS/2 mouse. */
    draw_desktop();
    draw_cursor(mouse_x, mouse_y);
    initialize_ps2_mouse();
    if (!mouse_initialized)
        status_message = "KEYBOARD POLLING ACTIVE - MOUSE NOT DETECTED";
    redraw();

    /* Poll input without requiring an IDT or interrupt handlers yet. */
    for (;;) {
        uint8_t value;
        bool changed = false;

        if (keyboard_read(&value)) {
            handle_key(value);
            changed = true;
        }

        if (mouse_initialized && mouse_read(&value)) {
            /* Bit 3 is always set in the first byte of a PS/2 packet. */
            if (mouse_packet_index == 0 && (value & 0x08u) == 0) continue;
            mouse_packet[mouse_packet_index++] = value;
            if (mouse_packet_index == 3) {
                mouse_packet_index = 0;
                handle_mouse_packet();
                changed = true;
            }
        }

        (void)changed;
        __asm__ volatile ("pause");
    }
}