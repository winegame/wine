/*
 * winegstreamer Unix library interface
 *
 * Copyright 2020-2021 Zebediah Figura for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

#define GLIB_VERSION_MIN_REQUIRED GLIB_VERSION_2_30
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/audio/audio.h>
#include <gst/base/base.h>
#include <gst/tag/tag.h>
#include <gst/gl/gl.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "winternl.h"
#include "dshow.h"

#include "unix_private.h"

/* GStreamer callbacks may be called on threads not created by Wine, and
 * therefore cannot access the Wine TEB. This means that we must use GStreamer
 * debug logging instead of Wine debug logging. In order to be safe we forbid
 * any use of Wine debug logging in this entire file. */

GST_DEBUG_CATEGORY(wine);

GstGLDisplay *gl_display;

GstStreamType stream_type_from_caps(GstCaps *caps)
{
    const gchar *media_type;

    if (!caps || !gst_caps_get_size(caps))
        return GST_STREAM_TYPE_UNKNOWN;

    media_type = gst_structure_get_name(gst_caps_get_structure(caps, 0));
    if (g_str_has_prefix(media_type, "video/")
            || g_str_has_prefix(media_type, "image/"))
        return GST_STREAM_TYPE_VIDEO;
    if (g_str_has_prefix(media_type, "audio/"))
        return GST_STREAM_TYPE_AUDIO;
    if (g_str_has_prefix(media_type, "text/")
            || g_str_has_prefix(media_type, "subpicture/")
            || g_str_has_prefix(media_type, "closedcaption/"))
        return GST_STREAM_TYPE_TEXT;

    return GST_STREAM_TYPE_UNKNOWN;
}

GstElement *create_element(const char *name, const char *plugin_set)
{
    GstElement *element;

    if (!(element = gst_element_factory_make(name, NULL)))
        fprintf(stderr, "winegstreamer: failed to create %s, are %u-bit GStreamer \"%s\" plugins installed?\n",
                name, 8 * (unsigned int)sizeof(void *), plugin_set);
    return element;
}

GstElement *find_element(GstElementFactoryListType type, GstCaps *src_caps, GstCaps *sink_caps)
{
    GstElement *element = NULL;
    GList *tmp, *transforms;
    const gchar *name;

    if (!(transforms = gst_element_factory_list_get_elements(type, GST_RANK_MARGINAL)))
        goto done;

    tmp = gst_element_factory_list_filter(transforms, src_caps, GST_PAD_SINK, FALSE);
    gst_plugin_feature_list_free(transforms);
    if (!(transforms = tmp))
        goto done;

    tmp = gst_element_factory_list_filter(transforms, sink_caps, GST_PAD_SRC, FALSE);
    gst_plugin_feature_list_free(transforms);
    if (!(transforms = tmp))
        goto done;

    transforms = g_list_sort(transforms, gst_plugin_feature_rank_compare_func);
    for (tmp = transforms; tmp != NULL && element == NULL; tmp = tmp->next)
    {
        name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(tmp->data));

        if (!strcmp(name, "vaapidecodebin"))
        {
            /* vaapidecodebin adds asynchronicity which breaks wg_transform synchronous drain / flush
             * requirements. Ignore it and use VA-API decoders directly instead.
             */
            GST_WARNING("Ignoring vaapidecodebin decoder.");
            continue;
        }

        if (!(element = gst_element_factory_create(GST_ELEMENT_FACTORY(tmp->data), NULL)))
            GST_WARNING("Failed to create %s element.", name);
    }
    gst_plugin_feature_list_free(transforms);

done:
    if (element)
    {
        GST_DEBUG("Created %s element %p.", name, element);
    }
    else
    {
        gchar *src_str = gst_caps_to_string(src_caps), *sink_str = gst_caps_to_string(sink_caps);
        GST_WARNING("Failed to create element matching caps %s / %s.", src_str, sink_str);
        g_free(sink_str);
        g_free(src_str);
    }

    return element;
}

bool append_element(GstElement *container, GstElement *element, GstElement **first, GstElement **last)
{
    gchar *name = gst_element_get_name(element);
    bool success = false;

    if (!gst_bin_add(GST_BIN(container), element) ||
            !gst_element_sync_state_with_parent(element) ||
            (*last && !gst_element_link(*last, element)))
    {
        GST_ERROR("Failed to link %s element.", name);
    }
    else
    {
        GST_DEBUG("Linked %s element %p.", name, element);
        if (!*first)
            *first = element;
        *last = element;
        success = true;
    }

    g_free(name);
    return success;
}

bool link_src_to_element(GstPad *src_pad, GstElement *element)
{
    GstPadLinkReturn ret;
    GstPad *sink_pad;

    if (!(sink_pad = gst_element_get_static_pad(element, "sink")))
    {
        gchar *name = gst_element_get_name(element);
        GST_ERROR("Failed to find sink pad on %s", name);
        g_free(name);
        return false;
    }
    if ((ret = gst_pad_link(src_pad, sink_pad)))
    {
        gchar *src_name = gst_pad_get_name(src_pad), *sink_name = gst_pad_get_name(sink_pad);
        GST_ERROR("Failed to link element pad %s with pad %s", src_name, sink_name);
        g_free(sink_name);
        g_free(src_name);
    }
    gst_object_unref(sink_pad);
    return !ret;
}

bool link_element_to_sink(GstElement *element, GstPad *sink_pad)
{
    GstPadLinkReturn ret;
    GstPad *src_pad;

    if (!(src_pad = gst_element_get_static_pad(element, "src")))
    {
        gchar *name = gst_element_get_name(element);
        GST_ERROR("Failed to find src pad on %s", name);
        g_free(name);
        return false;
    }
    if ((ret = gst_pad_link(src_pad, sink_pad)))
    {
        gchar *src_name = gst_pad_get_name(src_pad), *sink_name = gst_pad_get_name(sink_pad);
        GST_ERROR("Failed to link pad %s with element pad %s", src_name, sink_name);
        g_free(sink_name);
        g_free(src_name);
    }
    gst_object_unref(src_pad);
    return !ret;
}

#include <gstreamer-1.0/gst/base/gsttypefindhelper.h>

typedef struct
{
  const guint8 *data;           /* buffer data */
  gsize size;
  GstTypeFindProbability best_probability;
  GstCaps *caps;
  GstTypeFindFactory *factory;  /* for logging */
  GstObject *obj;               /* for logging */
} GstTypeFindBufHelper;

/*
 * buf_helper_find_peek:
 * @data: helper data struct
 * @off: stream offset
 * @size: block size
 *
 * Get data pointer within a buffer.
 *
 * Returns: (nullable): address inside the buffer or %NULL if buffer does not
 * cover the requested range.
 */
static const guint8 *
buf_helper_find_peek (gpointer data, gint64 off, guint size)
{
  GstTypeFindBufHelper *helper;

  helper = (GstTypeFindBufHelper *) data;
  GST_LOG_OBJECT (helper->obj, "'%s' called peek (%" G_GINT64_FORMAT ", %u)",
      GST_OBJECT_NAME (helper->factory), off, size);

  if (size == 0)
    return NULL;

  if (off < 0) {
    GST_LOG_OBJECT (helper->obj, "'%s' wanted to peek at end; not supported",
        GST_OBJECT_NAME (helper->factory));
    return NULL;
  }

  /* If we request beyond the available size, we're sure we can't return
   * anything regardless of the requested offset */
  if (size > helper->size)
    return NULL;

  /* Only return data if there's enough room left for the given offset.
   * This is the same as "if (off + size <= helper->size)" except that
   * it doesn't exceed type limits */
  if (off <= helper->size - size)
    return helper->data + off;

  return NULL;
}

/*
 * buf_helper_find_suggest:
 * @data: helper data struct
 * @probability: probability of the match
 * @caps: caps of the type
 *
 * If given @probability is higher, replace previously store caps.
 */
static void
buf_helper_find_suggest (gpointer data, guint probability, GstCaps * caps)
{
  GstTypeFindBufHelper *helper = (GstTypeFindBufHelper *) data;

  GST_LOG_OBJECT (helper->obj,
      "'%s' called suggest (%u, %" GST_PTR_FORMAT ")",
      GST_OBJECT_NAME (helper->factory), probability, caps);

  /* Note: not >= as we call typefinders in order of rank, highest first */
  if (probability > helper->best_probability) {
    gst_caps_replace (&helper->caps, caps);
    helper->best_probability = probability;
  }
}

static GList *
prioritize_extension (GstObject * obj, GList * type_list,
    const gchar * extension)
{
  gint pos = 0;
  GList *next, *l;

  if (!extension)
    return type_list;

  /* move the typefinders for the extension first in the list. The idea is that
   * when one of them returns MAX we don't need to search further as there is a
   * very high chance we got the right type. */

  GST_LOG_OBJECT (obj, "sorting typefind for extension %s to head", extension);

  for (l = type_list; l; l = next) {
    const gchar *const *ext;
    GstTypeFindFactory *factory;

    next = l->next;

    factory = GST_TYPE_FIND_FACTORY (l->data);

    ext = gst_type_find_factory_get_extensions (factory);
    if (ext == NULL)
      continue;

    GST_LOG_OBJECT (obj, "testing factory %s for extension %s",
        GST_OBJECT_NAME (factory), extension);

    while (*ext != NULL) {
      if (strcmp (*ext, extension) == 0) {
        /* found extension, move in front */
        GST_LOG_OBJECT (obj, "moving typefind for extension %s to head",
            extension);
        /* remove entry from list */
        type_list = g_list_delete_link (type_list, l);
        /* insert at the position */
        type_list = g_list_insert (type_list, factory, pos);
        /* next element will be inserted after this one */
        pos++;
        break;
      }
      ++ext;
    }
  }

  return type_list;
}

/**
 * gst_type_find_helper_for_data_with_extension:
 * @obj: (allow-none): object doing the typefinding, or %NULL (used for logging)
 * @data: (transfer none) (array length=size): * a pointer with data to typefind
 * @size: the size of @data
 * @extension: (allow-none): extension of the media, or %NULL
 * @prob: (out) (allow-none): location to store the probability of the found
 *     caps, or %NULL
 *
 * Tries to find what type of data is contained in the given @data, the
 * assumption being that the data represents the beginning of the stream or
 * file.
 *
 * All available typefinders will be called on the data in order of rank. If
 * a typefinding function returns a probability of %GST_TYPE_FIND_MAXIMUM,
 * typefinding is stopped immediately and the found caps will be returned
 * right away. Otherwise, all available typefind functions will the tried,
 * and the caps with the highest probability will be returned, or %NULL if
 * the content of @data could not be identified.
 *
 * When @extension is not %NULL, this function will first try the typefind
 * functions for the given extension, which might speed up the typefinding
 * in many cases.
 *
 * Free-function: gst_caps_unref
 *
 * Returns: (transfer full) (nullable): the #GstCaps corresponding to the data,
 *     or %NULL if no type could be found. The caller should free the caps
 *     returned with gst_caps_unref().
 *
 * Since: 1.16
 *
 */
GstCaps *
gst_type_find_helper_for_data_with_extension (GstObject * obj,
    const guint8 * data, gsize size, const gchar * extension,
    GstTypeFindProbability * prob)
{
  GstTypeFindBufHelper helper;
  GstTypeFind find;
  GList *l, *type_list;
  GstCaps *result = NULL;

  g_return_val_if_fail (data != NULL, NULL);

  helper.data = data;
  helper.size = size;
  helper.best_probability = GST_TYPE_FIND_NONE;
  helper.caps = NULL;
  helper.obj = obj;

  if (helper.data == NULL || helper.size == 0)
    return NULL;

  find.data = &helper;
  find.peek = buf_helper_find_peek;
  find.suggest = buf_helper_find_suggest;
  find.get_length = NULL;

  type_list = gst_type_find_factory_get_list ();
  type_list = prioritize_extension (obj, type_list, extension);

  for (l = type_list; l; l = l->next) {
    helper.factory = GST_TYPE_FIND_FACTORY (l->data);
    gst_type_find_factory_call_function (helper.factory, &find);
    if (helper.best_probability >= GST_TYPE_FIND_MAXIMUM)
      break;
  }
  gst_plugin_feature_list_free (type_list);

  if (helper.best_probability > 0)
    result = helper.caps;

  if (prob)
    *prob = helper.best_probability;

  GST_LOG_OBJECT (obj, "Returning %" GST_PTR_FORMAT " (probability = %u)",
      result, (guint) helper.best_probability);

  return result;
}

GstCaps *detect_caps_from_data(const char *url, const void *data, guint size)
{
    const char *extension = url ? strrchr(url, '.') : NULL;
    GstTypeFindProbability probability;
    GstCaps *caps;
    gchar *str;

    if (!(caps = gst_type_find_helper_for_data_with_extension(NULL, data, size,
            extension ? extension + 1 : NULL, &probability)))
    {
        GST_ERROR("Failed to detect caps for url %s, data %p, size %u", url, data, size);
        return NULL;
    }

    str = gst_caps_to_string(caps);
    if (probability > GST_TYPE_FIND_POSSIBLE)
        GST_INFO("Detected caps %s with probability %u for url %s, data %p, size %u",
                str, probability, url, data, size);
    else
        GST_FIXME("Detected caps %s with probability %u for url %s, data %p, size %u",
                str, probability, url, data, size);
    g_free(str);

    return caps;
}

GstPad *create_pad_with_caps(GstPadDirection direction, GstCaps *caps)
{
    GstCaps *pad_caps = caps ? gst_caps_ref(caps) : gst_caps_new_any();
    const char *name = direction == GST_PAD_SRC ? "src" : "sink";
    GstPadTemplate *template;
    GstPad *pad;

    if (!pad_caps || !(template = gst_pad_template_new(name, direction, GST_PAD_ALWAYS, pad_caps)))
        return NULL;
    pad = gst_pad_new_from_template(template, "src");
    g_object_unref(template);
    gst_caps_unref(pad_caps);
    return pad;
}

GstBuffer *create_buffer_from_bytes(const void *data, guint size)
{
    GstBuffer *buffer;

    if (!(buffer = gst_buffer_new_and_alloc(size)))
        GST_ERROR("Failed to allocate buffer for %#x bytes\n", size);
    else
    {
        gst_buffer_fill(buffer, 0, data, size);
        gst_buffer_set_size(buffer, size);
    }

    return buffer;
}

gchar *stream_lang_from_tags(GstTagList *tags, GstCaps *caps)
{
    gchar *value;

    if (!gst_tag_list_get_string(tags, GST_TAG_LANGUAGE_CODE, &value) || !value)
        return NULL;

    return value;
}

gchar *stream_name_from_tags(GstTagList *tags)
{
    /* Extract stream name from Quick Time demuxer private tag where it puts unrecognized chunks. */
    guint i, tag_count = gst_tag_list_get_tag_size(tags, "private-qt-tag");
    gchar *value = NULL;

    for (i = 0; !value && i < tag_count; ++i)
    {
        const gchar *name;
        const GValue *val;
        GstSample *sample;
        GstBuffer *buf;
        gsize size;

        if (!(val = gst_tag_list_get_value_index(tags, "private-qt-tag", i)))
            continue;
        if (!GST_VALUE_HOLDS_SAMPLE(val) || !(sample = gst_value_get_sample(val)))
            continue;
        name = gst_structure_get_name(gst_sample_get_info(sample));
        if (!name || strcmp(name, "application/x-gst-qt-name-tag"))
            continue;
        if (!(buf = gst_sample_get_buffer(sample)))
            continue;
        if ((size = gst_buffer_get_size(buf)) < 8)
            continue;
        size -= 8;
        if (!(value = g_malloc(size + 1)))
            return NULL;
        if (gst_buffer_extract(buf, 8, value, size) != size)
        {
            g_free(value);
            value = NULL;
            continue;
        }
        value[size] = 0;
    }

    return value;
}

NTSTATUS wg_init_gstreamer(void *arg)
{
    static GstGLContext *gl_context;

    char arg0[] = "wine";
    char arg1[] = "--gst-disable-registry-fork";
    char *args[] = {arg0, arg1, NULL};
    int argc = ARRAY_SIZE(args) - 1;
    char **argv = args;
    GError *err;

    const char *e;

    if ((e = getenv("WINE_GST_REGISTRY_DIR")))
    {
        char gst_reg[PATH_MAX];
#if defined(__x86_64__)
        const char *arch = "/registry.x86_64.bin";
#elif defined(__i386__)
        const char *arch = "/registry.i386.bin";
#else
#error Bad arch
#endif
        strcpy(gst_reg, e);
        strcat(gst_reg, arch);
        setenv("GST_REGISTRY_1_0", gst_reg, 1);
    }

    gst_segtrap_set_enabled(false);

    if (!gst_init_check(&argc, &argv, &err))
    {
        fprintf(stderr, "winegstreamer: failed to initialize GStreamer: %s\n", err->message);
        g_error_free(err);
        return STATUS_UNSUCCESSFUL;
    }

    GST_DEBUG_CATEGORY_INIT(wine, "WINE", GST_DEBUG_FG_RED, "Wine GStreamer support");

    GST_INFO("GStreamer library version %s; wine built with %d.%d.%d.",
            gst_version_string(), GST_VERSION_MAJOR, GST_VERSION_MINOR, GST_VERSION_MICRO);

    if (!(gl_display = gst_gl_display_new()))
        GST_ERROR("Failed to create OpenGL display");
    else
    {
        GError *error = NULL;
        gboolean ret;

        GST_OBJECT_LOCK(gl_display);
        ret = gst_gl_display_create_context(gl_display, NULL, &gl_context, &error);
        GST_OBJECT_UNLOCK(gl_display);
        g_clear_error(&error);

        if (ret)
            gst_gl_display_add_context(gl_display, gl_context);
        else
        {
            GST_ERROR("Failed to create OpenGL context");
            gst_object_unref(gl_display);
            gl_display = NULL;
        }
    }

    return STATUS_SUCCESS;
}
