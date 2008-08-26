/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*  GMime
 *  Copyright (C) 2000-2008 Jeffrey Stedfast
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2.1
 *  of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free
 *  Software Foundation, 51 Franklin Street, Fifth Floor, Boston, MA
 *  02110-1301, USA.
 */


#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

#include "gmime-part.h"
#include "gmime-utils.h"
#include "gmime-stream-mem.h"
#include "gmime-stream-null.h"
#include "gmime-stream-filter.h"
#include "gmime-filter-basic.h"
#include "gmime-filter-crlf.h"
#include "gmime-filter-md5.h"

#define d(x)


/**
 * SECTION: gmime-part
 * @title: GMimePart
 * @short_description: MIME parts
 * @see_also:
 *
 * A #GMimePart represents any MIME leaf part (meaning it has no
 * sub-parts).
 **/

/* GObject class methods */
static void g_mime_part_class_init (GMimePartClass *klass);
static void g_mime_part_init (GMimePart *mime_part, GMimePartClass *klass);
static void g_mime_part_finalize (GObject *object);

/* GMimeObject class methods */
static void mime_part_prepend_header (GMimeObject *object, const char *header, const char *value);
static void mime_part_append_header (GMimeObject *object, const char *header, const char *value);
static void mime_part_set_header (GMimeObject *object, const char *header, const char *value);
static const char *mime_part_get_header (GMimeObject *object, const char *header);
static gboolean mime_part_remove_header (GMimeObject *object, const char *header);
static char *mime_part_get_headers (GMimeObject *object);
static ssize_t mime_part_write_to_stream (GMimeObject *object, GMimeStream *stream);


static GMimeObjectClass *parent_class = NULL;


GType
g_mime_part_get_type (void)
{
	static GType type = 0;
	
	if (!type) {
		static const GTypeInfo info = {
			sizeof (GMimePartClass),
			NULL, /* base_class_init */
			NULL, /* base_class_finalize */
			(GClassInitFunc) g_mime_part_class_init,
			NULL, /* class_finalize */
			NULL, /* class_data */
			sizeof (GMimePart),
			0,    /* n_preallocs */
			(GInstanceInitFunc) g_mime_part_init,
		};
		
		type = g_type_register_static (GMIME_TYPE_OBJECT, "GMimePart", &info, 0);
	}
	
	return type;
}


static void
g_mime_part_class_init (GMimePartClass *klass)
{
	GMimeObjectClass *object_class = GMIME_OBJECT_CLASS (klass);
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
	
	parent_class = g_type_class_ref (GMIME_TYPE_OBJECT);
	
	gobject_class->finalize = g_mime_part_finalize;
	
	object_class->prepend_header = mime_part_prepend_header;
	object_class->append_header = mime_part_append_header;
	object_class->remove_header = mime_part_remove_header;
	object_class->set_header = mime_part_set_header;
	object_class->get_header = mime_part_get_header;
	object_class->get_headers = mime_part_get_headers;
	object_class->write_to_stream = mime_part_write_to_stream;
}

static void
g_mime_part_init (GMimePart *mime_part, GMimePartClass *klass)
{
	mime_part->encoding = GMIME_CONTENT_ENCODING_DEFAULT;
	mime_part->content_description = NULL;
	mime_part->content_location = NULL;
	mime_part->content_md5 = NULL;
	mime_part->content = NULL;
}

static void
g_mime_part_finalize (GObject *object)
{
	GMimePart *mime_part = (GMimePart *) object;
	
	g_free (mime_part->content_description);
	g_free (mime_part->content_location);
	g_free (mime_part->content_md5);
	
	if (mime_part->content)
		g_object_unref (mime_part->content);
	
	G_OBJECT_CLASS (parent_class)->finalize (object);
}


enum {
	HEADER_CONTENT_TRANSFER_ENCODING,
	HEADER_CONTENT_DESCRIPTION,
	HEADER_CONTENT_LOCATION,
	HEADER_CONTENT_MD5,
	HEADER_UNKNOWN
};

static const char *content_headers[] = {
	"Content-Transfer-Encoding",
	"Content-Description",
	"Content-Location",
	"Content-Md5",
};


static gboolean
process_header (GMimeObject *object, const char *header, const char *value)
{
	GMimePart *mime_part = (GMimePart *) object;
	char *text;
	int i;
	
	for (i = 0; i < G_N_ELEMENTS (content_headers); i++) {
		if (!g_ascii_strcasecmp (content_headers[i], header))
			break;
	}
	
	switch (i) {
	case HEADER_CONTENT_TRANSFER_ENCODING:
		text = g_alloca (strlen (value) + 1);
		strcpy (text, value);
		g_strstrip (text);
		mime_part->encoding = g_mime_content_encoding_from_string (text);
		break;
	case HEADER_CONTENT_DESCRIPTION:
		/* FIXME: we should decode this */
		g_free (mime_part->content_description);
		mime_part->content_description = g_strstrip (g_strdup (value));
		break;
	case HEADER_CONTENT_LOCATION:
		g_free (mime_part->content_location);
		mime_part->content_location = g_strstrip (g_strdup (value));
		break;
	case HEADER_CONTENT_MD5:
		g_free (mime_part->content_md5);
		mime_part->content_md5 = g_strstrip (g_strdup (value));
		break;
	default:
		return FALSE;
		break;
	}
	
	return TRUE;
}

static void
mime_part_prepend_header (GMimeObject *object, const char *header, const char *value)
{
	/* Make sure that the header is a Content-* header, else it
           doesn't belong on a mime part */
	if (g_ascii_strncasecmp ("Content-", header, 8) != 0)
		return;
	
	if (!process_header (object, header, value))
		GMIME_OBJECT_CLASS (parent_class)->prepend_header (object, header, value);
	else
		g_mime_header_list_prepend (object->headers, header, value);
}

static void
mime_part_append_header (GMimeObject *object, const char *header, const char *value)
{
	/* Make sure that the header is a Content-* header, else it
           doesn't belong on a mime part */
	if (g_ascii_strncasecmp ("Content-", header, 8) != 0)
		return;
	
	if (!process_header (object, header, value))
		GMIME_OBJECT_CLASS (parent_class)->append_header (object, header, value);
	else
		g_mime_header_list_append (object->headers, header, value);
}

static void
mime_part_set_header (GMimeObject *object, const char *header, const char *value)
{
	/* Make sure that the header is a Content-* header, else it
           doesn't belong on a mime part */
	if (g_ascii_strncasecmp ("Content-", header, 8) != 0)
		return;
	
	if (!process_header (object, header, value))
		GMIME_OBJECT_CLASS (parent_class)->set_header (object, header, value);
	else
		g_mime_header_list_set (object->headers, header, value);
}

static const char *
mime_part_get_header (GMimeObject *object, const char *header)
{
	/* Make sure that the header is a Content-* header, else it
           doesn't belong on a mime part */
	if (!g_ascii_strncasecmp ("Content-", header, 8))
		return GMIME_OBJECT_CLASS (parent_class)->get_header (object, header);
	else
		return NULL;
}

static gboolean
mime_part_remove_header (GMimeObject *object, const char *header)
{
	GMimePart *mime_part = (GMimePart *) object;
	int i;
	
	/* Make sure that the header is a Content-* header, else it
	   doesn't belong on a mime part */
	if (g_ascii_strncasecmp ("Content-", header, 8) != 0)
		return FALSE;
	
	for (i = 0; i < G_N_ELEMENTS (content_headers); i++) {
		if (!g_ascii_strcasecmp (content_headers[i], header))
			break;
	}
	
	switch (i) {
	case HEADER_CONTENT_TRANSFER_ENCODING:
		mime_part->encoding = GMIME_CONTENT_ENCODING_DEFAULT;
		break;
	case HEADER_CONTENT_DESCRIPTION:
		/* FIXME: we should decode this */
		g_free (mime_part->content_description);
		mime_part->content_description = NULL;
		break;
	case HEADER_CONTENT_LOCATION:
		g_free (mime_part->content_location);
		mime_part->content_location = NULL;
		break;
	case HEADER_CONTENT_MD5:
		g_free (mime_part->content_md5);
		mime_part->content_md5 = NULL;
		break;
	default:
		break;
	}
	
	return GMIME_OBJECT_CLASS (parent_class)->remove_header (object, header);
}

static char *
mime_part_get_headers (GMimeObject *object)
{
	return GMIME_OBJECT_CLASS (parent_class)->get_headers (object);
}

static ssize_t
write_content (GMimePart *part, GMimeStream *stream)
{
	ssize_t nwritten, total = 0;
	
	if (!part->content)
		return 0;
	
	/* Evil Genius's "slight" optimization: Since GMimeDataWrapper::write_to_stream()
	 * decodes its content stream to the raw format, we can cheat by requesting its
	 * content stream and not doing any encoding on the data if the source and
	 * destination encodings are identical.
	 */
	
	if (part->encoding != g_mime_data_wrapper_get_encoding (part->content)) {
		GMimeStream *filtered_stream;
		const char *filename;
		GMimeFilter *filter;
		
		switch (part->encoding) {
		case GMIME_CONTENT_ENCODING_UUENCODE:
			filename = g_mime_part_get_filename (part);
			nwritten = g_mime_stream_printf (stream, "begin 0644 %s\n",
							 filename ? filename : "unknown");
			if (nwritten == -1)
				return -1;
			
			total += nwritten;
			
			/* fall thru... */
		case GMIME_CONTENT_ENCODING_QUOTEDPRINTABLE:
		case GMIME_CONTENT_ENCODING_BASE64:
			filtered_stream = g_mime_stream_filter_new (stream);
			filter = g_mime_filter_basic_new (part->encoding, TRUE);
			g_mime_stream_filter_add (GMIME_STREAM_FILTER (filtered_stream), filter);
			g_object_unref (filter);
			break;
		default:
			filtered_stream = stream;
			g_object_ref (stream);
			break;
		}
		
		nwritten = g_mime_data_wrapper_write_to_stream (part->content, filtered_stream);
		g_mime_stream_flush (filtered_stream);
		g_object_unref (filtered_stream);
		
		if (nwritten == -1)
			return -1;
		
		total += nwritten;
		
		if (part->encoding == GMIME_CONTENT_ENCODING_UUENCODE) {
			/* FIXME: get rid of this special-case x-uuencode crap */
			nwritten = g_mime_stream_write (stream, "end\n", 4);
			if (nwritten == -1)
				return -1;
			
			total += nwritten;
		}
	} else {
		GMimeStream *content_stream;
		
		content_stream = g_mime_data_wrapper_get_stream (part->content);
		g_mime_stream_reset (content_stream);
		nwritten = g_mime_stream_write_to_stream (content_stream, stream);
		
		if (nwritten == -1)
			return -1;
		
		total += nwritten;
	}
	
	return total;
}

static ssize_t
mime_part_write_to_stream (GMimeObject *object, GMimeStream *stream)
{
	GMimePart *mime_part = (GMimePart *) object;
	ssize_t nwritten, total = 0;
	
	/* write the content headers */
	if ((nwritten = g_mime_header_list_write_to_stream (object->headers, stream)) == -1)
		return -1;
	
	total += nwritten;
	
	/* terminate the headers */
	if (g_mime_stream_write (stream, "\n", 1) == -1)
		return -1;
	
	total++;
	
	if ((nwritten = write_content (mime_part, stream)) == -1)
		return -1;
	
	total += nwritten;
	
	return total;
}


/**
 * g_mime_part_new:
 *
 * Creates a new MIME Part object with a default content-type of
 * text/plain.
 *
 * Returns: an empty MIME Part object with a default content-type of
 * text/plain.
 **/
GMimePart *
g_mime_part_new (void)
{
	GMimePart *mime_part;
	GMimeContentType *type;
	
	mime_part = g_object_new (GMIME_TYPE_PART, NULL);
	
	type = g_mime_content_type_new ("text", "plain");
	g_mime_object_set_content_type (GMIME_OBJECT (mime_part), type);
	
	return mime_part;
}


/**
 * g_mime_part_new_with_type:
 * @type: content-type
 * @subtype: content-subtype
 *
 * Creates a new MIME Part with a sepcified type.
 *
 * Returns: an empty MIME Part object with the specified content-type.
 **/
GMimePart *
g_mime_part_new_with_type (const char *type, const char *subtype)
{
	GMimePart *mime_part;
	GMimeContentType *content_type;
	
	mime_part = g_object_new (GMIME_TYPE_PART, NULL);
	
	content_type = g_mime_content_type_new (type, subtype);
	g_mime_object_set_content_type (GMIME_OBJECT (mime_part), content_type);
	
	return mime_part;
}


/**
 * g_mime_part_set_content_header:
 * @mime_part: mime part
 * @header: header name
 * @value: header value
 *
 * Set an arbitrary MIME content header.
 **/
void
g_mime_part_set_content_header (GMimePart *mime_part, const char *header, const char *value)
{
	g_return_if_fail (GMIME_IS_PART (mime_part));
	g_return_if_fail (header != NULL);
	
	g_mime_object_set_header (GMIME_OBJECT (mime_part), header, value);
}


/**
 * g_mime_part_get_content_header:
 * @mime_part: mime part
 * @header: header name
 *
 * Gets the value of the requested header if it exists, or %NULL
 * otherwise.
 *
 * Returns: the value of the content header @header.
 **/
const char *
g_mime_part_get_content_header (GMimePart *mime_part, const char *header)
{
	g_return_val_if_fail (GMIME_IS_PART (mime_part), NULL);
	g_return_val_if_fail (header != NULL, NULL);
	
	return g_mime_object_get_header (GMIME_OBJECT (mime_part), header);
}


/**
 * g_mime_part_set_content_description:
 * @mime_part: Mime part
 * @description: content description
 *
 * Set the content description for the specified mime part.
 **/
void
g_mime_part_set_content_description (GMimePart *mime_part, const char *description)
{
	g_return_if_fail (GMIME_IS_PART (mime_part));
	
	if (mime_part->content_description)
		g_free (mime_part->content_description);
	
	mime_part->content_description = g_strdup (description);
	g_mime_header_list_set (GMIME_OBJECT (mime_part)->headers, "Content-Description", description);
}


/**
 * g_mime_part_get_content_description:
 * @mime_part: Mime part
 *
 * Gets the value of the Content-Description for the specified mime
 * part if it exists or %NULL otherwise.
 *
 * Returns: the content description for the specified mime part.
 **/
const char *
g_mime_part_get_content_description (const GMimePart *mime_part)
{
	g_return_val_if_fail (GMIME_IS_PART (mime_part), NULL);
	
	return mime_part->content_description;
}


/**
 * g_mime_part_set_content_id:
 * @mime_part: Mime part
 * @content_id: content id
 *
 * Set the content id for the specified mime part.
 **/
void
g_mime_part_set_content_id (GMimePart *mime_part, const char *content_id)
{
	g_return_if_fail (GMIME_IS_PART (mime_part));
	
	g_mime_object_set_content_id (GMIME_OBJECT (mime_part), content_id);
}


/**
 * g_mime_part_get_content_id:
 * @mime_part: Mime part
 *
 * Gets the content-id of the specified mime part if it exists, or
 * %NULL otherwise.
 *
 * Returns: the content id for the specified mime part.
 **/
const char *
g_mime_part_get_content_id (GMimePart *mime_part)
{
	g_return_val_if_fail (GMIME_IS_PART (mime_part), NULL);
	
	return g_mime_object_get_content_id (GMIME_OBJECT (mime_part));
}


/**
 * g_mime_part_set_content_md5:
 * @mime_part: Mime part
 * @content_md5: content md5 or %NULL to generate the md5 digest.
 *
 * Set the content md5 for the specified mime part.
 **/
void
g_mime_part_set_content_md5 (GMimePart *mime_part, const char *content_md5)
{
	unsigned char digest[16], b64digest[32];
	const GMimeContentType *content_type;
	GMimeStreamFilter *filtered_stream;
	GMimeFilter *md5_filter;
	GMimeStream *stream;
	guint32 save = 0;
	int state = 0;
	size_t len;
	
	g_return_if_fail (GMIME_IS_PART (mime_part));
	
	if (mime_part->content_md5)
		g_free (mime_part->content_md5);
	
	if (!content_md5) {
		/* compute a md5sum */
		stream = g_mime_stream_null_new ();
		filtered_stream = (GMimeStreamFilter *) g_mime_stream_filter_new (stream);
		g_object_unref (stream);
		
		content_type = g_mime_object_get_content_type ((GMimeObject *) mime_part);
		if (g_mime_content_type_is_type (content_type, "text", "*")) {
			GMimeFilter *crlf_filter;
			
			crlf_filter = g_mime_filter_crlf_new (TRUE, FALSE);
			g_mime_stream_filter_add (filtered_stream, crlf_filter);
			g_object_unref (crlf_filter);
		}
		
		md5_filter = g_mime_filter_md5_new ();
		g_mime_stream_filter_add (filtered_stream, md5_filter);
		
		stream = (GMimeStream *) filtered_stream;
		g_mime_data_wrapper_write_to_stream (mime_part->content, stream);
		g_object_unref (stream);
		
		memset (digest, 0, 16);
		g_mime_filter_md5_get_digest ((GMimeFilterMd5 *) md5_filter, digest);
		g_object_unref (md5_filter);
		
		len = g_mime_encoding_base64_encode_close (digest, 16, b64digest, &state, &save);
		b64digest[len] = '\0';
		g_strstrip ((char *) b64digest);
		
		content_md5 = (const char *) b64digest;
	}
	
	mime_part->content_md5 = g_strdup (content_md5);
	g_mime_header_list_set (GMIME_OBJECT (mime_part)->headers, "Content-Md5", content_md5);
}


/**
 * g_mime_part_verify_content_md5:
 * @mime_part: Mime part
 *
 * Verify the content md5 for the specified mime part.
 *
 * Returns: %TRUE if the md5 is valid or %FALSE otherwise. Note: will
 * return %FALSE if the mime part does not contain a Content-MD5.
 **/
gboolean
g_mime_part_verify_content_md5 (GMimePart *mime_part)
{
	unsigned char digest[16], b64digest[32];
	const GMimeContentType *content_type;
	GMimeStreamFilter *filtered_stream;
	GMimeFilter *md5_filter;
	GMimeStream *stream;
	guint32 save = 0;
	int state = 0;
	size_t len;
	
	g_return_val_if_fail (GMIME_IS_PART (mime_part), FALSE);
	g_return_val_if_fail (mime_part->content != NULL, FALSE);
	
	if (!mime_part->content_md5)
		return FALSE;
	
	stream = g_mime_stream_null_new ();
	filtered_stream = (GMimeStreamFilter *) g_mime_stream_filter_new (stream);
	g_object_unref (stream);
	
	content_type = g_mime_object_get_content_type ((GMimeObject *) mime_part);
	if (g_mime_content_type_is_type (content_type, "text", "*")) {
		GMimeFilter *crlf_filter;
		
		crlf_filter = g_mime_filter_crlf_new (TRUE, FALSE);
		g_mime_stream_filter_add (filtered_stream, crlf_filter);
		g_object_unref (crlf_filter);
	}
	
	md5_filter = g_mime_filter_md5_new ();
	g_mime_stream_filter_add (filtered_stream, md5_filter);
	
	stream = (GMimeStream *) filtered_stream;
	g_mime_data_wrapper_write_to_stream (mime_part->content, stream);
	g_object_unref (stream);
	
	memset (digest, 0, 16);
	g_mime_filter_md5_get_digest ((GMimeFilterMd5 *) md5_filter, digest);
	g_object_unref (md5_filter);
	
	len = g_mime_encoding_base64_encode_close (digest, 16, b64digest, &state, &save);
	b64digest[len] = '\0';
	g_strstrip ((char *) b64digest);
	
	return !strcmp ((char *) b64digest, mime_part->content_md5);
}


/**
 * g_mime_part_get_content_md5:
 * @mime_part: Mime part
 *
 * Gets the md5sum contained in the Content-Md5 header of the
 * specified mime part if it exists, or %NULL otherwise.
 *
 * Returns: the content md5 for the specified mime part.
 **/
const char *
g_mime_part_get_content_md5 (GMimePart *mime_part)
{
	g_return_val_if_fail (GMIME_IS_PART (mime_part), NULL);
	
	return mime_part->content_md5;
}


/**
 * g_mime_part_set_content_location:
 * @mime_part: a #GMimePart object
 * @content_location: content location
 *
 * Set the content location for the specified mime part.
 **/
void
g_mime_part_set_content_location (GMimePart *mime_part, const char *content_location)
{
	g_return_if_fail (GMIME_IS_PART (mime_part));
	
	if (mime_part->content_location)
		g_free (mime_part->content_location);
	
	mime_part->content_location = g_strdup (content_location);
	g_mime_header_list_set (GMIME_OBJECT (mime_part)->headers, "Content-Location", content_location);
}


/**
 * g_mime_part_get_content_location:
 * @mime_part: a #GMimePart object
 *
 * Gets the value of the Content-Location header if it exists, or
 * %NULL otherwise.
 *
 * Returns: the content location for the specified mime part.
 **/
const char *
g_mime_part_get_content_location (GMimePart *mime_part)
{
	g_return_val_if_fail (GMIME_IS_PART (mime_part), NULL);
	
	return mime_part->content_location;
}


/**
 * g_mime_part_set_content_encoding:
 * @mime_part: a #GMimePart
 * @encoding: a #GMimeContentEncoding
 *
 * Set the content encoding for the specified mime part.
 **/
void
g_mime_part_set_content_encoding (GMimePart *mime_part, GMimeContentEncoding encoding)
{
	g_return_if_fail (GMIME_IS_PART (mime_part));
	
	mime_part->encoding = encoding;
	g_mime_header_list_set (GMIME_OBJECT (mime_part)->headers, "Content-Transfer-Encoding",
				g_mime_content_encoding_to_string (encoding));
}


/**
 * g_mime_part_get_content_encoding:
 * @mime_part: a #GMimePart
 *
 * Gets the content encoding of the mime part.
 *
 * Returns: the content encoding for the specified mime part.
 **/
GMimeContentEncoding
g_mime_part_get_content_encoding (GMimePart *mime_part)
{
	g_return_val_if_fail (GMIME_IS_PART (mime_part), GMIME_CONTENT_ENCODING_DEFAULT);
	
	return mime_part->encoding;
}


/**
 * g_mime_part_set_filename:
 * @mime_part: Mime part
 * @filename: the filename of the Mime Part's content
 *
 * Sets the "filename" parameter on the Content-Disposition and also sets the
 * "name" parameter on the Content-Type.
 **/
void
g_mime_part_set_filename (GMimePart *mime_part, const char *filename)
{
	GMimeObject *object = (GMimeObject *) mime_part;
	
	g_return_if_fail (GMIME_IS_PART (mime_part));
	
	if (!object->disposition)
		g_mime_object_set_disposition (object, GMIME_DISPOSITION_ATTACHMENT);
	
	g_mime_object_set_content_disposition_parameter (object, "filename", filename);
	g_mime_object_set_content_type_parameter (object, "name", filename);
}


/**
 * g_mime_part_get_filename:
 * @mime_part: Mime part
 *
 * Gets the filename of the specificed mime part, or %NULL if the mime
 * part does not have the filename or name parameter set.
 *
 * Returns: the filename of the specified MIME Part. It first checks to
 * see if the "filename" parameter was set on the Content-Disposition
 * and if not then checks the "name" parameter in the Content-Type.
 **/
const char *
g_mime_part_get_filename (const GMimePart *mime_part)
{
	GMimeObject *object = (GMimeObject *) mime_part;
	const char *filename = NULL;
	
	g_return_val_if_fail (GMIME_IS_PART (mime_part), NULL);
	
	if ((filename = g_mime_object_get_content_disposition_parameter (object, "filename")))
		return filename;
	
	/* check the "name" param in the content-type */
	return g_mime_object_get_content_type_parameter (object, "name");
}


/**
 * g_mime_part_set_content_object:
 * @mime_part: MIME Part
 * @content: content object
 *
 * Sets the content object on the mime part.
 **/
void
g_mime_part_set_content_object (GMimePart *mime_part, GMimeDataWrapper *content)
{
	g_return_if_fail (GMIME_IS_PART (mime_part));
	
	if (content)
		g_object_ref (content);
	
	if (mime_part->content)
		g_object_unref (mime_part->content);
	
	mime_part->content = content;
}


/**
 * g_mime_part_get_content_object:
 * @mime_part: MIME part object
 *
 * Gets the internal data-wrapper of the specified mime part, or %NULL
 * on error.
 *
 * Returns: the data-wrapper for the mime part's contents.
 **/
GMimeDataWrapper *
g_mime_part_get_content_object (const GMimePart *mime_part)
{
	g_return_val_if_fail (GMIME_IS_PART (mime_part), NULL);
	
	return mime_part->content;
}
