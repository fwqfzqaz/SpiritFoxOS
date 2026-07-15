/*
 * nuklear_fb.c - Nuklear framebuffer rendering backend for SpiritFoxOS
 * Bridges Nuklear's draw commands to our fb (framebuffer) driver
 *
 * Note: Font baking is disabled - we use our own built-in 8x16 bitmap font
 * via fb_draw_char() for text rendering in NK_COMMAND_TEXT.
 */

/* Configure Nuklear for freestanding environment - minimal config */
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_STANDARD_VARARGS

/* Disable things we don't have */
#define NK_ASSERT(expr) ((void)0)

/* Provide our own implementations */
#include <stdint.h>
#include <stddef.h>
#include "kmalloc.h"
#include "string.h"
#include "fb.h"
#include "keyboard.h"
#include "timer.h"

/* ---------- C library stubs for Nuklear ---------- */

void *malloc(size_t size)
{
    return kmalloc(size);
}

void free(void *ptr)
{
    kfree(ptr);
}

/* Provide assert stub */
void __assert_fail(const char *expr, const char *file, int line, const char *func)
{
    (void)expr; (void)file; (void)line; (void)func;
    /* Do nothing in kernel mode */
}

/* Provide simple qsort for Nuklear's rect packing */
static void swap(char *a, char *b, size_t size)
{
    char tmp;
    for (size_t i = 0; i < size; i++) {
        tmp = a[i];
        a[i] = b[i];
        b[i] = tmp;
    }
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *))
{
    char *arr = (char *)base;
    for (size_t i = 0; i < nmemb - 1; i++) {
        for (size_t j = 0; j < nmemb - i - 1; j++) {
            if (compar(arr + j * size, arr + (j + 1) * size) > 0) {
                swap(arr + j * size, arr + (j + 1) * size, size);
            }
        }
    }
}

/* Provide math stubs - simplified implementations for kernel */
double sqrt(double x)
{
    if (x <= 0) return 0;
    /* Newton's method */
    double z = x / 2.0;
    for (int i = 0; i < 20; i++) {
        z = (z + x / z) / 2.0;
    }
    return z;
}

double floor(double x)
{
    long long i = (long long)x;
    return (x < 0 && x != (double)i) ? (double)(i - 1) : (double)i;
}

double ceil(double x)
{
    long long i = (long long)x;
    return (x > 0 && x != (double)i) ? (double)(i + 1) : (double)i;
}

double fabs(double x)
{
    return x < 0 ? -x : x;
}

double pow(double x, double y)
{
    if (y == 0) return 1.0;
    double result = 1.0;
    int yi = (int)y;
    for (int i = 0; i < yi; i++) result *= x;
    return result;
}

double cos(double x)
{
    /* Very rough approximation for kernel use */
    double x2 = x * x;
    double x4 = x2 * x2;
    return 1.0 - x2/2.0 + x4/24.0;
}

double acos(double x)
{
    if (x >= 1.0) return 0.0;
    if (x <= -1.0) return 3.14159265358979;
    return 1.57079632679490 - x;
}

double fmod(double x, double y)
{
    return x - (double)((long long)(x / y)) * y;
}

/* Now include Nuklear implementation */
#define NK_IMPLEMENTATION
#include "nuklear.h"

/* ---------- Color conversion ---------- */

static fb_color_t nk_color_to_fb(struct nk_color c)
{
    return ((uint32_t)c.a << 24) | ((uint32_t)c.r << 16) |
           ((uint32_t)c.g << 8) | c.b;
}

/* ---------- Software rasterizer for Nuklear ---------- */

static void nk_fb_render_command(const struct nk_command *cmd)
{
    switch (cmd->type) {
    case NK_COMMAND_NOP: break;
    case NK_COMMAND_SCISSOR: break;

    case NK_COMMAND_LINE: {
        const struct nk_command_line *l = (const struct nk_command_line *)cmd;
        fb_draw_line(l->begin.x, l->begin.y, l->end.x, l->end.y, nk_color_to_fb(l->color));
        break;
    }

    case NK_COMMAND_RECT: {
        const struct nk_command_rect *r = (const struct nk_command_rect *)cmd;
        fb_draw_rect(r->x, r->y, r->w, r->h, nk_color_to_fb(r->color));
        break;
    }

    case NK_COMMAND_RECT_FILLED: {
        const struct nk_command_rect_filled *r = (const struct nk_command_rect_filled *)cmd;
        fb_fill_rect(r->x, r->y, r->w, r->h, nk_color_to_fb(r->color));
        break;
    }

    case NK_COMMAND_RECT_MULTI_COLOR: {
        const struct nk_command_rect_multi_color *r =
            (const struct nk_command_rect_multi_color *)cmd;
        fb_fill_rect(r->x, r->y, r->w, r->h, nk_color_to_fb(r->left));
        break;
    }

    case NK_COMMAND_CIRCLE: {
        const struct nk_command_circle *c = (const struct nk_command_circle *)cmd;
        fb_draw_rect(c->x, c->y, c->w, c->h, nk_color_to_fb(c->color));
        break;
    }

    case NK_COMMAND_CIRCLE_FILLED: {
        const struct nk_command_circle_filled *c =
            (const struct nk_command_circle_filled *)cmd;
        fb_fill_rect(c->x, c->y, c->w, c->h, nk_color_to_fb(c->color));
        break;
    }

    case NK_COMMAND_ARC: {
        const struct nk_command_arc *a = (const struct nk_command_arc *)cmd;
        fb_draw_line(a->cx, a->cy, a->cx + (int)a->r, a->cy, nk_color_to_fb(a->color));
        break;
    }

    case NK_COMMAND_ARC_FILLED: {
        const struct nk_command_arc_filled *a =
            (const struct nk_command_arc_filled *)cmd;
        fb_draw_line(a->cx, a->cy, a->cx + (int)a->r, a->cy, nk_color_to_fb(a->color));
        break;
    }

    case NK_COMMAND_TRIANGLE: {
        const struct nk_command_triangle *t = (const struct nk_command_triangle *)cmd;
        fb_color_t col = nk_color_to_fb(t->color);
        fb_draw_line(t->a.x, t->a.y, t->b.x, t->b.y, col);
        fb_draw_line(t->b.x, t->b.y, t->c.x, t->c.y, col);
        fb_draw_line(t->c.x, t->c.y, t->a.x, t->a.y, col);
        break;
    }

    case NK_COMMAND_TRIANGLE_FILLED: {
        const struct nk_command_triangle_filled *t =
            (const struct nk_command_triangle_filled *)cmd;
        int minx = t->a.x < t->b.x ? (t->a.x < t->c.x ? t->a.x : t->c.x) : (t->b.x < t->c.x ? t->b.x : t->c.x);
        int miny = t->a.y < t->b.y ? (t->a.y < t->c.y ? t->a.y : t->c.y) : (t->b.y < t->c.y ? t->b.y : t->c.y);
        int maxx = t->a.x > t->b.x ? (t->a.x > t->c.x ? t->a.x : t->c.x) : (t->b.x > t->c.x ? t->b.x : t->c.x);
        int maxy = t->a.y > t->b.y ? (t->a.y > t->c.y ? t->a.y : t->c.y) : (t->b.y > t->c.y ? t->b.y : t->c.y);
        fb_fill_rect(minx, miny, maxx - minx, maxy - miny, nk_color_to_fb(t->color));
        break;
    }

    case NK_COMMAND_POLYGON:
    case NK_COMMAND_POLYGON_FILLED:
    case NK_COMMAND_POLYLINE:
    case NK_COMMAND_CURVE:
        break;

    case NK_COMMAND_TEXT: {
        const struct nk_command_text *t = (const struct nk_command_text *)cmd;
        int tx = t->x;
        int ty = t->y;
        const char *str = (const char *)t->string;
        int len = t->length;
        fb_color_t fg = nk_color_to_fb(t->foreground);
        fb_color_t bg = nk_color_to_fb(t->background);

        for (int i = 0; i < len; i++) {
            if (str[i] == '\n') {
                tx = t->x;
                ty += 16;
            } else {
                fb_draw_char(tx, ty, str[i], fg, bg);
                tx += 8;
            }
        }
        break;
    }

    case NK_COMMAND_IMAGE:
    case NK_COMMAND_CUSTOM:
    default:
        break;
    }
}

/* Render all Nuklear draw commands to framebuffer */
void nk_fb_render(struct nk_context *ctx, struct nk_color bg)
{
    fb_clear(nk_color_to_fb(bg));

    const struct nk_command *cmd;
    nk_foreach(cmd, ctx) {
        nk_fb_render_command(cmd);
    }

    fb_swap_buffer();
    nk_clear(ctx);
}

/* ---------- Input handling ---------- */

void nk_fb_handle_keyboard(struct nk_context *ctx)
{
    while (keyboard_has_char()) {
        char c = keyboard_get_char();

        if ((unsigned char)c >= KEY_UP) {
            switch ((unsigned char)c) {
            case KEY_UP:    nk_input_key(ctx, NK_KEY_UP, nk_true); nk_input_key(ctx, NK_KEY_UP, nk_false); break;
            case KEY_DOWN:  nk_input_key(ctx, NK_KEY_DOWN, nk_true); nk_input_key(ctx, NK_KEY_DOWN, nk_false); break;
            case KEY_LEFT:  nk_input_key(ctx, NK_KEY_LEFT, nk_true); nk_input_key(ctx, NK_KEY_LEFT, nk_false); break;
            case KEY_RIGHT: nk_input_key(ctx, NK_KEY_RIGHT, nk_true); nk_input_key(ctx, NK_KEY_RIGHT, nk_false); break;
            case KEY_HOME:  nk_input_key(ctx, NK_KEY_TEXT_START, nk_true); nk_input_key(ctx, NK_KEY_TEXT_START, nk_false); break;
            case KEY_END:   nk_input_key(ctx, NK_KEY_TEXT_END, nk_true); nk_input_key(ctx, NK_KEY_TEXT_END, nk_false); break;
            case KEY_DELETE: nk_input_key(ctx, NK_KEY_DEL, nk_true); nk_input_key(ctx, NK_KEY_DEL, nk_false); break;
            default: break;
            }
        } else {
            switch (c) {
            case '\b':  nk_input_key(ctx, NK_KEY_BACKSPACE, nk_true);
                        nk_input_key(ctx, NK_KEY_BACKSPACE, nk_false); break;
            case '\t':  nk_input_key(ctx, NK_KEY_TAB, nk_true);
                        nk_input_key(ctx, NK_KEY_TAB, nk_false); break;
            case '\n':  nk_input_key(ctx, NK_KEY_ENTER, nk_true);
                        nk_input_key(ctx, NK_KEY_ENTER, nk_false); break;
            default:
                if (c >= 0x20 && c < 0x7F) {
                    nk_input_char(ctx, c);
                }
                break;
            }
        }
    }
}

/* ---------- Init / Cleanup ---------- */

static struct nk_context nk_ctx;
static int nk_initialized;

struct nk_context *nk_fb_init(void)
{
    if (nk_initialized) return &nk_ctx;

    if (!nk_init_default(&nk_ctx, NULL)) {
        return NULL;
    }

    nk_initialized = 1;
    return &nk_ctx;
}

void nk_fb_cleanup(void)
{
    if (!nk_initialized) return;
    nk_free(&nk_ctx);
    nk_initialized = 0;
}
