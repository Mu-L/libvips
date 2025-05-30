/* VipsEmbed
 *
 * Author: J. Cupitt
 * Written on: 21/2/95
 * Modified on:
 * 6/4/04
 *	- added extend pixels from edge mode
 *	- sets Xoffset / Yoffset to x / y
 * 15/4/04
 *	- added replicate and mirror modes
 * 4/3/05
 *	- added solid white mode
 * 4/1/07
 * 	- degenerate to im_copy() for 0/0/w/h
 * 1/8/07
 * 	- more general ... x and y can be negative
 * 24/3/09
 * 	- added IM_CODING_RAD support
 * 5/11/09
 * 	- gtkdoc
 * 27/1/10
 * 	- use im_region_paint()
 * 	- cleanups
 * 15/10/11
 * 	- rewrite as a class
 * 10/10/12
 *	- add @background
 * 19/9/17
 * 	- break into embed and gravity
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
#define VIPS_DEBUG
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/
#include <glib/gi18n-lib.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include <vips/vips.h>
#include <vips/internal.h>
#include <vips/debug.h>

#include "pconversion.h"

typedef struct _VipsEmbedBase {
	VipsConversion parent_instance;

	/* The input image.
	 */
	VipsImage *in;

	VipsExtend extend;
	VipsArrayDouble *background;
	int width;
	int height;

	/* Pixel we paint calculated from background.
	 */
	VipsPel *ink;

	/* Geometry calculations.
	 */
	VipsRect rout; /* Whole output area */
	VipsRect rsub; /* Rect occupied by image */

	/* The 8 border pieces. The 4 borders strictly up/down/left/right of
	 * the main image, and the 4 corner pieces.
	 */
	VipsRect border[8];

	/* Passed to us by subclasses.
	 */
	int x;
	int y;
} VipsEmbedBase;

typedef VipsConversionClass VipsEmbedBaseClass;

G_DEFINE_ABSTRACT_TYPE(VipsEmbedBase, vips_embed_base, VIPS_TYPE_CONVERSION);

/* r is the bit we are trying to paint, guaranteed to be entirely within
 * border area i. Set out to be the edge of the image we need to paint the
 * pixels in r.
 */
static void
vips_embed_base_find_edge(VipsEmbedBase *base,
	VipsRect *r, int i, VipsRect *out)
{
	/* Expand the border by 1 pixel, intersect with the image area, and we
	 * get the edge. Usually too much though: eg. we could make the entire
	 * right edge.
	 */
	*out = base->border[i];
	vips_rect_marginadjust(out, 1);
	vips_rect_intersectrect(out, &base->rsub, out);

	/* Usually too much though: eg. we could make the entire
	 * right edge. If we're strictly up/down/left/right of the image, we
	 * can trim.
	 */
	if (i == 0 ||
		i == 2) {
		VipsRect extend;

		/* Above or below.
		 */
		extend = *r;
		extend.top = 0;
		extend.height = base->height;
		vips_rect_intersectrect(out, &extend, out);
	}
	if (i == 1 ||
		i == 3) {
		VipsRect extend;

		/* Left or right.
		 */
		extend = *r;
		extend.left = 0;
		extend.width = base->width;
		vips_rect_intersectrect(out, &extend, out);
	}
}

/* Copy a single pixel sideways into a line of pixels.
 */
static void
vips_embed_base_copy_pixel(VipsEmbedBase *base,
	VipsPel *q, VipsPel *p, int n)
{
	const int bs = VIPS_IMAGE_SIZEOF_PEL(base->in);

	int x, b;

	for (x = 0; x < n; x++)
		for (b = 0; b < bs; b++)
			*q++ = p[b];
}

/* Paint r of region or. It's a border area, lying entirely within
 * base->border[i]. p points to the top-left source pixel to fill with.
 * plsk is the line stride.
 */
static void
vips_embed_base_paint_edge(VipsEmbedBase *base,
	VipsRegion *out_region, int i, VipsRect *r, VipsPel *p, int plsk)
{
	const int bs = VIPS_IMAGE_SIZEOF_PEL(base->in);

	VipsRect todo;
	VipsPel *q;
	int y;

	VIPS_GATE_START("vips_embed_base_paint_edge: work");

	/* Pixels left to paint.
	 */
	todo = *r;

	/* Corner pieces ... copy the single pixel to paint the top line of
	 * todo, then use the line copier below to paint the rest of it.
	 */
	if (i > 3) {
		q = VIPS_REGION_ADDR(out_region, todo.left, todo.top);
		vips_embed_base_copy_pixel(base, q, p, todo.width);

		p = q;
		todo.top += 1;
		todo.height -= 1;
	}

	if (i == 1 || i == 3) {
		/* Vertical line of pixels to copy.
		 */
		for (y = 0; y < todo.height; y++) {
			q = VIPS_REGION_ADDR(out_region, todo.left, todo.top + y);
			vips_embed_base_copy_pixel(base, q, p, todo.width);
			p += plsk;
		}
	}
	else {
		/* Horizontal line of pixels to copy.
		 */
		for (y = 0; y < todo.height; y++) {
			q = VIPS_REGION_ADDR(out_region, todo.left, todo.top + y);
			memcpy(q, p, (size_t) bs * todo.width);
		}
	}

	VIPS_GATE_STOP("vips_embed_base_paint_edge: work");
}

static int
vips_embed_base_gen(VipsRegion *out_region,
	void *seq, void *a, void *b, gboolean *stop)
{
	VipsRegion *ir = (VipsRegion *) seq;
	VipsEmbedBase *base = (VipsEmbedBase *) b;
	VipsRect *r = &out_region->valid;

	VipsRect ovl;
	int i;
	VipsPel *p;
	int plsk;
	int ink;

	/* Entirely within the input image? Generate the subimage and copy
	 * pointers.
	 */
	if (vips_rect_includesrect(&base->rsub, r)) {
		VipsRect need;

		need = *r;
		need.left -= base->x;
		need.top -= base->y;
		if (vips_region_prepare(ir, &need) ||
			vips_region_region(out_region, ir, r, need.left, need.top))
			return -1;

		return 0;
	}

	/* Does any of the input image appear in the area we have been asked
	 * to make? Paste it in.
	 */
	vips_rect_intersectrect(r, &base->rsub, &ovl);
	if (!vips_rect_isempty(&ovl)) {
		/* Paint the bits coming from the input image.
		 */
		ovl.left -= base->x;
		ovl.top -= base->y;
		if (vips_region_prepare_to(ir, out_region, &ovl,
				ovl.left + base->x, ovl.top + base->y))
			return -1;
		ovl.left += base->x;
		ovl.top += base->y;
	}

	switch (base->extend) {
	case VIPS_EXTEND_BLACK:
	case VIPS_EXTEND_WHITE:
		VIPS_GATE_START("vips_embed_base_gen: work1");

		ink = base->extend == VIPS_EXTEND_BLACK
			? 0
			: (int) vips_interpretation_max_alpha(base->in->Type);

		/* Paint the borders a solid value.
		 */
		for (i = 0; i < 8; i++)
			vips_region_paint(out_region, &base->border[i], ink);

		VIPS_GATE_STOP("vips_embed_base_gen: work1");

		break;

	case VIPS_EXTEND_BACKGROUND:
		VIPS_GATE_START("vips_embed_base_gen: work2");

		/* Paint the borders a solid value.
		 */
		for (i = 0; i < 8; i++)
			vips_region_paint_pel(out_region, &base->border[i], base->ink);

		VIPS_GATE_STOP("vips_embed_base_gen: work2");

		break;

	case VIPS_EXTEND_COPY:
		/* Extend the borders.
		 */
		for (i = 0; i < 8; i++) {
			VipsRect todo;
			VipsRect edge;

			vips_rect_intersectrect(r, &base->border[i], &todo);
			if (!vips_rect_isempty(&todo)) {
				vips_embed_base_find_edge(base,
					&todo, i, &edge);

				/* Did we paint any of the input image? If we
				 * did, we can fetch the edge pixels from
				 * that.
				 */
				if (!vips_rect_isempty(&ovl)) {
					p = VIPS_REGION_ADDR(out_region,
						edge.left, edge.top);
					plsk = VIPS_REGION_LSKIP(out_region);
				}
				else {
					/* No pixels painted ... fetch
					 * directly from the input image.
					 */
					edge.left -= base->x;
					edge.top -= base->y;
					if (vips_region_prepare(ir, &edge))
						return -1;
					p = VIPS_REGION_ADDR(ir,
						edge.left, edge.top);
					plsk = VIPS_REGION_LSKIP(ir);
				}

				vips_embed_base_paint_edge(base,
					out_region, i, &todo, p, plsk);
			}
		}

		break;

	default:
		g_assert_not_reached();
	}

	return 0;
}

static int
vips_embed_base_build(VipsObject *object)
{
	VipsObjectClass *class = VIPS_OBJECT_GET_CLASS(object);
	VipsConversion *conversion = VIPS_CONVERSION(object);
	VipsEmbedBase *base = (VipsEmbedBase *) object;
	VipsImage **t = (VipsImage **) vips_object_local_array(object, 7);

	VipsRect want;

	if (VIPS_OBJECT_CLASS(vips_embed_base_parent_class)->build(object))
		return -1;

	/* nip2 can generate this quite often ... just copy.
	 */
	if (base->x == 0 &&
		base->y == 0 &&
		base->width == base->in->Xsize &&
		base->height == base->in->Ysize)
		return vips_image_write(base->in, conversion->out);

	if (!vips_object_argument_isset(object, "extend") &&
		vips_object_argument_isset(object, "background"))
		base->extend = VIPS_EXTEND_BACKGROUND; // FIXME: Invalidates operation cache

	if (base->extend == VIPS_EXTEND_BACKGROUND)
		if (!(base->ink = vips__vector_to_ink(
				  class->nickname, base->in,
				  VIPS_AREA(base->background)->data, NULL,
				  VIPS_AREA(base->background)->n)))
			return -1;

	switch (base->extend) {
	case VIPS_EXTEND_REPEAT: {
		/* Clock arithmetic: we want negative x/y to wrap around
		 * nicely.
		 */
		const int nx = base->x < 0
			? -base->x % base->in->Xsize
			: base->in->Xsize - base->x % base->in->Xsize;
		const int ny = base->y < 0
			? -base->y % base->in->Ysize
			: base->in->Ysize - base->y % base->in->Ysize;

		if (vips_replicate(base->in, &t[0],
				base->width / base->in->Xsize + 2,
				base->height / base->in->Ysize + 2, NULL) ||
			vips_extract_area(t[0], &t[1],
				nx, ny, base->width, base->height, NULL) ||
			vips_image_write(t[1], conversion->out))
			return -1;
	} break;

	case VIPS_EXTEND_MIRROR: {
		/* As repeat, but the tiles are twice the size because of
		 * mirroring.
		 */
		const int w2 = base->in->Xsize * 2;
		const int h2 = base->in->Ysize * 2;

		const int nx = base->x < 0 ? -base->x % w2 : w2 - base->x % w2;
		const int ny = base->y < 0 ? -base->y % h2 : h2 - base->y % h2;

		if (
			/* Make a 2x2 mirror tile.
			 */
			vips_flip(base->in, &t[0],
				VIPS_DIRECTION_HORIZONTAL, NULL) ||
			vips_join(base->in, t[0], &t[1],
				VIPS_DIRECTION_HORIZONTAL, NULL) ||
			vips_flip(t[1], &t[2],
				VIPS_DIRECTION_VERTICAL, NULL) ||
			vips_join(t[1], t[2], &t[3],
				VIPS_DIRECTION_VERTICAL, NULL) ||

			/* Repeat, then cut out the centre.
			 */
			vips_replicate(t[3], &t[4],
				base->width / t[3]->Xsize + 2,
				base->height / t[3]->Ysize + 2, NULL) ||
			vips_extract_area(t[4], &t[5],
				nx, ny, base->width, base->height, NULL) ||

			/* Overwrite the centre with the in, much faster
			 * for centre pixels.
			 */
			vips_insert(t[5], base->in, &t[6],
				base->x, base->y, NULL) ||

			vips_image_write(t[6], conversion->out))
			return -1;
	} break;

	case VIPS_EXTEND_BLACK:
	case VIPS_EXTEND_WHITE:
	case VIPS_EXTEND_BACKGROUND:
	case VIPS_EXTEND_COPY:
		/* embed is used in many places. We don't really care about
		 * geometry, so use ANY to avoid disturbing all pipelines.
		 */
		if (vips_image_pipelinev(conversion->out,
				VIPS_DEMAND_STYLE_ANY, base->in, NULL))
			return -1;

		conversion->out->Xsize = base->width;
		conversion->out->Ysize = base->height;

		/* Whole output area.
		 */
		base->rout.left = 0;
		base->rout.top = 0;
		base->rout.width = conversion->out->Xsize;
		base->rout.height = conversion->out->Ysize;

		/* Rect occupied by image (can be clipped to nothing).
		 */
		want.left = base->x;
		want.top = base->y;
		want.width = base->in->Xsize;
		want.height = base->in->Ysize;
		vips_rect_intersectrect(&want, &base->rout, &base->rsub);

		/* FIXME ... actually, it can't. base_find_edge() will fail
		 * if rsub is empty. Make this more general at some point
		 * and remove this test.
		 */
		if (vips_rect_isempty(&base->rsub)) {
			vips_error(class->nickname,
				"%s", _("bad dimensions"));
			return -1;
		}

		/* Edge rects of new pixels ... top, right, bottom, left. Order
		 * important. Can be empty.
		 */
		base->border[0].left = base->rsub.left;
		base->border[0].top = 0;
		base->border[0].width = base->rsub.width;
		base->border[0].height = base->rsub.top;

		base->border[1].left = VIPS_RECT_RIGHT(&base->rsub);
		base->border[1].top = base->rsub.top;
		base->border[1].width = conversion->out->Xsize -
			VIPS_RECT_RIGHT(&base->rsub);
		base->border[1].height = base->rsub.height;

		base->border[2].left = base->rsub.left;
		base->border[2].top = VIPS_RECT_BOTTOM(&base->rsub);
		base->border[2].width = base->rsub.width;
		base->border[2].height = conversion->out->Ysize -
			VIPS_RECT_BOTTOM(&base->rsub);

		base->border[3].left = 0;
		base->border[3].top = base->rsub.top;
		base->border[3].width = base->rsub.left;
		base->border[3].height = base->rsub.height;

		/* Corner rects. Top-left, top-right, bottom-right,
		 * bottom-left. Order important.
		 */
		base->border[4].left = 0;
		base->border[4].top = 0;
		base->border[4].width = base->rsub.left;
		base->border[4].height = base->rsub.top;

		base->border[5].left = VIPS_RECT_RIGHT(&base->rsub);
		base->border[5].top = 0;
		base->border[5].width = conversion->out->Xsize -
			VIPS_RECT_RIGHT(&base->rsub);
		base->border[5].height = base->rsub.top;

		base->border[6].left = VIPS_RECT_RIGHT(&base->rsub);
		base->border[6].top = VIPS_RECT_BOTTOM(&base->rsub);
		base->border[6].width = conversion->out->Xsize -
			VIPS_RECT_RIGHT(&base->rsub);
		base->border[6].height = conversion->out->Ysize -
			VIPS_RECT_BOTTOM(&base->rsub);

		base->border[7].left = 0;
		base->border[7].top = VIPS_RECT_BOTTOM(&base->rsub);
		base->border[7].width = base->rsub.left;
		base->border[7].height = conversion->out->Ysize -
			VIPS_RECT_BOTTOM(&base->rsub);

		if (vips_image_generate(conversion->out,
				vips_start_one, vips_embed_base_gen, vips_stop_one,
				base->in, base))
			return -1;

		break;

	default:
		g_assert_not_reached();
	}

	return 0;
}

static void
vips_embed_base_class_init(VipsEmbedBaseClass *class)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(class);
	VipsObjectClass *vobject_class = VIPS_OBJECT_CLASS(class);

	VIPS_DEBUG_MSG("vips_embed_base_class_init\n");

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	vobject_class->nickname = "embed_base";
	vobject_class->description = _("embed an image in a larger image");
	vobject_class->build = vips_embed_base_build;

	/* Not seq with mirror.
	 */

	VIPS_ARG_IMAGE(class, "in", 1,
		_("Input"),
		_("Input image"),
		VIPS_ARGUMENT_REQUIRED_INPUT,
		G_STRUCT_OFFSET(VipsEmbedBase, in));

	VIPS_ARG_INT(class, "width", 5,
		_("Width"),
		_("Image width in pixels"),
		VIPS_ARGUMENT_REQUIRED_INPUT,
		G_STRUCT_OFFSET(VipsEmbedBase, width),
		1, 1000000000, 1);

	VIPS_ARG_INT(class, "height", 6,
		_("Height"),
		_("Image height in pixels"),
		VIPS_ARGUMENT_REQUIRED_INPUT,
		G_STRUCT_OFFSET(VipsEmbedBase, height),
		1, 1000000000, 1);

	VIPS_ARG_ENUM(class, "extend", 7,
		_("Extend"),
		_("How to generate the extra pixels"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsEmbedBase, extend),
		VIPS_TYPE_EXTEND, VIPS_EXTEND_BLACK);

	VIPS_ARG_BOXED(class, "background", 12,
		_("Background"),
		_("Color for background pixels"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsEmbedBase, background),
		VIPS_TYPE_ARRAY_DOUBLE);
}

static void
vips_embed_base_init(VipsEmbedBase *base)
{
	base->extend = VIPS_EXTEND_BLACK;
	base->background = vips_array_double_newv(1, 0.0);
}

/* Embed with specified x, y
 */

typedef struct _VipsEmbed {
	VipsEmbedBase parent_instance;

	int x;
	int y;
} VipsEmbed;

typedef VipsConversionClass VipsEmbedClass;

G_DEFINE_TYPE(VipsEmbed, vips_embed, vips_embed_base_get_type());

static int
vips_embed_build(VipsObject *object)
{
	VipsEmbedBase *base = (VipsEmbedBase *) object;
	VipsEmbed *embed = (VipsEmbed *) object;

	/* Just pass the specified x, y down.
	 */
	base->x = embed->x;
	base->y = embed->y;

	if (VIPS_OBJECT_CLASS(vips_embed_parent_class)->build(object))
		return -1;

	return 0;
}

static void
vips_embed_class_init(VipsEmbedClass *class)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(class);
	VipsObjectClass *vobject_class = VIPS_OBJECT_CLASS(class);

	VIPS_DEBUG_MSG("vips_embed_class_init\n");

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	vobject_class->nickname = "embed";
	vobject_class->description = _("embed an image in a larger image");
	vobject_class->build = vips_embed_build;

	VIPS_ARG_INT(class, "x", 3,
		_("x"),
		_("Left edge of input in output"),
		VIPS_ARGUMENT_REQUIRED_INPUT,
		G_STRUCT_OFFSET(VipsEmbed, x),
		-1000000000, 1000000000, 0);

	VIPS_ARG_INT(class, "y", 4,
		_("y"),
		_("Top edge of input in output"),
		VIPS_ARGUMENT_REQUIRED_INPUT,
		G_STRUCT_OFFSET(VipsEmbed, y),
		-1000000000, 1000000000, 0);
}

static void
vips_embed_init(VipsEmbed *embed)
{
}

/**
 * vips_embed: (method)
 * @in: input image
 * @out: (out): output image
 * @x: place @in at this x position in @out
 * @y: place @in at this y position in @out
 * @width: @out should be this many pixels across
 * @height: @out should be this many pixels down
 * @...: %NULL-terminated list of optional named arguments
 *
 * The opposite of [method@Image.extract_area]: embed @in within an image of
 * size @width by @height at position @x, @y.
 *
 * @extend controls what appears in the new pels, see [enum@Extend].
 *
 * ::: tip "Optional arguments"
 *     * @extend: [enum@Extend] to generate the edge pixels (default: black)
 *     * @background: [struct@ArrayDouble] colour for edge pixels
 *
 * ::: seealso
 *     [method@Image.extract_area], [method@Image.insert].
 *
 * Returns: 0 on success, -1 on error.
 */
int
vips_embed(VipsImage *in, VipsImage **out,
	int x, int y, int width, int height, ...)
{
	va_list ap;
	int result;

	va_start(ap, height);
	result = vips_call_split("embed", ap, in, out, x, y, width, height);
	va_end(ap);

	return result;
}

/* Embed with a general direction.
 */

typedef struct _VipsGravity {
	VipsEmbedBase parent_instance;

	VipsCompassDirection direction;
} VipsGravity;

typedef VipsConversionClass VipsGravityClass;

G_DEFINE_TYPE(VipsGravity, vips_gravity, vips_embed_base_get_type());

static int
vips_gravity_build(VipsObject *object)
{
	VipsEmbedBase *base = (VipsEmbedBase *) object;
	VipsGravity *gravity = (VipsGravity *) object;

	if (vips_object_argument_isset(object, "in") &&
		vips_object_argument_isset(object, "width") &&
		vips_object_argument_isset(object, "height") &&
		vips_object_argument_isset(object, "direction")) {
		switch (gravity->direction) {
		case VIPS_COMPASS_DIRECTION_CENTRE:
			base->x = (base->width - base->in->Xsize) / 2;
			base->y = (base->height - base->in->Ysize) / 2;
			break;

		case VIPS_COMPASS_DIRECTION_NORTH:
			base->x = (base->width - base->in->Xsize) / 2;
			base->y = 0;
			break;

		case VIPS_COMPASS_DIRECTION_EAST:
			base->x = base->width - base->in->Xsize;
			base->y = (base->height - base->in->Ysize) / 2;
			break;

		case VIPS_COMPASS_DIRECTION_SOUTH:
			base->x = (base->width - base->in->Xsize) / 2;
			base->y = base->height - base->in->Ysize;
			break;

		case VIPS_COMPASS_DIRECTION_WEST:
			base->x = 0;
			base->y = (base->height - base->in->Ysize) / 2;
			break;

		case VIPS_COMPASS_DIRECTION_NORTH_EAST:
			base->x = base->width - base->in->Xsize;
			base->y = 0;
			break;

		case VIPS_COMPASS_DIRECTION_SOUTH_EAST:
			base->x = base->width - base->in->Xsize;
			base->y = base->height - base->in->Ysize;
			break;

		case VIPS_COMPASS_DIRECTION_SOUTH_WEST:
			base->x = 0;
			base->y = base->height - base->in->Ysize;
			break;

		case VIPS_COMPASS_DIRECTION_NORTH_WEST:
			base->x = 0;
			base->y = 0;
			break;

		default:
			g_assert_not_reached();
		}
	}

	if (VIPS_OBJECT_CLASS(vips_gravity_parent_class)->build(object))
		return -1;

	return 0;
}

static void
vips_gravity_class_init(VipsGravityClass *class)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(class);
	VipsObjectClass *vobject_class = VIPS_OBJECT_CLASS(class);

	VIPS_DEBUG_MSG("vips_gravity_class_init\n");

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	vobject_class->nickname = "gravity";
	vobject_class->description = _("place an image within a larger "
								   "image with a certain gravity");
	vobject_class->build = vips_gravity_build;

	VIPS_ARG_ENUM(class, "direction", 3,
		_("Direction"),
		_("Direction to place image within width/height"),
		VIPS_ARGUMENT_REQUIRED_INPUT,
		G_STRUCT_OFFSET(VipsGravity, direction),
		VIPS_TYPE_COMPASS_DIRECTION, VIPS_COMPASS_DIRECTION_CENTRE);
}

static void
vips_gravity_init(VipsGravity *gravity)
{
	gravity->direction = VIPS_COMPASS_DIRECTION_CENTRE;
}

/**
 * vips_gravity: (method)
 * @in: input image
 * @out: output image
 * @direction: place @in at this direction in @out
 * @width: @out should be this many pixels across
 * @height: @out should be this many pixels down
 * @...: %NULL-terminated list of optional named arguments
 *
 * The opposite of [method@Image.extract_area]: place @in within an image of
 * size @width by @height at a certain gravity.
 *
 * @extend controls what appears in the new pels, see #VipsExtend.
 *
 * ::: tip "Optional arguments"
 *     * @extend: #VipsExtend to generate the edge pixels (default: black)
 *     * @background: [struct@ArrayDouble] colour for edge pixels
 *
 * ::: seealso
 *     [method@Image.extract_area], [method@Image.insert].
 *
 * Returns: 0 on success, -1 on error.
 */
int
vips_gravity(VipsImage *in, VipsImage **out,
	VipsCompassDirection direction, int width, int height, ...)
{
	va_list ap;
	int result;

	va_start(ap, height);
	result = vips_call_split("gravity", ap, in, out,
		direction, width, height);
	va_end(ap);

	return result;
}
