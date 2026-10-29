/*
 * Moji - a fast, simple Character-Map for Linux (GTK4, C)
 *
 * One continuous scrolling view of every Unicode character (in code-point order
 * or grouped by category). The list on the right jumps straight to any block.
 * The grid is drawn by hand and only the visible cells are rendered, so
 * scrolling through 150k characters stays instant.
 *
 * Build:  make        (needs gtk4 + a C compiler)
 */
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include "ucdata.h"

#define CELL 50   /* cell size in px          */
#define HDR  38   /* section header height    */
#define PAD  10   /* left/right margin        */
#define GAP  8    /* gap after each section   */

typedef struct {
    const char     *name;
    const uint32_t *chars;   /* ascending code points (or search results) */
    int             count;
    int             block;   /* index into UC_BLOCKS, -1 for search       */
} Section;

/* ---- widgets */
static GtkWidget *win, *da, *search, *buf_entry, *sort_dd, *list_box, *list_sw;
static GtkWidget *l_big, *l_name, *l_info, *l_toast;
static GtkAdjustment *adj;

/* ---- data */
static uint32_t *all_chars;
static int       boff[UC_NBLOCKS], bcount[UC_NBLOCKS];
static int       ord[UC_NBLOCKS], ordn;          /* blocks in display order  */
static int       by_category;

/* ---- current view */
static Section  *secs;
static int       nsecs;
static int      *sec_y;                          /* top y of every section   */
static int       cols = 1, total_h, da_w = 600, da_h = 400;
static gboolean  searching;
static uint32_t *results;
static int       nresults;
static char      search_title[160];
static char     *last_query;

/* ---- selection / hover */
static uint32_t  sel_cp = 'A';
static int       sel_sec = -1, sel_idx = -1;
static int       hov_sec = -1, hov_idx = -1;

/* ---- jump list */
static GtkWidget **list_rows;
static int         list_cur = -1;

static PangoLayout *lay_cell, *lay_hdr;
static guint        toast_id;

/* =================================================================== unicode helpers */
static gboolean valid_cp(uint32_t cp)
{
    if (cp > 0x10FFFF) return FALSE;
    switch (g_unichar_type(cp)) {
    case G_UNICODE_UNASSIGNED: case G_UNICODE_SURROGATE:
    case G_UNICODE_PRIVATE_USE: case G_UNICODE_CONTROL: return FALSE;
    default: return TRUE;
    }
}

static const char *cat_name(GUnicodeType t)
{
    static const char *n[] = {
        [G_UNICODE_CONTROL] = "Control", [G_UNICODE_FORMAT] = "Format",
        [G_UNICODE_UNASSIGNED] = "Unassigned", [G_UNICODE_PRIVATE_USE] = "Private Use",
        [G_UNICODE_SURROGATE] = "Surrogate", [G_UNICODE_LOWERCASE_LETTER] = "Lowercase Letter",
        [G_UNICODE_MODIFIER_LETTER] = "Modifier Letter", [G_UNICODE_OTHER_LETTER] = "Other Letter",
        [G_UNICODE_TITLECASE_LETTER] = "Titlecase Letter", [G_UNICODE_UPPERCASE_LETTER] = "Uppercase Letter",
        [G_UNICODE_SPACING_MARK] = "Spacing Mark", [G_UNICODE_ENCLOSING_MARK] = "Enclosing Mark",
        [G_UNICODE_NON_SPACING_MARK] = "Nonspacing Mark", [G_UNICODE_DECIMAL_NUMBER] = "Decimal Number",
        [G_UNICODE_LETTER_NUMBER] = "Letter Number", [G_UNICODE_OTHER_NUMBER] = "Other Number",
        [G_UNICODE_CONNECT_PUNCTUATION] = "Connector Punctuation", [G_UNICODE_DASH_PUNCTUATION] = "Dash Punctuation",
        [G_UNICODE_CLOSE_PUNCTUATION] = "Close Punctuation", [G_UNICODE_FINAL_PUNCTUATION] = "Final Quote",
        [G_UNICODE_INITIAL_PUNCTUATION] = "Initial Quote", [G_UNICODE_OTHER_PUNCTUATION] = "Other Punctuation",
        [G_UNICODE_OPEN_PUNCTUATION] = "Open Punctuation", [G_UNICODE_CURRENCY_SYMBOL] = "Currency Symbol",
        [G_UNICODE_MODIFIER_SYMBOL] = "Modifier Symbol", [G_UNICODE_MATH_SYMBOL] = "Math Symbol",
        [G_UNICODE_OTHER_SYMBOL] = "Other Symbol", [G_UNICODE_LINE_SEPARATOR] = "Line Separator",
        [G_UNICODE_PARAGRAPH_SEPARATOR] = "Paragraph Separator", [G_UNICODE_SPACE_SEPARATOR] = "Space Separator",
    };
    return (t < G_N_ELEMENTS(n) && n[t]) ? n[t] : "?";
}

static const char *HG_L[] = {"G","GG","N","D","DD","R","M","B","BB","S","SS","","J","JJ","C","K","T","P","H"};
static const char *HG_V[] = {"A","AE","YA","YAE","EO","E","YEO","YE","O","WA","WAE","OE","YO","U","WEO","WE","WI","YU","EU","YI","I"};
static const char *HG_T[] = {"","G","GG","GS","N","NJ","NH","D","L","LG","LM","LB","LS","LT","LP","LH","M","B","BS","S","SS","NG","J","C","K","T","P","H"};

static int name_cmp(const void *k, const void *e)
{
    uint32_t a = *(const uint32_t *)k, b = ((const UName *)e)->cp;
    return a < b ? -1 : a > b;
}

static const char *char_name(uint32_t cp, char *buf, size_t n)
{
    const UName *r = bsearch(&cp, UC_NAMES, UC_NNAMES, sizeof(UName), name_cmp);
    if (r) return UC_BLOB + r->off;
    if (cp >= 0xAC00 && cp <= 0xD7A3) {
        int s = cp - 0xAC00;
        snprintf(buf, n, "HANGUL SYLLABLE %s%s%s", HG_L[s / 588], HG_V[(s % 588) / 28], HG_T[s % 28]);
        return buf;
    }
    for (int i = 0; i < UC_NALGO; i++)
        if (cp >= UC_ALGO[i].start && cp <= UC_ALGO[i].end) {
            snprintf(buf, n, "%s%04X", UC_ALGO[i].prefix, cp);
            return buf;
        }
    return "(no name)";
}

static const char *block_name_of(uint32_t cp)
{
    for (int i = 0; i < UC_NBLOCKS; i++)
        if (cp >= UC_BLOCKS[i].start && cp <= UC_BLOCKS[i].end) return UC_BLOCKS[i].name;
    return "-";
}

/* text drawn inside a cell (marks get a dotted circle, invisible chars a symbol) */
static void cell_text(uint32_t cp, char *out)
{
    switch (g_unichar_type(cp)) {
    case G_UNICODE_NON_SPACING_MARK: case G_UNICODE_ENCLOSING_MARK: {
        int n = g_unichar_to_utf8(0x25CC, out);
        n += g_unichar_to_utf8(cp, out + n);
        out[n] = 0; return; }
    case G_UNICODE_SPACE_SEPARATOR: case G_UNICODE_LINE_SEPARATOR: case G_UNICODE_PARAGRAPH_SEPARATOR:
        out[g_unichar_to_utf8(0x2423, out)] = 0; return;
    case G_UNICODE_FORMAT:
        out[g_unichar_to_utf8(0x2B1A, out)] = 0; return;
    default:
        out[g_unichar_to_utf8(cp, out)] = 0;
    }
}

/* =================================================================== model */
static void init_data(void)
{
    GArray *a = g_array_new(FALSE, FALSE, sizeof(uint32_t));
    for (int b = 0; b < UC_NBLOCKS; b++) {
        boff[b] = a->len;
        for (uint32_t cp = UC_BLOCKS[b].start; cp <= UC_BLOCKS[b].end; cp++)
            if (valid_cp(cp)) g_array_append_val(a, cp);
        bcount[b] = a->len - boff[b];
    }
    all_chars = (uint32_t *)g_array_free(a, FALSE);
}

static int cmp_cat(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    int gx = UC_BLOCKS[x].group, gy = UC_BLOCKS[y].group;
    return gx != gy ? gx - gy : x - y;
}

static void build_order(void)
{
    ordn = 0;
    for (int i = 0; i < UC_NBLOCKS; i++) if (bcount[i] > 0) ord[ordn++] = i;
    if (by_category) qsort(ord, ordn, sizeof(int), cmp_cat);
}

static void build_sections(void)
{
    g_free(secs);
    if (searching) {
        secs = g_new0(Section, 1);
        secs[0] = (Section){search_title, results, nresults, -1};
        nsecs = 1;
    } else {
        secs = g_new0(Section, ordn);
        for (int k = 0; k < ordn; k++) {
            int b = ord[k];
            secs[k] = (Section){UC_BLOCKS[b].name, all_chars + boff[b], bcount[b], b};
        }
        nsecs = ordn;
    }
}

static int find_sec(double y)
{
    if (nsecs <= 0 || !sec_y) return -1;
    int lo = 0, hi = nsecs - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (sec_y[mid] <= y) lo = mid; else hi = mid - 1;
    }
    return lo;
}

static void relayout(void)
{
    cols = MAX(1, (da_w - 2 * PAD) / CELL);
    g_free(sec_y);
    sec_y = g_new(int, nsecs + 1);
    int y = 0;
    for (int i = 0; i < nsecs; i++) {
        sec_y[i] = y;
        y += HDR + ((secs[i].count + cols - 1) / cols) * CELL + GAP;
    }
    sec_y[nsecs] = y;
    total_h = y;
    gtk_adjustment_configure(adj, gtk_adjustment_get_value(adj), 0, MAX(total_h, da_h),
                             CELL, da_h * 0.9, da_h);
    gtk_widget_queue_draw(da);
}

static int grid_x0(void) { return MAX(PAD, (da_w - cols * CELL) / 2); }

static gboolean find_cp(uint32_t cp, int *s, int *i)
{
    if (searching) {
        for (int k = 0; k < nresults; k++) if (results[k] == cp) { *s = 0; *i = k; return TRUE; }
        return FALSE;
    }
    for (int k = 0; k < nsecs; k++) {
        int b = secs[k].block;
        if (cp < UC_BLOCKS[b].start || cp > UC_BLOCKS[b].end) continue;
        int lo = 0, hi = secs[k].count - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            uint32_t v = secs[k].chars[mid];
            if (v == cp) { *s = k; *i = mid; return TRUE; }
            if (v < cp) lo = mid + 1; else hi = mid - 1;
        }
        return FALSE;
    }
    return FALSE;
}

static void restore_selection(void)
{
    if (!find_cp(sel_cp, &sel_sec, &sel_idx)) {
        if (nsecs > 0 && secs[0].count > 0) { sel_sec = 0; sel_idx = 0; }
        else sel_sec = sel_idx = -1;
    }
}

/* =================================================================== details / clipboard */
static void toast_clear_cb(void) { gtk_label_set_text(GTK_LABEL(l_toast), ""); toast_id = 0; }

static gboolean toast_timeout(gpointer d) { toast_clear_cb(); return G_SOURCE_REMOVE; }

static void toast(const char *msg)
{
    gtk_label_set_text(GTK_LABEL(l_toast), msg);
    if (toast_id) g_source_remove(toast_id);
    toast_id = g_timeout_add(1500, toast_timeout, NULL);
}

static void copy_text(const char *t)
{
    if (!t || !*t) return;
    gdk_clipboard_set_text(gtk_widget_get_clipboard(win), t);
    toast("Copied");
}

static uint32_t selected_cp(gboolean *ok)
{
    *ok = sel_sec >= 0 && sel_sec < nsecs && sel_idx >= 0 && sel_idx < secs[sel_sec].count;
    return *ok ? secs[sel_sec].chars[sel_idx] : 0;
}

static void update_details(void)
{
    gboolean ok;
    uint32_t cp = selected_cp(&ok);
    if (!ok) {
        gtk_label_set_text(GTK_LABEL(l_big), "");
        gtk_label_set_text(GTK_LABEL(l_name), "");
        gtk_label_set_text(GTK_LABEL(l_info), "");
        return;
    }
    sel_cp = cp;
    char t[16], nb[96];
    cell_text(cp, t);
    gtk_label_set_text(GTK_LABEL(l_big), t);
    gtk_label_set_text(GTK_LABEL(l_name), char_name(cp, nb, sizeof nb));

    char u8[16]; int n = g_unichar_to_utf8(cp, u8);
    GString *s = g_string_new(NULL);
    g_string_append_printf(s, "U+%04X  ·  decimal %u  ·  &#x%X;\n", cp, cp, cp);
    g_string_append(s, "UTF-8  ");
    for (int i = 0; i < n; i++) g_string_append_printf(s, "%s%02X", i ? " " : "", (unsigned char)u8[i]);
    if (cp < 0x10000) g_string_append_printf(s, "   ·   UTF-16  %04X\n", cp);
    else g_string_append_printf(s, "   ·   UTF-16  %04X %04X\n", 0xD800 + ((cp - 0x10000) >> 10),
                                0xDC00 + ((cp - 0x10000) & 0x3FF));
    g_string_append_printf(s, "%s  ·  %s", cat_name(g_unichar_type(cp)), block_name_of(cp));
    gtk_label_set_text(GTK_LABEL(l_info), s->str);
    g_string_free(s, TRUE);
}

/* =================================================================== scrolling helpers */
static void scroll_to_value(double v) { gtk_adjustment_set_value(adj, v); }

static void ensure_visible(int s, int i)
{
    double top = sec_y[s] + HDR + (i / cols) * CELL, bot = top + CELL;
    double v = gtk_adjustment_get_value(adj);
    if (top < v + HDR) scroll_to_value(top - HDR - 4);
    else if (bot > v + da_h) scroll_to_value(bot - da_h + 4);
}

static void center_on(int s, int i)
{
    double top = sec_y[s] + HDR + (i / cols) * CELL;
    scroll_to_value(top - (da_h - CELL) / 2.0);
}

static void select_cell(int s, int i, gboolean scroll)
{
    sel_sec = s; sel_idx = i;
    update_details();
    if (scroll) ensure_visible(s, i);
    gtk_widget_queue_draw(da);
}

/* =================================================================== drawing */
static void get_colors(GdkRGBA *fg, GdkRGBA *bg, GdkRGBA *accent)
{
    GtkStyleContext *sc = gtk_widget_get_style_context(da);
    gtk_style_context_get_color(sc, fg);
    if (!gtk_style_context_lookup_color(sc, "window_bg_color", bg) &&
        !gtk_style_context_lookup_color(sc, "theme_bg_color", bg)) {
        double lum = 0.3 * fg->red + 0.6 * fg->green + 0.1 * fg->blue;
        *bg = lum > 0.5 ? (GdkRGBA){0.14, 0.14, 0.15, 1} : (GdkRGBA){0.98, 0.98, 0.98, 1};
    }
    if (!gtk_style_context_lookup_color(sc, "accent_bg_color", accent) &&
        !gtk_style_context_lookup_color(sc, "theme_selected_bg_color", accent))
        *accent = (GdkRGBA){0.21, 0.52, 0.89, 1};
}

static void set_rgba(cairo_t *cr, const GdkRGBA *c, double alpha)
{
    cairo_set_source_rgba(cr, c->red, c->green, c->blue, alpha);
}

static void draw_header(cairo_t *cr, const GdkRGBA *fg, const GdkRGBA *bg, int w, double y, int s, gboolean opaque)
{
    if (opaque) { set_rgba(cr, bg, 1); cairo_rectangle(cr, 0, y, w, HDR); cairo_fill(cr); }
    char rng[96];
    if (secs[s].block >= 0)
        snprintf(rng, sizeof rng, "U+%04X – U+%04X   ·   %d characters",
                 UC_BLOCKS[secs[s].block].start, UC_BLOCKS[secs[s].block].end, secs[s].count);
    else
        snprintf(rng, sizeof rng, secs[s].count ? "%d characters" : "no matches", secs[s].count);

    int tw, th;
    pango_layout_set_text(lay_hdr, secs[s].name, -1);
    pango_layout_get_pixel_size(lay_hdr, &tw, &th);
    set_rgba(cr, fg, 1);
    cairo_move_to(cr, PAD, y + (HDR - th) / 2.0);
    pango_cairo_show_layout(cr, lay_hdr);

    pango_layout_set_text(lay_cell, rng, -1);   /* reuse; font set below */
    PangoFontDescription *small = pango_font_description_from_string("Sans 9");
    pango_layout_set_font_description(lay_cell, small);
    int rw, rh;
    pango_layout_get_pixel_size(lay_cell, &rw, &rh);
    set_rgba(cr, fg, 0.55);
    cairo_move_to(cr, PAD + tw + 14, y + (HDR - rh) / 2.0 + 1);
    pango_cairo_show_layout(cr, lay_cell);
    PangoFontDescription *cellfont = pango_font_description_from_string("Sans 20");
    pango_layout_set_font_description(lay_cell, cellfont);
    pango_font_description_free(small);
    pango_font_description_free(cellfont);

    set_rgba(cr, fg, 0.22);
    cairo_rectangle(cr, PAD, y + HDR - 3, w - 2 * PAD, 1);
    cairo_fill(cr);
}

static void draw_cb(GtkDrawingArea *a, cairo_t *cr, int w, int h, gpointer d)
{
    if (!lay_cell) {
        PangoFontDescription *f = pango_font_description_from_string("Sans 20");
        lay_cell = gtk_widget_create_pango_layout(da, NULL);
        pango_layout_set_font_description(lay_cell, f);
        pango_font_description_free(f);
        f = pango_font_description_from_string("Sans Bold 11");
        lay_hdr = gtk_widget_create_pango_layout(da, NULL);
        pango_layout_set_font_description(lay_hdr, f);
        pango_font_description_free(f);
    }
    GdkRGBA fg, bg, accent;
    get_colors(&fg, &bg, &accent);
    set_rgba(cr, &bg, 1);
    cairo_paint(cr);

    double y0 = gtk_adjustment_get_value(adj);
    int s0 = find_sec(y0);
    if (s0 < 0) return;
    int x0 = grid_x0();
    cairo_set_line_width(cr, 1);

    for (int s = s0; s < nsecs && sec_y[s] < y0 + h; s++) {
        const Section *sc = &secs[s];
        double hy = sec_y[s] - y0;
        draw_header(cr, &fg, &bg, w, hy, s, FALSE);

        double gy = hy + HDR;
        int rows = (sc->count + cols - 1) / cols;
        int r0 = MAX(0, (int)floor(-gy / CELL));
        int r1 = MIN(rows, (int)ceil((h - gy) / CELL));

        /* backgrounds + grid lines */
        for (int r = r0; r < r1; r++)
            for (int c = 0; c < cols; c++) {
                int idx = r * cols + c;
                if (idx >= sc->count) break;
                double x = x0 + c * CELL, y = gy + r * CELL;
                if (s == sel_sec && idx == sel_idx) {
                    set_rgba(cr, &accent, 0.45);
                    cairo_rectangle(cr, x, y, CELL, CELL); cairo_fill(cr);
                } else if (s == hov_sec && idx == hov_idx) {
                    set_rgba(cr, &fg, 0.10);
                    cairo_rectangle(cr, x, y, CELL, CELL); cairo_fill(cr);
                }
            }
        set_rgba(cr, &fg, 0.10);
        for (int r = r0; r < r1; r++)
            for (int c = 0; c < cols; c++) {
                if (r * cols + c >= sc->count) break;
                cairo_rectangle(cr, x0 + c * CELL + .5, gy + r * CELL + .5, CELL - 1, CELL - 1);
            }
        cairo_stroke(cr);

        /* glyphs */
        set_rgba(cr, &fg, 1);
        char t[16];
        for (int r = r0; r < r1; r++)
            for (int c = 0; c < cols; c++) {
                int idx = r * cols + c;
                if (idx >= sc->count) break;
                cell_text(sc->chars[idx], t);
                pango_layout_set_text(lay_cell, t, -1);
                int tw, th;
                pango_layout_get_pixel_size(lay_cell, &tw, &th);
                cairo_move_to(cr, x0 + c * CELL + (CELL - tw) / 2.0, gy + r * CELL + (CELL - th) / 2.0);
                pango_cairo_show_layout(cr, lay_cell);
            }
    }

    /* sticky header of the section currently at the top */
    if (sec_y[s0] < (int)y0) {
        double sy = 0;
        if (s0 + 1 < nsecs) sy = MIN(0.0, (sec_y[s0 + 1] - y0) - HDR);
        draw_header(cr, &fg, &bg, w, sy, s0, TRUE);
    }
}

/* =================================================================== input */
static gboolean hit_test(double x, double y, int *s, int *i)
{
    int sc = find_sec(y + gtk_adjustment_get_value(adj));
    if (sc < 0) return FALSE;
    double ly = y + gtk_adjustment_get_value(adj) - sec_y[sc] - HDR;
    double lx = x - grid_x0();
    if (ly < 0 || lx < 0) return FALSE;
    int col = (int)(lx / CELL);
    if (col >= cols) return FALSE;
    int idx = (int)(ly / CELL) * cols + col;
    if (idx >= secs[sc].count) return FALSE;
    *s = sc; *i = idx;
    return TRUE;
}

static void append_to_buffer(uint32_t cp)
{
    char u[8]; u[g_unichar_to_utf8(cp, u)] = 0;
    char *n = g_strconcat(gtk_editable_get_text(GTK_EDITABLE(buf_entry)), u, NULL);
    gtk_editable_set_text(GTK_EDITABLE(buf_entry), n);
    gtk_editable_set_position(GTK_EDITABLE(buf_entry), -1);
    g_free(n);
}

static void on_click(GtkGestureClick *g, int n_press, double x, double y, gpointer d)
{
    gtk_widget_grab_focus(da);
    int s, i;
    if (!hit_test(x, y, &s, &i)) return;
    select_cell(s, i, FALSE);
    if (n_press == 2) append_to_buffer(secs[s].chars[i]);
}

static void on_right_click(GtkGestureClick *g, int n_press, double x, double y, gpointer d)
{
    int s, i;
    if (!hit_test(x, y, &s, &i)) return;
    select_cell(s, i, FALSE);
    char u[8]; u[g_unichar_to_utf8(secs[s].chars[i], u)] = 0;
    copy_text(u);
}

static void on_motion(GtkEventControllerMotion *c, double x, double y, gpointer d)
{
    int s = -1, i = -1;
    if (!hit_test(x, y, &s, &i)) { s = -1; i = -1; }
    if (s != hov_sec || i != hov_idx) { hov_sec = s; hov_idx = i; gtk_widget_queue_draw(da); }
}

static void on_leave(GtkEventControllerMotion *c, gpointer d)
{
    hov_sec = hov_idx = -1;
    gtk_widget_queue_draw(da);
}

static gboolean on_tooltip(GtkWidget *w, int x, int y, gboolean kb, GtkTooltip *tip, gpointer d)
{
    int s, i;
    if (!hit_test(x, y, &s, &i)) return FALSE;
    uint32_t cp = secs[s].chars[i];
    char nb[96], t[160];
    snprintf(t, sizeof t, "U+%04X  %s", cp, char_name(cp, nb, sizeof nb));
    gtk_tooltip_set_text(tip, t);
    return TRUE;
}

static gboolean on_scroll(GtkEventControllerScroll *c, double dx, double dy, gpointer d)
{
    gtk_adjustment_set_value(adj, gtk_adjustment_get_value(adj) + dy * CELL * 1.2);
    return TRUE;
}

static void move_sel(int dir)
{
    int s = sel_sec, i = sel_idx;
    if (s < 0) { if (nsecs > 0 && secs[0].count) select_cell(0, 0, TRUE); return; }
    int cnt = secs[s].count, rows = (cnt + cols - 1) / cols;
    switch (dir) {
    case 0: if (i > 0) i--; else if (s > 0) { s--; i = secs[s].count - 1; } break;
    case 1: if (i < cnt - 1) i++; else if (s < nsecs - 1) { s++; i = 0; } break;
    case 2:
        if (i >= cols) i -= cols;
        else if (s > 0) {
            int col = i % cols; s--;
            int r2 = (secs[s].count + cols - 1) / cols;
            i = MIN((r2 - 1) * cols + col, secs[s].count - 1);
        }
        break;
    case 3:
        if (i + cols < cnt) i += cols;
        else if (i / cols < rows - 1) i = cnt - 1;
        else if (s < nsecs - 1) { int col = i % cols; s++; i = MIN(col, secs[s].count - 1); }
        break;
    }
    if (secs[s].count > 0) select_cell(s, i, TRUE);
}

static gboolean on_key(GtkEventControllerKey *c, guint key, guint code, GdkModifierType st, gpointer d)
{
    gboolean ok;
    uint32_t cp;
    switch (key) {
    case GDK_KEY_Left:  move_sel(0); return TRUE;
    case GDK_KEY_Right: move_sel(1); return TRUE;
    case GDK_KEY_Up:    move_sel(2); return TRUE;
    case GDK_KEY_Down:  move_sel(3); return TRUE;
    case GDK_KEY_Page_Up: case GDK_KEY_Page_Down: {
        int n = MAX(1, da_h / CELL - 1);
        for (int k = 0; k < n; k++) move_sel(key == GDK_KEY_Page_Up ? 2 : 3);
        return TRUE; }
    case GDK_KEY_Home:
        if (nsecs > 0 && secs[0].count) select_cell(0, 0, FALSE);
        scroll_to_value(0); return TRUE;
    case GDK_KEY_End:
        if (nsecs > 0 && secs[nsecs - 1].count) select_cell(nsecs - 1, secs[nsecs - 1].count - 1, TRUE);
        return TRUE;
    case GDK_KEY_Return: case GDK_KEY_KP_Enter:
        cp = selected_cp(&ok); if (ok) append_to_buffer(cp);
        return TRUE;
    case GDK_KEY_c: case GDK_KEY_C:
        if (st & GDK_CONTROL_MASK) {
            cp = selected_cp(&ok);
            if (ok) { char u[8]; u[g_unichar_to_utf8(cp, u)] = 0; copy_text(u); }
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void sync_list(void);

static void on_resize(GtkDrawingArea *a, int w, int h, gpointer d)
{
    int oldcols = cols;
    da_w = w; da_h = h;
    int newcols = MAX(1, (w - 2 * PAD) / CELL);
    if (!sec_y || newcols == oldcols || nsecs == 0) { relayout(); sync_list(); return; }

    /* keep the same character at the top when the column count changes */
    double v = gtk_adjustment_get_value(adj);
    int s = find_sec(v), idx = -1;
    double off = v - sec_y[s];
    if (off >= HDR) idx = (int)((off - HDR) / CELL) * oldcols;
    relayout();
    gtk_adjustment_set_value(adj, sec_y[s] + (idx >= 0 ? HDR + (idx / cols) * CELL : 0));
    sync_list();
}

/* =================================================================== jump list */
static void list_scroll_to(GtkWidget *row)
{
    graphene_rect_t b;
    if (!gtk_widget_compute_bounds(row, list_box, &b)) return;
    GtkAdjustment *la = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(list_sw));
    double top = b.origin.y, bot = top + b.size.height;
    double v = gtk_adjustment_get_value(la), ps = gtk_adjustment_get_page_size(la);
    if (top < v) gtk_adjustment_set_value(la, top - 30);
    else if (bot > v + ps) gtk_adjustment_set_value(la, bot - ps + 30);
}

static void sync_list(void)
{
    if (searching || !list_rows || !sec_y) return;
    int s = find_sec(gtk_adjustment_get_value(adj) + 1);
    if (s < 0 || s == list_cur) return;
    list_cur = s;
    gtk_list_box_select_row(GTK_LIST_BOX(list_box), GTK_LIST_BOX_ROW(list_rows[s]));
    list_scroll_to(list_rows[s]);
}

static void on_value_changed(GtkAdjustment *a, gpointer d)
{
    gtk_widget_queue_draw(da);
    sync_list();
}

static void rebuild_list(void)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(list_box))) gtk_list_box_remove(GTK_LIST_BOX(list_box), c);
    g_free(list_rows);
    list_rows = g_new0(GtkWidget *, ordn);
    list_cur = -1;
    int lastg = -1;
    for (int k = 0; k < ordn; k++) {
        const UBlock *b = &UC_BLOCKS[ord[k]];
        if (by_category && b->group != lastg) {
            lastg = b->group;
            GtkWidget *h = gtk_list_box_row_new();
            gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(h), FALSE);
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(h), FALSE);
            GtkWidget *l = gtk_label_new(UC_GROUPS[lastg]);
            gtk_label_set_xalign(GTK_LABEL(l), 0);
            gtk_widget_add_css_class(l, "group-header");
            gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(h), l);
            gtk_list_box_append(GTK_LIST_BOX(list_box), h);
        }
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_top(box, 4); gtk_widget_set_margin_bottom(box, 4);
        gtk_widget_set_margin_start(box, 6); gtk_widget_set_margin_end(box, 6);
        char hx[16]; snprintf(hx, sizeof hx, "U+%04X", b->start);
        GtkWidget *h = gtk_label_new(hx);
        gtk_widget_add_css_class(h, "dim"); gtk_widget_add_css_class(h, "monospace");
        GtkWidget *n = gtk_label_new(b->name);
        gtk_label_set_xalign(GTK_LABEL(n), 0);
        gtk_label_set_ellipsize(GTK_LABEL(n), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(box), h);
        gtk_box_append(GTK_BOX(box), n);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        g_object_set_data(G_OBJECT(row), "k", GINT_TO_POINTER(k + 1));
        gtk_list_box_append(GTK_LIST_BOX(list_box), row);
        list_rows[k] = row;
    }
}

/* =================================================================== search */
static void do_search(const char *q)
{
    g_free(results); results = NULL; nresults = 0;
    if (!*q) { searching = FALSE; return; }
    searching = TRUE;
    GArray *res = g_array_new(FALSE, FALSE, sizeof(uint32_t));

    const char *p = q;
    if (!g_ascii_strncasecmp(p, "u+", 2) || !g_ascii_strncasecmp(p, "0x", 2)) p += 2;
    gboolean hex = *p && strlen(p) <= 6;
    for (const char *c = p; hex && *c; c++) if (!g_ascii_isxdigit(*c)) hex = FALSE;
    uint32_t first = 0, second = 0; gboolean h1 = FALSE, h2 = FALSE;
    if (hex) {
        uint32_t cp = strtoul(p, NULL, 16);
        if (valid_cp(cp)) { g_array_append_val(res, cp); first = cp; h1 = TRUE; }
    }
    if (g_utf8_strlen(q, -1) == 1) {
        uint32_t cp = g_utf8_get_char(q);
        if (valid_cp(cp) && !(h1 && cp == first)) { g_array_append_val(res, cp); second = cp; h2 = TRUE; }
    }

    char *up = g_ascii_strup(q, -1);
    char **w = g_strsplit_set(up, " \t", -1);
    int nw = 0; char *words[16];
    for (int i = 0; w[i] && nw < 16; i++) if (*w[i]) words[nw++] = w[i];
    for (int i = 0; nw > 0 && i < UC_NNAMES && res->len < 5000; i++) {
        const char *nm = UC_BLOB + UC_NAMES[i].off;
        gboolean all = TRUE;
        for (int k = 0; k < nw && all; k++) if (!strstr(nm, words[k])) all = FALSE;
        uint32_t cp = UC_NAMES[i].cp;
        if (all && !(h1 && cp == first) && !(h2 && cp == second)) g_array_append_val(res, cp);
    }
    g_strfreev(w); g_free(up);
    nresults = res->len;
    results = (uint32_t *)g_array_free(res, FALSE);
    snprintf(search_title, sizeof search_title, "Search: %s", q);
}

static void apply_search(const char *raw)
{
    char *q = g_strstrip(g_strdup(raw));
    if (g_strcmp0(q, last_query ? last_query : "") == 0) { g_free(q); return; }
    g_free(last_query); last_query = g_strdup(q);

    do_search(q);
    g_free(q);
    build_sections();
    if (searching) {
        gtk_adjustment_set_value(adj, 0);
        relayout();
        restore_selection();
        if (nresults) { sel_sec = 0; sel_idx = 0; }
        else sel_sec = sel_idx = -1;
        update_details();
    } else {
        relayout();
        restore_selection();
        update_details();
        if (sel_sec >= 0) center_on(sel_sec, sel_idx);
        list_cur = -1;
        sync_list();
    }
    gtk_widget_queue_draw(da);
}

static void on_search_changed(GtkSearchEntry *e, gpointer d)
{
    apply_search(gtk_editable_get_text(GTK_EDITABLE(e)));
}

static void on_stop_search(GtkSearchEntry *e, gpointer d)
{
    gtk_editable_set_text(GTK_EDITABLE(e), "");
    apply_search("");
    gtk_widget_grab_focus(da);
}

static void on_row_activated(GtkListBox *lb, GtkListBoxRow *row, gpointer d)
{
    int k = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "k")) - 1;
    if (k < 0) return;
    if (searching) {
        gtk_editable_set_text(GTK_EDITABLE(search), "");
        apply_search("");
    }
    if (k < nsecs) gtk_adjustment_set_value(adj, sec_y[k]);
    list_cur = -1;
    sync_list();
}

static void on_sort_changed(GObject *o, GParamSpec *p, gpointer d)
{
    by_category = gtk_drop_down_get_selected(GTK_DROP_DOWN(sort_dd)) == 1;
    int cur_block = -1;
    if (!searching) {
        int s = find_sec(gtk_adjustment_get_value(adj));
        if (s >= 0) cur_block = secs[s].block;
    }
    build_order();
    rebuild_list();
    if (!searching) {
        build_sections();
        relayout();
        restore_selection();
        for (int k = 0; k < nsecs; k++)
            if (secs[k].block == cur_block) { gtk_adjustment_set_value(adj, sec_y[k]); break; }
        sync_list();
    }
    gtk_widget_queue_draw(da);
}

/* =================================================================== buttons */
static void on_copy_char(GtkButton *b, gpointer d)
{
    gboolean ok; uint32_t cp = selected_cp(&ok);
    if (ok) { char u[8]; u[g_unichar_to_utf8(cp, u)] = 0; copy_text(u); }
}

static void on_copy_code(GtkButton *b, gpointer d)
{
    gboolean ok; uint32_t cp = selected_cp(&ok);
    if (ok) { char t[16]; snprintf(t, sizeof t, "U+%04X", cp); copy_text(t); }
}

static void on_copy_buf(GtkButton *b, gpointer d)
{
    copy_text(gtk_editable_get_text(GTK_EDITABLE(buf_entry)));
}

static void on_clear_buf(GtkButton *b, gpointer d)
{
    gtk_editable_set_text(GTK_EDITABLE(buf_entry), "");
}

/* =================================================================== UI */
static const char *CSS =
    ".big { font-size: 60px; }"
    ".dim { opacity: 0.6; }"
    ".group-header { font-weight: bold; font-size: 11px; opacity: 0.65; padding: 10px 8px 2px 8px; }";

static void activate(GApplication *app, gpointer d)
{
    GtkCssProvider *prov = gtk_css_provider_new();
#if GTK_CHECK_VERSION(4, 12, 0)
    gtk_css_provider_load_from_string(prov, CSS);
#else
    gtk_css_provider_load_from_data(prov, CSS, -1);
#endif
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(prov),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    init_data();
    build_order();
    build_sections();

    win = gtk_application_window_new(GTK_APPLICATION(app));
    gtk_window_set_title(GTK_WINDOW(win), "Moji");
    gtk_window_set_default_size(GTK_WINDOW(win), 1100, 720);

    /* header bar with search */
    GtkWidget *hb = gtk_header_bar_new();
    search = gtk_search_entry_new();
    gtk_widget_set_size_request(search, 400, -1);
    gtk_editable_set_text(GTK_EDITABLE(search), "");
    g_object_set(search, "placeholder-text", "Search by name, character or U+code…", NULL);
    g_signal_connect(search, "search-changed", G_CALLBACK(on_search_changed), NULL);
    g_signal_connect(search, "stop-search", G_CALLBACK(on_stop_search), NULL);
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(hb), search);
    gtk_window_set_titlebar(GTK_WINDOW(win), hb);
    gtk_search_entry_set_key_capture_widget(GTK_SEARCH_ENTRY(search), win);

    /* grid */
    adj = gtk_adjustment_new(0, 0, 100, CELL, 300, 100);
    g_signal_connect(adj, "value-changed", G_CALLBACK(on_value_changed), NULL);
    da = gtk_drawing_area_new();
    gtk_widget_set_hexpand(da, TRUE);
    gtk_widget_set_vexpand(da, TRUE);
    gtk_widget_set_size_request(da, 420, 300);
    gtk_widget_set_focusable(da, TRUE);
    gtk_widget_set_has_tooltip(da, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), draw_cb, NULL, NULL);
    g_signal_connect(da, "resize", G_CALLBACK(on_resize), NULL);
    g_signal_connect(da, "query-tooltip", G_CALLBACK(on_tooltip), NULL);

    GtkGesture *g1 = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g1), GDK_BUTTON_PRIMARY);
    g_signal_connect(g1, "pressed", G_CALLBACK(on_click), NULL);
    gtk_widget_add_controller(da, GTK_EVENT_CONTROLLER(g1));
    GtkGesture *g3 = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g3), GDK_BUTTON_SECONDARY);
    g_signal_connect(g3, "pressed", G_CALLBACK(on_right_click), NULL);
    gtk_widget_add_controller(da, GTK_EVENT_CONTROLLER(g3));
    GtkEventController *mc = gtk_event_controller_motion_new();
    g_signal_connect(mc, "motion", G_CALLBACK(on_motion), NULL);
    g_signal_connect(mc, "leave", G_CALLBACK(on_leave), NULL);
    gtk_widget_add_controller(da, mc);
    GtkEventController *sc = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(sc, "scroll", G_CALLBACK(on_scroll), NULL);
    gtk_widget_add_controller(da, sc);
    GtkEventController *kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key), NULL);
    gtk_widget_add_controller(da, kc);

    GtkWidget *sb = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, adj);
    GtkWidget *grid_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(grid_box, TRUE);
    gtk_box_append(GTK_BOX(grid_box), da);
    gtk_box_append(GTK_BOX(grid_box), sb);

    /* right: jump list */
    sort_dd = gtk_drop_down_new_from_strings((const char *[]){"Ordered by code point", "Grouped by category", NULL});
    gtk_widget_set_margin_start(sort_dd, 8); gtk_widget_set_margin_end(sort_dd, 8);
    gtk_widget_set_margin_top(sort_dd, 8);   gtk_widget_set_margin_bottom(sort_dd, 6);
    g_signal_connect(sort_dd, "notify::selected", G_CALLBACK(on_sort_changed), NULL);
    list_box = gtk_list_box_new();
    gtk_widget_add_css_class(list_box, "navigation-sidebar");
    g_signal_connect(list_box, "row-activated", G_CALLBACK(on_row_activated), NULL);
    list_sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(list_sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(list_sw), list_box);
    gtk_widget_set_vexpand(list_sw, TRUE);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(right, 270, -1);
    gtk_box_append(GTK_BOX(right), sort_dd);
    gtk_box_append(GTK_BOX(right), list_sw);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(main_box, TRUE);
    gtk_box_append(GTK_BOX(main_box), grid_box);
    gtk_box_append(GTK_BOX(main_box), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
    gtk_box_append(GTK_BOX(main_box), right);

    /* bottom: details */
    l_big = gtk_label_new("");
    gtk_widget_add_css_class(l_big, "big");
    gtk_widget_set_size_request(l_big, 120, -1);
    l_name = gtk_label_new("");
    gtk_widget_add_css_class(l_name, "title-3");
    gtk_label_set_xalign(GTK_LABEL(l_name), 0);
    gtk_label_set_selectable(GTK_LABEL(l_name), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(l_name), PANGO_ELLIPSIZE_END);
    l_info = gtk_label_new("");
    gtk_widget_add_css_class(l_info, "monospace");
    gtk_label_set_xalign(GTK_LABEL(l_info), 0);
    gtk_label_set_selectable(GTK_LABEL(l_info), TRUE);
    GtkWidget *txt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(txt, TRUE);
    gtk_widget_set_valign(txt, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(txt), l_name);
    gtk_box_append(GTK_BOX(txt), l_info);

    GtkWidget *b1 = gtk_button_new_with_label("Copy character");
    GtkWidget *b2 = gtk_button_new_with_label("Copy U+code");
    g_signal_connect(b1, "clicked", G_CALLBACK(on_copy_char), NULL);
    g_signal_connect(b2, "clicked", G_CALLBACK(on_copy_code), NULL);
    GtkWidget *bb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_valign(bb, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(bb), b1);
    gtk_box_append(GTK_BOX(bb), b2);

    GtkWidget *det = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_set_margin_start(det, 12); gtk_widget_set_margin_end(det, 12);
    gtk_widget_set_margin_top(det, 8);    gtk_widget_set_margin_bottom(det, 8);
    gtk_box_append(GTK_BOX(det), l_big);
    gtk_box_append(GTK_BOX(det), txt);
    gtk_box_append(GTK_BOX(det), bb);

    /* bottom: text collector */
    buf_entry = gtk_entry_new();
    gtk_widget_set_hexpand(buf_entry, TRUE);
    g_object_set(buf_entry, "placeholder-text", "Double-click or press Enter on characters to collect them here…", NULL);
    GtkWidget *b3 = gtk_button_new_with_label("Copy text");
    GtkWidget *b4 = gtk_button_new_with_label("Clear");
    l_toast = gtk_label_new("");
    gtk_widget_add_css_class(l_toast, "dim");
    gtk_widget_set_size_request(l_toast, 60, -1);
    g_signal_connect(b3, "clicked", G_CALLBACK(on_copy_buf), NULL);
    g_signal_connect(b4, "clicked", G_CALLBACK(on_clear_buf), NULL);
    GtkWidget *row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(row2, 12); gtk_widget_set_margin_end(row2, 12);
    gtk_widget_set_margin_bottom(row2, 10);
    gtk_box_append(GTK_BOX(row2), buf_entry);
    gtk_box_append(GTK_BOX(row2), b3);
    gtk_box_append(GTK_BOX(row2), b4);
    gtk_box_append(GTK_BOX(row2), l_toast);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(root), main_box);
    gtk_box_append(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_box_append(GTK_BOX(root), det);
    gtk_box_append(GTK_BOX(root), row2);
    gtk_window_set_child(GTK_WINDOW(win), root);

    rebuild_list();
    restore_selection();
    update_details();
    gtk_window_present(GTK_WINDOW(win));
    gtk_widget_grab_focus(da);
}

int main(int argc, char **argv)
{
#if GLIB_CHECK_VERSION(2, 74, 0)
    GtkApplication *app = gtk_application_new("local.Moji", G_APPLICATION_DEFAULT_FLAGS);
#else
    GtkApplication *app = gtk_application_new("local.Moji", G_APPLICATION_FLAGS_NONE);
#endif
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int r = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return r;
}
