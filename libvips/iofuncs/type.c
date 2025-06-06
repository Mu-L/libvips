/* array type
 *
 * Unlike GArray, this has fixed length, tracks a GType for elements, and has
 * a per-element free function.
 *
 * 27/10/11
 * 	- from header.c
 * 16/7/13
 * 	- leakcheck VipsArea
 * 16/8/17
 * 	- validate strings as utf-8 on set
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
#define DEBUG
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/
#include <glib/gi18n-lib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <vips/vips.h>
#include <vips/internal.h>
#include <vips/debug.h>

/* A very simple boxed type for testing. Just an int.
 *
 * You can manipulate this thing from Python (for example) with:
 *
 * from gi.repository import Vips
 * a = Vips.Thing.new(12)
 * print a.i
 * b = a
 * del a
 * print b.i
 * del b
 */

/**
 * vips_thing_new:
 * @i:
 *
 * Returns: (transfer full): a new [struct@Thing].
 */
VipsThing *
vips_thing_new(int i)
{
	VipsThing *thing;

	thing = g_new(VipsThing, 1);
	thing->i = i;

	printf("vips_thing_new: %d %p\n", i, thing);

	return thing;
}

static VipsThing *
vips_thing_copy(VipsThing *thing)
{
	VipsThing *thing2;

	thing2 = vips_thing_new(thing->i);

	printf("vips_thing_copy: %d %p = %p\n", thing->i, thing2, thing);

	return thing2;
}

static void
vips_thing_free(VipsThing *thing)
{
	printf("vips_thing_free: %d %p\n", thing->i, thing);

	g_free(thing);
}

G_DEFINE_BOXED_TYPE(VipsThing, vips_thing,
	(GBoxedCopyFunc) vips_thing_copy,
	(GBoxedFreeFunc) vips_thing_free)

static GSList *vips_area_all = NULL;

VipsArea *
vips_area_copy(VipsArea *area)
{
	g_mutex_lock(&area->lock);

	g_assert(area->count > 0);

	area->count += 1;

#ifdef DEBUG
	printf("vips_area_copy: %p count = %d\n", area, area->count);
#endif /*DEBUG*/

	g_mutex_unlock(&area->lock);

	return area;
}

int
vips_area_free_cb(void *mem, VipsArea *area)
{
	g_free(mem);

	return 0;
}

static void
vips_area_free(VipsArea *area)
{
	if (area->free_fn &&
		area->data) {
		area->free_fn(area->data, area);
		area->free_fn = NULL;
	}

	area->data = NULL;
}

void
vips_area_unref(VipsArea *area)
{
	g_mutex_lock(&area->lock);

	g_assert(area->count > 0);

	area->count -= 1;

#ifdef DEBUG
	printf("vips_area_unref: %p count = %d\n", area, area->count);
#endif /*DEBUG*/

	if (vips__leak) {
		g_mutex_lock(&vips__global_lock);
		g_assert(g_slist_find(vips_area_all, area));
		g_mutex_unlock(&vips__global_lock);
	}

	if (area->count == 0) {
		vips_area_free(area);

		g_mutex_unlock(&area->lock);

		g_mutex_clear(&area->lock);

		if (vips__leak) {
			g_mutex_lock(&vips__global_lock);
			vips_area_all = g_slist_remove(vips_area_all, area);
			g_mutex_unlock(&vips__global_lock);
		}

		g_free(area);

#ifdef DEBUG
		g_mutex_lock(&vips__global_lock);
		printf("vips_area_unref: free .. total = %d\n",
			g_slist_length(vips_area_all));
		g_mutex_unlock(&vips__global_lock);
#endif /*DEBUG*/
	}
	else
		g_mutex_unlock(&area->lock);
}

/* autoptr needs typed versions of functions for free.
 */
void
VipsArrayDouble_unref(VipsArrayDouble *array)
{
	vips_area_unref(VIPS_AREA(array));
}

void
VipsArrayImage_unref(VipsArrayImage *array)
{
	vips_area_unref(VIPS_AREA(array));
}

/**
 * vips_area_new:
 * @free_fn: (scope async) (nullable): @data will be freed with this function
 * @data: (transfer full): data will be freed with this function
 *
 * A VipsArea wraps a chunk of memory. It adds reference counting and a free
 * function. It also keeps a count and a [alias@GObject.Type], so the area can
 * be an array.
 *
 * This type is used for things like passing an array of double or an array of
 * [class@Object] pointers to operations, and for reference-counted immutable
 * strings.
 *
 * Initial count == 1, so [method@Area.unref] after attaching somewhere.
 *
 * ::: seealso
 *     [method@Area.unref].
 *
 * Returns: (transfer full): the new [struct@Area].
 */
VipsArea *
vips_area_new(VipsCallbackFn free_fn, void *data)
{
	VipsArea *area;

	area = g_new(VipsArea, 1);
	area->count = 1;
	g_mutex_init(&area->lock);
	area->length = 0;
	area->data = data;
	area->free_fn = free_fn;
	area->type = 0;
	area->sizeof_type = 0;

	if (vips__leak) {
		g_mutex_lock(&vips__global_lock);
		vips_area_all = g_slist_prepend(vips_area_all, area);
		g_mutex_unlock(&vips__global_lock);
	}

#ifdef DEBUG
	g_mutex_lock(&vips__global_lock);
	printf("vips_area_new: %p count = %d (%d in total)\n",
		area, area->count,
		g_slist_length(vips_area_all));
	g_mutex_unlock(&vips__global_lock);
#endif /*DEBUG*/

	return area;
}

int
vips__type_leak(void)
{
	int n_leaks;

	n_leaks = 0;

	if (vips_area_all) {
		GSList *p;

		fprintf(stderr, "%d VipsArea alive\n",
			g_slist_length(vips_area_all));
		for (p = vips_area_all; p; p = p->next) {
			VipsArea *area = VIPS_AREA(p->data);

			fprintf(stderr, "\t%p count = %d, bytes = %zd\n",
				area, area->count, area->length);

			n_leaks += 1;
		}
	}

	return n_leaks;
}

/**
 * vips_area_new_array:
 * @type: [alias@GObject.Type] of elements to store
 * @sizeof_type: `sizeof()` an element in the array
 * @n: number of elements in the array
 *
 * An area which holds an array of elements of some [alias@GObject.Type].
 * To set values for the elements, get the pointer and write.
 *
 * ::: seealso
 *     [method@Area.unref].
 *
 * Returns: (transfer full): the new [struct@Area].
 */
VipsArea *
vips_area_new_array(GType type, size_t sizeof_type, int n)
{
	VipsArea *area;
	void *array;

	array = g_malloc(n * sizeof_type);
	area = vips_area_new((VipsCallbackFn) vips_area_free_cb, array);
	area->n = n;
	area->length = n * sizeof_type;
	area->type = type;
	area->sizeof_type = sizeof_type;

	return area;
}

static int
vips_area_free_array_object(GObject **array, VipsArea *area)
{
	int i;

	for (i = 0; i < area->n; i++)
		VIPS_FREEF(g_object_unref, array[i]);
	VIPS_FREE(array);

	area->n = 0;

	return 0;
}

/**
 * vips_area_new_array_object: (constructor)
 * @n: number of elements in the array
 *
 * An area which holds an array of [class@GObject.Object] s. See [ctor@Area.new_array]. When
 * the area is freed, each [class@GObject.Object] will be unreffed.
 *
 * Add an extra `NULL` element at the end, handy for eg.
 * [func@Image.pipeline_array] etc.
 *
 * ::: seealso
 *     [method@Area.unref].
 *
 * Returns: (transfer full): the new [struct@Area].
 */
VipsArea *
vips_area_new_array_object(int n)
{
	GObject **array;
	VipsArea *area;

	array = g_new0(GObject *, n + 1);
	area = vips_area_new((VipsCallbackFn) vips_area_free_array_object,
		array);
	area->n = n;
	area->length = n * sizeof(GObject *);
	area->type = G_TYPE_OBJECT;
	area->sizeof_type = sizeof(GObject *);

	return area;
}

/**
 * vips_area_get_data:
 * @area: [struct@Area] to fetch from
 * @length: (out) (optional): optionally return length in bytes here
 * @n: (out) (optional): optionally return number of elements here
 * @type: (out) (optional): optionally return element type here
 * @sizeof_type: (out) (optional): optionally return `sizeof()` element type here
 *
 * Return the data pointer plus optionally the length in bytes of an area,
 * the number of elements, the [alias@GObject.Type] of each element and the
 * `sizeof()` each element.
 *
 * Returns: (transfer none): The pointer held by @area.
 */
void *
vips_area_get_data(VipsArea *area,
	size_t *length, int *n, GType *type, size_t *sizeof_type)
{
	if (!area)
		return NULL;

	if (length)
		*length = area->length;
	if (n)
		*n = area->n;
	if (type)
		*type = area->type;
	if (sizeof_type)
		*sizeof_type = area->sizeof_type;

	return area->data;
}

/* Transform an area to a G_TYPE_STRING.
 */
static void
transform_area_g_string(const GValue *src_value, GValue *dest_value)
{
	VipsArea *area;
	char buf[256];

	area = g_value_get_boxed(src_value);
	g_snprintf(buf, 256, "VIPS_TYPE_AREA, count = %d, data = %p",
		area->count, area->data);
	g_value_set_string(dest_value, buf);
}

G_DEFINE_BOXED_TYPE_WITH_CODE(VipsArea, vips_area,
							  (GBoxedCopyFunc) vips_area_copy,
							  (GBoxedFreeFunc) vips_area_unref,
							  g_value_register_transform_func(
								  g_define_type_id,
								  G_TYPE_STRING,
								  transform_area_g_string);)

/* Transform funcs for builtin types to SAVE_STRING.
 */
static void
transform_int_save_string(const GValue *src_value, GValue *dest_value)
{
	vips_value_set_save_stringf(dest_value,
		"%d", g_value_get_int(src_value));
}

static void
transform_save_string_int(const GValue *src_value, GValue *dest_value)
{
	g_value_set_int(dest_value,
		atoi(vips_value_get_save_string(src_value)));
}

static void
transform_double_save_string(const GValue *src_value, GValue *dest_value)
{
	char buf[G_ASCII_DTOSTR_BUF_SIZE];

	/* Need to be locale independent.
	 */
	g_ascii_dtostr(buf, G_ASCII_DTOSTR_BUF_SIZE,
		g_value_get_double(src_value));
	vips_value_set_save_string(dest_value, buf);
}

static void
transform_save_string_double(const GValue *src_value, GValue *dest_value)
{
	g_value_set_double(dest_value,
		g_ascii_strtod(vips_value_get_save_string(src_value),
			NULL));
}

static void
transform_float_save_string(const GValue *src_value, GValue *dest_value)
{
	char buf[G_ASCII_DTOSTR_BUF_SIZE];

	/* Need to be locale independent.
	 */
	g_ascii_dtostr(buf, G_ASCII_DTOSTR_BUF_SIZE,
		g_value_get_float(src_value));
	vips_value_set_save_string(dest_value, buf);
}

static void
transform_save_string_float(const GValue *src_value, GValue *dest_value)
{
	g_value_set_float(dest_value,
		g_ascii_strtod(vips_value_get_save_string(src_value),
			NULL));
}

/* Save meta fields to the header. We have a new string type for header fields
 * to save to XML and define transform functions to go from our meta types to
 * this string type.
 */
G_DEFINE_BOXED_TYPE_WITH_CODE(VipsSaveString, vips_save_string,
							  (GBoxedCopyFunc) g_strdup,
							  (GBoxedFreeFunc) g_free,
							  g_value_register_transform_func(
								  G_TYPE_INT,
								  g_define_type_id,
								  transform_int_save_string);

							  g_value_register_transform_func(
								  g_define_type_id,
								  G_TYPE_INT,
								  transform_save_string_int);

							  g_value_register_transform_func(
								  G_TYPE_DOUBLE,
								  g_define_type_id,
								  transform_double_save_string);

							  g_value_register_transform_func(
								  g_define_type_id,
								  G_TYPE_DOUBLE,
								  transform_save_string_double);

							  g_value_register_transform_func(
								  G_TYPE_FLOAT,
								  g_define_type_id,
								  transform_float_save_string);

							  g_value_register_transform_func(
								  g_define_type_id,
								  G_TYPE_FLOAT,
								  transform_save_string_float);)

/* Transform a refstring to a G_TYPE_STRING and back.
 */
static void
transform_ref_string_g_string(const GValue *src_value, GValue *dest_value)
{
	g_value_set_string(dest_value,
		vips_value_get_ref_string(src_value, NULL));
}

static void
transform_g_string_ref_string(const GValue *src_value, GValue *dest_value)
{
	vips_value_set_ref_string(dest_value,
		g_value_get_string(src_value));
}

/* To a save string.
 */
static void
transform_ref_string_save_string(const GValue *src_value, GValue *dest_value)
{
	vips_value_set_save_stringf(dest_value,
		"%s", vips_value_get_ref_string(src_value, NULL));
}

static void
transform_save_string_ref_string(const GValue *src_value, GValue *dest_value)
{
	vips_value_set_ref_string(dest_value,
		vips_value_get_save_string(src_value));
}

/**
 * vips_ref_string_new:
 * @str: (transfer none): string to store
 *
 * Create a new refstring. These are reference-counted immutable strings, used
 * to store string data in vips image metadata.
 *
 * Strings must be valid utf-8; use blob for binary data.
 *
 * ::: seealso
 *     [method@Area.unref].
 *
 * Returns: (transfer full) (nullable): the new [struct@RefString], or `NULL` on
 * error.
 */
VipsRefString *
vips_ref_string_new(const char *str)
{
	VipsArea *area;

	char *utf8_str = g_utf8_make_valid(str, -1);

	area = vips_area_new((VipsCallbackFn) vips_area_free_cb, utf8_str);

	/* Handy place to cache this.
	 */
	area->length = strlen(utf8_str);

	return (VipsRefString *) area;
}

/**
 * vips_ref_string_get:
 * @refstr: the [struct@RefString] to fetch from
 * @length: (out) (optional): return length here, optionally
 *
 * Get a pointer to the private string inside a refstr. Handy for language
 * bindings.
 *
 * ::: seealso
 *     [func@value_get_ref_string].
 *
 * Returns: (transfer none): The C string held by @refstr.
 */
const char *
vips_ref_string_get(VipsRefString *refstr, size_t *length)
{
	VipsArea *area = VIPS_AREA(refstr);

	return vips_area_get_data(area, length, NULL, NULL, NULL);
}

G_DEFINE_BOXED_TYPE_WITH_CODE(VipsRefString, vips_ref_string,
							  (GBoxedCopyFunc) vips_area_copy,
							  (GBoxedFreeFunc) vips_area_unref,
							  g_value_register_transform_func(
								  g_define_type_id,
								  G_TYPE_STRING,
								  transform_ref_string_g_string);

							  g_value_register_transform_func(
								  G_TYPE_STRING,
								  g_define_type_id,
								  transform_g_string_ref_string);

							  g_value_register_transform_func(
								  g_define_type_id,
								  VIPS_TYPE_SAVE_STRING,
								  transform_ref_string_save_string);

							  g_value_register_transform_func(
								  VIPS_TYPE_SAVE_STRING,
								  g_define_type_id,
								  transform_save_string_ref_string);)

/**
 * vips_blob_new:
 * @free_fn: (scope async) (allow-none): @data will be freed with this function
 * @data: (array length=length) (element-type guint8) (transfer full): data to store
 * @length: number of bytes in @data
 *
 * Like [ctor@Area.new], but track a length as well. The returned [struct@Blob]
 * takes ownership of @data and will free it with @free_fn. Pass `NULL` for
 * @free_fn to not transfer ownership.
 *
 * An area of mem with a free func and a length (some sort of binary object,
 * like an ICC profile).
 *
 * ::: seealso
 *     [method@Area.unref].
 *
 * Returns: (transfer full): the new [struct@Blob].
 */
VipsBlob *
vips_blob_new(VipsCallbackFn free_fn, const void *data, size_t length)
{
	VipsArea *area;

	area = vips_area_new(free_fn, (void *) data);
	area->length = length;

	return (VipsBlob *) area;
}

/**
 * vips_blob_copy:
 * @data: (array length=length) (element-type guint8) (transfer none): data to store
 * @length: number of bytes in @data
 *
 * Like [ctor@Blob.new], but take a copy of the data. Useful for bindings
 * which struggle with callbacks.
 *
 * ::: seealso
 *     [ctor@Blob.new].
 *
 * Returns: (transfer full): the new [struct@Blob].
 */
VipsBlob *
vips_blob_copy(const void *data, size_t length)
{
	void *data_copy;
	VipsArea *area;

	data_copy = g_malloc(length);
	memcpy(data_copy, data, length);
	area = vips_area_new((VipsCallbackFn) vips_area_free_cb, data_copy);
	area->length = length;

	return (VipsBlob *) area;
}

/**
 * vips_blob_get:
 * @blob: [struct@Blob] to fetch from
 * @length: return number of bytes of data
 *
 * Get the data from a [struct@Blob].
 *
 * ::: seealso
 *     [ctor@Blob.new].
 *
 * Returns: (array length=length) (element-type guint8) (transfer none): the
 * data
 */
const void *
vips_blob_get(VipsBlob *blob, size_t *length)
{
	return vips_area_get_data(VIPS_AREA(blob),
		length, NULL, NULL, NULL);
}

/**
 * vips_blob_set:
 * @blob: [struct@Blob] to set
 * @free_fn: (scope async) (allow-none): @data will be freed with this function
 * @data: (array length=length) (element-type guint8) (transfer full): data to store
 * @length: number of bytes in @data
 *
 * Any old data is freed and new data attached.
 *
 * It's sometimes useful to be able to create blobs as empty and then fill
 * them later.
 *
 * ::: seealso
 *     [ctor@Blob.new].
 */
void
vips_blob_set(VipsBlob *blob,
	VipsCallbackFn free_fn, const void *data, size_t length)
{
	VipsArea *area = VIPS_AREA(blob);

	g_mutex_lock(&area->lock);

	vips_area_free(area);

	area->free_fn = free_fn;
	area->length = length;
	area->data = (void *) data;

	g_mutex_unlock(&area->lock);
}

/* Transform a blob to a G_TYPE_STRING.
 */
static void
transform_blob_g_string(const GValue *src_value, GValue *dest_value)
{
	void *blob;
	size_t length;
	char buf[256];

	blob = vips_value_get_blob(src_value, &length);
	g_snprintf(buf, 256, "VIPS_TYPE_BLOB, data = %p, length = %zd",
		blob, length);
	g_value_set_string(dest_value, buf);
}

/* Transform a blob to a save string and back.
 */
static void
transform_blob_save_string(const GValue *src_value, GValue *dest_value)
{
	void *blob;
	size_t length;
	char *b64;

	blob = vips_value_get_blob(src_value, &length);
	if ((b64 = g_base64_encode(blob, length))) {
		vips_value_set_save_string(dest_value, b64);
		g_free(b64);
	}
	else
		/* No error return from transform, but we should set it to
		 * something.
		 */
		vips_value_set_save_string(dest_value, "");
}

static void
transform_save_string_blob(const GValue *src_value, GValue *dest_value)
{
	const char *b64;
	void *blob;
	size_t length;

	b64 = vips_value_get_save_string(src_value);
	if ((blob = g_base64_decode(b64, &length)))
		vips_value_set_blob(dest_value,
			(VipsCallbackFn) vips_area_free_cb, blob, length);
	else
		/* No error return from transform, but we should set it to
		 * something.
		 */
		vips_value_set_blob(dest_value, NULL, NULL, 0);
}

G_DEFINE_BOXED_TYPE_WITH_CODE(VipsBlob, vips_blob,
							  (GBoxedCopyFunc) vips_area_copy,
							  (GBoxedFreeFunc) vips_area_unref,
							  g_value_register_transform_func(
								  g_define_type_id,
								  G_TYPE_STRING,
								  transform_blob_g_string);

							  g_value_register_transform_func(
								  g_define_type_id,
								  VIPS_TYPE_SAVE_STRING,
								  transform_blob_save_string);

							  g_value_register_transform_func(
								  VIPS_TYPE_SAVE_STRING,
								  g_define_type_id,
								  transform_save_string_blob);)

/**
 * vips_array_int_new:
 * @array: (array length=n): array of int
 * @n: number of ints
 *
 * Allocate a new array of ints and copy @array into it. Free with
 * [method@Area.unref].
 *
 * ::: seealso
 *     [struct@Area].
 *
 * Returns: (transfer full): A new [struct@ArrayInt].
 */
VipsArrayInt *
vips_array_int_new(const int *array, int n)
{
	VipsArea *area;
	int *array_copy;

	area = vips_area_new_array(G_TYPE_INT, sizeof(int), n);
	array_copy = vips_area_get_data(area, NULL, NULL, NULL, NULL);
	memcpy(array_copy, array, n * sizeof(int));

	return (VipsArrayInt *) area;
}

/**
 * vips_array_int_newv:
 * @n: number of ints
 * @...: list of int arguments
 *
 * Allocate a new array of @n ints and copy @... into it. Free with
 * [method@Area.unref].
 *
 * ::: seealso
 *     [ctor@ArrayInt.new]
 *
 * Returns: (transfer full): A new [struct@ArrayInt].
 */
VipsArrayInt *
vips_array_int_newv(int n, ...)
{
	va_list ap;
	VipsArea *area;
	int *array;
	int i;

	area = vips_area_new_array(G_TYPE_INT, sizeof(int), n);
	array = vips_area_get_data(area, NULL, NULL, NULL, NULL);

	va_start(ap, n);
	for (i = 0; i < n; i++)
		array[i] = va_arg(ap, int);
	va_end(ap);

	return (VipsArrayInt *) area;
}

/**
 * vips_array_int_get:
 * @array: the [struct@ArrayInt] to fetch from
 * @n: length of array
 *
 * Fetch an int array from a [struct@ArrayInt]. Useful for language bindings.
 *
 * Returns: (array length=n) (transfer none): array of int
 */
int *
vips_array_int_get(VipsArrayInt *array, int *n)
{
	VipsArea *area = VIPS_AREA(array);

	g_assert(area->type == G_TYPE_INT);

	if (n)
		*n = area->n;

	return (int *) VIPS_ARRAY_ADDR(array, 0);
}

static void
transform_array_int_g_string(const GValue *src_value, GValue *dest_value)
{
	int n;
	int *array;

	char txt[1024];
	VipsBuf buf = VIPS_BUF_STATIC(txt);
	int i;

	if ((array = vips_value_get_array_int(src_value, &n)))
		for (i = 0; i < n; i++)
			/* Use space as a separator since ',' may be a
			 * decimal point in this locale.
			 */
			vips_buf_appendf(&buf, "%d ", array[i]);

	g_value_set_string(dest_value, vips_buf_all(&buf));
}

static void
transform_array_int_save_string(const GValue *src_value, GValue *dest_value)
{
	GValue intermediate = G_VALUE_INIT;

	g_value_init(&intermediate, G_TYPE_STRING);

	transform_array_int_g_string(src_value, &intermediate);

	vips_value_set_save_string(dest_value,
		g_value_get_string(&intermediate));

	g_value_unset(&intermediate);
}

/* It'd be great to be able to write a generic string->array function, but
 * it doesn't seem possible.
 */
static void
transform_g_string_array_int(const GValue *src_value, GValue *dest_value)
{
	char *str;
	int n;
	char *p, *q;
	int i;
	int *array;

	/* Walk the string to get the number of elements.
	 * We need a copy of the string, since we insert \0 during
	 * scan.
	 */
	str = g_value_dup_string(src_value);

	n = 0;
	for (p = str; (q = vips_break_token(p, "\t;, ")); p = q)
		n += 1;

	g_free(str);

	vips_value_set_array_int(dest_value, NULL, n);
	array = vips_value_get_array_int(dest_value, NULL);

	str = g_value_dup_string(src_value);

	i = 0;
	for (p = str; (q = vips_break_token(p, "\t; ")); p = q) {
		if (sscanf(p, "%d", &array[i]) != 1) {
			/* Set array to length zero to indicate an error.
			 */
			vips_error("vipstype",
				_("unable to convert \"%s\" to int"), p);
			vips_value_set_array(dest_value,
				0, G_TYPE_INT, sizeof(int));
			g_free(str);
			return;
		}

		i += 1;
	}

	g_free(str);
}

static void
transform_save_string_array_int(const GValue *src_value, GValue *dest_value)
{
	GValue intermediate = G_VALUE_INIT;

	g_value_init(&intermediate, G_TYPE_STRING);

	g_value_set_string(&intermediate,
		vips_value_get_save_string(src_value));

	transform_g_string_array_int(&intermediate, dest_value);

	g_value_unset(&intermediate);
}

/* We need a arrayint, we have an int, make a one-element array.
 */
static void
transform_int_array_int(const GValue *src_value, GValue *dest_value)
{
	int *array;

	vips_value_set_array_int(dest_value, NULL, 1);
	array = vips_value_get_array_int(dest_value, NULL);
	array[0] = g_value_get_int(src_value);
}

static void
transform_double_array_int(const GValue *src_value, GValue *dest_value)
{
	int *array;

	vips_value_set_array_int(dest_value, NULL, 1);
	array = vips_value_get_array_int(dest_value, NULL);
	array[0] = g_value_get_double(src_value);
}

static void
transform_array_double_array_int(const GValue *src_value, GValue *dest_value)
{
	int n;
	double *array_double = vips_value_get_array_double(src_value, &n);
	int *array_int;
	int i;

	vips_value_set_array_int(dest_value, NULL, n);
	array_int = vips_value_get_array_int(dest_value, NULL);
	for (i = 0; i < n; i++)
		array_int[i] = array_double[i];
}

G_DEFINE_BOXED_TYPE_WITH_CODE(VipsArrayInt, vips_array_int,
							  (GBoxedCopyFunc) vips_area_copy,
							  (GBoxedFreeFunc) vips_area_unref,
							  g_value_register_transform_func(
								  g_define_type_id,
								  G_TYPE_STRING,
								  transform_array_int_g_string);

							  g_value_register_transform_func(
								  G_TYPE_STRING,
								  g_define_type_id,
								  transform_g_string_array_int);

							  g_value_register_transform_func(
								  G_TYPE_INT,
								  g_define_type_id,
								  transform_int_array_int);

							  g_value_register_transform_func(
								  G_TYPE_DOUBLE,
								  g_define_type_id,
								  transform_double_array_int);

							  g_value_register_transform_func(
								  g_define_type_id,
								  VIPS_TYPE_SAVE_STRING,
								  transform_array_int_save_string);

							  g_value_register_transform_func(
								  VIPS_TYPE_SAVE_STRING,
								  g_define_type_id,
								  transform_save_string_array_int);)

/**
 * vips_array_double_new:
 * @array: (array length=n): array of double
 * @n: number of doubles
 *
 * Allocate a new array of doubles and copy @array into it. Free with
 * [method@Area.unref].
 *
 * ::: seealso
 *     [struct@Area].
 *
 * Returns: (transfer full): A new [struct@ArrayDouble].
 */
VipsArrayDouble *
vips_array_double_new(const double *array, int n)
{
	VipsArea *area;
	double *array_copy;

	area = vips_area_new_array(G_TYPE_DOUBLE, sizeof(double), n);
	array_copy = vips_area_get_data(area, NULL, NULL, NULL, NULL);
	memcpy(array_copy, array, n * sizeof(double));

	return (VipsArrayDouble *) area;
}

/**
 * vips_array_double_newv:
 * @n: number of doubles
 * @...: list of double arguments
 *
 * Allocate a new array of @n doubles and copy @... into it. Free with
 * [method@Area.unref].
 *
 * ::: seealso
 *     [ctor@ArrayDouble.new]
 *
 * Returns: (transfer full): A new [struct@ArrayDouble].
 */
VipsArrayDouble *
vips_array_double_newv(int n, ...)
{
	va_list ap;
	VipsArea *area;
	double *array;
	int i;

	area = vips_area_new_array(G_TYPE_DOUBLE, sizeof(double), n);
	array = vips_area_get_data(area, NULL, NULL, NULL, NULL);

	va_start(ap, n);
	for (i = 0; i < n; i++)
		array[i] = va_arg(ap, double);
	va_end(ap);

	return (VipsArrayDouble *) area;
}

/**
 * vips_array_double_get:
 * @array: the [struct@ArrayDouble] to fetch from
 * @n: length of array
 *
 * Fetch a double array from a [struct@ArrayDouble]. Useful for language bindings.
 *
 * Returns: (array length=n) (transfer none): array of double
 */
double *
vips_array_double_get(VipsArrayDouble *array, int *n)
{
	VipsArea *area = VIPS_AREA(array);

	g_assert(area->type == G_TYPE_DOUBLE);

	if (n)
		*n = area->n;

	return VIPS_ARRAY_ADDR(array, 0);
}

static void
transform_array_double_g_string(const GValue *src_value, GValue *dest_value)
{
	int n;
	double *array;

	char txt[1024];
	VipsBuf buf = VIPS_BUF_STATIC(txt);
	int i;

	if ((array = vips_value_get_array_double(src_value, &n)))
		for (i = 0; i < n; i++)
			/* Use space as a separator since ',' may be a decimal
			 * point in this locale.
			 */
			vips_buf_appendf(&buf, "%g ", array[i]);

	g_value_set_string(dest_value, vips_buf_all(&buf));
}

/* It'd be great to be able to write a generic string->array function, but
 * it doesn't seem possible.
 */
static void
transform_g_string_array_double(const GValue *src_value, GValue *dest_value)
{
	char *str;
	int n;
	char *p, *q;
	int i;
	double *array;

	/* Walk the string to get the number of elements.
	 * We need a copy of the string, since we insert \0 during scan.
	 */
	str = g_value_dup_string(src_value);

	n = 0;
	for (p = str; (q = vips_break_token(p, "\t;, ")); p = q)
		n += 1;

	g_free(str);

	vips_value_set_array_double(dest_value, NULL, n);
	array = vips_value_get_array_double(dest_value, NULL);

	str = g_value_dup_string(src_value);

	i = 0;
	for (p = str; (q = vips_break_token(p, "\t; ")); p = q) {
		if (vips_strtod(p, &array[i])) {
			/* Set array to length zero to indicate an error.
			 */
			g_warning("bad argument: not a numeric value \"%s\"", p);
			vips_value_set_array_double(dest_value, NULL, 0);
			g_free(str);
			return;
		}

		i += 1;
	}

	g_free(str);
}

/* We need a arraydouble, we have a double, make a one-element array.
 */
static void
transform_double_array_double(const GValue *src_value, GValue *dest_value)
{
	double *array;

	vips_value_set_array_double(dest_value, NULL, 1);
	array = vips_value_get_array_double(dest_value, NULL);
	array[0] = g_value_get_double(src_value);
}

static void
transform_int_array_double(const GValue *src_value, GValue *dest_value)
{
	double *array;

	vips_value_set_array_double(dest_value, NULL, 1);
	array = vips_value_get_array_double(dest_value, NULL);
	array[0] = g_value_get_int(src_value);
}

static void
transform_array_int_array_double(const GValue *src_value, GValue *dest_value)
{
	int n;
	int *array_int = vips_value_get_array_int(src_value, &n);
	double *array_double;
	int i;

	vips_value_set_array_double(dest_value, NULL, n);
	array_double = vips_value_get_array_double(dest_value, NULL);
	for (i = 0; i < n; i++)
		array_double[i] = array_int[i];
}

/* You can set enums from ints, but not doubles. Add a double converter too.
 */
static void
transform_double_enum(const GValue *src_value, GValue *dest_value)
{
	g_value_set_enum(dest_value, g_value_get_double(src_value));
}

G_DEFINE_BOXED_TYPE_WITH_CODE(VipsArrayDouble, vips_array_double,
							  (GBoxedCopyFunc) vips_area_copy,
							  (GBoxedFreeFunc) vips_area_unref,
							  g_value_register_transform_func(
								  g_define_type_id,
								  G_TYPE_STRING,
								  transform_array_double_g_string);

							  g_value_register_transform_func(
								  G_TYPE_STRING,
								  g_define_type_id,
								  transform_g_string_array_double);

							  g_value_register_transform_func(
								  G_TYPE_DOUBLE,
								  g_define_type_id,
								  transform_double_array_double);

							  g_value_register_transform_func(
								  G_TYPE_INT,
								  g_define_type_id,
								  transform_int_array_double);

							  g_value_register_transform_func(
								  G_TYPE_DOUBLE,
								  G_TYPE_ENUM,
								  transform_double_enum);)

/**
 * vips_array_image_new: (constructor)
 * @array: (array length=n): array of [class@Image]
 * @n: number of images
 *
 * Allocate a new array of images and copy @array into it. Free with
 * [method@Area.unref].
 *
 * The images will all be reffed by this function. They
 * will be automatically unreffed for you by
 * [method@Area.unref].
 *
 * Add an extra `NULL` element at the end, handy for eg.
 * [func@Image.pipeline_array] etc.
 *
 * ::: seealso
 *     [struct@Area].
 *
 * Returns: (transfer full): A new [struct@ArrayImage].
 */
VipsArrayImage *
vips_array_image_new(VipsImage **array, int n)
{
	VipsArea *area;
	VipsImage **array_copy;
	int i;

	area = vips_area_new_array_object(n);
	area->type = VIPS_TYPE_IMAGE;

	array_copy = vips_area_get_data(area, NULL, NULL, NULL, NULL);
	for (i = 0; i < n; i++) {
		array_copy[i] = (VipsImage *) array[i];
		g_object_ref(array_copy[i]);
	}

	return (VipsArrayImage *) area;
}

/**
 * vips_array_image_newv: (constructor)
 * @n: number of images
 * @...: list of [class@Image] arguments
 *
 * Allocate a new array of @n [class@Image] and copy @... into it. Free with
 * [method@Area.unref].
 *
 * The images will all be reffed by this function. They
 * will be automatically unreffed for you by
 * [method@Area.unref].
 *
 * Add an extra `NULL` element at the end, handy for eg.
 * [func@Image.pipeline_array] etc.
 *
 * ::: seealso
 *     [ctor@ArrayImage.new]
 *
 * Returns: (transfer full): A new [struct@ArrayImage].
 */
VipsArrayImage *
vips_array_image_newv(int n, ...)
{
	va_list ap;
	VipsArea *area;
	VipsImage **array;
	int i;

	area = vips_area_new_array_object(n);
	area->type = VIPS_TYPE_IMAGE;

	array = vips_area_get_data(area, NULL, NULL, NULL, NULL);
	va_start(ap, n);
	for (i = 0; i < n; i++) {
		array[i] = va_arg(ap, VipsImage *);
		g_object_ref(array[i]);
	}
	va_end(ap);

	return (VipsArrayImage *) area;
}

VipsArrayImage *
vips_array_image_new_from_string(const char *string, VipsAccess access)
{
	char *str;
	int n;
	VipsArea *area;
	VipsImage **array;
	char *p, *q;
	int i;

	/* We need a copy of the string, since we insert \0 during
	 * scan.
	 */
	str = g_strdup(string);

	n = 0;
	for (p = str; (q = vips_break_token(p, " \n\t\r")); p = q)
		n += 1;

	g_free(str);

	area = vips_area_new_array_object(n);
	area->type = VIPS_TYPE_IMAGE;

	array = vips_area_get_data(area, NULL, NULL, NULL, NULL);

	str = g_strdup(string);

	i = 0;
	for (p = str; (q = vips_break_token(p, " \n\t\r")); p = q) {
		if (!(array[i] = vips_image_new_from_file(p,
				  "access", access,
				  NULL))) {
			vips_area_unref(area);
			g_free(str);
			return NULL;
		}

		i += 1;
	}

	g_free(str);

	return (VipsArrayImage *) area;
}

/**
 * vips_array_image_empty: (constructor)
 *
 * Make an empty image array.
 * Handy with [method@ArrayImage.append] for bindings
 * which can't handle object array arguments.
 *
 * ::: seealso
 *     [method@ArrayImage.append].
 *
 * Returns: (transfer full): A new [struct@ArrayImage].
 */
VipsArrayImage *
vips_array_image_empty(void)
{
	return vips_array_image_new(NULL, 0);
}

/**
 * vips_array_image_append:
 * @array: (transfer none): append to this
 * @image: add this
 *
 * Make a new [struct@ArrayImage], one larger than @array, with @image appended
 * to the end.
 * Handy with [ctor@ArrayImage.empty] for bindings
 * which can't handle object array arguments.
 *
 * ::: seealso
 *     [ctor@ArrayImage.empty].
 *
 * Returns: (transfer full): A new [struct@ArrayImage].
 */
VipsArrayImage *
vips_array_image_append(VipsArrayImage *array, VipsImage *image)
{
	VipsArea *old_area = VIPS_AREA(array);
	int n = old_area->n;

	VipsArea *new_area;
	VipsImage **old_vector;
	VipsImage **new_vector;
	int i;

	new_area = vips_area_new_array_object(n + 1);
	new_area->type = VIPS_TYPE_IMAGE;

	old_vector = vips_area_get_data(old_area, NULL, NULL, NULL, NULL);
	new_vector = vips_area_get_data(new_area, NULL, NULL, NULL, NULL);
	for (i = 0; i < n; i++) {
		new_vector[i] = (VipsImage *) old_vector[i];
		g_object_ref(new_vector[i]);
	}
	new_vector[i] = image;
	g_object_ref(new_vector[i]);

	return (VipsArrayImage *) new_area;
}

/**
 * vips_array_image_get:
 * @array: the [struct@ArrayImage] to fetch from
 * @n: length of array
 *
 * Fetch an image array from a [struct@ArrayImage]. Useful for language bindings.
 *
 * Returns: (array length=n) (transfer none): array of [class@Image]
 */
VipsImage **
vips_array_image_get(VipsArrayImage *array, int *n)
{
	VipsArea *area = VIPS_AREA(array);

	g_assert(area->type == VIPS_TYPE_IMAGE);

	if (n)
		*n = area->n;

	return (VipsImage **) VIPS_ARRAY_ADDR(array, 0);
}

static void
transform_g_string_array_image(const GValue *src_value, GValue *dest_value)
{
	char *str;
	VipsArrayImage *array_image;

	str = g_value_dup_string(src_value);

	/* We can't get access here, just assume nothing. See the special case
	 * in vips_object_new_from_string() for how we usually get this right.
	 */
	if (!(array_image = vips_array_image_new_from_string(str, 0))) {
		/* Set the dest to length zero to indicate error.
		 */
		vips_value_set_array_image(dest_value, 0);
		g_free(str);
		return;
	}

	g_free(str);

	g_value_set_boxed(dest_value, array_image);
	vips_area_unref(VIPS_AREA(array_image));
}

G_DEFINE_BOXED_TYPE_WITH_CODE(VipsArrayImage, vips_array_image,
							  (GBoxedCopyFunc) vips_area_copy,
							  (GBoxedFreeFunc) vips_area_unref,
							  g_value_register_transform_func(
								  G_TYPE_STRING,
								  g_define_type_id,
								  transform_g_string_array_image);)

/**
 * vips_value_set_area:
 * @value: set this value
 * @free_fn: (scope async) (nullable): data will be freed with this function
 * @data: (transfer full): set @value to track this pointer
 *
 * Set value to be a ref-counted area of memory with a free function.
 */
void
vips_value_set_area(GValue *value, VipsCallbackFn free_fn, void *data)
{
	VipsArea *area;

	area = vips_area_new(free_fn, data);
	g_value_init(value, VIPS_TYPE_AREA);
	g_value_set_boxed(value, area);
	vips_area_unref(area);
}

/**
 * vips_value_get_area:
 * @value: get from this value
 * @length: (out) (optional): optionally return length here
 *
 * Get the pointer from an area. Don't touch count (area is static).
 *
 * Returns: (transfer none): The pointer held by @value.
 */
void *
vips_value_get_area(const GValue *value, size_t *length)
{
	VipsArea *area;

	area = g_value_get_boxed(value);

	return vips_area_get_data(area, length, NULL, NULL, NULL);
}

/**
 * vips_value_get_save_string:
 * @value: GValue to get from
 *
 * Get the C string held internally by the GValue.
 *
 * Returns: (transfer none): The C string held by @value.
 */
const char *
vips_value_get_save_string(const GValue *value)
{
	return (char *) g_value_get_boxed(value);
}

/**
 * vips_value_set_save_string:
 * @value: (out): GValue to set
 * @str: C string to copy into the GValue
 *
 * Copies the C string into @value.
 *
 * @str should be a valid utf-8 string.
 */
void
vips_value_set_save_string(GValue *value, const char *str)
{
	g_assert(G_VALUE_TYPE(value) == VIPS_TYPE_SAVE_STRING);

	if (!g_utf8_validate(str, -1, NULL))
		str = "<invalid utf-8 string>";

	g_value_set_boxed(value, str);
}

/**
 * vips_value_set_save_stringf:
 * @value: (out): GValue to set
 * @fmt: `printf()`-style format string
 * @...: arguments to `printf()`-formatted @fmt
 *
 * Generates a string and copies it into @value.
 */
void
vips_value_set_save_stringf(GValue *value, const char *fmt, ...)
{
	va_list ap;
	char *str;

	g_assert(G_VALUE_TYPE(value) == VIPS_TYPE_SAVE_STRING);

	va_start(ap, fmt);
	str = g_strdup_vprintf(fmt, ap);
	va_end(ap);
	vips_value_set_save_string(value, str);
	g_free(str);
}

/**
 * vips_value_get_ref_string:
 * @value: [struct@GObject.Value] to get from
 * @length: (out) (optional): return length here, optionally
 *
 * Get the C string held internally by the [struct@GObject.Value].
 *
 * Returns: (transfer none): The C string held by @value.
 */
const char *
vips_value_get_ref_string(const GValue *value, size_t *length)
{
	return vips_value_get_area(value, length);
}

/**
 * vips_value_set_ref_string:
 * @value: (out): [struct@GObject.Value] to set
 * @str: C string to copy into the GValue
 *
 * Copies the C string @str into @value.
 *
 * vips_ref_string are immutable C strings that are copied between images by
 * copying reference-counted pointers, making them much more efficient than
 * regular [struct@GObject.Value] strings.
 *
 * @str should be a valid utf-8 string.
 */
void
vips_value_set_ref_string(GValue *value, const char *str)
{
	VipsRefString *ref_str;

	g_assert(G_VALUE_TYPE(value) == VIPS_TYPE_REF_STRING);

	ref_str = vips_ref_string_new(str);
	g_value_set_boxed(value, ref_str);
	vips_area_unref(VIPS_AREA(ref_str));
}

/**
 * vips_value_set_blob:
 * @value: (out): GValue to set
 * @free_fn: (scope async) (nullable): free function for @data
 * @data: (array length=length) (element-type guint8) (transfer full): pointer to area of
 * memory
 * @length: length of memory area
 *
 * Sets @value to hold a @data. When @value is freed, @data will be
 * freed with @free_fn. @value also holds a note of the size of the memory
 * area.
 *
 * blobs are things like ICC profiles or EXIF data. They are relocatable, and
 * are saved to VIPS files for you coded as base64 inside the XML. They are
 * copied by copying reference-counted pointers.
 *
 * ::: seealso
 *     [func@value_get_blob]
 */
void
vips_value_set_blob(GValue *value,
	VipsCallbackFn free_fn, const void *data, size_t length)
{
	VipsBlob *blob;

	g_assert(G_VALUE_TYPE(value) == VIPS_TYPE_BLOB);

	blob = vips_blob_new(free_fn, data, length);
	g_value_set_boxed(value, blob);
	vips_area_unref(VIPS_AREA(blob));
}

/**
 * vips_value_set_blob_free:
 * @value: GValue to set
 * @data: (transfer full) (array length=length) (element-type guint8) : pointer to area of
 * memory
 * @length: length of memory area
 *
 * Just like [func@value_set_blob], but when
 * @value is freed, @data will be
 * freed with [func@GLib.free].
 *
 * This can be easier to call for language bindings.
 *
 * ::: seealso
 *     [func@value_set_blob]
 */
void
vips_value_set_blob_free(GValue *value, void *data, size_t length)
{
	VipsBlob *blob;

	g_assert(G_VALUE_TYPE(value) == VIPS_TYPE_BLOB);

	blob = vips_blob_new((VipsCallbackFn) vips_area_free_cb, data, length);
	g_value_set_boxed(value, blob);
	vips_area_unref(VIPS_AREA(blob));
}

/**
 * vips_value_get_blob:
 * @value: GValue to set
 * @length: (out) (optional): optionally return length of memory area
 *
 * Returns the data pointer from a blob. Optionally returns the length too.
 *
 * blobs are things like ICC profiles or EXIF data. They are relocatable, and
 * are saved to VIPS files for you coded as base64 inside the XML. They are
 * copied by copying reference-counted pointers.
 *
 * ::: seealso
 *     [func@value_set_blob]
 *
 * Returns: (transfer none) (array length=length) (element-type guint8): The pointer held
 * by @value.
 */
void *
vips_value_get_blob(const GValue *value, size_t *length)
{
	return vips_value_get_area(value, length);
}

/**
 * vips_value_set_array:
 * @value: (out): [struct@GObject.Value] to set
 * @n: number of elements
 * @type: the type of each element
 * @sizeof_type: the sizeof each element
 *
 * Set @value to be an array of things.
 *
 * This allocates memory but does not
 * initialise the contents: get the pointer and write instead.
 */
void
vips_value_set_array(GValue *value, int n, GType type, size_t sizeof_type)
{
	VipsArea *area;

	area = vips_area_new_array(type, sizeof_type, n);
	g_value_set_boxed(value, area);
	vips_area_unref(area);
}

/**
 * vips_value_get_array:
 * @value: [struct@GObject.Value] to get from
 * @n: (out) (optional): return the number of elements here, optionally
 * @type: (out) (optional): return the type of each element here, optionally
 * @sizeof_type: (out) (optional): return the sizeof each element here, optionally
 *
 * Return the pointer to the array held by @value.
 * Optionally return the other properties of the array in @n, @type,
 * @sizeof_type.
 *
 * ::: seealso
 *     [func@value_set_array].
 *
 * Returns: (transfer none): The array address.
 */
void *
vips_value_get_array(const GValue *value,
	int *n, GType *type, size_t *sizeof_type)
{
	VipsArea *area;

	/* Can't check value type, because we may get called from
	 * vips_*_get_type().
	 */

	if (!(area = g_value_get_boxed(value)))
		return NULL;
	if (n)
		*n = area->n;
	if (type)
		*type = area->type;
	if (sizeof_type)
		*sizeof_type = area->sizeof_type;

	return area->data;
}

/**
 * vips_value_get_array_int:
 * @value: [struct@GObject.Value] to get from
 * @n: (out) (optional): return the number of elements here, optionally
 *
 * Return the start of the array of ints held by @value.
 * optionally return the number of elements in @n.
 *
 * ::: seealso
 *     [ctor@ArrayInt.new].
 *
 * Returns: (transfer none) (array length=n): The array address.
 */
int *
vips_value_get_array_int(const GValue *value, int *n)
{
	return vips_value_get_array(value, n, NULL, NULL);
}

/**
 * vips_value_set_array_int:
 * @value: [struct@GObject.Value] to get from
 * @array: (array length=n) (allow-none): array of ints
 * @n: the number of elements
 *
 * Set @value to hold a copy of @array. Pass in the array length in @n.
 *
 * ::: seealso
 *     [method@ArrayInt.get].
 */
void
vips_value_set_array_int(GValue *value, const int *array, int n)
{
	vips_value_set_array(value, n, G_TYPE_INT, sizeof(int));

	if (array) {
		int *array_copy;

		array_copy = vips_value_get_array_int(value, NULL);
		memcpy(array_copy, array, n * sizeof(int));
	}
}

/**
 * vips_value_get_array_double:
 * @value: [struct@GObject.Value] to get from
 * @n: (out) (optional): return the number of elements here, optionally
 *
 * Return the start of the array of doubles held by @value.
 * optionally return the number of elements in @n.
 *
 * ::: seealso
 *     [ctor@ArrayDouble.new].
 *
 * Returns: (transfer none) (array length=n): The array address.
 */
double *
vips_value_get_array_double(const GValue *value, int *n)
{
	return vips_value_get_array(value, n, NULL, NULL);
}

/**
 * vips_value_set_array_double:
 * @value: [struct@GObject.Value] to get from
 * @array: (array length=n) (allow-none): array of doubles
 * @n: the number of elements
 *
 * Set @value to hold a copy of @array. Pass in the array length in @n.
 *
 * ::: seealso
 *     [method@ArrayDouble.get].
 */
void
vips_value_set_array_double(GValue *value, const double *array, int n)
{
	vips_value_set_array(value, n, G_TYPE_DOUBLE, sizeof(double));

	if (array) {
		double *array_copy;

		array_copy = vips_value_get_array_double(value, NULL);
		memcpy(array_copy, array, n * sizeof(double));
	}
}

/**
 * vips_value_get_array_image:
 * @value: [struct@GObject.Value] to get from
 * @n: (out) (optional): return the number of elements here, optionally
 *
 * Return the start of the array of images held by @value.
 * optionally return the number of elements in @n.
 *
 * ::: seealso
 *     [func@value_set_array_image].
 *
 * Returns: (transfer none) (array length=n): The array address.
 */
VipsImage **
vips_value_get_array_image(const GValue *value, int *n)
{
	return vips_value_get_array(value, n, NULL, NULL);
}

/**
 * vips_value_set_array_image:
 * @value: [struct@GObject.Value] to get from
 * @n: the number of elements
 *
 * Set @value to hold an array of images. Pass in the array length in @n.
 *
 * ::: seealso
 *     [method@ArrayImage.get].
 */
void
vips_value_set_array_image(GValue *value, int n)
{
	VipsArea *area;

	area = vips_area_new_array_object(n);
	area->type = VIPS_TYPE_IMAGE;
	g_value_set_boxed(value, area);
	vips_area_unref(area);
}

/**
 * vips_value_get_array_object: (skip)
 * @value: [struct@GObject.Value] to get from
 * @n: (out) (optional): return the number of elements here, optionally
 *
 * Return the start of the array of [class@GObject.Object] held by @value.
 * Optionally return the number of elements in @n.
 *
 * ::: seealso
 *     [ctor@Area.new_array_object].
 *
 * Returns: (transfer none) (array length=n): The array address.
 */
GObject **
vips_value_get_array_object(const GValue *value, int *n)
{
	return vips_value_get_array(value, n, NULL, NULL);
}

/**
 * vips_value_set_array_object:
 * @value: (out): [struct@GObject.Value] to set
 * @n: the number of elements
 *
 * Set @value to hold an array of [class@GObject.Object]. Pass in the array length in @n.
 *
 * ::: seealso
 *     [func@value_get_array_object].
 */
void
vips_value_set_array_object(GValue *value, int n)
{
	VipsArea *area;

	area = vips_area_new_array_object(n);
	g_value_set_boxed(value, area);
	vips_area_unref(area);
}

/* Make the types we need for basic functioning. Called from vips_init().
 */
void
vips__meta_init_types(void)
{
	(void) vips_thing_get_type();
	(void) vips_save_string_get_type();
	(void) vips_area_get_type();
	(void) vips_ref_string_get_type();
	(void) vips_blob_get_type();
	(void) vips_array_int_get_type();
	(void) vips_array_double_get_type();
	(void) vips_array_image_get_type();

	/* Register transform functions to convert between an array of
	 * integers and doubles. This is set here to prevent a recursive
	 * call chain.
	 */
	g_value_register_transform_func(VIPS_TYPE_ARRAY_INT,
		VIPS_TYPE_ARRAY_DOUBLE, transform_array_int_array_double);
	g_value_register_transform_func(VIPS_TYPE_ARRAY_DOUBLE,
		VIPS_TYPE_ARRAY_INT, transform_array_double_array_int);
}
