/* Implementation of GUI terminal on the Mac OS.
   Copyright (C) 2000-2008  Free Software Foundation, Inc.
   Copyright (C) 2009-2016  YAMAMOTO Mitsuharu

This file is part of GNU Emacs Mac port.

GNU Emacs Mac port is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at
your option) any later version.

GNU Emacs Mac port is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Emacs Mac port.  If not, see <http://www.gnu.org/licenses/>.  */

/* Originally contributed by Andrew Choi (akochoi@mac.com) for Emacs 21.  */

#include <config.h>
#include <stdio.h>

#include "lisp.h"
#include "blockinput.h"

#include "macterm.h"

#include "systime.h"

#include <errno.h>
#include <sys/stat.h>

#include "character.h"
#include "coding.h"
#include "frame.h"
#include "dispextern.h"
#include "fontset.h"
#include "termhooks.h"
#include "termopts.h"
#include "termchar.h"
#include "buffer.h"
#include "window.h"
#include "keyboard.h"
#include "atimer.h"
#include "font.h"
#include "menu.h"



/* This is a chain of structures for all the X displays currently in
   use.  */

struct x_display_info *x_display_list;

/* This is a list of X Resource Database equivalents, each of which is
   implemented with a Lisp object.  They are stored in parallel with
   x_display_list.  */

static Lisp_Object x_display_rdb_list;

/* This is display since Mac does not support multiple ones.  */
struct mac_display_info one_mac_display_info;

/* The keysyms to use for the various modifiers.  */

static struct terminal *mac_create_terminal (struct mac_display_info *dpyinfo);
static void x_frame_rehighlight (struct x_display_info *);

static void x_clip_to_row (struct window *, struct glyph_row *,
			   enum glyph_row_area, GC);
static void x_check_fullscreen (struct frame *);
static void mac_initialize (void);

static void mac_set_background_and_transparency (GC, unsigned long,
						 unsigned char);

/* Fringe bitmaps.  */

static int max_fringe_bmp = 0;
static CGImageRef *fringe_bmp = 0;

CGColorSpaceRef mac_cg_color_space_rgb;
static CGColorRef mac_cg_color_black;

static void
init_cg_color (void)
{
  mac_cg_color_space_rgb = CGColorSpaceCreateWithName (kCGColorSpaceSRGB);
  {
    CGFloat rgba[] = {0.0f, 0.0f, 0.0f, 1.0f};

    mac_cg_color_black = CGColorCreate (mac_cg_color_space_rgb, rgba);
  }
}

void
mac_begin_scale_mismatch_detection (struct frame *f)
{
  FRAME_SCALE_MISMATCH_STATE (f) = FRAME_BACKING_SCALE_FACTOR (f);
}

static void
mac_detect_scale_mismatch (struct frame *f, int target_backing_scale)
{
  FRAME_SCALE_MISMATCH_STATE (f) |= target_backing_scale;
}

bool
mac_end_scale_mismatch_detection (struct frame *f)
{
  return FRAME_SCALE_MISMATCH_STATE (f) == (1|2);
}

/* X display function emulation */

/* Mac version of XDrawLine (to Pixmap).  */

void
mac_draw_line_to_pixmap (Pixmap p, GC gc, int x1, int y1, int x2, int y2)
{
  CGContextRef context;
  XImagePtr ximg = p;
  CGColorSpaceRef color_space;
  CGImageAlphaInfo alpha_info;
  CGFloat gx1 = x1, gy1 = y1, gx2 = x2, gy2 = y2;

  if (y1 != y2)
    gx1 += 0.5f, gx2 += 0.5f;
  if (x1 != x2)
    gy1 += 0.5f, gy2 += 0.5f;

  if (ximg->bits_per_pixel == 32)
    {
      color_space = mac_cg_color_space_rgb;
      alpha_info = kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Host;
    }
  else
    {
      color_space = NULL;
      alpha_info = kCGImageAlphaOnly;
    }
  context = CGBitmapContextCreate (ximg->data, ximg->width, ximg->height, 8,
				   ximg->bytes_per_line, color_space,
				   alpha_info);
  if (ximg->bits_per_pixel == 32)
    CGContextSetStrokeColorWithColor (context, gc->cg_fore_color);
  else
    CGContextSetGrayStrokeColor (context,
				 (CGFloat) gc->xgcv.foreground / 255.0f, 1.0f);
  CGContextMoveToPoint (context, gx1, gy1);
  CGContextAddLineToPoint (context, gx2, gy2);
  CGContextStrokePath (context);
  CGContextRelease (context);
}


static void
mac_erase_rectangle (struct frame *f, GC gc, int x, int y,
		     int width, int height)
{
  MAC_BEGIN_DRAW_TO_FRAME (f, gc, context);
  {
    CGRect rect = mac_rect_make (f, x, y, width, height);

    CG_CONTEXT_FILL_RECT_WITH_GC_BACKGROUND (f, context, rect, gc);
  }
  MAC_END_DRAW_TO_FRAME (f);
}


/* Mac version of XClearArea.  */

void
mac_clear_area (struct frame *f, int x, int y, int width, int height)
{
  mac_erase_rectangle (f, f->output_data.mac->normal_gc, x, y, width, height);
}

/* Mac version of XClearWindow.  */

static void
mac_clear_window (struct frame *f)
{
  mac_clear_area (f, 0, 0, FRAME_PIXEL_WIDTH (f), FRAME_PIXEL_HEIGHT (f));
}


/* Mac replacement for XCopyArea.  */

#define MAC_DRAW_CG_IMAGE_OVERLAY		(1 << 0)
#define MAC_DRAW_CG_IMAGE_2X			(1 << 1)
#define MAC_DRAW_CG_IMAGE_NO_INTERPOLATION	(1 << 2)

static void
mac_draw_cg_image (CGImageRef image, struct frame *f, GC gc,
		   int src_x, int src_y, int width, int height,
		   int dest_x, int dest_y, int flags)
{
  MAC_BEGIN_DRAW_TO_FRAME (f, gc, context);
  {
    CGRect dest_rect, bounds;

    dest_rect = mac_rect_make (f, dest_x, dest_y, width, height);
    if (!(flags & MAC_DRAW_CG_IMAGE_2X))
      bounds = mac_rect_make (f, dest_x - src_x, dest_y - src_y,
			      CGImageGetWidth (image),
			      CGImageGetHeight (image));
    else
      bounds = mac_rect_make (f, dest_x - src_x, dest_y - src_y,
			      CGImageGetWidth (image) / 2,
			      CGImageGetHeight (image) / 2);
    if (!(flags & MAC_DRAW_CG_IMAGE_OVERLAY))
      CG_CONTEXT_FILL_RECT_WITH_GC_BACKGROUND (f, context, dest_rect, gc);
    CGContextClipToRects (context, &dest_rect, 1);
    CGContextTranslateCTM (context,
			   CGRectGetMinX (bounds), CGRectGetMaxY (bounds));
    CGContextScaleCTM (context, 1, -1);
    if (CGImageIsMask (image))
      CGContextSetFillColorWithColor (context, gc->cg_fore_color);
    if (flags & MAC_DRAW_CG_IMAGE_NO_INTERPOLATION)
      CGContextSetInterpolationQuality (context, kCGInterpolationNone);
    bounds.origin = CGPointZero;
    CGContextDrawImage (context, bounds, image);
  }
  MAC_END_DRAW_TO_FRAME (f);
}


/* Mac replacement for XCreateBitmapFromBitmapData.  */

static void
mac_create_bitmap_from_bitmap_data (BitMap *bitmap, char *bits, int w, int h)
{
  static const unsigned char swap_nibble[16]
    = { 0x0, 0x8, 0x4, 0xc,    /* 0000 1000 0100 1100 */
	0x2, 0xa, 0x6, 0xe,    /* 0010 1010 0110 1110 */
	0x1, 0x9, 0x5, 0xd,    /* 0001 1001 0101 1101 */
	0x3, 0xb, 0x7, 0xf };  /* 0011 1011 0111 1111 */
  int i, j, w1;
  char *p;

  w1 = (w + 7) / 8;         /* nb of 8bits elt in X bitmap */
  bitmap->rowBytes = ((w + 15) / 16) * 2; /* nb of 16bits elt in Mac bitmap */
  bitmap->baseAddr = xzalloc (bitmap->rowBytes * h);
  for (i = 0; i < h; i++)
    {
      p = bitmap->baseAddr + i * bitmap->rowBytes;
      for (j = 0; j < w1; j++)
	{
	  /* Bitswap XBM bytes to match how Mac does things.  */
	  unsigned char c = *bits++;
	  *p++ = (unsigned char)((swap_nibble[c & 0xf] << 4)
				 | (swap_nibble[(c>>4) & 0xf]));
	}
    }

  bitmap->bounds.left = bitmap->bounds.top = 0;
  bitmap->bounds.right = w;
  bitmap->bounds.bottom = h;
}


static void
mac_free_bitmap (BitMap *bitmap)
{
  xfree (bitmap->baseAddr);
}


Pixmap
mac_create_pixmap (unsigned int width, unsigned int height, unsigned int depth)
{
  XImagePtr ximg;

  ximg = xmalloc (sizeof (*ximg));
  ximg->width = width;
  ximg->height = height;
  ximg->bits_per_pixel = depth == 1 ? 8 : 32;
  ximg->bytes_per_line = width * (ximg->bits_per_pixel / 8);
  ximg->data = xmalloc (ximg->bytes_per_line * height);
  return ximg;
}


Pixmap
mac_create_pixmap_from_bitmap_data (char *data,
				    unsigned int width, unsigned int height,
				    unsigned long fg, unsigned long bg,
				    unsigned int depth)
{
  Pixmap pixmap = NULL;
  BitMap bitmap;
  CGDataProviderRef provider;
  CGImageRef image_mask = NULL;

  mac_create_bitmap_from_bitmap_data (&bitmap, data, width, height);
  provider = CGDataProviderCreateWithData (NULL, bitmap.baseAddr,
					   bitmap.rowBytes * height, NULL);
  if (provider)
    {
      image_mask = CGImageMaskCreate (width, height, 1, 1, bitmap.rowBytes,
				      provider, NULL, 0);
      CGDataProviderRelease (provider);
    }
  if (image_mask)
    {
      CGContextRef context;

      pixmap = mac_create_pixmap (width, height, depth);
      context = CGBitmapContextCreate (pixmap->data, width, height, 8,
				       pixmap->bytes_per_line,
				       mac_cg_color_space_rgb,
				       kCGImageAlphaNoneSkipFirst
				       | kCGBitmapByteOrder32Host);
      if (context)
	{
	  XGCValues xgcv;
	  GC gc;
	  CGRect rect = CGRectMake (0, 0, width, height);

	  xgcv.foreground = fg;
	  xgcv.background = bg;
	  gc = mac_create_gc (GCForeground | GCBackground, &xgcv);
	  CGContextSetFillColorWithColor (context, gc->cg_fore_color);
	  CGContextFillRects (context, &rect, 1);
	  CGContextSetFillColorWithColor (context, gc->cg_back_color);
	  CGContextDrawImage (context, rect, image_mask);
	  mac_free_gc (gc);
	  CGContextRelease (context);
	}
      else
	{
	  mac_free_pixmap (pixmap);
	  pixmap = NULL;
	}
      CGImageRelease (image_mask);
    }
  mac_free_bitmap (&bitmap);

  return pixmap;
}


void
mac_free_pixmap (Pixmap pixmap)
{
  if (pixmap)
    {
      xfree (pixmap->data);
      xfree (pixmap);
    }
}


/* Mac replacement for XFillRectangle.  */

static void
mac_fill_rectangle (struct frame *f, GC gc, int x, int y, int width, int height)
{
  MAC_BEGIN_DRAW_TO_FRAME (f, gc, context);
  CGContextSetFillColorWithColor (context, gc->cg_fore_color);
  {
    CGRect rect = mac_rect_make (f, x, y, width, height);

    CGContextFillRects (context, &rect, 1);
  }
  MAC_END_DRAW_TO_FRAME (f);
}


/* Mac replacement for XDrawRectangle: dest is a window.  */

static void
mac_draw_rectangle (struct frame *f, GC gc, int x, int y, int width, int height)
{
  MAC_BEGIN_DRAW_TO_FRAME (f, gc, context);
  {
    CGRect rect;

    CGContextSetStrokeColorWithColor (context, gc->cg_fore_color);
    rect = mac_rect_make (f, x, y, width + 1, height + 1);
    CGContextStrokeRect (context, CGRectInset (rect, 0.5f, 0.5f));
  }
  MAC_END_DRAW_TO_FRAME (f);
}


static void
mac_fill_trapezoid_for_relief (struct frame *f, GC gc, int x, int y,
			       int width, int height, int top_p)
{
  MAC_BEGIN_DRAW_TO_FRAME (f, gc, context);
  {
    CGRect rect = mac_rect_make (f, x, y, width, height);
    CGPoint points[4];

    points[0].x = points[1].x = CGRectGetMinX (rect);
    points[2].x = points[3].x = CGRectGetMaxX (rect);
    points[0].y = points[3].y = CGRectGetMinY (rect);
    points[1].y = points[2].y = CGRectGetMaxY (rect);

    if (top_p)
      points[2].x -= CGRectGetHeight (rect);
    else
      points[0].x += CGRectGetHeight (rect);

    CGContextSetFillColorWithColor (context, gc->cg_fore_color);
    CGContextAddLines (context, points, 4);
    CGContextFillPath (context);
  }
  MAC_END_DRAW_TO_FRAME (f);
}

enum corners
  {
    CORNER_BOTTOM_RIGHT,	/* 0 -> pi/2 */
    CORNER_BOTTOM_LEFT,		/* pi/2 -> pi */
    CORNER_TOP_LEFT,		/* pi -> 3pi/2 */
    CORNER_TOP_RIGHT,		/* 3pi/2 -> 2pi */
    CORNER_LAST
  };

static void
mac_erase_corners_for_relief (struct frame *f, GC gc, int x, int y,
			      int width, int height,
			      CGFloat radius, CGFloat margin, int corners)
{
  MAC_BEGIN_DRAW_TO_FRAME (f, gc, context);
  {
    CGRect rect = mac_rect_make (f, x, y, width, height);
    int i;

    for (i = 0; i < CORNER_LAST; i++)
      if (corners & (1 << i))
	{
	  CGFloat xm, ym, xc, yc;

	  if (i == CORNER_TOP_LEFT || i == CORNER_BOTTOM_LEFT)
	    xm = CGRectGetMinX (rect) - margin, xc = xm + radius;
	  else
	    xm = CGRectGetMaxX (rect) + margin, xc = xm - radius;
	  if (i == CORNER_TOP_LEFT || i == CORNER_TOP_RIGHT)
	    ym = CGRectGetMinY (rect) - margin, yc = ym + radius;
	  else
	    ym = CGRectGetMaxY (rect) + margin, yc = ym - radius;

	  CGContextMoveToPoint (context, xm, ym);
	  CGContextAddArc (context, xc, yc, radius,
			   i * M_PI_2, (i + 1) * M_PI_2, 0);
	}
    CGContextClip (context);
    CG_CONTEXT_FILL_RECT_WITH_GC_BACKGROUND (f, context, rect, gc);
  }
  MAC_END_DRAW_TO_FRAME (f);
}


static void
mac_draw_horizontal_wave (struct frame *f, GC gc, int x, int y,
			  int width, int height, int wave_length)
{
  MAC_BEGIN_DRAW_TO_FRAME (f, gc, context);
  {
    CGRect wave_clip;
    CGFloat gperiod, gx1, gxmax, gy1, gy2;

    gperiod = wave_length * 2;
    wave_clip = mac_rect_make (f, x, y, width, height);
    gx1 = floor ((CGRectGetMinX (wave_clip) - 1.0f) / gperiod) * gperiod + 0.5f;
    gxmax = CGRectGetMaxX (wave_clip);
    gy1 = (CGFloat) y + 0.5f;
    gy2 = (CGFloat) (y + height) - 0.5f;

    CGContextClipToRect (context, wave_clip);
    CGContextSetStrokeColorWithColor (context, gc->cg_fore_color);
    CGContextMoveToPoint (context, gx1, gy1);
    while (gx1 <= gxmax)
      {
	CGContextAddLineToPoint (context, gx1 + gperiod * 0.5f, gy2);
	gx1 += gperiod;
	CGContextAddLineToPoint (context, gx1, gy1);
      }
    CGContextStrokePath (context);
  }
  MAC_END_DRAW_TO_FRAME (f);
}


static void
mac_invert_rectangle (struct frame *f, int x, int y, int width, int height)
{
  GC gc = f->output_data.mac->normal_gc;

  MAC_BEGIN_DRAW_TO_FRAME (f, gc, context);
  CGContextSetGrayFillColor (context, 1.0f, 1.0f);
  CGContextSetBlendMode (context, kCGBlendModeDifference);
  {
    CGRect rect = mac_rect_make (f, x, y, width, height);

    CGContextFillRects (context, &rect, 1);
  }
  MAC_END_DRAW_TO_FRAME (f);
}

/* Invert rectangles RECTANGLES[0], ..., RECTANGLES[N-1] in the frame F,
   excluding scroll bar area.  */

static void
mac_invert_rectangles (struct frame *f, NativeRectangle *rectangles, int n)
{
  int i;

  for (i = 0; i < n; i++)
    mac_invert_rectangle (f, rectangles[i].x, rectangles[i].y,
			  rectangles[i].width, rectangles[i].height);
  if (FRAME_HAS_VERTICAL_SCROLL_BARS (f))
    {
      Lisp_Object bar;

      for (bar = FRAME_SCROLL_BARS (f); !NILP (bar);
	   bar = XSCROLL_BAR (bar)->next)
	{
	  struct scroll_bar *b = XSCROLL_BAR (bar);
	  NativeRectangle bar_rect, r;

	  STORE_NATIVE_RECT (bar_rect, b->left, b->top, b->width, b->height);
	  for (i = 0; i < n; i++)
	    if (x_intersect_rectangles (rectangles + i, &bar_rect, &r))
	      mac_invert_rectangle (f, r.x, r.y, r.width, r.height);
	}
    }
}


/* Mac replacement for XChangeGC.  */

static void
mac_change_gc (GC gc, unsigned long mask, XGCValues *xgcv)
{
  if (mask & GCForeground)
    mac_set_foreground (gc, xgcv->foreground);
  if (mask & GCBackground)
    mac_set_background_and_transparency (gc, xgcv->background,
					 ((mask & GCBackgroundTransparency)
					  ? xgcv->background_transparency
					  : gc->xgcv.background_transparency));
  else if (mask & GCBackgroundTransparency)
    /* This case does not happen in the current code.  */
    mac_set_background_and_transparency (gc, gc->xgcv.background,
					 xgcv->background_transparency);
}


/* Mac replacement for XCreateGC.  */

GC
mac_create_gc (unsigned long mask, XGCValues *xgcv)
{
  GC gc = xzalloc (sizeof (*gc));

  gc->cg_fore_color = gc->cg_back_color = mac_cg_color_black;
  CGColorRetain (gc->cg_fore_color);
  CGColorRetain (gc->cg_back_color);
  XChangeGC (display, gc, mask, xgcv);

  return gc;
}


GC
mac_duplicate_gc (GC gc)
{
  GC new = xmalloc (sizeof (*new));

  *new = *gc;
  CGColorRetain (new->cg_fore_color);
  CGColorRetain (new->cg_back_color);
  if (new->clip_rects_data)
    CFRetain (new->clip_rects_data);

  return new;
}


/* Used in xfaces.c.  */

void
mac_free_gc (GC gc)
{
  CGColorRelease (gc->cg_fore_color);
  CGColorRelease (gc->cg_back_color);
  if (gc->clip_rects_data)
    CFRelease (gc->clip_rects_data);
#if defined (XMALLOC_BLOCK_INPUT_CHECK) && DRAWING_USE_GCD
  /* Don't use xfree here, because this might be called in a non-main
     thread.  */
  free (gc);
#else
  xfree (gc);
#endif
}


/* Mac replacement for XGetGCValues.  */

static void
mac_get_gc_values (GC gc, unsigned long mask, XGCValues *xgcv)
{
  if (mask & GCForeground)
    xgcv->foreground = gc->xgcv.foreground;
  if (mask & GCBackground)
    xgcv->background = gc->xgcv.background;
  if (mask & GCBackgroundTransparency)
    xgcv->background_transparency = gc->xgcv.background_transparency;
}

static CGColorRef
mac_cg_color_create (unsigned long color, unsigned char transparency)
{
  if (color == 0 && transparency == 0)
    return CGColorRetain (mac_cg_color_black);
  else
    {
      CGFloat rgba[4];

      rgba[0] = (CGFloat) RED_FROM_ULONG (color) / 255.0f;
      rgba[1] = (CGFloat) GREEN_FROM_ULONG (color) / 255.0f;
      rgba[2] = (CGFloat) BLUE_FROM_ULONG (color) / 255.0f;
      rgba[3] = (CGFloat) (255 - transparency) / 255.0f;

      return CGColorCreate (mac_cg_color_space_rgb, rgba);
    }
}

/* Mac replacement for XSetForeground.  */

void
mac_set_foreground (GC gc, unsigned long color)
{
  if (gc->xgcv.foreground != color)
    {
      gc->xgcv.foreground = color;
      CGColorRelease (gc->cg_fore_color);
      gc->cg_fore_color = mac_cg_color_create (color, 0);
    }
}

static void
mac_set_background_and_transparency (GC gc, unsigned long color,
				     unsigned char transparency)
{
  if (gc->xgcv.background != color
      || gc->xgcv.background_transparency != transparency)
    {
      gc->xgcv.background = color;
      gc->xgcv.background_transparency = transparency;
      CGColorRelease (gc->cg_back_color);
      gc->cg_back_color = mac_cg_color_create (color, transparency);
    }
}

/* Mac replacement for XSetBackground.  */

void
mac_set_background (GC gc, unsigned long color)
{
  mac_set_background_and_transparency (gc, color,
				       gc->xgcv.background_transparency);
}

/* Mac replacement for XSetClipRectangles.  */

static void
mac_set_clip_rectangles (struct frame *f, GC gc,
			 NativeRectangle *rectangles, int n)
{
  CFIndex length = n * sizeof (CGRect);
  CGRect *clip_rects = alloca (length);
  int i;

  for (i = 0; i < n; i++)
    {
      NativeRectangle *rect = rectangles + i;

      clip_rects[i] = mac_rect_make (f, rect->x, rect->y,
				     rect->width, rect->height);
    }

  if (gc->clip_rects_data)
    CFRelease (gc->clip_rects_data);
  gc->clip_rects_data = CFDataCreate (NULL, (const UInt8 *) clip_rects, length);
}


/* Mac replacement for XSetClipMask.  */

static void
mac_reset_clip_rectangles (struct frame *f, GC gc)
{
  if (gc->clip_rects_data)
    {
      CFRelease (gc->clip_rects_data);
      gc->clip_rects_data = NULL;
    }
}

/* Remove calls to XFlush by defining XFlush to an empty replacement.
   Calls to XFlush should be unnecessary because the X output buffer
   is flushed automatically as needed by calls to XPending,
   XNextEvent, or XWindowEvent according to the XFlush man page.
   mac_read_socket calls XPending.  Removing XFlush improves
   performance.  */

#define XFlush(DISPLAY)	(void) 0


void
x_set_frame_alpha (struct frame *f)
{
  struct x_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  double alpha = 1.0;
  double alpha_min = 1.0;

  if (dpyinfo->x_highlight_frame == f)
    alpha = f->alpha[0];
  else
    alpha = f->alpha[1];

  if (FLOATP (Vframe_alpha_lower_limit))
    alpha_min = XFLOAT_DATA (Vframe_alpha_lower_limit);
  else if (INTEGERP (Vframe_alpha_lower_limit))
    alpha_min = (XINT (Vframe_alpha_lower_limit)) / 100.0;

  if (alpha < 0.0)
    return;
  else if (alpha > 1.0)
    alpha = 1.0;
  else if (0.0 <= alpha && alpha < alpha_min && alpha_min <= 1.0)
    alpha = alpha_min;

  mac_set_frame_window_alpha (f, alpha);
}


/***********************************************************************
		    Starting and ending an update
 ***********************************************************************/

/* Start an update of frame F.  This function is installed as a hook
   for update_begin, i.e. it is called when update_begin is called.
   This function is called prior to calls to x_update_window_begin for
   each window being updated.  */

static void
x_update_begin (struct frame *f)
{
  block_input ();
  mac_update_begin (f);
  unblock_input ();
}

/* Start update of window W.  */

static void
x_update_window_begin (struct window *w)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  Mouse_HLInfo *hlinfo = MOUSE_HL_INFO (f);

  w->being_updated_p = true;
  w->output_cursor = w->cursor;

  block_input ();

  if (f == hlinfo->mouse_face_mouse_frame)
    {
      /* Don't do highlighting for mouse motion during the update.  */
      hlinfo->mouse_face_defer = true;

      /* If F needs to be redrawn, simply forget about any prior mouse
	 highlighting.  */
      if (FRAME_GARBAGED_P (f))
	hlinfo->mouse_face_window = Qnil;
    }

  unblock_input ();
}


/* Return GC for the face with FACE_ID on frame F.  If the face is not
   available, return DEFAULT_GC.  */

static GC
mac_gc_for_face_id (struct frame *f, int face_id, GC default_gc)
{
  struct face *face = FACE_FROM_ID (f, face_id);

  if (face)
    {
      prepare_face_for_display (f, face);
      return face->gc;
    }
  else
    return default_gc;
}

/* Draw a vertical window border from (x,y0) to (x,y1)  */

static void
mac_draw_vertical_window_border (struct window *w, int x, int y0, int y1)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  GC gc = mac_gc_for_face_id (f, VERTICAL_BORDER_FACE_ID,
			      f->output_data.mac->normal_gc);

  mac_fill_rectangle (f, gc, x, y0, 1, y1 - y0);
}

/* Draw a window divider from (x0,y0) to (x1,y1)  */

static void
mac_draw_window_divider (struct window *w, int x0, int x1, int y0, int y1)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  GC gc = mac_gc_for_face_id (f, WINDOW_DIVIDER_FACE_ID,
			      f->output_data.mac->normal_gc);

  if ((y1 - y0 > x1 - x0 && x1 - x0 > 2)
      || (x1 - x0 > y1 - y0 && y1 - y0 > 3))
    {
      GC gc_first = mac_gc_for_face_id (f, WINDOW_DIVIDER_FIRST_PIXEL_FACE_ID,
					f->output_data.mac->normal_gc);
      GC gc_last = mac_gc_for_face_id (f, WINDOW_DIVIDER_LAST_PIXEL_FACE_ID,
				       f->output_data.mac->normal_gc);

      if (y1 - y0 > x1 - x0)
	/* Vertical.  */
	{
	  mac_fill_rectangle (f, gc_first, x0, y0, 1, y1 - y0);
	  mac_fill_rectangle (f, gc, x0 + 1, y0, x1 - x0 - 2, y1 - y0);
	  mac_fill_rectangle (f, gc_last, x1 - 1, y0, 1, y1 - y0);
	}
      else
	/* Horizontal.  */
	{
	  mac_fill_rectangle (f, gc_first, x0, y0, x1 - x0, 1);
	  mac_fill_rectangle (f, gc, x0, y0 + 1, x1 - x0, y1 - y0 - 2);
	  mac_fill_rectangle (f, gc_last, x0, y1 - 1, x1 - x0, 1);
	}
    }
  else
    mac_fill_rectangle (f, gc, x0, y0, x1 - x0, y1 - y0);
}

/* End update of window W.

   Draw vertical borders between horizontally adjacent windows, and
   display W's cursor if CURSOR_ON_P is non-zero.

   MOUSE_FACE_OVERWRITTEN_P non-zero means that some row containing
   glyphs in mouse-face were overwritten.  In that case we have to
   make sure that the mouse-highlight is properly redrawn.

   W may be a menu bar pseudo-window in case we don't have X toolkit
   support.  Such windows don't have a cursor, so don't display it
   here.  */

static void
x_update_window_end (struct window *w, bool cursor_on_p,
		     bool mouse_face_overwritten_p)
{
  if (!w->pseudo_window_p)
    {
      block_input ();

      if (cursor_on_p)
	display_and_set_cursor (w, true,
				w->output_cursor.hpos, w->output_cursor.vpos,
				w->output_cursor.x, w->output_cursor.y);

      if (draw_window_fringes (w, true))
	{
	  if (WINDOW_RIGHT_DIVIDER_WIDTH (w))
	    x_draw_right_divider (w);
	  else
	    x_draw_vertical_border (w);
	}

      if (w == XWINDOW (selected_window))
	mac_update_accessibility_status (XFRAME (w->frame));

      unblock_input ();
    }

  /* If a row with mouse-face was overwritten, arrange for
     mac_frame_up_to_date to redisplay the mouse highlight.  */
  if (mouse_face_overwritten_p)
    {
      Mouse_HLInfo *hlinfo = MOUSE_HL_INFO (XFRAME (w->frame));

      hlinfo->mouse_face_beg_row = hlinfo->mouse_face_beg_col = -1;
      hlinfo->mouse_face_end_row = hlinfo->mouse_face_end_col = -1;
      hlinfo->mouse_face_window = Qnil;
    }

  w->being_updated_p = false;
}


/* End update of frame F.  This function is installed as a hook in
   update_end.  */

static void
x_update_end (struct frame *f)
{
  /* Mouse highlight may be displayed again.  */
  MOUSE_HL_INFO (f)->mouse_face_defer = false;

  block_input ();
  mac_update_end (f);
  XFlush (FRAME_MAC_DISPLAY (f));
  unblock_input ();
}


/* This function is called from various places in xdisp.c
   whenever a complete update has been performed.  */

static void
mac_frame_up_to_date (struct frame *f)
{
  if (FRAME_MAC_P (f))
    FRAME_MOUSE_UPDATE (f);
}

/* Clear under internal border if any. */

void
x_clear_under_internal_border (struct frame *f)
{
  if (FRAME_INTERNAL_BORDER_WIDTH (f) > 0)
    {
      int border = FRAME_INTERNAL_BORDER_WIDTH (f);
      int width = FRAME_PIXEL_WIDTH (f);
      int height = FRAME_PIXEL_HEIGHT (f);
      int margin = FRAME_TOP_MARGIN_HEIGHT (f);

      block_input ();
      mac_clear_area (f, 0, 0, border, height);
      mac_clear_area (f, 0, margin, width, border);
      mac_clear_area (f, width - border, 0, border, height);
      mac_clear_area (f, 0, height - border, width, border);
      unblock_input ();
    }
}

/* Draw truncation mark bitmaps, continuation mark bitmaps, overlay
   arrow bitmaps, or clear the fringes if no bitmaps are required
   before DESIRED_ROW is made current.  This function is called from
   update_window_line only if it is known that there are differences
   between bitmaps to be drawn between current row and DESIRED_ROW.  */

static void
x_after_update_window_line (struct window *w, struct glyph_row *desired_row)
{
  eassert (w);

  if (!desired_row->mode_line_p && !w->pseudo_window_p)
    desired_row->redraw_fringe_bitmaps_p = true;

  /* When a window has disappeared, make sure that no rest of
     full-width rows stays visible in the internal border.  Could
     check here if updated window is the leftmost/rightmost window,
     but I guess it's not worth doing since vertically split windows
     are almost never used, internal border is rarely set, and the
     overhead is very small.  */
  {
    struct frame *f;
    int width, height;

    if (windows_or_buffers_changed
	&& desired_row->full_width_p
	&& (f = XFRAME (w->frame),
	    width = FRAME_INTERNAL_BORDER_WIDTH (f),
	    width != 0)
	&& (height = desired_row->visible_height,
	    height > 0))
      {
	int y = WINDOW_TO_FRAME_PIXEL_Y (w, max (0, desired_row->y));

	block_input ();
	mac_clear_area (f, 0, y, width, height);
	mac_clear_area (f, FRAME_PIXEL_WIDTH (f) - width, y, width, height);
	unblock_input ();
      }
  }
}

static void
x_draw_fringe_bitmap (struct window *w, struct glyph_row *row, struct draw_fringe_bitmap_params *p)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  Display *display = FRAME_MAC_DISPLAY (f);
  struct face *face = p->face;
  bool overlay_p = p->overlay_p;

  /* Must clip because of partially visible lines.  */
  x_clip_to_row (w, row, ANY_AREA, face->gc);

  if (p->bx >= 0 && !overlay_p)
    {
#if 0  /* MAC_TODO: stipple */
      /* In case the same realized face is used for fringes and
	 for something displayed in the text (e.g. face `region' on
	 mono-displays, the fill style may have been changed to
	 FillSolid in x_draw_glyph_string_background.  */
      if (face->stipple)
	XSetFillStyle (FRAME_X_DISPLAY (f), face->gc, FillOpaqueStippled);
      else
	XSetForeground (FRAME_X_DISPLAY (f), face->gc, face->background);
#endif

      mac_erase_rectangle (f, face->gc, p->bx, p->by, p->nx, p->ny);
      /* The fringe background has already been filled.  */
      overlay_p = 1;

#if 0  /* MAC_TODO: stipple */
      if (!face->stipple)
	XSetForeground (FRAME_X_DISPLAY (f), face->gc, face->foreground);
#endif
    }

  if (p->which && p->which < max_fringe_bmp)
    {
      XGCValues gcv;
      int flags;

      XGetGCValues (display, face->gc, GCForeground, &gcv);
      XSetForeground (display, face->gc,
		      (p->cursor_p
		       ? (p->overlay_p ? face->background
			  : f->output_data.mac->cursor_pixel)
		       : face->foreground));
      flags = overlay_p ? MAC_DRAW_CG_IMAGE_OVERLAY : 0;
      if (FRAME_BACKING_SCALE_FACTOR (f) != 1)
	flags |= MAC_DRAW_CG_IMAGE_NO_INTERPOLATION;
      mac_draw_cg_image (fringe_bmp[p->which], f, face->gc, 0, p->dh,
			 p->wd, p->h, p->x, p->y, flags);
      XSetForeground (display, face->gc, gcv.foreground);
    }

  mac_reset_clip_rectangles (f, face->gc);
}

static void
mac_define_fringe_bitmap (int which, unsigned short *bits, int h, int wd)
{
  int i;
  CGDataProviderRef provider;

  if (which >= max_fringe_bmp)
    {
      i = max_fringe_bmp;
      max_fringe_bmp = which + 20;
      fringe_bmp = (CGImageRef *) xrealloc (fringe_bmp, max_fringe_bmp * sizeof (CGImageRef));
      while (i < max_fringe_bmp)
	fringe_bmp[i++] = 0;
    }

  for (i = 0; i < h; i++)
    bits[i] = ~bits[i];

  block_input ();

  provider = CGDataProviderCreateWithData (NULL, bits,
					   sizeof (unsigned short) * h, NULL);
  if (provider)
    {
      fringe_bmp[which] = CGImageMaskCreate (wd, h, 1, 1,
					     sizeof (unsigned short),
					     provider, NULL, 0);
      CGDataProviderRelease (provider);
    }

  unblock_input ();
}

static void
mac_destroy_fringe_bitmap (int which)
{
  if (which >= max_fringe_bmp)
    return;

  if (fringe_bmp[which])
    {
      block_input ();
      CGImageRelease (fringe_bmp[which]);
      unblock_input ();
    }
  fringe_bmp[which] = 0;
}

/***********************************************************************
			    Glyph display
 ***********************************************************************/



static void x_set_glyph_string_clipping (struct glyph_string *);
static void x_set_glyph_string_gc (struct glyph_string *);
static void x_draw_glyph_string_foreground (struct glyph_string *);
static void x_draw_composite_glyph_string_foreground (struct glyph_string *);
static void x_draw_glyph_string_box (struct glyph_string *);
static void x_draw_glyph_string  (struct glyph_string *);
static _Noreturn void x_delete_glyphs (struct frame *, int);
static void x_compute_glyph_string_overhangs (struct glyph_string *);
static void x_set_cursor_gc (struct glyph_string *);
static void x_set_mode_line_face_gc (struct glyph_string *);
static void x_set_mouse_face_gc (struct glyph_string *);
/*static bool x_alloc_lighter_color (struct frame *, Display *, Colormap,
  unsigned long *, double, int);*/
static void x_setup_relief_color (struct frame *, struct relief *,
                                  double, int, unsigned long);
static void x_setup_relief_colors (struct glyph_string *);
static void x_draw_image_glyph_string (struct glyph_string *);
static void x_draw_image_relief (struct glyph_string *);
static void x_draw_image_foreground (struct glyph_string *);
static void x_clear_glyph_string_rect (struct glyph_string *, int,
                                       int, int, int);
static void x_draw_relief_rect (struct frame *, int, int, int, int,
                                int, bool, bool, bool, bool, bool,
				NativeRectangle *);
static void x_draw_box_rect (struct glyph_string *, int, int, int, int,
			     int, bool, bool, NativeRectangle *);
static void x_scroll_bar_clear (struct frame *);

#ifdef GLYPH_DEBUG
static void x_check_font (struct frame *, struct font *);
#endif


/* Set S->gc to a suitable GC for drawing glyph string S in cursor
   face.  */

static void
x_set_cursor_gc (struct glyph_string *s)
{
  if (s->font == FRAME_FONT (s->f)
      && s->face->background == FRAME_BACKGROUND_PIXEL (s->f)
      && s->face->foreground == FRAME_FOREGROUND_PIXEL (s->f)
      && !s->cmp)
    s->gc = s->f->output_data.mac->cursor_gc;
  else
    {
      /* Cursor on non-default face: must merge.  */
      XGCValues xgcv;
      unsigned long mask;

      xgcv.background = s->f->output_data.mac->cursor_pixel;
      xgcv.foreground = s->face->background;

      /* If the glyph would be invisible, try a different foreground.  */
      if (xgcv.foreground == xgcv.background)
	xgcv.foreground = s->face->foreground;
      if (xgcv.foreground == xgcv.background)
	xgcv.foreground = s->f->output_data.mac->cursor_foreground_pixel;
      if (xgcv.foreground == xgcv.background)
	xgcv.foreground = s->face->foreground;

      /* Make sure the cursor is distinct from text in this face.  */
      if (xgcv.background == s->face->background
	  && xgcv.foreground == s->face->foreground)
	{
	  xgcv.background = s->face->foreground;
	  xgcv.foreground = s->face->background;
	}

      IF_DEBUG (x_check_font (s->f, s->font));
      mask = GCForeground | GCBackground;

      if (FRAME_DISPLAY_INFO (s->f)->scratch_cursor_gc)
	XChangeGC (s->display, FRAME_DISPLAY_INFO (s->f)->scratch_cursor_gc,
		   mask, &xgcv);
      else
	FRAME_DISPLAY_INFO (s->f)->scratch_cursor_gc
	  = XCreateGC (s->display, s->window, mask, &xgcv);

      s->gc = FRAME_DISPLAY_INFO (s->f)->scratch_cursor_gc;
    }
}


/* Set up S->gc of glyph string S for drawing text in mouse face.  */

static void
x_set_mouse_face_gc (struct glyph_string *s)
{
  int face_id;
  struct face *face;

  /* What face has to be used last for the mouse face?  */
  face_id = MOUSE_HL_INFO (s->f)->mouse_face_face_id;
  face = FACE_FROM_ID (s->f, face_id);
  if (face == NULL)
    face = FACE_FROM_ID (s->f, MOUSE_FACE_ID);

  if (s->first_glyph->type == CHAR_GLYPH)
    face_id = FACE_FOR_CHAR (s->f, face, s->first_glyph->u.ch, -1, Qnil);
  else
    face_id = FACE_FOR_CHAR (s->f, face, 0, -1, Qnil);
  s->face = FACE_FROM_ID (s->f, face_id);
  prepare_face_for_display (s->f, s->face);

  if (s->font == s->face->font)
    s->gc = s->face->gc;
  else
    {
      /* Otherwise construct scratch_cursor_gc with values from FACE
	 except for FONT.  */
      XGCValues xgcv;
      unsigned long mask;

      xgcv.background = s->face->background;
      xgcv.foreground = s->face->foreground;
      mask = GCForeground | GCBackground;

      if (FRAME_DISPLAY_INFO (s->f)->scratch_cursor_gc)
	XChangeGC (s->display, FRAME_DISPLAY_INFO (s->f)->scratch_cursor_gc,
		   mask, &xgcv);
      else
	FRAME_DISPLAY_INFO (s->f)->scratch_cursor_gc
	  = XCreateGC (s->display, s->window, mask, &xgcv);

      s->gc = FRAME_DISPLAY_INFO (s->f)->scratch_cursor_gc;
    }

  eassert (s->gc != 0);
}


/* Set S->gc of glyph string S to a GC suitable for drawing a mode line.
   Faces to use in the mode line have already been computed when the
   matrix was built, so there isn't much to do, here.  */

static void
x_set_mode_line_face_gc (struct glyph_string *s)
{
  s->gc = s->face->gc;
}


/* Set S->gc of glyph string S for drawing that glyph string.  Set
   S->stippled_p to a non-zero value if the face of S has a stipple
   pattern.  */

static void
x_set_glyph_string_gc (struct glyph_string *s)
{
  prepare_face_for_display (s->f, s->face);

  if (s->hl == DRAW_NORMAL_TEXT)
    {
      s->gc = s->face->gc;
      s->stippled_p = s->face->stipple != 0;
    }
  else if (s->hl == DRAW_INVERSE_VIDEO)
    {
      x_set_mode_line_face_gc (s);
      s->stippled_p = s->face->stipple != 0;
    }
  else if (s->hl == DRAW_CURSOR)
    {
      x_set_cursor_gc (s);
      s->stippled_p = false;
    }
  else if (s->hl == DRAW_MOUSE_FACE)
    {
      x_set_mouse_face_gc (s);
      s->stippled_p = s->face->stipple != 0;
    }
  else if (s->hl == DRAW_IMAGE_RAISED
	   || s->hl == DRAW_IMAGE_SUNKEN)
    {
      s->gc = s->face->gc;
      s->stippled_p = s->face->stipple != 0;
    }
  else
    emacs_abort ();

  /* GC must have been set.  */
  eassert (s->gc != 0);
}


/* Set clipping for output of glyph string S.  S may be part of a mode
   line or menu if we don't have X toolkit support.  */

static void
x_set_glyph_string_clipping (struct glyph_string *s)
{
  NativeRectangle *r = s->clip;
  int n = get_glyph_string_clip_rects (s, r, 2);

  mac_set_clip_rectangles (s->f, s->gc, r, n);
  s->num_clips = n;
}


/* Set SRC's clipping for output of glyph string DST.  This is called
   when we are drawing DST's left_overhang or right_overhang only in
   the area of SRC.  */

static void
x_set_glyph_string_clipping_exactly (struct glyph_string *src, struct glyph_string *dst)
{
  NativeRectangle r;

  STORE_NATIVE_RECT (r, src->x, src->y, src->width, src->height);
  dst->clip[0] = r;
  dst->num_clips = 1;
  mac_set_clip_rectangles (dst->f, dst->gc, &r, 1);
}


/* RIF:
   Compute left and right overhang of glyph string S.  */

static void
x_compute_glyph_string_overhangs (struct glyph_string *s)
{
  if (s->cmp == NULL
      && (s->first_glyph->type == CHAR_GLYPH
	  || s->first_glyph->type == COMPOSITE_GLYPH))
    {
      struct font_metrics metrics;

      if (s->first_glyph->type == CHAR_GLYPH)
	{
	  unsigned *code = alloca (sizeof (unsigned) * s->nchars);
	  struct font *font = s->font;
	  int i;

	  for (i = 0; i < s->nchars; i++)
	    code[i] = s->char2b[i];
	  font->driver->text_extents (font, code, s->nchars, &metrics);
	}
      else
	{
	  Lisp_Object gstring = composition_gstring_from_id (s->cmp_id);

	  composition_gstring_width (gstring, s->cmp_from, s->cmp_to, &metrics);
	}
      s->right_overhang = (metrics.rbearing > metrics.width
			   ? metrics.rbearing - metrics.width : 0);
      s->left_overhang = metrics.lbearing < 0 ? - metrics.lbearing : 0;
    }
  else if (s->cmp)
    {
      s->right_overhang = s->cmp->rbearing - s->cmp->pixel_width;
      s->left_overhang = - s->cmp->lbearing;
    }
}


/* Fill rectangle X, Y, W, H with background color of glyph string S.  */

static void
x_clear_glyph_string_rect (struct glyph_string *s, int x, int y, int w, int h)
{
  mac_erase_rectangle (s->f, s->gc, x, y, w, h);
}


/* Draw the background of glyph_string S.  If S->background_filled_p
   is non-zero don't draw it.  FORCE_P non-zero means draw the
   background even if it wouldn't be drawn normally.  This is used
   when a string preceding S draws into the background of S, or S
   contains the first component of a composition.  */

static void
x_draw_glyph_string_background (struct glyph_string *s, bool force_p)
{
  /* Nothing to do if background has already been drawn or if it
     shouldn't be drawn in the first place.  */
  if (!s->background_filled_p)
    {
      int box_line_width = max (s->face->box_line_width, 0);

#if 0 /* MAC_TODO: stipple */
      if (s->stippled_p)
	{
	  /* Fill background with a stipple pattern.  */
	  XSetFillStyle (s->display, s->gc, FillOpaqueStippled);
	  XFillRectangle (s->display, s->window, s->gc, s->x,
			  s->y + box_line_width,
			  s->background_width,
			  s->height - 2 * box_line_width);
	  XSetFillStyle (s->display, s->gc, FillSolid);
	  s->background_filled_p = true;
	}
      else
#endif
        if (FONT_HEIGHT (s->font) < s->height - 2 * box_line_width
	       /* When xdisp.c ignores FONT_HEIGHT, we cannot trust
		  font dimensions, since the actual glyphs might be
		  much smaller.  So in that case we always clear the
		  rectangle with background color.  */
	       || FONT_TOO_HIGH (s->font)
	       || s->font_not_found_p
	       || s->extends_to_end_of_line_p
	       || force_p)
	{
	  x_clear_glyph_string_rect (s, s->x, s->y + box_line_width,
				     s->background_width,
				     s->height - 2 * box_line_width);
	  s->background_filled_p = true;
	}
    }
}


/* Draw the foreground of glyph string S.  */

static void
x_draw_glyph_string_foreground (struct glyph_string *s)
{
  int i, x;

  /* If first glyph of S has a left box line, start drawing the text
     of S to the right of that box line.  */
  if (s->face->box != FACE_NO_BOX
      && s->first_glyph->left_box_line_p)
    x = s->x + eabs (s->face->box_line_width);
  else
    x = s->x;

  /* Draw characters of S as rectangles if S's font could not be
     loaded.  */
  if (s->font_not_found_p)
    {
      for (i = 0; i < s->nchars; ++i)
	{
	  struct glyph *g = s->first_glyph + i;
	  mac_draw_rectangle (s->f, s->gc, x, s->y,
			      g->pixel_width - 1, s->height - 1);
	  x += g->pixel_width;
	}
    }
  else
    {
      struct font *font = s->font;
      int boff = font->baseline_offset;
      int y;

      if (font->vertical_centering)
	boff = VCENTER_BASELINE_OFFSET (font, s->f) - boff;

      y = s->ybase - boff;
      if (s->for_overlaps
	  || (s->background_filled_p && s->hl != DRAW_CURSOR))
	font->driver->draw (s, 0, s->nchars, x, y, false);
      else
	font->driver->draw (s, 0, s->nchars, x, y, true);
      if (s->face->overstrike)
	font->driver->draw (s, 0, s->nchars, x + 1, y, false);
    }
}

/* Draw the foreground of composite glyph string S.  */

static void
x_draw_composite_glyph_string_foreground (struct glyph_string *s)
{
  int i, j, x;
  struct font *font = s->font;

  /* If first glyph of S has a left box line, start drawing the text
     of S to the right of that box line.  */
  if (s->face && s->face->box != FACE_NO_BOX
      && s->first_glyph->left_box_line_p)
    x = s->x + eabs (s->face->box_line_width);
  else
    x = s->x;

  /* S is a glyph string for a composition.  S->cmp_from is the index
     of the first character drawn for glyphs of this composition.
     S->cmp_from == 0 means we are drawing the very first character of
     this composition.  */

  /* Draw a rectangle for the composition if the font for the very
     first character of the composition could not be loaded.  */
  if (s->font_not_found_p)
    {
      if (s->cmp_from == 0)
	mac_draw_rectangle (s->f, s->gc, x, s->y,
			    s->width - 1, s->height - 1);
    }
  else if (! s->first_glyph->u.cmp.automatic)
    {
      int y = s->ybase;

      for (i = 0, j = s->cmp_from; i < s->nchars; i++, j++)
	/* TAB in a composition means display glyphs with padding
	   space on the left or right.  */
	if (COMPOSITION_GLYPH (s->cmp, j) != '\t')
	  {
	    int xx = x + s->cmp->offsets[j * 2];
	    int yy = y - s->cmp->offsets[j * 2 + 1];

	    font->driver->draw (s, j, j + 1, xx, yy, false);
	    if (s->face->overstrike)
	      font->driver->draw (s, j, j + 1, xx + 1, yy, false);
	  }
    }
  else
    {
      Lisp_Object gstring = composition_gstring_from_id (s->cmp_id);
      Lisp_Object glyph;
      int y = s->ybase;
      int width = 0;

      for (i = j = s->cmp_from; i < s->cmp_to; i++)
	{
	  glyph = LGSTRING_GLYPH (gstring, i);
	  if (NILP (LGLYPH_ADJUSTMENT (glyph)))
	    width += LGLYPH_WIDTH (glyph);
	  else
	    {
	      int xoff, yoff, wadjust;

	      if (j < i)
		{
		  font->driver->draw (s, j, i, x, y, false);
		  if (s->face->overstrike)
		    font->driver->draw (s, j, i, x + 1, y, false);
		  x += width;
		}
	      xoff = LGLYPH_XOFF (glyph);
	      yoff = LGLYPH_YOFF (glyph);
	      wadjust = LGLYPH_WADJUST (glyph);
	      font->driver->draw (s, i, i + 1, x + xoff, y + yoff, false);
	      if (s->face->overstrike)
		font->driver->draw (s, i, i + 1, x + xoff + 1, y + yoff,
				    false);
	      x += wadjust;
	      j = i + 1;
	      width = 0;
	    }
	}
      if (j < i)
	{
	  font->driver->draw (s, j, i, x, y, false);
	  if (s->face->overstrike)
	    font->driver->draw (s, j, i, x + 1, y, false);
	}
    }
}


/* Draw the foreground of glyph string S for glyphless characters.  */

static void
x_draw_glyphless_glyph_string_foreground (struct glyph_string *s)
{
  struct glyph *glyph = s->first_glyph;
  XChar2b char2b[8];
  int x, i, j;

  /* If first glyph of S has a left box line, start drawing the text
     of S to the right of that box line.  */
  if (s->face && s->face->box != FACE_NO_BOX
      && s->first_glyph->left_box_line_p)
    x = s->x + eabs (s->face->box_line_width);
  else
    x = s->x;

  s->char2b = char2b;

  for (i = 0; i < s->nchars; i++, glyph++)
    {
      char buf[7], *str = NULL;
      int len = glyph->u.glyphless.len;

      if (glyph->u.glyphless.method == GLYPHLESS_DISPLAY_ACRONYM)
	{
	  if (len > 0
	      && CHAR_TABLE_P (Vglyphless_char_display)
	      && (CHAR_TABLE_EXTRA_SLOTS (XCHAR_TABLE (Vglyphless_char_display))
		  >= 1))
	    {
	      Lisp_Object acronym
		= (! glyph->u.glyphless.for_no_font
		   ? CHAR_TABLE_REF (Vglyphless_char_display,
				     glyph->u.glyphless.ch)
		   : XCHAR_TABLE (Vglyphless_char_display)->extras[0]);
	      if (STRINGP (acronym))
		str = SSDATA (acronym);
	    }
	}
      else if (glyph->u.glyphless.method == GLYPHLESS_DISPLAY_HEX_CODE)
	{
	  sprintf (buf, "%0*X",
		   glyph->u.glyphless.ch < 0x10000 ? 4 : 6,
		   glyph->u.glyphless.ch + 0u);
	  str = buf;
	}

      if (str)
	{
	  int upper_len = (len + 1) / 2;
	  unsigned code;

	  /* It is assured that all LEN characters in STR is ASCII.  */
	  for (j = 0; j < len; j++)
	    {
	      code = s->font->driver->encode_char (s->font, str[j]);
	      STORE_XCHAR2B (char2b + j, code >> 8, code & 0xFF);
	    }
	  s->font->driver->draw (s, 0, upper_len,
				 x + glyph->slice.glyphless.upper_xoff,
				 s->ybase + glyph->slice.glyphless.upper_yoff,
				 false);
	  s->font->driver->draw (s, upper_len, len,
				 x + glyph->slice.glyphless.lower_xoff,
				 s->ybase + glyph->slice.glyphless.lower_yoff,
				 false);
	}
      if (glyph->u.glyphless.method != GLYPHLESS_DISPLAY_THIN_SPACE)
	mac_draw_rectangle (s->f, s->gc, x, s->ybase - glyph->ascent,
			    glyph->pixel_width - 1,
			    glyph->ascent + glyph->descent - 1);
      x += glyph->pixel_width;
   }
}


/* On frame F, translate pixel colors to RGB values for the NCOLORS
   colors in COLORS.  */

void
x_query_colors (struct frame *f, XColor *colors, int ncolors)
{
  int i;

  for (i = 0; i < ncolors; ++i)
    {
      unsigned long pixel = colors[i].pixel;

      colors[i].red = RED16_FROM_ULONG (pixel);
      colors[i].green = GREEN16_FROM_ULONG (pixel);
      colors[i].blue = BLUE16_FROM_ULONG (pixel);
    }
}


/* On frame F, translate pixel color to RGB values for the color in
   COLOR.  */

void
x_query_color (struct frame *f, XColor *color)
{
  x_query_colors (f, color, 1);
}


/* Brightness beyond which a color won't have its highlight brightness
   boosted.

   Nominally, highlight colors for `3d' faces are calculated by
   brightening an object's color by a constant scale factor, but this
   doesn't yield good results for dark colors, so for colors who's
   brightness is less than this value (on a scale of 0-255) have to
   use an additional additive factor.

   The value here is set so that the default menu-bar/mode-line color
   (grey75) will not have its highlights changed at all.  */
#define HIGHLIGHT_COLOR_DARK_BOOST_LIMIT 187


/* Allocate a color which is lighter or darker than *COLOR by FACTOR
   or DELTA.  Try a color with RGB values multiplied by FACTOR first.
   If this produces the same color as COLOR, try a color where all RGB
   values have DELTA added.  Return the allocated color in *COLOR.
   DISPLAY is the X display, CMAP is the colormap to operate on.
   Value is non-zero if successful.  */

static bool
mac_alloc_lighter_color (struct frame *f, unsigned long *color, double factor,
			 int delta)
{
  unsigned long new;
  long bright;

  /* On Mac, RGB values are 0-255, not 0-65535, so scale delta. */
  delta /= 256;

  /* Change RGB values by specified FACTOR.  Avoid overflow!  */
  eassert (factor >= 0);
  new = RGB_TO_ULONG (min (0xff, (int) (factor * RED_FROM_ULONG (*color))),
                    min (0xff, (int) (factor * GREEN_FROM_ULONG (*color))),
                    min (0xff, (int) (factor * BLUE_FROM_ULONG (*color))));

  /* Calculate brightness of COLOR.  */
  bright = (2 * RED_FROM_ULONG (*color) + 3 * GREEN_FROM_ULONG (*color)
            + BLUE_FROM_ULONG (*color)) / 6;

  /* We only boost colors that are darker than
     HIGHLIGHT_COLOR_DARK_BOOST_LIMIT.  */
  if (bright < HIGHLIGHT_COLOR_DARK_BOOST_LIMIT)
    /* Make an additive adjustment to NEW, because it's dark enough so
       that scaling by FACTOR alone isn't enough.  */
    {
      /* How far below the limit this color is (0 - 1, 1 being darker).  */
      double dimness = 1 - (double)bright / HIGHLIGHT_COLOR_DARK_BOOST_LIMIT;
      /* The additive adjustment.  */
      int min_delta = delta * dimness * factor / 2;

      if (factor < 1)
        new = RGB_TO_ULONG (max (0, min (0xff, (int) (RED_FROM_ULONG (new)) - min_delta)),
			    max (0, min (0xff, (int) (GREEN_FROM_ULONG (new)) - min_delta)),
			    max (0, min (0xff, (int) (BLUE_FROM_ULONG (new)) - min_delta)));
      else
        new = RGB_TO_ULONG (max (0, min (0xff, (int) (min_delta + RED_FROM_ULONG (new)))),
			    max (0, min (0xff, (int) (min_delta + GREEN_FROM_ULONG (new)))),
			    max (0, min (0xff, (int) (min_delta + BLUE_FROM_ULONG (new)))));
    }

  if (new == *color)
    new = RGB_TO_ULONG (max (0, min (0xff, (int) (delta + RED_FROM_ULONG (*color)))),
                      max (0, min (0xff, (int) (delta + GREEN_FROM_ULONG (*color)))),
                      max (0, min (0xff, (int) (delta + BLUE_FROM_ULONG (*color)))));

  /* MAC_TODO: Map to palette and retry with delta if same? */
  /* MAC_TODO: Free colors (if using palette)? */

  if (new == *color)
    return 0;

  *color = new;

  return 1;
}


/* Set up the foreground color for drawing relief lines of glyph
   string S.  RELIEF is a pointer to a struct relief containing the GC
   with which lines will be drawn.  Use a color that is FACTOR or
   DELTA lighter or darker than the relief's background which is found
   in S->f->output_data.mac->relief_background.  If such a color cannot
   be allocated, use DEFAULT_PIXEL, instead.  */

static void
x_setup_relief_color (struct frame *f, struct relief *relief, double factor,
		      int delta, unsigned long default_pixel)
{
  XGCValues xgcv;
  struct mac_output *di = f->output_data.mac;
  unsigned long mask = GCForeground;
  unsigned long pixel;
  unsigned long background = di->relief_background;
  struct mac_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);

  /* MAC_TODO: Free colors (if using palette)? */

  /* Allocate new color.  */
  xgcv.foreground = default_pixel;
  pixel = background;
  if (dpyinfo->n_planes != 1
      && mac_alloc_lighter_color (f, &pixel, factor, delta))
    xgcv.foreground = relief->pixel = pixel;

  if (relief->gc == 0)
    {
#if 0 /* MAC_TODO: stipple */
      xgcv.stipple = dpyinfo->gray;
      mask |= GCStipple;
#endif
      relief->gc = XCreateGC (NULL, FRAME_MAC_WINDOW (f), mask, &xgcv);
    }
  else
    XChangeGC (NULL, relief->gc, mask, &xgcv);
}


/* Set up colors for the relief lines around glyph string S.  */

static void
x_setup_relief_colors (struct glyph_string *s)
{
  struct mac_output *di = s->f->output_data.mac;
  unsigned long color;

  if (s->face->use_box_color_for_shadows_p)
    color = s->face->box_color;
  else if (s->first_glyph->type == IMAGE_GLYPH
	   && s->img->pixmap
	   && !IMAGE_BACKGROUND_TRANSPARENT (s->img, s->f, 0))
    color = IMAGE_BACKGROUND (s->img, s->f, 0);
  else
    {
      XGCValues xgcv;

      /* Get the background color of the face.  */
      XGetGCValues (s->display, s->gc, GCBackground, &xgcv);
      color = xgcv.background;
    }

  if (di->white_relief.gc == 0
      || color != di->relief_background)
    {
      di->relief_background = color;
      x_setup_relief_color (s->f, &di->white_relief, 1.2, 0x8000,
			    WHITE_PIX_DEFAULT (s->f));
      x_setup_relief_color (s->f, &di->black_relief, 0.6, 0x4000,
			    BLACK_PIX_DEFAULT (s->f));
    }
}


/* Draw a relief on frame F inside the rectangle given by LEFT_X,
   TOP_Y, RIGHT_X, and BOTTOM_Y.  WIDTH is the thickness of the relief
   to draw, it must be >= 0.  RAISED_P means draw a raised
   relief.  LEFT_P means draw a relief on the left side of
   the rectangle.  RIGHT_P means draw a relief on the right
   side of the rectangle.  CLIP_RECT is the clipping rectangle to use
   when drawing.  */

static void
x_draw_relief_rect (struct frame *f,
		    int left_x, int top_y, int right_x, int bottom_y,
		    int width, bool raised_p, bool top_p, bool bot_p,
		    bool left_p, bool right_p,
		    NativeRectangle *clip_rect)
{
  GC top_left_gc, bottom_right_gc;
  int corners = 0;

  if (raised_p)
    {
      top_left_gc = f->output_data.mac->white_relief.gc;
      bottom_right_gc = f->output_data.mac->black_relief.gc;
    }
  else
    {
      top_left_gc = f->output_data.mac->black_relief.gc;
      bottom_right_gc = f->output_data.mac->white_relief.gc;
    }

  mac_set_clip_rectangles (f, top_left_gc, clip_rect, 1);
  mac_set_clip_rectangles (f, bottom_right_gc, clip_rect, 1);

  if (left_p)
    {
      mac_fill_rectangle (f, top_left_gc, left_x, top_y,
			  width, bottom_y + 1 - top_y);
      if (top_p)
	corners |= 1 << CORNER_TOP_LEFT;
      if (bot_p)
	corners |= 1 << CORNER_BOTTOM_LEFT;
    }
  if (right_p)
    {
      mac_fill_rectangle (f, bottom_right_gc, right_x + 1 - width, top_y,
			  width, bottom_y + 1 - top_y);
      if (top_p)
	corners |= 1 << CORNER_TOP_RIGHT;
      if (bot_p)
	corners |= 1 << CORNER_BOTTOM_RIGHT;
    }
  if (top_p)
    {
      if (!right_p)
	mac_fill_rectangle (f, top_left_gc, left_x, top_y,
			    right_x + 1 - left_x, width);
      else
	mac_fill_trapezoid_for_relief (f, top_left_gc, left_x, top_y,
				       right_x + 1 - left_x, width, 1);
    }
  if (bot_p)
    {
      if (!left_p)
	mac_fill_rectangle (f, bottom_right_gc, left_x, bottom_y + 1 - width,
			    right_x + 1 - left_x, width);
      else
	mac_fill_trapezoid_for_relief (f, bottom_right_gc,
				       left_x, bottom_y + 1 - width,
				       right_x + 1 - left_x, width, 0);
    }
  if (left_p && width != 1)
    mac_fill_rectangle (f, bottom_right_gc, left_x, top_y,
			1, bottom_y + 1 - top_y);
  if (top_p && width != 1)
    mac_fill_rectangle (f, bottom_right_gc, left_x, top_y,
			right_x + 1 - left_x, 1);
  if (corners)
    {
      XSetBackground (FRAME_MAC_DISPLAY (f), top_left_gc,
		      FRAME_BACKGROUND_PIXEL (f));
      mac_erase_corners_for_relief (f, top_left_gc, left_x, top_y,
				    right_x - left_x + 1, bottom_y - top_y + 1,
				    6, 1, corners);
    }

  mac_reset_clip_rectangles (f, top_left_gc);
  mac_reset_clip_rectangles (f, bottom_right_gc);
}


/* Draw a box on frame F inside the rectangle given by LEFT_X, TOP_Y,
   RIGHT_X, and BOTTOM_Y.  WIDTH is the thickness of the lines to
   draw, it must be >= 0.  LEFT_P means draw a line on the
   left side of the rectangle.  RIGHT_P means draw a line
   on the right side of the rectangle.  CLIP_RECT is the clipping
   rectangle to use when drawing.  */

static void
x_draw_box_rect (struct glyph_string *s,
		 int left_x, int top_y, int right_x, int bottom_y, int width,
		 bool left_p, bool right_p, NativeRectangle *clip_rect)
{
  XGCValues xgcv;

  XGetGCValues (s->display, s->gc, GCForeground, &xgcv);
  XSetForeground (s->display, s->gc, s->face->box_color);
  mac_set_clip_rectangles (s->f, s->gc, clip_rect, 1);

  /* Top.  */
  mac_fill_rectangle (s->f, s->gc, left_x, top_y,
		      right_x - left_x + 1, width);

  /* Left.  */
  if (left_p)
    mac_fill_rectangle (s->f, s->gc, left_x, top_y,
			width, bottom_y - top_y + 1);

  /* Bottom.  */
  mac_fill_rectangle (s->f, s->gc, left_x, bottom_y - width + 1,
		      right_x - left_x + 1, width);

  /* Right.  */
  if (right_p)
    mac_fill_rectangle (s->f, s->gc, right_x - width + 1,
			top_y, width, bottom_y - top_y + 1);

  XSetForeground (s->display, s->gc, xgcv.foreground);
  mac_reset_clip_rectangles (s->f, s->gc);
}


/* Draw a box around glyph string S.  */

static void
x_draw_glyph_string_box (struct glyph_string *s)
{
  int width, left_x, right_x, top_y, bottom_y, last_x;
  bool raised_p, left_p, right_p;
  struct glyph *last_glyph;
  NativeRectangle clip_rect;

  last_x = ((s->row->full_width_p && !s->w->pseudo_window_p)
	    ? WINDOW_RIGHT_EDGE_X (s->w)
	    : window_box_right (s->w, s->area));

  /* The glyph that may have a right box line.  */
  last_glyph = (s->cmp || s->img
		? s->first_glyph
		: s->first_glyph + s->nchars - 1);

  width = eabs (s->face->box_line_width);
  raised_p = s->face->box == FACE_RAISED_BOX;
  left_x = s->x;
  right_x = (s->row->full_width_p && s->extends_to_end_of_line_p
	     ? last_x - 1
	     : min (last_x, s->x + s->background_width) - 1);
  top_y = s->y;
  bottom_y = top_y + s->height - 1;

  left_p = (s->first_glyph->left_box_line_p
	    || (s->hl == DRAW_MOUSE_FACE
		&& (s->prev == NULL
		    || s->prev->hl != s->hl)));
  right_p = (last_glyph->right_box_line_p
	     || (s->hl == DRAW_MOUSE_FACE
		 && (s->next == NULL
		     || s->next->hl != s->hl)));

  get_glyph_string_clip_rect (s, &clip_rect);

  if (s->face->box == FACE_SIMPLE_BOX)
    x_draw_box_rect (s, left_x, top_y, right_x, bottom_y, width,
		     left_p, right_p, &clip_rect);
  else
    {
      x_setup_relief_colors (s);
      x_draw_relief_rect (s->f, left_x, top_y, right_x, bottom_y,
			  width, raised_p, true, true, left_p, right_p,
			  &clip_rect);
    }
}


/* Draw foreground of image glyph string S.  */

static void
x_draw_image_foreground (struct glyph_string *s)
{
  int x = s->x;
  int y = s->ybase - image_ascent (s->img, s->face, &s->slice);

  /* If first glyph of S has a left box line, start drawing it to the
     right of that line.  */
  if (s->face->box != FACE_NO_BOX
      && s->first_glyph->left_box_line_p
      && s->slice.x == 0)
    x += eabs (s->face->box_line_width);

  /* If there is a margin around the image, adjust x- and y-position
     by that margin.  */
  if (s->slice.x == 0)
    x += s->img->hmargin;
  if (s->slice.y == 0)
    y += s->img->vmargin;

  if (s->img->pixmap)
    {
      int flags = MAC_DRAW_CG_IMAGE_OVERLAY;

      x_set_glyph_string_clipping (s);

      mac_detect_scale_mismatch (s->f, s->img->target_backing_scale);
      if (s->img->target_backing_scale == 2)
	flags |= MAC_DRAW_CG_IMAGE_2X;
      mac_draw_cg_image (s->img->cg_image,
			 s->f, s->gc, s->slice.x, s->slice.y,
			 s->slice.width, s->slice.height, x, y, flags);
      if (!s->img->mask)
	{
	  /* When the image has a mask, we can expect that at
	     least part of a mouse highlight or a block cursor will
	     be visible.  If the image doesn't have a mask, make
	     a block cursor visible by drawing a rectangle around
	     the image.  I believe it's looking better if we do
	     nothing here for mouse-face.  */
	  if (s->hl == DRAW_CURSOR)
	    {
	      int relief = eabs (s->img->relief);
	      mac_draw_rectangle (s->f, s->gc, x - relief, y - relief,
				  s->slice.width + relief*2 - 1,
				  s->slice.height + relief*2 - 1);
	    }
	}
    }
  else
    /* Draw a rectangle if image could not be loaded.  */
    mac_draw_rectangle (s->f, s->gc, x, y,
			s->slice.width - 1, s->slice.height - 1);
}


/* Draw a relief around the image glyph string S.  */

static void
x_draw_image_relief (struct glyph_string *s)
{
  int x1, y1, thick;
  bool raised_p, top_p, bot_p, left_p, right_p;
  int extra_x, extra_y;
  NativeRectangle r;
  int x = s->x;
  int y = s->ybase - image_ascent (s->img, s->face, &s->slice);

  /* If first glyph of S has a left box line, start drawing it to the
     right of that line.  */
  if (s->face->box != FACE_NO_BOX
      && s->first_glyph->left_box_line_p
      && s->slice.x == 0)
    x += eabs (s->face->box_line_width);

  /* If there is a margin around the image, adjust x- and y-position
     by that margin.  */
  if (s->slice.x == 0)
    x += s->img->hmargin;
  if (s->slice.y == 0)
    y += s->img->vmargin;

  if (s->hl == DRAW_IMAGE_SUNKEN
      || s->hl == DRAW_IMAGE_RAISED)
    {
      thick = tool_bar_button_relief >= 0 ? tool_bar_button_relief : DEFAULT_TOOL_BAR_BUTTON_RELIEF;
      raised_p = s->hl == DRAW_IMAGE_RAISED;
    }
  else
    {
      thick = eabs (s->img->relief);
      raised_p = s->img->relief > 0;
    }

  x1 = x + s->slice.width - 1;
  y1 = y + s->slice.height - 1;

  extra_x = extra_y = 0;
  if (s->face->id == TOOL_BAR_FACE_ID)
    {
      if (CONSP (Vtool_bar_button_margin)
	  && INTEGERP (XCAR (Vtool_bar_button_margin))
	  && INTEGERP (XCDR (Vtool_bar_button_margin)))
	{
	  extra_x = XINT (XCAR (Vtool_bar_button_margin));
	  extra_y = XINT (XCDR (Vtool_bar_button_margin));
	}
      else if (INTEGERP (Vtool_bar_button_margin))
	extra_x = extra_y = XINT (Vtool_bar_button_margin);
    }

  top_p = bot_p = left_p = right_p = false;

  if (s->slice.x == 0)
    x -= thick + extra_x, left_p = true;
  if (s->slice.y == 0)
    y -= thick + extra_y, top_p = true;
  if (s->slice.x + s->slice.width == s->img->width)
    x1 += thick + extra_x, right_p = true;
  if (s->slice.y + s->slice.height == s->img->height)
    y1 += thick + extra_y, bot_p = true;

  x_setup_relief_colors (s);
  get_glyph_string_clip_rect (s, &r);
  x_draw_relief_rect (s->f, x, y, x1, y1, thick, raised_p,
		      top_p, bot_p, left_p, right_p, &r);
}


/* Draw part of the background of glyph string S.  X, Y, W, and H
   give the rectangle to draw.  */

static void
x_draw_glyph_string_bg_rect (struct glyph_string *s, int x, int y, int w, int h)
{
#if 0 /* MAC_TODO: stipple */
  if (s->stippled_p)
    {
      /* Fill background with a stipple pattern.  */
      XSetFillStyle (s->display, s->gc, FillOpaqueStippled);
      XFillRectangle (s->display, s->window, s->gc, x, y, w, h);
      XSetFillStyle (s->display, s->gc, FillSolid);
    }
  else
#endif /* MAC_TODO */
    x_clear_glyph_string_rect (s, x, y, w, h);
}


/* Draw image glyph string S.

            s->y
   s->x      +-------------------------
	     |   s->face->box
	     |
	     |     +-------------------------
	     |     |  s->img->margin
	     |     |
	     |     |       +-------------------
	     |     |       |  the image

 */

static void
x_draw_image_glyph_string (struct glyph_string *s)
{
  int box_line_hwidth = eabs (s->face->box_line_width);
  int box_line_vwidth = max (s->face->box_line_width, 0);
  int height;

  height = s->height;
  if (s->slice.y == 0)
    height -= box_line_vwidth;
  if (s->slice.y + s->slice.height >= s->img->height)
    height -= box_line_vwidth;

  /* Fill background with face under the image.  Do it only if row is
     taller than image or if image has a clip mask to reduce
     flickering.  */
  s->stippled_p = s->face->stipple != 0;
  if (height > s->slice.height
      || s->img->hmargin
      || s->img->vmargin
      || s->img->mask
      || s->img->pixmap == 0
      || s->width != s->background_width)
    {
      int x = s->x;
      int y = s->y;
      int width = s->background_width;

      if (s->first_glyph->left_box_line_p
	  && s->slice.x == 0)
	{
	  x += box_line_hwidth;
	  width -= box_line_hwidth;
	}

      if (s->slice.y == 0)
	y += box_line_vwidth;

      x_draw_glyph_string_bg_rect (s, x, y, width, height);

      s->background_filled_p = true;
    }

  /* Draw the foreground.  */
  x_draw_image_foreground (s);

  /* If we must draw a relief around the image, do it.  */
  if (s->img->relief
      || s->hl == DRAW_IMAGE_RAISED
      || s->hl == DRAW_IMAGE_SUNKEN)
    x_draw_image_relief (s);
}


/* Draw stretch glyph string S.  */

static void
x_draw_stretch_glyph_string (struct glyph_string *s)
{
  eassert (s->first_glyph->type == STRETCH_GLYPH);

  if (s->hl == DRAW_CURSOR
      && !x_stretch_cursor_p)
    {
      /* If `x-stretch-cursor' is nil, don't draw a block cursor as
	 wide as the stretch glyph.  */
      int width, background_width = s->background_width;
      int x = s->x;

      if (!s->row->reversed_p)
	{
	  int left_x = window_box_left_offset (s->w, TEXT_AREA);

	  if (x < left_x)
	    {
	      background_width -= left_x - x;
	      x = left_x;
	    }
	}
      else
	{
	  /* In R2L rows, draw the cursor on the right edge of the
	     stretch glyph.  */
	  int right_x = window_box_right (s->w, TEXT_AREA);

	  if (x + background_width > right_x)
	    background_width -= x - right_x;
	  x += background_width;
	}
      width = min (FRAME_COLUMN_WIDTH (s->f), background_width);
      if (s->row->reversed_p)
	x -= width;

      /* Draw cursor.  */
      x_draw_glyph_string_bg_rect (s, x, s->y, width, s->height);

      /* Clear rest using the GC of the original non-cursor face.  */
      if (width < background_width)
	{
	  int y = s->y;
	  int w = background_width - width, h = s->height;
	  NativeRectangle r;
	  GC gc;

	  if (!s->row->reversed_p)
	    x += width;
	  else
	    x = s->x;
	  if (s->row->mouse_face_p
	      && cursor_in_mouse_face_p (s->w))
	    {
	      x_set_mouse_face_gc (s);
	      gc = s->gc;
	    }
	  else
	    gc = s->face->gc;

	  get_glyph_string_clip_rect (s, &r);
	  mac_set_clip_rectangles (s->f, gc, &r, 1);

#if 0 /* MAC_TODO: stipple */
	  if (s->face->stipple)
	    {
	      /* Fill background with a stipple pattern.  */
	      XSetFillStyle (s->display, gc, FillOpaqueStippled);
	      XFillRectangle (s->display, s->window, gc, x, y, w, h);
	      XSetFillStyle (s->display, gc, FillSolid);
	    }
	  else
#endif /* MAC_TODO */
	    mac_erase_rectangle (s->f, gc, x, y, w, h);

	  mac_reset_clip_rectangles (s->f, gc);
	}
    }
  else if (!s->background_filled_p)
    {
      int background_width = s->background_width;
      int x = s->x, left_x = window_box_left_offset (s->w, TEXT_AREA);

      /* Don't draw into left margin, fringe or scrollbar area
         except for header line and mode line.  */
      if (x < left_x && !s->row->mode_line_p)
	{
	  background_width -= left_x - x;
	  x = left_x;
	}
      if (background_width > 0)
	x_draw_glyph_string_bg_rect (s, x, s->y, background_width, s->height);
    }

  s->background_filled_p = true;
}

/*
   Draw a wavy line under S. The wave fills wave_height pixels from y0.

                    x0         wave_length = 2
                                 --
                y0   *   *   *   *   *
                     |* * * * * * * * *
    wave_height = 3  | *   *   *   *

*/

static void
x_draw_underwave (struct glyph_string *s)
{
  int wave_height = 3, wave_length = 2;

  mac_draw_horizontal_wave (s->f, s->gc, s->x, s->ybase - wave_height + 3,
			    s->width, wave_height, wave_length);
}


/* Draw glyph string S.  */

static void
x_draw_glyph_string (struct glyph_string *s)
{
  bool relief_drawn_p = false;

  /* If S draws into the background of its successors, draw the
     background of the successors first so that S can draw into it.
     This makes S->next use XDrawString instead of XDrawImageString.  */
  if (s->next && s->right_overhang && !s->for_overlaps)
    {
      int width;
      struct glyph_string *next;

      for (width = 0, next = s->next;
	   next && width < s->right_overhang;
	   width += next->width, next = next->next)
	if (next->first_glyph->type != IMAGE_GLYPH)
	  {
	    x_set_glyph_string_gc (next);
	    x_set_glyph_string_clipping (next);
	    if (next->first_glyph->type == STRETCH_GLYPH)
	      x_draw_stretch_glyph_string (next);
	    else
	      x_draw_glyph_string_background (next, true);
	    next->num_clips = 0;
	  }
    }

  /* Set up S->gc, set clipping and draw S.  */
  x_set_glyph_string_gc (s);

  /* Draw relief (if any) in advance for char/composition so that the
     glyph string can be drawn over it.  */
  if (!s->for_overlaps
      && s->face->box != FACE_NO_BOX
      && (s->first_glyph->type == CHAR_GLYPH
	  || s->first_glyph->type == COMPOSITE_GLYPH))

    {
      x_set_glyph_string_clipping (s);
      x_draw_glyph_string_background (s, true);
      x_draw_glyph_string_box (s);
      x_set_glyph_string_clipping (s);
      relief_drawn_p = true;
    }
  else if (!s->clip_head /* draw_glyphs didn't specify a clip mask. */
	   && !s->clip_tail
	   && ((s->prev && s->prev->hl != s->hl && s->left_overhang)
	       || (s->next && s->next->hl != s->hl && s->right_overhang)))
    /* We must clip just this glyph.  left_overhang part has already
       drawn when s->prev was drawn, and right_overhang part will be
       drawn later when s->next is drawn. */
    x_set_glyph_string_clipping_exactly (s, s);
  else
    x_set_glyph_string_clipping (s);

  switch (s->first_glyph->type)
    {
    case IMAGE_GLYPH:
      x_draw_image_glyph_string (s);
      break;

    case STRETCH_GLYPH:
      x_draw_stretch_glyph_string (s);
      break;

    case CHAR_GLYPH:
      if (s->for_overlaps)
	s->background_filled_p = true;
      else
	x_draw_glyph_string_background (s, false);
      x_draw_glyph_string_foreground (s);
      break;

    case COMPOSITE_GLYPH:
      if (s->for_overlaps || (s->cmp_from > 0
			      && ! s->first_glyph->u.cmp.automatic))
	s->background_filled_p = true;
      else
	x_draw_glyph_string_background (s, true);
      x_draw_composite_glyph_string_foreground (s);
      break;

    case GLYPHLESS_GLYPH:
      if (s->for_overlaps)
	s->background_filled_p = true;
      else
	x_draw_glyph_string_background (s, true);
      x_draw_glyphless_glyph_string_foreground (s);
      break;

    default:
      emacs_abort ();
    }

  if (!s->for_overlaps)
    {
      /* Draw underline.  */
      if (s->face->underline_p)
	{
	  if (s->face->underline_type == FACE_UNDER_WAVE)
	    {
	      if (s->face->underline_defaulted_p)
		x_draw_underwave (s);
	      else
		{
		  XGCValues xgcv;
		  XGetGCValues (s->display, s->gc, GCForeground, &xgcv);
		  XSetForeground (s->display, s->gc, s->face->underline_color);
		  x_draw_underwave (s);
		  XSetForeground (s->display, s->gc, xgcv.foreground);
		}
	    }
	  else if (s->face->underline_type == FACE_UNDER_LINE)
	    {
	      unsigned long thickness, position;
	      int y;

	      if (s->prev && s->prev->face->underline_p
		  && s->prev->face->underline_type == FACE_UNDER_LINE)
		{
		  /* We use the same underline style as the previous one.  */
		  thickness = s->prev->underline_thickness;
		  position = s->prev->underline_position;
		}
	      else
		{
		  /* Get the underline thickness.  Default is 1 pixel.  */
		  if (s->font && s->font->underline_thickness > 0)
		    thickness = s->font->underline_thickness;
		  else
		    thickness = 1;
		  if (x_underline_at_descent_line)
		    position = (s->height - thickness) - (s->ybase - s->y);
		  else
		    {
		      /* Get the underline position.  This is the recommended
			 vertical offset in pixels from the baseline to the top of
			 the underline.  This is a signed value according to the
			 specs, and its default is

			 ROUND ((maximum descent) / 2), with
			 ROUND(x) = floor (x + 0.5)  */

		      if (x_use_underline_position_properties
			  && s->font && s->font->underline_position >= 0)
			position = s->font->underline_position;
		      else if (s->font)
			position = (s->font->descent + 1) / 2;
		      else
			position = underline_minimum_offset;
		    }
		  position = max (position, underline_minimum_offset);
		}
	      /* Check the sanity of thickness and position.  We should
		 avoid drawing underline out of the current line area.  */
	      if (s->y + s->height <= s->ybase + position)
		position = (s->height - 1) - (s->ybase - s->y);
	      if (s->y + s->height < s->ybase + position + thickness)
		thickness = (s->y + s->height) - (s->ybase + position);
	      s->underline_thickness = thickness;
	      s->underline_position = position;
	      y = s->ybase + position;
	      if (s->face->underline_defaulted_p)
		mac_fill_rectangle (s->f, s->gc, s->x, y, s->width, thickness);
	      else
		{
		  XGCValues xgcv;
		  XGetGCValues (s->display, s->gc, GCForeground, &xgcv);
		  XSetForeground (s->display, s->gc, s->face->underline_color);
		  mac_fill_rectangle (s->f, s->gc, s->x, y, s->width, thickness);
		  XSetForeground (s->display, s->gc, xgcv.foreground);
		}
	    }
	}

      /* Draw overline.  */
      if (s->face->overline_p)
	{
	  unsigned long dy = 0, h = 1;

	  if (s->face->overline_color_defaulted_p)
	    mac_fill_rectangle (s->f, s->gc, s->x, s->y + dy, s->width, h);
	  else
	    {
	      XGCValues xgcv;
	      XGetGCValues (s->display, s->gc, GCForeground, &xgcv);
	      XSetForeground (s->display, s->gc, s->face->overline_color);
	      mac_fill_rectangle (s->f, s->gc, s->x, s->y + dy, s->width, h);
	      XSetForeground (s->display, s->gc, xgcv.foreground);
	    }
	}

      /* Draw strike-through.  */
      if (s->face->strike_through_p)
	{
	  unsigned long h = 1;
	  unsigned long dy = (s->height - h) / 2;

	  if (s->face->strike_through_color_defaulted_p)
	    mac_fill_rectangle (s->f, s->gc, s->x, s->y + dy, s->width, h);
	  else
	    {
	      XGCValues xgcv;
	      XGetGCValues (s->display, s->gc, GCForeground, &xgcv);
	      XSetForeground (s->display, s->gc, s->face->strike_through_color);
	      mac_fill_rectangle (s->f, s->gc, s->x, s->y + dy, s->width, h);
	      XSetForeground (s->display, s->gc, xgcv.foreground);
	    }
	}

      /* Draw relief if not yet drawn.  */
      if (!relief_drawn_p && s->face->box != FACE_NO_BOX)
	x_draw_glyph_string_box (s);

      if (s->prev)
	{
	  struct glyph_string *prev;

	  for (prev = s->prev; prev; prev = prev->prev)
	    if (prev->hl != s->hl
		&& prev->x + prev->width + prev->right_overhang > s->x)
	      {
		/* As prev was drawn while clipped to its own area, we
		   must draw the right_overhang part using s->hl now.  */
		enum draw_glyphs_face save = prev->hl;

		prev->hl = s->hl;
		x_set_glyph_string_gc (prev);
		x_set_glyph_string_clipping_exactly (s, prev);
		if (prev->first_glyph->type == CHAR_GLYPH)
		  x_draw_glyph_string_foreground (prev);
		else
		  x_draw_composite_glyph_string_foreground (prev);
		mac_reset_clip_rectangles (prev->f, prev->gc);
		prev->hl = save;
		prev->num_clips = 0;
	      }
	}

      if (s->next)
	{
	  struct glyph_string *next;

	  for (next = s->next; next; next = next->next)
	    if (next->hl != s->hl
		&& next->x - next->left_overhang < s->x + s->width)
	      {
		/* As next will be drawn while clipped to its own area,
		   we must draw the left_overhang part using s->hl now.  */
		enum draw_glyphs_face save = next->hl;

		next->hl = s->hl;
		x_set_glyph_string_gc (next);
		x_set_glyph_string_clipping_exactly (s, next);
		if (next->first_glyph->type == CHAR_GLYPH)
		  x_draw_glyph_string_foreground (next);
		else
		  x_draw_composite_glyph_string_foreground (next);
		mac_reset_clip_rectangles (next->f, next->gc);
		next->hl = save;
		next->num_clips = 0;
		next->clip_head = s->next;
	      }
	}
    }

  /* Reset clipping.  */
  mac_reset_clip_rectangles (s->f, s->gc);
  s->num_clips = 0;
}

/* Shift display to make room for inserted glyphs.   */

static void
mac_shift_glyphs_for_insert (struct frame *f, int x, int y, int width, int height, int shift_by)
{
  mac_scroll_area (f, f->output_data.mac->normal_gc,
		   x, y, width, height,
		   x + shift_by, y);
}

/* Delete N glyphs at the nominal cursor position.  Not implemented
   for X frames.  */

static void
x_delete_glyphs (struct frame *f, register int n)
{
  emacs_abort ();
}


/* Clear an entire frame.  */

static void
x_clear_frame (struct frame *f)
{
  /* Clearing the frame will erase any cursor, so mark them all as no
     longer visible.  */
  mark_window_cursors_off (XWINDOW (FRAME_ROOT_WINDOW (f)));

  block_input ();

  mac_clear_window (f);

  /* We have to clear the scroll bars.  If we have changed colors or
     something like that, then they should be notified.  */
  x_scroll_bar_clear (f);

  XFlush (FRAME_MAC_DISPLAY (f));
  unblock_input ();
}


/* Invert the middle quarter of the frame for .15 sec.  */

static void
mac_flash (struct frame *f)
{
  /* Get the height not including a menu bar widget.  */
  int height = FRAME_PIXEL_HEIGHT (f);
  /* Height of each line to flash.  */
  int flash_height = FRAME_LINE_HEIGHT (f);
  /* These will be the left and right margins of the rectangles.  */
  int flash_left = FRAME_INTERNAL_BORDER_WIDTH (f);
  int flash_right = (FRAME_TEXT_COLS_TO_PIXEL_WIDTH (f, FRAME_COLS (f))
		     - FRAME_INTERNAL_BORDER_WIDTH (f));
  int width = flash_right - flash_left;
  NativeRectangle rects[2];
  int nrects;
  CGRect clip_rect;

  if (height > 3 * FRAME_LINE_HEIGHT (f))
    {
      /* If window is tall, flash top and bottom line.  */
      STORE_NATIVE_RECT (rects[0],
			 flash_left, (FRAME_INTERNAL_BORDER_WIDTH (f)
				      + FRAME_TOP_MARGIN_HEIGHT (f)),
			 width, flash_height);
      rects[1] = rects[0];
      rects[1].y = height - flash_height - FRAME_INTERNAL_BORDER_WIDTH (f);
      nrects = 2;
      clip_rect = mac_rect_make (f, flash_left, rects[0].y, width,
				 rects[1].y + rects[1].height - rects[0].y);
    }
  else
    {
      /* If it is short, flash it all.  */
      STORE_NATIVE_RECT (rects[0],
			 flash_left, FRAME_INTERNAL_BORDER_WIDTH (f),
			 width, height - 2 * FRAME_INTERNAL_BORDER_WIDTH (f));
      nrects = 1;
      clip_rect = mac_rect_make (f, flash_left, rects[0].y, width,
				 rects[0].height);
    }

  block_input ();

  mac_invert_rectangles (f, rects, nrects);
  mac_mask_rounded_bottom_corners (f, clip_rect, true);

  x_flush (f);
  if (!(mac_operating_system_version.major == 10
	&& mac_operating_system_version.minor <= 10))
    mac_run_loop_run_once (0);

  {
    struct timespec delay = make_timespec (0, 150 * 1000 * 1000);
    struct timespec wakeup = timespec_add (current_timespec (), delay);

    /* Keep waiting until past the time wakeup or any input gets
       available.  */
    while (! detect_input_pending ())
      {
	struct timespec current = current_timespec ();
	struct timespec timeout;

	/* Break if result would not be positive.  */
	if (timespec_cmp (wakeup, current) <= 0)
	  break;

	/* How long `select' should wait.  */
	timeout = make_timespec (0, 10 * 1000 * 1000);

	/* Try to wait that long--but we might wake up sooner.  */
	pselect (0, NULL, NULL, NULL, &timeout, NULL);
      }
  }

  mac_invert_rectangles (f, rects, nrects);
  if (FRAME_BACKGROUND_ALPHA_ENABLED_P (f)
      && !mac_accessibility_display_options.reduce_transparency_p)
    mac_invalidate_rectangles (f, rects, nrects);
  else
    mac_mask_rounded_bottom_corners (f, clip_rect, false);

  x_flush (f);
  if (!(mac_operating_system_version.major == 10
	&& mac_operating_system_version.minor <= 10))
    mac_run_loop_run_once (0);

  unblock_input ();
}



/* Make audible bell.  */

static void
mac_ring_bell (struct frame *f)
{
  if (visible_bell)
    mac_flash (f);
  else
    {
      block_input ();
      mac_alert_sound_play ();
      XFlush (FRAME_MAC_DISPLAY (f));
      unblock_input ();
    }
}

/***********************************************************************
			      Line Dance
 ***********************************************************************/

/* Perform an insert-lines or delete-lines operation, inserting N
   lines or deleting -N lines at vertical position VPOS.  */

static void
x_ins_del_lines (struct frame *f, int vpos, int n)
{
  emacs_abort ();
}


/* Scroll part of the display as described by RUN.  */

static void
x_scroll_run (struct window *w, struct run *run)
{
  struct frame *f = XFRAME (w->frame);
  int x, y, width, height, from_y, to_y, bottom_y;

  /* Get frame-relative bounding box of the text display area of W,
     without mode lines.  Include in this box the left and right
     fringe of W.  */
  window_box (w, ANY_AREA, &x, &y, &width, &height);

  from_y = WINDOW_TO_FRAME_PIXEL_Y (w, run->current_y);
  to_y = WINDOW_TO_FRAME_PIXEL_Y (w, run->desired_y);
  bottom_y = y + height;

  if (to_y < from_y)
    {
      /* Scrolling up.  Make sure we don't copy part of the mode
	 line at the bottom.  */
      if (from_y + run->height > bottom_y)
	height = bottom_y - from_y;
      else
	height = run->height;
    }
  else
    {
      /* Scrolling down.  Make sure we don't copy over the mode line.
	 at the bottom.  */
      if (to_y + run->height > bottom_y)
	height = bottom_y - to_y;
      else
	height = run->height;
    }

  block_input ();

  /* Cursor off.  Will be switched on again in x_update_window_end.  */
  x_clear_cursor (w);

  mac_scroll_area (f, f->output_data.mac->normal_gc,
		   x, from_y,
		   width, height,
		   x, to_y);

  unblock_input ();
}



/***********************************************************************
			   Exposure Events
 ***********************************************************************/


static void
frame_highlight (struct frame *f)
{
  x_update_cursor (f, true);
  block_input ();
  x_set_frame_alpha (f);
  unblock_input ();
}

static void
frame_unhighlight (struct frame *f)
{
  x_update_cursor (f, true);
  block_input ();
  x_set_frame_alpha (f);
  unblock_input ();
}

/* The focus has changed.  Update the frames as necessary to reflect
   the new situation.  Note that we can't change the selected frame
   here, because the Lisp code we are interrupting might become confused.
   Each event gets marked with the frame in which it occurred, so the
   Lisp code can tell when the switch took place by examining the events.  */

static void
x_new_focus_frame (struct x_display_info *dpyinfo, struct frame *frame)
{
  struct frame *old_focus = dpyinfo->x_focus_frame;

  if (frame != dpyinfo->x_focus_frame)
    {
      /* Set this before calling other routines, so that they see
	 the correct value of x_focus_frame.  */
      dpyinfo->x_focus_frame = frame;

      if (old_focus && old_focus->auto_lower)
	x_lower_frame (old_focus);

      if (dpyinfo->x_focus_frame && dpyinfo->x_focus_frame->auto_raise)
	dpyinfo->x_pending_autoraise_frame = dpyinfo->x_focus_frame;
      else
	dpyinfo->x_pending_autoraise_frame = NULL;

      if (frame)
	mac_set_font_info_for_selection (frame, DEFAULT_FACE_ID, 0, -1, Qnil);
    }

  x_frame_rehighlight (dpyinfo);
}

/* Handle FocusIn and FocusOut state changes for FRAME.
   If FRAME has focus and there exists more than one frame, puts
   a FOCUS_IN_EVENT into *BUFP.  */

void
mac_focus_changed (int type, struct mac_display_info *dpyinfo, struct frame *frame, struct input_event *bufp)
{
  if (type == activeFlag)
    {
      if (dpyinfo->x_focus_event_frame != frame)
        {
          x_new_focus_frame (dpyinfo, frame);
          dpyinfo->x_focus_event_frame = frame;

          /* Don't stop displaying the initial startup message
             for a switch-frame event we don't need.  */
          /* When run as a daemon, Vterminal_frame is always NIL.  */
          bufp->arg = (((NILP (Vterminal_frame)
                         || ! FRAME_MAC_P (XFRAME (Vterminal_frame))
                         || EQ (Fdaemonp (), Qt))
			&& CONSP (Vframe_list)
			&& !NILP (XCDR (Vframe_list)))
		       ? Qt : Qnil);
          bufp->kind = FOCUS_IN_EVENT;
          XSETFRAME (bufp->frame_or_window, frame);
        }
    }
  else
    {
      if (dpyinfo->x_focus_event_frame == frame)
        {
          dpyinfo->x_focus_event_frame = 0;
          x_new_focus_frame (dpyinfo, 0);

          bufp->kind = FOCUS_OUT_EVENT;
          XSETFRAME (bufp->frame_or_window, frame);
        }
    }
}

/* Handle an event saying the mouse has moved out of an Emacs frame.  */

void
x_mouse_leave (struct x_display_info *dpyinfo)
{
  x_new_focus_frame (dpyinfo, dpyinfo->x_focus_event_frame);
}

/* The focus has changed, or we have redirected a frame's focus to
   another frame (this happens when a frame uses a surrogate
   mini-buffer frame).  Shift the highlight as appropriate.

   The FRAME argument doesn't necessarily have anything to do with which
   frame is being highlighted or un-highlighted; we only use it to find
   the appropriate X display info.  */

static void
mac_frame_rehighlight (struct frame *frame)
{
  x_frame_rehighlight (FRAME_DISPLAY_INFO (frame));
}

static void
x_frame_rehighlight (struct x_display_info *dpyinfo)
{
  struct frame *old_highlight = dpyinfo->x_highlight_frame;

  if (dpyinfo->x_focus_frame)
    {
      dpyinfo->x_highlight_frame
	= ((FRAMEP (FRAME_FOCUS_FRAME (dpyinfo->x_focus_frame)))
	   ? XFRAME (FRAME_FOCUS_FRAME (dpyinfo->x_focus_frame))
	   : dpyinfo->x_focus_frame);
      if (! FRAME_LIVE_P (dpyinfo->x_highlight_frame))
	{
	  fset_focus_frame (dpyinfo->x_focus_frame, Qnil);
	  dpyinfo->x_highlight_frame = dpyinfo->x_focus_frame;
	}
    }
  else
    dpyinfo->x_highlight_frame = 0;

  if (dpyinfo->x_highlight_frame != old_highlight)
    {
      if (old_highlight)
	frame_unhighlight (old_highlight);
      if (dpyinfo->x_highlight_frame)
	frame_highlight (dpyinfo->x_highlight_frame);
    }
}



/* Convert a keysym to its name.  */

char *
x_get_keysym_name (int keysym)
{
  char *value;

  block_input ();
#if 0
  value = XKeysymToString (keysym);
#else
  value = 0;
#endif
  unblock_input ();

  return value;
}



/************************************************************************
			      Mouse Face
 ************************************************************************/

struct frame *
mac_focus_frame (struct mac_display_info *dpyinfo)
{
  if (dpyinfo->x_focus_frame)
    return dpyinfo->x_focus_frame;
  else
    /* Mac version may get events, such as a menu bar click, even when
       all the frames are invisible.  In this case, we regard the
       event came to the selected frame.  */
    return SELECTED_FRAME ();
}


/* Return the current position of the mouse.
   *FP should be a frame which indicates which display to ask about.

   If the mouse movement started in a scroll bar, set *FP, *BAR_WINDOW,
   and *PART to the frame, window, and scroll bar part that the mouse
   is over.  Set *X and *Y to the portion and whole of the mouse's
   position on the scroll bar.

   If the mouse movement started elsewhere, set *FP to the frame the
   mouse is on, *BAR_WINDOW to nil, and *X and *Y to the character cell
   the mouse is over.

   Set *TIMESTAMP to the server time-stamp for the time at which the mouse
   was at this position.

   Don't store anything if we don't have a valid set of values to report.

   This clears the mouse_moved flag, so we can wait for the next mouse
   movement.  */

static void
mac_mouse_position (struct frame **fp, int insist, Lisp_Object *bar_window,
		    enum scroll_bar_part *part, Lisp_Object *x, Lisp_Object *y,
		    Time *timestamp)
{
  struct frame *f1;
  struct x_display_info *dpyinfo = FRAME_DISPLAY_INFO (*fp);

  block_input ();

  {
    Lisp_Object frame, tail;

    /* Clear the mouse-moved flag for every frame on this display.  */
    FOR_EACH_FRAME (tail, frame)
      if (FRAME_MAC_P (XFRAME (frame)))
	XFRAME (frame)->mouse_moved = false;

    if (x_mouse_grabbed (dpyinfo))
      f1 = dpyinfo->last_mouse_frame;
    else
      f1 = mac_focus_frame (FRAME_DISPLAY_INFO (*fp));

    if (f1)
      {
	/* Ok, we found a frame.  Store all the values.
	   last_mouse_glyph is a rectangle used to reduce the
	   generation of mouse events.  To not miss any motion events,
	   we must divide the frame into rectangles of the size of the
	   smallest character that could be displayed on it, i.e. into
	   the same rectangles that matrices on the frame are divided
	   into.  */
	CGPoint mouse_pos = mac_get_frame_mouse (f1);

	/* FIXME: what if F1 is not an X frame?  */
	dpyinfo = FRAME_DISPLAY_INFO (f1);
	remember_mouse_glyph (f1, mouse_pos.x, mouse_pos.y,
			      &dpyinfo->last_mouse_glyph);
	dpyinfo->last_mouse_glyph_frame = f1;

	*bar_window = Qnil;
	*part = 0;
	*fp = f1;
	XSETINT (*x, mouse_pos.x);
	XSETINT (*y, mouse_pos.y);
	*timestamp = dpyinfo->last_mouse_movement_time;
      }
  }

  unblock_input ();
}


/************************************************************************
			 Scroll bars, general
 ************************************************************************/

/* Create a scroll bar and return the scroll bar vector for it.  W is
   the Emacs window on which to create the scroll bar. TOP, LEFT,
   WIDTH and HEIGHT are the pixel coordinates and dimensions of the
   scroll bar. */

static struct scroll_bar *
x_scroll_bar_create (struct window *w, int top, int left,
		     int width, int height, bool horizontal)
{
  struct frame *f = XFRAME (w->frame);
  struct scroll_bar *bar
    = ALLOCATE_PSEUDOVECTOR (struct scroll_bar, mac_control_ref, PVEC_OTHER);
  Lisp_Object barobj;

  block_input ();

  XSETWINDOW (bar->window, w);
  bar->top = top;
  bar->left = left;
  bar->width = width;
  bar->height = height;
  bar->redraw_needed_p = 0;
  bar->horizontal = horizontal;

  mac_create_scroll_bar (bar);

  /* Add bar to its frame's list of scroll bars.  */
  bar->next = FRAME_SCROLL_BARS (f);
  bar->prev = Qnil;
  XSETVECTOR (barobj, bar);
  fset_scroll_bars (f, barobj);
  if (!NILP (bar->next))
    XSETVECTOR (XSCROLL_BAR (bar->next)->prev, bar);

  unblock_input ();
  return bar;
}


/* Destroy scroll bar BAR, and set its Emacs window's scroll bar to
   nil.  */

static void
x_scroll_bar_remove (struct scroll_bar *bar)
{
  block_input ();

  /* Destroy the Mac scroll bar control  */
  mac_dispose_scroll_bar (bar);

  /* Dissociate this scroll bar from its window.  */
  if (bar->horizontal)
    wset_horizontal_scroll_bar (XWINDOW (bar->window), Qnil);
  else
    wset_vertical_scroll_bar (XWINDOW (bar->window), Qnil);

  unblock_input ();
}


/* Set the handle of the vertical scroll bar for WINDOW to indicate
   that we are displaying PORTION characters out of a total of WHOLE
   characters, starting at POSITION.  If WINDOW has no scroll bar,
   create one.  */

static void
mac_set_vertical_scroll_bar (struct window *w, int portion, int whole, int position)
{
  struct frame *f = XFRAME (w->frame);
  struct scroll_bar *bar;
  int top, height, left, width;
  int window_y, window_height;

  /* Get window dimensions.  */
  window_box (w, ANY_AREA, 0, &window_y, 0, &window_height);
  top = window_y;
  height = window_height;
  left = WINDOW_SCROLL_BAR_AREA_X (w);
  width = WINDOW_SCROLL_BAR_AREA_WIDTH (w);

  /* Does the scroll bar exist yet?  */
  if (NILP (w->vertical_scroll_bar))
    {
      Lisp_Object barobj;

      block_input ();
      mac_clear_area (f, left, top, width, height);
      unblock_input ();
      bar = x_scroll_bar_create (w, top, left, width, height, false);
      XSETVECTOR (barobj, bar);
      wset_vertical_scroll_bar (w, barobj);
    }
  else
    {
      /* It may just need to be moved and resized.  */
      bar = XSCROLL_BAR (w->vertical_scroll_bar);

      block_input ();

      /* If already correctly positioned, do nothing.  */
      if (bar->left == left && bar->top == top
	  && bar->width == width && bar->height == height)
	{
	  if (bar->redraw_needed_p)
	    mac_redraw_scroll_bar (bar);
	}
      else
	{
	  /* Since toolkit scroll bars are smaller than the space reserved
	     for them on the frame, we have to clear "under" them.  */
	  mac_clear_area (f, left, top, width, height);

          /* Remember new settings.  */
          bar->left = left;
          bar->top = top;
          bar->width = width;
          bar->height = height;

	  mac_update_scroll_bar_bounds (bar);
        }

      unblock_input ();
    }

  bar->redraw_needed_p = 0;

  mac_set_scroll_bar_thumb (bar, portion, position, whole);
}


static void
mac_set_horizontal_scroll_bar (struct window *w, int portion, int whole, int position)
{
  struct frame *f = XFRAME (w->frame);
  struct scroll_bar *bar;
  int top, height, left, width;
  int window_x, window_width;
  int pixel_width = WINDOW_PIXEL_WIDTH (w);

  /* Get window dimensions.  */
  window_box (w, ANY_AREA, &window_x, 0, &window_width, 0);
  left = window_x;
  width = window_width;
  top = WINDOW_SCROLL_BAR_AREA_Y (w);
  height = WINDOW_SCROLL_BAR_AREA_HEIGHT (w);

  /* Does the scroll bar exist yet?  */
  if (NILP (w->horizontal_scroll_bar))
    {
      Lisp_Object barobj;

      block_input ();

      /* Clear also part between window_width and
	 WINDOW_PIXEL_WIDTH.  */
      mac_clear_area (f, WINDOW_LEFT_EDGE_X (w), top,
		      pixel_width - WINDOW_RIGHT_DIVIDER_WIDTH (w), height);
      unblock_input ();
      bar = x_scroll_bar_create (w, top, left, width, height, true);
      XSETVECTOR (barobj, bar);
      wset_horizontal_scroll_bar (w, barobj);
    }
  else
    {
      /* It may just need to be moved and resized.  */
      bar = XSCROLL_BAR (w->horizontal_scroll_bar);

      block_input ();

      /* If already correctly positioned, do nothing.  */
      if (bar->left == left && bar->top == top
	  && bar->width == width && bar->height == height)
	{
	  if (bar->redraw_needed_p)
	    mac_redraw_scroll_bar (bar);
	}
      else
	{
	  /* Since toolkit scroll bars are smaller than the space reserved
	     for them on the frame, we have to clear "under" them.  */
	  mac_clear_area (f, WINDOW_LEFT_EDGE_X (w), top,
			  pixel_width - WINDOW_RIGHT_DIVIDER_WIDTH (w), height);

          /* Remember new settings.  */
          bar->left = left;
          bar->top = top;
          bar->width = width;
          bar->height = height;

	  mac_update_scroll_bar_bounds (bar);
        }

      unblock_input ();
    }

  bar->redraw_needed_p = 0;

  mac_set_scroll_bar_thumb (bar, portion, position, whole);
}


/* The following three hooks are used when we're doing a thorough
   redisplay of the frame.  We don't explicitly know which scroll bars
   are going to be deleted, because keeping track of when windows go
   away is a real pain - "Can you say set-window-configuration, boys
   and girls?"  Instead, we just assert at the beginning of redisplay
   that *all* scroll bars are to be removed, and then save a scroll bar
   from the fiery pit when we actually redisplay its window.  */

/* Arrange for all scroll bars on FRAME to be removed at the next call
   to `*judge_scroll_bars_hook'.  A scroll bar may be spared if
   `*redeem_scroll_bar_hook' is applied to its window before the judgment.  */

static void
mac_condemn_scroll_bars (struct frame *frame)
{
  if (!NILP (FRAME_SCROLL_BARS (frame)))
    {
      if (!NILP (FRAME_CONDEMNED_SCROLL_BARS (frame)))
	{
	  /* Prepend scrollbars to already condemned ones.  */
	  Lisp_Object last = FRAME_SCROLL_BARS (frame);

	  while (!NILP (XSCROLL_BAR (last)->next))
	    last = XSCROLL_BAR (last)->next;

	  XSCROLL_BAR (last)->next = FRAME_CONDEMNED_SCROLL_BARS (frame);
	  XSCROLL_BAR (FRAME_CONDEMNED_SCROLL_BARS (frame))->prev = last;
	}

      fset_condemned_scroll_bars (frame, FRAME_SCROLL_BARS (frame));
      fset_scroll_bars (frame, Qnil);
    }
}


/* Un-mark WINDOW's scroll bar for deletion in this judgment cycle.
   Note that WINDOW isn't necessarily condemned at all.  */

static void
mac_redeem_scroll_bar (struct window *w)
{
  struct scroll_bar *bar;
  Lisp_Object barobj;
  struct frame *f;

  /* We can't redeem this window's scroll bar if it doesn't have one.  */
  if (NILP (w->vertical_scroll_bar) && NILP (w->horizontal_scroll_bar))
    emacs_abort ();

  if (!NILP (w->vertical_scroll_bar) && WINDOW_HAS_VERTICAL_SCROLL_BAR (w))
    {
      bar = XSCROLL_BAR (w->vertical_scroll_bar);
      /* Unlink it from the condemned list.  */
      f = XFRAME (WINDOW_FRAME (w));
      if (NILP (bar->prev))
	{
	  /* If the prev pointer is nil, it must be the first in one of
	     the lists.  */
	  if (EQ (FRAME_SCROLL_BARS (f), w->vertical_scroll_bar))
	    /* It's not condemned.  Everything's fine.  */
	    goto horizontal;
	  else if (EQ (FRAME_CONDEMNED_SCROLL_BARS (f),
		       w->vertical_scroll_bar))
	    fset_condemned_scroll_bars (f, bar->next);
	  else
	    /* If its prev pointer is nil, it must be at the front of
	       one or the other!  */
	    emacs_abort ();
	}
      else
	XSCROLL_BAR (bar->prev)->next = bar->next;

      if (! NILP (bar->next))
	XSCROLL_BAR (bar->next)->prev = bar->prev;

      bar->next = FRAME_SCROLL_BARS (f);
      bar->prev = Qnil;
      XSETVECTOR (barobj, bar);
      fset_scroll_bars (f, barobj);
      if (! NILP (bar->next))
	XSETVECTOR (XSCROLL_BAR (bar->next)->prev, bar);
    }

 horizontal:
  if (!NILP (w->horizontal_scroll_bar) && WINDOW_HAS_HORIZONTAL_SCROLL_BAR (w))
    {
      bar = XSCROLL_BAR (w->horizontal_scroll_bar);
      /* Unlink it from the condemned list.  */
      f = XFRAME (WINDOW_FRAME (w));
      if (NILP (bar->prev))
	{
	  /* If the prev pointer is nil, it must be the first in one of
	     the lists.  */
	  if (EQ (FRAME_SCROLL_BARS (f), w->horizontal_scroll_bar))
	    /* It's not condemned.  Everything's fine.  */
	    return;
	  else if (EQ (FRAME_CONDEMNED_SCROLL_BARS (f),
		       w->horizontal_scroll_bar))
	    fset_condemned_scroll_bars (f, bar->next);
	  else
	    /* If its prev pointer is nil, it must be at the front of
	       one or the other!  */
	    emacs_abort ();
	}
      else
	XSCROLL_BAR (bar->prev)->next = bar->next;

      if (! NILP (bar->next))
	XSCROLL_BAR (bar->next)->prev = bar->prev;

      bar->next = FRAME_SCROLL_BARS (f);
      bar->prev = Qnil;
      XSETVECTOR (barobj, bar);
      fset_scroll_bars (f, barobj);
      if (! NILP (bar->next))
	XSETVECTOR (XSCROLL_BAR (bar->next)->prev, bar);
    }
}

/* Remove all scroll bars on FRAME that haven't been saved since the
   last call to `*condemn_scroll_bars_hook'.  */

static void
mac_judge_scroll_bars (struct frame *f)
{
  Lisp_Object bar, next;

  bar = FRAME_CONDEMNED_SCROLL_BARS (f);

  /* Clear out the condemned list now so we won't try to process any
     more events on the hapless scroll bars.  */
  fset_condemned_scroll_bars (f, Qnil);

  for (; ! NILP (bar); bar = next)
    {
      struct scroll_bar *b = XSCROLL_BAR (bar);

      x_scroll_bar_remove (b);

      next = b->next;
      b->next = b->prev = Qnil;
    }

  /* Now there should be no references to the condemned scroll bars,
     and they should get garbage-collected.  */
}

/* The screen has been cleared so we may have changed foreground or
   background colors, and the scroll bars may need to be redrawn.
   Clear out the scroll bars, and ask for expose events, so we can
   redraw them.  */

void
x_scroll_bar_clear (struct frame *f)
{
  Lisp_Object bar;

  /* We can have scroll bars even if this is 0,
     if we just turned off scroll bar mode.
     But in that case we should not clear them.  */
  if (FRAME_HAS_VERTICAL_SCROLL_BARS (f))
    for (bar = FRAME_SCROLL_BARS (f); !NILP (bar);
	 bar = XSCROLL_BAR (bar)->next)
      XSCROLL_BAR (bar)->redraw_needed_p = 1;
}


/***********************************************************************
			       Tool-bars
 ***********************************************************************/

void
mac_set_frame_window_gravity_reference_bounds (struct frame *f, int win_gravity,
					       NativeRectangle r)
{
  NativeRectangle bounds;

  mac_get_frame_window_structure_bounds (f, &bounds);

  if (r.width <= 0)
    r.width = bounds.width;
  if (r.height <= 0)
    r.height = bounds.height;

  switch (win_gravity)
    {
    case NorthWestGravity:
    case WestGravity:
    case SouthWestGravity:
      break;

    case NorthGravity:
    case CenterGravity:
    case SouthGravity:
      r.x -= r.width / 2;
      break;

    case NorthEastGravity:
    case EastGravity:
    case SouthEastGravity:
      r.x -= r.width;
      break;

    default:
      r.x = bounds.x;
    }

  switch (win_gravity)
    {
    case NorthWestGravity:
    case NorthGravity:
    case NorthEastGravity:
      break;

    case WestGravity:
    case CenterGravity:
    case EastGravity:
      r.y -= r.height / 2;
      break;

    case SouthWestGravity:
    case SouthGravity:
    case SouthEastGravity:
      r.y -= r.height;
      break;

    default:
      r.y = bounds.y;
    }

  if (r.x != bounds.x || r.y != bounds.y
      || r.width != bounds.width || r.height != bounds.height)
    mac_set_frame_window_structure_bounds (f, r);
}

void
mac_get_frame_window_gravity_reference_bounds (struct frame *f, int win_gravity,
					       NativeRectangle *r)
{
  mac_get_frame_window_structure_bounds (f, r);

  switch (win_gravity)
    {
    case NorthWestGravity:
    case WestGravity:
    case SouthWestGravity:
      break;

    case NorthGravity:
    case CenterGravity:
    case SouthGravity:
      r->x += r->width / 2;
      break;

    case NorthEastGravity:
    case EastGravity:
    case SouthEastGravity:
      r->x += r->width;
      break;
    }

  switch (win_gravity)
    {
    case NorthWestGravity:
    case NorthGravity:
    case NorthEastGravity:
      break;

    case WestGravity:
    case CenterGravity:
    case EastGravity:
      r->y += r->height / 2;
      break;

    case SouthWestGravity:
    case SouthGravity:
    case SouthEastGravity:
      r->y += r->height;
      break;
    }
}


/***********************************************************************
			     Text Cursor
 ***********************************************************************/

/* Set clipping for output in glyph row ROW.  W is the window in which
   we operate.  GC is the graphics context to set clipping in.

   ROW may be a text row or, e.g., a mode line.  Text rows must be
   clipped to the interior of the window dedicated to text display,
   mode lines must be clipped to the whole window.  */

static void
x_clip_to_row (struct window *w, struct glyph_row *row,
	       enum glyph_row_area area, GC gc)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  NativeRectangle clip_rect;
  int window_x, window_y, window_width;

  window_box (w, area, &window_x, &window_y, &window_width, 0);

  clip_rect.x = window_x;
  clip_rect.y = WINDOW_TO_FRAME_PIXEL_Y (w, max (0, row->y));
  clip_rect.y = max (clip_rect.y, window_y);
  clip_rect.width = window_width;
  clip_rect.height = row->visible_height;

  mac_set_clip_rectangles (f, gc, &clip_rect, 1);
}


/* Draw a hollow box cursor on window W in glyph row ROW.  */

static void
x_draw_hollow_cursor (struct window *w, struct glyph_row *row)
{
  struct frame *f = XFRAME (WINDOW_FRAME (w));
  struct mac_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  Display *dpy = FRAME_MAC_DISPLAY (f);
  int x, y, wd, h;
  XGCValues xgcv;
  struct glyph *cursor_glyph;
  GC gc;

  /* Get the glyph the cursor is on.  If we can't tell because
     the current matrix is invalid or such, give up.  */
  cursor_glyph = get_phys_cursor_glyph (w);
  if (cursor_glyph == NULL)
    return;

  /* Compute frame-relative coordinates for phys cursor.  */
  get_phys_cursor_geometry (w, row, cursor_glyph, &x, &y, &h);
  wd = w->phys_cursor_width - 1;

  /* The foreground of cursor_gc is typically the same as the normal
     background color, which can cause the cursor box to be invisible.  */
  xgcv.foreground = f->output_data.mac->cursor_pixel;
  if (dpyinfo->scratch_cursor_gc)
    XChangeGC (dpy, dpyinfo->scratch_cursor_gc, GCForeground, &xgcv);
  else
    dpyinfo->scratch_cursor_gc = XCreateGC (dpy, FRAME_MAC_WINDOW (f),
					    GCForeground, &xgcv);
  gc = dpyinfo->scratch_cursor_gc;

  /* When on R2L character, show cursor at the right edge of the
     glyph, unless the cursor box is as wide as the glyph or wider
     (the latter happens when x-stretch-cursor is non-nil).  */
  if ((cursor_glyph->resolved_level & 1) != 0
      && cursor_glyph->pixel_width > wd)
    {
      x += cursor_glyph->pixel_width - wd;
      if (wd > 0)
	wd -= 1;
    }
  /* Set clipping, draw the rectangle, and reset clipping again.  */
  x_clip_to_row (w, row, TEXT_AREA, gc);
  mac_draw_rectangle (f, gc, x, y, wd, h - 1);
  mac_reset_clip_rectangles (f, gc);
}


/* Draw a bar cursor on window W in glyph row ROW.

   Implementation note: One would like to draw a bar cursor with an
   angle equal to the one given by the font property XA_ITALIC_ANGLE.
   Unfortunately, I didn't find a font yet that has this property set.
   --gerd.  */

static void
x_draw_bar_cursor (struct window *w, struct glyph_row *row, int width, enum text_cursor_kinds kind)
{
  struct frame *f = XFRAME (w->frame);
  struct glyph *cursor_glyph;

  /* If cursor is out of bounds, don't draw garbage.  This can happen
     in mini-buffer windows when switching between echo area glyphs
     and mini-buffer.  */
  cursor_glyph = get_phys_cursor_glyph (w);
  if (cursor_glyph == NULL)
    return;

  /* If on an image, draw like a normal cursor.  That's usually better
     visible than drawing a bar, esp. if the image is large so that
     the bar might not be in the window.  */
  if (cursor_glyph->type == IMAGE_GLYPH)
    {
      struct glyph_row *r;
      r = MATRIX_ROW (w->current_matrix, w->phys_cursor.vpos);
      draw_phys_cursor_glyph (w, r, DRAW_CURSOR);
    }
  else
    {
      Display *dpy = FRAME_MAC_DISPLAY (f);
      Window window = FRAME_MAC_WINDOW (f);
      GC gc = FRAME_DISPLAY_INFO (f)->scratch_cursor_gc;
      unsigned long mask = GCForeground | GCBackground;
      struct face *face = FACE_FROM_ID (f, cursor_glyph->face_id);
      XGCValues xgcv;

      /* If the glyph's background equals the color we normally draw
	 the bars cursor in, the bar cursor in its normal color is
	 invisible.  Use the glyph's foreground color instead in this
	 case, on the assumption that the glyph's colors are chosen so
	 that the glyph is legible.  */
      if (face->background == f->output_data.mac->cursor_pixel)
	xgcv.background = xgcv.foreground = face->foreground;
      else
	xgcv.background = xgcv.foreground = f->output_data.mac->cursor_pixel;

      if (gc)
	XChangeGC (dpy, gc, mask, &xgcv);
      else
	{
	  gc = XCreateGC (dpy, window, mask, &xgcv);
	  FRAME_DISPLAY_INFO (f)->scratch_cursor_gc = gc;
	}

      x_clip_to_row (w, row, TEXT_AREA, gc);

      if (kind == BAR_CURSOR)
	{
	  int x = WINDOW_TEXT_TO_FRAME_PIXEL_X (w, w->phys_cursor.x);

	  if (width < 0)
	    width = FRAME_CURSOR_WIDTH (f);
	  width = min (cursor_glyph->pixel_width, width);

	  w->phys_cursor_width = width;

	  /* If the character under cursor is R2L, draw the bar cursor
	     on the right of its glyph, rather than on the left.  */
	  if ((cursor_glyph->resolved_level & 1) != 0)
	    x += cursor_glyph->pixel_width - width;

	  mac_fill_rectangle (f, gc, x,
			      WINDOW_TO_FRAME_PIXEL_Y (w, w->phys_cursor.y),
			      width, row->height);
	}
      else /* HBAR_CURSOR */
	{
	  int dummy_x, dummy_y, dummy_h;
	  int x = WINDOW_TEXT_TO_FRAME_PIXEL_X (w, w->phys_cursor.x);

	  if (width < 0)
	    width = row->height;

	  width = min (row->height, width);

	  get_phys_cursor_geometry (w, row, cursor_glyph, &dummy_x,
				    &dummy_y, &dummy_h);

	  if ((cursor_glyph->resolved_level & 1) != 0
	      && cursor_glyph->pixel_width > w->phys_cursor_width)
	    x += cursor_glyph->pixel_width - w->phys_cursor_width;
	  mac_fill_rectangle (f, gc, x,
			      WINDOW_TO_FRAME_PIXEL_Y (w, w->phys_cursor.y +
						       row->height - width),
			      w->phys_cursor_width, width);
	}

      mac_reset_clip_rectangles (f, gc);
    }
}


/* RIF: Define cursor CURSOR on frame F.  */

static void
mac_define_frame_cursor (struct frame *f, Cursor cursor)
{
  if (f->output_data.mac->current_cursor != cursor)
    {
      f->output_data.mac->current_cursor = cursor;
      mac_invalidate_frame_cursor_rects (f);
    }
}


/* RIF: Clear area on frame F.  */

static void
mac_clear_frame_area (struct frame *f, int x, int y, int width, int height)
{
  mac_clear_area (f, x, y, width, height);
}


/* RIF: Draw cursor on window W.  */

static void
mac_draw_window_cursor (struct window *w, struct glyph_row *glyph_row, int x,
		      int y, enum text_cursor_kinds cursor_type,
		      int cursor_width, bool on_p, bool active_p)
{
  if (on_p)
    {
      w->phys_cursor_type = cursor_type;
      w->phys_cursor_on_p = true;

      if (glyph_row->exact_window_width_line_p
	  && (glyph_row->reversed_p
	      ? (w->phys_cursor.hpos < 0)
	      : (w->phys_cursor.hpos >= glyph_row->used[TEXT_AREA])))
	{
	  glyph_row->cursor_in_fringe_p = true;
	  draw_fringe_bitmap (w, glyph_row, glyph_row->reversed_p);
	}
      else
	{
	  switch (cursor_type)
	    {
	    case HOLLOW_BOX_CURSOR:
	      x_draw_hollow_cursor (w, glyph_row);
	      break;

	    case FILLED_BOX_CURSOR:
	      draw_phys_cursor_glyph (w, glyph_row, DRAW_CURSOR);
	      break;

	    case BAR_CURSOR:
	      x_draw_bar_cursor (w, glyph_row, cursor_width, BAR_CURSOR);
	      break;

	    case HBAR_CURSOR:
	      x_draw_bar_cursor (w, glyph_row, cursor_width, HBAR_CURSOR);
	      break;

	    case NO_CURSOR:
	      w->phys_cursor_width = 0;
	      break;

	    default:
	      emacs_abort ();
	    }
	}

      if (w == XWINDOW (selected_window))
	mac_update_accessibility_status (XFRAME (w->frame));
    }
}


/* Changing the font of the frame.  */

/* Give frame F the font FONT-OBJECT as its default font.  The return
   value is FONT-OBJECT.  FONTSET is an ID of the fontset for the
   frame.  If it is negative, generate a new fontset from
   FONT-OBJECT.  */

Lisp_Object
x_new_font (struct frame *f, Lisp_Object font_object, int fontset)
{
  struct font *font = XFONT_OBJECT (font_object);
  int unit, font_ascent, font_descent;

  if (fontset < 0)
    fontset = fontset_from_font (font_object);
  FRAME_FONTSET (f) = fontset;
  if (FRAME_FONT (f) == font)
    /* This font is already set in frame F.  There's nothing more to
       do.  */
    return font_object;

  FRAME_FONT (f) = font;
  FRAME_BASELINE_OFFSET (f) = font->baseline_offset;
  FRAME_COLUMN_WIDTH (f) = font->average_width;
  get_font_ascent_descent (font, &font_ascent, &font_descent);
  FRAME_LINE_HEIGHT (f) = font_ascent + font_descent;

  FRAME_TOOL_BAR_HEIGHT (f) = FRAME_TOOL_BAR_LINES (f) * FRAME_LINE_HEIGHT (f);
  FRAME_MENU_BAR_HEIGHT (f) = FRAME_MENU_BAR_LINES (f) * FRAME_LINE_HEIGHT (f);

  unit = FRAME_COLUMN_WIDTH (f);
  /* The width of a toolkit scrollbar does not change with the new
     font but we have to calculate the number of columns it occupies
     anew.  */
  FRAME_CONFIG_SCROLL_BAR_COLS (f)
    = (FRAME_CONFIG_SCROLL_BAR_WIDTH (f) + unit - 1) / unit;

  if (FRAME_MAC_WINDOW (f) != 0)
    {
      /* Don't change the size of a tip frame; there's no point in
	 doing it because it's done in Fx_show_tip, and it leads to
	 problems because the tip frame has no widget.  */
      if (NILP (tip_frame) || XFRAME (tip_frame) != f)
	adjust_frame_size (f, FRAME_COLS (f) * FRAME_COLUMN_WIDTH (f),
			   FRAME_LINES (f) * FRAME_LINE_HEIGHT (f), 3,
			   false, Qfont);
    }

  return font_object;
}


void
mac_handle_origin_change (struct frame *f)
{
  x_real_positions (f, &f->left_pos, &f->top_pos);
}

void
mac_handle_size_change (struct frame *f, int pixelwidth, int pixelheight)
{
  int width, height;

  /* This might be called when a full screen window is closed on OS X
     10.10.  */
  if (!WINDOWP (FRAME_ROOT_WINDOW (f)))
    return;

  width = FRAME_PIXEL_TO_TEXT_WIDTH (f, pixelwidth);
  height = FRAME_PIXEL_TO_TEXT_HEIGHT (f, pixelheight);

  /* Pass true for DELAY since we can't run Lisp code inside of a
     block_input.  */
  change_frame_size (f, width, height, false, true, false, true);

  /* If cursor was outside the new size, mark it as off.  */
  mark_window_cursors_off (XWINDOW (f->root_window));

  /* Clear out any recollection of where the mouse highlighting was,
     since it might be in a place that's outside the new frame size.
     Actually checking whether it is outside is a pain in the neck, so
     don't try--just let the highlighting be done afresh with new
     size.  */
  cancel_mouse_face (f);
}


/* Calculate the absolute position in frame F
   from its current recorded position values and gravity.  */

static void
x_calc_absolute_position (struct frame *f)
{
  int flags = f->size_hint_flags;
  NativeRectangle bounds;

  /* We have nothing to do if the current position
     is already for the top-left corner.  */
  if (! ((flags & XNegative) || (flags & YNegative)))
    return;

  /* Find the offsets of the outside upper-left corner of
     the inner window, with respect to the outer window.  */
  block_input ();
  mac_get_frame_window_structure_bounds (f, &bounds);
  unblock_input ();

  /* Treat negative positions as relative to the leftmost bottommost
     position that fits on the screen.  */
  if (flags & XNegative)
    f->left_pos += (x_display_pixel_width (FRAME_DISPLAY_INFO (f))
		    - bounds.width);

  if (flags & YNegative)
    f->top_pos += (x_display_pixel_height (FRAME_DISPLAY_INFO (f))
		   - bounds.height);

  /* The left_pos and top_pos
     are now relative to the top and left screen edges,
     so the flags should correspond.  */
  f->size_hint_flags &= ~ (XNegative | YNegative);
}

/* CHANGE_GRAVITY is 1 when calling from Fset_frame_position,
   to really change the position, and 0 when calling from
   x_make_frame_visible (in that case, XOFF and YOFF are the current
   position values).  It is -1 when calling from x_set_frame_parameters,
   which means, do adjust for borders but don't change the gravity.  */

void
x_set_offset (struct frame *f, register int xoff, register int yoff, int change_gravity)
{
  if (change_gravity > 0)
    {
      f->top_pos = yoff;
      f->left_pos = xoff;
      f->size_hint_flags &= ~ (XNegative | YNegative);
      if (xoff < 0)
	f->size_hint_flags |= XNegative;
      if (yoff < 0)
	f->size_hint_flags |= YNegative;
      f->win_gravity = NorthWestGravity;
    }
  x_calc_absolute_position (f);

  block_input ();
  x_wm_set_size_hint (f, 0, false);

  mac_move_frame_window_structure (f, f->left_pos, f->top_pos);
  /* When the frame is maximized/fullscreen, the actual window will
     not be moved and mac_handle_origin_change will not be called via
     window system events.  */
  mac_handle_origin_change (f);
  unblock_input ();
}

void
x_set_sticky (struct frame *f, Lisp_Object new_value, Lisp_Object old_value)
{
  block_input ();
  mac_change_frame_window_wm_state (f, !NILP (new_value) ? WM_STATE_STICKY : 0,
				    NILP (new_value) ? WM_STATE_STICKY : 0);
  unblock_input ();
}

static void
mac_fullscreen_hook (struct frame *f)
{
  FRAME_CHECK_FULLSCREEN_NEEDED_P (f) = 1;
  if (FRAME_VISIBLE_P (f))
    {
      block_input ();
      x_check_fullscreen (f);
      unblock_input ();
    }
}

/* Check if we need to resize the frame due to a fullscreen request.
   If so needed, resize the frame. */
static void
x_check_fullscreen (struct frame *f)
{
  WMState flags_to_set, flags_to_clear;

  switch (f->want_fullscreen)
    {
    case FULLSCREEN_NONE:
      flags_to_set = 0;
      break;
    case FULLSCREEN_BOTH:
      flags_to_set = WM_STATE_FULLSCREEN;
      break;
    case FULLSCREEN_DEDICATED_DESKTOP:
      flags_to_set = (WM_STATE_FULLSCREEN | WM_STATE_DEDICATED_DESKTOP);
      break;
    case FULLSCREEN_WIDTH:
      flags_to_set = WM_STATE_MAXIMIZED_HORZ;
      break;
    case FULLSCREEN_HEIGHT:
      flags_to_set = WM_STATE_MAXIMIZED_VERT;
      break;
    case FULLSCREEN_MAXIMIZED:
      flags_to_set = (WM_STATE_MAXIMIZED_HORZ | WM_STATE_MAXIMIZED_VERT);
      break;
    }

  flags_to_clear = (flags_to_set ^ (WM_STATE_MAXIMIZED_HORZ
				    | WM_STATE_MAXIMIZED_VERT
				    | WM_STATE_FULLSCREEN
				    | WM_STATE_DEDICATED_DESKTOP));

  f->want_fullscreen = FULLSCREEN_NONE;
  FRAME_CHECK_FULLSCREEN_NEEDED_P (f) = 0;

  mac_change_frame_window_wm_state (f, flags_to_set, flags_to_clear);
}

/* Call this to change the size of frame F's x-window.
   If CHANGE_GRAVITY, change to top-left-corner window gravity
   for this size change and subsequent size changes.
   Otherwise we leave the window gravity unchanged.  */

void
x_set_window_size (struct frame *f, bool change_gravity,
		   int width, int height, bool pixelwise)
{
  int pixelwidth, pixelheight;

  block_input ();

  if ((NILP (tip_frame) || XFRAME (tip_frame) != f)
      /* Don't override pending size change.  */
      && f->new_height == 0 && f->new_width == 0)
    {
      int text_width, text_height;

      /* When the frame is maximized/fullscreen or running under for
         example Xmonad, x_set_window_size_1 will be a no-op.
         In that case, the right thing to do is extend rows/width to
         the current frame size.  We do that first if x_set_window_size_1
         turns out to not be a no-op (there is no way to know).
         The size will be adjusted again if the frame gets a
         ConfigureNotify event as a result of x_set_window_size.  */
      text_width = FRAME_PIXEL_TO_TEXT_WIDTH (f, FRAME_PIXEL_WIDTH (f));
      text_height = FRAME_PIXEL_TO_TEXT_HEIGHT (f, FRAME_PIXEL_HEIGHT (f));

      change_frame_size (f, text_width, text_height, false, true, false, true);
    }

  pixelwidth = (pixelwise
		? FRAME_TEXT_TO_PIXEL_WIDTH (f, width)
		: FRAME_TEXT_COLS_TO_PIXEL_WIDTH (f, width));
  pixelheight = (pixelwise
		 ? FRAME_TEXT_TO_PIXEL_HEIGHT (f, height)
		 : FRAME_TEXT_LINES_TO_PIXEL_HEIGHT (f, height));
  f->win_gravity = NorthWestGravity;
  x_wm_set_size_hint (f, 0, false);

  mac_size_frame_window (f, pixelwidth, pixelheight, false);

  SET_FRAME_GARBAGED (f);

  unblock_input ();

  do_pending_window_change (false);
}

/* Move the mouse to position pixel PIX_X, PIX_Y relative to frame F.  */

void
frame_set_mouse_pixel_position (struct frame *f, int pix_x, int pix_y)
{
  block_input ();
  mac_convert_frame_point_to_global (f, &pix_x, &pix_y);
  CGWarpMouseCursorPosition (CGPointMake (pix_x, pix_y));
  unblock_input ();
}

/* Raise frame F.  */

void
x_raise_frame (struct frame *f)
{
  if (FRAME_VISIBLE_P (f))
    {
      block_input ();
      mac_bring_frame_window_to_front (f);
      unblock_input ();
    }
}

/* Lower frame F.  */

void
x_lower_frame (struct frame *f)
{
  if (FRAME_VISIBLE_P (f))
    {
      block_input ();
      mac_send_frame_window_behind (f);
      unblock_input ();
    }
}

static void
mac_frame_raise_lower (struct frame *f, bool raise_flag)
{
  if (raise_flag)
    x_raise_frame (f);
  else
    x_lower_frame (f);
}

/* Change of visibility.  */

void
mac_handle_visibility_change (struct frame *f)
{
  bool visible = false, iconified = false;
  struct input_event buf;

  if (mac_is_frame_window_visible (f))
    {
      if (mac_is_frame_window_collapsed (f))
	iconified = true;
      else
	visible = true;
    }

  if (!FRAME_VISIBLE_P (f) && visible)
    {
      if (FRAME_CHECK_FULLSCREEN_NEEDED_P (f))
	x_check_fullscreen (f);

      if (FRAME_ICONIFIED_P (f))
	{
	  EVENT_INIT (buf);
	  buf.kind = DEICONIFY_EVENT;
	  XSETFRAME (buf.frame_or_window, f);
	  buf.arg = Qnil;
	  kbd_buffer_store_event (&buf);
	}
      else if (! NILP (Vframe_list) && ! NILP (XCDR (Vframe_list)))
	/* Force a redisplay sooner or later to update the
	   frame titles in case this is the second frame.  */
	record_asynch_buffer_change ();
    }
  else if (FRAME_VISIBLE_P (f) && !visible)
    if (iconified)
      {
	EVENT_INIT (buf);
	buf.kind = ICONIFY_EVENT;
	XSETFRAME (buf.frame_or_window, f);
	buf.arg = Qnil;
	kbd_buffer_store_event (&buf);
      }

  SET_FRAME_VISIBLE (f, visible);
  SET_FRAME_ICONIFIED (f, iconified);
}

/* This tries to wait until the frame is really visible.
   However, if the window manager asks the user where to position
   the frame, this will return before the user finishes doing that.
   The frame will not actually be visible at that time,
   but it will become visible later when the window manager
   finishes with it.  */

void
x_make_frame_visible (struct frame *f)
{
  block_input ();

  if (! FRAME_VISIBLE_P (f))
    {
      /* We test FRAME_GARBAGED_P here to make sure we don't
	 call x_set_offset a second time
	 if we get to x_make_frame_visible a second time
	 before the window gets really visible.  */
      if (! FRAME_ICONIFIED_P (f)
	  && ! f->output_data.mac->asked_for_visible)
	x_set_offset (f, f->left_pos, f->top_pos, 0);

      f->output_data.mac->asked_for_visible = true;

      mac_collapse_frame_window (f, false);
      mac_show_frame_window (f);
    }

  XFlush (FRAME_MAC_DISPLAY (f));

  /* Synchronize to ensure Emacs knows the frame is visible
     before we do anything else.  We do this loop with input not blocked
     so that incoming events are handled.  */
  {
    Lisp_Object frame;

    unblock_input ();

    XSETFRAME (frame, f);

    /* Process X events until a MapNotify event has been seen.  */
    while (!FRAME_VISIBLE_P (f))
      {
	/* This hack is still in use at least for Cygwin.  See
	   http://lists.gnu.org/archive/html/emacs-devel/2013-12/msg00351.html.

	   Machines that do polling rather than SIGIO have been
	   observed to go into a busy-wait here.  So we'll fake an
	   alarm signal to let the handler know that there's something
	   to be read.  We used to raise a real alarm, but it seems
	   that the handler isn't always enabled here.  This is
	   probably a bug.  */
	if (input_polling_used ())
	  {
	    /* It could be confusing if a real alarm arrives while
	       processing the fake one.  Turn it off and let the
	       handler reset it.  */
	    int old_poll_suppress_count = poll_suppress_count;
	    poll_suppress_count = 1;
	    poll_for_input_1 ();
	    poll_suppress_count = old_poll_suppress_count;
	  }
      }
  }
}

/* Change from mapped state to withdrawn state.  */

/* Make the frame visible (mapped and not iconified).  */

void
x_make_frame_invisible (struct frame *f)
{
  /* A deactivate event does not occur when the last visible frame is
     made invisible.  So if we clear the highlight here, it will not
     be rehighlighted when it is made visible.  */
#if 0
  /* Don't keep the highlight on an invisible frame.  */
  if (FRAME_DISPLAY_INFO (f)->x_highlight_frame == f)
    FRAME_DISPLAY_INFO (f)->x_highlight_frame = 0;
#endif

  block_input ();

  /* Before unmapping the window, update the WM_SIZE_HINTS property to claim
     that the current position of the window is user-specified, rather than
     program-specified, so that when the window is mapped again, it will be
     placed at the same location, without forcing the user to position it
     by hand again (they have already done that once for this window.)  */
  x_wm_set_size_hint (f, 0, true);

  mac_hide_frame_window (f);

  unblock_input ();

  mac_handle_visibility_change (f);
}

/* Change window state from mapped to iconified.  */

void
x_iconify_frame (struct frame *f)
{
  OSStatus err;

  /* A deactivate event does not occur when the last visible frame is
     iconified.  So if we clear the highlight here, it will not be
     rehighlighted when it is deiconified.  */
#if 0
  /* Don't keep the highlight on an invisible frame.  */
  if (FRAME_DISPLAY_INFO (f)->x_highlight_frame == f)
    FRAME_DISPLAY_INFO (f)->x_highlight_frame = 0;
#endif

  if (FRAME_ICONIFIED_P (f))
    return;

  block_input ();

  if (! FRAME_VISIBLE_P (f))
    mac_show_frame_window (f);

  err = mac_collapse_frame_window (f, true);

  unblock_input ();

  if (err != noErr)
    error ("Can't notify window manager of iconification");

  mac_handle_visibility_change (f);
}


/* Free X resources of frame F.  */

void
x_free_frame_resources (struct frame *f)
{
  struct mac_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);
  Mouse_HLInfo *hlinfo = &dpyinfo->mouse_highlight;

  block_input ();

  /* AppKit version of mac_dispose_frame_window, which is implemented
     as -[NSWindow close], will change the focus to the next window
     during its call.  So, unlike other platforms, we clean up the
     focus-related variables before calling mac_dispose_frame_window.  */
  if (f == dpyinfo->x_focus_frame)
    {
      dpyinfo->x_focus_frame = 0;
      mac_set_font_info_for_selection (NULL, DEFAULT_FACE_ID, 0, -1, Qnil);
    }
  if (f == dpyinfo->x_focus_event_frame)
    dpyinfo->x_focus_event_frame = 0;
  if (f == dpyinfo->x_highlight_frame)
    dpyinfo->x_highlight_frame = 0;
  if (f == hlinfo->mouse_face_mouse_frame)
    reset_mouse_highlight (hlinfo);

  if (FRAME_MAC_WINDOW (f))
    mac_dispose_frame_window (f);

  free_frame_menubar (f);

  free_frame_faces (f);

  x_free_gcs (f);

  xfree (FRAME_SIZE_HINTS (f));

  /* Free cursors.  */
  mac_cursor_release (f->output_data.mac->text_cursor);
  mac_cursor_release (f->output_data.mac->nontext_cursor);
  mac_cursor_release (f->output_data.mac->modeline_cursor);
  mac_cursor_release (f->output_data.mac->hand_cursor);
  mac_cursor_release (f->output_data.mac->hourglass_cursor);
  mac_cursor_release (f->output_data.mac->horizontal_drag_cursor);
  mac_cursor_release (f->output_data.mac->vertical_drag_cursor);

  xfree (f->output_data.mac);
  f->output_data.mac = NULL;

  unblock_input ();
}


/* Destroy the X window of frame F.  */

static void
x_destroy_window (struct frame *f)
{
  struct mac_display_info *dpyinfo = FRAME_DISPLAY_INFO (f);

  x_free_frame_resources (f);

  dpyinfo->reference_count--;
}


/* Setting window manager hints.  */

/* Set the normal size hints for the window manager, for frame F.
   FLAGS is the flags word to use--or 0 meaning preserve the flags
   that the window now has.
   If USER_POSITION, set the USPosition
   flag (this is useful when FLAGS is 0).  */
void
x_wm_set_size_hint (struct frame *f, long flags, bool user_position)
{
  int base_width, base_height;
  XSizeHints *size_hints;

  base_width = FRAME_TEXT_COLS_TO_PIXEL_WIDTH (f, 0);
  base_height = FRAME_TEXT_LINES_TO_PIXEL_HEIGHT (f, 0);

  size_hints = FRAME_SIZE_HINTS (f);
  if (size_hints == NULL)
    size_hints = FRAME_SIZE_HINTS (f) = xzalloc (sizeof (XSizeHints));

  size_hints->flags |= PResizeInc | PMinSize | PBaseSize ;
  size_hints->width_inc = frame_resize_pixelwise ? 1 : FRAME_COLUMN_WIDTH (f);
  size_hints->height_inc = frame_resize_pixelwise ? 1 : FRAME_LINE_HEIGHT (f);
  size_hints->min_width  = base_width;
  size_hints->min_height = base_height;
  size_hints->base_width  = base_width;
  size_hints->base_height = base_height;

  if (flags)
    size_hints->flags = flags;
  else if (user_position)
    {
      size_hints->flags &= ~ PPosition;
      size_hints->flags |= USPosition;
    }
}

void
x_wm_set_icon_position (struct frame *f, int icon_x, int icon_y)
{
#if 0 /* MAC_TODO: no icons on Mac */
#ifdef USE_X_TOOLKIT
  Window window = XtWindow (f->output_data.x->widget);
#else
  Window window = FRAME_X_WINDOW (f);
#endif

  f->output_data.x->wm_hints.flags |= IconPositionHint;
  f->output_data.x->wm_hints.icon_x = icon_x;
  f->output_data.x->wm_hints.icon_y = icon_y;

  XSetWMHints (FRAME_X_DISPLAY (f), window, &f->output_data.x->wm_hints);
#endif /* MAC_TODO */
}


/***********************************************************************
				Fonts
 ***********************************************************************/

#ifdef GLYPH_DEBUG

/* Check that FONT is valid on frame F.  It is if it can be found in F's
   font table.  */

static void
x_check_font (struct frame *f, struct font *font)
{
  eassert (font != NULL && ! NILP (font->props[FONT_TYPE_INDEX]));
  if (font->driver->check)
    eassert (font->driver->check (f, font) == 0);
}

#endif /* GLYPH_DEBUG */


/* The Mac Event loop code */

/* Whether or not the screen configuration has changed.  */
bool mac_screen_config_changed = 0;

/* Table for translating Mac keycode to X keysym values.  Contributed
   by Sudhir Shenoy.
   Mapping for special keys is now identical to that in Apple X11
   except `clear' (-> <clear>) on the KeyPad, `enter' (-> <kp-enter>)
   on the right of the Cmd key on laptops, and fn + `enter' (->
   <linefeed>). */
const unsigned char keycode_to_xkeysym_table[] = {
  /*0x00*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  /*0x10*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  /*0x20*/ 0, 0, 0, 0, 0x0d /*return*/, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,

  /*0x30*/ 0x09 /*tab*/, 0 /*0x0020 space*/, 0, 0x08 /*backspace*/,
  /*0x34*/ 0x8d /*enter on laptops*/, 0x1b /*escape*/, 0, 0,
  /*0x38*/ 0, 0, 0, 0,
  /*0x3C*/ 0, 0, 0, 0,

  /*0x40*/ 0xce /*f17*/, 0xae /*kp-decimal*/, 0, 0xaa /*kp-multiply*/,
  /*0x44*/ 0, 0xab /*kp-add*/, 0, 0x0b /*clear*/,
  /*0x48*/ 0, 0, 0, 0xaf /*kp-divide*/,
  /*0x4C*/ 0x8d /*kp-enter*/, 0, 0xad /*kp-subtract*/, 0xcf /*f18*/,

  /*0x50*/ 0xd0 /*f19*/, 0xbd /*kp-equal*/, 0xb0 /*kp-0*/, 0xb1 /*kp-1*/,
  /*0x54*/ 0xb2 /*kp-2*/, 0xb3 /*kp-3*/, 0xb4 /*kp-4*/, 0xb5 /*kp-5*/,
  /*0x58*/ 0xb6 /*kp-6*/, 0xb7 /*kp-7*/, 0xd1 /*f20*/, 0xb8 /*kp-8*/,
  /*0x5C*/ 0xb9 /*kp-9*/, 0, 0, 0xac /*kp-separator*/,

  /*0x60*/ 0xc2 /*f5*/, 0xc3 /*f6*/, 0xc4 /*f7*/, 0xc0 /*f3*/,
  /*0x64*/ 0xc5 /*f8*/, 0xc6 /*f9*/, 0, 0xc8 /*f11*/,
  /*0x68*/ 0, 0xca /*f13*/, 0xcd /*f16*/, 0xcb /*f14*/,
  /*0x6C*/ 0, 0xc7 /*f10*/, 0x0a /*fn+enter on laptops*/, 0xc9 /*f12*/,

  /*0x70*/ 0, 0xcc /*f15*/, 0x6a /*help*/, 0x50 /*home*/,
  /*0x74*/ 0x55 /*pgup*/, 0xff /*delete*/, 0xc1 /*f4*/, 0x57 /*end*/,
  /*0x78*/ 0xbf /*f2*/, 0x56 /*pgdown*/, 0xbe /*f1*/, 0x51 /*left*/,
  /*0x7C*/ 0x53 /*right*/, 0x54 /*down*/, 0x52 /*up*/, 0
};

/* Table for translating Mac keycode with the `fn' key to that without
   it.  Destination symbols in comments are keys on US keyboard, and
   they may not be the same on other types of keyboards.  If the
   destination is identical to the source, it doesn't map `fn' key to
   a modifier.  */
static const unsigned char fn_keycode_to_keycode_table[] = {
  /*0x00*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  /*0x10*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  /*0x20*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,

  /*0x30*/ 0, 0, 0, 0,
  /*0x34*/ 0, 0, 0, 0,
  /*0x38*/ 0, 0, 0, 0,
  /*0x3C*/ 0, 0, 0, 0,

  /*0x40*/ 0x40 /*f17 = f17*/, 0x2f /*kp-decimal -> '.'*/, 0, 0x23 /*kp-multiply -> 'p'*/,
  /*0x44*/ 0, 0x2c /*kp-add -> '/'*/, 0, 0x16 /*clear -> '6'*/,
  /*0x48*/ 0, 0, 0, 0x1d /*kp-/ -> '0'*/,
  /*0x4C*/ 0x24 /*kp-enter -> return*/, 0, 0x29 /*kp-subtract -> ';'*/, 0x4f /*f18 = f18*/,

  /*0x50*/ 0x50 /*f19 = f19*/, 0x1b /*kp-equal -> '-'*/, 0x2e /*kp-0 -> 'm'*/, 0x26 /*kp-1 -> 'j'*/,
  /*0x54*/ 0x28 /*kp-2 -> 'k'*/, 0x25 /*kp-3 -> 'l'*/, 0x20 /*kp-4 -> 'u'*/, 0x22 /*kp-5 ->'i'*/,
  /*0x58*/ 0x1f /*kp-6 -> 'o'*/, 0x1a /*kp-7 -> '7'*/, 0x5a /*f20 = f20*/, 0x1c /*kp-8 -> '8'*/,
  /*0x5C*/ 0x19 /*kp-9 -> '9'*/, 0, 0, 0,

  /*0x60*/ 0x60 /*f5 = f5*/, 0x61 /*f6 = f6*/, 0x62 /*f7 = f7*/, 0x63 /*f3 = f3*/,
  /*0x64*/ 0x64 /*f8 = f8*/, 0x65 /*f9 = f9*/, 0, 0x67 /*f11 = f11*/,
  /*0x68*/ 0, 0x69 /*f13 = f13*/, 0x6a /*f16 = f16*/, 0x6b /*f14 = f14*/,
  /*0x6C*/ 0, 0x6d /*f10 = f10*/, 0, 0x6f /*f12 = f12*/,

  /*0x70*/ 0, 0x71 /*f15 = f15*/, 0x72 /*help = help*/, 0x7b /*home -> left*/,
  /*0x74*/ 0x7e /*pgup -> up*/, 0x33 /*delete -> backspace*/, 0x76 /*f4 = f4*/, 0x7c /*end -> right*/,
  /*0x78*/ 0x78 /*f2 = f2*/, 0x7d /*pgdown -> down*/, 0x7a /*f1 = f1*/, 0x7b /*left = left*/,
  /*0x7C*/ 0x7c /*right = right*/, 0x7d /*down = down*/, 0x7e /*up = up*/, 0
};

/* Convert CGEvent flags to Carbon key modifiers.  */

UInt32
mac_cgevent_flags_to_modifiers (CGEventFlags flags)
{
  UInt32 modifiers = 0;

  if (flags & kCGEventFlagMaskAlphaShift)
    modifiers |= alphaLock;
  if (flags & kCGEventFlagMaskShift)
    modifiers |= shiftKey;
  if (flags & kCGEventFlagMaskControl)
    modifiers |= controlKey;
  if (flags & kCGEventFlagMaskAlternate)
    modifiers |= optionKey;
  if (flags & kCGEventFlagMaskCommand)
    modifiers |= cmdKey;
  /* if (flags & kCGEventFlagMaskHelp); */
  if (flags & kCGEventFlagMaskSecondaryFn)
    modifiers |= kEventKeyModifierFnMask;
  if (flags & kCGEventFlagMaskNumericPad)
    modifiers |= kEventKeyModifierNumLockMask;

  return modifiers;
}

/* Update the Unicode string in a Quartz event CGEVENT according to
   its keycode, flags, and keyboard type values.  Return true if and
   only if the Unicode string is successfully updated.  */

static bool
mac_cgevent_update_unicode_string (CGEventRef cgevent)
{
  bool result = false;
  UInt32 modifiers = mac_cgevent_flags_to_modifiers (CGEventGetFlags (cgevent));
  UInt32 keycode = CGEventGetIntegerValueField (cgevent,
						kCGKeyboardEventKeycode);
  UCKeyboardLayout *uchr_ptr = NULL;
  TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource ();
  CFDataRef uchr_data = NULL;

  if (source)
    uchr_data =
      TISGetInputSourceProperty (source, kTISPropertyUnicodeKeyLayoutData);
  if (uchr_data)
    uchr_ptr = (UCKeyboardLayout *) CFDataGetBytePtr (uchr_data);

  if (uchr_ptr)
    {
      OSStatus status;
      UInt16 key_action =
	(CGEventGetIntegerValueField (cgevent, kCGKeyboardEventAutorepeat)
	 ? kUCKeyActionAutoKey : kUCKeyActionDown);
      UInt32 modifier_key_state = modifiers >> 8;
      UInt32 keyboard_type =
	CGEventGetIntegerValueField (cgevent, kCGKeyboardEventKeyboardType);
      UInt32 dead_key_state = 0;
      UniCharCount actual_length;
      UniChar string[255];

      status = UCKeyTranslate (uchr_ptr, keycode, key_action,
			       modifier_key_state, keyboard_type,
			       kUCKeyTranslateNoDeadKeysMask,
			       &dead_key_state, 255, &actual_length, string);
      if (status == noErr)
	{
	  CGEventKeyboardSetUnicodeString (cgevent, actual_length, string);
	  result = true;
	}
    }
  if (source)
    CFRelease (source);

  return result;
}

static Lisp_Object
mac_modifier_map_lookup (Lisp_Object modifier_map, Lisp_Object kind)
{
  if (SYMBOLP (modifier_map))
    return modifier_map;
  else
    {
      Lisp_Object value = Fplist_get (modifier_map, kind);

      return SYMBOLP (value) ? value : Qnil;
    }
}

static bool
mac_modifier_map_button (Lisp_Object modifier_map, ptrdiff_t *code)
{
  Lisp_Object value = Fplist_get (modifier_map, QCbutton);
  ptrdiff_t c = *code;

  while (CONSP (value) && c > 0)
    value = XCDR (value), c--;
  if (c == 0)
    {
      if (CONSP (value))
	value = XCAR (value);
      if (INTEGERP (value) && XINT (value) > 0 && *code != XINT (value) - 1)
	{
	  *code = XINT (value) - 1;

	  return true;
	}
    }

  return false;
}

/* Convert a Quartz event CGEVENT to an input event, and update
   `modifier', `code', `timestamp' (and also `kind' for key-down
   events) members of *BUF if BUF is not NULL.  Return bitwise-or of
   CGEvent flag masks that have been mapped to emacs modifiers.  */

CGEventFlags
mac_cgevent_to_input_event (CGEventRef cgevent, struct input_event *buf)
{
  CGEventMask key_event_mask =
    (CGEventMaskBit (kCGEventKeyDown) | CGEventMaskBit (kCGEventKeyUp));
  CGEventMask mouse_button_event_mask =
    (CGEventMaskBit (kCGEventLeftMouseDown)
     | CGEventMaskBit (kCGEventLeftMouseUp)
     | CGEventMaskBit (kCGEventRightMouseDown)
     | CGEventMaskBit (kCGEventRightMouseUp)
     | CGEventMaskBit (kCGEventLeftMouseDragged)
     | CGEventMaskBit (kCGEventRightMouseDragged)
     | CGEventMaskBit (kCGEventOtherMouseDown)
     | CGEventMaskBit (kCGEventOtherMouseUp)
     | CGEventMaskBit (kCGEventOtherMouseDragged));
  CGEventMask other_mouse_event_mask =
    (CGEventMaskBit (kCGEventMouseMoved)
     | CGEventMaskBit (kCGEventScrollWheel)
     | CGEventMaskBit (29)	/* NSEventTypeGesture */
     /* Probably NSEventTypeGesture above covers all the cases below.
	But just in case...  */
     | CGEventMaskBit (30)	/* NSEventTypeMagnify */
     | CGEventMaskBit (31)	/* NSEventTypeSwipe */
     | CGEventMaskBit (18)	/* NSEventTypeRotate */
     | CGEventMaskBit (19)	/* NSEventTypeBeginGesture */
     | CGEventMaskBit (20)	/* NSEventTypeEndGesture */
     | CGEventMaskBit (32)	/* NSEventTypeSmartMagnify */
     | CGEventMaskBit (33));	/* NSEventTypeQuickLook */
  CGEventFlags possibly_mapped_flags =
    (kCGEventFlagMaskControl | kCGEventFlagMaskAlternate
     | kCGEventFlagMaskCommand | kCGEventFlagMaskSecondaryFn);
  enum side {LEFT, RIGHT, NSIDES};
  static const struct {
    CGEventFlags device_indep;
    CGEventFlags device_deps[NSIDES];
  } mask_table[] = {{kCGEventFlagMaskControl,
		     {NX_DEVICELCTLKEYMASK, NX_DEVICERCTLKEYMASK}},
		    {kCGEventFlagMaskAlternate,
		     {NX_DEVICELALTKEYMASK, NX_DEVICERALTKEYMASK}},
		    {kCGEventFlagMaskCommand,
		     {NX_DEVICELCMDKEYMASK, NX_DEVICERCMDKEYMASK}},
		    {kCGEventFlagMaskSecondaryFn,
		     {0, 0}}};
  static Lisp_Object *const modifier_maps[][NSIDES] =
    {{&Vmac_control_modifier, &Vmac_right_control_modifier},
     {&Vmac_option_modifier, &Vmac_right_option_modifier},
     {&Vmac_command_modifier, &Vmac_right_command_modifier},
     {&Vmac_function_modifier, NULL}};
  Lisp_Object kind;
  int i, keycode, emacs_modifiers = 0;
  ptrdiff_t code = -1;
  bool map_button_p = false;
  CGEventFlags mapped_flags = 0;
  CGEventMask type_mask;
  CGEventFlags flags = CGEventGetFlags (cgevent);

  if (buf == NULL && (flags & possibly_mapped_flags) == 0)
    return 0;

  type_mask = CGEventMaskBit (CGEventGetType (cgevent));
  if (type_mask & key_event_mask)
    {
      keycode = CGEventGetIntegerValueField (cgevent, kCGKeyboardEventKeycode);

      if ((flags & kCGEventFlagMaskSecondaryFn)
	  /* We exclude the case `keycode == 0' (kVK_ANSI_A) because
	     the condition `fn_keycode_to_keycode_table[keycode] ==
	     keycode' holds bogusly.  */
	  && keycode > 0 && keycode <= 0x7f)
	{
	  ptrdiff_t stripped = fn_keycode_to_keycode_table[keycode];

	  /* The meaning of kCGEventFlagMaskSecondaryFn has changed in
	     Mac OS X 10.5, and it now behaves much like Cocoa's
	     NSFunctionKeyMask.  It no longer means `fn' key is down
	     for the following keys: F1, F2, and so on, Help, Forward
	     Delete, Home, End, Page Up, Page Down, the arrow keys,
	     and Clear.  We ignore the corresponding bit if that key
	     can be entered without the `fn' key.  */
	  if (stripped == keycode)
	    flags &= ~kCGEventFlagMaskSecondaryFn;
	  else if (stripped)
	    {
	      Lisp_Object k = (keycode_to_xkeysym_table[stripped]
			       ? QCfunction : QCordinary);

	      if (!NILP (mac_modifier_map_lookup (Vmac_function_modifier, k)))
		keycode = stripped;
	    }
	}

      if (keycode >= 0 && keycode <= 0x7f && keycode_to_xkeysym_table[keycode])
	kind = QCfunction;
      else
	kind = QCordinary;
    }
  else if (type_mask & mouse_button_event_mask)
    {
      code = CGEventGetIntegerValueField (cgevent, kCGMouseEventButtonNumber);

      if (mac_wheel_button_is_mouse_2)
	{
	  if (code == kCGMouseButtonRight)
	    code = kCGMouseButtonCenter;
	  else if (code == kCGMouseButtonCenter)
	    code = kCGMouseButtonRight;
	}

      if (NILP (Vmac_emulate_three_button_mouse))
	map_button_p = true;
      else
	{
	  if (code == kCGMouseButtonLeft
	      && (flags & (kCGEventFlagMaskAlternate
			   | kCGEventFlagMaskCommand)))
	    {
	      CGEventFlags mask_for_center =
		(!EQ (Vmac_emulate_three_button_mouse, Qreverse)
		 ? kCGEventFlagMaskCommand : kCGEventFlagMaskAlternate);

	      if (flags & mask_for_center)
		{
		  code = kCGMouseButtonCenter;
		  flags &= ~mask_for_center;
		}
	      else
		{
		  code = kCGMouseButtonRight;
		  flags &= ~(kCGEventFlagMaskAlternate
			     | kCGEventFlagMaskCommand);
		}
	    }
	}

      kind = QCmouse;
    }
  else if (type_mask & other_mouse_event_mask)
    kind = QCmouse;
  else
    kind = QCordinary;

  if (flags & kCGEventFlagMaskShift)
    emacs_modifiers |= shift_modifier;
  for (i = 0; i < ARRAYELTS (mask_table); i++)
    if (flags & mask_table[i].device_indep)
      {
	Lisp_Object modifier_symbols[NSIDES];
	bool lookup_left_p = true;
	int n = 0;

	if (flags & mask_table[i].device_deps[RIGHT])
	  {
	    if (map_button_p
		&& mac_modifier_map_button (*modifier_maps[i][RIGHT], &code))
	      map_button_p = lookup_left_p = false;
	    else
	      {
		Lisp_Object right_modifier_symbol =
		  mac_modifier_map_lookup (*modifier_maps[i][RIGHT], kind);

		if (!EQ (right_modifier_symbol, Qleft))
		  {
		    modifier_symbols[n++] = right_modifier_symbol;
		    if (!(flags & mask_table[i].device_deps[LEFT]))
		      lookup_left_p = false;
		  }
	      }
	  }
	if (lookup_left_p)
	  {
	    if (map_button_p
		&& mac_modifier_map_button (*modifier_maps[i][LEFT], &code))
	      map_button_p = false;
	    else
	      modifier_symbols[n++] =
		mac_modifier_map_lookup (*modifier_maps[i][LEFT], kind);
	  }

	flags &= ~mask_table[i].device_indep;
	while (n > 0)
	  {
	    Lisp_Object modifier_symbol = modifier_symbols[--n];

	    if (NILP (modifier_symbol))
	      flags |= mask_table[i].device_indep;
	    else
	      {
		Lisp_Object value = Fget (modifier_symbol, Qmodifier_value);

		if (INTEGERP (value))
		  {
		    emacs_modifiers |= XUINT (value);
		    mapped_flags |= mask_table[i].device_indep;
		  }
	      }
	  }
      }

  if (buf == NULL)
    return mapped_flags;

  if (type_mask == CGEventMaskBit (kCGEventKeyDown))
    {
      if (keycode >= 0 && keycode <= 0x7f && keycode_to_xkeysym_table[keycode])
	{
	  buf->kind = NON_ASCII_KEYSTROKE_EVENT;
	  code = 0xff00 | keycode_to_xkeysym_table[keycode];
	}
      else
	{
	  UniCharCount length;
	  UniChar text[2];

	  if ((emacs_modifiers & ~shift_modifier) == 0)
	    CGEventKeyboardGetUnicodeString (cgevent, 2, &length, text);
	  else
	    {
	      CGEventRef tmp = CGEventCreateCopy (cgevent);

	      CGEventSetIntegerValueField (tmp, kCGKeyboardEventKeycode,
					   keycode);
	      CGEventSetFlags (tmp, flags);
	      mac_cgevent_update_unicode_string (tmp);
	      CGEventKeyboardGetUnicodeString (tmp, 2, &length, text);
	      CFRelease (tmp);
	    }

	  if (length == 1)
	    {
	      if (text[0] < 0x80)
		buf->kind = ASCII_KEYSTROKE_EVENT;
	      else
		buf->kind = MULTIBYTE_CHAR_KEYSTROKE_EVENT;
	      code = text[0];
	    }
	  else if (length == 2
		   && UCIsSurrogateHighCharacter (text[0])
		   && UCIsSurrogateLowCharacter (text[1]))
	    {
	      buf->kind = MULTIBYTE_CHAR_KEYSTROKE_EVENT;
	      code = UCGetUnicodeScalarValueForSurrogatePair (text[0], text[1]);
	    }
	  else
	    {
	      buf->kind = NO_EVENT;
	      code = 0;
	    }
	}
      emacs_modifiers |= (extra_keyboard_modifiers
			  & (meta_modifier | alt_modifier
			     | hyper_modifier | super_modifier));
    }

  buf->modifiers = emacs_modifiers;
  buf->code = code;
  buf->timestamp = CGEventGetTimestamp (cgevent) / kMillisecondScale;

  return mapped_flags;
}

void
mac_get_selected_range (struct window *w, CFRange *range)
{
  struct buffer *b = XBUFFER (w->contents);
  EMACS_INT begv = BUF_BEGV (b), zv = BUF_ZV (b);
  EMACS_INT start, end;

  if (w == XWINDOW (selected_window) && b == current_buffer)
    start = PT;
  else
    start = marker_position (w->pointm);

  if (NILP (Vtransient_mark_mode) || NILP (BVAR (b, mark_active))
      || XMARKER (BVAR (b, mark))->buffer == NULL)
    end = start;
  else
    {
      EMACS_INT mark_pos = marker_position (BVAR (b, mark));

      if (start <= mark_pos)
	end = mark_pos;
      else
	{
	  end = start;
	  start = mark_pos;
	}
    }

  if (start != end)
    {
      if (start < begv)
	start = begv;
      else if (start > zv)
	start = zv;

      if (end < begv)
	end = begv;
      else if (end > zv)
	end = zv;
    }

  range->location = start - begv;
  range->length = end - start;
}

/* Store the text of the buffer BUF from START to END as Unicode
   characters in CHARACTERS.  Return true if successful.  */

static bool
mac_store_buffer_text_to_unicode_chars (struct buffer *buf, EMACS_INT start,
					EMACS_INT end, UniChar *characters)
{
  EMACS_INT start_byte = buf_charpos_to_bytepos (buf, start);

#define BUF_FETCH_CHAR_ADVANCE(OUTPUT, BUF, CHARIDX, BYTEIDX)	\
  do    							\
    {								\
      CHARIDX++;						\
      if (!NILP (BVAR (BUF, enable_multibyte_characters)))	\
	{							\
	  unsigned char *ptr = BUF_BYTE_ADDRESS (BUF, BYTEIDX);	\
	  int len;						\
								\
	  OUTPUT= STRING_CHAR_AND_LENGTH (ptr, len);		\
	  BYTEIDX += len;					\
	}							\
      else							\
	{							\
	  OUTPUT = BUF_FETCH_BYTE (BUF, BYTEIDX);		\
	  BYTEIDX++;						\
	}							\
    }								\
  while (0)

  while (start < end)
    {
      int c;

      BUF_FETCH_CHAR_ADVANCE (c, buf, start, start_byte);
      *characters++ = (c < 0xD800 || (c > 0xDFFF && c < 0x10000)) ? c : 0xfffd;
    }

  return true;
}

CGRect
mac_get_first_rect_for_range (struct window *w, const CFRange *range,
			      CFRange *actual_range)
{
  struct buffer *b = XBUFFER (w->contents);
  EMACS_INT start_charpos, end_charpos, min_charpos, max_charpos;
  struct glyph_row *row;
  struct glyph *glyph, *end, *left_glyph, *right_glyph;
  int x, left_x, right_x, text_area_width;

  start_charpos = BUF_BEGV (b) + range->location;
  end_charpos = start_charpos + range->length;
  if (range->length == 0)
    {
      end_charpos++;
      row = row_containing_pos (w, start_charpos,
				MATRIX_FIRST_TEXT_ROW (w->current_matrix),
				NULL, 0);
      if (row == NULL)
	row = MATRIX_ROW (w->current_matrix, w->window_end_vpos);
    }
  else
    {
      struct glyph_row *r2;

      /* Find the rows corresponding to START_CHARPOS and END_CHARPOS.  */
      rows_from_pos_range (w, start_charpos, end_charpos, Qnil, &row, &r2);
      if (row == NULL)
	row = MATRIX_ROW (w->current_matrix, w->window_end_vpos);
      if (r2 == NULL)
	r2 = MATRIX_ROW (w->current_matrix, w->window_end_vpos);
      if (row->y > r2->y)
	row = r2;
    }

  if (!row->reversed_p)
    {
      /* This row is in a left to right paragraph.  Scan it left to
	 right.  */
      glyph = row->glyphs[TEXT_AREA];
      end = glyph + row->used[TEXT_AREA];
      x = row->x;

      /* Skip truncation glyphs at the start of the glyph row.  */
      if (row->displays_text_p)
	for (; glyph < end
	       && INTEGERP (glyph->object)
	       && glyph->charpos < 0;
	     ++glyph)
	  x += glyph->pixel_width;

      /* Scan the glyph row, looking for the first glyph from buffer
	 whose position is between START_CHARPOS and END_CHARPOS.  */
      for (; glyph < end
	     && !INTEGERP (glyph->object)
	     && !(BUFFERP (glyph->object)
		  && (glyph->charpos >= start_charpos
		      && glyph->charpos < end_charpos));
	   ++glyph)
	x += glyph->pixel_width;

      left_x = x;
      left_glyph = glyph;
    }
  else
    {
      /* This row is in a right to left paragraph.  Scan it right to
	 left.  */
      struct glyph *g;

      end = row->glyphs[TEXT_AREA] - 1;
      glyph = end + row->used[TEXT_AREA];

      /* Skip truncation glyphs at the start of the glyph row.  */
      if (row->displays_text_p)
	for (; glyph > end
	       && INTEGERP (glyph->object)
	       && glyph->charpos < 0;
	     --glyph)
	  ;

      /* Scan the glyph row, looking for the first glyph from buffer
	 whose position is between START_CHARPOS and END_CHARPOS.  */
      for (; glyph > end
	     && !INTEGERP (glyph->object)
	     && !(BUFFERP (glyph->object)
		  && (glyph->charpos >= start_charpos
		      && glyph->charpos < end_charpos));
	   --glyph)
	;

      glyph++; /* first glyph to the right of the first rect */
      for (g = row->glyphs[TEXT_AREA], x = row->x; g < glyph; g++)
	x += g->pixel_width;

      right_x = x;
      right_glyph = glyph;
    }

  if (range->length == 0)
    {
      min_charpos = max_charpos = start_charpos;
      if (!row->reversed_p)
	right_x = left_x;
      else
	left_x = right_x;
    }
  else
    {
      if (!row->reversed_p)
	{
	  if (MATRIX_ROW_END_CHARPOS (row) <= end_charpos)
	    {
	      min_charpos = max_charpos = MATRIX_ROW_END_CHARPOS (row);
	      right_x = INT_MAX;
	      right_glyph = end;
	    }
	  else
	    {
	      /* Skip truncation and continuation glyphs near the end
		 of the row, and also blanks and stretch glyphs
		 inserted by extend_face_to_end_of_line.  */
	      while (end > glyph
		     && INTEGERP ((end - 1)->object))
		--end;
	      /* Scan the rest of the glyph row from the end, looking
		 for the first glyph whose position is between
		 START_CHARPOS and END_CHARPOS */
	      for (--end;
		   end > glyph
		     && !INTEGERP (end->object)
		     && !(BUFFERP (end->object)
			  && (end->charpos >= start_charpos
			      && end->charpos < end_charpos));
		   --end)
		;
	      /* Find the X coordinate of the last glyph of the first
		 rect.  */
	      for (; glyph <= end; ++glyph)
		x += glyph->pixel_width;

	      min_charpos = end_charpos;
	      max_charpos = start_charpos;
	      right_x = x;
	      right_glyph = glyph;
	    }
	}
      else
	{
	  if (MATRIX_ROW_END_CHARPOS (row) <= end_charpos)
	    {
	      min_charpos = max_charpos = MATRIX_ROW_END_CHARPOS (row);
	      left_x = 0;
	      left_glyph = end + 1;
	    }
	  else
	    {
	      /* Skip truncation and continuation glyphs near the end
		 of the row, and also blanks and stretch glyphs
		 inserted by extend_face_to_end_of_line.  */
	      x = row->x;
	      end++;
	      while (end < glyph
		     && INTEGERP (end->object))
		{
		  x += end->pixel_width;
		  ++end;
		}
	      /* Scan the rest of the glyph row from the end, looking
		 for the first glyph whose position is between
		 START_CHARPOS and END_CHARPOS */
	      for ( ;
		    end < glyph
		      && !INTEGERP (end->object)
		      && !(BUFFERP (end->object)
			   && (end->charpos >= start_charpos
			       && end->charpos < end_charpos));
		    ++end)
		x += end->pixel_width;

	      min_charpos = end_charpos;
	      max_charpos = start_charpos;
	      left_x = x;
	      left_glyph = end;
	    }
	}

      for (glyph = left_glyph; glyph < right_glyph; ++glyph)
	if (!STRINGP (glyph->object) && glyph->charpos > 0)
	  {
	    if (glyph->charpos < min_charpos)
	      min_charpos = glyph->charpos;
	    if (glyph->charpos + 1 > max_charpos)
	      max_charpos = glyph->charpos + 1;
	  }
      if (min_charpos > max_charpos)
	min_charpos = max_charpos;
    }

  if (actual_range)
    {
      actual_range->location = min_charpos - BUF_BEGV (b);
      actual_range->length = max_charpos - min_charpos;
    }

  text_area_width = window_box_width (w, TEXT_AREA);
  if (left_x < 0)
    left_x = 0;
  else if (left_x > text_area_width)
    left_x = text_area_width;
  if (right_x < 0)
    right_x = 0;
  else if (right_x > text_area_width)
    right_x = text_area_width;

  return CGRectMake (WINDOW_TEXT_TO_FRAME_PIXEL_X (w, left_x),
		     WINDOW_TO_FRAME_PIXEL_Y (w, row->y),
		     right_x - left_x, row->height);
}

void
mac_ax_selected_text_range (struct frame *f, CFRange *range)
{
  mac_get_selected_range (XWINDOW (f->selected_window), range);
}

EMACS_INT
mac_ax_number_of_characters (struct frame *f)
{
  struct buffer *b = XBUFFER (XWINDOW (f->selected_window)->contents);

  return BUF_ZV (b) - BUF_BEGV (b);
}

void
mac_ax_visible_character_range (struct frame *f, CFRange *range)
{
  struct window *w = XWINDOW (f->selected_window);
  struct buffer *b = XBUFFER (w->contents);
  EMACS_INT start, end;

  /* XXX: Check validity of window_end_pos?  */
  start = marker_position (w->start);
  end = BUF_Z (b) - w->window_end_pos;

  range->location = start - BUF_BEGV (b);
  range->length = end - start;
}

EMACS_INT
mac_ax_line_for_index (struct frame *f, EMACS_INT index)
{
  struct buffer *b = XBUFFER (XWINDOW (f->selected_window)->contents);
  EMACS_INT line;
  const unsigned char *limit, *begv, *zv, *gap_end, *p;

  if (index >= 0)
    limit = BUF_CHAR_ADDRESS (b, BUF_BEGV (b) + index);
  else
    limit = BUF_BYTE_ADDRESS (b, BUF_PT_BYTE (b));
  begv = BUF_BYTE_ADDRESS (b, BUF_BEGV_BYTE (b));
  zv = BUF_BYTE_ADDRESS (b, BUF_ZV_BYTE (b));

  if (limit < begv || limit > zv)
    return -1;

  line = 0;
  gap_end = BUF_GAP_END_ADDR (b);
  if (begv < gap_end && gap_end <= limit)
    {
      for (p = gap_end; (p = memchr (p, '\n', limit - p)) != NULL; p++)
	line++;
      limit = BUF_GPT_ADDR (b);
    }

  for (p = begv; (p = memchr (p, '\n', limit - p)) != NULL; p++)
    line++;

  return line;
}

static const unsigned char *
mac_ax_buffer_skip_lines (struct buffer *buf, EMACS_INT n,
			  const unsigned char *start, const unsigned char *end)
{
  const unsigned char *gpt, *p, *limit;

  gpt = BUF_GPT_ADDR (buf);
  p = start;

  if (p <= gpt && gpt < end)
    limit = gpt;
  else
    limit = end;

  while (n > 0)
    {
      p = memchr (p, '\n', limit - p);
      if (p)
	p++;
      else if (limit == end)
	break;
      else
	{
	  p = BUF_GAP_END_ADDR (buf);
	  p = memchr (p, '\n', end - p);
	  if (p)
	    p++;
	  else
	    break;
	}
      n--;
    }

  return p;
}

int
mac_ax_range_for_line (struct frame *f, EMACS_INT line, CFRange *range)
{
  struct buffer *b = XBUFFER (XWINDOW (f->selected_window)->contents);
  const unsigned char *begv, *zv, *p;
  EMACS_INT start, end;

  if (line < 0)
    return 0;

  begv = BUF_BYTE_ADDRESS (b, BUF_BEGV_BYTE (b));
  zv = BUF_BYTE_ADDRESS (b, BUF_ZV_BYTE (b));

  p = mac_ax_buffer_skip_lines (b, line, begv, zv);
  if (p == NULL)
    return 0;

  start = buf_bytepos_to_charpos (b, BUF_PTR_BYTE_POS (b, p));
  p = mac_ax_buffer_skip_lines (b, 1, p, zv);
  if (p)
    end = buf_bytepos_to_charpos (b, BUF_PTR_BYTE_POS (b, p));
  else
    end = BUF_ZV (b);

  range->location = start - BUF_BEGV (b);
  range->length = end - start;

  return 1;
}

CFStringRef
mac_ax_create_string_for_range (struct frame *f, const CFRange *range,
				CFRange *actual_range)
{
  struct buffer *b = XBUFFER (XWINDOW (f->selected_window)->contents);
  CFStringRef result = NULL;
  EMACS_INT start, end, begv = BUF_BEGV (b), zv = BUF_ZV (b);

  start = begv + range->location;
  end = start + range->length;
  if (start < begv)
    start = begv;
  else if (start > zv)
    start = zv;
  if (end < begv)
    end = begv;
  else if (end > zv)
    end = zv;
  if (start <= end)
    {
      EMACS_INT length = end - start;
      UniChar *characters = xmalloc (length * sizeof (UniChar));

      if (mac_store_buffer_text_to_unicode_chars (b, start, end, characters))
	{
	  result = CFStringCreateWithCharacters (NULL, characters, length);
	  if (actual_range)
	    {
	      actual_range->location = start - begv;
	      actual_range->length = length;
	    }
	}
      xfree (characters);
    }

  return result;
}

/* Return true if and only if a key-down Quartz event CGEVENT is
   regarded as a quit char event.  */

bool
mac_keydown_cgevent_quit_p (CGEventRef cgevent)
{
  struct input_event buf;

  mac_cgevent_to_input_event (cgevent, &buf);
  if (buf.kind == ASCII_KEYSTROKE_EVENT)
    {
      int c = buf.code & 0377;

      if (buf.modifiers & ctrl_modifier)
	c = make_ctrl_char (c);

      c |= (buf.modifiers
	    & (meta_modifier | alt_modifier
	       | hyper_modifier | super_modifier));

      return c == quit_char;
    }
  else
    return false;
}

void
mac_store_apple_event (Lisp_Object class, Lisp_Object id, const AEDesc *desc)
{
  struct input_event buf;

  EVENT_INIT (buf);

  buf.kind = MAC_APPLE_EVENT;
  buf.x = class;
  buf.y = id;
  XSETFRAME (buf.frame_or_window,
	     mac_focus_frame (&one_mac_display_info));
  /* Now that Lisp object allocations are protected by BLOCK_INPUT, it
     is safe to use them during read_socket_hook.  */
  buf.arg = mac_aedesc_to_lisp (desc);
  kbd_buffer_store_event (&buf);
}

OSStatus
mac_store_event_ref_as_apple_event (AEEventClass class, AEEventID id,
				    Lisp_Object class_key, Lisp_Object id_key,
				    EventRef event, UInt32 num_params,
				    const EventParamName *names,
				    const EventParamType *types)
{
  OSStatus err = eventNotHandledErr;
  Lisp_Object binding;

  binding = mac_find_apple_event_spec (class, id, &class_key, &id_key);
  if (!NILP (binding) && !EQ (binding, Qundefined))
    {
      if (INTEGERP (binding))
	err = XINT (binding);
      else
	{
	  struct input_event buf;

	  EVENT_INIT (buf);

	  buf.kind = MAC_APPLE_EVENT;
	  buf.x = class_key;
	  buf.y = id_key;
	  XSETFRAME (buf.frame_or_window,
		     mac_focus_frame (&one_mac_display_info));
	  /* Now that Lisp object allocations are protected by
	     BLOCK_INPUT, it is safe to use them during
	     read_socket_hook.  */
	  buf.arg = Fcons (build_string ("aevt"),
			   mac_event_parameters_to_lisp (event, num_params,
							 names, types));
	  kbd_buffer_store_event (&buf);
	  err = noErr;
	}
    }

  return err;
}

static void
mac_handle_cg_display_reconfig (CGDirectDisplayID display,
				CGDisplayChangeSummaryFlags flags,
				void *user_info)
{
  mac_screen_config_changed = 1;
}

static OSErr
init_dm_notification_handler (void)
{
  CGDisplayRegisterReconfigurationCallback (mac_handle_cg_display_reconfig,
					    NULL);

  return noErr;
}

static void
mac_handle_tis_notification (CFNotificationCenterRef center, void *observer,
			     CFStringRef name, const void *object,
			     CFDictionaryRef userInfo)
{
  struct input_event buf;
  Lisp_Object arg;

  EVENT_INIT (buf);

  buf.kind = MAC_APPLE_EVENT;
  buf.x = Qtext_input;
  buf.y = Qnotification;
  XSETFRAME (buf.frame_or_window, mac_focus_frame (&one_mac_display_info));
  arg = list1 (Fcons (Qname, (Fcons (build_string ("Lisp"),
				     cfstring_to_lisp_nodecode (name)))));
  buf.arg = Fcons (build_string ("aevt"), arg);
  kbd_buffer_store_event (&buf);
}

static void
init_tis_notification_handler (void)
{
  CFNotificationCenterRef center = CFNotificationCenterGetDistributedCenter ();

  CFNotificationCenterAddObserver (center, NULL, mac_handle_tis_notification,
				   kTISNotifySelectedKeyboardInputSourceChanged,
				   NULL, CFNotificationSuspensionBehaviorDrop);
  CFNotificationCenterAddObserver (center, NULL, mac_handle_tis_notification,
				   kTISNotifyEnabledKeyboardInputSourcesChanged,
				   NULL,
				   CFNotificationSuspensionBehaviorCoalesce);
}


/***********************************************************************
			    Initialization
 ***********************************************************************/

static int mac_initialized = 0;

static XrmDatabase
mac_make_rdb (const char *xrm_option)
{
  XrmDatabase database;

  database = xrm_get_preference_database (NULL);
  if (xrm_option)
    xrm_merge_string_database (database, xrm_option);

  return database;
}

struct mac_display_info *
mac_term_init (Lisp_Object display_name, char *xrm_option, char *resource_name)
{
  struct terminal *terminal;
  struct mac_display_info *dpyinfo;

  block_input ();

  if (!mac_initialized)
    {
      mac_initialize ();
      mac_initialized = 1;
    }

  if (x_display_list)
    error ("Sorry, this version can only handle one display");

  dpyinfo = &one_mac_display_info;
  memset (dpyinfo, 0, sizeof (*dpyinfo));
  terminal = mac_create_terminal (dpyinfo);

  terminal->kboard = allocate_kboard (Qmac);
  /* Don't let the initial kboard remain current longer than necessary.
     That would cause problems if a file loaded on startup tries to
     prompt in the mini-buffer.  */
  if (current_kboard == initial_kboard)
    current_kboard = terminal->kboard;
  terminal->kboard->reference_count++;

  /* Put this display on the chain.  */
  dpyinfo->next = x_display_list;
  x_display_list = dpyinfo;

  dpyinfo->name_list_element = Fcons (display_name, Qnil);

  /* http://lists.gnu.org/archive/html/emacs-devel/2015-11/msg00194.html  */
  dpyinfo->smallest_font_height = 1;
  dpyinfo->smallest_char_width = 1;

  /* Set the name of the terminal. */
  terminal->name = xlispstrdup (display_name);

  Lisp_Object system_name = Fsystem_name ();
  ptrdiff_t nbytes;
  if (INT_ADD_WRAPV (SBYTES (Vinvocation_name), SBYTES (system_name) + 2,
		     &nbytes))
    memory_full (SIZE_MAX);
  dpyinfo->mac_id_name = xmalloc (nbytes);
  char *nametail = lispstpcpy (dpyinfo->mac_id_name, Vinvocation_name);
  *nametail++ = '@';
  lispstpcpy (nametail, system_name);

  /* Get the scroll bar cursor.  */
  dpyinfo->vertical_scroll_bar_cursor =
    mac_cursor_create (kThemeArrowCursor, NULL, NULL);

  /* Put the rdb where we can find it in a way that works on
     all versions.  */
  dpyinfo->xrdb = mac_make_rdb (xrm_option);
  x_display_rdb_list = Fcons (dpyinfo->xrdb, x_display_rdb_list);

  mac_get_screen_info (dpyinfo);

  reset_mouse_highlight (&dpyinfo->mouse_highlight);

  dpyinfo->resx = 72.0;
  dpyinfo->resy = 72.0;

  add_keyboard_wait_descriptor (0);

  /* In Mac GUI, asynchronous I/O (using SIGIO) can't be used for
     window events because they don't come from sockets, even though
     it works fine on tty's.  */
  Fset_input_interrupt_mode (Qnil);

  mac_init_fringe (terminal->rif);

  unblock_input ();

  return dpyinfo;
}

/* Get rid of display DPYINFO, assuming all frames are already gone.  */

static void
x_delete_display (struct mac_display_info *dpyinfo)
{
  struct terminal *t;

  /* Close all frames and delete the generic struct terminal for this
     display.  */
  for (t = terminal_list; t; t = t->next_terminal)
    if (t->type == output_mac && t->display_info.mac == dpyinfo)
      {
        delete_terminal (t);
        break;
      }

  if (x_display_list == dpyinfo)
    {
      x_display_list = dpyinfo->next;
      x_display_rdb_list = XCDR (x_display_rdb_list);
    }
  else
    {
      struct x_display_info *tail;
      Lisp_Object tail_rdb;

      for (tail = x_display_list, tail_rdb = x_display_rdb_list; tail;
	   tail = tail->next, tail_rdb = XCDR (tail_rdb))
	if (tail->next == dpyinfo)
	  {
	    tail->next = tail->next->next;
	    XSETCDR (tail_rdb, XCDR (XCDR (tail_rdb)));
	  }
    }

  xfree (dpyinfo->mac_id_name);
}


static void
mac_handle_user_signal (int sig)
{
  mac_wakeup_from_run_loop_run_once ();
}

static void
record_startup_key_modifiers (void)
{
  Vmac_startup_options = Fcons (Fcons (Qkeyboard_modifiers,
				       make_number (GetCurrentKeyModifiers ())),
				Vmac_startup_options);
}

/* Set up use of X before we make the first connection.  */

static struct redisplay_interface mac_redisplay_interface =
  {
    mac_frame_parm_handlers,
    x_produce_glyphs,
    x_write_glyphs,
    x_insert_glyphs,
    x_clear_end_of_line,
    x_scroll_run,
    x_after_update_window_line,
    x_update_window_begin,
    x_update_window_end,
    mac_flush,
    x_clear_window_mouse_face,
    x_get_glyph_overhangs,
    x_fix_overlapping_area,
    x_draw_fringe_bitmap,
    mac_define_fringe_bitmap,
    mac_destroy_fringe_bitmap,
    x_compute_glyph_string_overhangs,
    x_draw_glyph_string,
    mac_define_frame_cursor,
    mac_clear_frame_area,
    mac_draw_window_cursor,
    mac_draw_vertical_window_border,
    mac_draw_window_divider,
    mac_shift_glyphs_for_insert, /* Never called; see comment in function.  */
    mac_show_hourglass,
    mac_hide_hourglass
  };


/* This function is called when the last frame on a display is deleted. */
void
x_delete_terminal (struct terminal *terminal)
{
  struct mac_display_info *dpyinfo = terminal->display_info.mac;

  /* Protect against recursive calls.  delete_frame in
     delete_terminal calls us back when it deletes our last frame.  */
  if (!terminal->name)
    return;

  block_input ();
  x_destroy_all_bitmaps (dpyinfo);

  /* No more input on this descriptor.  */
  delete_keyboard_wait_descriptor (0);

  x_delete_display (dpyinfo);
  unblock_input ();
}

static struct terminal *
mac_create_terminal (struct mac_display_info *dpyinfo)
{
  struct terminal *terminal;

  terminal = create_terminal (output_mac, &mac_redisplay_interface);

  terminal->display_info.mac = dpyinfo;
  dpyinfo->terminal = terminal;

  /* kboard is initialized in mac_term_init. */

  terminal->clear_frame_hook = x_clear_frame;
  terminal->ins_del_lines_hook = x_ins_del_lines;
  terminal->delete_glyphs_hook = x_delete_glyphs;
  terminal->ring_bell_hook = mac_ring_bell;
  terminal->toggle_invisible_pointer_hook = NULL;
  terminal->update_begin_hook = x_update_begin;
  terminal->update_end_hook = x_update_end;
  terminal->read_socket_hook = mac_read_socket;
  terminal->frame_up_to_date_hook = mac_frame_up_to_date;
  terminal->mouse_position_hook = mac_mouse_position;
  terminal->frame_rehighlight_hook = mac_frame_rehighlight;
  terminal->frame_raise_lower_hook = mac_frame_raise_lower;
  terminal->fullscreen_hook = mac_fullscreen_hook;
  terminal->menu_show_hook = mac_menu_show;
  terminal->popup_dialog_hook = mac_popup_dialog;
  terminal->set_vertical_scroll_bar_hook = mac_set_vertical_scroll_bar;
  terminal->set_horizontal_scroll_bar_hook = mac_set_horizontal_scroll_bar;
  terminal->condemn_scroll_bars_hook = mac_condemn_scroll_bars;
  terminal->redeem_scroll_bar_hook = mac_redeem_scroll_bar;
  terminal->judge_scroll_bars_hook = mac_judge_scroll_bars;
  terminal->delete_frame_hook = x_destroy_window;
  terminal->delete_terminal_hook = x_delete_terminal;
  /* Other hooks are NULL by default.  */

  return terminal;
}

static void
mac_initialize (void)
{
  baud_rate = 19200;

  block_input ();

  if (init_wakeup_fds () < 0)
    emacs_abort ();

  handle_user_signal_hook = mac_handle_user_signal;

  init_coercion_handler ();

  init_dm_notification_handler ();

  init_tis_notification_handler ();

  install_application_handler ();

  init_cg_color ();

  record_startup_key_modifiers ();

  unblock_input ();
}


void
syms_of_macterm (void)
{
  DEFSYM (Qcontrol, "control");
  DEFSYM (Qmeta, "meta");
  DEFSYM (Qalt, "alt");
  DEFSYM (Qhyper, "hyper");
  DEFSYM (Qsuper, "super");
  DEFSYM (Qmodifier_value, "modifier-value");
  DEFSYM (QCordinary, ":ordinary");
  DEFSYM (QCfunction, ":function");
  DEFSYM (QCmouse, ":mouse");
  DEFSYM (QCbutton, ":button");

  Fput (Qcontrol, Qmodifier_value, make_number (ctrl_modifier));
  Fput (Qmeta,    Qmodifier_value, make_number (meta_modifier));
  Fput (Qalt,     Qmodifier_value, make_number (alt_modifier));
  Fput (Qhyper,   Qmodifier_value, make_number (hyper_modifier));
  Fput (Qsuper,   Qmodifier_value, make_number (super_modifier));

  DEFSYM (Qpanel_closed, "panel-closed");
  DEFSYM (Qselection, "selection");

  DEFSYM (Qservice, "service");
  DEFSYM (Qpaste, "paste");
  DEFSYM (Qperform, "perform");

  DEFSYM (Qtext_input, "text-input");
  DEFSYM (Qinsert_text, "insert-text");
  DEFSYM (Qset_marked_text, "set-marked-text");
  DEFSYM (Qnotification, "notification");

  DEFSYM (Qaction, "action");
  DEFSYM (Qmac_action_key_paths, "mac-action-key-paths");

  DEFSYM (Qaccessibility, "accessibility");

  DEFSYM (Qreverse, "reverse");

  DEFSYM (Qkeyboard_modifiers, "keyboard-modifiers");

  DEFSYM (Qautomatic, "automatic");
  DEFSYM (Qinverted, "inverted");

  DEFSYM (QCdirection_inverted_from_device_p,
	  ":direction-inverted-from-device-p");
  DEFSYM (QCdelta_x, ":delta-x");
  DEFSYM (QCdelta_y, ":delta-y");
  DEFSYM (QCdelta_z, ":delta-z");
  DEFSYM (QCscrolling_delta_x, ":scrolling-delta-x");
  DEFSYM (QCscrolling_delta_y, ":scrolling-delta-y");
  DEFSYM (QCphase, ":phase");
  DEFSYM (QCmomentum_phase, ":momentum-phase");
  DEFSYM (QCswipe_tracking_from_scroll_events_enabled_p,
	  ":swipe-tracking-from-scroll-events-enabled-p");
  DEFSYM (Qbegan, "began");
  DEFSYM (Qstationary, "stationary");
  DEFSYM (Qchanged, "changed");
  DEFSYM (Qended, "ended");
  DEFSYM (Qcancelled, "cancelled");
  DEFSYM (Qmay_begin, "may-begin");
  DEFSYM (QCmagnification, ":magnification");

  staticpro (&x_display_rdb_list);
  x_display_rdb_list = Qnil;

  /* We don't yet support this, but defining this here avoids whining
     from cus-start.el and other places, like "M-x set-variable".  */
  DEFVAR_BOOL ("x-use-underline-position-properties",
	       x_use_underline_position_properties,
     doc: /* Non-nil means make use of UNDERLINE_POSITION font properties.
A value of nil means ignore them.  If you encounter fonts with bogus
UNDERLINE_POSITION font properties, for example 7x13 on XFree prior
to 4.1, set this to nil.  */);
  x_use_underline_position_properties = true;

  DEFVAR_BOOL ("x-underline-at-descent-line",
	       x_underline_at_descent_line,
     doc: /* Non-nil means to draw the underline at the same place as the descent line.
A value of nil means to draw the underline according to the value of the
variable `x-use-underline-position-properties', which is usually at the
baseline level.  The default value is nil.  */);
  x_underline_at_descent_line = false;

  DEFVAR_LISP ("x-toolkit-scroll-bars", Vx_toolkit_scroll_bars,
    doc: /* If not nil, Emacs uses toolkit scroll bars.  */);
  Vx_toolkit_scroll_bars = Qt;

  DEFVAR_BOOL ("mac-redisplay-dont-reset-vscroll", mac_redisplay_dont_reset_vscroll,
	       doc: /* Non-nil means update doesn't reset vscroll.  */);
  mac_redisplay_dont_reset_vscroll = false;

  DEFVAR_BOOL ("mac-ignore-momentum-wheel-events", mac_ignore_momentum_wheel_events,
	       doc: /* Non-nil means momentum wheel events are ignored.  */);
  mac_ignore_momentum_wheel_events = false;

/* Variables to configure modifier key assignment.  */

  DEFVAR_LISP ("mac-control-modifier", Vmac_control_modifier,
    doc: /* Modifier key assumed when the Mac control key is pressed.
The value is of the form either SYMBOL or `(:ordinary SYMBOL :function
SYMBOL :mouse SYMBOL)'.  The latter allows us to specify different
behaviors among ordinary keys, function keys, and mouse operations.

Each SYMBOL can be `control', `meta', `alt', `hyper', or `super' for
the respective modifier.  The default is `control'.

The property list form can include the `:button' property for button
number mapping, which becomes active when the value of
`mac-emulate-three-button-mouse' is nil.  The `:button' property can
be either a positive integer specifying the destination of the primary
button only, or a list (VALUE-FOR-PRIMARY-BUTTON VALUE-FOR-MOUSE-2
VALUE-FOR-MOUSE-3 ...) of positive integers specifying the
destinations of multiple buttons in order.  Note that the secondary
button and the button 3 (usually the wheel button) correspond to
mouse-3 and mouse-2 respectively if the value of
`mac-wheel-button-is-mouse-2' is non-nil (default), and mouse-2 and
mouse-3 respectively otherwise.  If a button is mapped to the same
number as its source, then it behaves as if the button were not mapped
so the `:mouse' property becomes in effect instead.  */);
  Vmac_control_modifier = Qcontrol;

  DEFVAR_LISP ("mac-right-control-modifier", Vmac_right_control_modifier,
    doc: /* Modifier key assumed when the Mac right control key is pressed.
The value is of the form either SYMBOL or `(:ordinary SYMBOL :function
SYMBOL :mouse SYMBOL)'.  The latter allows us to specify different
behaviors among ordinary keys, function keys, and mouse operations.

Each SYMBOL can be `control', `meta', `alt', `hyper', or `super' for
the respective modifier.  The value `left' means the same setting as
`mac-control-modifier'.  The default is `left'.

The property list form can include the `:button' property for button
number mapping, which becomes active when the value of
`mac-emulate-three-button-mouse' is nil.  The `:button' property can
be either a positive integer specifying the destination of the primary
button only, or a list (VALUE-FOR-PRIMARY-BUTTON VALUE-FOR-MOUSE-2
VALUE-FOR-MOUSE-3 ...) of positive integers specifying the
destinations of multiple buttons in order.  Note that the secondary
button and the button 3 (usually the wheel button) correspond to
mouse-3 and mouse-2 respectively if the value of
`mac-wheel-button-is-mouse-2' is non-nil (default), and mouse-2 and
mouse-3 respectively otherwise.  If a button is mapped to the same
number as its source, then it behaves as if the button were not mapped
so the `:mouse' property becomes in effect instead.

Note: the left and right versions cannot be distinguished on some
environments such as Screen Sharing.  Also, certain combinations of a
key with both versions of the same modifier do not emit events at the
system level.  */);
  Vmac_right_control_modifier = Qleft;

  DEFVAR_LISP ("mac-option-modifier", Vmac_option_modifier,
    doc: /* Modifier key assumed when the Mac alt/option key is pressed.
The value is of the form either SYMBOL or `(:ordinary SYMBOL :function
SYMBOL :mouse SYMBOL)'.  The latter allows us to specify different
behaviors among ordinary keys, function keys, and mouse operations.

Each SYMBOL can be `control', `meta', `alt', `hyper', or `super' for
the respective modifier.  If the value is nil then the key will act as
the normal Mac option modifier, and the option key can be used to
compose characters depending on the chosen Mac keyboard setting.

The property list form can include the `:button' property for button
number mapping, which becomes active when the value of
`mac-emulate-three-button-mouse' is nil.  The `:button' property can
be either a positive integer specifying the destination of the primary
button only, or a list (VALUE-FOR-PRIMARY-BUTTON VALUE-FOR-MOUSE-2
VALUE-FOR-MOUSE-3 ...) of positive integers specifying the
destinations of multiple buttons in order.  Note that the secondary
button and the button 3 (usually the wheel button) correspond to
mouse-3 and mouse-2 respectively if the value of
`mac-wheel-button-is-mouse-2' is non-nil (default), and mouse-2 and
mouse-3 respectively otherwise.  If a button is mapped to the same
number as its source, then it behaves as if the button were not mapped
so the `:mouse' property becomes in effect instead.  */);
  Vmac_option_modifier = list4 (QCfunction, Qalt, QCmouse, Qalt);

  DEFVAR_LISP ("mac-right-option-modifier", Vmac_right_option_modifier,
    doc: /* Modifier key assumed when the Mac right option key is pressed.
The value is of the form either SYMBOL or `(:ordinary SYMBOL :function
SYMBOL :mouse SYMBOL)'.  The latter allows us to specify different
behaviors among ordinary keys, function keys, and mouse operations.

Each SYMBOL can be `control', `meta', `alt', `hyper', or `super' for
the respective modifier.  If the value is nil then the key will act as
the normal Mac option modifier, and the option key can be used to
compose characters depending on the chosen Mac keyboard setting.  The
value `left' means the same setting as `mac-option-modifier'.  The
default is `left'.

The property list form can include the `:button' property for button
number mapping, which becomes active when the value of
`mac-emulate-three-button-mouse' is nil.  The `:button' property can
be either a positive integer specifying the destination of the primary
button only, or a list (VALUE-FOR-PRIMARY-BUTTON VALUE-FOR-MOUSE-2
VALUE-FOR-MOUSE-3 ...) of positive integers specifying the
destinations of multiple buttons in order.  Note that the secondary
button and the button 3 (usually the wheel button) correspond to
mouse-3 and mouse-2 respectively if the value of
`mac-wheel-button-is-mouse-2' is non-nil (default), and mouse-2 and
mouse-3 respectively otherwise.  If a button is mapped to the same
number as its source, then it behaves as if the button were not mapped
so the `:mouse' property becomes in effect instead.

Note: the left and right versions cannot be distinguished on some
environments such as Screen Sharing.  Also, certain combinations of a
key with both versions of the same modifier do not emit events at the
system level.  */);
  Vmac_right_option_modifier = Qleft;

  DEFVAR_LISP ("mac-command-modifier", Vmac_command_modifier,
    doc: /* Modifier key assumed when the Mac command key is pressed.
The value is of the form either SYMBOL or `(:ordinary SYMBOL :function
SYMBOL :mouse SYMBOL)'.  The latter allows us to specify different
behaviors among ordinary keys, function keys, and mouse operations.

Each SYMBOL can be `control', `meta', `alt', `hyper', or `super' for
the respective modifier.  The default is `meta'.

The property list form can include the `:button' property for button
number mapping, which becomes active when the value of
`mac-emulate-three-button-mouse' is nil.  The `:button' property can
be either a positive integer specifying the destination of the primary
button only, or a list (VALUE-FOR-PRIMARY-BUTTON VALUE-FOR-MOUSE-2
VALUE-FOR-MOUSE-3 ...) of positive integers specifying the
destinations of multiple buttons in order.  Note that the secondary
button and the button 3 (usually the wheel button) correspond to
mouse-3 and mouse-2 respectively if the value of
`mac-wheel-button-is-mouse-2' is non-nil (default), and mouse-2 and
mouse-3 respectively otherwise.  If a button is mapped to the same
number as its source, then it behaves as if the button were not mapped
so the `:mouse' property becomes in effect instead.  */);
  Vmac_command_modifier = Qmeta;

  DEFVAR_LISP ("mac-right-command-modifier", Vmac_right_command_modifier,
    doc: /* Modifier key assumed when the Mac right command key is pressed.
The value is of the form either SYMBOL or `(:ordinary SYMBOL :function
SYMBOL :mouse SYMBOL)'.  The latter allows us to specify different
behaviors among ordinary keys, function keys, and mouse operations.

Each SYMBOL can be `control', `meta', `alt', `hyper', or `super' for
the respective modifier.  The value `left' means the same setting as
`mac-command-modifier'.  The default is `left'.

The property list form can include the `:button' property for button
number mapping, which becomes active when the value of
`mac-emulate-three-button-mouse' is nil.  The `:button' property can
be either a positive integer specifying the destination of the primary
button only, or a list (VALUE-FOR-PRIMARY-BUTTON VALUE-FOR-MOUSE-2
VALUE-FOR-MOUSE-3 ...) of positive integers specifying the
destinations of multiple buttons in order.  Note that the secondary
button and the button 3 (usually the wheel button) correspond to
mouse-3 and mouse-2 respectively if the value of
`mac-wheel-button-is-mouse-2' is non-nil (default), and mouse-2 and
mouse-3 respectively otherwise.  If a button is mapped to the same
number as its source, then it behaves as if the button were not mapped
so the `:mouse' property becomes in effect instead.

Note: the left and right versions cannot be distinguished on some
environments such as Screen Sharing.  Also, certain combinations of a
key with both versions of the same modifier do not emit events at the
system level.  */);
  Vmac_right_command_modifier = Qleft;

  DEFVAR_LISP ("mac-function-modifier", Vmac_function_modifier,
    doc: /* Modifier key assumed when the Mac function (fn) key is pressed.
The value is of the form either SYMBOL or `(:ordinary SYMBOL :function
SYMBOL :mouse SYMBOL)'.  The latter allows us to specify different
behaviors among ordinary keys, function keys, and mouse operations.

Each SYMBOL can be `control', `meta', `alt', `hyper', or `super' for
the respective modifier.  If the value is nil, then the key will act
as the normal Mac function (fn) key.

The property list form can include the `:button' property for button
number mapping, which becomes active when the value of
`mac-emulate-three-button-mouse' is nil.  The `:button' property can
be either a positive integer specifying the destination of the primary
button only, or a list (VALUE-FOR-PRIMARY-BUTTON VALUE-FOR-MOUSE-2
VALUE-FOR-MOUSE-3 ...) of positive integers specifying the
destinations of multiple buttons in order.  Note that the secondary
button and the button 3 (usually the wheel button) correspond to
mouse-3 and mouse-2 respectively if the value of
`mac-wheel-button-is-mouse-2' is non-nil (default), and mouse-2 and
mouse-3 respectively otherwise.  If a button is mapped to the same
number as its source, then it behaves as if the button were not mapped
so the `:mouse' property becomes in effect instead.

The default value is `(:button 2)' meaning that the primary button is
recognized as mouse-2 if the Mac function (fn) key is pressed.  */);
  Vmac_function_modifier = list2 (QCbutton, make_number (2));

  DEFVAR_LISP ("mac-emulate-three-button-mouse",
	       Vmac_emulate_three_button_mouse,
    doc: /* Specify a way of three button mouse emulation.
The value can be nil, t, or the symbol `reverse'.  The latter two
provide handy settings especially for one-button mice.

A value of nil means that the modifier variable settings, if any, are
used for mapping button numbers.  If you need more flexible settings
than what t or the symbol `reverse' below provides, then set this
variable to nil and customize the modifier variables
`mac-control-modifier', `mac-right-control-modifier',
`mac-option-modifier', `mac-right-option-modifier',
`mac-command-modifier', `mac-right-command-modifier', and/or
`mac-function-modifier' so their values have the `:button' property.

t means that when the option-key is held down while pressing the mouse
button, the click will register as mouse-2 and while the command-key
is held down, the click will register as mouse-3.

The symbol `reverse' means that the option-key will register for
mouse-3 and the command-key will register for mouse-2.  */);
  Vmac_emulate_three_button_mouse = Qnil;

  DEFVAR_BOOL ("mac-wheel-button-is-mouse-2", mac_wheel_button_is_mouse_2,
    doc: /* Non-nil if the wheel button is mouse-2 and the right click mouse-3.
Otherwise, the right click will be treated as mouse-2 and the wheel
button will be mouse-3.  */);
  mac_wheel_button_is_mouse_2 = true;

  DEFVAR_BOOL ("mac-pass-command-to-system", mac_pass_command_to_system,
    doc: /* Non-nil if command key presses are passed on to the Mac Toolbox.  */);
  mac_pass_command_to_system = true;

  DEFVAR_BOOL ("mac-pass-control-to-system", mac_pass_control_to_system,
    doc: /* Non-nil if control key presses are passed on to the Mac Toolbox.  */);
  mac_pass_control_to_system = true;

  DEFVAR_LISP ("mac-startup-options", Vmac_startup_options,
    doc: /* Alist of Mac-specific startup options.
Each element looks like (OPTION-TYPE . OPTIONS).
OPTION-TYPE is a symbol specifying the type of startup options:

  command-line -- List of Mac-specific command line options.
  apple-event -- Apple event that came with the \"Open Application\" event.
  keyboard-modifiers -- Number representing keyboard modifiers on startup.
    See also `mac-keyboard-modifier-mask-alist'.  */);
  Vmac_startup_options = Qnil;

  DEFVAR_LISP ("mac-ts-active-input-overlay", Vmac_ts_active_input_overlay,
    doc: /* Overlay used to display Mac TSM active input area.  */);
  Vmac_ts_active_input_overlay = Qnil;

  DEFVAR_BOOL ("mac-drawing-use-gcd", mac_drawing_use_gcd,
    doc: /* Non-nil means graphical drawing uses GCD (Grand Central Dispatch).
It allows us to perform graphical drawing operations in a non-main
thread in some situations.  */);
  mac_drawing_use_gcd = true;

  DEFVAR_LISP ("mac-frame-tabbing", Vmac_frame_tabbing,
    doc: /* Specify tabbing behavior of a frame that is becoming visible.
The symbol `automatic' (default) means it follows the system setting,
and `inverted' means the system setting is inverted.  Nil and t means
tabbing is disallowed and preferred, respectively.

This variable has no effect on OS X 10.11 and earlier.  */);
  Vmac_frame_tabbing = Qautomatic;
}
