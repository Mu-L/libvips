/* horizontal reduce by a float factor with a kernel
 *
 * 29/1/16
 * 	- from shrinkh.c
 * 10/3/16
 * 	- add other kernels
 * 15/8/16
 * 	- rename xshrink as hshrink for consistency
 * 9/9/16
 * 	- add @centre option
 * 6/6/20 kleisauke
 * 	- deprecate @centre option, it's now always on
 * 	- fix pixel shift
 * 22/4/22 kleisauke
 * 	- add @gap option
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
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#include <vips/vips.h>
#include <vips/vector.h>
#include <vips/debug.h>
#include <vips/internal.h>

#include "presample.h"
#include "templates.h"

typedef struct _VipsReduceh {
	VipsResample parent_instance;

	double hshrink; /* Reduce factor */
	double gap;		/* Reduce gap */

	/* The thing we use to make the kernel.
	 */
	VipsKernel kernel;

	/* Number of points in kernel.
	 */
	int n_point;

	/* Horizontal displacement.
	 */
	double hoffset;

	/* The hshrink we do after integer reduction.
	 */
	double residual_hshrink;

	/* Precalculated interpolation matrices. short (used for pel
	 * sizes up to int), and double (for all others). We go to
	 * scale + 1 so we can round-to-nearest safely.
	 */
	short *matrixs[VIPS_TRANSFORM_SCALE + 1];
	double *matrixf[VIPS_TRANSFORM_SCALE + 1];

	/* Deprecated.
	 */
	gboolean centre;

} VipsReduceh;

typedef VipsResampleClass VipsReducehClass;

/* We need C linkage for this.
 */
extern "C" {
G_DEFINE_TYPE(VipsReduceh, vips_reduceh, VIPS_TYPE_RESAMPLE);
}

/* Get n points. @shrink is the shrink factor, so 2 for a 50% reduction.
 */
int
vips_reduce_get_points(VipsKernel kernel, double shrink)
{
	switch (kernel) {
	case VIPS_KERNEL_NEAREST:
		return 1;

	case VIPS_KERNEL_LINEAR:
		return 2 * rint(shrink) + 1;

	case VIPS_KERNEL_CUBIC:
	case VIPS_KERNEL_MITCHELL:
		return 2 * rint(2 * shrink) + 1;

	case VIPS_KERNEL_LANCZOS2:
		return 2 * rint(2 * shrink) + 1;

	case VIPS_KERNEL_LANCZOS3:
		return 2 * rint(3 * shrink) + 1;

	case VIPS_KERNEL_MKS2013:
		return 2 * rint(3 * shrink) + 1;

	case VIPS_KERNEL_MKS2021:
		return 2 * rint(5 * shrink) + 1;

	default:
		g_assert_not_reached();
		return 0;
	}
}

template <typename T, T max_value>
static void inline reduceh_unsigned_int_tab(VipsReduceh *reduceh,
	VipsPel *pout, const VipsPel *pin,
	const int bands, const short *restrict cx)
{
	T *restrict out = (T *) pout;
	const T *restrict in = (T *) pin;
	const int n = reduceh->n_point;

	for (int z = 0; z < bands; z++) {
		typename LongT<T>::type sum;

		sum = reduce_sum<T>(in + z, bands, cx, n);
		sum = unsigned_fixed_round(sum);
		out[z] = VIPS_CLIP(0, sum, max_value);
	}
}

template <typename T, int min_value, int max_value>
static void inline reduceh_signed_int_tab(VipsReduceh *reduceh,
	VipsPel *pout, const VipsPel *pin,
	const int bands, const short *restrict cx)
{
	T *restrict out = (T *) pout;
	const T *restrict in = (T *) pin;
	const int n = reduceh->n_point;

	for (int z = 0; z < bands; z++) {
		typename LongT<T>::type sum;

		sum = reduce_sum<T>(in + z, bands, cx, n);
		sum = signed_fixed_round(sum);
		out[z] = VIPS_CLIP(min_value, sum, max_value);
	}
}

/* Floating-point version.
 */
template <typename T>
static void inline reduceh_float_tab(VipsReduceh *reduceh,
	VipsPel *pout, const VipsPel *pin,
	const int bands, const double *restrict cx)
{
	T *restrict out = (T *) pout;
	const T *restrict in = (T *) pin;
	const int n = reduceh->n_point;

	for (int z = 0; z < bands; z++)
		out[z] = reduce_sum<T>(in + z, bands, cx, n);
}

/* Ultra-high-quality version for double images.
 */
template <typename T>
static void inline reduceh_notab(VipsReduceh *reduceh,
	VipsPel *pout, const VipsPel *pin,
	const int bands, double x)
{
	T *restrict out = (T *) pout;
	const T *restrict in = (T *) pin;
	const int n = reduceh->n_point;

	typename LongT<T>::type cx[MAX_POINT];

	vips_reduce_make_mask(cx, reduceh->kernel, reduceh->n_point,
		reduceh->residual_hshrink, x);

	for (int z = 0; z < bands; z++)
		out[z] = reduce_sum<T>(in + z, bands, cx, n);
}

static int
vips_reduceh_gen(VipsRegion *out_region, void *seq,
	void *a, void *b, gboolean *stop)
{
	VipsImage *in = (VipsImage *) a;
	VipsReduceh *reduceh = (VipsReduceh *) b;
	const int ps = VIPS_IMAGE_SIZEOF_PEL(in);
	VipsRegion *ir = (VipsRegion *) seq;
	VipsRect *r = &out_region->valid;

	/* Double bands for complex.
	 */
	const int bands = in->Bands *
		(vips_band_format_iscomplex(in->BandFmt) ? 2 : 1);

	VipsRect s;

#ifdef DEBUG
	printf("vips_reduceh_gen: generating %d x %d at %d x %d\n",
		r->width, r->height, r->left, r->top);
#endif /*DEBUG*/

	s.left = r->left * reduceh->residual_hshrink - reduceh->hoffset;
	s.top = r->top;
	s.width = r->width * reduceh->residual_hshrink + reduceh->n_point;
	s.height = r->height;
	if (vips_region_prepare(ir, &s))
		return -1;

	VIPS_GATE_START("vips_reduceh_gen: work");

	for (int y = 0; y < r->height; y++) {
		VipsPel *p0;
		VipsPel *q;

		double X;

		q = VIPS_REGION_ADDR(out_region, r->left, r->top + y);

		X = (r->left + 0.5) * reduceh->residual_hshrink - 0.5 -
			reduceh->hoffset;

		/* We want p0 to be the start (ie. x == 0) of the input
		 * scanline we are reading from. We can then calculate the p we
		 * need for each pixel with a single mul and avoid calling ADDR
		 * for each pixel.
		 *
		 * We can't get p0 directly with ADDR since it could be outside
		 * valid, so get the leftmost pixel in valid and subtract a
		 * bit.
		 */
		p0 = VIPS_REGION_ADDR(ir, ir->valid.left, r->top + y) -
			ir->valid.left * ps;

		for (int x = 0; x < r->width; x++) {
			const int ix = (int) X;
			VipsPel *p = p0 + ix * ps;
			const int sx = X * VIPS_TRANSFORM_SCALE * 2;
			const int six = sx & (VIPS_TRANSFORM_SCALE * 2 - 1);
			const int tx = (six + 1) >> 1;
			const short *cxs = reduceh->matrixs[tx];
			const double *cxf = reduceh->matrixf[tx];

			switch (in->BandFmt) {
			case VIPS_FORMAT_UCHAR:
				reduceh_unsigned_int_tab<unsigned char,
					UCHAR_MAX>(reduceh, q, p, bands, cxs);
				break;

			case VIPS_FORMAT_CHAR:
				reduceh_signed_int_tab<signed char,
					SCHAR_MIN, SCHAR_MAX>(reduceh, q, p, bands, cxs);
				break;

			case VIPS_FORMAT_USHORT:
				reduceh_unsigned_int_tab<unsigned short,
					USHRT_MAX>(reduceh, q, p, bands, cxs);
				break;

			case VIPS_FORMAT_SHORT:
				reduceh_signed_int_tab<signed short,
					SHRT_MIN, SHRT_MAX>(reduceh, q, p, bands, cxs);
				break;

			case VIPS_FORMAT_UINT:
				reduceh_unsigned_int_tab<unsigned int,
					UINT_MAX>(reduceh, q, p, bands, cxs);
				break;

			case VIPS_FORMAT_INT:
				reduceh_signed_int_tab<signed int,
					INT_MIN, INT_MAX>(reduceh, q, p, bands, cxs);
				break;

			case VIPS_FORMAT_FLOAT:
			case VIPS_FORMAT_COMPLEX:
				reduceh_float_tab<float>(reduceh,
					q, p, bands, cxf);
				break;

			case VIPS_FORMAT_DOUBLE:
			case VIPS_FORMAT_DPCOMPLEX:
				reduceh_notab<double>(reduceh,
					q, p, bands, X - ix);
				break;

			default:
				g_assert_not_reached();
				break;
			}

			X += reduceh->residual_hshrink;
			q += ps;
		}
	}

	VIPS_GATE_STOP("vips_reduceh_gen: work");

	VIPS_COUNT_PIXELS(out_region, "vips_reduceh_gen");

	return 0;
}

#ifdef HAVE_HWY
static int
vips_reduceh_uchar_vector_gen(VipsRegion *out_region, void *seq,
	void *a, void *b, gboolean *stop)
{
	VipsImage *in = (VipsImage *) a;
	VipsReduceh *reduceh = (VipsReduceh *) b;
	const int ps = VIPS_IMAGE_SIZEOF_PEL(in);
	VipsRegion *ir = (VipsRegion *) seq;
	VipsRect *r = &out_region->valid;
	const int bands = in->Bands;

	VipsRect s;

#ifdef DEBUG
	printf("vips_reduceh_uchar_vector_gen: generating %d x %d at %d x %d\n",
		r->width, r->height, r->left, r->top);
#endif /*DEBUG*/

	s.left = r->left * reduceh->residual_hshrink - reduceh->hoffset;
	s.top = r->top;
	s.width = r->width * reduceh->residual_hshrink + reduceh->n_point;
	s.height = r->height;
	if (vips_region_prepare(ir, &s))
		return -1;

	VIPS_GATE_START("vips_reduceh_uchar_vector_gen: work");

	for (int y = 0; y < r->height; y++) {
		VipsPel *p0;
		VipsPel *q;

		double X;

		q = VIPS_REGION_ADDR(out_region, r->left, r->top + y);

		X = (r->left + 0.5) * reduceh->residual_hshrink - 0.5 -
			reduceh->hoffset;

		p0 = VIPS_REGION_ADDR(ir, ir->valid.left, r->top + y) -
			ir->valid.left * ps;

		vips_reduceh_uchar_hwy(q, p0, reduceh->n_point, r->width,
			bands, reduceh->matrixs, X, reduceh->residual_hshrink);
	}

	VIPS_GATE_STOP("vips_reduceh_uchar_vector_gen: work");

	VIPS_COUNT_PIXELS(out_region, "vips_reduceh_uchar_vector_gen");

	return 0;
}
#endif /*HAVE_HWY*/

static int
vips_reduceh_build(VipsObject *object)
{
	VipsObjectClass *object_class = VIPS_OBJECT_GET_CLASS(object);
	VipsResample *resample = VIPS_RESAMPLE(object);
	VipsReduceh *reduceh = (VipsReduceh *) object;
	VipsImage **t = (VipsImage **)
		vips_object_local_array(object, 3);

	VipsImage *in;
	VipsGenerateFn generate;
	int width;
	int int_hshrink;
	double extra_pixels;

	if (VIPS_OBJECT_CLASS(vips_reduceh_parent_class)->build(object))
		return -1;

	in = resample->in;

	if (reduceh->hshrink < 1.0) {
		vips_error(object_class->nickname,
			"%s", _("reduce factor should be >= 1.0"));
		return -1;
	}

	/* Output size. We need to always round to nearest, so round(), not
	 * rint().
	 */
	width = VIPS_ROUND_UINT(
		(double) in->Xsize / reduceh->hshrink);

	/* How many pixels we are inventing in the input, -ve for
	 * discarding.
	 */
	extra_pixels = width * reduceh->hshrink - in->Xsize;

	/* The hshrink we do after integer reduction.
	 */
	reduceh->residual_hshrink = reduceh->hshrink;

	if (reduceh->gap > 0.0 &&
		reduceh->kernel != VIPS_KERNEL_NEAREST) {
		if (reduceh->gap < 1.0) {
			vips_error(object_class->nickname,
				"%s", _("reduce gap should be >= 1.0"));
			return -1;
		}

		/* The int part of our reduce.
		 */
		int_hshrink = VIPS_MAX(1,
			floor((double) in->Xsize / width / reduceh->gap));

		if (int_hshrink > 1) {
			g_info("shrinkh by %d", int_hshrink);
			if (vips_shrinkh(in, &t[0], int_hshrink,
					"ceil", TRUE,
					nullptr))
				return -1;
			in = t[0];

			reduceh->residual_hshrink /= int_hshrink;
			extra_pixels /= int_hshrink;
		}
	}

	if (reduceh->residual_hshrink == 1.0)
		return vips_image_write(in, resample->out);

	reduceh->n_point =
		vips_reduce_get_points(reduceh->kernel, reduceh->residual_hshrink);
	g_info("reduceh: %d point mask", reduceh->n_point);
	if (reduceh->n_point > MAX_POINT) {
		vips_error(object_class->nickname,
			"%s", _("reduce factor too large"));
		return -1;
	}

	/* If we are rounding down, we are not using some input
	 * pixels. We need to move the origin *inside* the input image
	 * by half that distance so that we discard pixels equally
	 * from left and right.
	 */
	reduceh->hoffset = (1 + extra_pixels) / 2.0 - 1;

	/* Build the tables of pre-computed coefficients.
	 */
	for (int x = 0; x < VIPS_TRANSFORM_SCALE + 1; x++) {
		reduceh->matrixf[x] =
			VIPS_ARRAY(object, reduceh->n_point, double);
		reduceh->matrixs[x] =
			VIPS_ARRAY(object, reduceh->n_point, short);
		if (!reduceh->matrixf[x] ||
			!reduceh->matrixs[x])
			return -1;

		vips_reduce_make_mask(reduceh->matrixf[x], reduceh->kernel,
			reduceh->n_point, reduceh->residual_hshrink,
			(float) x / VIPS_TRANSFORM_SCALE);

		for (int i = 0; i < reduceh->n_point; i++)
			reduceh->matrixs[x][i] = (short) (reduceh->matrixf[x][i] *
				VIPS_INTERPOLATE_SCALE);
#ifdef DEBUG
		printf("vips_reduceh_build: mask %d\n    ", x);
		for (int i = 0; i < reduceh->n_point; i++)
			printf("%d ", reduceh->matrixs[x][i]);
		printf("\n");
#endif /*DEBUG*/
	}

	/* Unpack for processing.
	 */
	if (vips_image_decode(in, &t[1]))
		return -1;
	in = t[1];

	/* Add new pixels around the input so we can interpolate at the edges.
	 */
	if (vips_embed(in, &t[2],
			ceil(reduceh->n_point / 2.0) - 1, 0,
			in->Xsize + reduceh->n_point, in->Ysize,
			"extend", VIPS_EXTEND_COPY,
			nullptr))
		return -1;
	in = t[2];

	/* For uchar input, try to make a vector path.
	 */
#ifdef HAVE_HWY
	if (in->BandFmt == VIPS_FORMAT_UCHAR &&
		(in->Bands == 4 || in->Bands == 3) &&
		vips_vector_isenabled()) {
		generate = vips_reduceh_uchar_vector_gen;
		g_info("reduceh: using vector path");
	}
	else
#endif /*HAVE_HWY*/
		/* Default to the C path.
		 */
		generate = vips_reduceh_gen;

	if (vips_image_pipelinev(resample->out,
			VIPS_DEMAND_STYLE_FATSTRIP, in, nullptr))
		return -1;

	/* Don't change xres/yres, leave that to the application layer. For
	 * example, vipsthumbnail knows the true reduce factor (including the
	 * fractional part), we just see the integer part here.
	 */
	resample->out->Xsize = width;
	if (resample->out->Xsize <= 0) {
		vips_error(object_class->nickname,
			"%s", _("image has shrunk to nothing"));
		return -1;
	}

#ifdef DEBUG
	printf("vips_reduceh_build: reducing %d x %d image to %d x %d\n",
		in->Xsize, in->Ysize,
		resample->out->Xsize, resample->out->Ysize);
#endif /*DEBUG*/

	if (vips_image_generate(resample->out,
			vips_start_one, generate, vips_stop_one,
			in, reduceh))
		return -1;

	vips_reorder_margin_hint(resample->out, reduceh->n_point);

	return 0;
}

static void
vips_reduceh_class_init(VipsReducehClass *reduceh_class)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(reduceh_class);
	VipsObjectClass *vobject_class = VIPS_OBJECT_CLASS(reduceh_class);
	VipsOperationClass *operation_class =
		VIPS_OPERATION_CLASS(reduceh_class);

	VIPS_DEBUG_MSG("vips_reduceh_class_init\n");

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	vobject_class->nickname = "reduceh";
	vobject_class->description = _("shrink an image horizontally");
	vobject_class->build = vips_reduceh_build;

	operation_class->flags = VIPS_OPERATION_SEQUENTIAL;

	VIPS_ARG_DOUBLE(reduceh_class, "hshrink", 3,
		_("Hshrink"),
		_("Horizontal shrink factor"),
		VIPS_ARGUMENT_REQUIRED_INPUT,
		G_STRUCT_OFFSET(VipsReduceh, hshrink),
		1.0, 1000000.0, 1.0);

	VIPS_ARG_ENUM(reduceh_class, "kernel", 4,
		_("Kernel"),
		_("Resampling kernel"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsReduceh, kernel),
		VIPS_TYPE_KERNEL, VIPS_KERNEL_LANCZOS3);

	VIPS_ARG_DOUBLE(reduceh_class, "gap", 5,
		_("Gap"),
		_("Reducing gap"),
		VIPS_ARGUMENT_OPTIONAL_INPUT,
		G_STRUCT_OFFSET(VipsReduceh, gap),
		0.0, 1000000.0, 0.0);

	/* Old name.
	 */
	VIPS_ARG_DOUBLE(reduceh_class, "xshrink", 3,
		_("Xshrink"),
		_("Horizontal shrink factor"),
		VIPS_ARGUMENT_REQUIRED_INPUT | VIPS_ARGUMENT_DEPRECATED,
		G_STRUCT_OFFSET(VipsReduceh, hshrink),
		1.0, 1000000.0, 1.0);

	/* We used to let people pick centre or corner, but it's automatic now.
	 */
	VIPS_ARG_BOOL(reduceh_class, "centre", 7,
		_("Centre"),
		_("Use centre sampling convention"),
		VIPS_ARGUMENT_OPTIONAL_INPUT | VIPS_ARGUMENT_DEPRECATED,
		G_STRUCT_OFFSET(VipsReduceh, centre),
		FALSE);
}

static void
vips_reduceh_init(VipsReduceh *reduceh)
{
	reduceh->gap = 0.0;
	reduceh->kernel = VIPS_KERNEL_LANCZOS3;
}

/**
 * vips_reduceh: (method)
 * @in: input image
 * @out: (out): output image
 * @hshrink: horizontal reduce
 * @...: %NULL-terminated list of optional named arguments
 *
 * Reduce @in horizontally by a float factor.
 *
 * The pixels in @out are
 * interpolated with a 1D mask generated by @kernel.
 *
 * Set @gap to speed up reducing by having [method@Image.shrinkh] to shrink
 * with a box filter first. The bigger @gap, the closer the result
 * to the fair resampling. The smaller @gap, the faster resizing.
 * The default value is 0.0 (no optimization).
 *
 * This is a very low-level operation: see [method@Image.resize] for a more
 * convenient way to resize images.
 *
 * This operation does not change xres or yres. The image resolution needs to
 * be updated by the application.
 *
 * ::: tip "Optional arguments"
 *     * @kernel: [enum@Kernel], to use to interpolate (default: lanczos3)
 *     * @gap: %gboolean, reducing gap to use (default: 0.0)
 *
 * ::: seealso
 *     [method@Image.shrink], [method@Image.resize], [method@Image.affine].
 *
 * Returns: 0 on success, -1 on error
 */
int
vips_reduceh(VipsImage *in, VipsImage **out, double hshrink, ...)
{
	va_list ap;
	int result;

	va_start(ap, hshrink);
	result = vips_call_split("reduceh", ap, in, out, hshrink);
	va_end(ap);

	return result;
}
