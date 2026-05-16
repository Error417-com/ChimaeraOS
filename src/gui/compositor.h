/*
 * ChimaeraOS — Window Compositor
 * src/gui/compositor.h
 *
 * Minimal text-mode compositor prototype.
 *
 * The compositor operates on an 80x25 character back-buffer.  Each window
 * is a rectangular region with its own character buffer, a title string,
 * a position, and a z-order.  The compositor draws windows back-to-front
 * into the back-buffer, then copies it to VGA memory (0xB8000).
 *
 * This prototype deliberately avoids VESA/graphics mode so it can run on
 * the existing VGA text-mode driver without additional hardware support.
 * The VESA upgrade path is described in docs/compositor_design.md.
 */

#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#include "../include/types.h"

/* ── Screen geometry ─────────────────────────────────────────────────────── */

#define COMP_COLS   80
#define COMP_ROWS   25

/* ── VGA colour attributes ───────────────────────────────────────────────── */

#define ATTR_DESKTOP    0x17  /* white on blue  — desktop background         */
#define ATTR_TITLEBAR   0x70  /* black on grey  — title bar                  */
#define ATTR_TITLEFOCUS 0x4F  /* white on red   — focused window title bar   */
#define ATTR_BORDER     0x07  /* white on black — window border               */
#define ATTR_CONTENT    0x07  /* white on black — window content area         */
#define ATTR_CLOSE_BTN  0xC0  /* black on bright red — close button          */
#define ATTR_CURSOR     0x6F  /* white on brown — mouse cursor character      */

/* ── Window limits ───────────────────────────────────────────────────────── */

#define COMP_MAX_WINDOWS  8
#define COMP_TITLE_LEN    30

/* ── Window content buffer ───────────────────────────────────────────────── */

/* Maximum window content area: (COMP_COLS-2) x (COMP_ROWS-3) */
#define WIN_MAX_COLS  (COMP_COLS - 2)
#define WIN_MAX_ROWS  (COMP_ROWS - 3)  /* minus title bar + top/bottom borders */
#define WIN_BUF_SIZE  (WIN_MAX_COLS * WIN_MAX_ROWS)

/*
 * A cell in the window content buffer: character + VGA attribute byte.
 */
typedef struct {
    uint8_t ch;
    uint8_t attr;
} cell_t;

/* ── Window structure ────────────────────────────────────────────────────── */

typedef struct window {
    /* Geometry (in character cells) */
    int x, y;           /* top-left corner of the window frame               */
    int w, h;           /* total width/height including border and title bar  */

    /* Metadata */
    char title[COMP_TITLE_LEN + 1];
    bool active;        /* true = this window exists                          */
    bool focused;       /* true = receives keyboard events                    */

    /* Content buffer — written by the window's "app" task */
    cell_t buf[WIN_BUF_SIZE];
    int    cursor_col;  /* text cursor column within content area             */
    int    cursor_row;  /* text cursor row within content area                */
} window_t;

/* ── Compositor state ────────────────────────────────────────────────────── */

typedef struct {
    window_t windows[COMP_MAX_WINDOWS];

    /*
     * z_order[0] = index of the bottom-most window,
     * z_order[n-1] = index of the top-most (focused) window.
     */
    int z_order[COMP_MAX_WINDOWS];
    int nwindows;

    /* Mouse cursor position */
    int mouse_x, mouse_y;

    /* Back-buffer: composited before writing to VGA memory */
    uint16_t backbuf[COMP_COLS * COMP_ROWS];
} compositor_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/*
 * comp_init — initialise the compositor, clear the back-buffer, draw the
 *             desktop background.
 */
void comp_init(compositor_t *c);

/*
 * comp_create_window — create a new window at (x, y) with dimensions (w, h).
 *   Returns the window index (0..COMP_MAX_WINDOWS-1) or -1 on failure.
 *   The new window is placed at the top of the z-order and receives focus.
 */
int comp_create_window(compositor_t *c, int x, int y, int w, int h,
                       const char *title);

/*
 * comp_close_window — destroy the window at index idx.
 *   Focus moves to the next window in z-order.
 */
void comp_close_window(compositor_t *c, int idx);

/*
 * comp_bring_to_front — move window idx to the top of the z-order.
 *   Also sets focus to that window.
 */
void comp_bring_to_front(compositor_t *c, int idx);

/*
 * comp_win_putchar — write a character to window idx at its current cursor
 *   position, advancing the cursor.  Scrolls the content area if needed.
 */
void comp_win_putchar(compositor_t *c, int idx, char ch, uint8_t attr);

/*
 * comp_win_puts — write a NUL-terminated string to window idx.
 */
void comp_win_puts(compositor_t *c, int idx, const char *s);

/*
 * comp_win_puts_attr — write a string with an explicit VGA attribute byte.
 */
void comp_win_puts_attr(compositor_t *c, int idx, const char *s, uint8_t attr);

/*
 * comp_render — composite all windows into the back-buffer and copy it to
 *   VGA memory.  Call this after any state change.
 */
void comp_render(compositor_t *c);

/*
 * comp_hit_test — return the index of the topmost window whose frame
 *   contains screen coordinate (x, y), or -1 if only the desktop is hit.
 */
int comp_hit_test(const compositor_t *c, int x, int y);

/*
 * comp_on_mouse_move — update cursor position and re-render.
 */
void comp_on_mouse_move(compositor_t *c, int x, int y);

/*
 * comp_on_mouse_click — handle a left-click at (x, y):
 *   - brings the hit window to front,
 *   - detects close-button clicks.
 *   Returns the index of the window that was clicked, or -1.
 */
int comp_on_mouse_click(compositor_t *c, int x, int y);

/*
 * comp_on_key — route a key event to the focused window.
 *   The window's "app" task is responsible for consuming it via
 *   comp_win_get_key().
 */
void comp_on_key(compositor_t *c, uint8_t ascii);

/*
 * comp_focused — return the index of the currently focused window, or -1.
 */
int comp_focused(const compositor_t *c);

#endif /* COMPOSITOR_H */
