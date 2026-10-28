#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limine.h>

/* VeritasOS 0.2.0: framebuffer desktop + polled PS/2 input. */
#define OS_NAME    "VeritasOS"
#define OS_VERSION "0.3.0"

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
static const char *status_message = "READY - CLICK DESKTOP ICONS OR DOCK";

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

enum window_kind { WIN_NONE, WIN_HOME, WIN_FILES, WIN_SETTINGS };
static enum window_kind active_window = WIN_HOME;
static int win_x, win_y, win_w, win_h;
static int dock_x, dock_y, dock_w, dock_h;
static bool power_menu_open;
static bool dragging_window;
static int drag_offset_x, drag_offset_y;
static bool show_clock = true;
static bool dark_theme = true;
static bool show_desktop_icons = true;
static unsigned long uptime_ticks;

static void rtc_read_time(char out[9]) {
    uint8_t sec, min, hour, status_b;
    /* Read CMOS RTC time. Retry if seconds change during the read. */
    for (unsigned retry = 0; retry < 4; ++retry) {
        outb(0x70, 0x00); sec = inb(0x71);
        outb(0x70, 0x02); min = inb(0x71);
        outb(0x70, 0x04); hour = inb(0x71);
        outb(0x70, 0x0B); status_b = inb(0x71);
        outb(0x70, 0x00);
        if ((inb(0x71) & 0x7Fu) == (sec & 0x7Fu)) break;
    }
    bool pm = (hour & 0x80u) != 0;
    hour &= 0x7Fu;
    if ((status_b & 0x04u) == 0) {
        sec = (uint8_t)(((sec >> 4) * 10u) + (sec & 0x0Fu));
        min = (uint8_t)(((min >> 4) * 10u) + (min & 0x0Fu));
        hour = (uint8_t)(((hour >> 4) * 10u) + (hour & 0x0Fu));
    }
    if ((status_b & 0x02u) == 0) {
        hour = (uint8_t)(hour % 12u);
        if (pm) hour = (uint8_t)(hour + 12u);
    }
    out[0] = (char)('0' + ((hour / 10u) % 10u));
    out[1] = (char)('0' + (hour % 10u)); out[2] = ':';
    out[3] = (char)('0' + ((min / 10u) % 10u));
    out[4] = (char)('0' + (min % 10u)); out[5] = ':';
    out[6] = (char)('0' + ((sec / 10u) % 10u));
    out[7] = (char)('0' + (sec % 10u)); out[8] = '\0';
}

static void draw_icon(int x, int y, const char *label, uint8_t r, uint8_t g, uint8_t b) {
    rectangle(x, y, 34, 30, r, g, b);
    rectangle(x + 4, y + 4, 26, 22, 235, 241, 252);
    rectangle(x + 8, y + 8, 18, 14, r, g, b);
    draw_text(x - 2, y + 36, label, 240, 244, 255);
}

static void draw_window(void) {
    if (active_window == WIN_NONE) return;
    int w = (int)screen_w, h = (int)screen_h;
    win_w = w * 2 / 3;
    if (win_w > 620) win_w = 620;
    if (win_w < 260) win_w = w - 20;
    win_h = h * 2 / 3;
    if (win_h > 390) win_h = 390;
    if (win_h < 150) win_h = h - 80;
    if (win_x == 0 && win_y == 0) {
        win_x = (w - win_w) / 2;
        win_y = (h - win_h) / 2 - 8;
    }
    if (win_x < 0) win_x = 0;
    if (win_y < 42) win_y = 42;
    if (win_x + win_w > w) win_x = w - win_w;
    if (win_y + win_h > h - 66) win_y = h - 66 - win_h;

    rectangle(win_x + 5, win_y + 6, win_w, win_h, 7, 10, 20);
    rectangle(win_x, win_y, win_w, win_h, dark_theme ? 43 : 225,
              dark_theme ? 55 : 232, dark_theme ? 78 : 241);
    rectangle(win_x, win_y, win_w, 38, 28, 39, 59);
    rectangle(win_x, win_y + 37, win_w, 1, 82, 101, 135);
    rectangle(win_x + 13, win_y + 13, 11, 11, 255, 103, 119);
    rectangle(win_x + 31, win_y + 13, 11, 11, 255, 195, 91);
    rectangle(win_x + 49, win_y + 13, 11, 11, 95, 222, 155);

    if (active_window == WIN_HOME) {
        draw_text(win_x + 72, win_y + 15, "WELCOME", 242, 245, 255);
        draw_text(win_x + 22, win_y + 62, "WELCOME TO VERITASOS", 247, 249, 255);
        draw_text(win_x + 22, win_y + 84, "A DESKTOP BUILT FROM C", 183, 205, 237);
        draw_text(win_x + 22, win_y + 106, "VERSION 0.3.0", 183, 205, 237);
        rectangle(win_x + 22, win_y + 128, win_w - 44, 1, 91, 111, 147);
        draw_text(win_x + 22, win_y + 145, "INPUT STATUS", 242, 245, 255);
        draw_text(win_x + 22, win_y + 163, status_message, 174, 208, 190);
        if (input_length) {
            draw_text(win_x + 22, win_y + 185, "KEYS:", 183, 205, 237);
            draw_text(win_x + 58, win_y + 185, input_line, 255, 215, 140);
        }
        draw_text(win_x + 22, win_y + win_h - 25, "TIP: DRAG THIS TITLE BAR TO MOVE THE WINDOW", 185, 200, 226);
    } else if (active_window == WIN_FILES) {
        draw_text(win_x + 72, win_y + 15, "FILE MANAGER", 242, 245, 255);
        draw_text(win_x + 22, win_y + 61, "VERITASOS FILE MANAGER", 247, 249, 255);
        draw_text(win_x + 22, win_y + 84, "DEMO DIRECTORY: /", 183, 205, 237);
        draw_text(win_x + 22, win_y + 112, "[DIR]  SYSTEM", 174, 208, 190);
        draw_text(win_x + 22, win_y + 134, "[DIR]  HOME", 174, 208, 190);
        draw_text(win_x + 22, win_y + 156, "[FILE] KERNEL.ELF", 255, 215, 140);
        draw_text(win_x + 22, win_y + 178, "[FILE] LIMINE.CONF", 255, 215, 140);
        draw_text(win_x + 22, win_y + 200, "DEMO ONLY - DISK FILESYSTEM NOT YET IMPLEMENTED", 220, 190, 160);
    } else if (active_window == WIN_SETTINGS) {
        draw_text(win_x + 72, win_y + 15, "SETTINGS", 242, 245, 255);
        draw_text(win_x + 22, win_y + 61, "DESKTOP SETTINGS", 247, 249, 255);
        draw_text(win_x + 22, win_y + 88, dark_theme ? "THEME: DARK (CLICK TO CHANGE)" : "THEME: LIGHT (CLICK TO CHANGE)", 183, 205, 237);
        draw_text(win_x + 22, win_y + 116, show_clock ? "CLOCK: VISIBLE (CLICK TO HIDE)" : "CLOCK: HIDDEN (CLICK TO SHOW)", 183, 205, 237);
        draw_text(win_x + 22, win_y + 144, show_desktop_icons ? "DESKTOP ICONS: VISIBLE" : "DESKTOP ICONS: HIDDEN", 183, 205, 237);
        draw_text(win_x + 22, win_y + 178, "CLICK INSIDE THIS WINDOW TO CHANGE THEME", 174, 208, 190);
        draw_text(win_x + 22, win_y + 198, "CLOCK USES THE VIRTUAL MACHINE RTC", 174, 208, 190);
    }
}

static void draw_desktop(void) {
    int w = (int)screen_w, h = (int)screen_h;
    rectangle(0, 0, w, h, dark_theme ? 17 : 190, dark_theme ? 25 : 207, dark_theme ? 44 : 230);
    rectangle(0, 42, w, h - 42, dark_theme ? 20 : 205, dark_theme ? 30 : 220, dark_theme ? 53 : 235);
    rectangle(0, h / 2, w, h / 2, dark_theme ? 22 : 212, dark_theme ? 32 : 225, dark_theme ? 57 : 238);
    rectangle(0, 0, w, 42, 11, 17, 31);
    rectangle(0, 41, w, 1, 49, 73, 111);
    draw_text(18, 17, OS_NAME, 240, 244, 255);
    draw_text(112, 17, "DESKTOP", 120, 157, 205);
    if (show_clock && w > 260) {
        char clock_text[9];
        rtc_read_time(clock_text);
        draw_text(w - 74, 17, clock_text, 240, 244, 255);
    } else if (w > 150) draw_text(w - 105, 17, OS_VERSION, 180, 199, 225);

    if (show_desktop_icons) {
        draw_icon(24, 76, "HOME", 89, 157, 255);
        draw_icon(24, 134, "FILES", 100, 221, 163);
        draw_icon(24, 192, "SETTINGS", 255, 190, 92);
    }
    draw_window();

    dock_w = 226; dock_h = 48;
    dock_x = (w - dock_w) / 2;
    dock_y = h - dock_h - 12;
    rectangle(dock_x, dock_y, dock_w, dock_h, 10, 15, 27);
    rectangle(dock_x, dock_y, dock_w, 1, 60, 79, 111);
    rectangle(dock_x + 14, dock_y + 12, 26, 24, 89, 157, 255);
    rectangle(dock_x + 51, dock_y + 12, 26, 24, 100, 221, 163);
    rectangle(dock_x + 88, dock_y + 12, 26, 24, 255, 190, 92);
    rectangle(dock_x + 125, dock_y + 12, 26, 24, 186, 133, 255);
    rectangle(dock_x + 162, dock_y + 12, 26, 24, 255, 112, 133);
    draw_text(dock_x + 18, dock_y + 19, "H", 255, 255, 255);
    draw_text(dock_x + 56, dock_y + 19, "F", 255, 255, 255);
    draw_text(dock_x + 93, dock_y + 19, "S", 35, 40, 50);
    draw_text(dock_x + 130, dock_y + 19, "T", 255, 255, 255);
    draw_text(dock_x + 167, dock_y + 19, "P", 255, 255, 255);

    if (power_menu_open) {
        int px = w - 184, py = 48;
        rectangle(px, py, 170, 82, 10, 15, 27);
        rectangle(px, py, 170, 1, 90, 112, 148);
        draw_text(px + 12, py + 14, "POWER MENU", 240, 244, 255);
        draw_text(px + 12, py + 36, "REBOOT", 174, 208, 190);
        draw_text(px + 12, py + 58, "SHUT DOWN", 255, 190, 170);
    }
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

static void reboot_system(void) {
    /* Ask the PS/2 controller to pulse the CPU reset line. */
    for (unsigned i = 0; i < 100000; ++i) {
        if ((inb(0x64) & 0x02u) == 0) break;
    }
    outb(0x64, 0xFE);
    for (;;) __asm__ volatile ("cli; hlt");
}

static void shutdown_system(void) {
    status_message = "SHUTDOWN REQUESTED - YOU MAY CLOSE THE VM";
    power_menu_open = false;
    redraw();
    /* ACPI power-off is not implemented yet; halt safely instead. */
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

static void open_window(enum window_kind kind) {
    active_window = kind;
    power_menu_open = false;
    win_x = 0;
    win_y = 0;
    redraw();
}

static void handle_mouse_packet(void) {
    int old_x = mouse_x, old_y = mouse_y;
    int dx = (int)(int8_t)mouse_packet[1];
    int dy = (int)(int8_t)mouse_packet[2];
    if ((mouse_packet[0] & 0xC0u) == 0) { mouse_x += dx; mouse_y -= dy; }
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= (int)screen_w) mouse_x = (int)screen_w - 1;
    if (mouse_y >= (int)screen_h) mouse_y = (int)screen_h - 1;

    bool left_button = (mouse_packet[0] & 0x01u) != 0;
    bool left_click = left_button && !previous_left_button;
    previous_left_button = left_button;

    if (dragging_window && left_button && active_window != WIN_NONE) {
        win_x = mouse_x - drag_offset_x;
        win_y = mouse_y - drag_offset_y;
        if (win_x < 0) win_x = 0;
        if (win_y < 42) win_y = 42;
        if (win_x + win_w > (int)screen_w) win_x = (int)screen_w - win_w;
        if (win_y + win_h > (int)screen_h - 60) win_y = (int)screen_h - 60 - win_h;
        redraw();
        return;
    }
    if (!left_button) dragging_window = false;

    if (left_click) {
        int x = mouse_x, y = mouse_y;
        if (power_menu_open) {
            int px = (int)screen_w - 184, py = 48;
            power_menu_open = false;
            if (x >= px && x < px + 170 && y >= py + 28 && y < py + 52) reboot_system();
            if (x >= px && x < px + 170 && y >= py + 52 && y < py + 80) shutdown_system();
            redraw(); return;
        }
        if (y >= dock_y && y < dock_y + dock_h && x >= dock_x && x < dock_x + dock_w) {
            int slot = (x - dock_x) / 37;
            if (slot == 0) open_window(WIN_HOME);
            else if (slot == 1) open_window(WIN_FILES);
            else if (slot == 2) open_window(WIN_SETTINGS);
            else if (slot == 3) { active_window = WIN_NONE; power_menu_open = false; redraw(); }
            else { power_menu_open = !power_menu_open; redraw(); }
            return;
        }
        if (show_desktop_icons && x >= 15 && x < 100) {
            if (y >= 70 && y < 126) { open_window(WIN_HOME); return; }
            if (y >= 128 && y < 184) { open_window(WIN_FILES); return; }
            if (y >= 186 && y < 242) { open_window(WIN_SETTINGS); return; }
        }
        if (active_window != WIN_NONE && x >= win_x && x < win_x + win_w &&
            y >= win_y && y < win_y + 38) {
            if (x < win_x + 27) { active_window = WIN_NONE; redraw(); return; }
            dragging_window = true;
            drag_offset_x = x - win_x;
            drag_offset_y = y - win_y;
            status_message = "WINDOW MOVED WITH MOUSE";
            redraw(); return;
        }
        if (active_window == WIN_SETTINGS && x >= win_x && x < win_x + win_w &&
            y >= win_y + 45 && y < win_y + win_h) {
            if (y < win_y + 110) dark_theme = !dark_theme;
            else if (y < win_y + 140) show_clock = !show_clock;
            else show_desktop_icons = !show_desktop_icons;
            redraw(); return;
        }
        if (x >= (int)screen_w - 80 && y < 42) { power_menu_open = true; redraw(); return; }
        status_message = "DESKTOP CLICKED";
        redraw(); return;
    }

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