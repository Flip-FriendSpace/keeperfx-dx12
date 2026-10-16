// KeeperFX Windows x64 / DirectX 12 preview changes: 2026-10-14.
/******************************************************************************/
// Dungeon Keeper - Renderer Abstraction Layer
/******************************************************************************/
/** @file GLWorldViewRenderer.cpp
 *     OpenGL world-geometry renderer.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "kfx/renderer/WorldSceneCapture.h"
#include "kfx/renderer/SceneAssetCapture.h"
#include "kfx/renderer/opengl/GLWorldViewRenderer.h"
#include "kfx/renderer/opengl/GLTileAtlas.h"
#include "kfx/renderer/SpriteScale.h"
#include "kfx/renderer/KeeperSpriteDecode.h"
#include "kfx/renderer/KeeperShadowCoverage.h"
#include "kfx/renderer/LightmapSnapshot.h"

#include "kfx/renderer/ITileAtlas.h"
#include "kfx/renderer/opengl/GLShaders.h"
#include "kfx/renderer/opengl/GLResourceMapper.h"
#include "kfx/renderer/RendererThread.h"   // ASSERT_GAME_THREAD/ASSERT_RENDER_THREAD
#include "kfx/renderer/WorldRenderState.h"
#include "kfx/renderer/RendererSettings.h" // g_renderer_settings
#include "kfx/renderer/RendererManager.h"  // RendererGetCurrentSpriteOwner/WantsOutline
#include "kfx/profiling/KfxProfiling.h"
// TODO : I really don't like this touching the engine, should really be cleaning this up.
#include "front_simple.h"                   // engine_palette
#include "player_data.h"                    // get_player_color_idx/player_room_colours (outline colour)

#include "engine_buckets.h"   // QKinds enum, BasicQ, BucketKind* structs, buckets[]
#include "engine_render.h"    // BUCKETS_COUNT, orient_to_mapU/V tables
#include "engine_textures.h"  // TEXTURE_BLOCKS_COUNT, TEXTURE_LAND_MARKED_*, block_ptrs[]
#include "creature_graphics.h" // struct KeeperSprite, creature_table[], creature_table_add[]
#include "bflib_render.h"     // struct PolyPoint, render_fade_tables/render_ghost/render_alpha
#include "bflib_vidraw.h"     // vec_window_width/height
#include "bflib_video.h"      // LbPaletteGetReadonly(), pixel_size, Lb_SPRITE_* flags
#include "bflib_basics.h"     // ERRORLOG / SYNCLOG / WARNLOG
#include "vidmode.h"          // pixmap, alpha_sprite_table
#include "player_data.h"      // get_my_player(), PVM_*
#include "local_camera.h"     // get_local_active_camera() (spinning-key gate)
#include "game_legacy.h"      // game.lish.subtile_lightness (lightmap snapshot in FlipBuffers)

#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <exception>
#include <stdexcept>
#include "post_inc.h"

/******************************************************************************/

namespace {
bool capture_tight_unpack(unsigned row_bytes)
{
    GLint alignment = 0, length = 0, rows = 0, pixels = 0, buffer = 0, swap = 0;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment); glGetIntegerv(GL_UNPACK_ROW_LENGTH, &length);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &rows); glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &pixels);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &buffer); glGetIntegerv(GL_UNPACK_SWAP_BYTES, &swap);
    return alignment > 0 && row_bytes % unsigned(alignment) == 0 && !length && !rows && !pixels && !buffer && !swap;
}
bool capture_clear_state()
{
    GLboolean colour[4]{}, depth = GL_FALSE; GLdouble depth_clear = 0;
    glGetBooleanv(GL_COLOR_WRITEMASK, colour); glGetBooleanv(GL_DEPTH_WRITEMASK, &depth);
    glGetDoublev(GL_DEPTH_CLEAR_VALUE, &depth_clear);
    return !glIsEnabled(GL_SCISSOR_TEST) && !glIsEnabled(GL_FRAMEBUFFER_SRGB)
        && colour[0] && colour[1] && colour[2] && colour[3] && depth && depth_clear == 1;
}
}

GLuint GLWorldViewRenderer::ResolvePaletteTexId() const
{
    const GLTexture* tex = m_resource_mapper ? m_resource_mapper->ResolveTexture(m_palette_tex_handle) : nullptr;
    return tex ? tex->id : 0;
}

GLuint GLWorldViewRenderer::ResolveFadeTexId() const
{
    const GLTexture* tex = m_resource_mapper ? m_resource_mapper->ResolveTexture(m_fade_tex_handle) : nullptr;
    return tex ? tex->id : 0;
}

GLuint GLWorldViewRenderer::ResolveShaderId(GpuResourceHandle handle) const
{
    const GLProgram* prog = m_resource_mapper ? m_resource_mapper->ResolveProgram(handle) : nullptr;
    return prog ? prog->id : 0;
}


/** Max dimension of a keeper-sprite decode buffer (stride for GL_UNPACK_ROW_LENGTH). */
static constexpr int k_kspr_decode_dim = 1024;
static constexpr int k_kspr_atlas_dim = 512;

/** Scratch buffer used to hold decoded RG pixels (palette index, coverage)
 *  before upload. Stride is always k_kspr_decode_dim pixels so glTexSubImage*()
 *  can use GL_UNPACK_ROW_LENGTH regardless of the sprite's actual width. */
static uint8_t s_kspr_decode_buf[k_kspr_decode_dim * k_kspr_decode_dim * 2];
static uint8_t s_kspr_shadow_coverage[k_kspr_atlas_dim * k_kspr_atlas_dim * 2];

/** True when the sprite just decoded into s_kspr_decode_buf has black glow
 *  pixels (codes 50-56), which a glow draw subtracts in a second pass. */
static bool decoded_sprite_darkens(int w, int h)
{
    for (int y = 0; y < h; ++y)
    {
        const uint8_t* row = s_kspr_decode_buf + y * k_kspr_decode_dim * 2;
        for (int x = 0; x < w; ++x)
            if (row[x * 2 + 1] != 0 && row[x * 2] >= 50 && row[x * 2] <= 56)
                return true;
    }
    return false;
}

/** Decode only a verified source extent. Failure never exposes partial pixels. */
static bool decode_keeper_rle(uint8_t* dst, const uint8_t* data, size_t data_bytes, int w, int h)
{
    const auto result = kfx::render::DecodeKeeperSpriteRG(
        {{dst, sizeof(s_kspr_decode_buf)}, k_kspr_decode_dim, k_kspr_decode_dim,
            k_kspr_decode_dim * 2}, {data, data_bytes}, (uint32_t)w, (uint32_t)h);
    if (!result) {
        static unsigned warnings = 0;
        if (warnings++ < 20)
            WARNLOG("Keeper sprite decode rejected: status %u at byte %" PRIu64,
                (unsigned)result.status, (uint64_t)result.bytes_consumed);
        return false;
    }
    return true;
}
/******************************************************************************/

GLWorldViewRenderer::~GLWorldViewRenderer()
{
    free_gl_resources();
}

/******************************************************************************/

bool GLWorldViewRenderer::init_gl_resources()
{
    if (m_initialized)
        return true;

    if (!compile_world_shaders())
    {
        ERRORLOG("GLWorldViewRenderer: compile_world_shaders failed");
        return false;
    }

    // VAO + dynamic VBO for world geometry
    {
        GpuGeometryBufferDesc geom_desc;
        geom_desc.vertex_stride = (uint32_t)sizeof(WorldVertex);
        geom_desc.attribs = {
            // layout(location=0) vec3 a_pos  — x,y,z at byte offset 0
            { 0, 3, GpuVertexAttribType::Float, 0 },
            // layout(location=1) vec2 a_uv   — u,v at byte offset 12 (after x,y,z)
            { 1, 2, GpuVertexAttribType::Float, 3 * (uint32_t)sizeof(float) },
            // layout(location=2) float a_shade — shade at byte offset 20 (after x,y,z,u,v)
            { 2, 1, GpuVertexAttribType::Float, 5 * (uint32_t)sizeof(float) },
            // layout(location=3) vec2 a_stl — subtile coords at byte offset 24 (after x,y,z,u,v,shade)
            { 3, 2, GpuVertexAttribType::Float, 6 * (uint32_t)sizeof(float) },
            // layout(location=4) float a_camera_z — camera-space depth for perspective correction, byte offset 32
            { 4, 1, GpuVertexAttribType::Float, 8 * (uint32_t)sizeof(float) },
            // layout(location=5) float a_layer — texture array layer (atlas variation), byte offset 36
            { 5, 1, GpuVertexAttribType::Float, 9 * (uint32_t)sizeof(float) },
            // layout(location=6) vec3 aWorldPos — pre-projection world-space position
            { 6, 3, GpuVertexAttribType::Float, 10 * (uint32_t)sizeof(float) },
            // layout(location=7) float a_tile — tile index within the atlas layer
            { 7, 1, GpuVertexAttribType::Float, 13 * (uint32_t)sizeof(float) },
        };
        geom_desc.dynamic = true;
        geom_desc.initial_vertex_capacity = k_initial_verts * sizeof(WorldVertex);
        geom_desc.debug_name = "world_geom";
        m_geom_handle = m_resource_mapper->RequestCreateGeometryBuffer(geom_desc);
        if (m_resource_mapper->ResolveGeometryBuffer(m_geom_handle) == nullptr)
        {
            ERRORLOG("GLWorldViewRenderer: world geometry buffer realization failed");
            return false;
        }
    }

    // Cache uniform locations and bind samplers to fixed texture units
    const GLuint world_shader_id = ResolveShaderId(m_shader_handle);
    glUseProgram(world_shader_id);
    m_loc_tile_atlas = glGetUniformLocation(world_shader_id, "u_tile_atlas");
    glUniform1i(m_loc_tile_atlas, 0);   // GL_TEXTURE0 — R8 palette-index atlas array
    m_loc_palette = glGetUniformLocation(world_shader_id, "u_palette");
    glUniform1i(m_loc_palette, 1);      // GL_TEXTURE1 — 1D RGBA8 palette
   
    m_loc_fullbright    = glGetUniformLocation(world_shader_id, "u_fullbright");
    m_loc_ambient       = glGetUniformLocation(world_shader_id, "u_ambient");
    m_loc_shade_scale   = glGetUniformLocation(world_shader_id, "u_shade_scale");
    m_loc_shade_gamma   = glGetUniformLocation(world_shader_id, "u_shade_gamma");
    m_loc_lighting_mode = glGetUniformLocation(world_shader_id, "u_lighting_mode");
    m_loc_darkness_mode = glGetUniformLocation(world_shader_id, "u_darkness_mode");
    m_loc_fade_table    = glGetUniformLocation(world_shader_id, "u_fade_table");
    m_loc_time          = glGetUniformLocation(world_shader_id, "u_time");
    m_loc_fog_speed     = glGetUniformLocation(world_shader_id, "u_fog_speed");
    m_loc_fog_density   = glGetUniformLocation(world_shader_id, "u_fog_density");
    m_loc_lightmap      = glGetUniformLocation(world_shader_id, "u_lightmap");
    m_loc_missing_tile  = glGetUniformLocation(world_shader_id, "u_missing_tile");
    m_loc_tile_filter   = glGetUniformLocation(world_shader_id, "u_tile_filter");
    
    glUniform1i(m_loc_fade_table,    3);  // GL_TEXTURE3
    glUniform1f(m_loc_time,          0.0f);
    glUniform1i(m_loc_lightmap,      2);  // GL_TEXTURE2
    glUniform1f(m_loc_missing_tile,  0.0f);
    glUseProgram(0);
    m_shade_settings_applied = false;
    push_shade_uniforms();

    // Lightmap texture — mirrors game.lish.subtile_lightness[] each frame.
    // GL_R16UI stores the raw 0..16128 lightness values; the shader normalises them.
    {
        GpuTextureDesc desc;
        desc.width = MAX_SUBTILES_X;
        desc.height = MAX_SUBTILES_Y;
        desc.format = GpuTextureFormat::R16UI;
        desc.min_filter = GpuTextureFilter::Nearest;
        desc.mag_filter = GpuTextureFilter::Nearest;
        desc.wrap = GpuTextureWrap::Clamp;
        desc.debug_name = "world_lightmap";
        m_lightmap_tex_handle = m_resource_mapper->RequestCreateTexture(desc);
        if (m_resource_mapper->ResolveTexture(m_lightmap_tex_handle) == nullptr)
        {
            ERRORLOG("GLWorldViewRenderer: lightmap texture realization failed");
            return false;
        }
    }

    if (!init_keeper_sprite_shader())
    {
        ERRORLOG("GLWorldViewRenderer: failed to initialise keeper-sprite shader");
        free_gl_resources();
        return false;
    }

    if (!init_shadow_shader())
    {
        WARNLOG("GLWorldViewRenderer: shadow shader unavailable -- creature shadows disabled");
    }

    if (!init_lens_shaders())
    {
        WARNLOG("GLWorldViewRenderer: lens shaders unavailable -- possession lens distortion disabled");
    }

    m_initialized = true;
    SYNCLOG("GLWorldViewRenderer: initialised (VBO %d verts x %u bytes)",
            k_initial_verts, (unsigned)sizeof(WorldVertex));
    return true;
}

/** Pushes the settings-derived subset of the world shader's uniforms from
 *  the current g_renderer_settings. No-op if the world shader hasn't
 *  compiled yet (uniform locations still -1, harmless glUniform on an
 *  unbound program otherwise). */
void GLWorldViewRenderer::push_shade_uniforms()
{
    const GLuint world_shader_id = ResolveShaderId(m_shader_handle);
    if (world_shader_id == 0)
        return;
    const auto values = kfx::render::WorldShadeSettings(g_renderer_settings);
    if (m_shade_settings_applied && values == m_applied_shade_settings)
        return;
    m_applied_shade_settings = values;
    m_shade_settings_applied = true;
    glUseProgram(world_shader_id);
    glUniform1f(m_loc_fullbright,    values[0]);
    glUniform1f(m_loc_ambient,       values[1]);
    glUniform1f(m_loc_shade_scale,   values[2]);
    glUniform1f(m_loc_shade_gamma,   values[3]);
    glUniform1i(m_loc_lighting_mode, static_cast<int>(values[4]));
    glUniform1i(m_loc_darkness_mode, static_cast<int>(values[5]));
    glUniform1f(m_loc_fog_speed,     values[6]);
    glUniform1f(m_loc_fog_density,   values[7]);
    glUniform1i(m_loc_tile_filter,   static_cast<int>(values[8]));
    m_tile_filter_applied = -1;  // force reapply on next flush
    glUseProgram(0);
}

void GLWorldViewRenderer::RefreshFadeTableAndSettings()
{
    if (m_resource_mapper != nullptr)
    {
        const GLTexture* tex = m_resource_mapper->ResolveTexture(m_fade_tex_handle);
        if (tex != nullptr)
        {
            glBindTexture(GL_TEXTURE_2D, tex->id);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 64, GL_RED, GL_UNSIGNED_BYTE, pixmap.fade_tables);
            RecordAppliedFadeForCapture(pixmap.fade_tables);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }
    push_shade_uniforms();
}

void GLWorldViewRenderer::free_gl_resources()
{
    free_keeper_sprite_resources();
    free_shadow_resources();
    free_lens_resources();

    m_initialized = false;
}

void GLWorldViewRenderer::free_keeper_sprite_resources()
{
    m_capture_clut_known.fill(false);
    m_capture_applied_palette_known = false;
    m_capture_applied_fade_known = false;
    m_capture_world_target_known = false;
    for (auto& source : m_capture_sprite_coverage) source = {};
    m_kspr_atlas_used = 0;
    m_kspr_atlas_map.clear();
    m_kspr_clut_remaps.clear();
    m_kspr_clut_used = 1;
    memset(m_kspr_clut_palette_snap, 0, sizeof(m_kspr_clut_palette_snap));
    m_kspr_instances.clear();
    m_kspr_instance_runs.clear();
    m_kspr_outline_instances.clear();
}

bool GLWorldViewRenderer::compile_world_shaders()
{
    if (!m_resource_mapper) return false;
    GpuProgramDesc desc;
    desc.vertex_src = WORLD_VERTEX_SHADER;
    desc.fragment_src = WORLD_FRAGMENT_SHADER;
    desc.debug_name = "world";
    m_shader_handle = m_resource_mapper->RequestCreateProgram(desc);
    return m_resource_mapper->ResolveProgram(m_shader_handle) != nullptr;
}

/******************************************************************************/
// Creature shadows 

namespace {
// GL-side vertex layout for the shadow pass: 7 floats/vertex
// (x, y, u, v, layer, z_ndc, darken). Only used within this section.
struct ShadowGLVertex { float x, y, u, v, layer, z_ndc, darken; };

std::vector<ShadowGLVertex> s_shadow_verts;
} // namespace

bool GLWorldViewRenderer::init_shadow_shader()
{
    if (!m_resource_mapper) return false;

    GpuProgramDesc prog_desc;
    prog_desc.vertex_src = SHADOW_VERTEX_SHADER;
    prog_desc.fragment_src = SHADOW_FRAGMENT_SHADER;
    prog_desc.debug_name = "shadow";
    m_shadow_shader_handle = m_resource_mapper->RequestCreateProgram(prog_desc);

    const GLProgram* prog = m_resource_mapper->ResolveProgram(m_shadow_shader_handle);
    if (!prog) return false;

    glUseProgram(prog->id);
    m_shadow_loc_viewport = glGetUniformLocation(prog->id, "u_viewport");
    m_shadow_loc_sprite   = glGetUniformLocation(prog->id, "u_sprite");
    glUniform1i(m_shadow_loc_sprite, 0);  // GL_TEXTURE0
    glUseProgram(0);

    // VAO + dynamic VBO: 7 floats/vertex (x, y, u, v, layer, z_ndc, darken)
    GpuGeometryBufferDesc geom_desc;
    geom_desc.vertex_stride = (uint32_t)sizeof(ShadowGLVertex);
    geom_desc.attribs = {
        { 0, 2, GpuVertexAttribType::Float, 0 },
        { 1, 2, GpuVertexAttribType::Float, 2 * (uint32_t)sizeof(float) },
        { 2, 1, GpuVertexAttribType::Float, 4 * (uint32_t)sizeof(float) },
        { 3, 1, GpuVertexAttribType::Float, 5 * (uint32_t)sizeof(float) },
        { 4, 1, GpuVertexAttribType::Float, 6 * (uint32_t)sizeof(float) },
    };
    geom_desc.dynamic = true;
    geom_desc.debug_name = "shadow_geom";
    m_shadow_geom_handle = m_resource_mapper->RequestCreateGeometryBuffer(geom_desc);

    return m_resource_mapper->ResolveGeometryBuffer(m_shadow_geom_handle) != nullptr;
}

void GLWorldViewRenderer::free_shadow_resources()
{
    m_shadow_cmds.clear();
    m_rt_shadow_cmds.clear();
}

/******************************************************************************/
// Possession lens
/******************************************************************************/

bool GLWorldViewRenderer::init_lens_shaders()
{
    if (!m_resource_mapper) return false;

    // Shared unit quad: two triangles covering NDC [-1,1], UV [0,1]. Drawn
    // with glViewport already set to the destination sub-rect (see
    // ResolveLensComposite()), so this one static buffer serves all three
    // passes regardless of where on screen the viewport actually sits.
    static const float k_quad[] = {
        //  x,     y,    u,    v
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f,
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f,
        -1.0f,  1.0f,  0.0f, 1.0f,
    };
    {
        GpuGeometryBufferDesc geom_desc;
        geom_desc.vertex_stride = 4 * (uint32_t)sizeof(float);
        geom_desc.attribs = {
            { 0, 2, GpuVertexAttribType::Float, 0 },
            { 1, 2, GpuVertexAttribType::Float, 2 * (uint32_t)sizeof(float) },
        };
        geom_desc.dynamic = false; // static unit quad, content never changes
        geom_desc.initial_vertex_capacity = sizeof(k_quad);
        geom_desc.debug_name = "lens_quad";
        m_lens_quad_geom_handle = m_resource_mapper->RequestCreateGeometryBuffer(geom_desc);
        const GLGeometryBuffer* geom = m_resource_mapper->ResolveGeometryBuffer(m_lens_quad_geom_handle);
        if (!geom) return false;
        glBindBuffer(GL_ARRAY_BUFFER, geom->vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(k_quad), k_quad);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    bool any_ok = false;

    // Mist
    {
        GpuProgramDesc desc;
        desc.vertex_src = LENS_COMPOSITE_VERTEX_SHADER;
        desc.fragment_src = LENS_MIST_FRAGMENT_SHADER;
        desc.debug_name = "lens_mist";
        m_lens_shader_mist_handle = m_resource_mapper->RequestCreateProgram(desc);
        const GLProgram* prog = m_resource_mapper->ResolveProgram(m_lens_shader_mist_handle);
        if (prog)
        {
            glUseProgram(prog->id);
            glUniform1i(glGetUniformLocation(prog->id, "u_scene"), 0);
            glUniform1i(glGetUniformLocation(prog->id, "u_mist"), 1);
            glUniform1i(glGetUniformLocation(prog->id, "u_palette"), 2);
            glUniform1i(glGetUniformLocation(prog->id, "u_index_lookup"), 3);
            glUniform1i(glGetUniformLocation(prog->id, "u_fade_table"), 4);
            m_lens_mist_loc_src_x     = glGetUniformLocation(prog->id, "u_src_x");
            m_lens_mist_loc_view_size = glGetUniformLocation(prog->id, "u_view_size");
            m_lens_mist_loc_pos       = glGetUniformLocation(prog->id, "u_pos");
            m_lens_mist_loc_sec       = glGetUniformLocation(prog->id, "u_sec");
            m_lens_mist_loc_lightness = glGetUniformLocation(prog->id, "u_lightness");
            glUseProgram(0);
            any_ok = true;
        }
        else
        {
            WARNLOG("GLWorldViewRenderer: lens mist shader unavailable");
        }
    }

    // Remap (displacement/flyeye -- same table layout, same shader)
    {
        GpuProgramDesc desc;
        desc.vertex_src = LENS_COMPOSITE_VERTEX_SHADER;
        desc.fragment_src = LENS_REMAP_FRAGMENT_SHADER;
        desc.debug_name = "lens_remap";
        m_lens_shader_remap_handle = m_resource_mapper->RequestCreateProgram(desc);
        const GLProgram* prog = m_resource_mapper->ResolveProgram(m_lens_shader_remap_handle);
        if (prog)
        {
            glUseProgram(prog->id);
            glUniform1i(glGetUniformLocation(prog->id, "u_scene"), 0);
            glUniform1i(glGetUniformLocation(prog->id, "u_remap"), 1);
            m_lens_remap_loc_src_x     = glGetUniformLocation(prog->id, "u_src_x");
            m_lens_remap_loc_view_size = glGetUniformLocation(prog->id, "u_view_size");
            glUseProgram(0);
            any_ok = true;
        }
        else
        {
            WARNLOG("GLWorldViewRenderer: lens remap shader unavailable");
        }
    }

    // Overlay
    {
        GpuProgramDesc desc;
        desc.vertex_src = LENS_COMPOSITE_VERTEX_SHADER;
        desc.fragment_src = LENS_OVERLAY_FRAGMENT_SHADER;
        desc.debug_name = "lens_overlay";
        m_lens_shader_overlay_handle = m_resource_mapper->RequestCreateProgram(desc);
        const GLProgram* prog = m_resource_mapper->ResolveProgram(m_lens_shader_overlay_handle);
        if (prog)
        {
            glUseProgram(prog->id);
            glUniform1i(glGetUniformLocation(prog->id, "u_scene"), 0);
            glUniform1i(glGetUniformLocation(prog->id, "u_overlay"), 1);
            glUniform1i(glGetUniformLocation(prog->id, "u_palette"), 2);
            glUniform1i(glGetUniformLocation(prog->id, "u_index_lookup"), 3);
            m_lens_overlay_loc_src_x     = glGetUniformLocation(prog->id, "u_src_x");
            m_lens_overlay_loc_view_size = glGetUniformLocation(prog->id, "u_view_size");
            m_lens_overlay_loc_alpha     = glGetUniformLocation(prog->id, "u_alpha");
            glUseProgram(0);
            any_ok = true;
        }
        else
        {
            WARNLOG("GLWorldViewRenderer: lens overlay shader unavailable");
        }
    }

    // Copy (no pixel effect, or the effect's resources are unavailable)
    {
        GpuProgramDesc desc;
        desc.vertex_src = LENS_COMPOSITE_VERTEX_SHADER;
        desc.fragment_src = LENS_COPY_FRAGMENT_SHADER;
        desc.debug_name = "lens_copy";
        m_lens_shader_copy_handle = m_resource_mapper->RequestCreateProgram(desc);
        const GLProgram* prog = m_resource_mapper->ResolveProgram(m_lens_shader_copy_handle);
        if (prog)
        {
            glUseProgram(prog->id);
            glUniform1i(glGetUniformLocation(prog->id, "u_scene"), 0);
            m_lens_copy_loc_src_x     = glGetUniformLocation(prog->id, "u_src_x");
            m_lens_copy_loc_view_size = glGetUniformLocation(prog->id, "u_view_size");
            glUseProgram(0);
            any_ok = true;
        }
        else
        {
            WARNLOG("GLWorldViewRenderer: lens copy shader unavailable");
        }
    }

    return any_ok;
}

void GLWorldViewRenderer::free_lens_resources()
{
    m_lens_mist_uploaded_version = m_lens_remap_uploaded_version = m_lens_overlay_uploaded_version = 0xFFFFFFFFu;
    m_lens_scene_gt_w = m_lens_scene_gt_h = 0;
    m_lens_remap_gt_w = m_lens_remap_gt_h = 0;
    m_lens_overlay_gt_w = m_lens_overlay_gt_h = 0;

    m_lens_cmd    = IRWorldLensCmd{};
    m_rt_lens_cmd = IRWorldLensCmd{};
}

void GLWorldViewRenderer::SubmitPossessionLens(const IRWorldLensCmd& cmd)
{
    ASSERT_GAME_THREAD();
    m_lens_cmd = cmd;

    EnsureLensSceneRT(RendererScreenWidth(), RendererScreenHeight());

    if (cmd.type == LensPixelEffectType::Displacement || cmd.type == LensPixelEffectType::Flyeye)
        EnsureLensRemapTexture(cmd.remap_w, cmd.remap_h);
    else if (cmd.type == LensPixelEffectType::Overlay)
        EnsureLensOverlayTexture(cmd.overlay_w, cmd.overlay_h);
}

void GLWorldViewRenderer::EnsureLensSceneRT(int w, int h)
{
    if (!m_resource_mapper || w <= 0 || h <= 0)
        return;
    if (m_lens_scene_rt_handle != kInvalidGpuResource && m_lens_scene_gt_w == w && m_lens_scene_gt_h == h)
        return;

    GpuRenderTargetDesc desc;
    desc.width = w;
    desc.height = h;
    // Depth24Stencil8, not the original's depth-only GL_DEPTH_COMPONENT24
    // renderbuffer -- GpuTextureFormat has no depth-only option and
    // RealizeRenderTarget only supports texture attachments (not
    // renderbuffers). Functionally equivalent for this use (nothing reads
    // the depth buffer as a texture; GL_DEPTH_TEST is all that's needed),
    // just an unused stencil channel.
    desc.attachments = {
        { GpuTextureFormat::RGBA8, false },
        { GpuTextureFormat::Depth24Stencil8, true },
    };
    desc.debug_name = "lens_scene";

    if (m_lens_scene_rt_handle == kInvalidGpuResource)
        m_lens_scene_rt_handle = m_resource_mapper->RequestCreateRenderTarget(desc);
    else
        m_lens_scene_rt_handle = m_resource_mapper->RequestReloadRenderTarget(m_lens_scene_rt_handle, desc);

    m_lens_scene_gt_w = w;
    m_lens_scene_gt_h = h;
}

void GLWorldViewRenderer::EnsureLensRemapTexture(int w, int h)
{
    if (!m_resource_mapper || w <= 0 || h <= 0)
        return;
    if (m_lens_remap_tex_handle != kInvalidGpuResource && m_lens_remap_gt_w == w && m_lens_remap_gt_h == h)
        return;

    GpuTextureDesc desc;
    desc.width = w;
    desc.height = h;
    desc.format = GpuTextureFormat::RG16UI;
    desc.min_filter = GpuTextureFilter::Nearest;
    desc.mag_filter = GpuTextureFilter::Nearest;
    desc.wrap = GpuTextureWrap::Clamp;
    desc.debug_name = "lens_remap";

    if (m_lens_remap_tex_handle == kInvalidGpuResource)
        m_lens_remap_tex_handle = m_resource_mapper->RequestCreateTexture(desc);
    else
        m_lens_remap_tex_handle = m_resource_mapper->RequestReloadTexture(m_lens_remap_tex_handle, desc);

    m_lens_remap_gt_w = w;
    m_lens_remap_gt_h = h;
}

void GLWorldViewRenderer::EnsureLensOverlayTexture(int w, int h)
{
    if (!m_resource_mapper || w <= 0 || h <= 0)
        return;
    if (m_lens_overlay_tex_handle != kInvalidGpuResource && m_lens_overlay_gt_w == w && m_lens_overlay_gt_h == h)
        return;

    GpuTextureDesc desc;
    desc.width = w;
    desc.height = h;
    desc.format = GpuTextureFormat::R8;
    // GL_NEAREST matches COverlayRenderer::Render()'s own "nearest-neighbor
    // sampling" comment.
    desc.min_filter = GpuTextureFilter::Nearest;
    desc.mag_filter = GpuTextureFilter::Nearest;
    desc.wrap = GpuTextureWrap::Clamp;
    desc.debug_name = "lens_overlay";

    if (m_lens_overlay_tex_handle == kInvalidGpuResource)
        m_lens_overlay_tex_handle = m_resource_mapper->RequestCreateTexture(desc);
    else
        m_lens_overlay_tex_handle = m_resource_mapper->RequestReloadTexture(m_lens_overlay_tex_handle, desc);

    m_lens_overlay_gt_w = w;
    m_lens_overlay_gt_h = h;
}

void GLWorldViewRenderer::upload_lens_textures_if_dirty()
{
    if (!m_resource_mapper) return;

    switch (m_rt_lens_cmd.type)
    {
    case LensPixelEffectType::Mist:
        if (m_rt_lens_cmd.mist_pixels.size() != 256 * 256)
            break;
        if (m_lens_mist_tex_handle == kInvalidGpuResource)
        {
            GpuTextureDesc desc;
            desc.width = 256;
            desc.height = 256;
            desc.format = GpuTextureFormat::R8;
            desc.min_filter = GpuTextureFilter::Nearest;
            desc.mag_filter = GpuTextureFilter::Nearest;
            desc.wrap = GpuTextureWrap::Clamp;
            desc.debug_name = "lens_mist";
            m_lens_mist_tex_handle = m_resource_mapper->RequestCreateTexture(desc);
            m_lens_mist_uploaded_version = 0xFFFFFFFFu;
        }
        if (m_rt_lens_cmd.mist_version != m_lens_mist_uploaded_version)
        {
            const GLTexture* tex = m_resource_mapper->ResolveTexture(m_lens_mist_tex_handle);
            if (tex)
            {
                glBindTexture(GL_TEXTURE_2D, tex->id);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_RED, GL_UNSIGNED_BYTE, m_rt_lens_cmd.mist_pixels.data());
                if (capture_tight_unpack(256))
                    m_capture_mist_upload.Record(m_lens_mist_tex_handle, tex->id, 256, 256,
                        m_rt_lens_cmd.mist_pixels.data(), m_rt_lens_cmd.mist_pixels.size());
                else m_capture_mist_upload.bytes.clear();
                glBindTexture(GL_TEXTURE_2D, 0);
                m_lens_mist_uploaded_version = m_rt_lens_cmd.mist_version;
            }
        }
        break;

    case LensPixelEffectType::Displacement:
    case LensPixelEffectType::Flyeye:
        if (m_rt_lens_cmd.remap_w <= 0 || m_rt_lens_cmd.remap_h <= 0 ||
            (size_t)(m_rt_lens_cmd.remap_w * m_rt_lens_cmd.remap_h * 2) != m_rt_lens_cmd.remap_pixels.size())
            break;
        if (m_rt_lens_cmd.remap_version != m_lens_remap_uploaded_version)
        {
            const GLTexture* tex = m_resource_mapper->ResolveTexture(m_lens_remap_tex_handle);
            if (tex)
            {
                glBindTexture(GL_TEXTURE_2D, tex->id);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_rt_lens_cmd.remap_w, m_rt_lens_cmd.remap_h,
                                GL_RG_INTEGER, GL_UNSIGNED_SHORT, m_rt_lens_cmd.remap_pixels.data());
                if (capture_tight_unpack(unsigned(m_rt_lens_cmd.remap_w) * 4))
                    m_capture_remap_upload.Record(m_lens_remap_tex_handle, tex->id, tex->width, tex->height,
                        m_rt_lens_cmd.remap_pixels.data(), m_rt_lens_cmd.remap_pixels.size() * sizeof(int16_t));
                else m_capture_remap_upload.bytes.clear();
                glBindTexture(GL_TEXTURE_2D, 0);
                m_lens_remap_uploaded_version = m_rt_lens_cmd.remap_version;
            }
        }
        break;

    case LensPixelEffectType::Overlay:
        // Sizing already settled on the game thread (EnsureLensOverlayTexture(),
        // called from SubmitPossessionLens()) -- this only uploads content.
        if (m_rt_lens_cmd.overlay_w <= 0 || m_rt_lens_cmd.overlay_h <= 0 ||
            (size_t)(m_rt_lens_cmd.overlay_w * m_rt_lens_cmd.overlay_h) != m_rt_lens_cmd.overlay_pixels.size())
            break;
        if (m_rt_lens_cmd.overlay_version != m_lens_overlay_uploaded_version)
        {
            const GLTexture* tex = m_resource_mapper->ResolveTexture(m_lens_overlay_tex_handle);
            if (tex)
            {
                glBindTexture(GL_TEXTURE_2D, tex->id);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_rt_lens_cmd.overlay_w, m_rt_lens_cmd.overlay_h,
                                GL_RED, GL_UNSIGNED_BYTE, m_rt_lens_cmd.overlay_pixels.data());
                if (capture_tight_unpack(unsigned(m_rt_lens_cmd.overlay_w)))
                    m_capture_overlay_upload.Record(m_lens_overlay_tex_handle, tex->id, tex->width, tex->height,
                        m_rt_lens_cmd.overlay_pixels.data(), m_rt_lens_cmd.overlay_pixels.size());
                else m_capture_overlay_upload.bytes.clear();
                glBindTexture(GL_TEXTURE_2D, 0);
                m_lens_overlay_uploaded_version = m_rt_lens_cmd.overlay_version;
            }
        }
        break;

    default:
        break;
    }
}

bool GLWorldViewRenderer::BeginLensCapture(const float* clear_rgba)
{
    ASSERT_RENDER_THREAD();

    if (!m_rt_lens_cmd.active)
        return false;
    if (!m_resource_mapper)
        return false;

    GpuResourceHandle shader_handle = kInvalidGpuResource;
    switch (m_rt_lens_cmd.type)
    {
    case LensPixelEffectType::Mist:         shader_handle = m_lens_shader_mist_handle;    break;
    case LensPixelEffectType::Displacement:
    case LensPixelEffectType::Flyeye:       shader_handle = m_lens_shader_remap_handle;   break;
    case LensPixelEffectType::Overlay:      shader_handle = m_lens_shader_overlay_handle; break;
    default: break;
    }
    if (ResolveShaderId(shader_handle) == 0 && ResolveShaderId(m_lens_shader_copy_handle) == 0)
        return false;

    const GLRenderTarget* rt = m_resource_mapper->ResolveRenderTarget(m_lens_scene_rt_handle);
    if (!rt)
        return false;

    glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
    if (m_rt_outline_capture && (!capture_clear_state() || rt->width != m_full_screen_w || rt->height != m_full_screen_h))
        WorldSceneFailOutlineCapture(m_rt_outline_capture, "Lens source clear state or native extent is not proved");
    glClearColor(clear_rgba[0], clear_rgba[1], clear_rgba[2], clear_rgba[3]);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    RecordWorldTargetForCapture(m_full_screen_w, m_full_screen_h,
        kfx::render::SceneGLWorldTargetKind::LensSource, clear_rgba);
    if (m_rt_outline_capture) m_capture_lens.capture_began = true;
    return true;
}

void GLWorldViewRenderer::ResolveLensComposite()
{
    ASSERT_RENDER_THREAD();
    if (!m_resource_mapper) return;

    glBindFramebuffer(GL_FRAMEBUFFER, m_resource_mapper->GetScreenFramebuffer());

    upload_lens_textures_if_dirty();

    const GLRenderTarget* scene_rt = m_resource_mapper->ResolveRenderTarget(m_lens_scene_rt_handle);
    const GLGeometryBuffer* quad_geom = m_resource_mapper->ResolveGeometryBuffer(m_lens_quad_geom_handle);
    if (!scene_rt || scene_rt->color_attachments.empty() || !quad_geom)
        return;

    if (m_rt_lens_cmd.viewport_w <= 0 || m_rt_lens_cmd.viewport_h <= 0)
        return;
    const float vp_x = (float)m_rt_lens_cmd.viewport_x;
    const float vp_w = (float)m_rt_lens_cmd.viewport_w;
    const float vp_h = (float)m_rt_lens_cmd.viewport_h;

    const int dest_vp_y_gl = m_full_screen_h - m_rt_lens_cmd.viewport_y - m_rt_lens_cmd.viewport_h;
    glViewport(m_rt_lens_cmd.viewport_x, dest_vp_y_gl, m_rt_lens_cmd.viewport_w, m_rt_lens_cmd.viewport_h);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    const GLTexture* index_tex = m_resource_mapper->ResolveTexture(m_palette_index_tex_handle);
    const GLuint palette_tex_id = ResolvePaletteTexId();
    const GLuint fade_tex_id = ResolveFadeTexId();
    const bool have_index_tables = (index_tex != nullptr) && (palette_tex_id != 0) && (fade_tex_id != 0);

    glBindVertexArray(quad_geom->vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_rt->color_attachments[0]);

    bool drawn = false;
    switch (m_rt_lens_cmd.type)
    {
    case LensPixelEffectType::Mist:
    {
        const GLProgram* prog = m_resource_mapper->ResolveProgram(m_lens_shader_mist_handle);
        const GLTexture* mist_tex = m_resource_mapper->ResolveTexture(m_lens_mist_tex_handle);
        if (prog && mist_tex && have_index_tables)
        {
            drawn = true;
            glUseProgram(prog->id);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, mist_tex->id);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, palette_tex_id);
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, index_tex->id);
            glActiveTexture(GL_TEXTURE4);
            glBindTexture(GL_TEXTURE_2D, fade_tex_id);
            glUniform1f(m_lens_mist_loc_src_x, vp_x);
            glUniform2f(m_lens_mist_loc_view_size, vp_w, vp_h);
            glUniform2f(m_lens_mist_loc_pos, m_rt_lens_cmd.mist_pos_x, m_rt_lens_cmd.mist_pos_y);
            glUniform2f(m_lens_mist_loc_sec, m_rt_lens_cmd.mist_sec_x, m_rt_lens_cmd.mist_sec_y);
            glUniform1f(m_lens_mist_loc_lightness, (float)m_rt_lens_cmd.mist_lightness);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            record_lens_operation(kfx::render::SceneGLLensOperation::Mist);
        }
        break;
    }

    case LensPixelEffectType::Displacement:
    case LensPixelEffectType::Flyeye:
    {
        const GLProgram* prog = m_resource_mapper->ResolveProgram(m_lens_shader_remap_handle);
        const GLTexture* remap_tex = m_resource_mapper->ResolveTexture(m_lens_remap_tex_handle);
        if (prog && remap_tex)
        {
            drawn = true;
            glUseProgram(prog->id);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, remap_tex->id);
            glUniform1f(m_lens_remap_loc_src_x, vp_x);
            glUniform2f(m_lens_remap_loc_view_size, vp_w, vp_h);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            record_lens_operation(kfx::render::SceneGLLensOperation::Remap);
        }
        break;
    }

    case LensPixelEffectType::Overlay:
    {
        const GLProgram* prog = m_resource_mapper->ResolveProgram(m_lens_shader_overlay_handle);
        const GLTexture* overlay_tex = m_resource_mapper->ResolveTexture(m_lens_overlay_tex_handle);
        if (prog && overlay_tex && have_index_tables)
        {
            drawn = true;
            glUseProgram(prog->id);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, overlay_tex->id);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, palette_tex_id);
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, index_tex->id);
            glUniform1f(m_lens_overlay_loc_src_x, vp_x);
            glUniform2f(m_lens_overlay_loc_view_size, vp_w, vp_h);
            glUniform1i(m_lens_overlay_loc_alpha, (GLint)(m_rt_lens_cmd.overlay_alpha * 256.0f + 0.5f));
            glDrawArrays(GL_TRIANGLES, 0, 6);
            record_lens_operation(kfx::render::SceneGLLensOperation::Overlay);
        }
        break;
    }

    default:
        break;
    }

    if (!drawn)
    {
        const GLProgram* prog = m_resource_mapper->ResolveProgram(m_lens_shader_copy_handle);
        if (prog)
        {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, scene_rt->color_attachments[0]);
            glUseProgram(prog->id);
            glUniform1f(m_lens_copy_loc_src_x, vp_x);
            glUniform2f(m_lens_copy_loc_view_size, vp_w, vp_h);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            record_lens_operation(kfx::render::SceneGLLensOperation::Copy);
        }
    }

    glUseProgram(0);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDepthMask(GL_TRUE);
    // Everything drawn after the lens (UI, overlays) expects the full screen.
    glViewport(0, 0, m_full_screen_w, m_full_screen_h);
}

void GLWorldViewRenderer::append_shadow_quad(const struct BucketKindCreatureShadow* sh,
                                             const kfx::render::WorldShadowSourceView& source)
{
    ShadowSubmission cmd;
    static_cast<kfx::render::WorldShadowGeometry&>(cmd) = source.geometry;
    cmd.data_bytes = source.bytes.size();
    cmd.capture_entry = WorldSceneGLShadowTag(sh->caster_slot, cmd.capture_caster);
    cmd.draw_idx = source.draw_idx;
    cmd.data     = source.bytes.data();
    cmd.src_w    = source.src_w;
    cmd.src_h    = source.src_h;
    m_shadow_cmds.push_back(cmd);
}

void GLWorldViewRenderer::draw_shadows_gpu()
{
    if (m_rt_shadow_cmds.empty() || !m_resource_mapper)
        return;
    const GLProgram* const shadow_prog = m_resource_mapper->ResolveProgram(m_shadow_shader_handle);
    const GLGeometryBuffer* const shadow_geom = m_resource_mapper->ResolveGeometryBuffer(m_shadow_geom_handle);
    if (!shadow_prog || !shadow_geom) {
        WorldSceneFailOutlineCapture(m_rt_outline_capture, "Complete GL shadow program or geometry unavailable");
        return;
    }

    std::vector<ShadowGLVertex>& s_verts = s_shadow_verts;
    s_verts.clear();
    s_verts.reserve(m_rt_shadow_cmds.size() * 6);

    for (const auto& cmd : m_rt_shadow_cmds)
    {
        const int coverage_factor = keeper_shadow_coverage_factor(cmd.src_w, cmd.src_h, cmd.u, cmd.v);
        if (!coverage_factor) {
            WorldSceneFailOutlineCapture(m_rt_outline_capture, "Invalid GL shadow coverage dimensions or UV bounds");
            continue;
        }
        const int coverage_w = (cmd.src_w + coverage_factor - 1) / coverage_factor;
        const int coverage_h = (cmd.src_h + coverage_factor - 1) / coverage_factor;
        const int layer = resolve_atlas_layer(cmd.draw_idx, cmd.data, cmd.data_bytes, cmd.src_w, cmd.src_h, nullptr, coverage_factor);
        if (layer < 0) {
            WorldSceneFailOutlineCapture(m_rt_outline_capture, "Complete GL shadow atlas resolution dropped a quad");
            continue;  // atlas full/unsupported -- shadow silently dropped, matches
                       // the sprite pass's own "no CPU fallback for this beat" scope
        }
        if (m_rt_outline_capture) {
            if (cmd.capture_entry == kfx::render::SceneOutlineNoDraw || size_t(layer) >= m_capture_sprite_coverage.size()
                || m_capture_sprite_coverage[layer].width != coverage_w || m_capture_sprite_coverage[layer].height != coverage_h
                || m_capture_sprite_coverage[layer].bits.size() != size_t(k_kspr_atlas_dim) * k_kspr_atlas_dim / 8)
                WorldSceneFailOutlineCapture(m_rt_outline_capture, "Complete GL shadow caster or actual source upload proof unavailable");
            else try {
                const auto& uploaded = m_capture_sprite_coverage[layer];
                kfx::render::SceneGLShadowSource source;
                // The atlas upload initializes the entire layer. Shadow frame
                // bounds can exceed the decoded artwork's logical dimensions.
                source.width = k_kspr_atlas_dim; source.height = k_kspr_atlas_dim;
                source.coverage.resize(size_t(source.width) * source.height);
                for (size_t i = 0; i < source.coverage.size(); ++i)
                    source.coverage[i] = (uploaded.bits[i / 8] & (1u << (i % 8))) ? 255 : 0;
                kfx::render::SceneGLShadow shadow;
                shadow.entry = cmd.capture_entry; shadow.caster = cmd.capture_caster;
                shadow.z_ndc = cmd.z_ndc; shadow.darken = cmd.darken;
                for (unsigned i = 0; i < 4; ++i) {
                    shadow.vertices[i] = {cmd.x[i], cmd.y[i], keeper_shadow_coverage_uv(cmd.u[i], coverage_factor), keeper_shadow_coverage_uv(cmd.v[i], coverage_factor)};
                    if (!std::isfinite(cmd.x[i]) || !std::isfinite(cmd.y[i])
                        || std::abs(cmd.x[i]) > 16777216 || std::abs(cmd.y[i]) > 16777216
                        || !std::isfinite(cmd.u[i]) || !std::isfinite(cmd.v[i])
                        || cmd.u[i] < 0 || cmd.v[i] < 0
                        || keeper_shadow_coverage_uv(cmd.u[i], coverage_factor) > float(source.width - 1) / 256
                        || keeper_shadow_coverage_uv(cmd.v[i], coverage_factor) > float(source.height - 1) / 256)
                        ERRORLOG("GL shadow capture bounds: entry=%u caster=%u draw=%d layer=%d source=%ux%u initialized=%ux512 vertex=%u xy=(%.9g,%.9g) uv=(%.9g,%.9g) texel=(%.9g,%.9g)",
                            cmd.capture_entry, unsigned(cmd.capture_caster.slot), cmd.draw_idx, layer,
                            unsigned(uploaded.width), unsigned(uploaded.height), unsigned(source.width), i,
                            double(cmd.x[i]), double(cmd.y[i]), double(cmd.u[i]), double(cmd.v[i]),
                            double(cmd.u[i]) * 256, double(cmd.v[i]) * 256);
                }
                WorldSceneAppendGLShadow(m_rt_outline_capture, shadow, source);
            } catch (...) { WorldSceneFailOutlineCapture(m_rt_outline_capture, "GL shadow source allocation failed"); }
        }

        const int tri[6] = { 0, 1, 2, 0, 2, 3 };
        for (int t : tri)
        {
            ShadowGLVertex gv;
            gv.x = cmd.x[t];
            gv.y = cmd.y[t];
            gv.u = keeper_shadow_coverage_uv(cmd.u[t], coverage_factor);
            gv.v = keeper_shadow_coverage_uv(cmd.v[t], coverage_factor);
            gv.layer  = (float)layer;
            gv.z_ndc  = cmd.z_ndc;
            gv.darken = cmd.darken;
            s_verts.push_back(gv);
        }
    }
    if (s_verts.empty())
        return;

    glBindBuffer(GL_ARRAY_BUFFER, shadow_geom->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(s_verts.size() * sizeof(ShadowGLVertex)),
                 s_verts.data(), GL_STREAM_DRAW);

    glUseProgram(shadow_prog->id);
    glBindVertexArray(shadow_geom->vao);
    glUniform2f(m_shadow_loc_viewport, (float)m_draw_screen_w, (float)m_draw_screen_h);

    const GLTexture* sprite_array_tex = m_resource_mapper->ResolveTexture(m_kspr_sprite_array_handle);
    if (!sprite_array_tex) WorldSceneFailOutlineCapture(m_rt_outline_capture, "Complete GL shadow texture unavailable");
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, sprite_array_tex ? sprite_array_tex->id : 0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ZERO, GL_SRC_COLOR);
    glDepthMask(GL_FALSE);  // shadows test against tile depth but don't occlude each other

    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)s_verts.size());

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

/******************************************************************************/
// Keeper sprites
namespace {
/** Mechanical helper for init_keeper_sprite_shader()'s many "create program,
 *  resolve, cache uniform locations if it linked" blocks -- returns the
 *  resolved GLProgram* (nullptr on failure), so the caller's own
 *  uniform-caching code (which differs per shader) stays inline. */
const GLProgram* CreateKsprProgram(GLResourceMapper* mapper, GpuResourceHandle& handle,
                                   const char* vs_src, const char* fs_src, const char* debug_name)
{
    GpuProgramDesc desc;
    desc.vertex_src = vs_src;
    desc.fragment_src = fs_src;
    desc.debug_name = debug_name;
    handle = mapper->RequestCreateProgram(desc);
    return mapper->ResolveProgram(handle);
}
} // namespace

bool GLWorldViewRenderer::init_keeper_sprite_shader()
{
    if (!m_resource_mapper) return false;

    // Non-instanced fallback: single-texture, palette+CLUT shader.
    const GLProgram* kspr_prog = CreateKsprProgram(m_resource_mapper, m_kspr_shader_handle,
        KSPR_VERTEX_SHADER, KSPR_FRAGMENT_SHADER, "kspr");
    if (!kspr_prog)
    {
        ERRORLOG("GLWorldViewRenderer: keeper-sprite shader link error");
        return false;
    }
    glUseProgram(kspr_prog->id);
    m_kspr_loc_viewport = glGetUniformLocation(kspr_prog->id, "u_viewport");
    m_kspr_loc_sprite   = glGetUniformLocation(kspr_prog->id, "u_sprite");
    m_kspr_loc_palette  = glGetUniformLocation(kspr_prog->id, "u_palette");
    m_kspr_loc_alpha    = glGetUniformLocation(kspr_prog->id, "u_alpha");
    m_kspr_loc_z_ndc    = glGetUniformLocation(kspr_prog->id, "u_z_ndc");
    glUniform1i(m_kspr_loc_sprite,  0);  // GL_TEXTURE0
    glUniform1i(m_kspr_loc_palette, 1);  // GL_TEXTURE1
    glUseProgram(0);

    // Additive-glow variant
    {
        const GLProgram* glow_prog = CreateKsprProgram(m_resource_mapper, m_kspr_glow_shader_handle,
            KSPR_VERTEX_SHADER, KSPR_GLOW_FRAGMENT_SHADER, "kspr_glow");
        if (glow_prog)
        {
            glUseProgram(glow_prog->id);
            m_kspr_glow_loc_viewport = glGetUniformLocation(glow_prog->id, "u_viewport");
            m_kspr_glow_loc_sprite   = glGetUniformLocation(glow_prog->id, "u_sprite");
            m_kspr_glow_loc_z_ndc    = glGetUniformLocation(glow_prog->id, "u_z_ndc");
            m_kspr_glow_loc_palette_xform = glGetUniformLocation(glow_prog->id, "u_palette_xform");
            m_kspr_glow_loc_darken   = glGetUniformLocation(glow_prog->id, "u_darken");
            glUniform1i(m_kspr_glow_loc_sprite, 0);  // GL_TEXTURE0
            glUseProgram(0);
        }
        else
        {
            WARNLOG("GLWorldViewRenderer: keeper-sprite glow shader unavailable");
        }
    }

    // Reusable 1024x1024 RG8 fallback for oversized held frames. Zero-initialised
    // so unwritten regions sample as zero coverage (transparent) rather than
    // undefined garbage.
    {
        static const uint8_t s_zero_keeper[k_kspr_decode_dim * k_kspr_decode_dim * 2] = {};
        GpuTextureDesc desc;
        desc.width = k_kspr_decode_dim;
        desc.height = k_kspr_decode_dim;
        desc.format = GpuTextureFormat::RG8;
        desc.min_filter = GpuTextureFilter::Nearest;
        desc.mag_filter = GpuTextureFilter::Nearest;
        desc.wrap = GpuTextureWrap::Clamp;
        desc.initial_pixels.assign(s_zero_keeper, s_zero_keeper + sizeof(s_zero_keeper));
        desc.debug_name = "kspr_sprite";
        m_kspr_sprite_tex_handle = m_resource_mapper->RequestCreateTexture(desc);
        if (m_resource_mapper->ResolveTexture(m_kspr_sprite_tex_handle) == nullptr)
        {
            ERRORLOG("GLWorldViewRenderer: keeper-sprite texture realization failed");
            return false;
        }
    }

    // VAO + VBO: 6 vertices x (pos, local px, map, src, flip) = 11 floats each
    {
        GpuGeometryBufferDesc desc;
        desc.vertex_stride = 11 * (uint32_t)sizeof(float);
        desc.attribs = {
            { 0, 2, GpuVertexAttribType::Float, 0 },
            { 1, 2, GpuVertexAttribType::Float, 2 * (uint32_t)sizeof(float) },
            { 2, 4, GpuVertexAttribType::Float, 4 * (uint32_t)sizeof(float) },
            { 3, 2, GpuVertexAttribType::Float, 8 * (uint32_t)sizeof(float) },
            { 4, 1, GpuVertexAttribType::Float, 10 * (uint32_t)sizeof(float) },
        };
        desc.dynamic = true;
        desc.initial_vertex_capacity = 6 * 11 * sizeof(float);
        desc.debug_name = "kspr_geom";
        m_kspr_geom_handle = m_resource_mapper->RequestCreateGeometryBuffer(desc);
        if (m_resource_mapper->ResolveGeometryBuffer(m_kspr_geom_handle) == nullptr)
        {
            ERRORLOG("GLWorldViewRenderer: keeper-sprite geometry buffer realization failed");
            return false;
        }
    }

    // Attempt to build the sprite decode atlas (GL_TEXTURE_2D_ARRAY).
    do {
        const GLProgram* atlas_prog = CreateKsprProgram(m_resource_mapper, m_kspr_atlas_shader_handle,
            KSPR_VERTEX_SHADER, KSPR_ARRAY_FRAGMENT_SHADER, "kspr_array");
        if (!atlas_prog)
        {
            WARNLOG("kspr_array shader link failed");
            break;
        }
        glUseProgram(atlas_prog->id);
        m_kspr_atlas_loc_viewport = glGetUniformLocation(atlas_prog->id, "u_viewport");
        m_kspr_atlas_loc_sprite   = glGetUniformLocation(atlas_prog->id, "u_sprite");
        m_kspr_atlas_loc_clut     = glGetUniformLocation(atlas_prog->id, "u_clut");
        m_kspr_atlas_loc_alpha    = glGetUniformLocation(atlas_prog->id, "u_alpha");
        m_kspr_atlas_loc_z_ndc    = glGetUniformLocation(atlas_prog->id, "u_z_ndc");
        m_kspr_atlas_loc_layer    = glGetUniformLocation(atlas_prog->id, "u_layer");
        m_kspr_atlas_loc_clut_v   = glGetUniformLocation(atlas_prog->id, "u_clut_v");
        glUniform1i(m_kspr_atlas_loc_sprite, 0);  // GL_TEXTURE0
        glUniform1i(m_kspr_atlas_loc_clut,   1);  // GL_TEXTURE1
        glUseProgram(0);

        // Additive-glow variant of the atlas shader
        {
            const GLProgram* atlas_glow_prog = CreateKsprProgram(m_resource_mapper, m_kspr_atlas_glow_shader_handle,
                KSPR_VERTEX_SHADER, KSPR_ARRAY_GLOW_FRAGMENT_SHADER, "kspr_array_glow");
            if (atlas_glow_prog)
            {
                glUseProgram(atlas_glow_prog->id);
                m_kspr_atlas_glow_loc_viewport = glGetUniformLocation(atlas_glow_prog->id, "u_viewport");
                m_kspr_atlas_glow_loc_sprite   = glGetUniformLocation(atlas_glow_prog->id, "u_sprite");
                m_kspr_atlas_glow_loc_z_ndc    = glGetUniformLocation(atlas_glow_prog->id, "u_z_ndc");
                m_kspr_atlas_glow_loc_layer    = glGetUniformLocation(atlas_glow_prog->id, "u_layer");
                m_kspr_atlas_glow_loc_palette_xform = glGetUniformLocation(atlas_glow_prog->id, "u_palette_xform");
                m_kspr_atlas_glow_loc_darken   = glGetUniformLocation(atlas_glow_prog->id, "u_darken");
                glUniform1i(m_kspr_atlas_glow_loc_sprite, 0);  // GL_TEXTURE0
                glUseProgram(0);
            }
            else
            {
                WARNLOG("GLWorldViewRenderer: failed to compile keeper-sprite array glow shader");
            }
        }

        // Allocate the texture array. RequestCreateTexture's initial_pixels
        // stays empty (nullptr upload) -- undefined storage is valid and
        // expected here, matching the original's own glTexImage3D(...,
        // nullptr) call.
        GpuTextureDesc array_desc;
        array_desc.width = k_kspr_atlas_dim;
        array_desc.height = k_kspr_atlas_dim;
        array_desc.array_layers = k_kspr_atlas_layers;
        array_desc.format = GpuTextureFormat::RG8;
        array_desc.min_filter = GpuTextureFilter::Nearest;
        array_desc.mag_filter = GpuTextureFilter::Nearest;
        array_desc.wrap = GpuTextureWrap::Clamp;
        array_desc.debug_name = "kspr_sprite_array";
        m_kspr_sprite_array_handle = m_resource_mapper->RequestCreateTexture(array_desc);
        if (m_resource_mapper->ResolveTexture(m_kspr_sprite_array_handle) == nullptr)
        {
            WARNLOG("GL_TEXTURE_2D_ARRAY alloc failed, sprite atlas disabled");
            m_kspr_sprite_array_handle = kInvalidGpuResource;
            m_kspr_atlas_shader_handle = kInvalidGpuResource;
            m_kspr_atlas_glow_shader_handle = kInvalidGpuResource;
            break;
        }
        SYNCLOG("GLWorldViewRenderer: keeper-sprite decode atlas ready (%d layers, 512x512 GL_RG8)",
                k_kspr_atlas_layers);
    } while (false);

    // CLUT texture -- only useful (and only allocated) when the atlas shader
    // compiled successfully.
    if (m_kspr_atlas_shader_handle != kInvalidGpuResource && m_kspr_clut_tex_handle == kInvalidGpuResource)
    {
        // Clear the entire CLUT to zero (transparent black) so unwritten
        // rows never produce garbage. Only 256*k_clut_rows*4 = 128 KB.
        std::vector<uint8_t> zero_clut((size_t)256 * k_clut_rows * 4, 0);
        GpuTextureDesc desc;
        desc.width = 256;
        desc.height = k_clut_rows;
        desc.format = GpuTextureFormat::RGBA8;
        desc.min_filter = GpuTextureFilter::Nearest;
        desc.mag_filter = GpuTextureFilter::Nearest;
        desc.wrap = GpuTextureWrap::Clamp;
        desc.initial_pixels = std::move(zero_clut);
        desc.debug_name = "kspr_clut";
        m_kspr_clut_tex_handle = m_resource_mapper->RequestCreateTexture(desc);
        const auto* capture_clut = m_resource_mapper->ResolveTexture(m_kspr_clut_tex_handle); // force realization now
        m_capture_clut_known.fill(capture_clut != nullptr);
        for (auto& row : m_capture_clut_rows) row.fill(0);
        m_kspr_clut_used = 1;
    }

    // ── Depth-fail outline shaders ───────────────────────────────────
    // Todo : fix and customise,the colour should be the owner's colour of the creature, same rules as unclaimed rooms
    {
        const GLProgram* outline_prog = CreateKsprProgram(m_resource_mapper, m_kspr_outline_shader_handle,
            KSPR_VERTEX_SHADER, KSPR_OUTLINE_FRAGMENT_SHADER, "kspr_outline");
        if (outline_prog)
        {
            glUseProgram(outline_prog->id);
            m_kspr_outline_loc_viewport = glGetUniformLocation(outline_prog->id, "u_viewport");
            m_kspr_outline_loc_sprite   = glGetUniformLocation(outline_prog->id, "u_sprite");
            m_kspr_outline_loc_z_ndc    = glGetUniformLocation(outline_prog->id, "u_z_ndc");
            m_kspr_outline_loc_color    = glGetUniformLocation(outline_prog->id, "u_outline_color");
            glUniform1i(m_kspr_outline_loc_sprite, 0); // GL_TEXTURE0
            glUseProgram(0);
        }
        else
        {
            WARNLOG("GLWorldViewRenderer: kspr_outline shader link failed");
        }

        if (m_kspr_atlas_shader_handle != kInvalidGpuResource)
        {
            const GLProgram* atlas_outline_prog = CreateKsprProgram(m_resource_mapper, m_kspr_atlas_outline_shader_handle,
                KSPR_VERTEX_SHADER, KSPR_ARRAY_OUTLINE_FRAGMENT_SHADER, "kspr_array_outline");
            if (atlas_outline_prog)
            {
                glUseProgram(atlas_outline_prog->id);
                m_kspr_atlas_outline_loc_viewport = glGetUniformLocation(atlas_outline_prog->id, "u_viewport");
                m_kspr_atlas_outline_loc_sprite   = glGetUniformLocation(atlas_outline_prog->id, "u_sprite");
                m_kspr_atlas_outline_loc_z_ndc    = glGetUniformLocation(atlas_outline_prog->id, "u_z_ndc");
                m_kspr_atlas_outline_loc_color    = glGetUniformLocation(atlas_outline_prog->id, "u_outline_color");
                m_kspr_atlas_outline_loc_layer    = glGetUniformLocation(atlas_outline_prog->id, "u_layer");
                glUniform1i(m_kspr_atlas_outline_loc_sprite, 0); // GL_TEXTURE0
                glUseProgram(0);
            }
            else
            {
                WARNLOG("GLWorldViewRenderer: kspr_array_outline shader link failed");
            }
        }
    }

    // ── Edge-detect outline shaders ──────────────────────────────────
    // Todo : fix and customise,the colour should be the owner's colour of the creature, same rules as unclaimed rooms
    {
        const GLProgram* edge_prog = CreateKsprProgram(m_resource_mapper, m_kspr_edge_shader_handle,
            KSPR_VERTEX_SHADER, KSPR_EDGE_FRAGMENT_SHADER, "kspr_edge");
        if (edge_prog)
        {
            glUseProgram(edge_prog->id);
            m_kspr_edge_loc_viewport = glGetUniformLocation(edge_prog->id, "u_viewport");
            m_kspr_edge_loc_sprite   = glGetUniformLocation(edge_prog->id, "u_sprite");
            m_kspr_edge_loc_z_ndc    = glGetUniformLocation(edge_prog->id, "u_z_ndc");
            m_kspr_edge_loc_color    = glGetUniformLocation(edge_prog->id, "u_outline_color");
            glUniform1i(m_kspr_edge_loc_sprite, 0);
            glUseProgram(0);
        }
        else
        {
            WARNLOG("GLWorldViewRenderer: kspr_edge shader link failed");
        }

        if (m_kspr_atlas_shader_handle != kInvalidGpuResource)
        {
            const GLProgram* atlas_edge_prog = CreateKsprProgram(m_resource_mapper, m_kspr_atlas_edge_shader_handle,
                KSPR_VERTEX_SHADER, KSPR_ARRAY_EDGE_FRAGMENT_SHADER, "kspr_array_edge");
            if (atlas_edge_prog)
            {
                glUseProgram(atlas_edge_prog->id);
                m_kspr_atlas_edge_loc_viewport = glGetUniformLocation(atlas_edge_prog->id, "u_viewport");
                m_kspr_atlas_edge_loc_sprite   = glGetUniformLocation(atlas_edge_prog->id, "u_sprite");
                m_kspr_atlas_edge_loc_z_ndc    = glGetUniformLocation(atlas_edge_prog->id, "u_z_ndc");
                m_kspr_atlas_edge_loc_color    = glGetUniformLocation(atlas_edge_prog->id, "u_outline_color");
                m_kspr_atlas_edge_loc_layer    = glGetUniformLocation(atlas_edge_prog->id, "u_layer");
                glUniform1i(m_kspr_atlas_edge_loc_sprite, 0);
                glUseProgram(0);
            }
            else
            {
                WARNLOG("GLWorldViewRenderer: kspr_array_edge shader link failed");
            }
        }
    }

    SYNCLOG("GLWorldViewRenderer: keeper-sprite shader initialised");

    // Instanced fast path: optional, requires the decode atlas. Failure just
    // leaves the per-sprite path active (handle invalid gates usage in
    // gpu_execute_passes()).
    if (m_kspr_sprite_array_handle != kInvalidGpuResource && !init_keeper_sprite_instancing())
        WARNLOG("GLWorldViewRenderer: sprite instancing unavailable, using per-sprite path");

    return true;
}

bool GLWorldViewRenderer::init_keeper_sprite_instancing()
{
    if (!m_resource_mapper) return false;

    const GLProgram* inst_prog = CreateKsprProgram(m_resource_mapper, m_kspr_inst_shader_handle,
        KSPR_INST_VERTEX_SHADER, KSPR_INST_FRAGMENT_SHADER, "kspr_inst");
    if (!inst_prog)
        return false;

    glUseProgram(inst_prog->id);
    m_kspr_inst_loc_viewport = glGetUniformLocation(inst_prog->id, "u_viewport");
    m_kspr_inst_loc_palette_xform = glGetUniformLocation(inst_prog->id, "u_palette_xform");
    glUniform1i(glGetUniformLocation(inst_prog->id, "u_sprite"), 0);  // GL_TEXTURE0
    glUniform1i(glGetUniformLocation(inst_prog->id, "u_clut"),   1);  // GL_TEXTURE1
    glUseProgram(0);

    static const float k_unit_quad[8] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 1.0f,
    };
    {
        GpuGeometryBufferDesc desc;
        desc.vertex_stride = 2 * (uint32_t)sizeof(float);
        desc.attribs = { { 0, 2, GpuVertexAttribType::Float, 0 } };
        desc.dynamic = false; // static unit quad, content never changes
        desc.initial_vertex_capacity = sizeof(k_unit_quad);
        desc.debug_name = "kspr_inst_quad";
        m_kspr_inst_quad_geom_handle = m_resource_mapper->RequestCreateGeometryBuffer(desc);
        const GLGeometryBuffer* quad_geom = m_resource_mapper->ResolveGeometryBuffer(m_kspr_inst_quad_geom_handle);
        if (!quad_geom) return false;
        glBindBuffer(GL_ARRAY_BUFFER, quad_geom->vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(k_unit_quad), k_unit_quad);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    // Attribute byte offsets, locked to KsprInstance's layout.
    static_assert(sizeof(KsprInstance) == 60, "KsprInstance layout changed -- update attrib offsets");
    constexpr uintptr_t k_inst_off_rect  = 0;   // float[4]
    constexpr uintptr_t k_inst_off_src   = 16;  // float[2]
    constexpr uintptr_t k_inst_off_map   = 24;  // float[4]
    constexpr uintptr_t k_inst_off_misc  = 40;  // layer, clut_v, alpha, z_ndc
    constexpr uintptr_t k_inst_off_flags = 56;  // uint32_t

    {
        GpuGeometryBufferDesc desc;
        desc.vertex_stride = (uint32_t)sizeof(KsprInstance);
        desc.dynamic = true;
        desc.debug_name = "kspr_inst_geom";
        m_kspr_inst_geom_handle = m_resource_mapper->RequestCreateGeometryBuffer(desc);
    }
    const GLGeometryBuffer* quad_geom = m_resource_mapper->ResolveGeometryBuffer(m_kspr_inst_quad_geom_handle);
    const GLGeometryBuffer* inst_geom = m_resource_mapper->ResolveGeometryBuffer(m_kspr_inst_geom_handle);
    if (!quad_geom || !inst_geom) return false;

    glBindVertexArray(inst_geom->vao);
    glBindBuffer(GL_ARRAY_BUFFER, quad_geom->vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, inst_geom->vbo);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(KsprInstance),
                          (void*)k_inst_off_rect);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(KsprInstance),
                          (void*)k_inst_off_src);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(KsprInstance),
                          (void*)k_inst_off_misc);
    glVertexAttribIPointer(4, 1, GL_UNSIGNED_INT, sizeof(KsprInstance),
                           (void*)k_inst_off_flags);
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(KsprInstance),
                          (void*)k_inst_off_map);
    for (GLuint loc = 1; loc <= 5; ++loc)
    {
        glEnableVertexAttribArray(loc);
        glVertexAttribDivisor(loc, 1);
    }
    glBindVertexArray(0);

    // Instanced depth-fail outline shader
    {
        const GLProgram* inst_outline_prog = CreateKsprProgram(m_resource_mapper, m_kspr_inst_outline_shader_handle,
            KSPR_INST_OUTLINE_VERTEX_SHADER, KSPR_INST_OUTLINE_FRAGMENT_SHADER, "kspr_inst_outline");
        if (inst_outline_prog)
        {
            glUseProgram(inst_outline_prog->id);
            m_kspr_inst_outline_loc_viewport = glGetUniformLocation(inst_outline_prog->id, "u_viewport");
            glUniform1i(glGetUniformLocation(inst_outline_prog->id, "u_sprite"), 0);
            glUseProgram(0);
        }
    }

    // Instanced edge-detect shader -- shares the same vertex shader and VAO
    // as the instanced outline, but uses edge-detection in the fragment shader.
    {
        const GLProgram* inst_edge_prog = CreateKsprProgram(m_resource_mapper, m_kspr_inst_edge_shader_handle,
            KSPR_INST_OUTLINE_VERTEX_SHADER, KSPR_INST_EDGE_FRAGMENT_SHADER, "kspr_inst_edge");
        if (inst_edge_prog)
        {
            glUseProgram(inst_edge_prog->id);
            m_kspr_inst_edge_loc_viewport = glGetUniformLocation(inst_edge_prog->id, "u_viewport");
            glUniform1i(glGetUniformLocation(inst_edge_prog->id, "u_sprite"), 0);
            glUseProgram(0);
        }
    }

    // Outline VAO: same shared unit quad, KsprOutlineInstance layout.
    static_assert(sizeof(KsprOutlineInstance) == 68, "KsprOutlineInstance layout changed -- update attrib offsets");
    constexpr uintptr_t k_outl_off_rect  = 0;   // float[4]
    constexpr uintptr_t k_outl_off_src   = 16;  // float[2]
    constexpr uintptr_t k_outl_off_map   = 24;  // float[4]
    constexpr uintptr_t k_outl_off_lzf   = 40;  // layer, z_ndc, flip
    constexpr uintptr_t k_outl_off_color = 52;  // float[4]

    {
        GpuGeometryBufferDesc desc;
        desc.vertex_stride = (uint32_t)sizeof(KsprOutlineInstance);
        desc.dynamic = true;
        desc.debug_name = "kspr_inst_outline_geom";
        m_kspr_inst_outline_geom_handle = m_resource_mapper->RequestCreateGeometryBuffer(desc);
    }
    const GLGeometryBuffer* outline_geom = m_resource_mapper->ResolveGeometryBuffer(m_kspr_inst_outline_geom_handle);
    if (!outline_geom) return false;

    glBindVertexArray(outline_geom->vao);
    glBindBuffer(GL_ARRAY_BUFFER, quad_geom->vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, outline_geom->vbo);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(KsprOutlineInstance),
                          (void*)k_outl_off_rect);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(KsprOutlineInstance),
                          (void*)k_outl_off_src);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(KsprOutlineInstance),
                          (void*)k_outl_off_lzf);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(KsprOutlineInstance),
                          (void*)k_outl_off_color);
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(KsprOutlineInstance),
                          (void*)k_outl_off_map);
    for (GLuint loc = 1; loc <= 5; ++loc)
    {
        glEnableVertexAttribArray(loc);
        glVertexAttribDivisor(loc, 1);
    }
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    SYNCLOG("GLWorldViewRenderer: instanced keeper-sprite path ready");
    return true;
}

/******************************************************************************/

void GLWorldViewRenderer::BeginWorldPass(int w, int h, int vp_x, int vp_y)
{
    ASSERT_GAME_THREAD();

    m_screen_w         = w;
    m_screen_h         = h;
    m_vp_x             = vp_x;
    m_vp_y             = vp_y;
    m_current_bucket   = 0;
    m_world_pass_active = true;
    m_sprite_entry_seq  = 0;
    m_kspr_pass_start   = m_kspr_ir.size();

    gpu_flush();
    m_cmd_vert_start = m_vert_count;

    // Set the vec globals that bucket-list filling code reads
    // (setup_rotate_stuff, fill_in_points_*, etc.) without the software
    // world pass's setup_vecs(), which also points vec_screen/poly_screen
    // at the CPU raster's target -- unnecessary for the GPU path.
    if (w > 0) vec_window_width  = (long)w;
    if (h > 0) vec_window_height = (long)h;
}

/******************************************************************************/

void GLWorldViewRenderer::Triangle(const kfx::render::WorldDrawTriangle& vertices)
{
    if (!m_world_write_cmds) return;
    auto& target = m_world_write_cmds->tile_verts;
    target.resize((size_t)(m_vert_count + 3));
    std::copy(vertices.begin(), vertices.end(), target.begin() + m_vert_count);
    const uint32_t outline_entry = WorldSceneOutlineEntryTag();
    if (outline_entry != kfx::render::SceneOutlineNoDraw) {
        try {
            if (size_t(m_vert_count / 3) >= 262144)
                WorldSceneOutlineFailProducer("GL outline terrain tag budget exceeded");
            else {
                m_outline_tile_tags.resize(size_t(m_vert_count / 3) + 1, kfx::render::SceneOutlineNoDraw);
                m_outline_tile_tags[size_t(m_vert_count / 3)] = outline_entry;
            }
        } catch (const std::exception&) {
            WorldSceneOutlineFailProducer("Cannot allocate GL outline terrain tags");
        }
    }
    m_vert_count += 3;
}

void GLWorldViewRenderer::BeginBucket(int bucket)
{
    m_current_bucket = bucket;
}

void GLWorldViewRenderer::ObserveEntry(int bucket, const BasicQ& entry)
{
    WorldSceneTraceBucket(bucket, entry.kind);
}

void GLWorldViewRenderer::Sprite(kfx::render::WorldSpriteDispatch mode,
    BucketKindJontySprite& sprite, Camera* camera)
{
    kfx::render::DispatchWorldSprite(mode, sprite, camera);
}

void GLWorldViewRenderer::Shadow(const BucketKindCreatureShadow& shadow)
{
    kfx::render::WorldShadowSourceView source;
    const auto status = kfx::render::ResolveWorldShadowSource(shadow, source);
    if (status == kfx::render::WorldShadowSourceStatus::ResolutionFailed)
        WorldSceneOutlineFailProducer("Complete GL shadow source resolution failed");
    else if (m_shadow_shader_handle == kInvalidGpuResource)
        WorldSceneOutlineFailProducer("Complete GL world shadow shader unavailable");
    else if (status == kfx::render::WorldShadowSourceStatus::ExtentUnavailable)
        WorldSceneOutlineFailProducer("Complete GL shadow source extent unavailable");
    else
        append_shadow_quad(&shadow, source);
}

void GLWorldViewRenderer::Overlay(kfx::render::WorldOverlayDispatch kind,
    kfx::render::WorldOverlayDepth depth, float z_ndc, BasicQ& entry)
{
    kfx::render::DispatchWorldOverlay(kind, depth, z_ndc, entry);
}

/******************************************************************************/
// Keeper sprites

void GLWorldViewRenderer::ClearKeeperSpriteAtlas()
{
    DrawCmd cmd;
    cmd.type = DrawCmd::CMD_CLEAR_KSPR_ATLAS;
    m_draw_cmds.push_back(cmd);
    SYNCLOG("GLWorldViewRenderer: CMD_CLEAR_KSPR_ATLAS queued");
}

void GLWorldViewRenderer::UpdateAnimatedTiles()
{
    DrawCmd cmd;
    cmd.type = DrawCmd::CMD_UPDATE_ANIMATED_TILES;
    m_draw_cmds.push_back(cmd);
}

void GLWorldViewRenderer::execute_clear_atlas()
{
    m_kspr_atlas_map.clear();
    m_kspr_atlas_used = 0;
    m_kspr_clut_remaps.clear();
    m_kspr_clut_used = 1;
    memset(m_kspr_clut_palette_snap, 0, sizeof(m_kspr_clut_palette_snap));
    SYNCDBG(6, "GLWorldViewRenderer: keeper-sprite atlas cleared");
}

void GLWorldViewRenderer::PreloadKeeperSpriteAtlas()
{
    DrawCmd cmd;
    cmd.type = DrawCmd::CMD_PRELOAD_KSPR_ATLAS;
    m_draw_cmds.push_back(cmd);
    SYNCLOG("GLWorldViewRenderer: CMD_PRELOAD_KSPR_ATLAS queued");
}

void GLWorldViewRenderer::execute_preload_atlas()
{
    if (!m_resource_mapper) return;
    const GLTexture* sprite_array = m_resource_mapper->ResolveTexture(m_kspr_sprite_array_handle);
    if (!sprite_array || m_kspr_atlas_shader_handle == kInvalidGpuResource) {
        ERRORLOG("execute_preload_atlas: GL resources not ready -- preload skipped");
        return;
    }

    ensure_clut_valid();  // identity row 0 must exist before any atlas draws

    int preloaded = 0;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, sprite_array->id);

    // Vanilla sprites: only those already resident in RAM. keepsprite[i] is
    // lazy-loaded from disk; null entries haven't been accessed yet and
    // cannot be preloaded without a full disk-load pass.
    for (int i = 0; i < KEEPSPRITE_LENGTH && m_kspr_atlas_used < k_kspr_atlas_preload_max; i++)
    {
        if (!keepsprite[i] || !*keepsprite[i]) continue;
        if ((size_t)i >= creature_table_length) continue;
        const struct KeeperSprite& ks = creature_table[i];
        if (ks.SWidth <= 0 || ks.SHeight <= 0 ||
            ks.SWidth > k_kspr_atlas_dim || ks.SHeight > k_kspr_atlas_dim) continue;
        const uint8_t* data = *keepsprite[i];
        if (m_kspr_atlas_map.count(i)) continue;

        size_t data_bytes = 0;
        if (!get_keepersprite_source_extent(i, data, &data_bytes)
            || !decode_keeper_rle(s_kspr_decode_buf, data, data_bytes, ks.SWidth, ks.SHeight)) continue;
        int layer = m_kspr_atlas_used++;
        glPixelStorei(GL_UNPACK_ROW_LENGTH, k_kspr_decode_dim);
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer,
                        k_kspr_atlas_dim, k_kspr_atlas_dim, 1,
                        GL_RG, GL_UNSIGNED_BYTE, s_kspr_decode_buf);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        m_kspr_atlas_map[i] = {layer, ks.SWidth, decoded_sprite_darkens(ks.SWidth, ks.SHeight)};
        record_sprite_coverage_for_capture(layer, ks.SWidth, ks.SHeight, s_kspr_decode_buf);
        preloaded++;
    }

    // Custom sprites: fully in RAM after init_custom_sprites().
    for (int i = 0; i < KEEPERSPRITE_ADD_NUM && m_kspr_atlas_used < k_kspr_atlas_preload_max; i++)
    {
        if (!keepersprite_add[i]) continue;
        const struct KeeperSprite& ks = creature_table_add[i];
        if (ks.SWidth <= 0 || ks.SHeight <= 0 ||
            ks.SWidth > k_kspr_atlas_dim || ks.SHeight > k_kspr_atlas_dim) continue;
        const uint8_t* data = keepersprite_add[i];
        const int32_t sprite_id = KEEPERSPRITE_ADD_OFFSET + i;
        if (m_kspr_atlas_map.count(sprite_id)) continue;

        size_t data_bytes = 0;
        if (!get_keepersprite_source_extent(sprite_id, data, &data_bytes)
            || !decode_keeper_rle(s_kspr_decode_buf, data, data_bytes, ks.SWidth, ks.SHeight)) continue;
        int layer = m_kspr_atlas_used++;
        glPixelStorei(GL_UNPACK_ROW_LENGTH, k_kspr_decode_dim);
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer,
                        k_kspr_atlas_dim, k_kspr_atlas_dim, 1,
                        GL_RG, GL_UNSIGNED_BYTE, s_kspr_decode_buf);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        m_kspr_atlas_map[sprite_id] = {layer, ks.SWidth, decoded_sprite_darkens(ks.SWidth, ks.SHeight)};
        record_sprite_coverage_for_capture(layer, ks.SWidth, ks.SHeight, s_kspr_decode_buf);
        preloaded++;
    }

    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    if (m_kspr_atlas_used > m_kspr_atlas_peak)
        m_kspr_atlas_peak = m_kspr_atlas_used;
    SYNCLOG("GLWorldViewRenderer: atlas preload complete -- %d sprites uploaded (%d/%d layers used)",
            preloaded, m_kspr_atlas_used, k_kspr_atlas_layers);
}

void GLWorldViewRenderer::ensure_clut_valid()
{
    if (!m_resource_mapper) return;
    const GLTexture* clut_tex = m_resource_mapper->ResolveTexture(m_kspr_clut_tex_handle);
    if (!clut_tex) return;
    if (memcmp(m_rt_palette, m_kspr_clut_palette_snap, sizeof(m_rt_palette)) == 0) return;

    memcpy(m_kspr_clut_palette_snap, m_rt_palette, sizeof(m_rt_palette));

    // Rebuild identity CLUT row 0: palette[i] for all i.
    uint8_t row[256 * 4];
    for (int i = 0; i < 256; i++) {
        row[i*4+0] = RendererPaletteChannel8(m_rt_palette[i*3+0]);
        row[i*4+1] = RendererPaletteChannel8(m_rt_palette[i*3+1]);
        row[i*4+2] = RendererPaletteChannel8(m_rt_palette[i*3+2]);
        row[i*4+3] = 255;
    }
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, clut_tex->id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
    std::copy_n(row, m_capture_clut_rows[0].size(), m_capture_clut_rows[0].begin());
    m_capture_clut_known[0] = true;
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);  // restore active unit

    // Invalidate all cached remap CLUTs -- they depend on the old palette values.
    m_kspr_clut_remaps.clear();
    m_kspr_clut_used = 1;

    SYNCDBG(6, "GLWorldViewRenderer: CLUT rebuilt (palette changed)");
}

void GLWorldViewRenderer::SubmitKeeperSprite(
    const struct SpriteScale* scale,
    const unsigned char* data, int src_w, int src_h, int32_t content_h,
    unsigned int draw_flags, const unsigned char* remap,
    int32_t sprite_id)
{
    if (src_w <= 0 || src_h <= 0 || src_w > k_kspr_decode_dim || src_h > k_kspr_decode_dim) {
        static int s_dim = 0;
        if (s_dim++ < 20)
            WARNLOG("SubmitKeeperSprite: invalid sprite dimensions %dx%d (max %d) -- dropped",
                    src_w, src_h, k_kspr_decode_dim);
        return;
    }
    // Clamp rather than trust the caller: a bad content_h must not produce
    // rows past the decoded sprite.
    if (content_h <= 0 || content_h > src_h) content_h = src_h;

    struct SpriteScaleAxis ax, ay;
    sprite_scale_axis(scale->frame_x, scale->frame_src_w, scale->frame_dst_w, scale->window_w,
                      scale->content_x, src_w, &ax);
    sprite_scale_axis(scale->frame_y, scale->frame_src_h, scale->frame_dst_h, scale->window_h,
                      scale->content_y, content_h, &ay);
    if (ax.dst_len <= 0 || ay.dst_len <= 0) return;

    IRWorldKeeperSpriteCmd cmd;
    if (!get_keepersprite_source_extent(sprite_id, data, &cmd.data_bytes)) {
        static unsigned warnings = 0;
        if (warnings++ < 20)
            WARNLOG("SubmitKeeperSprite: source extent unavailable for sprite %d", (int)sprite_id);
        WorldSceneOutlineFailProducer("GL keeper sprite source extent unavailable");
        return;
    }
    cmd.dst_x         = ax.dst_start;
    cmd.dst_y         = ay.dst_start;
    cmd.dst_w         = ax.dst_len;
    cmd.dst_h         = ay.dst_len;
    cmd.phase_x       = ax.phase;
    cmd.phase_y       = ay.phase;
    cmd.step_x        = ax.step;
    cmd.step_y        = ay.step;
    cmd.src_w         = src_w;
    cmd.src_h         = src_h;
    cmd.content_h     = content_h;
    cmd.draw_flags    = draw_flags;
    cmd.data          = data;
    cmd.remap_enabled = (remap != nullptr) ? 1 : 0;
    if (cmd.remap_enabled)
        memcpy(cmd.remap_table, remap, sizeof(cmd.remap_table));
    cmd.z_ndc         = m_current_sprite_z;
    cmd.owner         = (int8_t)RendererGetCurrentSpriteOwner();
    cmd.wants_outline = (int8_t)RendererGetCurrentSpriteWantsOutline();
    cmd.sprite_id     = sprite_id;
    cmd.sort_key      = m_current_sprite_sort_key;
    if (m_capturing_cursor)
        m_cursor_kspr_ir.push_back(cmd);
    else {
        m_kspr_ir.push_back(cmd);
        const uint32_t outline_draw = WorldSceneOutlineSpriteTag();
        if (outline_draw != kfx::render::SceneOutlineNoDraw) {
            try {
                if (m_kspr_ir.size() > 327680)
                    WorldSceneOutlineFailProducer("GL outline sprite tag budget exceeded");
                else {
                    m_outline_sprite_tags.resize(m_kspr_ir.size(), kfx::render::SceneOutlineNoDraw);
                    m_outline_sprite_tags.back() = outline_draw;
                }
            } catch (const std::exception&) {
                WorldSceneOutlineFailProducer("Cannot allocate GL outline sprite tags");
            }
        }
    }
}

int GLWorldViewRenderer::resolve_atlas_layer(int32_t sprite_id, const unsigned char* data, size_t data_bytes, int src_w, int src_h,
                                             bool* darkens, int shadow_factor)
{
    if (darkens) *darkens = false;
    if (src_w <= 0 || src_h <= 0 || src_w > k_kspr_decode_dim || src_h > k_kspr_decode_dim) return -1;
    if (shadow_factor != 1 && shadow_factor != 2) return -1;
    const bool reduced = shadow_factor == 2;
    if (!reduced && (src_w > k_kspr_atlas_dim || src_h > k_kspr_atlas_dim)) return -1;
    if (!m_resource_mapper) return -1;
    const GLTexture* sprite_array = m_resource_mapper->ResolveTexture(m_kspr_sprite_array_handle);
    const GLTexture* clut_tex = m_resource_mapper->ResolveTexture(m_kspr_clut_tex_handle);
    if (!sprite_array || m_kspr_atlas_shader_handle == kInvalidGpuResource || !clut_tex)
        return -1;
    if (sprite_id < 0)
        return -1;  // no stable identity -- cannot cache safely
    const int32_t cache_key = keeper_shadow_atlas_cache_key(sprite_id, reduced);
    auto it = m_kspr_atlas_map.find(cache_key);
    if (it != m_kspr_atlas_map.end())
    {
        m_kspr_atlas_hits++;
        if (darkens) *darkens = it->second.darkens;
        return it->second.layer;
    }
    if (m_kspr_atlas_used >= k_kspr_atlas_layers)
        return -1;

    // Initialize and upload the whole layer. FrameWidth/FrameHeight shadow
    // geometry can sample beyond SWidth/SHeight; both right and bottom padding
    // must be transparent instead of reading undefined or previously used data.
    if (!decode_keeper_rle(s_kspr_decode_buf, data, data_bytes, src_w, src_h)) return -1;
    const uint8_t* upload = s_kspr_decode_buf;
    if (reduced) {
        if (!keeper_shadow_reduce_rg(s_kspr_shadow_coverage, sizeof(s_kspr_shadow_coverage),
            s_kspr_decode_buf, sizeof(s_kspr_decode_buf), src_w, src_h, k_kspr_decode_dim * 2)) return -1;
        upload = s_kspr_shadow_coverage;
    }
    const int atlas_layer = m_kspr_atlas_used++;
    if (m_kspr_atlas_used > m_kspr_atlas_peak)
        m_kspr_atlas_peak = m_kspr_atlas_used;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, sprite_array->id);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, reduced ? k_kspr_atlas_dim : k_kspr_decode_dim);
    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0,
                    0, 0, atlas_layer,
                    k_kspr_atlas_dim, k_kspr_atlas_dim, 1,
                    GL_RG, GL_UNSIGNED_BYTE, upload);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    const bool sprite_darkens = !reduced && decoded_sprite_darkens(src_w, src_h);
    m_kspr_atlas_map[cache_key] = {atlas_layer, src_w, sprite_darkens};
    record_sprite_coverage_for_capture(atlas_layer, reduced ? (src_w + 1) / 2 : src_w,
        reduced ? (src_h + 1) / 2 : src_h, upload, reduced ? k_kspr_atlas_dim : k_kspr_decode_dim);
    if (darkens) *darkens = sprite_darkens;
    m_kspr_atlas_misses++;
    return atlas_layer;
}

float GLWorldViewRenderer::resolve_clut_v(const unsigned char* remap,
    kfx::render::SceneOutlineColourResolution& resolution)
{
    resolution = kfx::render::SceneOutlineColourResolution::ClutIdentity;
    // Row 0 = identity (non-remapped sprites).
    float clut_v = 0.5f / (float)k_clut_rows;
    if (!m_resource_mapper) return clut_v;
    const GLTexture* clut_tex = m_resource_mapper->ResolveTexture(m_kspr_clut_tex_handle);
    if (!remap || !clut_tex)
        return clut_v;
    for (size_t i = 0; i < m_kspr_clut_remaps.size(); ++i)
    {
        if (memcmp(m_kspr_clut_remaps[i].data(), remap, 256) == 0)
        {
            resolution = kfx::render::SceneOutlineColourResolution::ClutRemap;
            return (float(i + 1) + 0.5f) / (float)k_clut_rows;
        }
    }
    if (m_kspr_clut_used >= k_clut_rows)
    {
        resolution = kfx::render::SceneOutlineColourResolution::ClutCapacityIdentity;
        return clut_v;  // CLUT full: identity fallback (wrong colour, no crash)
    }

    const int row_idx = m_kspr_clut_used++;
    uint8_t row_data[256 * 4];
    for (int ci = 0; ci < 256; ci++) {
        int ri = remap[ci];
        row_data[ci*4+0] = RendererPaletteChannel8(m_rt_palette[ri*3+0]);
        row_data[ci*4+1] = RendererPaletteChannel8(m_rt_palette[ri*3+1]);
        row_data[ci*4+2] = RendererPaletteChannel8(m_rt_palette[ri*3+2]);
        row_data[ci*4+3] = 255;
    }
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, clut_tex->id);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row_idx, 256, 1,
                    GL_RGBA, GL_UNSIGNED_BYTE, row_data);
    std::copy_n(row_data, m_capture_clut_rows[row_idx].size(), m_capture_clut_rows[row_idx].begin());
    m_capture_clut_known[row_idx] = true;
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    std::array<uint8_t, 256> remap_copy;
    memcpy(remap_copy.data(), remap, remap_copy.size());
    m_kspr_clut_remaps.push_back(remap_copy);
    resolution = kfx::render::SceneOutlineColourResolution::ClutRemap;
    return (float(row_idx) + 0.5f) / (float)k_clut_rows;
}

void GLWorldViewRenderer::BeginWorldSpriteCapture(int32_t bucket_idx)
{
    if (!m_initialized || m_world_write_cmds == nullptr)
        return;
    if (bucket_idx >= BUCKETS_COUNT)
        bucket_idx = BUCKETS_COUNT - 1;
    else if (bucket_idx < 0)
        bucket_idx = 0;
    // Biased half a bucket closer to the camera so sprites always pass the
    // depth test against same-bucket ground polygons.
    m_current_sprite_z = 2.0f * ((float)bucket_idx - 0.5f) / (float)(BUCKETS_COUNT - 1) - 1.0f;
    m_current_sprite_sort_key = ((uint64_t)bucket_idx << 32) | m_sprite_entry_seq++;
}

void GLWorldViewRenderer::BeginCursorCapture()
{
    ASSERT_GAME_THREAD();
    m_capturing_cursor = true;
    m_current_sprite_z = -1.0f;
    m_current_sprite_sort_key = 0;
}

void GLWorldViewRenderer::EndCursorCapture()
{
    ASSERT_GAME_THREAD();
    m_capturing_cursor = false;
}

void GLWorldViewRenderer::RecordAppliedPaletteForCapture(const uint8_t* rgba, GpuResourceHandle handle, unsigned int texture) noexcept
{
    std::copy_n(rgba, m_capture_applied_palette.size(), m_capture_applied_palette.begin());
    m_capture_applied_palette_known = true;
    if (!handle) { handle = m_palette_tex_handle; texture = ResolvePaletteTexId(); }
    if (texture && capture_tight_unpack(1024)) m_capture_palette_upload.Record(handle, texture, 256, 1, rgba, 1024);
    else m_capture_palette_upload.bytes.clear();
}
void GLWorldViewRenderer::RecordAppliedFadeForCapture(const uint8_t* indices, GpuResourceHandle handle, unsigned int texture) noexcept
{
    memcpy(m_capture_applied_fade.data(), indices, m_capture_applied_fade.size());
    m_capture_applied_fade_known = true;
    if (!handle) { handle = m_fade_tex_handle; texture = ResolveFadeTexId(); }
    if (texture && capture_tight_unpack(256)) m_capture_fade_upload.Record(handle, texture, 256, 64, indices, 16384);
    else m_capture_fade_upload.bytes.clear();
}
void GLWorldViewRenderer::RecordWorldTargetForCapture(int width, int height,
    kfx::render::SceneGLWorldTargetKind kind, const float* clear_rgba) noexcept
{
    m_capture_world_target = {};
    m_capture_world_target.native_width = width; m_capture_world_target.native_height = height;
    m_capture_world_target.target_kind = kind;
    std::copy(clear_rgba, clear_rgba + 4, m_capture_world_target.clear_rgba.begin());
    m_capture_world_target_known = width > 0 && height > 0;
}
void GLWorldViewRenderer::BeginCompositionForCapture(const float* clear_rgba) noexcept
{
    if (!m_rt_outline_capture) return;
    m_capture_lens = {};
    m_capture_lens.requested_active = m_rt_lens_cmd.active;
    m_capture_lens.requested_type = m_rt_lens_cmd.active ? uint8_t(m_rt_lens_cmd.type) : 0;
    m_capture_lens.operation = m_rt_lens_cmd.active ? kfx::render::SceneGLLensOperation::NotDrawn
        : kfx::render::SceneGLLensOperation::None;
    GLint framebuffer = 0, samples = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer); glGetIntegerv(GL_SAMPLES, &samples);
    if (!m_resource_mapper || unsigned(framebuffer) != m_resource_mapper->GetScreenFramebuffer()
        || samples || !capture_clear_state())
        WorldSceneFailOutlineCapture(m_rt_outline_capture, "Screen clear target/state is not proved");
    std::array<float, 4> clear{}; std::copy_n(clear_rgba, 4, clear.begin());
    WorldSceneBeginCompositionCapture(m_rt_outline_capture, m_full_screen_w, m_full_screen_h, clear);
}
void GLWorldViewRenderer::FinishCompositionForCapture() noexcept
{
    if (!m_rt_outline_capture) return;
    WorldSceneRecordLensCapture(m_rt_outline_capture, std::move(m_capture_lens));
    WorldSceneFinishCompositionCapture(m_rt_outline_capture);
    m_rt_outline_capture.reset(); m_capture_lens = {};
}
unsigned int GLWorldViewRenderer::CompositionTargetForCapture() const noexcept
{
    if (!m_rt_outline_capture || !m_resource_mapper) return 0;
    if (!m_capture_lens.capture_began) return m_resource_mapper->GetScreenFramebuffer();
    const GLRenderTarget* rt = m_resource_mapper->ResolveRenderTarget(m_lens_scene_rt_handle);
    return rt ? rt->fbo : 0;
}
void GLWorldViewRenderer::record_lens_operation(kfx::render::SceneGLLensOperation operation) noexcept
{
    if (!m_rt_outline_capture) return;
    using namespace kfx::render;
    try {
        const auto require = [](bool good, const char* reason) { if (!good) throw std::runtime_error(reason); };
        require(m_capture_lens.operation == SceneGLLensOperation::NotDrawn && m_capture_lens.capture_began,
            "Lens draw outside its owned capture bracket");
        GLint viewport[4]{}, framebuffer = 0, scissor[4]{}, samples = 0;
        GLboolean mask[4]{}, depth_mask = GL_TRUE;
        glGetIntegerv(GL_VIEWPORT, viewport); glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
        glGetIntegerv(GL_SCISSOR_BOX, scissor); glGetIntegerv(GL_SAMPLES, &samples);
        glGetBooleanv(GL_COLOR_WRITEMASK, mask); glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
        const int gl_y = m_full_screen_h - m_rt_lens_cmd.viewport_y - m_rt_lens_cmd.viewport_h;
        require(unsigned(framebuffer) == m_resource_mapper->GetScreenFramebuffer() && !samples
            && !glIsEnabled(GL_BLEND) && !glIsEnabled(GL_DEPTH_TEST) && !depth_mask
            && !glIsEnabled(GL_FRAMEBUFFER_SRGB) && !glIsEnabled(GL_CULL_FACE) && !glIsEnabled(GL_RASTERIZER_DISCARD)
            && !glIsEnabled(GL_STENCIL_TEST) && !glIsEnabled(GL_COLOR_LOGIC_OP) && !glIsEnabled(GL_POLYGON_OFFSET_FILL)
            && mask[0] && mask[1] && mask[2] && mask[3]
            && viewport[0] == m_rt_lens_cmd.viewport_x && viewport[1] == gl_y
            && viewport[2] == m_rt_lens_cmd.viewport_w && viewport[3] == m_rt_lens_cmd.viewport_h,
            "Unsupported effective GL lens draw state");
        require(!glIsEnabled(GL_SCISSOR_TEST) || (scissor[0] <= viewport[0] && scissor[1] <= viewport[1]
            && int64_t(scissor[0]) + scissor[2] >= int64_t(viewport[0]) + viewport[2]
            && int64_t(scissor[1]) + scissor[3] >= int64_t(viewport[1]) + viewport[3]),
            "Lens scissor clips the declared destination");
        const GLRenderTarget* rt = m_resource_mapper->ResolveRenderTarget(m_lens_scene_rt_handle);
        require(rt && rt->width == m_full_screen_w && rt->height == m_full_screen_h,
            "Lens source native extent changed");
        SceneGLLensPass pass;
        pass.kind = SceneGLLensPassKind(unsigned(operation) - 1);
        pass.source_width = pass.target_width = m_full_screen_w;
        pass.source_height = pass.target_height = m_full_screen_h;
        pass.source_x = pass.destination_x = m_rt_lens_cmd.viewport_x;
        pass.destination_y = m_rt_lens_cmd.viewport_y;
        pass.viewport_width = m_rt_lens_cmd.viewport_w; pass.viewport_height = m_rt_lens_cmd.viewport_h;
        const auto consume = [&](const WorldSceneGLUploadMirror& mirror, GpuResourceHandle handle, unsigned slot) -> const std::vector<uint8_t>& {
            const GLTexture* tex = m_resource_mapper->ResolveTexture(handle);
            require(tex && mirror.Matches(handle, tex->id, tex->width, tex->height), "Lens effective upload provenance unavailable");
            m_capture_lens.resources[slot] = mirror.stamp; return mirror.bytes;
        };
        if (operation == SceneGLLensOperation::Mist || operation == SceneGLLensOperation::Overlay) {
            pass.palette_rgba = consume(m_capture_palette_upload, m_palette_tex_handle, 0);
            require(m_capture_index, "Lens RGB6 upload mirror unavailable");
            pass.rgb6_lookup = consume(*m_capture_index, m_palette_index_tex_handle, 2);
        }
        if (operation == SceneGLLensOperation::Mist) {
            pass.fade = consume(m_capture_fade_upload, m_fade_tex_handle, 1);
            pass.density = consume(m_capture_mist_upload, m_lens_mist_tex_handle, 3);
            pass.mist_offsets = {m_rt_lens_cmd.mist_pos_x, m_rt_lens_cmd.mist_pos_y,
                m_rt_lens_cmd.mist_sec_x, m_rt_lens_cmd.mist_sec_y};
            pass.mist_lightness = m_rt_lens_cmd.mist_lightness;
        } else if (operation == SceneGLLensOperation::Remap) {
            const auto& bytes = consume(m_capture_remap_upload, m_lens_remap_tex_handle, 4);
            pass.remap_width = m_capture_remap_upload.width; pass.remap_height = m_capture_remap_upload.height;
            require(bytes.size() == uint64_t(pass.remap_width) * pass.remap_height * 4, "Lens remap uploaded extent mismatch");
            pass.remap.resize(bytes.size() / 4); memcpy(pass.remap.data(), bytes.data(), bytes.size());
        } else if (operation == SceneGLLensOperation::Overlay) {
            pass.overlay = consume(m_capture_overlay_upload, m_lens_overlay_tex_handle, 5);
            pass.overlay_width = m_capture_overlay_upload.width; pass.overlay_height = m_capture_overlay_upload.height;
            pass.overlay_alpha = uint16_t(GLint(m_rt_lens_cmd.overlay_alpha * 256.0f + 0.5f));
        }
        ValidateSceneGLLensPass(pass);
        m_capture_lens.pass = std::move(pass); m_capture_lens.operation = operation;
        m_capture_lens.fallback_copy = operation == SceneGLLensOperation::Copy && m_capture_lens.requested_type != 0;
    } catch (const std::exception& e) { WorldSceneFailOutlineCapture(m_rt_outline_capture, e.what()); }
    catch (...) { WorldSceneFailOutlineCapture(m_rt_outline_capture, "Lens executed draw capture failed"); }
}
void GLWorldViewRenderer::record_sprite_coverage_for_capture(int layer, int width, int height, const uint8_t* rg, int stride_pixels) noexcept
{
    if (layer < 0 || size_t(layer) >= m_capture_sprite_coverage.size()) return;
    auto& source = m_capture_sprite_coverage[layer];
    source = {};
    if (!rg || width <= 0 || width > k_kspr_atlas_dim || height <= 0 || height > k_kspr_atlas_dim
        || stride_pixels < k_kspr_atlas_dim) return;
    try {
        source.bits.resize(size_t(k_kspr_atlas_dim) * k_kspr_atlas_dim / 8, 0);
        for (int y = 0; y < k_kspr_atlas_dim; ++y) for (int x = 0; x < k_kspr_atlas_dim; ++x) {
            const size_t pixel = size_t(y) * k_kspr_atlas_dim + x;
            if (rg[(size_t(y) * stride_pixels + x) * 2 + 1]) source.bits[pixel / 8] |= uint8_t(1u << (pixel % 8));
        }
        source.width = uint16_t(width); source.height = uint16_t(height);
    } catch (...) { source = {}; }
}

uint32_t GLWorldViewRenderer::capture_outline_sprite(const IRWorldKeeperSpriteCmd& cmd,
    kfx::render::SceneOutlineExecutionPath path, bool darkens, const float* rgba,
    float clut_v, kfx::render::SceneOutlineColourResolution resolution, const unsigned char* effective_remap)
{
    if (!m_rt_outline_capture || m_rt_outline_draw == kfx::render::SceneOutlineNoDraw)
        return kfx::render::SceneOutlineNoDraw;
    kfx::render::SceneOutlineSprite sprite;
    sprite.draw = m_rt_outline_draw;
    sprite.dst_x = cmd.dst_x; sprite.dst_y = cmd.dst_y;
    sprite.dst_w = cmd.dst_w; sprite.dst_h = cmd.dst_h;
    sprite.phase_x = cmd.phase_x; sprite.phase_y = cmd.phase_y;
    sprite.step_x = cmd.step_x; sprite.step_y = cmd.step_y;
    sprite.z_ndc = cmd.z_ndc; sprite.outline_z_ndc = cmd.z_ndc + 0.002f;
    sprite.source_alpha = draw_flags_source_weight(cmd.draw_flags);
    sprite.sort_key = cmd.sort_key; sprite.flags = cmd.draw_flags;
    sprite.owner = cmd.owner; sprite.wants_outline = cmd.wants_outline != 0;
    sprite.additive = (cmd.draw_flags & Lb_SPRITE_ALPHA_ADDITIVE) != 0;
    sprite.darkens = darkens; sprite.path = path;
    if (rgba) std::copy_n(rgba, sprite.owner_rgba.size(), sprite.owner_rgba.begin());
    if (sprite.additive)
        return WorldSceneAppendOutlineSprite(m_rt_outline_capture, sprite,
            kfx::render::SceneOutlineColourResolution::Glow, nullptr);
    if (path == kfx::render::SceneOutlineExecutionPath::TextureFallback) {
        if (!m_capture_applied_palette_known) {
            WorldSceneFailOutlineCapture(m_rt_outline_capture, "Applied GL palette upload provenance unavailable");
            return kfx::render::SceneOutlineNoDraw;
        }
        std::array<uint8_t, 1024> row;
        for (size_t i = 0; i < 256; ++i) {
            const size_t source = effective_remap ? effective_remap[i] : i;
            std::copy_n(m_capture_applied_palette.begin() + source * 4, 4, row.begin() + i * 4);
        }
        return WorldSceneAppendOutlineSprite(m_rt_outline_capture, sprite, effective_remap
            ? kfx::render::SceneOutlineColourResolution::TextureRemap
            : kfx::render::SceneOutlineColourResolution::TextureIdentity, &row);
    }
    // Row centres are exact binary fractions (128 rows); this identifies the
    // actual resolver result without resolving again or mutating the cache.
    const unsigned row = unsigned(clut_v * float(k_clut_rows));
    if (row >= m_capture_clut_rows.size() || !m_capture_clut_known[row]) {
        WorldSceneFailOutlineCapture(m_rt_outline_capture, "GL CLUT upload provenance unavailable");
        return kfx::render::SceneOutlineNoDraw;
    }
    return WorldSceneAppendOutlineSprite(m_rt_outline_capture, sprite, resolution, &m_capture_clut_rows[row]);
}

void GLWorldViewRenderer::capture_outline_command(kfx::render::SceneOutlineCommandKind kind,
    uint32_t sprite, uint32_t group)
{
    if (m_rt_outline_capture && sprite != kfx::render::SceneOutlineNoDraw)
        WorldSceneAppendOutlineCommand(m_rt_outline_capture, {kind, sprite, group});
}

void GLWorldViewRenderer::append_keeper_sprite_instance(const IRWorldKeeperSpriteCmd& cmd)
{
    if (cmd.src_w <= 0 || cmd.src_h <= 0 ||
        cmd.src_w > k_kspr_decode_dim || cmd.src_h > k_kspr_decode_dim)
        return;
    if (cmd.dst_w <= 0 || cmd.dst_h <= 0)
        return;

    const bool additive = (cmd.draw_flags & Lb_SPRITE_ALPHA_ADDITIVE) != 0;
    const unsigned char* remap = cmd.remap_enabled ? cmd.remap_table : nullptr;
    const bool use_remap = remap && (cmd.draw_flags & Lb_SPRITE_REMAP) && !additive;

    bool darkens = false;
    const int layer = resolve_atlas_layer(cmd.sprite_id, cmd.data, cmd.data_bytes, cmd.src_w, cmd.src_h, &darkens);
    if (layer < 0)
    {
        // Atlas full: flush what we have so painter's order holds, then draw
        // this one sprite through the legacy per-sprite path.
        flush_keeper_sprite_instances();
        DrawKeeperSpriteGL(cmd);
        return;
    }

    float clut_v = 0.5f / (float)k_clut_rows;
    auto colour_resolution = kfx::render::SceneOutlineColourResolution::ClutIdentity;
    if (use_remap)
        clut_v = resolve_clut_v(remap, colour_resolution);

    const float alpha = draw_flags_source_weight(cmd.draw_flags);

    KsprInstance inst;
    inst.rect[0]  = (float)cmd.dst_x;
    inst.rect[1]  = (float)cmd.dst_y;
    inst.rect[2]  = (float)cmd.dst_w;
    inst.rect[3]  = (float)cmd.dst_h;
    inst.src[0]   = (float)cmd.src_w;
    inst.src[1]   = (float)cmd.content_h;
    inst.map[0]   = (float)cmd.phase_x;
    inst.map[1]   = (float)cmd.phase_y;
    inst.map[2]   = (float)cmd.step_x;
    inst.map[3]   = (float)cmd.step_y;
    inst.layer    = (float)layer;
    inst.clut_v   = clut_v;
    inst.alpha    = alpha;
    inst.z_ndc    = cmd.z_ndc;
    inst.flags    = ((cmd.draw_flags & Lb_SPRITE_FLIP_HORIZ) ? 1u : 0u)
                  | (additive ? 2u : 0u);
    push_keeper_sprite_instance(inst, false);
    if (additive && darkens)
    {
        inst.flags |= 4u;
        push_keeper_sprite_instance(inst, true);
    }

    float captured_rgba[4] = {};
    bool captured_outline = false;
    // Depth-fail outline pass
    if (g_renderer_settings.creature_outline_mode != RENDERER_OUTLINE_NONE && !additive && cmd.wants_outline
        && (m_kspr_inst_outline_shader_handle != kInvalidGpuResource || m_kspr_inst_edge_shader_handle != kInvalidGpuResource))
    {
        // Resolve owner -> player colour index -> linear RGB from the palette.
        float oc_r = 0.9f, oc_g = 0.9f, oc_b = 0.9f;
        if (cmd.owner >= 0)
        {
            unsigned char color_idx = get_player_color_idx((PlayerNumber)cmd.owner);
            if (color_idx < 9)
            {
                uint8_t pal_idx = player_room_colours[color_idx];
                oc_r = (float)RendererPaletteChannel8(m_rt_palette[pal_idx * 3 + 0]) / 255.0f;
                oc_g = (float)RendererPaletteChannel8(m_rt_palette[pal_idx * 3 + 1]) / 255.0f;
                oc_b = (float)RendererPaletteChannel8(m_rt_palette[pal_idx * 3 + 2]) / 255.0f;
            }
        }
        KsprOutlineInstance o;
        memcpy(o.rect, inst.rect, sizeof(o.rect));
        memcpy(o.src,  inst.src,  sizeof(o.src));
        memcpy(o.map,  inst.map,  sizeof(o.map));
        o.layer    = (float)layer;
        // Push the outline slightly farther from the camera so it only appears
        // when the sprite is meaningfully behind geometry, not at tile edges
        // where depth values are nearly equal (avoids stray corner pixels).
        o.z_ndc    = cmd.z_ndc + 0.002f;
        o.flip     = (cmd.draw_flags & Lb_SPRITE_FLIP_HORIZ) ? 1.0f : 0.0f;
        o.color[0] = oc_r;
        o.color[1] = oc_g;
        o.color[2] = oc_b;
        o.color[3] = g_renderer_settings.creature_outline_alpha;
        m_kspr_outline_instances.push_back(o);
        memcpy(captured_rgba, o.color, sizeof(captured_rgba));
        captured_outline = true;
    }
    if (m_rt_outline_capture)
    {
        const uint32_t ref = capture_outline_sprite(cmd,
            kfx::render::SceneOutlineExecutionPath::Instanced, additive && darkens, captured_rgba,
            clut_v, colour_resolution, use_remap ? remap : nullptr);
        try {
            if (m_outline_main_refs.size() + 2 > 196608 || m_outline_refs.size() >= 65536)
                WorldSceneFailOutlineCapture(m_rt_outline_capture, "outline instance reference budget exceeded");
            else {
                m_outline_main_refs.push_back(ref);
                if (additive && darkens) m_outline_main_refs.push_back(ref);
                if (captured_outline) m_outline_refs.push_back(ref);
            }
        } catch (const std::exception&) {
            WorldSceneFailOutlineCapture(m_rt_outline_capture, "outline instance reference allocation failed");
        }
    }
}

void GLWorldViewRenderer::push_keeper_sprite_instance(const KsprInstance& inst, bool darken)
{
    m_kspr_instances.push_back(inst);
    if (m_kspr_instance_runs.empty() || m_kspr_instance_runs.back().darken != darken)
        m_kspr_instance_runs.push_back({0, darken});
    m_kspr_instance_runs.back().count++;
}

void GLWorldViewRenderer::flush_keeper_sprite_instances()
{
    if (m_kspr_instances.empty() && m_kspr_outline_instances.empty())
        return;
    if (!m_resource_mapper) {
        WorldSceneFailOutlineCapture(m_rt_outline_capture, "outline instance mapper missing");
        return;
    }
    const uint32_t capture_group = m_rt_outline_group++;

    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    // Premultiplied alpha: the shader outputs (rgb*a, a) for normal sprites
    // and (rgb, 0) for additive glow, so one blend mode covers both.
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    {
        const GLTexture* clut_tex = m_resource_mapper->ResolveTexture(m_kspr_clut_tex_handle);
        const GLTexture* sprite_array = m_resource_mapper->ResolveTexture(m_kspr_sprite_array_handle);
        if (!clut_tex || !sprite_array)
            WorldSceneFailOutlineCapture(m_rt_outline_capture, "outline instance textures missing");
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, clut_tex ? clut_tex->id : 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, sprite_array ? sprite_array->id : 0);
    }

    // Depth-fail outline pass first
    if (!m_kspr_outline_instances.empty())
    {
        const bool use_edge = (g_renderer_settings.creature_outline_mode == RENDERER_OUTLINE_EDGE);
        GpuResourceHandle prog_handle = use_edge ? m_kspr_inst_edge_shader_handle : m_kspr_inst_outline_shader_handle;
        GLint  vloc = use_edge ? m_kspr_inst_edge_loc_viewport : m_kspr_inst_outline_loc_viewport;
        const GLProgram* prog = m_resource_mapper->ResolveProgram(prog_handle);
        const GLGeometryBuffer* outline_geom = m_resource_mapper->ResolveGeometryBuffer(m_kspr_inst_outline_geom_handle);
        if (prog && outline_geom)
        {
            glDepthFunc(GL_GREATER);
            glUseProgram(prog->id);
            glUniform2f(vloc, (float)m_draw_screen_w, (float)m_draw_screen_h);
            glBindVertexArray(outline_geom->vao);
            glBindBuffer(GL_ARRAY_BUFFER, outline_geom->vbo);
            glBufferData(GL_ARRAY_BUFFER,
                         (GLsizeiptr)(m_kspr_outline_instances.size() * sizeof(KsprOutlineInstance)),
                         m_kspr_outline_instances.data(), GL_STREAM_DRAW);
            if (m_rt_outline_capture) {
                if (m_outline_refs.size() != m_kspr_outline_instances.size())
                    WorldSceneFailOutlineCapture(m_rt_outline_capture, "outline instance references incomplete");
                for (uint32_t ref : m_outline_refs)
                    capture_outline_command(kfx::render::SceneOutlineCommandKind::Outline, ref, capture_group);
            }
            glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4,
                                  (GLsizei)m_kspr_outline_instances.size());
        }
        else WorldSceneFailOutlineCapture(m_rt_outline_capture, "outline instance shader or geometry missing");
    }

    if (!m_kspr_instances.empty())
    {
        const GLProgram* prog = m_resource_mapper->ResolveProgram(m_kspr_inst_shader_handle);
        const GLGeometryBuffer* inst_geom = m_resource_mapper->ResolveGeometryBuffer(m_kspr_inst_geom_handle);
        if (prog && inst_geom)
        {
            glDepthFunc(GL_LEQUAL);
            glUseProgram(prog->id);
            glUniform2f(m_kspr_inst_loc_viewport,
                        (float)m_draw_screen_w, (float)m_draw_screen_h);
            glUniformMatrix3fv(m_kspr_inst_loc_palette_xform, 1, GL_TRUE, &m_rt_palette_xform.m[0][0]);
            glBindVertexArray(inst_geom->vao);
            glBindBuffer(GL_ARRAY_BUFFER, inst_geom->vbo);
            // Runs keep submission order; a darken run subtracts from what is already drawn.
            size_t first = 0;
            for (const KsprInstanceRun& run : m_kspr_instance_runs)
            {
                glBlendEquation(run.darken ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD);
                glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(run.count * sizeof(KsprInstance)),
                             m_kspr_instances.data() + first, GL_STREAM_DRAW);
                if (m_rt_outline_capture) {
                    if (m_outline_main_refs.size() != m_kspr_instances.size())
                        WorldSceneFailOutlineCapture(m_rt_outline_capture, "sprite instance references incomplete");
                    else for (size_t j = first; j < first + run.count; ++j)
                        capture_outline_command(run.darken ? kfx::render::SceneOutlineCommandKind::GlowDarken
                            : kfx::render::SceneOutlineCommandKind::Sprite, m_outline_main_refs[j], capture_group);
                }
                glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)run.count);
                first += run.count;
            }
            glBlendEquation(GL_FUNC_ADD);
        }
        else WorldSceneFailOutlineCapture(m_rt_outline_capture, "sprite instance shader or geometry missing");
    }

    glDepthFunc(GL_LEQUAL);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_BLEND);
    glBindVertexArray(0);
    glUseProgram(0);

    m_kspr_instances.clear();
    m_kspr_instance_runs.clear();
    m_kspr_outline_instances.clear();
    m_outline_main_refs.clear();
    m_outline_refs.clear();
}

int GLWorldViewRenderer::render_keepersprite_gpu(const IRWorldKeeperSpriteCmd& cmd)
{
    const unsigned char* data = cmd.data;
    const int src_w = cmd.src_w;
    const int src_h = cmd.src_h;
    int32_t content_h = cmd.content_h;
    const unsigned int draw_flags = cmd.draw_flags;
    const unsigned char* remap = cmd.remap_enabled ? cmd.remap_table : nullptr;
    const float z_ndc = cmd.z_ndc;
    const int sprite_owner = (int)cmd.owner;
    const int sprite_wants_outline = (int)cmd.wants_outline;
    const int32_t sprite_id = cmd.sprite_id;
    // No mapper, no run.
    if (!m_resource_mapper) {
        WorldSceneFailOutlineCapture(m_rt_outline_capture, "fallback sprite mapper missing");
        return 1;
    }
    const GLProgram* kspr_prog = m_resource_mapper->ResolveProgram(m_kspr_shader_handle);
    const GLTexture* kspr_sprite_tex = m_resource_mapper->ResolveTexture(m_kspr_sprite_tex_handle);
    const GLGeometryBuffer* kspr_geom = m_resource_mapper->ResolveGeometryBuffer(m_kspr_geom_handle);
    if (!kspr_prog || !kspr_sprite_tex || !kspr_geom || m_palette_tex_handle == kInvalidGpuResource) {
        static int s_miss = 0;
        if (s_miss++ < 5)
            ERRORLOG("render_keepersprite_gpu: GL resources not ready -- sprite dropped");
        WorldSceneFailOutlineCapture(m_rt_outline_capture, "fallback sprite resources missing");
        return 1;
    }
    const GLProgram* kspr_glow_prog = m_resource_mapper->ResolveProgram(m_kspr_glow_shader_handle);
    const GLProgram* kspr_atlas_glow_prog = m_resource_mapper->ResolveProgram(m_kspr_atlas_glow_shader_handle);
    const GLProgram* kspr_atlas_prog = m_resource_mapper->ResolveProgram(m_kspr_atlas_shader_handle);
    const GLTexture* kspr_sprite_array = m_resource_mapper->ResolveTexture(m_kspr_sprite_array_handle);
    const GLTexture* kspr_clut_tex = m_resource_mapper->ResolveTexture(m_kspr_clut_tex_handle);
    const GLProgram* kspr_outline_prog = m_resource_mapper->ResolveProgram(m_kspr_outline_shader_handle);
    const GLProgram* kspr_atlas_outline_prog = m_resource_mapper->ResolveProgram(m_kspr_atlas_outline_shader_handle);
    const GLProgram* kspr_edge_prog = m_resource_mapper->ResolveProgram(m_kspr_edge_shader_handle);
    const GLProgram* kspr_atlas_edge_prog = m_resource_mapper->ResolveProgram(m_kspr_atlas_edge_shader_handle);
    if (src_w <= 0 || src_h <= 0 || src_w > k_kspr_decode_dim || src_h > k_kspr_decode_dim) {
        static int s_dim = 0;
        if (s_dim++ < 20)
            WARNLOG("render_keepersprite_gpu: invalid sprite dimensions %dx%d (max %d) -- dropped",
                    src_w, src_h, k_kspr_decode_dim);
        return 1;
    }
    if (cmd.dst_w <= 0 || cmd.dst_h <= 0) return 1;
    if (content_h <= 0 || content_h > src_h) content_h = src_h;

    const bool additive = (draw_flags & Lb_SPRITE_ALPHA_ADDITIVE) != 0;
    const bool use_remap = remap && (draw_flags & Lb_SPRITE_REMAP) && !additive;
    int atlas_layer = resolve_atlas_layer(sprite_id, data, cmd.data_bytes, src_w, src_h);
    // Validate/decode before touching draw state, including the outline path.
    // Both fallback draws below reuse this full initialized image.
    if (atlas_layer < 0 && !decode_keeper_rle(s_kspr_decode_buf, data, cmd.data_bytes, src_w, src_h)) {
        WorldSceneFailOutlineCapture(m_rt_outline_capture, "Malformed GL keeper sprite source");
        return 1;
    }

    float clut_v = 0.5f / (float)k_clut_rows;
    auto colour_resolution = kfx::render::SceneOutlineColourResolution::ClutIdentity;
    if (atlas_layer >= 0 && use_remap && kspr_clut_tex)
        clut_v = resolve_clut_v(remap, colour_resolution);

    const float vx0 = (float)cmd.dst_x, vy0 = (float)cmd.dst_y;
    const float vw  = (float)cmd.dst_w, vh  = (float)cmd.dst_h;
    const float mpx = (float)cmd.phase_x, mpy = (float)cmd.phase_y;
    const float msx = (float)cmd.step_x,  msy = (float)cmd.step_y;
    const float sw  = (float)src_w, sh = (float)content_h;
    const float flip = (draw_flags & Lb_SPRITE_FLIP_HORIZ) ? 1.0f : 0.0f;

    float sv[6][11] = {
        { vx0,      vy0,      0.0f, 0.0f, mpx, mpy, msx, msy, sw, sh, flip },
        { vx0 + vw, vy0,      vw,   0.0f, mpx, mpy, msx, msy, sw, sh, flip },
        { vx0 + vw, vy0 + vh, vw,   vh,   mpx, mpy, msx, msy, sw, sh, flip },
        { vx0,      vy0,      0.0f, 0.0f, mpx, mpy, msx, msy, sw, sh, flip },
        { vx0 + vw, vy0 + vh, vw,   vh,   mpx, mpy, msx, msy, sw, sh, flip },
        { vx0,      vy0 + vh, 0.0f, vh,   mpx, mpy, msx, msy, sw, sh, flip },
    };
    glBindBuffer(GL_ARRAY_BUFFER, kspr_geom->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(sv), nullptr, GL_DYNAMIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(sv), sv);

    const float alpha = draw_flags_source_weight(draw_flags);

    // Additive glow
    const bool use_glow_atlas    = additive && atlas_layer >= 0 && kspr_atlas_glow_prog;
    const bool use_glow_fallback = additive && atlas_layer <  0 && kspr_glow_prog;
    const GLenum blend_dfactor = additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA;
    const uint32_t capture_group = m_rt_outline_group++;
    uint32_t capture_ref = kfx::render::SceneOutlineNoDraw;
    bool capture_started = false;
    float captured_rgba[4] = {};
    auto capture_pass = [&](kfx::render::SceneOutlineCommandKind kind) {
        if (!m_rt_outline_capture) return;
        if (!capture_started) {
            capture_ref = capture_outline_sprite(cmd, atlas_layer >= 0
                ? kfx::render::SceneOutlineExecutionPath::AtlasFallback
                : kfx::render::SceneOutlineExecutionPath::TextureFallback,
                use_glow_atlas || use_glow_fallback, captured_rgba,
                clut_v, colour_resolution, use_remap ? remap : nullptr);
            capture_started = true;
        }
        capture_outline_command(kind, capture_ref, capture_group);
    };
    if (additive && !use_glow_atlas && !use_glow_fallback)
        WorldSceneFailOutlineCapture(m_rt_outline_capture, "fallback glow shader missing");
    if (atlas_layer >= 0 && (!kspr_sprite_array || (!additive && (!kspr_atlas_prog || !kspr_clut_tex))))
        WorldSceneFailOutlineCapture(m_rt_outline_capture, "fallback atlas resources missing");
    // Draws the black pixels of the glow quad just drawn, subtracted.
    auto draw_glow_darken_pass = [&](GLint loc_darken) {
        glUniform1i(loc_darken, 1);
        glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
        capture_pass(kfx::render::SceneOutlineCommandKind::GlowDarken);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glBlendEquation(GL_FUNC_ADD);
    };

    glBindVertexArray(kspr_geom->vao);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);

    // ── Depth-fail outline pass ──────────────────────────────────────
    // Draws a flat owner-colour silhouette only where the sprite is BEHIND
    // geometry (GL_GREATER depth test). The normal draw below then paints
    // over the visible portion with GL_LEQUAL, leaving the outline only
    // peeking out from behind walls/columns.
    const bool wants_outline = sprite_wants_outline != 0;
    if (g_renderer_settings.creature_outline_mode != RENDERER_OUTLINE_NONE && !additive && wants_outline)
    {
        // Resolve owner -> player colour index -> linear RGB from the palette.
        float oc_r = 0.9f, oc_g = 0.9f, oc_b = 0.9f;
        if (sprite_owner >= 0)
        {
            unsigned char color_idx = get_player_color_idx((PlayerNumber)sprite_owner);
            if (color_idx < 9)
            {
                uint8_t pal_idx = player_room_colours[color_idx];
                oc_r = (float)RendererPaletteChannel8(m_rt_palette[pal_idx * 3 + 0]) / 255.0f;
                oc_g = (float)RendererPaletteChannel8(m_rt_palette[pal_idx * 3 + 1]) / 255.0f;
                oc_b = (float)RendererPaletteChannel8(m_rt_palette[pal_idx * 3 + 2]) / 255.0f;
            }
        }
        const float oc_a = g_renderer_settings.creature_outline_alpha;

        glDepthFunc(GL_GREATER);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Push the outline slightly farther from the camera so it only appears
        // when the sprite is meaningfully behind geometry, not at tile edges
        // where depth values are nearly equal (avoids stray corner pixels).
        const float outline_z = z_ndc + 0.002f;

        const bool use_edge = (g_renderer_settings.creature_outline_mode == RENDERER_OUTLINE_EDGE);

        if (atlas_layer >= 0)
        {
            // Select edge vs silhouette shader for the atlas path.
            const GLProgram* prog = use_edge ? kspr_atlas_edge_prog : kspr_atlas_outline_prog;
            GLint  loc_vp  = use_edge ? m_kspr_atlas_edge_loc_viewport   : m_kspr_atlas_outline_loc_viewport;
            GLint  loc_z   = use_edge ? m_kspr_atlas_edge_loc_z_ndc      : m_kspr_atlas_outline_loc_z_ndc;
            GLint  loc_lay = use_edge ? m_kspr_atlas_edge_loc_layer      : m_kspr_atlas_outline_loc_layer;
            GLint  loc_col = use_edge ? m_kspr_atlas_edge_loc_color      : m_kspr_atlas_outline_loc_color;
            if (prog && kspr_sprite_array)
            {
                // Atlas path: texture already cached in kspr_sprite_array.
                glUseProgram(prog->id);
                glUniform2f(loc_vp, (float)m_draw_screen_w, (float)m_draw_screen_h);
                glUniform1f(loc_z,    outline_z);
                glUniform1f(loc_lay,  (float)atlas_layer);
                glUniform4f(loc_col,  oc_r, oc_g, oc_b, oc_a);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D_ARRAY, kspr_sprite_array->id);
                captured_rgba[0] = oc_r; captured_rgba[1] = oc_g;
                captured_rgba[2] = oc_b; captured_rgba[3] = oc_a;
                capture_pass(kfx::render::SceneOutlineCommandKind::Outline);
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }
            else WorldSceneFailOutlineCapture(m_rt_outline_capture, "fallback atlas outline resources missing");
        }
        else
        {
            // Fallback path: decode + upload now so the outline has pixel data.
            // The normal draw below will re-use the already-uploaded texture.
            const GLProgram* prog = use_edge ? kspr_edge_prog : kspr_outline_prog;
            GLint  loc_vp  = use_edge ? m_kspr_edge_loc_viewport   : m_kspr_outline_loc_viewport;
            GLint  loc_z   = use_edge ? m_kspr_edge_loc_z_ndc      : m_kspr_outline_loc_z_ndc;
            GLint  loc_col = use_edge ? m_kspr_edge_loc_color      : m_kspr_outline_loc_color;
            if (prog)
            {
                if (use_remap)
                {
                    for (int oy = 0; oy < src_h; ++oy) {
                        uint8_t* orow = s_kspr_decode_buf + oy * k_kspr_decode_dim * 2;
                        for (int ox = 0; ox < src_w; ++ox)
                            if (orow[ox * 2 + 1] != 0) orow[ox * 2] = remap[orow[ox * 2]];
                    }
                }
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, kspr_sprite_tex->id);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, k_kspr_decode_dim);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, src_w, src_h,
                                GL_RG, GL_UNSIGNED_BYTE, s_kspr_decode_buf);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

                glUseProgram(prog->id);
                glUniform2f(loc_vp, (float)m_draw_screen_w, (float)m_draw_screen_h);
                glUniform1f(loc_z,    outline_z);
                glUniform4f(loc_col,  oc_r, oc_g, oc_b, oc_a);
                captured_rgba[0] = oc_r; captured_rgba[1] = oc_g;
                captured_rgba[2] = oc_b; captured_rgba[3] = oc_a;
                capture_pass(kfx::render::SceneOutlineCommandKind::Outline);
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }
            else WorldSceneFailOutlineCapture(m_rt_outline_capture, "fallback outline shader missing");
        }

        glDepthFunc(GL_LEQUAL); // restore for normal draw below
    }

    if (atlas_layer >= 0) {
        if (use_glow_atlas) {
            // Additive glow via atlas: no CLUT/palette needed, glow math in shader.
            glUseProgram(kspr_atlas_glow_prog->id);
            glUniform2f(m_kspr_atlas_glow_loc_viewport, (float)m_draw_screen_w, (float)m_draw_screen_h);
            glUniform1f(m_kspr_atlas_glow_loc_z_ndc, z_ndc);
            glUniform1f(m_kspr_atlas_glow_loc_layer, (float)atlas_layer);
            glUniformMatrix3fv(m_kspr_atlas_glow_loc_palette_xform, 1, GL_TRUE, &m_rt_palette_xform.m[0][0]);
            glUniform1i(m_kspr_atlas_glow_loc_darken, 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D_ARRAY, kspr_sprite_array ? kspr_sprite_array->id : 0);
            glBlendFunc(GL_ONE, GL_ONE);
        } else {
            // Normal or remapped sprite via atlas + CLUT.
            glUseProgram(kspr_atlas_prog->id);
            glUniform2f(m_kspr_atlas_loc_viewport, (float)m_draw_screen_w, (float)m_draw_screen_h);
            glUniform1f(m_kspr_atlas_loc_alpha, alpha);
            glUniform1f(m_kspr_atlas_loc_z_ndc, z_ndc);
            glUniform1f(m_kspr_atlas_loc_layer, (float)atlas_layer);
            glUniform1f(m_kspr_atlas_loc_clut_v, clut_v);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, kspr_clut_tex ? kspr_clut_tex->id : 0);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D_ARRAY, kspr_sprite_array ? kspr_sprite_array->id : 0);
            glBlendFunc(GL_SRC_ALPHA, blend_dfactor);
        }
        capture_pass(kfx::render::SceneOutlineCommandKind::Sprite);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        if (use_glow_atlas)
            draw_glow_darken_pass(m_kspr_atlas_glow_loc_darken);
        glDisable(GL_BLEND);
        glBindVertexArray(0);
        glUseProgram(0);
        return 1;
    }

    // Atlas miss/full/unknown-id: decode into the reusable single texture.
    // If the outline block above already decoded + uploaded the sprite
    // (fallback-path outline), skip the redundant decode/upload here.
    const bool outline_uploaded = g_renderer_settings.creature_outline_mode != RENDERER_OUTLINE_NONE
                                   && !additive
                                   && wants_outline
                                   && atlas_layer < 0
                                   && (kspr_outline_prog || kspr_edge_prog);
    if (!outline_uploaded)
    {
        if (use_remap)
        {
            for (int y = 0; y < src_h; ++y) {
                uint8_t* row = s_kspr_decode_buf + y * k_kspr_decode_dim * 2;
                for (int x = 0; x < src_w; ++x)
                    if (row[x * 2 + 1] != 0) row[x * 2] = remap[row[x * 2]];
            }
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, kspr_sprite_tex->id);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, k_kspr_decode_dim);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, src_w, src_h,
                        GL_RG, GL_UNSIGNED_BYTE, s_kspr_decode_buf);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }

    if (use_glow_fallback)
    {
        // Pure additive blend: adds the glow RGB delta to the framebuffer.
        glUseProgram(kspr_glow_prog->id);
        glUniform2f(m_kspr_glow_loc_viewport, (float)m_draw_screen_w, (float)m_draw_screen_h);
        glUniform1f(m_kspr_glow_loc_z_ndc,    z_ndc);
        glUniformMatrix3fv(m_kspr_glow_loc_palette_xform, 1, GL_TRUE, &m_rt_palette_xform.m[0][0]);
        glUniform1i(m_kspr_glow_loc_darken, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, kspr_sprite_tex->id);
        glBlendFunc(GL_ONE, GL_ONE);
    }
    else
    {
        glUseProgram(kspr_prog->id);
        glUniform2f(m_kspr_loc_viewport, (float)m_draw_screen_w, (float)m_draw_screen_h);
        glUniform1f(m_kspr_loc_alpha,    alpha);
        glUniform1f(m_kspr_loc_z_ndc,    z_ndc);
        glActiveTexture(GL_TEXTURE1);
        const GLuint applied_palette = ResolvePaletteTexId();
        if (!applied_palette)
            WorldSceneFailOutlineCapture(m_rt_outline_capture, "Fallback palette texture unavailable");
        glBindTexture(GL_TEXTURE_2D, applied_palette);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, kspr_sprite_tex->id);
        glBlendFunc(GL_SRC_ALPHA, blend_dfactor);
    }

    capture_pass(kfx::render::SceneOutlineCommandKind::Sprite);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    if (use_glow_fallback)
        draw_glow_darken_pass(m_kspr_glow_loc_darken);

    glDisable(GL_BLEND);
    glBindVertexArray(0);
    glUseProgram(0);
    return 1;
}

void GLWorldViewRenderer::DrawKeeperSpriteGL(const IRWorldKeeperSpriteCmd& cmd)
{
    ASSERT_RENDER_THREAD();
    render_keepersprite_gpu(cmd);
}

void GLWorldViewRenderer::DrawCursorKeeperSprites()
{
    ASSERT_RENDER_THREAD();
    if (m_rt_cursor_kspr_ir.empty())
        return;
    ensure_clut_valid();

    const int saved_draw_w = m_draw_screen_w;
    const int saved_draw_h = m_draw_screen_h;
    const float saved_z    = m_current_sprite_z;

    // Cursor sprites are full-screen (mouse coords, not 3D-viewport-relative)
    // and must always render on top -- force full-window size/near-z before
    // drawing, same as develop's DrawCursorKeeperSprites().
    m_draw_screen_w    = m_full_screen_w;
    m_draw_screen_h    = m_full_screen_h;
    m_current_sprite_z = -1.0f;

    glViewport(0, 0, m_full_screen_w, m_full_screen_h);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    for (const IRWorldKeeperSpriteCmd& cmd : m_rt_cursor_kspr_ir)
        append_keeper_sprite_instance(cmd);
    flush_keeper_sprite_instances();

    glDepthMask(GL_TRUE);

    m_draw_screen_w    = saved_draw_w;
    m_draw_screen_h    = saved_draw_h;
    m_current_sprite_z = saved_z;
}

void GLWorldViewRenderer::gpu_flush()
{
    if (!m_initialized || m_vert_count <= m_cmd_vert_start)
        return;

    // Record the current tile batch as a deferred draw command. No GL calls
    // are issued here — everything is replayed in GPURenderNow().
    DrawCmd cmd;
    cmd.type       = DrawCmd::CMD_TILES;
    cmd.vert_start = m_cmd_vert_start;
    cmd.vert_count = m_vert_count - m_cmd_vert_start;
    m_draw_cmds.push_back(cmd);
    m_cmd_vert_start = m_vert_count;
}

void GLWorldViewRenderer::FlipBuffers()
{
    ASSERT_GAME_THREAD();
    if (m_rt_outline_capture)
        WorldSceneFailOutlineCapture(m_rt_outline_capture, "outline session replaced before execution");
    m_rt_outline_capture.reset();
    if (m_world_write_cmds && m_world_write_cmds->scene)
        m_rt_outline_capture = WorldSceneTakeOutlineCapture(m_world_write_cmds->scene->epoch,
            m_world_write_cmds->scene->sequence);
    m_rt_outline_sprite_tags = std::move(m_outline_sprite_tags);
    m_rt_outline_tile_tags = std::move(m_outline_tile_tags);
    m_outline_sprite_tags.clear();
    m_outline_tile_tags.clear();
    m_rt_draw_cmds      = std::move(m_draw_cmds);
    m_rt_kspr_ir        = std::move(m_kspr_ir);
    m_rt_cursor_kspr_ir = std::move(m_cursor_kspr_ir);
    m_rt_shadow_cmds    = std::move(m_shadow_cmds);
    m_rt_lens_cmd    = std::move(m_lens_cmd);
    m_lens_cmd = IRWorldLensCmd{};
    m_rt_screen_w  = m_screen_w;
    m_rt_screen_h  = m_screen_h;
    m_rt_vp_x      = m_vp_x;
    m_rt_vp_y      = m_vp_y;

    uint8_t palette[768] = {};
    if (m_palette_data)
        memcpy(palette, m_palette_data, sizeof(palette));
    if (memcmp(palette, m_rt_palette, sizeof(palette)) != 0)
    {
        memcpy(m_rt_palette, palette, sizeof(m_rt_palette));
        m_rt_palette_xform = PaletteTransformFit(engine_palette, m_rt_palette);
    }

    // Snapshot the lightmap so the render thread never reads the live game array.
    snapshot_lightmap(m_rt_lightmap, k_lightmap_w, k_lightmap_h,
        game.lish.subtile_lightness, std::max(1, (int)game.map_subtiles_x + 1),
        std::max(1, (int)game.map_subtiles_y + 1));

    m_vert_count     = 0;
    m_cmd_vert_start = 0;
    // m_kspr_ir is already empty after std::move above.
}

/******************************************************************************/

void GLWorldViewRenderer::GPURenderNow(const WorldCommandBuffers& cmds)
{
    ASSERT_RENDER_THREAD();
    if (cmds.scene) WorldSceneValidateConsumed(*cmds.scene);
    m_rt_outline_draw = kfx::render::SceneOutlineNoDraw;
    m_rt_outline_group = 0;
    m_outline_main_refs.clear();
    m_outline_refs.clear();
    if (m_rt_outline_capture) {
        GLint depth_bits = 0, framebuffer = 0;
        GLdouble depth_range[2] = {}, depth_clear = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,
            framebuffer ? GL_DEPTH_ATTACHMENT : GL_DEPTH,
            GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &depth_bits);
        glGetDoublev(GL_DEPTH_RANGE, depth_range);
        glGetDoublev(GL_DEPTH_CLEAR_VALUE, &depth_clear);
        if (depth_bits != 24 || depth_range[0] != 0 || depth_range[1] != 1 || depth_clear != 1)
            WorldSceneFailOutlineCapture(m_rt_outline_capture, "GL outline capture requires canonical D24 depth state");
        std::array<float, 9> transform{};
        std::array<uint8_t, 768> palette{};
        memcpy(transform.data(), &m_rt_palette_xform.m[0][0], sizeof(transform));
        memcpy(palette.data(), m_rt_palette, sizeof(palette));
        WorldSceneBeginOutlineCapture(m_rt_outline_capture,
            (uint8_t)g_renderer_settings.creature_outline_mode,
            g_renderer_settings.creature_outline_alpha, transform, palette);
    }
    auto finish_capture = [&]() {
        m_capture_world_target_known = false;
        WorldSceneFinishOutlineCapture(m_rt_outline_capture);
        m_rt_outline_draw = kfx::render::SceneOutlineNoDraw;
        m_rt_outline_sprite_tags.clear();
        m_rt_outline_tile_tags.clear();
        m_outline_main_refs.clear();
        m_outline_refs.clear();
    };

    m_world_pass_active = false;

    if (!m_initialized)
    {
        WorldSceneFailOutlineCapture(m_rt_outline_capture, "GL world renderer not initialized");
        finish_capture();
        m_rt_draw_cmds.clear();
        m_cmd_vert_start = 0;
        return;
    }

    if (m_rt_draw_cmds.empty()) {
        WorldSceneFailOutlineCapture(m_rt_outline_capture, "GL world command stream empty");
        finish_capture();
        return;
    }

    // Settings can change while the simulation is paused. Refresh on the GL
    // thread before drawing; unchanged settings issue no additional GL calls.
    push_shade_uniforms();
    const int vp_y_gl = m_full_screen_h - m_rt_vp_y - m_rt_screen_h;
    gpu_execute_passes(m_rt_vp_x, vp_y_gl, m_rt_screen_w, m_rt_screen_h, cmds.tile_verts);
    finish_capture();
}

void GLWorldViewRenderer::gpu_execute_passes(int vp_x, int vp_y_gl, int screen_w, int screen_h,
                                             const std::vector<WorldVertex>& tile_verts)
{
    KFX_ZONE_COLOR("GLWorldViewRenderer::gpu_execute_passes", KFX_COLOR_RENDER_GPU);
    KFX_GPU_ZONE("World Draw");

    m_draw_screen_w = screen_w;
    m_draw_screen_h = screen_h;

    m_kspr_atlas_hits   = 0;
    m_kspr_atlas_misses = 0;

    ensure_clut_valid();  // identity row 0 must exist before any sprite draws

    if (!m_resource_mapper) {
        WorldSceneFailOutlineCapture(m_rt_outline_capture, "GL world resource mapper missing");
        return;
    }
    const GLGeometryBuffer* const world_geom = m_resource_mapper->ResolveGeometryBuffer(m_geom_handle);
    const GLuint world_shader_id = ResolveShaderId(m_shader_handle);
    const GLTexture* const lightmap_tex = m_resource_mapper->ResolveTexture(m_lightmap_tex_handle);
    if (!world_geom || !world_shader_id || !lightmap_tex) {
        WorldSceneFailOutlineCapture(m_rt_outline_capture, "GL world resources missing");
        return;
    }

    glBindBuffer(GL_ARRAY_BUFFER, world_geom->vbo);
    // Re-specify the storage each frame so it always fits the frame's vertices.
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)(tile_verts.size() * sizeof(WorldVertex)),
                 tile_verts.data(), GL_STREAM_DRAW);

    glViewport(vp_x, vp_y_gl, screen_w, screen_h);
    glUseProgram(world_shader_id);
    glBindVertexArray(world_geom->vao);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);

    bool atlas_bound       = false;  // tile atlas array bound to GL_TEXTURE0
    bool capture_missing_tile = false;

    // Upload lightmap shadow copy (snapshotted from game.lish.subtile_lightness[]
    // during FlipBuffers() on the game thread — no live game struct access here).
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, lightmap_tex->id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, k_lightmap_w, k_lightmap_h,
                    GL_RED_INTEGER, GL_UNSIGNED_SHORT,
                    m_rt_lightmap);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);  // restore default

    if (m_rt_outline_capture) {
        if (!m_capture_world_target_known || !m_capture_applied_palette_known || !m_capture_applied_fade_known || !m_shade_settings_applied
            || !ResolvePaletteTexId() || !ResolveFadeTexId())
            WorldSceneFailOutlineCapture(m_rt_outline_capture, "Complete GL world clear, palette or fade upload proof unavailable");
        else try {
            auto state = m_capture_world_target;
            state.viewport_x = m_rt_vp_x; state.viewport_y = m_rt_vp_y;
            state.viewport_width = screen_w; state.viewport_height = screen_h;
            state.palette_rgba = m_capture_applied_palette; state.fade = m_capture_applied_fade;
            state.shade_settings = m_applied_shade_settings;
            state.lightmap.assign(m_rt_lightmap, m_rt_lightmap + k_lightmap_w * k_lightmap_h);
            WorldSceneBeginGLWorldCapture(m_rt_outline_capture, std::move(state));
        } catch (...) { WorldSceneFailOutlineCapture(m_rt_outline_capture, "GL world state allocation failed"); }
    }

    // Bind fade table to unit 3 unconditionally — the sampler must always
    // reference a valid texture even when darkness_mode != PALETTE.
    if (m_fade_tex_handle != kInvalidGpuResource)
    {
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, ResolveFadeTexId());
    }

    glActiveTexture(GL_TEXTURE0);  // restore default active texture unit

    // Bind palette once for the entire pass — it never changes mid-frame.
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ResolvePaletteTexId());

    // Opaque geometry only (tiles, flat-colour polys) -- no shadow/sprite
    // passes in this slice.
    for (const auto& cmd : m_rt_draw_cmds)
    {
        if (cmd.type == DrawCmd::CMD_TILES)
        {
            if (!atlas_bound)
            {
                GLuint atlas_tex = 0;
                if (m_atlas && m_atlas->IsInitialized() && m_resource_mapper)
                {
                    const GLTexture* const tex = m_resource_mapper->ResolveTexture(m_atlas->GetAtlasTextureArray());
                    atlas_tex = tex ? tex->id : 0;
                }
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, 0);
                glBindTexture(GL_TEXTURE_2D_ARRAY, atlas_tex);
                glUniform1f(m_loc_missing_tile, (atlas_tex == 0) ? 1.0f : 0.0f);
                capture_missing_tile = atlas_tex == 0;
                atlas_bound = true;
                if (m_tile_filter_applied != g_renderer_settings.tile_filter)
                {
                    const GLint filter = (g_renderer_settings.tile_filter == RENDERER_FILTER_LINEAR)
                                        ? GL_LINEAR : GL_NEAREST;
                    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, filter);
                    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, filter);
                    m_tile_filter_applied = g_renderer_settings.tile_filter;
                }
            }
            glDepthFunc(GL_ALWAYS);
            if (m_rt_outline_capture) {
                if (cmd.vert_start < 0 || cmd.vert_count < 0 || cmd.vert_start % 3 || cmd.vert_count % 3
                    || uint64_t(cmd.vert_start) + uint64_t(cmd.vert_count) > tile_verts.size())
                    WorldSceneFailOutlineCapture(m_rt_outline_capture, "GL occluder range invalid");
                else for (int i = cmd.vert_start; i < cmd.vert_start + cmd.vert_count; i += 3) {
                    const size_t tag = size_t(i / 3);
                    if (tag >= m_rt_outline_tile_tags.size()
                        || m_rt_outline_tile_tags[tag] == kfx::render::SceneOutlineNoDraw) {
                        WorldSceneFailOutlineCapture(m_rt_outline_capture, "GL occluder entry missing");
                        continue;
                    }
                    kfx::render::SceneOutlineOccluder triangle;
                    kfx::render::SceneGLTerrainTriangle material;
                    triangle.entry = m_rt_outline_tile_tags[tag];
                    for (int j = 0; j < 3; ++j) {
                        const auto& v = tile_verts[size_t(i + j)];
                        triangle.vertices[j] = {v.x, v.y, v.z, v.camera_z};
                        material.vertices[j] = {v.u, v.v, v.shade, v.stl_x, v.stl_y};
                    }
                    std::array<uint8_t, 1024> pixels{};
                    const auto& first = tile_verts[size_t(i)];
                    const auto* uploaded_atlas = dynamic_cast<const GLTileAtlas*>(m_atlas);
                    bool known = capture_missing_tile;
                    if (!capture_missing_tile && uploaded_atlas && first.atlas_layer >= 0 && first.atlas_layer < 32
                        && first.tile >= 0 && first.tile < 2048
                        && first.atlas_layer == float(unsigned(first.atlas_layer)) && first.tile == float(unsigned(first.tile))) {
                        known = true;
                        for (int j = 1; j < 3; ++j)
                            known &= tile_verts[size_t(i+j)].atlas_layer == first.atlas_layer && tile_verts[size_t(i+j)].tile == first.tile;
                        known &= uploaded_atlas->CopyUploadedTileForCapture(unsigned(first.atlas_layer), unsigned(first.tile), pixels);
                    }
                    if (!known) WorldSceneFailOutlineCapture(m_rt_outline_capture, "Complete GL terrain actual tile upload unavailable");
                    else WorldSceneAppendGLTerrain(m_rt_outline_capture, triangle, material, capture_missing_tile ? nullptr : &pixels);
                }
            }
            glDrawArrays(GL_TRIANGLES, cmd.vert_start, cmd.vert_count);
        }
        else if (cmd.type == DrawCmd::CMD_PRELOAD_KSPR_ATLAS)
        {
            execute_preload_atlas();
            // Restore tile shader and VAO that were active before the preload.
            glUseProgram(world_shader_id);
            glBindVertexArray(world_geom->vao);
            atlas_bound = false;
        }
        else if (cmd.type == DrawCmd::CMD_CLEAR_KSPR_ATLAS)
        {
            // Pure CPU-side cache reset, no GL calls -- unlike preload, no
            // shader/VAO state to restore afterward.
            execute_clear_atlas();
        }
        else if (cmd.type == DrawCmd::CMD_UPDATE_ANIMATED_TILES)
        {
            if (m_atlas)
                m_atlas->UpdateAnimatedTiles();
        }
    }

    glDepthFunc(GL_LEQUAL);

    // ── Creature shadows  ───────────────────────────────────────────────
    draw_shadows_gpu();
    glUseProgram(world_shader_id);
    glBindVertexArray(world_geom->vao);

    // ── Keeper sprites ────────────────────────────────────────────────────────
    // Scissor-clip to the world viewport so sprites can't bleed into the
    // sidebar region -- without it the GPU still rasterises (and
    // alpha-blends) sprites behind the sidebar UI.
    {
        bool any_sprites = false;
        for (const auto& cmd : m_rt_draw_cmds)
            if (cmd.type == DrawCmd::CMD_IR_KEEPER_SPRITES) { any_sprites = true; break; }

        if (any_sprites)
        {
            glEnable(GL_SCISSOR_TEST);
            glScissor(vp_x, vp_y_gl, screen_w, screen_h);
            glBindVertexArray(0);
            glUseProgram(0);

            auto sort_range = [&](const DrawCmd& cmd) -> const std::vector<int>& {
                m_kspr_sorted_idx.clear();
                const int end = cmd.sprite_ir_start + cmd.sprite_ir_count;
                for (int i = cmd.sprite_ir_start; i < end; ++i)
                    m_kspr_sorted_idx.push_back(i);
                std::stable_sort(m_kspr_sorted_idx.begin(), m_kspr_sorted_idx.end(),
                                 [this](int a, int b) {
                                     // Far buckets first; within a bucket, keep submission
                                     // order to match the software bucket walk.
                                     const uint64_t ka = m_rt_kspr_ir[a].sort_key;
                                     const uint64_t kb = m_rt_kspr_ir[b].sort_key;
                                     if ((ka >> 32) != (kb >> 32))
                                         return (ka >> 32) > (kb >> 32);
                                     return (uint32_t)ka < (uint32_t)kb;
                                 });
                return m_kspr_sorted_idx;
            };

            const bool use_instancing = (m_kspr_inst_shader_handle != kInvalidGpuResource)
                                      && (m_kspr_sprite_array_handle != kInvalidGpuResource);
            if (use_instancing)
            {
                m_kspr_instances.clear();
                for (const auto& cmd : m_rt_draw_cmds)
                {
                    if (cmd.type != DrawCmd::CMD_IR_KEEPER_SPRITES) continue;
                    for (int i : sort_range(cmd)) {
                        m_rt_outline_draw = size_t(i) < m_rt_outline_sprite_tags.size()
                            ? m_rt_outline_sprite_tags[size_t(i)] : kfx::render::SceneOutlineNoDraw;
                        append_keeper_sprite_instance(m_rt_kspr_ir[i]);
                    }
                }
                flush_keeper_sprite_instances();
            }
            else
            {
                for (const auto& cmd : m_rt_draw_cmds)
                {
                    if (cmd.type != DrawCmd::CMD_IR_KEEPER_SPRITES) continue;
                    for (int i : sort_range(cmd)) {
                        m_rt_outline_draw = size_t(i) < m_rt_outline_sprite_tags.size()
                            ? m_rt_outline_sprite_tags[size_t(i)] : kfx::render::SceneOutlineNoDraw;
                        DrawKeeperSpriteGL(m_rt_kspr_ir[i]);
                    }
                }
            }

            m_rt_outline_draw = kfx::render::SceneOutlineNoDraw;
            glDisable(GL_SCISSOR_TEST);
            glUseProgram(world_shader_id);
            glBindVertexArray(world_geom->vao);
            // Sprite pass leaves unit 1 bound to m_kspr_clut_tex. The tile
            // shader samples unit 1 as the 256x1 palette -- restore it so a
            // subsequent CMD_TILES batch (front view after iso, etc.)
            // doesn't sample the wrong row of the CLUT.
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, ResolvePaletteTexId());
            glActiveTexture(GL_TEXTURE0);
        }
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glDepthMask(GL_TRUE);
    glDisable(GL_DEPTH_TEST);
    glViewport(0, 0, m_full_screen_w, m_full_screen_h);

    m_rt_draw_cmds.clear();
    m_rt_kspr_ir.clear();
    m_rt_shadow_cmds.clear();
    m_cmd_vert_start = 0;
}

/******************************************************************************/

void GLWorldViewRenderer::DrawIsometricView()
{
    if (!m_initialized)
    {
        ERRORLOG("GLWorldViewRenderer asked to draw ISO but not initialised — frame dropped");
        return;
    }
    if (!m_world_write_cmds)
        return;

    render_fade_tables = pixmap.fade_tables;
    render_ghost        = pixmap.ghost;
    render_alpha        = (unsigned char *)&alpha_sprite_table;

    WorldSceneTraceBegin(1);
    kfx::render::ProduceWorldDraws(kfx::render::WorldDrawView::Isometric,
        buckets, {m_screen_w, m_screen_h, BUCKETS_COUNT}, pixel_size, nullptr, *this);
    WorldSceneTraceEnd();
    gpu_flush();


    {
        const int ir_count = (int)(m_kspr_ir.size() - m_kspr_pass_start);
        if (ir_count > 0)
        {
            DrawCmd cmd;
            cmd.type            = DrawCmd::CMD_IR_KEEPER_SPRITES;
            cmd.sprite_ir_start = (int)m_kspr_pass_start;
            cmd.sprite_ir_count = ir_count;
            m_draw_cmds.push_back(cmd);
            m_kspr_pass_start = m_kspr_ir.size();
        }
    }
}

void GLWorldViewRenderer::DrawFrontView(struct Camera* cam)
{
    if (!m_initialized)
        return;
    if (!m_world_write_cmds)
        return;

    render_fade_tables = pixmap.fade_tables;
    render_ghost        = pixmap.ghost;
    render_alpha        = (unsigned char *)&alpha_sprite_table;

    WorldSceneTraceBegin(1);
    kfx::render::ProduceWorldDraws(kfx::render::WorldDrawView::Front,
        buckets, {m_screen_w, m_screen_h, BUCKETS_COUNT}, pixel_size, cam, *this);
    WorldSceneTraceEnd();
    gpu_flush();
    {
        const int ir_count = (int)(m_kspr_ir.size() - m_kspr_pass_start);
        if (ir_count > 0)
        {
            DrawCmd cmd;
            cmd.type            = DrawCmd::CMD_IR_KEEPER_SPRITES;
            cmd.sprite_ir_start = (int)m_kspr_pass_start;
            cmd.sprite_ir_count = ir_count;
            m_draw_cmds.push_back(cmd);
            m_kspr_pass_start = m_kspr_ir.size();
        }
    }
}
