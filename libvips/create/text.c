/* vips_text
 *
 * Written on: 20/5/04
 * 29/7/04
 *	- !HAVE_PANGOFT2 was broken, thanks Kenneth
 * 15/11/04
 *	- gah, still broken, thanks Stefan
 * 5/4/06
 * 	- return an error for im_text( "" ) rather than trying to make an
 * 	  empty image
 * 2/2/10
 * 	- gtkdoc
 * 3/6/13
 * 	- rewrite as a class
 * 20/9/15 leiyangyou
 * 	- add @spacing
 * 29/5/17
 * 	- don't set "font" if unset, it breaks caching
 * 16/7/17 gargsms
 * 	- implement auto fitting of text inside bounds
 * 12/3/18
 * 	- better fitting of fonts with overhanging edges, thanks Adrià
 * 26/4/18 fangqiao
 * 	- add fontfile option
 * 5/12/18
 * 	- fitting mode could set wrong dpi
 * 	- fitting mode leaked
 * 16/3/19
 * 	- add `justify`
 * 	- set Xoffset/Yoffset to ink left/top
 * 27/6/19
 * 	- fitting could occasionally terminate early [levmorozov]
 * 16/5/20 [keiviv]
 * 	- don't add fontfiles repeatedly
 * 12/4/21
 * 	- switch to cairo for text rendering
 * 	- add rgba flag
 * 31/10/22
 * 	- add @wrap
 * 14/1/23
 *	- make our own fontmap to prevent conflict with other API users
 * 15/2/23
 *	- allow negative line spacing
 */

/*

	This file is part of VIPS.

	VIPS is free software; you can redistribute it and/or modify
	it under the terms of the GNU Lesser General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU Lesser General Public License for more details.

	You should have received a copy of the GNU Lesser General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
	02110-1301  USA

 */

/*

	These files are distributed with VIPS - http://www.vips.ecs.soton.ac.uk

 */

/*
#define DEBUG
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/
#include <glib/gi18n-lib.h>

#include <stdio.h>
#include <string.h>

#include <vips/vips.h>
#include <vips/internal.h>

#ifdef HAVE_PANGOCAIRO

#include <cairo.h>
#include <pango/pango.h>
#include <pango/pangocairo.h>

#ifdef HAVE_FONTCONFIG
/* ftconfig.h also appears to define HAVE_UNISTD_H.
 */
#ifdef HAVE_UNISTD_H
#undef HAVE_UNISTD_H
#endif
#include <pango/pangofc-fontmap.h>
#include <fontconfig/fontconfig.h>
#endif

#include "pcreate.h"

typedef struct _VipsText {
	VipsCreate parent_instance;

	char *text;
	char *font;
	int width;
	int height;
	int spacing;
	VipsAlign align;
	gboolean justify;
	int dpi;
	char *fontfile;
	gboolean rgba;
	VipsTextWrap wrap;

	PangoContext *context;
	PangoLayout *layout;

} VipsText;

typedef VipsCreateClass VipsTextClass;

G_DEFINE_TYPE(VipsText, vips_text, VIPS_TYPE_CREATE);

/* These are expensive and do not unref cleanly on many platforms. We keep a
 * single value for libvips and reuse it behind a lock.
 *
 * Have one shared between libvips threads.
 */
static PangoFontMap *vips_text_fontmap = NULL;

/* ... single-thread vips_text_fontfiles with this.
 */
static GMutex vips_text_lock;

/* All the fontfiles we've loaded. fontconfig lets you add a fontfile
 * repeatedly, and we obviously don't want that.
 */
static GHashTable *vips_text_fontfiles = NULL;

static void
vips_text_dispose(GObject *gobject)
{
	VipsText *text = (VipsText *) gobject;

	VIPS_UNREF(text->layout);
	VIPS_UNREF(text->context);

	G_OBJECT_CLASS(vips_text_parent_class)->dispose(gobject);
}

static PangoLayout *
text_layout_new(PangoContext *context,
	const char *text, const char *font, int width, int spacing,
	VipsAlign align, VipsTextWrap wrap, gboolean justify)
{
	PangoLayout *layout;
	PangoFontDescription *font_description;
	PangoAlignment palign;
	PangoWrapMode pwrap;
	int pwidth;

	layout = pango_layout_new(context);
	pango_layout_set_markup(layout, text, -1);

	font_description = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, font_description);
	pango_font_description_free(font_description);
	pango_layout_set_justify(layout, justify);
	pango_layout_set_spacing(layout, spacing * PANGO_SCALE);

	switch (align) {
	case VIPS_ALIGN_LOW:
		palign = PANGO_ALIGN_LEFT;
		break;

	case VIPS_ALIGN_CENTRE:
		palign = PANGO_ALIGN_CENTER;
		break;

	case VIPS_ALIGN_HIGH:
		palign = PANGO_ALIGN_RIGHT;
		break;

	default:
		palign = PANGO_ALIGN_LEFT;
		break;
	}
	pango_layout_set_alignment(layout, palign);

	switch (wrap) {
	case VIPS_TEXT_WRAP_NONE:
		pwrap = PANGO_WRAP_WORD_CHAR;
		pwidth = -1;
		break;

	case VIPS_TEXT_WRAP_CHAR:
		pwrap = PANGO_WRAP_CHAR;
		pwidth = width * PANGO_SCALE;
		break;

	case VIPS_TEXT_WRAP_WORD:
		pwrap = PANGO_WRAP_WORD;
		pwidth = width * PANGO_SCALE;
		break;

	case VIPS_TEXT_WRAP_WORD_CHAR:
	default:
		pwrap = PANGO_WRAP_WORD_CHAR;
		pwidth = width * PANGO_SCALE;
		break;
	}

	pango_layout_set_wrap(layout, pwrap);

	if (pwidth > 0)
		pango_layout_set_width(layout, width * PANGO_SCALE);

	return layout;
}

static int
vips_text_get_extents(VipsText *text, VipsRect *extents)
{
	PangoRectangle ink_rect;
	PangoRectangle logical_rect;

	pango_cairo_font_map_set_resolution(
		PANGO_CAIRO_FONT_MAP(vips_text_fontmap), text->dpi);

	VIPS_UNREF(text->layout);
	if (!(text->layout = text_layout_new(text->context,
			  text->text, text->font,
			  text->width, text->spacing, text->align, text->wrap,
			  text->justify)))
		return -1;

	pango_layout_get_pixel_extents(text->layout, &ink_rect, &logical_rect);
	extents->left = ink_rect.x;
	extents->top = ink_rect.y;
	extents->width = ink_rect.width;
	extents->height = ink_rect.height;

#ifdef DEBUG
	printf("vips_text_get_extents: dpi = %d\n", text->dpi);
	printf("    ink left = %d, top = %d, width = %d, height = %d\n",
		ink_rect.x, ink_rect.y, ink_rect.width, ink_rect.height);
	printf("    logical left = %d, top = %d, width = %d, height = %d\n",
		logical_rect.x, logical_rect.y,
		logical_rect.width, logical_rect.height);
#endif /*DEBUG*/

	return 0;
}

/* Return -ve for extents too small, +ve for extents too large.
 */
static int
vips_text_rect_difference(VipsRect *target, VipsRect *extents)
{
	if (vips_rect_includesrect(target, extents))
		return -1;
	else
		return 1;
}

/* Adjust text->dpi to try to fit to the bounding box.
 */
static int
vips_text_autofit(VipsText *text)
{
	VipsRect target;
	VipsRect extents;
	int difference;
	int previous_difference;
	int previous_dpi;

	int lower_dpi;
	int upper_dpi;

	/* First, repeatedly double or halve dpi until we pass the correct
	 * value. This will give us a lower and upper bound.
	 */
	target.left = 0;
	target.top = 0;
	target.width = text->width;
	target.height = text->height;
	previous_dpi = -1;
	previous_difference = 0;

#ifdef DEBUG
	printf("vips_text_autofit: "
		   "target left = %d, top = %d, width = %d, height = %d\n",
		target.left, target.top, target.width, target.height);
#endif /*DEBUG*/

	for (;;) {
		if (vips_text_get_extents(text, &extents))
			return -1;
		target.left = extents.left;
		target.top = extents.top;
		difference = vips_text_rect_difference(&target, &extents);

		if (previous_dpi == -1) {
			previous_dpi = text->dpi;
			previous_difference = difference;
		}

		/* Stop if we straddle the target.
		 */
		if (difference != previous_difference)
			break;

		previous_difference = difference;
		previous_dpi = text->dpi;

		text->dpi = difference < 0 ? text->dpi * 2 : text->dpi / 2; // FIXME: Invalidates operation cache

		/* This can happen with fixed-size fonts.
		 */
		if (text->dpi < 2 ||
			text->dpi > 10000)
			break;
	}

	if (difference < 0) {
		/* We've been coming down.
		 */
		lower_dpi = text->dpi;
		upper_dpi = previous_dpi;
	}
	else {
		lower_dpi = previous_dpi;
		upper_dpi = text->dpi;
	}

#ifdef DEBUG
	printf("vips_text_autofit: lower dpi = %d, upper dpi = %d\n",
		lower_dpi, upper_dpi);
#endif /*DEBUG*/

	/* Refine lower and upper until they are almost touching.
	 */
	while (upper_dpi - lower_dpi > 1 &&
		difference != 0) {
		text->dpi = (upper_dpi + lower_dpi) / 2; // FIXME: Invalidates operation cache
		if (vips_text_get_extents(text, &extents))
			return -1;
		target.left = extents.left;
		target.top = extents.top;
		difference = vips_text_rect_difference(&target, &extents);

		if (difference < 0)
			lower_dpi = text->dpi;
		else
			upper_dpi = text->dpi;
	}

	/* If we've hit the target exactly and quit the loop, diff will be 0
	 * and we can use upper. Otherwise we are straddling the target and we
	 * must take lower.
	 */
	if (difference == 0)
		text->dpi = upper_dpi; // FIXME: Invalidates operation cache
	else
		text->dpi = lower_dpi; // FIXME: Invalidates operation cache
	g_object_set(text, "autofit_dpi", text->dpi, NULL);

#ifdef DEBUG
	printf("vips_text_autofit: final dpi = %d\n", text->dpi);
#endif /*DEBUG*/

	return 0;
}

static void *
vips_text_init_once(void *client)
{
	vips_text_fontmap = pango_cairo_font_map_new();
	vips_text_fontfiles = g_hash_table_new(g_str_hash, g_str_equal);

	return NULL;
}

static int
vips_text_build(VipsObject *object)
{
	static GOnce once = G_ONCE_INIT;

	VipsObjectClass *class = VIPS_OBJECT_GET_CLASS(object);
	VipsCreate *create = VIPS_CREATE(object);
	VipsText *text = (VipsText *) object;
	VipsImage **t = (VipsImage **) vips_object_local_array(object, 3);

	VipsRect extents;
	VipsImage *in;
	cairo_surface_t *surface;
	cairo_t *cr;
	cairo_status_t status;

	if (VIPS_OBJECT_CLASS(vips_text_parent_class)->build(object))
		return -1;

	if (!pango_parse_markup(text->text, -1, 0, NULL, NULL, NULL, NULL)) {
		vips_error(class->nickname,
			"%s", _("invalid markup in text"));
		return -1;
	}

	VIPS_ONCE(&once, vips_text_init_once, NULL);

	text->context = pango_font_map_create_context(vips_text_fontmap);

	if (text->rgba) {
		/* Prevent use of subpixel anti-aliasing to avoid artefacts.
		 */
		cairo_font_options_t *opts = cairo_font_options_create();
		cairo_font_options_set_antialias(opts, CAIRO_ANTIALIAS_GRAY);
		pango_cairo_context_set_font_options(text->context, opts);
		cairo_font_options_destroy(opts);
	}

	/* Because we set resolution on vips_text_fontmap and that's shared
	 * between all vips_text instances, we must lock all the way to the
	 * end of text rendering.
	 */
	g_mutex_lock(&vips_text_lock);

#ifdef HAVE_FONTCONFIG
	if (text->fontfile &&
		!g_hash_table_contains(vips_text_fontfiles, text->fontfile)) {
		/* This can fail if you eg. add the same font from two
		 * different files. Just warn.
		 */
		if (!FcConfigAppFontAddFile(NULL,
				(const FcChar8 *) text->fontfile))
			g_warning("unable to load fontfile \"%s\"",
				text->fontfile);
		g_hash_table_insert(vips_text_fontfiles,
			g_strdup(text->fontfile), NULL);

		/* We need to inform that pango should invalidate its
		 * fontconfig cache whenever any changes are made.
		 */
		if (PANGO_IS_FC_FONT_MAP(vips_text_fontmap))
			pango_fc_font_map_cache_clear(
				PANGO_FC_FONT_MAP(vips_text_fontmap));
	}
#else  /*!HAVE_FONTCONFIG*/
	if (text->fontfile)
		g_warning("ignoring fontfile (no fontconfig support)");
#endif /*HAVE_FONTCONFIG*/

	/* If our caller set height and not dpi, we adjust dpi until
	 * we get a fit.
	 */
	if (vips_object_argument_isset(object, "height") &&
		!vips_object_argument_isset(object, "dpi")) {
		if (vips_text_autofit(text)) {
			g_mutex_unlock(&vips_text_lock);
			return -1;
		}
	}

	/* Layout. Can fail for "", for example.
	 */
	if (vips_text_get_extents(text, &extents)) {
		g_mutex_unlock(&vips_text_lock);
		return -1;
	}

	if (extents.width == 0 ||
		extents.height == 0) {
		g_mutex_unlock(&vips_text_lock);
		vips_error(class->nickname, "%s", _("no text to render"));
		return -1;
	}

	t[0] = vips_image_new_memory();
	vips_image_init_fields(t[0],
		extents.width, extents.height, 4,
		VIPS_FORMAT_UCHAR, VIPS_CODING_NONE, VIPS_INTERPRETATION_sRGB,
		text->dpi / 25.4, text->dpi / 25.4);
	t[0]->Xoffset = extents.left;
	t[0]->Yoffset = extents.top;

	if (vips_image_pipelinev(t[0], VIPS_DEMAND_STYLE_ANY, NULL) ||
		vips_image_write_prepare(t[0])) {
		g_mutex_unlock(&vips_text_lock);
		return -1;
	}
	in = t[0];

	surface = cairo_image_surface_create_for_data(
		VIPS_IMAGE_ADDR(in, 0, 0),
		CAIRO_FORMAT_ARGB32,
		in->Xsize, in->Ysize,
		VIPS_IMAGE_SIZEOF_LINE(in));

	status = cairo_surface_status(surface);
	if (status) {
		cairo_surface_destroy(surface);
		g_mutex_unlock(&vips_text_lock);
		vips_error(class->nickname,
			"%s", cairo_status_to_string(status));
		return -1;
	}

	cr = cairo_create(surface);
	cairo_surface_destroy(surface);

	cairo_translate(cr, -extents.left, -extents.top);

	pango_cairo_show_layout(cr, text->layout);

	cairo_destroy(cr);

	g_mutex_unlock(&vips_text_lock);

	if (text->rgba) {
		/* Cairo makes pre-multipled BRGA -- we must byteswap and
		 * unpremultiply.
		 */
		for (int y = 0; y < in->Ysize; y++)
			vips__premultiplied_bgra2rgba(
				(guint32 *) VIPS_IMAGE_ADDR(in, 0, y),
				in->Xsize);
	}
	else {
		/* We just want the alpha channel.
		 */
		if (vips_extract_band(in, &t[1], 3, NULL) ||
			vips_copy(t[1], &t[2],
				"interpretation", VIPS_INTERPRETATION_MULTIBAND,
				NULL))
			return -1;
		in = t[2];
	}

	if (vips_image_write(in, create->out))
		return -1;

	return 0;
}

static void
vips_text_class_init(VipsTextClass *class)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(class);
	VipsObjectClass *vobject_class = VIPS_OBJECT_CLASS(class);

	gobject_class->dispose = vips_text_dispose;
	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	vobject_class->nickname = "text";
	vobject_class->description = _("make a text image");
	vobject_class->build = vips_text_build;

	VIPS_ARG_STRING(class, "text", 4,
		_("Text"),
		_("Text to render"),
		VIPS_ARGUMENT_REQUIRED_INPUT,
		G_STRUCT_OFFSET(VipsText, text),
		NULL);

	VIPS_ARG_STRING(class, "font", 5,
		_("Font"),
		_("Font to render with"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsText, font),
		NULL);

	VIPS_ARG_INT(class, "width", 6,
		_("Width"),
		_("Maximum image width in pixels"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsText, width),
		0, VIPS_MAX_COORD, 0);

	VIPS_ARG_INT(class, "height", 7,
		_("Height"),
		_("Maximum image height in pixels"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsText, height),
		0, VIPS_MAX_COORD, 0);

	VIPS_ARG_ENUM(class, "align", 8,
		_("Align"),
		_("Align on the low, centre or high edge"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsText, align),
		VIPS_TYPE_ALIGN, VIPS_ALIGN_LOW);

	VIPS_ARG_BOOL(class, "justify", 9,
		_("Justify"),
		_("Justify lines"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsText, justify),
		FALSE);

	VIPS_ARG_INT(class, "dpi", 10,
		_("DPI"),
		_("DPI to render at"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsText, dpi),
		1, 1000000, 72);

	VIPS_ARG_INT(class, "autofit_dpi", 11,
		_("Autofit DPI"),
		_("DPI selected by autofit"),
		VIPS_ARGUMENT_OPTIONAL_OUTPUT,
		G_STRUCT_OFFSET(VipsText, dpi),
		1, 1000000, 72);

	VIPS_ARG_INT(class, "spacing", 12,
		_("Spacing"),
		_("Line spacing"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsText, spacing),
		-1000000, 1000000, 0);

	VIPS_ARG_STRING(class, "fontfile", 13,
		_("Font file"),
		_("Load this font file"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsText, fontfile),
		NULL);

	VIPS_ARG_BOOL(class, "rgba", 14,
		_("RGBA"),
		_("Enable RGBA output"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsText, rgba),
		FALSE);

	VIPS_ARG_ENUM(class, "wrap", 15,
		_("Wrap"),
		_("Wrap lines on word or character boundaries"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsText, wrap),
		VIPS_TYPE_TEXT_WRAP, VIPS_TEXT_WRAP_WORD);
}

static void
vips_text_init(VipsText *text)
{
	text->align = VIPS_ALIGN_LOW;
	text->dpi = 72;
	text->wrap = VIPS_TEXT_WRAP_WORD;
	VIPS_SETSTR(text->font, "sans 12");
}

#endif /*HAVE_PANGOCAIRO*/

/**
 * vips_text:
 * @out: (out): output image
 * @text: utf-8 text string to render
 * @...: `NULL`-terminated list of optional named arguments
 *
 * Draw the string @text to an image.
 *
 * @out is normally a one-band 8-bit
 * unsigned char image, with 0 for no text and 255 for text. Values between
 * are used for anti-aliasing.
 *
 * Set @rgba to enable RGBA output. This is useful for colour emoji rendering,
 * or support for pango markup features like `<span
 * foreground="red">Red!</span>`.
 *
 * @text is the text to render as a UTF-8 string. It can contain Pango markup,
 * for example `<i>The</i>Guardian`.
 *
 * @font is the font to render with, as a fontconfig name. Examples might be
 * `sans 12` or perhaps `bitstream charter bold 10`.
 *
 * You can specify a font to load with @fontfile. You'll need to also set the
 * name of the font with @font.
 *
 * @width is the number of pixels to word-wrap at. By default, lines of text
 * wider than this will be broken at word boundaries.
 * Use @wrap to set lines to wrap on word or character boundaries, or to
 * disable line breaks.
 *
 * Set @justify to turn on line justification.
 * @align can be used to set the alignment style for multi-line
 * text to the low (left) edge centre, or high (right) edge. Note that the
 * output image can be wider than @width if there are no
 * word breaks, or narrower if the lines don't break exactly at @width.
 *
 * @height is the maximum number of pixels high the generated text can be. This
 * only takes effect when @dpi is not set, and @width is set, making a box.
 * In this case, [ctor@Image.text] will search for a @dpi and set of line breaks
 * which will just fit the text into @width and @height.
 *
 * You can use @autofit_dpi to read out the DPI selected by auto fit.
 *
 * @dpi sets the resolution to render at. "sans 12" at 72 dpi draws characters
 * approximately 12 pixels high.
 *
 * @spacing sets the line spacing, in points. It would typically be something
 * like font size times 1.2.
 *
 * You can read the coordinate of the top edge of the character from `Xoffset`
 * / `Yoffset`. This can be helpful if you need to line up the output of
 * several [ctor@Image.text].
 *
 * ::: tip "Optional arguments"
 *     * @font: `gchararray`, font to render with
 *     * @fontfile: `gchararray`, load this font file
 *     * @width: `gint`, image should be no wider than this many pixels
 *     * @height: `gint`, image should be no higher than this many pixels
 *     * @align: [enum@Align], set justification alignment
 *     * @justify: `gboolean`, justify lines
 *     * @dpi: `gint`, render at this resolution
 *     * @autofit_dpi: `gint`, output, auto-fitted DPI
 *     * @rgba: `gboolean`, enable RGBA output
 *     * @spacing: `gint`, space lines by this in points
 *     * @wrap: [enum@TextWrap], wrap lines on characters or words
 *
 * ::: seealso
 *     [func@Image.bandjoin], [func@Image.composite].
 *
 * Returns: 0 on success, -1 on error
 */
int
vips_text(VipsImage **out, const char *text, ...)
{
	va_list ap;
	int result;

	va_start(ap, text);
	result = vips_call_split("text", ap, out, text);
	va_end(ap);

	return result;
}
