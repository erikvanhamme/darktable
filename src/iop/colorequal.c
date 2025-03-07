/*
    This file is part of darktable,
    Copyright (C) 2022-2024 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

/* Midi mapping is supported, here is the reference for loupedeck+
midi:D7=iop/colorequal/page;hue
midi:D#7=iop/colorequal/page
midi:E7=iop/colorequal/page;brightness
None;midi:CC1=iop/colorequal/hue/red
None;midi:CC2=iop/colorequal/hue/orange
None;midi:CC3=iop/colorequal/hue/yellow
None;midi:CC4=iop/colorequal/hue/green
None;midi:CC5=iop/colorequal/hue/cyan
None;midi:CC6=iop/colorequal/hue/blue
None;midi:CC7=iop/colorequal/hue/lavender
None;midi:CC8=iop/colorequal/hue/magenta
None;midi:CC9=iop/colorequal/saturation/red
None;midi:CC10=iop/colorequal/saturation/orange
None;midi:CC11=iop/colorequal/saturation/yellow
None;midi:CC12=iop/colorequal/saturation/green
None;midi:CC13=iop/colorequal/saturation/cyan
None;midi:CC14=iop/colorequal/saturation/blue
None;midi:CC15=iop/colorequal/saturation/lavender
None;midi:CC16=iop/colorequal/saturation/magenta
None;midi:CC17=iop/colorequal/brightness/red
None;midi:CC18=iop/colorequal/brightness/orange
None;midi:CC19=iop/colorequal/brightness/yellow
None;midi:CC20=iop/colorequal/brightness/green
None;midi:CC21=iop/colorequal/brightness/cyan
None;midi:CC22=iop/colorequal/brightness/blue
None;midi:CC23=iop/colorequal/brightness/lavender
None;midi:CC24=iop/colorequal/brightness/magenta
*/

#include "common/extra_optimizations.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bauhaus/bauhaus.h"
#include "common/chromatic_adaptation.h"
#include "common/darktable_ucs_22_helpers.h"
#include "common/darktable.h"
#include "common/eigf.h"
#include "common/interpolation.h"
#include "common/opencl.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/expander.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"
#include "iop/choleski.h"
#include "common/colorspaces_inline_conversions.h"

#ifdef _OPENMP
#include <omp.h>
#endif

#define NODES 8

#define SLIDER_BRIGHTNESS 0.65f // 65 %

DT_MODULE_INTROSPECTION(2, dt_iop_colorequal_params_t)

typedef struct dt_iop_colorequal_params_t
{
  float reserved1;
  float smoothing_hue;           // $MIN: 0.05 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "hue curve"
  float reserved2;

  float white_level;        // $MIN: -2.0 $MAX: 16.0 $DEFAULT: 1.0 $DESCRIPTION: "white level"
  float chroma_size;        // $MIN: 1.0 $MAX: 10.0 $DEFAULT: 1.5 $DESCRIPTION: "analysis radius"
  float param_size;         // $MIN: 1.0 $MAX: 128. $DEFAULT: 1.0 $DESCRIPTION: "effect radius"
  gboolean use_filter;      // $DEFAULT: TRUE $DESCRIPTION: "use guided filter"

  // Note: what follows is tedious because each param needs to be declared separately.
  // A more efficient way would be to use 3 arrays of 8 elements,
  // but then GUI sliders would need to be wired manually to the correct array index.
  // So we do it the tedious way here, and let the introspection magic connect sliders to params automatically,
  // then we pack the params in arrays in commit_params().

  float sat_red;         // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "red"
  float sat_orange;      // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "orange"
  float sat_yellow;      // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "yellow"
  float sat_green;       // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "green"
  float sat_cyan;        // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "cyan"
  float sat_blue;        // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "blue"
  float sat_lavender;    // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "lavender"
  float sat_magenta;     // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "magenta"

  float hue_red;         // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "red"
  float hue_orange;      // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "orange"
  float hue_yellow;      // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "yellow"
  float hue_green;       // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "green"
  float hue_cyan;        // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "cyan"
  float hue_blue;        // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "blue"
  float hue_lavender;    // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "lavender"
  float hue_magenta;     // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "magenta"

  float bright_red;      // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "red"
  float bright_orange;   // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "orange"
  float bright_yellow;   // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "yellow"
  float bright_green;    // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "green"
  float bright_cyan;     // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "cyan"
  float bright_blue;     // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "blue"
  float bright_lavender; // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "lavender"
  float bright_magenta;  // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "magenta"

  float hue_shift;       // $MIN: -23. $MAX: 23. $DEFAULT: 0.0 $DESCRIPTION: "node placement"
} dt_iop_colorequal_params_t;

typedef enum dt_iop_colorequal_channel_t
{
  HUE = 0,
  SATURATION = 1,
  BRIGHTNESS = 2,
  NUM_CHANNELS = 3,
} dt_iop_colorequal_channel_t;

typedef struct dt_iop_colorequal_data_t
{
  float *LUT_saturation;
  float *LUT_hue;
  float *LUT_brightness;
  float *gamut_LUT;
  gboolean lut_inited;
  float white_level;
  float chroma_size;
  float chroma_feathering;
  float param_size;
  float param_feathering;
  gboolean use_filter;
  dt_iop_order_iccprofile_info_t *work_profile;
  float hue_shift;
} dt_iop_colorequal_data_t;

const char *name()
{
  return _("color equalizer");
}

const char *aliases()
{
  return _("color zones");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description
    (self, _("change saturation, hue and brightness depending on local hue"),
     _("corrective and creative"),
     _("linear, RGB, scene-referred"),
     _("quasi-linear, RGB"),
     _("quasi-linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_COLOR;
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

typedef struct dt_iop_colorequal_gui_data_t
{
  GtkWidget *white_level;
  GtkWidget *sat_red, *sat_orange, *sat_yellow, *sat_green;
  GtkWidget *sat_cyan, *sat_blue, *sat_lavender, *sat_magenta;
  GtkWidget *hue_red, *hue_orange, *hue_yellow, *hue_green;
  GtkWidget *hue_cyan, *hue_blue, *hue_lavender, *hue_magenta;
  GtkWidget *bright_red, *bright_orange, *bright_yellow, *bright_green;
  GtkWidget *bright_cyan, *bright_blue, *bright_lavender, *bright_magenta;

  GtkWidget *smoothing_hue;
  GtkWidget *chroma_size, *param_size, *use_filter;
  GtkWidget *hue_shift;

  // Array-like re-indexing of the above for efficient uniform
  // handling in loops Populate the array in gui_init()
  GtkWidget *slider_group[3];
  GtkWidget *sat_sliders[NODES];
  GtkWidget *hue_sliders[NODES];
  GtkWidget *bright_sliders[NODES];
  int page_num;
  GtkWidget *opts_box;

  GtkNotebook *notebook;
  GtkDrawingArea *area;
  dt_gui_collapsible_section_t cs;
  float *LUT;
  dt_iop_colorequal_channel_t channel;

  dt_iop_order_iccprofile_info_t *work_profile;
  dt_iop_order_iccprofile_info_t *white_adapted_profile;

  unsigned char *b_data[NUM_CHANNELS];
  cairo_surface_t *b_surface[NUM_CHANNELS];

  float max_saturation;
  gboolean gradients_cached;

  float *gamut_LUT;

  int mask_mode;
  gboolean dragging;
  gboolean on_node;
  int selected;
  float points[NODES+1][2];

  GtkWidget *box[3];
} dt_iop_colorequal_gui_data_t;

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  if(old_version == 1)
  {
    const dt_iop_colorequal_params_t *o =
      (dt_iop_colorequal_params_t *)old_params;
    dt_iop_colorequal_params_t *n =
      (dt_iop_colorequal_params_t *)malloc(sizeof(dt_iop_colorequal_params_t));

    memcpy(n, o, sizeof(dt_iop_colorequal_params_t) - sizeof(float));
    n->hue_shift = 0.0f;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_colorequal_params_t);
    *new_version = 2;
    return 0;
  }

  return 1;
}

void _mean_gaussian(float *const buf,
                    const size_t width,
                    const size_t height,
                    const uint32_t ch,
                    const float sigma)
{
  const float range = 1.0e9;
  const dt_aligned_pixel_t max = {range, range, range, range};
  const dt_aligned_pixel_t min = {-range, -range, -range, -range};
  dt_gaussian_t *g = dt_gaussian_init(width, height, ch, max, min, sigma, 0);
  if(!g) return;
  if(ch == 4)
    dt_gaussian_blur_4c(g, buf, buf);
  else
    dt_gaussian_blur(g, buf, buf);
  dt_gaussian_free(g);
}

static inline float _get_scaling(const float sigma)
{
  return MAX(1.0f, MIN(4.0f, floorf(sigma - 1.5f)));
}

static inline float _fast_sqrtf(const float a)
{
  return (a / (0.5f - a * 0.5f + a));
}

// sRGB primary red records at 20° of hue in darktable UCS 22, so we offset the whole hue range
// such that red is the origin hues in the GUI. This is consistent with HSV/HSL color wheels UI.
#define ANGLE_SHIFT +20.f
static inline float _deg_to_rad(const float angle)
{
  return (angle + ANGLE_SHIFT) * M_PI_F / 180.f;
}

void _prefilter_chromaticity(float *const restrict UV,
                             float *const restrict weights,
                             const dt_iop_roi_t *const roi,
                             const float csigma,
                             const float epsilon)
{
  // We guide the 3-channels corrections with the 2-channels
  // chromaticity coordinates UV aka we express corrections = a * UV +
  // b where a is a 2×2 matrix and b a constant Therefore the guided
  // filter computation is a bit more complicated than the typical
  // 1-channel case.  We use by-the-book 3-channels fast guided filter
  // as in http://kaiminghe.com/eccv10/ but obviously reduced to 2.
  // We know that it tends to oversmooth the input where its intensity
  // is close to 0, but this is actually desirable here since
  // chromaticity -> 0 means neutral greys and we want to discard them
  // as much as possible from any color equalization.

  const float sigma = csigma * roi->scale;
  const size_t width = roi->width;
  const size_t height = roi->height;
  // possibly downsample for speed-up
  const size_t pixels = width * height;
  const float scaling = _get_scaling(sigma);
  const float gsigma = MAX(0.3f, 0.5f * sigma / scaling);
  const size_t ds_height = height / scaling;
  const size_t ds_width = width / scaling;
  const size_t ds_pixels = ds_width * ds_height;
  const gboolean resized = width != ds_width || height != ds_height;

  float *ds_UV = UV;
  if(resized)
  {
    ds_UV = dt_alloc_align_float(ds_pixels * 2);
    interpolate_bilinear(UV, width, height, ds_UV, ds_width, ds_height, 2);
  }

  // Init the symmetric covariance matrix of the guide (4 elements by pixel) :
  // covar = [[ covar(U, U), covar(U, V)],
  //          [ covar(V, U), covar(V, V)]]
  // with covar(x, y) = avg(x * y) - avg(x) * avg(y), corr(x, y) = x * y
  // so here, we init it with x * y, compute all the avg() at the next step
  // and subtract avg(x) * avg(y) later
  float *const restrict covariance = dt_alloc_align_float(ds_pixels * 4);

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(ds_pixels, ds_UV, covariance)  \
  schedule(simd:static) aligned(ds_UV, covariance: 64)
#endif
  for(size_t k = 0; k < ds_pixels; k++)
  {
    // corr(U, U)
    covariance[4 * k + 0] = ds_UV[2 * k] * ds_UV[2 * k];
    // corr(U, V)
    covariance[4 * k + 1] = covariance[4 * k + 2] = ds_UV[2 * k] * ds_UV[2 * k + 1];
    // corr(V, V)
    covariance[4 * k + 3] = ds_UV[2 * k + 1] * ds_UV[2 * k + 1];
  }

  // Compute the local averages of everything over the window size We
  // use a gaussian blur as a weighted local average because it's a
  // radial function so it will not favour vertical and horizontal
  // edges over diagonal ones as the by-the-book box blur (unweighted
  // local average) would.

  // We use unbounded signals, so don't care for the internal value clipping
  _mean_gaussian(ds_UV, ds_width, ds_height, 2, gsigma);
  _mean_gaussian(covariance, ds_width, ds_height, 4, gsigma);

  // Finish the UV covariance matrix computation by subtracting avg(x) * avg(y)
  // to avg(x * y) already computed
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(ds_pixels, ds_UV, covariance)  \
  schedule(simd:static) aligned(ds_UV, covariance: 64)
#endif
  for(size_t k = 0; k < ds_pixels; k++)
  {
    // covar(U, U) = var(U)
    covariance[4 * k + 0] -= ds_UV[2 * k] * ds_UV[2 * k];
    // covar(U, V)
    covariance[4 * k + 1] -= ds_UV[2 * k] * ds_UV[2 * k + 1];
    covariance[4 * k + 2] -= ds_UV[2 * k] * ds_UV[2 * k + 1];
    // covar(V, V) = var(V)
    covariance[4 * k + 3] -= ds_UV[2 * k + 1] * ds_UV[2 * k + 1];
  }

  // Compute a and b the params of the guided filters
  float *const restrict a = dt_alloc_align_float(4 * ds_pixels);
  float *const restrict b = dt_alloc_align_float(2 * ds_pixels);

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(ds_pixels, ds_UV, covariance, a, b, epsilon)  \
  schedule(simd:static) aligned(ds_UV, covariance, a, b: 64)
#endif
  for(size_t k = 0; k < ds_pixels; k++)
  {
    // Extract the 2×2 covariance matrix sigma = cov(U, V) at current pixel
    dt_aligned_pixel_t Sigma = { covariance[4 * k + 0], covariance[4 * k + 1],
                                 covariance[4 * k + 2], covariance[4 * k + 3] };

    // Add the variance threshold : sigma' = sigma + epsilon * Identity
    Sigma[0] += epsilon;
    Sigma[3] += epsilon;

    // Invert the 2×2 sigma matrix algebraically
    // see https://www.mathcentre.ac.uk/resources/uploaded/sigma-matrices7-2009-1.pdf
    const float det = Sigma[0] * Sigma[3] - Sigma[1] * Sigma[2];
    dt_aligned_pixel_t sigma_inv = { Sigma[3] / det, -Sigma[1] / det,
                                    -Sigma[2] / det,  Sigma[0] / det };

    // a(chan) = dot_product(cov(chan, uv), sigma_inv)
    if(fabsf(det) > 4.f * FLT_EPSILON)
    {
      // find a_1, a_2 s.t. U' = a_1 * U + a_2 * V
      a[4 * k + 0] = (covariance[4 * k + 0] * sigma_inv[0]
                    + covariance[4 * k + 1] * sigma_inv[1]);
      a[4 * k + 1] = (covariance[4 * k + 0] * sigma_inv[2]
                    + covariance[4 * k + 1] * sigma_inv[3]);

      // find a_3, a_4 s.t. V' = a_3 * U + a_4 V
      a[4 * k + 2] = (covariance[4 * k + 2] * sigma_inv[0]
                    + covariance[4 * k + 3] * sigma_inv[1]);
      a[4 * k + 3] = (covariance[4 * k + 2] * sigma_inv[2]
                    + covariance[4 * k + 3] * sigma_inv[3]);
    }
    else
    {
      // determinant too close to 0: singular matrix
      a[4 * k + 0] = a[4 * k + 1] = a[4 * k + 2] = a[4 * k + 3] = 0.f;
    }

    b[2 * k + 0] = ds_UV[2 * k + 0]
      - a[4 * k + 0] * ds_UV[2 * k + 0]
      - a[4 * k + 1] * ds_UV[2 * k + 1];
    b[2 * k + 1] = ds_UV[2 * k + 1]
      - a[4 * k + 2] * ds_UV[2 * k + 0]
      - a[4 * k + 3] * ds_UV[2 * k + 1];
  }

  dt_free_align(covariance);
  if(ds_UV != UV) dt_free_align(ds_UV);

  // Compute the averages of a and b for each filter
  _mean_gaussian(a, ds_width, ds_height, 4, gsigma);
  _mean_gaussian(b, ds_width, ds_height, 2, gsigma);

  // Upsample a and b to real-size image
  float *a_full = a;
  float *b_full = b;
  if(resized)
  {
    a_full = dt_alloc_align_float(pixels * 4);
    b_full = dt_alloc_align_float(pixels * 2);
    interpolate_bilinear(a, ds_width, ds_height, a_full, width, height, 4);
    interpolate_bilinear(b, ds_width, ds_height, b_full, width, height, 2);
    dt_free_align(a);
    dt_free_align(b);
  }

  // Apply the guided filter
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(pixels, a_full, b_full, UV, weights)  \
  schedule(simd:static) aligned(a_full, b_full, weights, UV: 64)
#endif
  for(size_t k = 0; k < pixels; k++)
  {
    // For each correction factor, we re-express it as a[0] * U + a[1] * V + b
    const float uv[2] = { UV[2 * k + 0], UV[2 * k + 1] };
    const float cv[2] = { a_full[4 * k + 0] * uv[0] + a_full[4 * k + 1] * uv[1] + b_full[2 * k + 0],
                          a_full[4 * k + 2] * uv[0] + a_full[4 * k + 3] * uv[1] + b_full[2 * k + 1] };

    // we avoid chroma blurring into achromatic areas by interpolating
    // input UV vs corrected UV
    UV[2 * k + 0] = interpolatef(weights[k], cv[0], uv[0]);
    UV[2 * k + 1] = interpolatef(weights[k], cv[1], uv[1]);
  }

  dt_free_align(a_full);
  dt_free_align(b_full);
}

void _guide_with_chromaticity(float *const restrict UV,
                              float *const restrict corrections,
                              float *const restrict weights,
                              float *const restrict b_corrections,
                              const dt_iop_roi_t *const roi,
                              const float csigma,
                              const float epsilon)
{
  // We guide the 3-channels corrections with the 2-channels
  // chromaticity coordinates UV aka we express corrections = a * UV +
  // b where a is a 2×2 matrix and b a constant Therefore the guided
  // filter computation is a bit more complicated than the typical
  // 1-channel case.  We use by-the-book 3-channels fast guided filter
  // as in http://kaiminghe.com/eccv10/ but obviously reduced to 2.
  // We know that it tends to oversmooth the input where its intensity
  // is close to 0, but this is actually desirable here since
  // chromaticity -> 0 means neutral greys and we want to discard them
  // as much as possible from any color equalization.

  // Downsample for speed-up
  const float sigma = csigma * roi->scale;
  const size_t width = roi->width;
  const size_t height = roi->height;
  // Downsample for speed-up
  const size_t pixels = width * height;
  const float scaling = _get_scaling(sigma);
  const float gsigma = MAX(0.2f, 0.5f * sigma / scaling);
  const size_t ds_height = height / scaling;
  const size_t ds_width = width / scaling;
  const size_t ds_pixels = ds_width * ds_height;
  const gboolean resized = width != ds_width || height != ds_height;

  float *ds_UV = UV;
  float *ds_corrections = corrections;
  float *ds_b_corrections = b_corrections;
  if(resized)
  {
    ds_UV = dt_alloc_align_float(ds_pixels * 2);
    interpolate_bilinear(UV, width, height, ds_UV, ds_width, ds_height, 2);
    ds_corrections = dt_alloc_align_float(ds_pixels * 2);
    interpolate_bilinear(corrections, width, height, ds_corrections, ds_width, ds_height, 2);
    ds_b_corrections = dt_alloc_align_float(ds_pixels);
    interpolate_bilinear(b_corrections, width, height, ds_b_corrections, ds_width, ds_height, 1);
  }

  // Init the symmetric covariance matrix of the guide (4 elements by pixel) :
  // covar = [[ covar(U, U), covar(U, V)],
  //          [ covar(V, U), covar(V, V)]]
  // with covar(x, y) = avg(x * y) - avg(x) * avg(y), corr(x, y) = x * y
  // so here, we init it with x * y, compute all the avg() at the next step
  // and subtract avg(x) * avg(y) later
  float *const restrict covariance = dt_alloc_align_float(ds_pixels * 4);

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(ds_pixels, ds_UV, covariance)  \
  schedule(simd:static) aligned(ds_UV, covariance: 64)
#endif
  for(size_t k = 0; k < ds_pixels; k++)
  {
    // corr(U, U)
    covariance[4 * k + 0] = ds_UV[2 * k + 0] * ds_UV[2 * k + 0];
    // corr(U, V)
    covariance[4 * k + 1] = covariance[4 * k + 2] = ds_UV[2 * k] * ds_UV[2 * k + 1];
    // corr(V, V)
    covariance[4 * k + 3] = ds_UV[2 * k + 1] * ds_UV[2 * k + 1];
  }

  // Get the correlations between corrections and UV
  float *const restrict correlations = dt_alloc_align_float(ds_pixels * 4);

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(ds_pixels, ds_UV, ds_corrections, ds_b_corrections, correlations)  \
  schedule(simd:static) aligned(ds_UV, ds_corrections, ds_b_corrections, correlations: 64)
#endif
  for(size_t k = 0; k < ds_pixels; k++)
  {
    /* Dont filter hue
    // corr(hue, U)
    correlations[6 * k + 0] = ds_UV[2 * k + 0] * ds_corrections[4 * k + 0];
    // corr(hue, V)
    correlations[6 * k + 1] = ds_UV[2 * k + 1] * ds_corrections[4 * k + 0];
    */

    // corr(sat, U)
    correlations[4 * k + 0] = ds_UV[2 * k + 0] * ds_corrections[2 * k + 1];
    // corr(sat, V)
    correlations[4 * k + 1] = ds_UV[2 * k + 1] * ds_corrections[2 * k + 1];

    // corr(bright, U)
    correlations[4 * k + 2] = ds_UV[2 * k + 0] * ds_b_corrections[k];
    // corr(bright, V)
    correlations[4 * k + 3] = ds_UV[2 * k + 1] * ds_b_corrections[k];
  }

  // Compute the local averages of everything over the window size We
  // use a gaussian blur as a weighted local average because it's a
  // radial function so it will not favour vertical and horizontal
  // edges over diagonal ones as the by-the-book box blur (unweighted
  // local average) would.
  // We use unbounded signals, so don't care for the internal value clipping
  _mean_gaussian(ds_UV, ds_width, ds_height, 2, gsigma);
  _mean_gaussian(covariance, ds_width, ds_height, 4, gsigma);
  _mean_gaussian(ds_corrections, ds_width, ds_height, 2, gsigma);
  _mean_gaussian(ds_b_corrections, ds_width, ds_height, 1, 0.2f * gsigma);
  _mean_gaussian(correlations, ds_width, ds_height, 4, gsigma);

  // Finish the UV covariance matrix computation by subtracting avg(x) * avg(y)
  // to avg(x * y) already computed
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(ds_pixels, ds_UV, covariance)  \
  schedule(simd:static) aligned(ds_UV, covariance: 64)
#endif
  for(size_t k = 0; k < ds_pixels; k++)
  {
    // covar(U, U) = var(U)
    covariance[4 * k + 0] -= ds_UV[2 * k + 0] * ds_UV[2 * k + 0];
    // covar(U, V)
    covariance[4 * k + 1] -= ds_UV[2 * k + 0] * ds_UV[2 * k + 1];
    covariance[4 * k + 2] -= ds_UV[2 * k + 0] * ds_UV[2 * k + 1];
    // covar(V, V) = var(V)
    covariance[4 * k + 3] -= ds_UV[2 * k + 1] * ds_UV[2 * k + 1];
  }

  // Finish the guide * guided correlation computation
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(ds_pixels, ds_UV, ds_corrections, ds_b_corrections, correlations)  \
  schedule(simd:static) aligned(ds_UV, ds_corrections, correlations: 64)
#endif
  for(size_t k = 0; k < ds_pixels; k++)
  {
    /* Don't filter hue
    correlations[6 * k + 0] -= ds_UV[2 * k + 0] * ds_corrections[4 * k + 0];
    correlations[6 * k + 1] -= ds_UV[2 * k + 1] * ds_corrections[4 * k + 0];
    */

    correlations[4 * k + 0] -= ds_UV[2 * k + 0] * ds_corrections[2 * k + 1];
    correlations[4 * k + 1] -= ds_UV[2 * k + 1] * ds_corrections[2 * k + 1];

    correlations[4 * k + 2] -= ds_UV[2 * k + 0] * ds_b_corrections[k];
    correlations[4 * k + 3] -= ds_UV[2 * k + 1] * ds_b_corrections[k];
  }

  // Compute a and b the params of the guided filters
  float *const restrict a = dt_alloc_align_float(4 * ds_pixels);
  float *const restrict b = dt_alloc_align_float(2 * ds_pixels);

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(ds_pixels, ds_UV, covariance, correlations, ds_corrections, ds_b_corrections, a, b, epsilon)  \
  schedule(simd:static) aligned(ds_UV, covariance, correlations, ds_corrections, ds_b_corrections, a, b: 64)
#endif
  for(size_t k = 0; k < ds_pixels; k++)
  {
    // Extract the 2×2 covariance matrix sigma = cov(U, V) at current pixel
    dt_aligned_pixel_t Sigma
        = { covariance[4 * k + 0],
            covariance[4 * k + 1],
            covariance[4 * k + 2],
            covariance[4 * k + 3] };

    // Add the covariance threshold : sigma' = sigma + epsilon * Identity
    Sigma[0] += epsilon;
    Sigma[3] += epsilon;

    // Invert the 2×2 sigma matrix algebraically
    // see https://www.mathcentre.ac.uk/resources/uploaded/sigma-matrices7-2009-1.pdf
    const float det = MAX((Sigma[0] * Sigma[3] - Sigma[1] * Sigma[2]), 1e-15f);
    dt_aligned_pixel_t sigma_inv
        = { Sigma[3] / det,
           -Sigma[1] / det,
           -Sigma[2] / det,
            Sigma[0] / det };
    // Note : epsilon prevents determinant == 0 so the invert exists all the time

    // a(chan) = dot_product(cov(chan, uv), sigma_inv)
    /* Don't filter hue
    a[6 * k + 0] = (correlations[6 * k + 0] * sigma_inv[0] + correlations[6 * k + 1] * sigma_inv[1]);
    a[6 * k + 1] = (correlations[6 * k + 0] * sigma_inv[2] + correlations[6 * k + 1] * sigma_inv[3]);
    */
    if(fabsf(det) > 4.f * FLT_EPSILON)
    {
      a[4 * k + 0] = (correlations[4 * k + 0] * sigma_inv[0]
                    + correlations[4 * k + 1] * sigma_inv[1]);
      a[4 * k + 1] = (correlations[4 * k + 0] * sigma_inv[2]
                    + correlations[4 * k + 1] * sigma_inv[3]);

      a[4 * k + 2] = (correlations[4 * k + 2] * sigma_inv[0]
                    + correlations[4 * k + 3] * sigma_inv[1]);
      a[4 * k + 3] = (correlations[4 * k + 2] * sigma_inv[2]
                    + correlations[4 * k + 3] * sigma_inv[3]);
    }
    else
    {
      a[4 * k + 0] = a[4 * k + 1] = a[4 * k + 2] = a[4 * k + 3] = 0.f;
    }
    // b = avg(chan) - dot_product(a_chan * avg(UV))
    b[2 * k + 0] = ds_corrections[2 * k + 1]
      - a[4 * k + 0] * ds_UV[2 * k + 0]
      - a[4 * k + 1] * ds_UV[2 * k + 1];
    b[2 * k + 1] = ds_b_corrections[k]
      - a[4 * k + 2] * ds_UV[2 * k + 0]
      - a[4 * k + 3] * ds_UV[2 * k + 1];
  }

  if(resized)
  {
    dt_free_align(ds_corrections);
    dt_free_align(ds_b_corrections);
    dt_free_align(ds_UV);
  }
  dt_free_align(correlations);
  dt_free_align(covariance);

  // Compute the averages of a and b for each filter and blur
  _mean_gaussian(a, ds_width, ds_height, 4, gsigma);
  _mean_gaussian(b, ds_width, ds_height, 2, gsigma);

  // Upsample a and b to real-size image
  float *a_full = a;
  float *b_full = b;
  if(resized)
  {
    a_full = dt_alloc_align_float(pixels * 4);
    b_full = dt_alloc_align_float(pixels * 2);
    interpolate_bilinear(a, ds_width, ds_height, a_full, width, height, 4);
    interpolate_bilinear(b, ds_width, ds_height, b_full, width, height, 2);
    dt_free_align(a);
    dt_free_align(b);
  }

  // Apply the guided filter
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(pixels, a_full, b_full, corrections, b_corrections, UV, weights)   \
  schedule(simd:static) aligned(a_full, b_full, corrections, weights, UV: 64)
#endif
  for(size_t k = 0; k < pixels; k++)
  {
    // For each correction factor, we re-express it as a[0] * U + a[1] * V + b
    const float uv[2] = { UV[2 * k + 0], UV[2 * k + 1] };
    const float cv[2] = { a_full[4 * k + 0] * uv[0] + a_full[4 * k + 1] * uv[1] + b_full[2 * k + 0],
                          a_full[4 * k + 2] * uv[0] + a_full[4 * k + 3] * uv[1] + b_full[2 * k + 1] };
    corrections[2 * k + 1] = interpolatef(weights[k], cv[0], 1.0f);
    b_corrections[k] = interpolatef(weights[k], cv[1], 0.0f);
  }

  dt_free_align(a_full);
  dt_free_align(b_full);
}

void process(struct dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const i,
             void *const o,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorequal_data_t *d = (dt_iop_colorequal_data_t *)piece->data;

  if(piece->colors != 4) return;

  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  const gboolean fullpipe = piece->pipe->type & DT_DEV_PIXELPIPE_FULL;
  const int mask_mode = g && fullpipe ? g->mask_mode : 0;

  const float *const restrict in = (float*)i;
  float *const restrict out = (float*)o;

  const size_t npixels = (size_t)roi_out->width * roi_out->height;

  // STEP 0: prepare the RGB <-> XYZ D65 matrices
  // see colorbalancergb.c process() for the details, it's exactly the same
  const struct dt_iop_order_iccprofile_info_t *const work_profile =
    dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if(work_profile == NULL) return; // no point

  dt_colormatrix_t input_matrix;
  dt_colormatrix_t output_matrix;
  dt_colormatrix_mul(input_matrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in);
  dt_colormatrix_mul(output_matrix, work_profile->matrix_out, XYZ_D65_to_D50_CAT16);

  float *const restrict UV = dt_alloc_align_float(npixels * 2);
  float *const restrict corrections = dt_alloc_align_float(npixels * 2);
  float *const restrict b_corrections = dt_alloc_align_float(npixels);
  float *const restrict L = dt_alloc_align_float(npixels);
  float *const restrict weights = dt_alloc_align_float(npixels);

  const float white = Y_to_dt_UCS_L_star(d->white_level);

  // STEP 1: convert image from RGB to darktable UCS LUV and calc weights
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(npixels, in, UV, L, weights, input_matrix, white) \
  schedule(simd:static) aligned(in, UV, L, weights, input_matrix : 64)
#endif
  for(size_t k = 0; k < npixels; k++)
  {
    const float *const restrict pix_in = __builtin_assume_aligned(in + k * 4, 16);
    float *const restrict uv = UV + k * 2;

    // Convert to XYZ D65
    dt_aligned_pixel_t XYZ_D65 = { 0.0f, 0.0f, 0.0f, 0.0f };
    dot_product(pix_in, input_matrix, XYZ_D65);
    // Convert to dt UCS 22 UV and store UV
    dt_aligned_pixel_t xyY = { 0.0f, 0.0f, 0.0f, 0.0f };
    dt_D65_XYZ_to_xyY(XYZ_D65, xyY);

    const float X = _fast_sqrtf(XYZ_D65[0]);
    const float Y = _fast_sqrtf(XYZ_D65[1]);
    const float Z = _fast_sqrtf(XYZ_D65[2]);

    const float dmin = MIN(X, MIN(Y, Z));
    const float dmax = MAX(X, MAX(Y, Z));
    const float delta = dmax - dmin;
    const float val = (fabsf(dmax) > 1e-6f && fabsf(delta) > 1e-6f) ? delta / dmax : 0.0f;

    // We want to avoid any change of hue, saturation or brightness in achromatic
    // parts of the image. We make sure we have expose independent saturation as the
    // weighing parameter and use a pretty sharp logistic transition on it.
    const float coef = dt_fast_expf(-(20.0f * (2.0f * val - 0.4f)));

    weights[k] = fmaxf(1.0f / (1.0f + coef), 0.0f);

    xyY_to_dt_UCS_UV(xyY, uv);
    L[k] = Y_to_dt_UCS_L_star(xyY[2]);
  }

  // We blur the weights slightly depending on roi_scale
  _mean_gaussian(weights, roi_out->width, roi_out->height, 1, roi_out->scale);

  // STEP 2 : smoothen UV to avoid discontinuities in hue
  if(d->use_filter)
    _prefilter_chromaticity(UV, weights, roi_out, d->chroma_size, d->chroma_feathering);

  // STEP 3 : carry-on with conversion from LUV to HSB

  float B_norm = 0.01f;
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  reduction(max: B_norm) \
  dt_omp_firstprivate(npixels, in, out, UV, L, corrections, b_corrections, d, white)  \
  schedule(simd:static) aligned(in, out, UV, L, corrections, b_corrections : 64)
#endif
  for(size_t k = 0; k < npixels; k++)
  {
    const float *const restrict pix_in = __builtin_assume_aligned(in + k * 4, 16);
    float *const restrict pix_out = __builtin_assume_aligned(out + k * 4, 16);
    float *const restrict corrections_out = corrections + k * 2;

    float *const restrict uv = UV + k * 2;

    // Finish the conversion to dt UCS JCH then HSB
    dt_aligned_pixel_t JCH = { 0.0f, 0.0f, 0.0f, 0.0f };
    dt_UCS_LUV_to_JCH(L[k], white, uv, JCH);
    dt_UCS_JCH_to_HSB(JCH, pix_out);
    B_norm = fmaxf(B_norm, pix_out[2]);
    // Get the boosts - if chroma = 0, we have a neutral grey so set everything to 0

    if(JCH[1] > 0.f)
    {
      const float hue = pix_out[0];
      const float sat = pix_out[1];
      corrections_out[0] = lookup_gamut(d->LUT_hue, hue);
      corrections_out[1] = lookup_gamut(d->LUT_saturation, hue);
      b_corrections[k] = sat * (lookup_gamut(d->LUT_brightness, hue) - 1.0f);
    }
    else
    {
      corrections_out[0] = 0.0f;
      corrections_out[1] = 1.0f;
      b_corrections[k] = 0.0f;
    }

    // Copy alpha
    pix_out[3] = pix_in[3];
  }

  // STEP 2: apply a guided filter on the corrections, guided with UV
  // chromaticity, to ensure spatially-contiguous corrections even
  // though the hue is not perfectly constant this will help avoiding
  // chroma noise.
  if(d->use_filter)
    _guide_with_chromaticity(UV, corrections, weights, b_corrections, roi_out, d->param_size, d->param_feathering);

  if(mask_mode == 0)
  {
    // STEP 3: apply the corrections and convert back to RGB
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(npixels, out, corrections, b_corrections, output_matrix, white, d)  \
  schedule(simd:static) aligned(out, b_corrections, output_matrix: 64)
#endif
    for(size_t k = 0; k < npixels; k++)
    {
      const float *const restrict corrections_out = corrections + k * 2;
      float *const restrict pix_out = __builtin_assume_aligned(out + k * 4, 16);

      // Apply the corrections
      pix_out[0] += corrections_out[0]; // WARNING: hue is an offset
      // pix_out[1] (saturation) and pix_out[2] (brightness) are gains
      pix_out[1] = MAX(0.0f, pix_out[1] * (1.0f + 1.5f * (corrections_out[1] - 1.0f)));
      pix_out[2] = MAX(0.0f, pix_out[2] * (1.0f + 6.0f * b_corrections[k]));

      // Sanitize gamut
      gamut_map_HSB(pix_out, d->gamut_LUT, white);

      // Convert back to XYZ D65
      dt_aligned_pixel_t XYZ_D65 = { 0.f };
      dt_UCS_HSB_to_XYZ(pix_out, white, XYZ_D65);

      // And back to pipe RGB through XYZ D50
      dot_product(XYZ_D65, output_matrix, pix_out);
    }
  }
  else
  {
    const int mode = mask_mode - 1;
    B_norm = 1.5f / B_norm;
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(npixels, out, b_corrections, corrections, weights, mode, B_norm)  \
  schedule(simd:static) aligned(out, corrections, b_corrections, weights: 64)
#endif
    for(size_t k = 0; k < npixels; k++)
    {
      float *const restrict pix_out = __builtin_assume_aligned(out + k * 4, 16);
      const float *const restrict corrections_out = corrections + k * 2;

      const float val = pix_out[2] * B_norm;
      float corr = 0.0f;
      switch(mode)
      {
        case BRIGHTNESS:
          corr = 6.0f * b_corrections[k];
          break;
        case SATURATION:
          corr = corrections_out[1] - 1.0f;
          break;
        case HUE:
          corr = 0.2f * corrections_out[0];
          break;
        default:
          corr = 0.5f * (weights[k] - 0.5f);
      }

      const gboolean neg = corr < 0.0f;
      corr = fabsf(corr);
      pix_out[0] = MAX(0.0f, neg ? val - corr : val);
      pix_out[1] = MAX(0.0f, neg ? val - corr : val - corr);
      pix_out[2] = MAX(0.0f, neg ? val        : val - corr);
    }
  }

  dt_free_align(corrections);
  dt_free_align(b_corrections);
  dt_free_align(weights);
  dt_free_align(UV);
  dt_free_align(L);
}

static inline float _get_hue_node(const int k, const float hue_shift)
{
  // Get the angular coordinate of the k-th hue node, including hue shift
  return _deg_to_rad(((float)k) * 360.f / ((float)NODES) + hue_shift);
}

static inline float _cosine_coeffs(const float l,
                                   const float c)
{
  return expf(-l * l / c);
}

static inline void _periodic_RBF_interpolate(float nodes[NODES],
                                             const float smoothing,
                                             float *const LUT,
                                             const float hue_shift,
                                             const gboolean clip)
{
  // Perform a periodic interpolation across hue angles using radial-basis functions
  // see https://eng.aurelienpierre.com/2022/06/interpolating-hue-angles/#Refined-approach
  // for the theory and Python demo

  // Number of terms for the cosine series
  const int m = (int)ceilf(3.f * sqrtf(smoothing));

  float DT_ALIGNED_ARRAY A[NODES][NODES] = { { 0.f } };

  // Build the A matrix with nodes
  for(int i = 0; i < NODES; i++)
    for(int j = 0; j < NODES; j++)
    {
      for(int l = 0; l < m; l++)
      {
        A[i][j] += _cosine_coeffs(l, smoothing) * \
          cosf(((float)l) * fabsf(_get_hue_node(i, hue_shift) - _get_hue_node(j, hue_shift)));
      }
      A[i][j] = expf(A[i][j]);
    }

  // Solve A * x = y for lambdas
  pseudo_solve((float *)A, nodes, NODES, NODES, 0);

  // Interpolate data for all x : generate the LUT
  // WARNING: the LUT spans from [-pi; pi[ for consistency with the output of atan2f()
  for(int i = 0; i < LUT_ELEM; i++)
  {
    // i is directly the hue angle in degree since we sample the LUT
    // every degree.  We use un-offset angles here, since thue hue
    // offset is merely a GUI thing, only relevant for user-defined
    // nodes.
    const float hue = (float)i * M_PI_F / 180.f - M_PI_F;
    LUT[i] = 0.f;

    for(int k = 0; k < NODES; k++)
    {
      float result = 0;
      for(int l = 0; l < m; l++)
      {
        result += _cosine_coeffs(l, smoothing)
          * cosf(((float)l) * fabsf(hue - _get_hue_node(k, hue_shift)));
      }
      LUT[i] += nodes[k] * expf(result);
    }

    if(clip) LUT[i] = fmaxf(0.f, LUT[i]);
  }
}


void init_pipe(struct dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_aligned(sizeof(dt_iop_colorequal_data_t));
  dt_iop_colorequal_data_t *d = (dt_iop_colorequal_data_t *)piece->data;
  d->LUT_saturation = dt_alloc_align_float(LUT_ELEM);
  d->LUT_hue = dt_alloc_align_float(LUT_ELEM);
  d->LUT_brightness = dt_alloc_align_float(LUT_ELEM);
  d->gamut_LUT = dt_alloc_align_float(LUT_ELEM);
  d->lut_inited = FALSE;
  d->work_profile = NULL;
}


void cleanup_pipe(struct dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorequal_data_t *d = (dt_iop_colorequal_data_t *)piece->data;
  dt_free_align(d->LUT_saturation);
  dt_free_align(d->LUT_hue);
  dt_free_align(d->LUT_brightness);
  dt_free_align(d->gamut_LUT);
  dt_free_align(piece->data);
  piece->data = NULL;
}

static inline void _pack_saturation(struct dt_iop_colorequal_params_t *p,
                                    float array[NODES])
{
  array[0] = p->sat_red;
  array[1] = p->sat_orange;
  array[2] = p->sat_yellow;
  array[3] = p->sat_green;
  array[4] = p->sat_cyan;
  array[5] = p->sat_blue;
  array[6] = p->sat_lavender;
  array[7] = p->sat_magenta;
}

static inline void _pack_hue(struct dt_iop_colorequal_params_t *p,
                             float array[NODES])
{
  array[0] = p->hue_red;
  array[1] = p->hue_orange;
  array[2] = p->hue_yellow;
  array[3] = p->hue_green;
  array[4] = p->hue_cyan;
  array[5] = p->hue_blue;
  array[6] = p->hue_lavender;
  array[7] = p->hue_magenta;

  for(int i = 0; i < NODES; i++)
    array[i] = array[i] / 180.f * M_PI_F; // Convert to radians
}

static inline void _pack_brightness(struct dt_iop_colorequal_params_t *p,
                                    float array[NODES])
{
  array[0] = p->bright_red;
  array[1] = p->bright_orange;
  array[2] = p->bright_yellow;
  array[3] = p->bright_green;
  array[4] = p->bright_cyan;
  array[5] = p->bright_blue;
  array[6] = p->bright_lavender;
  array[7] = p->bright_magenta;
}


void commit_params(struct dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorequal_params_t *p = (dt_iop_colorequal_params_t *)p1;
  dt_iop_colorequal_data_t *d = (dt_iop_colorequal_data_t *)piece->data;

  d->white_level = exp2f(p->white_level);
  d->chroma_size = p->chroma_size;
  d->chroma_feathering = powf(10.f, -5.0f);
  d->param_size = p->param_size;
  d->param_feathering = powf(10.f, -6.0f);
  d->use_filter = p->use_filter;
  d->hue_shift = p->hue_shift;

  float DT_ALIGNED_ARRAY sat_values[NODES];
  float DT_ALIGNED_ARRAY hue_values[NODES];
  float DT_ALIGNED_ARRAY bright_values[NODES];

  // FIXME only calc LUTs if necessary
  _pack_saturation(p, sat_values);
  _periodic_RBF_interpolate(sat_values,
                            M_PI_F,
                            d->LUT_saturation, d->hue_shift, TRUE);

  _pack_hue(p, hue_values);
  _periodic_RBF_interpolate(hue_values,
                            1.f / p->smoothing_hue * M_PI_F,
                            d->LUT_hue, d->hue_shift, FALSE);

  _pack_brightness(p, bright_values);
  _periodic_RBF_interpolate(bright_values,
                            M_PI_F,
                            d->LUT_brightness, d->hue_shift, TRUE);

  // Check if the RGB working profile has changed in pipe
  // WARNING: this function is not triggered upon working profile change,
  // so the gamut boundaries are wrong until we change some param in this module
  struct dt_iop_order_iccprofile_info_t *const work_profile =
    dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if(work_profile == NULL)
    return;
  if(work_profile != d->work_profile)
  {
    d->lut_inited = FALSE;
    d->work_profile = work_profile;
  }

  // find the maximum chroma allowed by the current working gamut in
  // conjunction to hue this will be used to prevent users to mess up
  // their images by pushing chroma out of gamut
  if(!d->lut_inited)
  {
    dt_colormatrix_t input_matrix;
    dt_colormatrix_mul(input_matrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in);
    dt_UCS_22_build_gamut_LUT(input_matrix, d->gamut_LUT);
    d->lut_inited = TRUE;
  }
}


static inline void _build_dt_UCS_HSB_gradients
  (dt_aligned_pixel_t HSB,
   dt_aligned_pixel_t RGB,
   const struct dt_iop_order_iccprofile_info_t *work_profile,
   const float *const gamut_LUT)
{
  // Generate synthetic HSB gradients and convert to display RGB

  // First, gamut-map to ensure the requested HSB color is available in display gamut
  gamut_map_HSB(HSB, gamut_LUT, 1.f);

  // Then, convert to XYZ D65
  dt_aligned_pixel_t XYZ_D65 = { 1.0f, 1.0f, 1.0f, 1.0f };
  dt_UCS_HSB_to_XYZ(HSB, 1.f, XYZ_D65);

  if(work_profile)
  {
    dt_ioppr_xyz_to_rgb_matrix(XYZ_D65, RGB,
                               work_profile->matrix_out_transposed,
                               work_profile->lut_out,
                               work_profile->unbounded_coeffs_out,
                               work_profile->lutsize,
                               work_profile->nonlinearlut);
  }
  else
  {
    // Fall back to sRGB output and slow white point conversion
    dt_aligned_pixel_t XYZ_D50;
    XYZ_D65_to_D50(XYZ_D65, XYZ_D50);
    dt_XYZ_to_sRGB(XYZ_D50, RGB);
  }

  dt_vector_clip(RGB);
}

static inline void _draw_sliders_saturation_gradient
  (const float sat_min,
   const float sat_max,
   const float hue,
   const float brightness,
   GtkWidget *const slider,
   const struct dt_iop_order_iccprofile_info_t *work_profile,
   const float *const gamut_LUT)
{
  const float range = sat_max - sat_min;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float sat = sat_min + stop * range;
    dt_aligned_pixel_t RGB = { 1.0f, 1.0f, 1.0f, 1.0f };
    _build_dt_UCS_HSB_gradients((dt_aligned_pixel_t){ hue, sat, brightness, 0.f },
                                RGB, work_profile, gamut_LUT);
    dt_bauhaus_slider_set_stop(slider, stop, RGB[0], RGB[1], RGB[2]);
  }
}

static inline void _draw_sliders_hue_gradient
  (const float sat,
   const float hue,
   const float brightness,
   GtkWidget *const slider,
   const struct dt_iop_order_iccprofile_info_t *work_profile,
   const float *const gamut_LUT)
{
  const float hue_min = hue - M_PI_F;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float hue_temp = hue_min + stop * 2.f * M_PI_F;
    dt_aligned_pixel_t RGB = {  1.0f, 1.0f, 1.0f, 1.0f };
    _build_dt_UCS_HSB_gradients((dt_aligned_pixel_t){ hue_temp, sat, brightness, 0.f },
                                RGB, work_profile, gamut_LUT);
    dt_bauhaus_slider_set_stop(slider, stop, RGB[0], RGB[1], RGB[2]);
  }
}

static inline void _draw_sliders_brightness_gradient
  (const float sat,
   const float hue,
   GtkWidget *const slider,
   const struct dt_iop_order_iccprofile_info_t *work_profile,
   const float *const gamut_LUT)
{
  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1))
      * (1.f - 0.001f);
    dt_aligned_pixel_t RGB = {  1.0f, 1.0f, 1.0f, 1.0f };
    _build_dt_UCS_HSB_gradients((dt_aligned_pixel_t){ hue, sat, stop + 0.001f, 0.f },
                                RGB, work_profile, gamut_LUT);
    dt_bauhaus_slider_set_stop(slider, stop, RGB[0], RGB[1], RGB[2]);
  }
}

static inline void _init_sliders(dt_iop_module_t *self)
{
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  dt_iop_colorequal_params_t *p = (dt_iop_colorequal_params_t *)self->params;

  // Saturation sliders
  for(int k = 0; k < NODES; k++)
  {
    GtkWidget *const slider = g->sat_sliders[k];
    _draw_sliders_saturation_gradient(0.f, g->max_saturation, _get_hue_node(k, p->hue_shift),
                                      SLIDER_BRIGHTNESS, slider,
                                      g->white_adapted_profile,
                                      g->gamut_LUT);
    dt_bauhaus_slider_set_format(slider, "%");
    dt_bauhaus_slider_set_offset(slider, -100.0f);
    dt_bauhaus_slider_set_digits(slider, 2);
    gtk_widget_queue_draw(slider);
  }

  // Hue sliders
  for(int k = 0; k < NODES; k++)
  {
    GtkWidget *const slider = g->hue_sliders[k];
    _draw_sliders_hue_gradient(g->max_saturation, _get_hue_node(k, p->hue_shift),
                               SLIDER_BRIGHTNESS, slider,
                               g->white_adapted_profile,
                               g->gamut_LUT);
    dt_bauhaus_slider_set_format(slider, "°");
    dt_bauhaus_slider_set_digits(slider, 2);
    gtk_widget_queue_draw(slider);
  }

  // Brightness sliders
  for(int k = 0; k < NODES; k++)
  {
    GtkWidget *const slider = g->bright_sliders[k];
    _draw_sliders_brightness_gradient(g->max_saturation, _get_hue_node(k, p->hue_shift),
                                      slider,
                                      g->white_adapted_profile,
                                      g->gamut_LUT);
    dt_bauhaus_slider_set_format(slider, "%");
    dt_bauhaus_slider_set_offset(slider, -100.0f);
    dt_bauhaus_slider_set_digits(slider, 2);
    gtk_widget_queue_draw(slider);
  }
}

static void _init_graph_backgrounds(dt_iop_colorequal_gui_data_t *g,
                                    const float graph_width,
                                    const float graph_height,
                                    const float *const restrict gamut_LUT)
{
  const int gwidth = graph_width;
  const int gheight = graph_height;
  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, gwidth);
  const float max_saturation = g->max_saturation;

  for(int c = 0; c < NUM_CHANNELS; c++)
  {
    if(g->b_data[c])
      free(g->b_data[c]);
    g->b_data[c] = malloc(stride * gheight);

    if(g->b_surface[c])
      cairo_surface_destroy(g->b_surface[c]);
    g->b_surface[c] = cairo_image_surface_create_for_data(g->b_data[c], CAIRO_FORMAT_RGB24, gwidth, gheight, stride);
  }

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(gheight, gwidth, stride, g, gamut_LUT, max_saturation, graph_width, graph_height) \
  schedule(static) collapse(2)
#endif
  for(int i = 0; i < gheight; i++)
  {
    for(int j = 0; j < gwidth; j++)
    {
      const size_t idx = i * stride + j * 4;
      const float x = 360.0f * (float)(gwidth - j - 1) / (graph_width - 1.0f) - 90.0f;
      const float y = 1.0f - (float)i / (graph_height - 1.0f);
      const float hue = (x < -180.0f) ? _deg_to_rad(x +180.0f) : _deg_to_rad(x);
      const float hhue = hue - (y - 0.5f) * 2.f * M_PI_F;

      dt_aligned_pixel_t RGB;
      dt_aligned_pixel_t HSB[NUM_CHANNELS] = {{ hhue, max_saturation,     SLIDER_BRIGHTNESS,      1.0f },
                                              { hue,  max_saturation * y, SLIDER_BRIGHTNESS,      1.0f },
                                              { hue,  max_saturation,     SLIDER_BRIGHTNESS * y,  1.0f } };

      for(int k = 0; k < NUM_CHANNELS; k++)
      {
        _build_dt_UCS_HSB_gradients(HSB[k], RGB, g->white_adapted_profile, gamut_LUT);
        for_three_channels(c)
          g->b_data[k][idx + c] = roundf(RGB[c] * 255.f);
      }
    }
  }
  g->gradients_cached = TRUE;
}

void reload_defaults(dt_iop_module_t *self)
{
  // we might be called from presets update infrastructure => there is no image
  if(!self->dev || !dt_is_valid_imgid(self->dev->image_storage.id)) return;

  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  if(g)
  {
    // reset masking
    dt_bauhaus_widget_set_quad_active(g->param_size, FALSE);
    dt_bauhaus_widget_set_quad_active(g->chroma_size, FALSE);
    g->mask_mode = 0;
  }
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  if(!in)
  {
    const int mask_mode = g->mask_mode;
    dt_bauhaus_widget_set_quad_active(g->param_size, FALSE);
    dt_bauhaus_widget_set_quad_active(g->chroma_size, FALSE);
    g->mask_mode = 0;
    if(mask_mode) dt_dev_reprocess_center(self->dev);
  }
}

static gboolean _iop_colorequalizer_draw(GtkWidget *widget,
                                         cairo_t *crf,
                                         gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  dt_iop_colorequal_params_t *p = (dt_iop_colorequal_params_t *)self->params;

  // Cache the graph objects to avoid recomputing all the view at each redraw
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  GtkStyleContext *context = gtk_widget_get_style_context(widget);

  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                       allocation.width,
                                                       allocation.height);
  PangoFontDescription *desc =
    pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  cairo_t *cr = cairo_create(cst);
  PangoLayout *layout = pango_cairo_create_layout(cr);

  const gint font_size = pango_font_description_get_size(desc);
  pango_font_description_set_size(desc, 0.95 * font_size);
  pango_layout_set_font_description(layout, desc);
  pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);

  char text[256];

  // Get the text line height for spacing
  PangoRectangle ink;
  snprintf(text, sizeof(text), "X");
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  const float line_height = ink.height;

  const float inset = DT_PIXEL_APPLY_DPI(4);
  const float margin_top = inset;
  const float margin_bottom = line_height + 2.0 * inset;
  const float margin_left = 0.0;
  const float margin_right = 0.0;

  const float graph_width =
    allocation.width - margin_right - margin_left;   // align the right border on sliders
  const float graph_height =
    allocation.height - margin_bottom - margin_top; // give room to nodes

  gtk_render_background(context, cr, 0.0, 0.0, allocation.width, allocation.height);

  // draw x gradient as axis legend
  cairo_pattern_t *grad = cairo_pattern_create_linear(margin_left, 0.0, graph_width, 0.0);
  if(g->gamut_LUT)
  {
    for(int k = 0; k < LUT_ELEM; k++)
    {
      const float x = (float)k / (float)(LUT_ELEM);
      const float hue = _deg_to_rad((float)k);
      dt_aligned_pixel_t RGB = { 1.f };
      _build_dt_UCS_HSB_gradients((dt_aligned_pixel_t){ hue, g->max_saturation,
                                                        SLIDER_BRIGHTNESS, 1.0f },
        RGB, g->white_adapted_profile, g->gamut_LUT);
      cairo_pattern_add_color_stop_rgba(grad, x, RGB[0], RGB[1], RGB[2], 1.0);
    }
  }

  cairo_set_line_width(cr, 0.0);
  cairo_rectangle(cr, margin_left, graph_height + 2 * inset, graph_width, line_height);
  cairo_set_source(cr, grad);
  cairo_fill(cr);
  cairo_pattern_destroy(grad);

  // set the graph as the origin of the coordinates
  cairo_translate(cr, margin_left, margin_top);

  // possibly recalculate and draw background
  if(!g->gradients_cached)
    _init_graph_backgrounds(g, graph_width, graph_height, g->gamut_LUT);

  cairo_rectangle(cr, 0.0, 0.0, graph_width, graph_height);
  cairo_set_source_surface(cr, g->b_surface[g->channel], 0.0, 0.0);
  cairo_fill(cr);

  cairo_rectangle(cr, 0, 0, graph_width, graph_height);
  cairo_clip(cr);

  // draw grid
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(0.5));
  set_color(cr, darktable.bauhaus->graph_border);
  dt_draw_grid(cr, 8, 0, 0, graph_width, graph_height);

  // draw ground level
  set_color(cr, darktable.bauhaus->graph_fg);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0));
  dt_draw_line(cr, 0.0, 0.5 * graph_height, graph_width, 0.5 * graph_height);
  cairo_stroke(cr);

  GdkRGBA fg_color = darktable.bauhaus->graph_fg;
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.0));
  set_color(cr, fg_color);

  // Build the curve LUT and plotting params for the current channel
  g->LUT = dt_alloc_align_float(LUT_ELEM);
  float DT_ALIGNED_ARRAY values[NODES];
  float smoothing;
  float offset;
  float factor;
  gboolean clip;

  switch(g->channel)
  {
    case SATURATION:
    {
      _pack_saturation(p, values);
      smoothing = 1.0f;
      clip = TRUE;
      offset = 1.f;
      factor = 0.5f;
      break;
    }
    case HUE:
    {
      _pack_hue(p, values);
      smoothing = p->smoothing_hue;
      clip = FALSE;
      offset = 0.5f;
      factor = 1.f / (2.f * M_PI_F);
      break;
    }
    case BRIGHTNESS:
    default:
    {
      _pack_brightness(p, values);
      smoothing = 1.0f;
      clip = TRUE;
      offset = 1.0f;
      factor = 0.5f;
      break;
    }
  }

  _periodic_RBF_interpolate(values, 1.f / smoothing * M_PI_F, g->LUT, 0.0f, clip);

  const float dx = p->hue_shift / 360.0f;
  const int first = -dx * LUT_ELEM;
  for(int k = first; k < (LUT_ELEM + first); k++)
  {
    const float x = ((float)k / (float)(LUT_ELEM - 1) + dx) * graph_width;
    float hue = _deg_to_rad(k);
    hue = (hue < M_PI_F) ? hue : -2.f * M_PI_F + hue; // The LUT is defined in [-pi; pi[
    const float y = (offset - lookup_gamut(g->LUT, hue) * factor) * graph_height;

    if(k == first)
      cairo_move_to(cr, x, y);
    else
      cairo_line_to(cr, x, y);
  }
  cairo_stroke(cr);

  // draw nodes positions
  for(int k = 0; k < NODES + 1; k++)
  {
    float hue = _get_hue_node(k, 0.0f); // in radians
    const float xn = (k / ((float)NODES) + dx    ) * graph_width;
    hue = (hue < M_PI_F) ? hue : -2.f * M_PI_F + hue; // The LUT is defined in [-pi; pi[
    const float yn = (offset - lookup_gamut(g->LUT, hue) * factor) * graph_height;

    // fill bars
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(6));
    set_color(cr, darktable.bauhaus->color_fill);
    dt_draw_line(cr, xn, 0.5 * graph_height, xn, yn);
    cairo_stroke(cr);

    // bullets
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3));
    cairo_arc(cr, xn, yn, DT_PIXEL_APPLY_DPI(4), 0, 2. * M_PI);
    set_color(cr, darktable.bauhaus->graph_fg);
    cairo_stroke_preserve(cr);

    // record nodes positions for motion events
    g->points[k][0] = xn;
    g->points[k][1] = yn;

    if(g->on_node && g->selected == k % NODES)
      set_color(cr, darktable.bauhaus->graph_fg);
    else
      set_color(cr, darktable.bauhaus->graph_bg);

    cairo_fill(cr);
  }

  dt_free_align(g->LUT);
  cairo_restore(cr);

  // restore font size
  pango_font_description_set_size(desc, font_size);
  pango_layout_set_font_description(layout, desc);

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  g_object_unref(layout);
  pango_font_description_free(desc);
  return FALSE;
}

static void _pipe_RGB_to_Ych(dt_iop_module_t *self,
                             dt_dev_pixelpipe_t *pipe,
                             const dt_aligned_pixel_t RGB,
                             dt_aligned_pixel_t Ych)
{
  const struct dt_iop_order_iccprofile_info_t *const work_profile =
    dt_ioppr_get_pipe_current_profile_info(self, pipe);
  if(work_profile == NULL) return; // no point

  dt_aligned_pixel_t XYZ_D50 = { 0.0f, 0.0f, 0.0f, 0.0f };
  dt_aligned_pixel_t XYZ_D65 = { 0.0f, 0.0f, 0.0f, 0.0f };

  dt_ioppr_rgb_matrix_to_xyz(RGB, XYZ_D50, work_profile->matrix_in_transposed,
                             work_profile->lut_in,
                             work_profile->unbounded_coeffs_in,
                             work_profile->lutsize,
                             work_profile->nonlinearlut);
  XYZ_D50_to_D65(XYZ_D50, XYZ_D65);
  XYZ_to_Ych(XYZ_D65, Ych);

  if(Ych[2] < 0.f)
    Ych[2] = 2.f * M_PI_F + Ych[2];
}

void color_picker_apply(dt_iop_module_t *self,
                        GtkWidget *picker,
                        dt_dev_pixelpipe_t *pipe)
{
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  dt_iop_colorequal_params_t *p = (dt_iop_colorequal_params_t *)self->params;

  dt_aligned_pixel_t max_Ych = { 0.0f, 0.0f, 0.0f, 0.0f };
  _pipe_RGB_to_Ych(self, pipe, (const float *)self->picked_color_max, max_Ych);

  ++darktable.gui->reset;
  if(picker == g->white_level)
  {
    p->white_level = log2f(max_Ych[0]);
    dt_bauhaus_slider_set(g->white_level, p->white_level);
  }
  else
    dt_print(DT_DEBUG_PIPE, "[colorequal] unknown color picker\n");
  --darktable.gui->reset;

  gui_changed(self, picker, NULL);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _masking_callback_p(GtkWidget *quad, gpointer user_data)
{
  if(darktable.gui->reset) return;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  dt_bauhaus_widget_set_quad_active(g->chroma_size, FALSE);

  g->mask_mode = (dt_bauhaus_widget_get_quad_active(quad)) ? g->channel + 1 : 0;
  dt_dev_reprocess_center(self->dev);
}

static void _masking_callback_c(GtkWidget *quad, gpointer user_data)
{
  if(darktable.gui->reset) return;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  dt_bauhaus_widget_set_quad_active(g->param_size, FALSE);
  g->mask_mode = (dt_bauhaus_widget_get_quad_active(quad)) ? 4 : 0;
  dt_dev_reprocess_center(self->dev);
}

static void _channel_tabs_switch_callback(GtkNotebook *notebook,
                                          GtkWidget *page,
                                          guint page_num,
                                          dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;

  // The 4th tab is options, in which case we do nothing
  // For the first 3 tabs, update color channel and redraw the graph
  if(page_num < NUM_CHANNELS)
  {
    g->channel = (dt_iop_colorequal_channel_t)page_num;
  }

  g->page_num = page_num;

  const int old_mask_mode = g->mask_mode;
  const gboolean masking_p = dt_bauhaus_widget_get_quad_active(g->param_size);
  const gboolean masking_c = dt_bauhaus_widget_get_quad_active(g->chroma_size);
  gui_update(self);

  dt_bauhaus_widget_set_quad_active(g->param_size, masking_p);
  dt_bauhaus_widget_set_quad_active(g->chroma_size, masking_c);

  g->mask_mode = masking_p ? g->channel + 1 : (masking_c ? 4 : 0);
  if(g->mask_mode != old_mask_mode)
    dt_dev_reprocess_center(self->dev);
}

static GtkWidget *_get_selected(dt_iop_colorequal_gui_data_t *g)
{
  GtkWidget *w = NULL;

  switch(g->channel)
  {
    case(SATURATION):
      w = g->sat_sliders[g->selected];
      break;
    case(HUE):
      w = g->hue_sliders[g->selected];
      break;
    case(BRIGHTNESS):
    default:
      w = g->bright_sliders[g->selected];
      break;
  }

  gtk_widget_realize(w);
  return w;
}

static void _area_set_value(dt_iop_colorequal_gui_data_t *g,
                            const float graph_height,
                            const float pos)
{
  float factor = .0f;
  float max = .0f;

  GtkWidget *w = _get_selected(g);

  if(w)
  {
    switch(g->channel)
    {
       case(SATURATION):
         factor = 0.5f;
         max = 100.0f;
         break;
       case(HUE):
         factor = 1.f / (2.f * M_PI_F);
         max = (100.0f / 180.0f) * 100.0f;
         break;
       case(BRIGHTNESS):
       default:
         factor = 0.5f;
         max = 100.0f;
         break;
    }

    const float val = (0.5f - (pos / graph_height)) * max / factor;
    dt_bauhaus_slider_set_val(w, val);
  }
}

static void _area_set_pos(dt_iop_colorequal_gui_data_t *g,
                          const float pos)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(GTK_WIDGET(g->area), &allocation);
  const float graph_height = allocation.height;

  const float y = CLAMP(pos, 0.0f, graph_height);

  _area_set_value(g, graph_height, y);
}

static void _area_reset_nodes(dt_iop_colorequal_gui_data_t *g)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(GTK_WIDGET(g->area), &allocation);
  const float graph_height = allocation.height;
  const float y = graph_height / 2.0f;

  if(g->on_node)
  {
    _area_set_value(g, graph_height, y);
  }
  else
  {
    for(int k=0; k<NODES; k++)
    {
      g->selected = k;
      _area_set_value(g, graph_height, y);
    }
    g->on_node = FALSE;
  }
}

static gboolean _area_scrolled_callback(GtkWidget *widget,
                                        GdkEventScroll *event,
                                        gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;

  return gtk_widget_event(_get_selected(g), (GdkEvent*)event);
}

static gboolean _area_motion_notify_callback(GtkWidget *widget,
                                             GdkEventMotion *event,
                                             gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;

  if(g->dragging && g->on_node)
    _area_set_pos(g, event->y);
  else
  {
    // look if close to a node
    const float epsilon = DT_PIXEL_APPLY_DPI(10.0);
    const int oldsel = g->selected;
    const int oldon = g->on_node;
    g->selected = (int)(((float)event->x - g->points[0][0])
                        / (g->points[1][0] - g->points[0][0]) + 0.5f) % NODES;
    g->on_node = fabsf(g->points[g->selected][1] - (float)event->y) < epsilon;

    char *tooltip = g_strdup_printf(_("middle click to toggle sliders visibility\n\n%s"),
                                    DT_BAUHAUS_WIDGET(g->sat_sliders[g->selected])->label);
    gtk_widget_set_tooltip_text(widget, tooltip);
    g_free(tooltip);
    if(oldsel != g->selected || oldon != g->on_node)
      gtk_widget_queue_draw(GTK_WIDGET(g->area));
  }

  return TRUE;
}

static gboolean _area_button_press_callback(GtkWidget *widget,
                                            GdkEventButton *event,
                                            gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;

  if(event->button == 2
     || (event->button == 1 // Ctrl+Click alias for macOS
         && dt_modifier_is(event->state, GDK_CONTROL_MASK)))
  {
    dt_conf_set_bool("plugins/darkroom/colorequal/show_sliders",
                     gtk_widget_get_visible(g->cs.expander));
    gui_update(self);
  }
  else if(event->button == 1)
  {
    if(event->type == GDK_2BUTTON_PRESS)
    {
      _area_reset_nodes(g);
      return TRUE;
    }
    else
    {
      g->dragging = TRUE;
    }
  }
  else
    return gtk_widget_event(_get_selected(g), (GdkEvent*)event);

  return FALSE;
}

static gboolean _area_button_release_callback(GtkWidget *widget,
                                              GdkEventButton *event,
                                              gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;

  if(event->button == 1)
  {
    g->dragging = FALSE;
    return TRUE;
  }

  return FALSE;
}


static gboolean _area_size_callback(GtkWidget *widget,
                                              GdkEventButton *event,
                                              gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  g->gradients_cached = FALSE;
  return FALSE;
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;

  // Get the current display profile
  struct dt_iop_order_iccprofile_info_t *work_profile =
    dt_ioppr_get_pipe_output_profile_info(self->dev->full.pipe);

  // Check if it is different than the one in cache, and update it if needed
  if(work_profile != g->work_profile)
  {
    // Re-init the profiles
    if(g->white_adapted_profile)
      dt_free_align(g->white_adapted_profile);
    g->white_adapted_profile = D65_adapt_iccprofile(work_profile);
    g->work_profile = work_profile;
    g->gradients_cached = FALSE;

    // Regenerate the display gamut LUT - Default to Rec709 D65 aka linear sRGB
    dt_colormatrix_t input_matrix = { { 0.4124564f, 0.3575761f, 0.1804375f, 0.f },
                                      { 0.2126729f, 0.7151522f, 0.0721750f, 0.f },
                                      { 0.0193339f, 0.1191920f, 0.9503041f, 0.f },
                                      { 0.f } };
    if(g->white_adapted_profile != NULL)
      memcpy(input_matrix, g->white_adapted_profile->matrix_in, sizeof(dt_colormatrix_t));
    else
      dt_print(DT_DEBUG_PIPE, "[colorequal] display color space falls back to sRGB\n");

    dt_UCS_22_build_gamut_LUT(input_matrix, g->gamut_LUT);
    g->max_saturation = get_minimum_saturation(g->gamut_LUT, 1.0f, 1.f);
  }

  ++darktable.gui->reset;
  if((work_profile != g->work_profile) || (w == g->hue_shift))
    _init_sliders(self);
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
  --darktable.gui->reset;
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

  if(g->white_adapted_profile)
  {
    dt_free_align(g->white_adapted_profile);
    g->white_adapted_profile = NULL;
  }

  dt_free_align(g->gamut_LUT);

  // Destroy the background cache
  for(dt_iop_colorequal_channel_t chan = 0; chan < NUM_CHANNELS; chan++)
  {
    if(g->b_data[chan])
      free(g->b_data[chan]);
    if(g->b_surface[chan])
      cairo_surface_destroy(g->b_surface[chan]);
  }

  dt_conf_set_int("plugins/darkroom/colorequal/gui_page",
                  gtk_notebook_get_current_page (g->notebook));

  IOP_GUI_FREE;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_colorequal_params_t *p = (dt_iop_colorequal_params_t *)self->params;
  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->use_filter), p->use_filter);
  gui_changed(self, NULL, NULL);

  gboolean show_sliders = dt_conf_get_bool("plugins/darkroom/colorequal/show_sliders");
  gtk_widget_set_visible(g->cs.expander, !show_sliders);

  // reset masking
  dt_bauhaus_widget_set_quad_active(g->param_size, FALSE);
  dt_bauhaus_widget_set_quad_active(g->chroma_size, FALSE);
  g->mask_mode = 0;

  gtk_widget_set_name(GTK_WIDGET(g->cs.container), show_sliders ? NULL : "collapsible");

  const int nbpage = gtk_notebook_get_n_pages(g->notebook);

  if((nbpage == 4) ^ show_sliders)
  {
    GtkWidget *cs = GTK_WIDGET(g->cs.container);

    g_object_ref(cs);
    gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(cs)), cs);

    if(show_sliders)
    {
      // create a new tab for options
      GtkWidget *np = dt_ui_notebook_page(g->notebook, N_("options"), _("options"));
      // move options container into the opts_box (inlined into the main gui box)
      gtk_container_add(GTK_CONTAINER(g->opts_box), cs);
      gtk_widget_show_all(np);
    }
    else
    {
      // remove options notebook tab
      gtk_notebook_remove_page(g->notebook, 3);
      // add the options container into the collapsible section
      gtk_container_add
        (GTK_CONTAINER(dtgtk_expander_get_body_event_box(DTGTK_EXPANDER(g->cs.expander))),
         cs);
    }

    g_object_unref(cs);
  }

  // hide all groups of sliders
  for(int k = 0; k < 3; k++)
    gtk_widget_hide(g->slider_group[k]);

  // display widgets depening on the selected notebook page
  if(g->page_num < 3)
  {
    gtk_widget_show(GTK_WIDGET(g->area));
    gtk_widget_show(g->hue_shift);
    gtk_widget_hide(g->opts_box);

    if(show_sliders)
    {
      gtk_widget_show_all(g->slider_group[g->page_num]);
    }
  }
  else
  {
    gtk_widget_hide(GTK_WIDGET(g->area));
    gtk_widget_hide(g->hue_shift);
    gtk_widget_show_all(g->opts_box);
  }

  gtk_widget_queue_draw(GTK_WIDGET(g->notebook));
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_colorequal_gui_data_t *g = IOP_GUI_ALLOC(colorequal);

  // Init the color profiles and cache them
  struct dt_iop_order_iccprofile_info_t *work_profile = NULL;
  if(self->dev)
    work_profile = dt_ioppr_get_pipe_output_profile_info(self->dev->full.pipe);
  if(g->white_adapted_profile)
    dt_free_align(g->white_adapted_profile);
  g->white_adapted_profile = D65_adapt_iccprofile(work_profile);
  g->work_profile = work_profile;
  g->gradients_cached = FALSE;
  g->on_node = FALSE;
  for(dt_iop_colorequal_channel_t chan = 0; chan < NUM_CHANNELS; chan++)
  {
    g->b_data[chan] = NULL;
    g->b_surface[chan] = NULL;
  }

  // Init the display gamut LUT - Default to Rec709 D65 aka linear sRGB
  g->gamut_LUT = dt_alloc_align_float(LUT_ELEM);
  dt_colormatrix_t input_matrix = { { 0.4124564f, 0.3575761f, 0.1804375f, 0.f },
                                    { 0.2126729f, 0.7151522f, 0.0721750f, 0.f },
                                    { 0.0193339f, 0.1191920f, 0.9503041f, 0.f },
                                    { 0.f } };
  if(g->white_adapted_profile)
    memcpy(input_matrix, g->white_adapted_profile->matrix_in, sizeof(dt_colormatrix_t));

  dt_UCS_22_build_gamut_LUT(input_matrix, g->gamut_LUT);
  g->max_saturation = get_minimum_saturation(g->gamut_LUT, 1.0f, 1.f);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  // start building top level widget
  static dt_action_def_t notebook_def = { };
  g->notebook = dt_ui_notebook_new(&notebook_def);
  dt_action_define_iop(self, NULL, N_("page"), GTK_WIDGET(g->notebook), &notebook_def);
  g_signal_connect(G_OBJECT(g->notebook), "switch_page",
                   G_CALLBACK(_channel_tabs_switch_callback), self);
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(g->notebook), TRUE, TRUE, 0);

  // add notebook tab, will remain empty as we need to share the graph. the widgets
  // to show/hide are handled in gui_update depending on the actual tab selected.
  dt_ui_notebook_page(g->notebook, N_("hue"), _("change hue hue-wise"));
  dt_ui_notebook_page(g->notebook, N_("saturation"), _("change saturation hue-wise"));
  dt_ui_notebook_page(g->notebook, N_("brightness"), _("change brightness hue-wise"));

  // graph
  g->area = GTK_DRAWING_AREA
    (dt_ui_resize_wrap(NULL, 0,
                       "plugins/darkroom/colorequal/aspect_percent"));
  g_object_set_data(G_OBJECT(g->area), "iop-instance", self);
  dt_action_define_iop(self, NULL, N_("graph"), GTK_WIDGET(g->area), NULL);
  gtk_widget_set_can_focus(GTK_WIDGET(g->area), TRUE);
  gtk_widget_add_events(GTK_WIDGET(g->area),
                        GDK_BUTTON_PRESS_MASK
                        | GDK_POINTER_MOTION_MASK
                        | GDK_BUTTON_RELEASE_MASK
                        | GDK_SCROLL_MASK
                        | GDK_SMOOTH_SCROLL_MASK);
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(_iop_colorequalizer_draw), self);
  g_signal_connect(G_OBJECT(g->area), "button-press-event",
                   G_CALLBACK(_area_button_press_callback), self);
  g_signal_connect(G_OBJECT(g->area), "button-release-event",
                   G_CALLBACK(_area_button_release_callback), self);
  g_signal_connect(G_OBJECT(g->area), "motion-notify-event",
                   G_CALLBACK(_area_motion_notify_callback), self);
  g_signal_connect(G_OBJECT(g->area), "scroll-event",
                   G_CALLBACK(_area_scrolled_callback), self);
  g_signal_connect(G_OBJECT(g->area), "size_allocate",
                   G_CALLBACK(_area_size_callback), self);
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(g->area), TRUE, TRUE, 0);

  // box containing all options. the widget in here can be either into a collapsible
  // section or inside this box when the options tab is activated.
  g->opts_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(g->opts_box), TRUE, TRUE, 0);

  self->widget = box;
  g->hue_shift = dt_bauhaus_slider_from_params(self, "hue_shift");
  dt_bauhaus_slider_set_format(g->hue_shift, "°");
  dt_bauhaus_slider_set_digits(g->hue_shift, 0);
  gtk_widget_set_tooltip_text(g->hue_shift,
                              _("shift nodes to lower or higher hue"));

  GtkWidget *group;
  int group_n = 0;

#define GROUP_SLIDERS                               \
  group = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); \
  gtk_box_pack_start(GTK_BOX(box), group, TRUE, TRUE, 0); \
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); \
  gtk_box_pack_start(GTK_BOX(group), self->widget, TRUE, TRUE, 0); \
  g->slider_group[group_n++] = group;

  dt_iop_module_t *sect = DT_IOP_SECTION_FOR_PARAMS(self, N_("hue"));

  GROUP_SLIDERS
  g->hue_sliders[0] = g->hue_red =
    dt_bauhaus_slider_from_params(sect, "hue_red");
  g->hue_sliders[1] = g->hue_orange =
    dt_bauhaus_slider_from_params(sect, "hue_orange");
  g->hue_sliders[2] = g->hue_yellow =
    dt_bauhaus_slider_from_params(sect, "hue_yellow");
  g->hue_sliders[3] = g->hue_green =
    dt_bauhaus_slider_from_params(sect, "hue_green");
  g->hue_sliders[4] = g->hue_cyan =
    dt_bauhaus_slider_from_params(sect, "hue_cyan");
  g->hue_sliders[5] = g->hue_blue =
    dt_bauhaus_slider_from_params(sect, "hue_blue");
  g->hue_sliders[6] = g->hue_lavender =
    dt_bauhaus_slider_from_params(sect, "hue_lavender");
  g->hue_sliders[7] = g->hue_magenta =
    dt_bauhaus_slider_from_params(sect, "hue_magenta");

  sect = DT_IOP_SECTION_FOR_PARAMS(self, N_("saturation"));

  GROUP_SLIDERS
  g->sat_sliders[0] = g->sat_red =
    dt_bauhaus_slider_from_params(sect, "sat_red");
  g->sat_sliders[1] = g->sat_orange =
    dt_bauhaus_slider_from_params(sect, "sat_orange");
  g->sat_sliders[2] = g->sat_yellow =
    dt_bauhaus_slider_from_params(sect, "sat_yellow");
  g->sat_sliders[3] = g->sat_green =
    dt_bauhaus_slider_from_params(sect, "sat_green");
  g->sat_sliders[4] = g->sat_cyan =
    dt_bauhaus_slider_from_params(sect, "sat_cyan");
  g->sat_sliders[5] = g->sat_blue =
    dt_bauhaus_slider_from_params(sect, "sat_blue");
  g->sat_sliders[6] = g->sat_lavender =
    dt_bauhaus_slider_from_params(sect, "sat_lavender");
  g->sat_sliders[7] = g->sat_magenta =
    dt_bauhaus_slider_from_params(sect, "sat_magenta");

  sect = DT_IOP_SECTION_FOR_PARAMS(self, N_("brightness"));

  GROUP_SLIDERS
  g->bright_sliders[0] = g->bright_red =
    dt_bauhaus_slider_from_params(sect, "bright_red");
  g->bright_sliders[1] = g->bright_orange =
    dt_bauhaus_slider_from_params(sect, "bright_orange");
  g->bright_sliders[2] = g->bright_yellow =
    dt_bauhaus_slider_from_params(sect, "bright_yellow");
  g->bright_sliders[3] = g->bright_green =
    dt_bauhaus_slider_from_params(sect, "bright_green");
  g->bright_sliders[4] = g->bright_cyan =
    dt_bauhaus_slider_from_params(sect, "bright_cyan");
  g->bright_sliders[5] = g->bright_blue =
    dt_bauhaus_slider_from_params(sect, "bright_blue");
  g->bright_sliders[6] = g->bright_lavender =
    dt_bauhaus_slider_from_params(sect, "bright_lavender");
  g->bright_sliders[7] = g->bright_magenta =
    dt_bauhaus_slider_from_params(sect, "bright_magenta");

  dt_gui_new_collapsible_section
    (&g->cs,
     "plugins/darkroom/colorequal/expand_options",
     _("options"),
     GTK_BOX(box),
     DT_ACTION(self));
  self->widget = GTK_WIDGET(g->cs.container);

  g->white_level = dt_color_picker_new(self, DT_COLOR_PICKER_AREA,
                                       dt_bauhaus_slider_from_params(self, "white_level"));
  dt_bauhaus_slider_set_soft_range(g->white_level, -2., +2.);
  dt_bauhaus_slider_set_format(g->white_level, _(" EV"));

  g->smoothing_hue = dt_bauhaus_slider_from_params(sect, "smoothing_hue");
  gtk_widget_set_tooltip_text(g->smoothing_hue,
                              _("change for sharper or softer hue curve"));

  g->use_filter = dt_bauhaus_toggle_from_params(self, "use_filter");

  g->chroma_size = dt_bauhaus_slider_from_params(self, "chroma_size");
  dt_bauhaus_slider_set_digits(g->chroma_size, 1);
  dt_bauhaus_slider_set_format(g->chroma_size, _(" px"));
  gtk_widget_set_tooltip_text(g->chroma_size,
                              _("blurring radius of chroma prefilter analysis"));
  dt_bauhaus_widget_set_quad_paint(g->chroma_size, dtgtk_cairo_paint_showmask, 0, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->chroma_size, TRUE);
  dt_bauhaus_widget_set_quad_active(g->chroma_size, FALSE);
  g_signal_connect(G_OBJECT(g->chroma_size), "quad-pressed", G_CALLBACK(_masking_callback_c), self);
  dt_bauhaus_widget_set_quad_tooltip(g->chroma_size,
    _("visualize weighing function on changed output.\n"
      "red shows possibly changed data, blueish parts will not be changed."));

  g->param_size = dt_bauhaus_slider_from_params(self, "param_size");
  dt_bauhaus_slider_set_digits(g->param_size, 1);
  dt_bauhaus_slider_set_format(g->param_size, _(" px"));
  gtk_widget_set_tooltip_text(g->param_size, _("blurring radius of applied parameters"));

  dt_bauhaus_widget_set_quad_paint(g->param_size, dtgtk_cairo_paint_showmask, 0, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->param_size, TRUE);
  dt_bauhaus_widget_set_quad_active(g->param_size, FALSE);
  g_signal_connect(G_OBJECT(g->param_size), "quad-pressed", G_CALLBACK(_masking_callback_p), self);
  dt_bauhaus_widget_set_quad_tooltip(g->param_size,
    _("visualize changed output for the selected tab.\n"
    "red shows increased data, blue decreased."));

  _init_sliders(self);

  // restore the previously saved active tab
  const guint active_page = dt_conf_get_int("plugins/darkroom/colorequal/gui_page");
  if(active_page < 3)
  {
    gtk_widget_show(gtk_notebook_get_nth_page(g->notebook, active_page));
    gtk_notebook_set_current_page(g->notebook, active_page);
  }
  g->channel = (active_page >= NUM_CHANNELS) ? SATURATION : active_page;
  g->page_num = active_page;

  self->widget = GTK_WIDGET(box);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
