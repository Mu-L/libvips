/* save to fits
 *
 * 2/12/11
 * 	- wrap a class around the fits writer
 * 2/7/14
 * 	- cache the image before write so we are sequential
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
#define DEBUG_VERBOSE
#define DEBUG
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/
#include <glib/gi18n-lib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vips/vips.h>

#ifdef HAVE_CFITSIO

#include "pforeign.h"

typedef struct _VipsForeignSaveFits {
	VipsForeignSave parent_object;

	/* Filename for save.
	 */
	char *filename;

} VipsForeignSaveFits;

typedef VipsForeignSaveClass VipsForeignSaveFitsClass;

G_DEFINE_TYPE(VipsForeignSaveFits, vips_foreign_save_fits,
	VIPS_TYPE_FOREIGN_SAVE);

static int
vips_foreign_save_fits_build(VipsObject *object)
{
	VipsForeignSave *save = (VipsForeignSave *) object;
	VipsForeignSaveFits *fits = (VipsForeignSaveFits *) object;
	VipsImage **t = (VipsImage **)
		vips_object_local_array(VIPS_OBJECT(fits), 2);

	if (VIPS_OBJECT_CLASS(vips_foreign_save_fits_parent_class)->build(object))
		return -1;

	/* FITS is written bottom-to-top, so we must flip.
	 *
	 * But all vips readers must work top-to-bottom (or vips_copy()'s seq
	 * hint won't work) so we must cache the input image.
	 *
	 * We cache to RAM, but perhaps we should use something like
	 * vips_get_disc_threshold() and copy to a tempfile.
	 */
	t[0] = vips_image_new_memory();
	if (vips_image_write(save->ready, t[0]) ||
		vips_flip(t[0], &t[1], VIPS_DIRECTION_VERTICAL, NULL) ||
		vips__fits_write(t[1], fits->filename))
		return -1;

	return 0;
}

/* Save a bit of typing.
 */
#define UC VIPS_FORMAT_UCHAR
#define C VIPS_FORMAT_CHAR
#define US VIPS_FORMAT_USHORT
#define S VIPS_FORMAT_SHORT
#define UI VIPS_FORMAT_UINT
#define I VIPS_FORMAT_INT
#define F VIPS_FORMAT_FLOAT
#define X VIPS_FORMAT_COMPLEX
#define D VIPS_FORMAT_DOUBLE
#define DX VIPS_FORMAT_DPCOMPLEX

static VipsBandFormat bandfmt_fits[10] = {
	/* Band format:  UC  C   US  S   UI  I   F  X  D  DX */
	/* Promotion: */ UC, UC, US, US, UI, UI, F, X, D, DX
};

static void
vips_foreign_save_fits_class_init(VipsForeignSaveFitsClass *class)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(class);
	VipsObjectClass *object_class = (VipsObjectClass *) class;
	VipsOperationClass *operation_class = VIPS_OPERATION_CLASS(class);
	VipsForeignClass *foreign_class = (VipsForeignClass *) class;
	VipsForeignSaveClass *save_class = (VipsForeignSaveClass *) class;

	gobject_class->set_property = vips_object_set_property;
	gobject_class->get_property = vips_object_get_property;

	object_class->nickname = "fitssave";
	object_class->description = _("save image to fits file");
	object_class->build = vips_foreign_save_fits_build;

	/* cfitsio has not been fuzzed, so should not be used with
	 * untrusted input unless you are very careful.
	 */
	operation_class->flags |= VIPS_OPERATION_UNTRUSTED;

	foreign_class->suffs = vips__fits_suffs;

	save_class->saveable = VIPS_SAVEABLE_ANY;
	save_class->format_table = bandfmt_fits;

	VIPS_ARG_STRING(class, "filename", 1,
		_("Filename"),
		_("Filename to save to"),
		VIPS_ARGUMENT_REQUIRED_INPUT,
		G_STRUCT_OFFSET(VipsForeignSaveFits, filename),
		NULL);
}

static void
vips_foreign_save_fits_init(VipsForeignSaveFits *fits)
{
}

#endif /*HAVE_CFITSIO*/

/**
 * vips_fitssave: (method)
 * @in: image to save
 * @filename: file to write to
 * @...: %NULL-terminated list of optional named arguments
 *
 * Write a VIPS image to a file in FITS format.
 *
 * ::: seealso
 *     [method@Image.write_to_file].
 *
 * Returns: 0 on success, -1 on error.
 */
int
vips_fitssave(VipsImage *in, const char *filename, ...)
{
	va_list ap;
	int result;

	va_start(ap, filename);
	result = vips_call_split("fitssave", ap, in, filename);
	va_end(ap);

	return result;
}
