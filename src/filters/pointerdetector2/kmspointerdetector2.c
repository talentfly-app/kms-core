/*
 * (C) Copyright 2013 Kurento (http://kurento.org/)
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * (LGPL) version 2.1 which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 */
#define _XOPEN_SOURCE 500

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/video/gstvideofilter.h>
#include <glib/gstdio.h>
#include <ftw.h>
#include <string.h>
#include <errno.h>
#include <libsoup/soup.h>

#include "kmspointerdetector2.h"

#define PLUGIN_NAME "pointerdetector2"

GST_DEBUG_CATEGORY_STATIC (kms_pointer_detector2_debug_category);
#define GST_CAT_DEFAULT kms_pointer_detector2_debug_category

#define KMS_POINTER_DETECTOR2_GET_PRIVATE(obj) ( \
  G_TYPE_INSTANCE_GET_PRIVATE (                  \
    (obj),                                       \
    KMS_TYPE_POINTER_DETECTOR2,                  \
    KmsPointerDetector2Private                   \
  )                                              \
)

#define FRAMES_TO_RESET  ((int) 250)
#define GREEN CV_RGB (0, 255, 0)
#define WHITE CV_RGB (255, 255, 255)
#define RED cvScalar (359, 89, 100, 0)

#define TEMP_PATH "/tmp/XXXXXX"
#define INACTIVE_IMAGE_VARIANT_NAME "i"
#define ACTIVE_IMAGE_VARIANT_NAME "a"
#define V_MIN 30
#define V_MAX 256

enum
{
  PROP_0,
  PROP_SHOW_DEBUG_INFO,
  PROP_WINDOWS_LAYOUT,
  PROP_MESSAGE,
  PROP_SHOW_WINDOWS_LAYOUT,
  PROP_COLOR_TARGET
};

struct _KmsPointerDetector2Private
{
  IplImage *cvImage;
  CvPoint finalPointerPosition;
  int iteration;
  CvSize frameSize;
  gboolean show_debug_info;
  GstStructure *buttonsLayout;
  GSList *buttonsLayoutList;
  const gchar *previousButtonClickedId;
  gboolean putMessage;
  gboolean show_windows_layout;
  gchar *images_dir;
  gint h_min, h_max, s_min, s_max;
  IplConvKernel *kernel1;
  IplConvKernel *kernel2;
};

/* pad templates */

#define VIDEO_SRC_CAPS \
    GST_VIDEO_CAPS_MAKE("{ BGR }")

#define VIDEO_SINK_CAPS \
    GST_VIDEO_CAPS_MAKE("{ BGR }")

/* class initialization */

G_DEFINE_TYPE_WITH_CODE (KmsPointerDetector2, kms_pointer_detector2,
    GST_TYPE_VIDEO_FILTER,
    GST_DEBUG_CATEGORY_INIT (kms_pointer_detector2_debug_category, PLUGIN_NAME,
        0, "debug category for pointerdetector2 element"));

static void
dispose_button_struct (gpointer data)
{
  ButtonStruct *aux = data;

  if (aux->id != NULL)
    g_free (aux->id);

  if (aux->inactive_icon != NULL)
    cvReleaseImage (&aux->inactive_icon);

  if (aux->active_icon != NULL)
    cvReleaseImage (&aux->active_icon);

  g_free (aux);
}

static void
kms_pointer_detector2_dispose_buttons_layout_list (KmsPointerDetector2 *
    pointerdetector)
{
  g_slist_free_full (pointerdetector->priv->buttonsLayoutList,
      dispose_button_struct);
  pointerdetector->priv->buttonsLayoutList = NULL;
}

static gboolean
is_valid_uri (const gchar * url)
{
  gboolean ret;
  GRegex *regex;

  regex = g_regex_new ("^(?:((?:https?):)\\/\\/)([^:\\/\\s]+)(?::(\\d*))?(?:\\/"
      "([^\\s?#]+)?([?][^?#]*)?(#.*)?)?$", 0, 0, NULL);
  ret = g_regex_match (regex, url, G_REGEX_MATCH_ANCHORED, NULL);
  g_regex_unref (regex);

  return ret;
}

static void
load_from_url (gchar * file_name, gchar * url)
{
  SoupSession *session;
  SoupMessage *msg;
  FILE *dst;

  session = soup_session_sync_new ();
  msg = soup_message_new ("GET", url);
  soup_session_send_message (session, msg);

  dst = fopen (file_name, "w+");

  if (dst == NULL) {
    GST_ERROR ("It is not possible to create the file");
    goto end;
  }
  fwrite (msg->response_body->data, 1, msg->response_body->length, dst);
  fclose (dst);

end:
  g_object_unref (msg);
  g_object_unref (session);
}

static IplImage *
load_image (gchar * uri, gchar * dir, gchar * image_name,
    const gchar * name_variant)
{
  IplImage *aux;

  aux = cvLoadImage (uri, CV_LOAD_IMAGE_UNCHANGED);
  if (aux == NULL) {
    if (is_valid_uri (uri)) {
      gchar *file_name;

      file_name =
          g_strconcat (dir, "/", image_name, name_variant, ".png", NULL);
      load_from_url (file_name, uri);
      aux = cvLoadImage (file_name, CV_LOAD_IMAGE_UNCHANGED);
      g_remove (file_name);
      g_free (file_name);
    }
  }

  return aux;
}

static void
kms_pointer_detector2_load_buttonsLayout (KmsPointerDetector2 * pointerdetector)
{
  int aux, len;
  gboolean have_inactive_icon, have_active_icon, have_transparency;
  gchar *inactive_uri, *active_uri;

  if (pointerdetector->priv->buttonsLayoutList != NULL) {
    kms_pointer_detector2_dispose_buttons_layout_list (pointerdetector);
  }

  len = gst_structure_n_fields (pointerdetector->priv->buttonsLayout);
  GST_DEBUG ("len: %d", len);

  for (aux = 0; aux < len; aux++) {
    const gchar *name =
        gst_structure_nth_field_name (pointerdetector->priv->buttonsLayout,
        aux);
    GstStructure *button;
    gboolean ret;

    ret =
        gst_structure_get (pointerdetector->priv->buttonsLayout, name,
        GST_TYPE_STRUCTURE, &button, NULL);
    if (ret) {
      ButtonStruct *structAux = g_malloc0 (sizeof (ButtonStruct));
      IplImage *aux = NULL;

      gst_structure_get (button, "upRightCornerX", G_TYPE_INT,
          &structAux->cvButtonLayout.x, NULL);
      gst_structure_get (button, "upRightCornerY", G_TYPE_INT,
          &structAux->cvButtonLayout.y, NULL);
      gst_structure_get (button, "width", G_TYPE_INT,
          &structAux->cvButtonLayout.width, NULL);
      gst_structure_get (button, "height", G_TYPE_INT,
          &structAux->cvButtonLayout.height, NULL);
      gst_structure_get (button, "id", G_TYPE_STRING, &structAux->id, NULL);
      have_inactive_icon =
          gst_structure_get (button, "inactive_uri", G_TYPE_STRING,
          &inactive_uri, NULL);
      have_transparency =
          gst_structure_get (button, "transparency", G_TYPE_DOUBLE,
          &structAux->transparency, NULL);
      have_active_icon =
          gst_structure_get (button, "active_uri", G_TYPE_STRING, &active_uri,
          NULL);

      if (have_inactive_icon) {
        aux =
            load_image (inactive_uri, pointerdetector->priv->images_dir,
            structAux->id, INACTIVE_IMAGE_VARIANT_NAME);

        if (aux != NULL) {
          structAux->inactive_icon =
              cvCreateImage (cvSize (structAux->cvButtonLayout.width,
                  structAux->cvButtonLayout.height), aux->depth,
              aux->nChannels);
          cvResize (aux, structAux->inactive_icon, CV_INTER_CUBIC);
          cvReleaseImage (&aux);
        } else {
          structAux->inactive_icon = NULL;
        }
      } else {
        structAux->inactive_icon = NULL;
      }

      if (have_active_icon) {
        aux =
            load_image (active_uri, pointerdetector->priv->images_dir,
            structAux->id, ACTIVE_IMAGE_VARIANT_NAME);

        if (aux != NULL) {
          structAux->active_icon =
              cvCreateImage (cvSize (structAux->cvButtonLayout.width,
                  structAux->cvButtonLayout.height), aux->depth,
              aux->nChannels);
          cvResize (aux, structAux->active_icon, CV_INTER_CUBIC);
          cvReleaseImage (&aux);
        } else {
          structAux->active_icon = NULL;
        }
      } else {
        structAux->active_icon = NULL;
      }

      if (have_transparency) {
        structAux->transparency = 1.0 - structAux->transparency;
      } else {
        structAux->transparency = 1.0;
      }

      GST_DEBUG ("check: %d %d %d %d", structAux->cvButtonLayout.x,
          structAux->cvButtonLayout.y, structAux->cvButtonLayout.width,
          structAux->cvButtonLayout.height);
      pointerdetector->priv->buttonsLayoutList =
          g_slist_append (pointerdetector->priv->buttonsLayoutList, structAux);
      gst_structure_free (button);

      if (have_inactive_icon) {
        g_free (inactive_uri);
      }
      if (have_active_icon) {
        g_free (active_uri);
      }
    }
  }
}

static void
kms_pointer_detector2_init (KmsPointerDetector2 * pointerdetector)
{
  pointerdetector->priv = KMS_POINTER_DETECTOR2_GET_PRIVATE (pointerdetector);

  pointerdetector->priv->cvImage = NULL;
  pointerdetector->priv->iteration = 0;
  pointerdetector->priv->show_debug_info = FALSE;
  pointerdetector->priv->buttonsLayout = NULL;
  pointerdetector->priv->buttonsLayoutList = NULL;
  pointerdetector->priv->putMessage = TRUE;
  pointerdetector->priv->show_windows_layout = TRUE;

  pointerdetector->priv->h_min = 0;
  pointerdetector->priv->h_max = 0;
  pointerdetector->priv->s_min = 0;
  pointerdetector->priv->s_max = 0;

  gchar d[] = TEMP_PATH;
  gchar *aux = g_mkdtemp (d);

  pointerdetector->priv->images_dir = g_strdup (aux);
  pointerdetector->priv->kernel1 =
      cvCreateStructuringElementEx (21, 21, 10, 10, CV_SHAPE_RECT, NULL);
  pointerdetector->priv->kernel2 =
      cvCreateStructuringElementEx (11, 11, 5, 5, CV_SHAPE_RECT, NULL);
}

void
kms_pointer_detector2_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  KmsPointerDetector2 *pointerdetector = KMS_POINTER_DETECTOR2 (object);

  GST_DEBUG_OBJECT (pointerdetector, "set_property");

  switch (property_id) {
    case PROP_SHOW_DEBUG_INFO:
      pointerdetector->priv->show_debug_info = g_value_get_boolean (value);
      break;
    case PROP_WINDOWS_LAYOUT:
      if (pointerdetector->priv->buttonsLayout != NULL)
        gst_structure_free (pointerdetector->priv->buttonsLayout);

      pointerdetector->priv->buttonsLayout = g_value_dup_boxed (value);
      kms_pointer_detector2_load_buttonsLayout (pointerdetector);
      break;
    case PROP_MESSAGE:
      pointerdetector->priv->putMessage = g_value_get_boolean (value);
      break;
    case PROP_SHOW_WINDOWS_LAYOUT:
      pointerdetector->priv->show_windows_layout = g_value_get_boolean (value);
      break;
    case PROP_COLOR_TARGET:{
      GstStructure *aux;

      aux = g_value_dup_boxed (value);
      gst_structure_get (aux, "h_min", G_TYPE_INT,
          &pointerdetector->priv->h_min, NULL);
      gst_structure_get (aux, "h_max", G_TYPE_INT,
          &pointerdetector->priv->h_max, NULL);
      gst_structure_get (aux, "s_min", G_TYPE_INT,
          &pointerdetector->priv->s_min, NULL);
      gst_structure_get (aux, "s_max", G_TYPE_INT,
          &pointerdetector->priv->s_max, NULL);
      gst_structure_free (aux);
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
kms_pointer_detector2_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  KmsPointerDetector2 *pointerdetector = KMS_POINTER_DETECTOR2 (object);

  GST_DEBUG_OBJECT (pointerdetector, "get_property");

  switch (property_id) {
    case PROP_SHOW_DEBUG_INFO:
      g_value_set_boolean (value, pointerdetector->priv->show_debug_info);
      break;
    case PROP_WINDOWS_LAYOUT:
      if (pointerdetector->priv->buttonsLayout == NULL) {
        pointerdetector->priv->buttonsLayout =
            gst_structure_new_empty ("windows");
      }
      g_value_set_boxed (value, pointerdetector->priv->buttonsLayout);
      break;
    case PROP_MESSAGE:
      g_value_set_boolean (value, pointerdetector->priv->putMessage);
      break;
    case PROP_SHOW_WINDOWS_LAYOUT:
      g_value_set_boolean (value, pointerdetector->priv->show_windows_layout);
      break;
    case PROP_COLOR_TARGET:{
      GstStructure *aux;

      aux = gst_structure_new ("color_target",
          "h_min", G_TYPE_INT, pointerdetector->priv->h_min,
          "h_max", G_TYPE_INT, pointerdetector->priv->h_max,
          "s_min", G_TYPE_INT, pointerdetector->priv->s_min,
          "s_max", G_TYPE_INT, pointerdetector->priv->s_max, NULL);
      g_value_set_boxed (value, aux);
      gst_structure_free (aux);
      break;
    }
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
kms_pointer_detector2_dispose (GObject * object)
{
  KmsPointerDetector2 *pointerdetector = KMS_POINTER_DETECTOR2 (object);

  GST_DEBUG_OBJECT (pointerdetector, "dispose");

  /* clean up as possible.  may be called multiple times */

  G_OBJECT_CLASS (kms_pointer_detector2_parent_class)->dispose (object);
}

static int
delete_file (const char *fpath, const struct stat *sb, int typeflag,
    struct FTW *ftwbuf)
{
  int rv = g_remove (fpath);

  if (rv) {
    GST_WARNING ("Error deleting file: %s. %s", fpath, strerror (errno));
  }

  return rv;
}

static void
remove_recursive (const gchar * path)
{
  nftw (path, delete_file, 64, FTW_DEPTH | FTW_PHYS);
}

void
kms_pointer_detector2_finalize (GObject * object)
{
  KmsPointerDetector2 *pointerdetector = KMS_POINTER_DETECTOR2 (object);

  GST_DEBUG_OBJECT (pointerdetector, "finalize");

  /* clean up object here */

  // TODO: Release window structure and window list

  cvReleaseImageHeader (&pointerdetector->priv->cvImage);

  cvReleaseStructuringElement (&pointerdetector->priv->kernel1);
  cvReleaseStructuringElement (&pointerdetector->priv->kernel2);

  remove_recursive (pointerdetector->priv->images_dir);
  g_free (pointerdetector->priv->images_dir);

  G_OBJECT_CLASS (kms_pointer_detector2_parent_class)->finalize (object);
}

static gboolean
kms_pointer_detector2_start (GstBaseTransform * trans)
{
  KmsPointerDetector2 *pointerdetector = KMS_POINTER_DETECTOR2 (trans);

  GST_DEBUG_OBJECT (pointerdetector, "start");

  return TRUE;
}

static gboolean
kms_pointer_detector2_stop (GstBaseTransform * trans)
{
  KmsPointerDetector2 *pointerdetector = KMS_POINTER_DETECTOR2 (trans);

  GST_DEBUG_OBJECT (pointerdetector, "stop");

  return TRUE;
}

static gboolean
kms_pointer_detector2_set_info (GstVideoFilter * filter, GstCaps * incaps,
    GstVideoInfo * in_info, GstCaps * outcaps, GstVideoInfo * out_info)
{
  KmsPointerDetector2 *pointerdetector = KMS_POINTER_DETECTOR2 (filter);

  GST_DEBUG_OBJECT (pointerdetector, "set_info");

  return TRUE;
}

static void
kms_pointer_detector2_initialize_images (KmsPointerDetector2 * pointerdetector,
    GstVideoFrame * frame)
{
  if (pointerdetector->priv->cvImage == NULL) {
    pointerdetector->priv->cvImage =
        cvCreateImageHeader (cvSize (frame->info.width, frame->info.height),
        IPL_DEPTH_8U, 3);
  } else if ((pointerdetector->priv->cvImage->width != frame->info.width)
      || (pointerdetector->priv->cvImage->height != frame->info.height)) {
    cvReleaseImageHeader (&pointerdetector->priv->cvImage);
    pointerdetector->priv->cvImage =
        cvCreateImageHeader (cvSize (frame->info.width, frame->info.height),
        IPL_DEPTH_8U, 3);
  }
}

static gboolean
kms_pointer_detector2_check_pointer_into_button (CvPoint * pointer_position,
    ButtonStruct * buttonStruct)
{
  int downLeftCornerX =
      buttonStruct->cvButtonLayout.x + buttonStruct->cvButtonLayout.width;
  int downLeftCornerY =
      buttonStruct->cvButtonLayout.y + buttonStruct->cvButtonLayout.height;
  if (((pointer_position->x - buttonStruct->cvButtonLayout.x) > 0)
      && ((pointer_position->y - buttonStruct->cvButtonLayout.y) > 0)
      && ((pointer_position->x - downLeftCornerX) < 0)
      && ((pointer_position->y - downLeftCornerY) < 0)) {
    return TRUE;
  } else {
    return FALSE;
  }
}

static void
kms_pointer_detector2_overlay_icon (IplImage * icon,
    gint x, gint y,
    gdouble transparency,
    gboolean saturate, KmsPointerDetector2 * pointerdetector)
{
  int w, h;
  uchar *row, *image_row;

  row = (uchar *) icon->imageData;
  image_row = (uchar *) pointerdetector->priv->cvImage->imageData +
      (y * pointerdetector->priv->cvImage->widthStep);

  for (h = 0; h < icon->height; h++) {

    uchar *column = row;
    uchar *image_column = image_row + (x * 3);

    for (w = 0; w < icon->width; w++) {
      /* Check if point is inside overlay boundaries */
      if (((w + x) < pointerdetector->priv->cvImage->width)
          && ((w + x) >= 0)) {
        if (((h + y) < pointerdetector->priv->cvImage->height)
            && ((h + y) >= 0)) {

          if (icon->nChannels == 1) {
            *(image_column) = (uchar) (*(column));
            *(image_column + 1) = (uchar) (*(column));
            *(image_column + 2) = (uchar) (*(column));
          } else if (icon->nChannels == 3) {
            *(image_column) = (uchar) (*(column));
            *(image_column + 1) = (uchar) (*(column + 1));
            *(image_column + 2) = (uchar) (*(column + 2));
          } else if (icon->nChannels == 4) {
            double proportion =
                ((double) *(uchar *) (column + 3)) / (double) 255;
            double overlay = transparency * proportion;
            double original = 1 - overlay;

            *image_column =
                (uchar) ((*column * overlay) + (*image_column * original));

            if (saturate) {
              *(image_column + 1) =
                  (uchar) ((255 * overlay) + (*(image_column + 1) * original));
            } else {
              *(image_column + 1) =
                  (uchar) ((*(column + 1) * overlay) + (*(image_column +
                          1) * original));
            }

            *(image_column + 2) =
                (uchar) ((*(column + 2) * overlay) + (*(image_column +
                        2) * original));
          }
        }
      }

      column += icon->nChannels;
      image_column += pointerdetector->priv->cvImage->nChannels;
    }

    row += icon->widthStep;
    image_row += pointerdetector->priv->cvImage->widthStep;
  }
}

static void
kms_pointer_detector2_check_pointer_position (KmsPointerDetector2 *
    pointerdetector)
{
  ButtonStruct *structAux;
  GSList *l;
  int buttonClickedCounter = 0;
  const gchar *actualButtonClickedId;

  for (l = pointerdetector->priv->buttonsLayoutList; l != NULL; l = l->next) {
    CvPoint upRightCorner;
    CvPoint downLeftCorner;
    CvScalar color;
    gboolean is_active_window;

    structAux = l->data;
    upRightCorner.x = structAux->cvButtonLayout.x;
    upRightCorner.y = structAux->cvButtonLayout.y;
    downLeftCorner.x =
        structAux->cvButtonLayout.x + structAux->cvButtonLayout.width;
    downLeftCorner.y =
        structAux->cvButtonLayout.y + structAux->cvButtonLayout.height;

    if (kms_pointer_detector2_check_pointer_into_button
        (&pointerdetector->priv->finalPointerPosition, structAux)) {
      buttonClickedCounter++;

      color = GREEN;
      is_active_window = TRUE;
      actualButtonClickedId = structAux->id;
    } else {
      color = WHITE;
      is_active_window = FALSE;;
    }

    if (pointerdetector->priv->show_windows_layout) {
      if (!is_active_window) {
        if (structAux->inactive_icon != NULL) {
          kms_pointer_detector2_overlay_icon (structAux->inactive_icon,
              structAux->cvButtonLayout.x,
              structAux->cvButtonLayout.y,
              structAux->transparency, FALSE, pointerdetector);
        } else {
          cvRectangle (pointerdetector->priv->cvImage, upRightCorner,
              downLeftCorner, color, 1, 8, 0);
        }
      } else {
        if (structAux->active_icon != NULL) {
          kms_pointer_detector2_overlay_icon (structAux->active_icon,
              structAux->cvButtonLayout.x,
              structAux->cvButtonLayout.y,
              structAux->transparency, FALSE, pointerdetector);
        } else if (structAux->inactive_icon != NULL) {
          kms_pointer_detector2_overlay_icon (structAux->inactive_icon,
              structAux->cvButtonLayout.x,
              structAux->cvButtonLayout.y,
              structAux->transparency, TRUE, pointerdetector);
        } else {
          cvRectangle (pointerdetector->priv->cvImage, upRightCorner,
              downLeftCorner, color, 1, 8, 0);
        }
      }
    }
  }

  if (buttonClickedCounter == 0) {
    if (pointerdetector->priv->previousButtonClickedId != NULL) {
      GstStructure *s;
      GstMessage *m;

      /* post a message to bus */
      GST_DEBUG ("exit window: %s",
          pointerdetector->priv->previousButtonClickedId);
      if (pointerdetector->priv->putMessage) {
        s = gst_structure_new ("window-out",
            "window", G_TYPE_STRING,
            pointerdetector->priv->previousButtonClickedId, NULL);
        m = gst_message_new_element (GST_OBJECT (pointerdetector), s);
        gst_element_post_message (GST_ELEMENT (pointerdetector), m);
      }
      pointerdetector->priv->previousButtonClickedId = NULL;
    }
  } else {
    if (pointerdetector->priv->previousButtonClickedId != actualButtonClickedId) {
      GstStructure *s;
      GstMessage *m;

      /* post a message to bus */
      GST_DEBUG ("into window: %s", actualButtonClickedId);
      if (pointerdetector->priv->putMessage) {
        s = gst_structure_new ("window-in",
            "window", G_TYPE_STRING, actualButtonClickedId, NULL);
        m = gst_message_new_element (GST_OBJECT (pointerdetector), s);
        gst_element_post_message (GST_ELEMENT (pointerdetector), m);
      }
      pointerdetector->priv->previousButtonClickedId = actualButtonClickedId;
    }
  }

}

static GstFlowReturn
kms_pointer_detector2_transform_frame_ip (GstVideoFilter * filter,
    GstVideoFrame * frame)
{
  KmsPointerDetector2 *pointerdetector = KMS_POINTER_DETECTOR2 (filter);
  GstMapInfo info;
  IplImage *color_filter;
  IplImage *hsv_image;
  IplImage *hough_image;
  CvMemStorage *storage = NULL;
  CvSeq *circles;
  int distance;
  int best_candidate;

  if ((pointerdetector->priv->h_min == 0) && (pointerdetector->priv->h_max == 0)
      && (pointerdetector->priv->s_min == 0)
      && (pointerdetector->priv->s_max == 0)) {
    return GST_FLOW_OK;
  }

  pointerdetector->priv->frameSize =
      cvSize (frame->info.width, frame->info.height);
  kms_pointer_detector2_initialize_images (pointerdetector, frame);
  gst_buffer_map (frame->buffer, &info, GST_MAP_READ);
  pointerdetector->priv->cvImage->imageData = (char *) info.data;

  //detect the coordenates of the pointer
  hsv_image = cvCreateImage (cvGetSize (pointerdetector->priv->cvImage),
      pointerdetector->priv->cvImage->depth, 3);
  cvCvtColor (pointerdetector->priv->cvImage, hsv_image, CV_BGR2HSV);
  color_filter = cvCreateImage (cvGetSize (pointerdetector->priv->cvImage),
      pointerdetector->priv->cvImage->depth, 1);
  cvInRangeS (hsv_image,
      cvScalar (pointerdetector->priv->h_min, pointerdetector->priv->s_min,
          V_MIN, 0), cvScalar (pointerdetector->priv->h_max,
          pointerdetector->priv->s_max, V_MAX, 0), color_filter);

  cvMorphologyEx (color_filter, color_filter, NULL,
      pointerdetector->priv->kernel1, CV_MOP_CLOSE, 1);
  cvMorphologyEx (color_filter, color_filter, NULL,
      pointerdetector->priv->kernel2, CV_MOP_OPEN, 1);

  hough_image = cvCloneImage (color_filter);
  cvSmooth (hough_image, hough_image, CV_GAUSSIAN, 15, 15, 0, 0);
  storage = cvCreateMemStorage (0);
  circles =
      cvHoughCircles (hough_image, storage, CV_HOUGH_GRADIENT, 2,
      color_filter->height / 10, 100, 40, 0, 0);

  if (circles->total == 0) {
    goto checkPoint;
  }

  if ((circles->total == 1)) {
    float *p = (float *) cvGetSeqElem (circles, 0);

    pointerdetector->priv->finalPointerPosition.x = cvRound (p[0]);
    pointerdetector->priv->finalPointerPosition.y = cvRound (p[1]);
    goto checkPoint;
  }

  distance = 0;
  best_candidate = 0;

  for (int i = 0; i < circles->total; i++) {
    int current_distance;
    float *p = (float *) cvGetSeqElem (circles, i);

    if (distance == 0) {
      best_candidate = i;
      continue;
    }
    current_distance =
        sqrt (((abs (pointerdetector->priv->finalPointerPosition.x - p[0]) -
                abs (pointerdetector->priv->finalPointerPosition.x - p[0]))
            * (abs (pointerdetector->priv->finalPointerPosition.x - p[0]) -
                abs (pointerdetector->priv->finalPointerPosition.x - p[0])))
        + ((abs (pointerdetector->priv->finalPointerPosition.y - p[1]) -
                abs (pointerdetector->priv->finalPointerPosition.y - p[1]))
            * (abs (pointerdetector->priv->finalPointerPosition.y - p[1]) -
                abs (pointerdetector->priv->finalPointerPosition.y - p[1]))));

    if (current_distance < distance) {
      best_candidate = i;
    }
  }
  float *p = (float *) cvGetSeqElem (circles, best_candidate);

  pointerdetector->priv->finalPointerPosition.x = p[0];
  pointerdetector->priv->finalPointerPosition.y = p[1];

checkPoint:
  if (storage != NULL) {
    cvReleaseMemStorage (&storage);
  }

  kms_pointer_detector2_check_pointer_position (pointerdetector);

  cvCircle (pointerdetector->priv->cvImage,
      pointerdetector->priv->finalPointerPosition, 10.0, cvScalar (0, 0, 255,
          0), -1, 8, 0);

  pointerdetector->priv->iteration++;
  if (hsv_image != NULL) {
    cvReleaseImage (&hsv_image);
  }
  if (color_filter != NULL) {
    cvReleaseImage (&color_filter);
  }
  if (hough_image != NULL) {
    cvReleaseImage (&hough_image);
  }
  gst_buffer_unmap (frame->buffer, &info);
  return GST_FLOW_OK;
}

static void
kms_pointer_detector2_class_init (KmsPointerDetector2Class * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseTransformClass *base_transform_class =
      GST_BASE_TRANSFORM_CLASS (klass);
  GstVideoFilterClass *video_filter_class = GST_VIDEO_FILTER_CLASS (klass);

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS (klass),
      gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
          gst_caps_from_string (VIDEO_SRC_CAPS)));
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS (klass),
      gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
          gst_caps_from_string (VIDEO_SINK_CAPS)));

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "Pointer detector element", "Video/Filter",
      "Detects pointer an raises events with its position",
      "Francisco Rivero <fj.riverog@gmail.com>");

  gobject_class->set_property = kms_pointer_detector2_set_property;
  gobject_class->get_property = kms_pointer_detector2_get_property;
  gobject_class->dispose = kms_pointer_detector2_dispose;
  gobject_class->finalize = kms_pointer_detector2_finalize;
  base_transform_class->start = GST_DEBUG_FUNCPTR (kms_pointer_detector2_start);
  base_transform_class->stop = GST_DEBUG_FUNCPTR (kms_pointer_detector2_stop);
  video_filter_class->set_info =
      GST_DEBUG_FUNCPTR (kms_pointer_detector2_set_info);
  video_filter_class->transform_frame_ip =
      GST_DEBUG_FUNCPTR (kms_pointer_detector2_transform_frame_ip);

  /* Properties initialization */
  g_object_class_install_property (gobject_class, PROP_SHOW_DEBUG_INFO,
      g_param_spec_boolean ("show-debug-region", "show debug region",
          "show evaluation regions over the image", FALSE, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_WINDOWS_LAYOUT,
      g_param_spec_boxed ("windows-layout", "windows layout",
          "supply the positions and dimensions of windows into the main window",
          GST_TYPE_STRUCTURE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_MESSAGE,
      g_param_spec_boolean ("message", "message",
          "Put a window-in or window-out message in the bus if "
          "an object enters o leaves a window", TRUE, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_SHOW_WINDOWS_LAYOUT,
      g_param_spec_boolean ("show-windows-layout", "show windows layout",
          "show windows layout over the image", TRUE, G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class, PROP_COLOR_TARGET,
      g_param_spec_boxed ("color-target", "color target",
          "define the h and s value ranges of the pointer color",
          GST_TYPE_STRUCTURE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_type_class_add_private (klass, sizeof (KmsPointerDetector2Private));
}

gboolean
kms_pointer_detector2_plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, PLUGIN_NAME, GST_RANK_NONE,
      KMS_TYPE_POINTER_DETECTOR2);
}
