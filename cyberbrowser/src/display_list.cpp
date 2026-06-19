/*
 * Display List - Implementation
 */

#include "display_list.h"
#include "platform.h"

#include <stdlib.h>
#include <string.h>

#define LOG_TAG "display_list"
#define LOG_ERROR(...) platform_log(LOG_LEVEL_ERROR, LOG_TAG, __VA_ARGS__)

void display_list_init(DisplayList *dl)
{
    if (!dl) return;
    dl->cmds = NULL;
    dl->count = 0;
    dl->capacity = 0;
}

void display_list_free(DisplayList *dl)
{
    if (!dl) return;
    free(dl->cmds);
    dl->cmds = NULL;
    dl->count = 0;
    dl->capacity = 0;
}

bool display_list_reserve(DisplayList *dl, int extra)
{
    if (!dl) return false;
    int needed = dl->count + extra;
    if (needed <= dl->capacity) return true;

    int new_capacity = dl->capacity ? dl->capacity * 2 : 64;
    while (new_capacity < needed) new_capacity *= 2;

    DisplayListCmd *new_cmds = (DisplayListCmd*)realloc(dl->cmds, new_capacity * sizeof(DisplayListCmd));
    if (!new_cmds) {
        LOG_ERROR("Out of memory growing display list");
        return false;
    }
    dl->cmds = new_cmds;
    dl->capacity = new_capacity;
    return true;
}

bool display_list_add_rect(DisplayList *dl, float x, float y, float w, float h,
                           float r, float g, float b, float a)
{
    if (!display_list_reserve(dl, 1)) return false;
    DisplayListCmd *cmd = &dl->cmds[dl->count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = DL_RECT;
    cmd->x = x; cmd->y = y; cmd->w = w; cmd->h = h;
    cmd->r = r; cmd->g = g; cmd->b = b; cmd->a = a;
    return true;
}

bool display_list_add_border(DisplayList *dl, float x, float y, float w, float h,
                             float thickness, float r, float g, float b, float a)
{
    if (!display_list_reserve(dl, 1)) return false;
    DisplayListCmd *cmd = &dl->cmds[dl->count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = DL_BORDER;
    cmd->x = x; cmd->y = y; cmd->w = w; cmd->h = h;
    cmd->r = r; cmd->g = g; cmd->b = b; cmd->a = a;
    cmd->u.border.thickness = thickness;
    return true;
}

bool display_list_add_glyph(DisplayList *dl, float x, float y, float w, float h,
                            float u0, float v0, float u1, float v1,
                            float r, float g, float b, float a)
{
    if (!display_list_reserve(dl, 1)) return false;
    DisplayListCmd *cmd = &dl->cmds[dl->count++];
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = DL_GLYPH;
    cmd->x = x; cmd->y = y; cmd->w = w; cmd->h = h;
    cmd->r = r; cmd->g = g; cmd->b = b; cmd->a = a;
    cmd->u.glyph.u0 = u0; cmd->u.glyph.v0 = v0;
    cmd->u.glyph.u1 = u1; cmd->u.glyph.v1 = v1;
    return true;
}

bool css_layout_build_display_list(LayoutContext *ctx, DisplayList *dl)
{
    if (!ctx || !dl) return false;
    display_list_init(dl);

    for (int i = 0; i < ctx->tree.count; i++) {
        LayoutBox *box = &ctx->boxes[i];
        if (!(box->flags & LAYOUT_HAS_LAYOUT)) continue;

        /* Background rectangle. */
        if (box->background_color_a > 0.0f) {
            if (!display_list_add_rect(dl,
                                       (float)box->x, (float)box->y,
                                       (float)box->width, (float)box->height,
                                       (float)box->background_color_r,
                                       (float)box->background_color_g,
                                       (float)box->background_color_b,
                                       (float)box->background_color_a)) {
                return false;
            }
        }

        /* Border. */
        if (box->border_top > 0 || box->border_right > 0 ||
            box->border_bottom > 0 || box->border_left > 0) {
            float thickness = (float)(box->border_top + box->border_right +
                                      box->border_bottom + box->border_left) / 4.0f;
            if (thickness <= 0) thickness = 1.0f;
            if (!display_list_add_border(dl,
                                         (float)box->x, (float)box->y,
                                         (float)box->width, (float)box->height,
                                         thickness,
                                         (float)box->color_r,
                                         (float)box->color_g,
                                         (float)box->color_b,
                                         (float)box->color_a)) {
                return false;
            }
        }
    }

    return true;
}
