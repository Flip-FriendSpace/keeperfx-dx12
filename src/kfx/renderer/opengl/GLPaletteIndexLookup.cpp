// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
/******************************************************************************/
// Dungeon Keeper - Renderer Abstraction Layer
/******************************************************************************/
/** @file GLPaletteIndexLookup.cpp
 *     See GLPaletteIndexLookup.h.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/renderer/opengl/GLPaletteIndexLookup.h"
#include "kfx/renderer/opengl/GLFunctions.h"
#include "kfx/renderer/opengl/GLResourceMapper.h"
#include "kfx/renderer/PaletteIndexTable.h"
#include "bflib_video.h"   // LbPaletteFindColour
#include <cstring>
#include "post_inc.h"

namespace {
constexpr int k_dim = 512;
constexpr int k_keys = k_dim * k_dim;
}

bool GLPaletteIndexLookup::Init(GLResourceMapper* mapper)
{
    m_mapper = mapper;
    m_base.assign(k_keys, 0);
    m_table.assign(k_keys, 0);
    m_have_palette = false;

    GpuTextureDesc desc;
    desc.width = k_dim;
    desc.height = k_dim;
    desc.format = GpuTextureFormat::R8;
    desc.min_filter = GpuTextureFilter::Nearest;
    desc.mag_filter = GpuTextureFilter::Nearest;
    desc.wrap = GpuTextureWrap::Clamp;
    desc.initial_pixels = m_table;
    desc.debug_name = "palette_index_lookup";
    m_tex_handle = m_mapper->RequestCreateTexture(desc);
    const GLTexture* texture = m_mapper->ResolveTexture(m_tex_handle);
    if (texture) m_capture_uploaded.Record(m_tex_handle, texture->id, k_dim, k_dim, m_table.data(), m_table.size());
    return texture != nullptr;
}

void GLPaletteIndexLookup::Shutdown()
{
    m_base.clear();
    m_table.clear();
    m_have_palette = false;
    m_capture_uploaded = {};
}

void GLPaletteIndexLookup::RefreshBase(const unsigned char* engine_palette)
{
    if (engine_palette == nullptr || m_base.empty())
        return;
    kfx::render::BuildPaletteIndexBase(m_base, {engine_palette, 768}, LbPaletteFindColour);

    m_table = m_base;
    if (m_have_palette)
        apply_palette();
    upload();
}

void GLPaletteIndexLookup::SetPalette(const unsigned char* rgba)
{
    if (rgba == nullptr || m_table.empty())
        return;
    if (m_have_palette && std::memcmp(m_palette_rgba, rgba, sizeof(m_palette_rgba)) == 0)
        return;

    if (m_have_palette)
    {
        for (int i = 0; i < 256; i++)
            m_table[m_palette_keys[i]] = m_base[m_palette_keys[i]];
    }
    std::memcpy(m_palette_rgba, rgba, sizeof(m_palette_rgba));
    m_have_palette = true;
    apply_palette();
    upload();
}

void GLPaletteIndexLookup::apply_palette()
{
    kfx::render::ApplyPaletteIndexOverrides(m_table, m_palette_rgba, m_palette_keys);
}

void GLPaletteIndexLookup::upload()
{
    const GLTexture* tex = m_mapper ? m_mapper->ResolveTexture(m_tex_handle) : nullptr;
    if (tex == nullptr)
        return;
    glBindTexture(GL_TEXTURE_2D, tex->id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, k_dim, k_dim, GL_RED, GL_UNSIGNED_BYTE, m_table.data());
    GLint row_length = 0, skip_rows = 0, skip_pixels = 0, unpack_buffer = 0;
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length); glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels); glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
    if (!row_length && !skip_rows && !skip_pixels && !unpack_buffer)
        m_capture_uploaded.Record(m_tex_handle, tex->id, k_dim, k_dim, m_table.data(), m_table.size());
    else m_capture_uploaded.bytes.clear();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
}

/******************************************************************************/
