/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "desktop.h"
#include "common/debug.h"
#include "common/option.h"
#include "common/locking.h"
#include "common/array.h"

#include "app.h"
#include "texture.h"
#include "shader.h"
#include "desktop_rects.h"
#include "cimgui.h"

#include <stdlib.h>
#include <string.h>

// these headers are auto generated by cmake
#include "desktop.vert.h"
#include "desktop_rgb.frag.h"
#include "desktop_rgb.def.h"

#include "basic.vert.h"
#include "ffx_cas.frag.h"

struct DesktopShader
{
  EGL_Shader * shader;
  GLint uTransform;
  GLint uDesktopSize;
  GLint uTextureScale;
  GLint uScaleAlgo;
  GLint uNVGain;
  GLint uCBMode;
};

struct EGL_Desktop
{
  EGL * egl;
  EGLDisplay * display;

  EGL_Texture          * texture;
  struct DesktopShader shader;
  EGL_DesktopRects     * mesh;
  CountedBuffer        * matrix;

  // internals
  int               width, height;
  LG_RendererRotate rotate;

  // scale algorithm
  int scaleAlgo;

  // night vision
  int nvMax;
  int nvGain;

  // colorblind mode
  int cbMode;

  bool useDMA;
  LG_RendererFormat format;

  EGL_Shader * ffxCAS;
  bool enableCAS;
  PostProcessHandle ffxCASHandle;
};

// forwards
void egl_desktop_toggle_nv(int key, void * opaque);

static bool egl_initDesktopShader(
  struct DesktopShader * shader,
  const char * vertex_code  , size_t vertex_size,
  const char * fragment_code, size_t fragment_size
)
{
  if (!egl_shaderInit(&shader->shader))
    return false;

  if (!egl_shaderCompile(shader->shader,
        vertex_code  , vertex_size,
        fragment_code, fragment_size))
  {
    return false;
  }

  shader->uTransform    = egl_shaderGetUniform(shader->shader, "transform"   );
  shader->uDesktopSize  = egl_shaderGetUniform(shader->shader, "size"        );
  shader->uTextureScale = egl_shaderGetUniform(shader->shader, "textureScale");
  shader->uScaleAlgo    = egl_shaderGetUniform(shader->shader, "scaleAlgo"   );
  shader->uNVGain       = egl_shaderGetUniform(shader->shader, "nvGain"      );
  shader->uCBMode       = egl_shaderGetUniform(shader->shader, "cbMode"      );

  return true;
}

static void setupFilters(EGL_Desktop * desktop)
{
  desktop->ffxCASHandle =
    egl_textureAddFilter(desktop->texture, desktop->ffxCAS, 1.0f, false);
}

bool egl_desktopInit(EGL * egl, EGL_Desktop ** desktop_, EGLDisplay * display,
    bool useDMA, int maxRects)
{
  EGL_Desktop * desktop = (EGL_Desktop *)calloc(1, sizeof(EGL_Desktop));
  if (!desktop)
  {
    DEBUG_ERROR("Failed to malloc EGL_Desktop");
    return false;
  }
  *desktop_ = desktop;

  desktop->egl     = egl;
  desktop->display = display;

  if (!egl_textureInit(egl, &desktop->texture, display,
        useDMA ? EGL_TEXTYPE_DMABUF : EGL_TEXTYPE_FRAMEBUFFER, true))
  {
    DEBUG_ERROR("Failed to initialize the desktop texture");
    return false;
  }

  if (!egl_initDesktopShader(
    &desktop->shader,
    b_shader_desktop_vert    , b_shader_desktop_vert_size,
    b_shader_desktop_rgb_frag, b_shader_desktop_rgb_frag_size))
  {
    DEBUG_ERROR("Failed to initialize the generic desktop shader");
    return false;
  }

  if (!egl_desktopRectsInit(&desktop->mesh, maxRects))
  {
    DEBUG_ERROR("Failed to initialize the desktop mesh");
    return false;
  }

  desktop->matrix = countedBufferNew(6 * sizeof(GLfloat));
  if (!desktop->matrix)
  {
    DEBUG_ERROR("Failed to allocate the desktop matrix buffer");
    return false;
  }

  app_registerKeybind(KEY_N, egl_desktop_toggle_nv, desktop,
      "Toggle night vision mode");

  desktop->nvMax     = option_get_int("egl", "nvGainMax");
  desktop->nvGain    = option_get_int("egl", "nvGain"   );
  desktop->cbMode    = option_get_int("egl", "cbMode"   );
  desktop->scaleAlgo = option_get_int("egl", "scale"    );
  desktop->useDMA    = useDMA;

  egl_shaderInit(&desktop->ffxCAS);
  egl_shaderCompile(desktop->ffxCAS,
      b_shader_basic_vert  , b_shader_basic_vert_size,
      b_shader_ffx_cas_frag, b_shader_ffx_cas_frag_size);

  setupFilters(desktop);

  return true;
}

void egl_desktop_toggle_nv(int key, void * opaque)
{
  EGL_Desktop * desktop = (EGL_Desktop *)opaque;
  if (desktop->nvGain++ == desktop->nvMax)
    desktop->nvGain = 0;

       if (desktop->nvGain == 0) app_alert(LG_ALERT_INFO, "NV Disabled");
  else if (desktop->nvGain == 1) app_alert(LG_ALERT_INFO, "NV Enabled");
  else app_alert(LG_ALERT_INFO, "NV Gain + %d", desktop->nvGain - 1);

  app_invalidateWindow(true);
}

bool egl_desktopScaleValidate(struct Option * opt, const char ** error)
{
  if (opt->value.x_int >= 0 && opt->value.x_int < EGL_SCALE_MAX)
    return true;

  *error = "Invalid scale algorithm number";
  return false;
}

void egl_desktopFree(EGL_Desktop ** desktop)
{
  if (!*desktop)
    return;

  egl_textureFree    (&(*desktop)->texture      );
  egl_shaderFree     (&(*desktop)->shader.shader);
  egl_desktopRectsFree(&(*desktop)->mesh        );
  countedBufferRelease(&(*desktop)->matrix      );
  egl_shaderFree(&(*desktop)->ffxCAS);

  free(*desktop);
  *desktop = NULL;
}

static const char * algorithmNames[EGL_SCALE_MAX] = {
  [EGL_SCALE_AUTO]    = "Automatic (downscale: linear, upscale: nearest)",
  [EGL_SCALE_NEAREST] = "Nearest",
  [EGL_SCALE_LINEAR]  = "Linear",
};

void egl_desktopConfigUI(EGL_Desktop * desktop)
{
  igText("Scale algorithm:");
  igPushItemWidth(igGetWindowWidth() - igGetStyle()->WindowPadding.x * 2);
  if (igBeginCombo("##scale", algorithmNames[desktop->scaleAlgo], 0))
  {
    for (int i = 0; i < EGL_SCALE_MAX; ++i)
    {
      bool selected = i == desktop->scaleAlgo;
      if (igSelectableBool(algorithmNames[i], selected, 0, (ImVec2) { 0.0f, 0.0f }))
        desktop->scaleAlgo = i;
      if (selected)
        igSetItemDefaultFocus();
    }
    igEndCombo();
  }
  igPopItemWidth();

  igText("Night vision mode:");
  igSameLine(0.0f, -1.0f);
  igPushItemWidth(igGetWindowWidth() - igGetCursorPosX() - igGetStyle()->WindowPadding.x);

  const char * format;
  switch (desktop->nvGain)
  {
    case 0: format = "off"; break;
    case 1: format = "on";  break;
    default: format = "gain: %d";
  }
  igSliderInt("##nvgain", &desktop->nvGain, 0, desktop->nvMax, format, 0);
  igPopItemWidth();

  bool cas = desktop->enableCAS;
  igCheckbox("AMD FidelityFX CAS", &cas);
  if (cas != desktop->enableCAS)
  {
    desktop->enableCAS = cas;
    egl_textureEnableFilter(desktop->ffxCASHandle, cas);
  }
}

bool egl_desktopSetup(EGL_Desktop * desktop, const LG_RendererFormat format)
{
  memcpy(&desktop->format, &format, sizeof(LG_RendererFormat));

  enum EGL_PixelFormat pixFmt;
  switch(format.type)
  {
    case FRAME_TYPE_BGRA:
      pixFmt = EGL_PF_BGRA;
      break;

    case FRAME_TYPE_RGBA:
      pixFmt = EGL_PF_RGBA;
      break;

    case FRAME_TYPE_RGBA10:
      pixFmt = EGL_PF_RGBA10;
      break;

    case FRAME_TYPE_RGBA16F:
      pixFmt = EGL_PF_RGBA16F;
      break;

    default:
      DEBUG_ERROR("Unsupported frame format");
      return false;
  }

  desktop->width  = format.width;
  desktop->height = format.height;

  if (!egl_textureSetup(
    desktop->texture,
    pixFmt,
    format.width,
    format.height,
    format.pitch
  ))
  {
    DEBUG_ERROR("Failed to setup the desktop texture");
    return false;
  }

  return true;
}

bool egl_desktop_update(EGL_Desktop * desktop, const FrameBuffer * frame, int dmaFd,
    const FrameDamageRect * damageRects, int damageRectsCount)
{
  if (desktop->useDMA && dmaFd >= 0)
  {
    if (egl_textureUpdateFromDMA(desktop->texture, frame, dmaFd))
      return true;

    DEBUG_WARN("DMA update failed, disabling DMABUF imports");
    desktop->useDMA = false;

    egl_textureFree(&desktop->texture);
    if (!egl_textureInit(desktop->egl, &desktop->texture, desktop->display,
          EGL_TEXTYPE_FRAMEBUFFER, true))
    {
      DEBUG_ERROR("Failed to initialize the desktop texture");
      return false;
    }

    setupFilters(desktop);

    if (!egl_desktopSetup(desktop, desktop->format))
      return false;
  }

  return egl_textureUpdateFromFrame(desktop->texture, frame, damageRects, damageRectsCount);
}

bool egl_desktopRender(EGL_Desktop * desktop, const float x, const float y,
    const float scaleX, const float scaleY, enum EGL_DesktopScaleType scaleType,
    LG_RendererRotate rotate, const struct DamageRects * rects)
{
  enum EGL_TexStatus status;
  if ((status = egl_textureProcess(desktop->texture)) != EGL_TEX_STATUS_OK)
  {
    if (status != EGL_TEX_STATUS_NOTREADY)
      DEBUG_ERROR("Failed to process the desktop texture");
  }

  int scaleAlgo = EGL_SCALE_NEAREST;

  switch (desktop->scaleAlgo)
  {
    case EGL_SCALE_AUTO:
      switch (scaleType)
      {
        case EGL_DESKTOP_NOSCALE:
        case EGL_DESKTOP_UPSCALE:
          scaleAlgo = EGL_SCALE_NEAREST;
          break;

        case EGL_DESKTOP_DOWNSCALE:
          scaleAlgo = EGL_SCALE_LINEAR;
          break;
      }
      break;

    default:
      scaleAlgo = desktop->scaleAlgo;
  }

  egl_desktopRectsMatrix((float *)desktop->matrix->data,
      desktop->width, desktop->height, x, y, scaleX, scaleY, rotate);
  egl_desktopRectsUpdate(desktop->mesh, rects, desktop->width, desktop->height);

  egl_textureBind(desktop->texture);

  const struct DesktopShader * shader = &desktop->shader;
  EGL_Uniform uniforms[] =
  {
    {
      .type        = EGL_UNIFORM_TYPE_1I,
      .location    = shader->uScaleAlgo,
      .i           = { scaleAlgo },
    },
    {
      .type        = EGL_UNIFORM_TYPE_2F,
      .location    = shader->uDesktopSize,
      .f           = { desktop->width, desktop->height },
    },
    {
      .type        = EGL_UNIFORM_TYPE_1F,
      .location    = shader->uTextureScale,
      .f           = { egl_textureGetScale(desktop->texture) },
    },
    {
      .type        = EGL_UNIFORM_TYPE_M3x2FV,
      .location    = shader->uTransform,
      .m.transpose = GL_FALSE,
      .m.v         = desktop->matrix
    },
    {
      .type        = EGL_UNIFORM_TYPE_1F,
      .location    = shader->uNVGain,
      .f           = { (float)desktop->nvGain }
    },
    {
      .type        = EGL_UNIFORM_TYPE_1I,
      .location    = shader->uCBMode,
      .f           = { desktop->cbMode }
    }
  };

  egl_shaderSetUniforms(shader->shader, uniforms, ARRAY_LENGTH(uniforms));
  egl_shaderUse(shader->shader);
  egl_desktopRectsRender(desktop->mesh);
  glBindTexture(GL_TEXTURE_2D, 0);
  return true;
}
