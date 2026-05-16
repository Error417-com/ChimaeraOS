/*
 * ChimaeraOS — Window Compositor
 * src/gui/compositor.c
 *
 * Text-mode compositor prototype.  See compositor.h for the full API
 * description and docs/compositor_design.md for the architectural rationale.
 *
 * Rendering model
 * ───────────────
 * The compositor maintains a uint16_t back-buffer of COMP_COLS × COMP_ROWS
 * cells.  Each uint16_t is a standard VGA cell: bits 15:8 = attribute byte,
 * bits 7:0 = ASCII character.
 *
 * comp_render() composites the scene in this order:
 *   1. Desktop background (solid fill)
 *   2. Windows, back-to-front (z_order[0] first, z_order[nwindows-1] last)
 *      a. Window border (single-line box drawing)
 *      b. Title bar (full width, with title text and [X] close button)
 *      c. Content area (copy from window->buf)
 *   3. Mouse cursor character
 *
 * The back-buffer is then copied to VGA memory (0xB8000) in one pass.
 */

#include "compositor.h"
#include "../include/types.h"

/* ── VGA memory ──────────────────────────────────────────────────────────── */

#define VGA_MEM  ((volatile uint16_t *)0xB8000)

/* ── Helper: make a VGA cell ─────────────────────────────────────────────── */

static inline uint16_t vga_cell(uint8_t ch, uint8_t attr)
{
    return (uint16_t)((attr << 8) | ch);
}

/* ── Helper: string length (no libc) ────────────────────────────────────── */

static int comp_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* ── Helper: string copy (no libc) ──────────────────────────────────────── */

static void comp_strcpy(char *dst, const char *src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* ── Helper: memset for uint16_t ─────────────────────────────────────────── */

static void comp_fill(uint16_t *buf, uint16_t val, int count)
{
    for (int i = 0; i < count; i++)
        buf[i] = val;
}

/* ── Back-buffer write helper ────────────────────────────────────────────── */

static inline void bb_put(compositor_t *c, int col, int row,
                           uint8_t ch, uint8_t attr)
{
    if (col < 0 || col >= COMP_COLS || row < 0 || row >= COMP_ROWS)
        return;
    c->backbuf[row * COMP_COLS + col] = vga_cell(ch, attr);
}

/* ── comp_init ───────────────────────────────────────────────────────────── */

void comp_init(compositor_t *c)
{
    /* Zero all windows */
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        c->windows[i].active  = false;
        c->windows[i].focused = false;
    }
    c->nwindows = 0;
    c->mouse_x  = COMP_COLS / 2;
    c->mouse_y  = COMP_ROWS / 2;

    /* Fill back-buffer with desktop colour */
    comp_fill(c->backbuf, vga_cell(' ', ATTR_DESKTOP), COMP_COLS * COMP_ROWS);
}

/* ── comp_create_window ──────────────────────────────────────────────────── */

int comp_create_window(compositor_t *c, int x, int y, int w, int h,
                       const char *title)
{
    if (c->nwindows >= COMP_MAX_WINDOWS)
        return -1;

    /* Find a free slot */
    int idx = -1;
    for (int i = 0; i < COMP_MAX_WINDOWS; i++) {
        if (!c->windows[i].active) { idx = i; break; }
    }
    if (idx < 0) return -1;

    window_t *win = &c->windows[idx];
    win->x          = x;
    win->y          = y;
    win->w          = w;
    win->h          = h;
    win->active     = true;
    win->focused    = false;
    win->cursor_col = 0;
    win->cursor_row = 0;
    comp_strcpy(win->title, title, COMP_TITLE_LEN + 1);

    /* Clear content buffer */
    for (int i = 0; i < WIN_BUF_SIZE; i++) {
        win->buf[i].ch   = ' ';
        win->buf[i].attr = ATTR_CONTENT;
    }

    /* Place at top of z-order and give focus */
    /* Defocus the previously focused window */
    if (c->nwindows > 0) {
        int top = c->z_order[c->nwindows - 1];
        c->windows[top].focused = false;
    }
    c->z_order[c->nwindows] = idx;
    c->nwindows++;
    win->focused = true;

    return idx;
}

/* ── comp_close_window ───────────────────────────────────────────────────── */

void comp_close_window(compositor_t *c, int idx)
{
    if (idx < 0 || idx >= COMP_MAX_WINDOWS) return;
    if (!c->windows[idx].active) return;

    c->windows[idx].active  = false;
    c->windows[idx].focused = false;

    /* Remove from z_order array */
    int pos = -1;
    for (int i = 0; i < c->nwindows; i++) {
        if (c->z_order[i] == idx) { pos = i; break; }
    }
    if (pos >= 0) {
        for (int i = pos; i < c->nwindows - 1; i++)
            c->z_order[i] = c->z_order[i + 1];
        c->nwindows--;
    }

    /* Give focus to the new top window */
    if (c->nwindows > 0) {
        int top = c->z_order[c->nwindows - 1];
        c->windows[top].focused = true;
    }
}

/* ── comp_bring_to_front ─────────────────────────────────────────────────── */

void comp_bring_to_front(compositor_t *c, int idx)
{
    if (idx < 0 || idx >= COMP_MAX_WINDOWS) return;
    if (!c->windows[idx].active) return;

    /* Find current position in z_order */
    int pos = -1;
    for (int i = 0; i < c->nwindows; i++) {
        if (c->z_order[i] == idx) { pos = i; break; }
    }
    if (pos < 0 || pos == c->nwindows - 1) {
        /* Already on top or not found — just ensure focus */
        if (c->nwindows > 0) {
            int top = c->z_order[c->nwindows - 1];
            c->windows[top].focused = (top == idx);
            if (top != idx) {
                /* Defocus old top, focus idx */
                c->windows[top].focused = false;
                c->windows[idx].focused = true;
            }
        }
        return;
    }

    /* Defocus old top */
    int old_top = c->z_order[c->nwindows - 1];
    c->windows[old_top].focused = false;

    /* Shift entries down to fill the gap */
    for (int i = pos; i < c->nwindows - 1; i++)
        c->z_order[i] = c->z_order[i + 1];
    c->z_order[c->nwindows - 1] = idx;

    /* Focus new top */
    c->windows[idx].focused = true;
}

/* ── comp_win_putchar ────────────────────────────────────────────────────── */

void comp_win_putchar(compositor_t *c, int idx, char ch, uint8_t attr)
{
    if (idx < 0 || idx >= COMP_MAX_WINDOWS) return;
    window_t *win = &c->windows[idx];
    if (!win->active) return;

    /* Content area dimensions */
    int cw = win->w - 2;  /* minus left and right borders */
    int ch_rows = win->h - 3; /* minus top border, title bar, bottom border */
    if (cw <= 0 || ch_rows <= 0) return;

    if (ch == '\n') {
        win->cursor_col = 0;
        win->cursor_row++;
    } else if (ch == '\r') {
        win->cursor_col = 0;
    } else {
        int pos = win->cursor_row * WIN_MAX_COLS + win->cursor_col;
        if (pos >= 0 && pos < WIN_BUF_SIZE) {
            win->buf[pos].ch   = (uint8_t)ch;
            win->buf[pos].attr = attr;
        }
        win->cursor_col++;
        if (win->cursor_col >= cw) {
            win->cursor_col = 0;
            win->cursor_row++;
        }
    }

    /* Scroll if needed */
    if (win->cursor_row >= ch_rows) {
        /* Shift all rows up by one */
        for (int r = 1; r < ch_rows; r++) {
            for (int col = 0; col < cw; col++) {
                win->buf[(r-1)*WIN_MAX_COLS + col] =
                    win->buf[r*WIN_MAX_COLS + col];
            }
        }
        /* Clear last row */
        for (int col = 0; col < cw; col++) {
            win->buf[(ch_rows-1)*WIN_MAX_COLS + col].ch   = ' ';
            win->buf[(ch_rows-1)*WIN_MAX_COLS + col].attr = attr;
        }
        win->cursor_row = ch_rows - 1;
    }
}

/* ── comp_win_puts ───────────────────────────────────────────────────────── */

void comp_win_puts(compositor_t *c, int idx, const char *s)
{
    while (*s)
        comp_win_putchar(c, idx, *s++, ATTR_CONTENT);
}

/* ── comp_win_puts_attr ──────────────────────────────────────────────────── */

void comp_win_puts_attr(compositor_t *c, int idx, const char *s, uint8_t attr)
{
    while (*s)
        comp_win_putchar(c, idx, *s++, attr);
}

/* ── comp_render ─────────────────────────────────────────────────────────── */

/*
 * Draw a single window into the back-buffer.
 *
 * Window layout (example 20x8 window at (5, 3)):
 *
 *   col:  5  6  7  8  9  10 11 12 13 14 15 16 17 18 19 20 21 22 23 24
 *   row 3:+--[Shell Window]----[X]+   <- top border + title bar
 *   row 4:|                        |   <- content row 0
 *   row 5:|                        |
 *   row 6:|                        |
 *   row 7:|                        |
 *   row 8:|                        |
 *   row 9:|                        |
 *   row10:+------------------------+   <- bottom border
 *
 * The title bar occupies row y.  Content starts at row y+1.
 * The bottom border is at row y+h-1.
 */
static void render_window(compositor_t *c, int idx)
{
    window_t *win = &c->windows[idx];
    if (!win->active) return;

    int x = win->x, y = win->y;
    int w = win->w, h = win->h;

    uint8_t tb_attr = win->focused ? ATTR_TITLEFOCUS : ATTR_TITLEBAR;

    /* ── Title bar (row y) ───────────────────────────────────────────────── */
    /* Left corner */
    bb_put(c, x, y, '+', ATTR_BORDER);

    /* Title text, centred in the title bar */
    int title_len = comp_strlen(win->title);
    int bar_inner = w - 4;  /* minus '+' on left, '[X]' on right, '+' on right */
    if (bar_inner < 0) bar_inner = 0;

    /* Fill title bar background */
    for (int col = 1; col < w - 1; col++)
        bb_put(c, x + col, y, '-', tb_attr);

    /* Write title text (truncated to bar_inner) */
    int tlen = title_len < bar_inner ? title_len : bar_inner;
    for (int i = 0; i < tlen; i++)
        bb_put(c, x + 1 + i, y, win->title[i], tb_attr);

    /* Close button [X] at the right end of the title bar */
    if (w >= 5) {
        bb_put(c, x + w - 4, y, '[', ATTR_CLOSE_BTN);
        bb_put(c, x + w - 3, y, 'X', ATTR_CLOSE_BTN);
        bb_put(c, x + w - 2, y, ']', ATTR_CLOSE_BTN);
    }

    /* Right corner */
    bb_put(c, x + w - 1, y, '+', ATTR_BORDER);

    /* ── Content rows (y+1 .. y+h-2) ────────────────────────────────────── */
    int cw = w - 2;
    int ch_rows = h - 2;  /* title bar is the top row, bottom border is last */
    for (int r = 0; r < ch_rows; r++) {
        int screen_row = y + 1 + r;
        /* Left border */
        bb_put(c, x, screen_row, '|', ATTR_BORDER);
        /* Content */
        for (int col = 0; col < cw; col++) {
            int buf_pos = r * WIN_MAX_COLS + col;
            uint8_t ch_char = ' ';
            uint8_t ch_attr = ATTR_CONTENT;
            if (buf_pos < WIN_BUF_SIZE) {
                ch_char = win->buf[buf_pos].ch;
                ch_attr = win->buf[buf_pos].attr;
            }
            bb_put(c, x + 1 + col, screen_row, ch_char, ch_attr);
        }
        /* Right border */
        bb_put(c, x + w - 1, screen_row, '|', ATTR_BORDER);
    }

    /* ── Bottom border (row y+h-1) ───────────────────────────────────────── */
    int bot = y + h - 1;
    bb_put(c, x, bot, '+', ATTR_BORDER);
    for (int col = 1; col < w - 1; col++)
        bb_put(c, x + col, bot, '-', ATTR_BORDER);
    bb_put(c, x + w - 1, bot, '+', ATTR_BORDER);
}

void comp_render(compositor_t *c)
{
    /* 1. Desktop background */
    comp_fill(c->backbuf, vga_cell(' ', ATTR_DESKTOP), COMP_COLS * COMP_ROWS);

    /* 2. Windows, back-to-front */
    for (int i = 0; i < c->nwindows; i++)
        render_window(c, c->z_order[i]);

    /* 3. Mouse cursor */
    if (c->mouse_x >= 0 && c->mouse_x < COMP_COLS &&
        c->mouse_y >= 0 && c->mouse_y < COMP_ROWS) {
        /* Invert the cell under the cursor */
        uint16_t cell = c->backbuf[c->mouse_y * COMP_COLS + c->mouse_x];
        uint8_t  ch   = (uint8_t)(cell & 0xFF);
        uint8_t  attr = (uint8_t)(cell >> 8);
        /* Swap foreground and background nibbles */
        attr = (uint8_t)(((attr & 0x0F) << 4) | ((attr >> 4) & 0x0F));
        c->backbuf[c->mouse_y * COMP_COLS + c->mouse_x] = vga_cell(ch, attr);
    }

    /* 4. Flush back-buffer to VGA memory */
    volatile uint16_t *vga = VGA_MEM;
    for (int i = 0; i < COMP_COLS * COMP_ROWS; i++)
        vga[i] = c->backbuf[i];
}

/* ── comp_hit_test ───────────────────────────────────────────────────────── */

int comp_hit_test(const compositor_t *c, int x, int y)
{
    /* Test from top to bottom (highest z-order first) */
    for (int i = c->nwindows - 1; i >= 0; i--) {
        int idx = c->z_order[i];
        const window_t *win = &c->windows[idx];
        if (!win->active) continue;
        if (x >= win->x && x < win->x + win->w &&
            y >= win->y && y < win->y + win->h)
            return idx;
    }
    return -1;
}

/* ── comp_on_mouse_move ──────────────────────────────────────────────────── */

void comp_on_mouse_move(compositor_t *c, int x, int y)
{
    if (x < 0) x = 0;
    if (x >= COMP_COLS) x = COMP_COLS - 1;
    if (y < 0) y = 0;
    if (y >= COMP_ROWS) y = COMP_ROWS - 1;
    c->mouse_x = x;
    c->mouse_y = y;
    comp_render(c);
}

/* ── comp_on_mouse_click ─────────────────────────────────────────────────── */

int comp_on_mouse_click(compositor_t *c, int x, int y)
{
    int idx = comp_hit_test(c, x, y);
    if (idx < 0) return -1;

    window_t *win = &c->windows[idx];

    /* Check for close-button click: last 3 chars of title bar row */
    if (y == win->y && win->w >= 5 &&
        x >= win->x + win->w - 4 && x <= win->x + win->w - 2) {
        comp_close_window(c, idx);
        comp_render(c);
        return idx;
    }

    /* Bring to front and focus */
    comp_bring_to_front(c, idx);
    comp_render(c);
    return idx;
}

/* ── comp_on_key ─────────────────────────────────────────────────────────── */

void comp_on_key(compositor_t *c, uint8_t ascii)
{
    int idx = comp_focused(c);
    if (idx < 0) return;
    /* Route to focused window's content area */
    comp_win_putchar(c, idx, (char)ascii, ATTR_CONTENT);
    comp_render(c);
}

/* ── comp_focused ────────────────────────────────────────────────────────── */

int comp_focused(const compositor_t *c)
{
    if (c->nwindows == 0) return -1;
    return c->z_order[c->nwindows - 1];
}
