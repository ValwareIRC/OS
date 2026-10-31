/* The first-boot look (onboard.c, onboard_talk.inc): light, clean, a little
 * like a game's menus. A pale stage with a grid floor, white cards with soft
 * shadows, rounded friendly type, keycaps for the keys, chapters along the
 * top. Text always sits on white, so it reads over any room. */

#define LK_INK RGBA(31, 34, 64, 255)
#define LK_SOFT RGBA(107, 111, 147, 255)
#define LK_FAINT RGBA(150, 154, 186, 255)
#define LK_ACCENT RGBA(124, 108, 255, 255)
#define LK_ACCENT2 RGBA(255, 143, 184, 255)
#define LK_MINT RGBA(95, 216, 192, 255)
#define LK_SUN RGBA(255, 197, 107, 255)
#define LK_WHITE RGBA(255, 255, 255, 255)

static font *lk_head(float px) { return font_get_styled("Outfit", px, 600, false, 0); }
static font *lk_body(float px) { return font_get("Manrope", px); }
static font *lk_bold(float px) { return font_get_styled("Manrope", px, 700, false, 0); }
static font *lk_label(float px) { return font_get("Unbounded", px); }

/* The stage: lavender, peach and mint light on pearl, a grid floor
 * receding into the distance, a few small coloured sparks. Drawn once per
 * screen size and kept. */
static void look_stage(void) {
    static canvas *stage;
    if (!stage || stage->w != A.W || stage->h != A.H) {
        if (stage)
            canvas_free(stage);
        stage = canvas_new(A.W, A.H);
        canvas *s = stage;
        fill_vgradient(s, (rect){0, 0, A.W, A.H}, RGBA(247, 246, 255, 255), RGBA(240, 242, 252, 255));
        glow_circle(s, A.W * 0.18f, A.H * 0.08f, A.W * 0.42f, RGBA(220, 212, 255, 210));
        glow_circle(s, A.W * 0.88f, A.H * 0.20f, A.W * 0.38f, RGBA(255, 214, 230, 200));
        glow_circle(s, A.W * 0.60f, A.H * 1.10f, A.W * 0.45f, RGBA(205, 244, 234, 220));
        /* the floor: lines running to a point far away, and across, closer together with distance */
        float hy = A.H * 0.64f, vx = A.W / 2.0f;
        for (int i = -14; i <= 14; i++) {
            float bx = vx + i * A.W * 0.09f;
            draw_line(s, vx + (bx - vx) * 0.08f, hy, bx, (float)A.H, 1.0f, RGBA(124, 108, 255, 26));
        }
        for (int k = 1; k <= 12; k++) {
            float f = (float)k / 12, y = hy + (A.H - hy) * f * f;
            draw_line(s, 0, y, (float)A.W, y, 1.0f, RGBA(124, 108, 255, (int)(10 + 26 * f)));
        }
        /* fade the floor into the light above it */
        fill_vgradient(s, (rect){0, (int)hy - 2, A.W, (int)((A.H - hy) * 0.45f)}, RGBA(244, 244, 254, 255),
                       RGBA(244, 244, 254, 0));
        static const struct {
            float x, y;
            color c;
        } sparks[] = {{0.14f, 0.33f, LK_ACCENT2}, {0.86f, 0.20f, LK_SUN}, {0.81f, 0.58f, LK_MINT}, {0.21f, 0.68f, LK_ACCENT},
                      {0.70f, 0.09f, LK_ACCENT}, {0.06f, 0.52f, LK_SUN}};
        for (size_t i = 0; i < sizeof sparks / sizeof sparks[0]; i++) {
            float x = sparks[i].x * A.W, y = sparks[i].y * A.H, r = 6;
            float d[8] = {x, y - r, x + r, y, x, y + r, x - r, y};
            fill_polygon(s, d, 4, color_alpha(sparks[i].c, 0.55f));
        }
    }
    memcpy(A.frame->px, stage->px, sizeof(uint32_t) * (size_t)A.W * A.H);
}

/* A white card with a soft shadow; lifted and outlined in the accent on hover. */
static void look_card(rect r, float radius, bool hover, bool selected) {
    if (hover)
        r.y -= 3;
    shadow_round(A.frame, (rect){r.x, r.y + (hover ? 14 : 10), r.w, r.h}, radius, hover ? 34 : 26,
                 hover ? RGBA(124, 108, 255, 60) : RGBA(60, 50, 140, 30));
    fill_round(A.frame, r, radius, LK_WHITE);
    stroke_round(A.frame, r, radius, selected || hover ? 2 : 1,
                 selected || hover ? LK_ACCENT : RGBA(124, 108, 255, 26));
}

static void look_pill(rect r, color fill, color edge) {
    shadow_round(A.frame, (rect){r.x, r.y + 4, r.w, r.h}, r.h / 2.0f, 12, RGBA(60, 50, 140, 22));
    fill_round(A.frame, r, r.h / 2.0f, fill);
    if (C_A(edge))
        stroke_round(A.frame, r, r.h / 2.0f, 1, edge);
}

/* A little key, as on a keyboard ("1", "ENTER"); returns its width. */
static int look_keycap(int x, int y, const char *key, bool lit) {
    font *f = lk_label(10);
    int w = text_width(f, key) + 14, h = 22;
    fill_round(A.frame, (rect){x, y + 2, w, h}, 7, lit ? RGBA(91, 75, 216, 255) : RGBA(220, 218, 243, 255));
    fill_round(A.frame, (rect){x, y, w, h}, 7, lit ? LK_ACCENT : LK_WHITE);
    draw_text_centered(A.frame, f, (rect){x, y, w, h}, key, lit ? LK_WHITE : LK_SOFT);
    return w;
}

/* Chapters along the top: done, now, to come. */
static void look_steps(int y, const char *const *labels, const int *state, int n) {
    font *f = lk_label(10);
    int widths[12], total = 0;
    for (int i = 0; i < n; i++) {
        widths[i] = text_width(f, labels[i]) + 46;
        total += widths[i] + (i ? 10 : 0);
    }
    int x = (A.W - total) / 2;
    for (int i = 0; i < n; i++) {
        rect r = {x, y, widths[i], 32};
        bool done = state[i] == 2, now = state[i] == 1;
        if (now)
            look_pill(r, LK_WHITE, 0);
        else
            fill_round(A.frame, r, 16, RGBA(255, 255, 255, 150));
        color dot = done ? LK_MINT : now ? LK_ACCENT : RGBA(232, 231, 248, 255);
        fill_circle(A.frame, r.x + 17.0f, r.y + 16.0f, 9, dot);
        if (done) { /* a tick */
            draw_line(A.frame, r.x + 13.0f, r.y + 16.0f, r.x + 16.0f, r.y + 19.5f, 1.8f, LK_WHITE);
            draw_line(A.frame, r.x + 16.0f, r.y + 19.5f, r.x + 21.5f, r.y + 12.5f, 1.8f, LK_WHITE);
        } else {
            char num[4];
            snprintf(num, sizeof num, "%d", i + 1);
            draw_text_centered(A.frame, lk_label(8), (rect){r.x + 8, r.y + 7, 18, 18}, num, now ? LK_WHITE : LK_FAINT);
        }
        draw_text(A.frame, f, r.x + 32, r.y + (32 - font_line_height(f)) / 2, labels[i],
                  now ? LK_ACCENT : done ? LK_SOFT : LK_FAINT);
        x += widths[i] + 10;
    }
}
