/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "anv_private.h"
#include "anv_nir.h"

#include <brw_context.h>
#include <brw_wm.h> /* brw_new_shader_program is here */
#include <brw_nir.h>

#include <brw_vs.h>
#include <brw_gs.h>
#include <brw_cs.h>
#include "brw_vec4_gs_visitor.h"

#include <mesa/main/shaderobj.h>
#include <mesa/main/fbobject.h>
#include <mesa/main/context.h>
#include <mesa/program/program.h>
#include <glsl/program.h>

/* XXX: We need this to keep symbols in nir.h from conflicting with the
 * generated GEN command packing headers.  We need to fix *both* to not
 * define something as generic as LOAD.
 */
#undef LOAD

#include <glsl/nir/nir_spirv.h>

#define SPIR_V_MAGIC_NUMBER 0x07230203

static void
fail_if(int cond, const char *format, ...)
{
   va_list args;

   if (!cond)
      return;

   va_start(args, format);
   vfprintf(stderr, format, args);
   va_end(args);

   exit(1);
}

static VkResult
set_binding_table_layout(struct brw_stage_prog_data *prog_data,
                         struct anv_pipeline *pipeline, uint32_t stage)
{
   uint32_t bias, count, k, *map;
   struct anv_pipeline_layout *layout = pipeline->layout;

   /* No layout is valid for shaders that don't bind any resources. */
   if (pipeline->layout == NULL)
      return VK_SUCCESS;

   if (stage == VK_SHADER_STAGE_FRAGMENT)
      bias = MAX_RTS;
   else
      bias = 0;

   count = layout->stage[stage].surface_count;
   prog_data->map_entries =
      (uint32_t *) malloc(count * sizeof(prog_data->map_entries[0]));
   if (prog_data->map_entries == NULL)
      return vk_error(VK_ERROR_OUT_OF_HOST_MEMORY);

   k = bias;
   map = prog_data->map_entries;
   for (uint32_t i = 0; i < layout->num_sets; i++) {
      prog_data->bind_map[i].index = map;
      for (uint32_t j = 0; j < layout->set[i].layout->stage[stage].surface_count; j++)
         *map++ = k++;

      prog_data->bind_map[i].index_count =
         layout->set[i].layout->stage[stage].surface_count;
   }

   return VK_SUCCESS;
}

static uint32_t
upload_kernel(struct anv_pipeline *pipeline, const void *data, size_t size)
{
   struct anv_state state =
      anv_state_stream_alloc(&pipeline->program_stream, size, 64);

   assert(size < pipeline->program_stream.block_pool->block_size);

   memcpy(state.map, data, size);

   return state.offset;
}

static void
create_params_array(struct anv_pipeline *pipeline,
                    struct gl_shader *shader,
                    struct brw_stage_prog_data *prog_data)
{
   VkShaderStage stage = anv_vk_shader_stage_for_mesa_stage(shader->Stage);
   unsigned num_params = 0;

   if (shader->num_uniform_components) {
      /* If the shader uses any push constants at all, we'll just give
       * them the maximum possible number
       */
      num_params += MAX_PUSH_CONSTANTS_SIZE / sizeof(float);
   }

   if (pipeline->layout && pipeline->layout->stage[stage].has_dynamic_offsets)
      num_params += MAX_DYNAMIC_BUFFERS;

   if (num_params == 0)
      return;

   prog_data->param = (const gl_constant_value **)
      anv_device_alloc(pipeline->device,
                       num_params * sizeof(gl_constant_value *),
                       8, VK_SYSTEM_ALLOC_TYPE_INTERNAL_SHADER);

   /* We now set the param values to be offsets into a
    * anv_push_constant_data structure.  Since the compiler doesn't
    * actually dereference any of the gl_constant_value pointers in the
    * params array, it doesn't really matter what we put here.
    */
   struct anv_push_constants *null_data = NULL;
   for (unsigned i = 0; i < num_params; i++)
      prog_data->param[i] =
         (const gl_constant_value *)&null_data->client_data[i * sizeof(float)];
}

/**
 * Return a bitfield where bit n is set if barycentric interpolation mode n
 * (see enum brw_wm_barycentric_interp_mode) is needed by the fragment shader.
 */
unsigned
brw_compute_barycentric_interp_modes(const struct brw_device_info *devinfo,
                                     bool shade_model_flat,
                                     bool persample_shading,
                                     nir_shader *shader)
{
   unsigned barycentric_interp_modes = 0;

   nir_foreach_variable(var, &shader->inputs) {
      enum glsl_interp_qualifier interp_qualifier =
         (enum glsl_interp_qualifier) var->data.interpolation;
      bool is_centroid = var->data.centroid && !persample_shading;
      bool is_sample = var->data.sample || persample_shading;
      bool is_gl_Color = (var->data.location == VARYING_SLOT_COL0) ||
                         (var->data.location == VARYING_SLOT_COL1);

      /* Ignore WPOS and FACE, because they don't require interpolation. */
      if (var->data.location == VARYING_SLOT_POS ||
          var->data.location == VARYING_SLOT_FACE)
         continue;

      /* Determine the set (or sets) of barycentric coordinates needed to
       * interpolate this variable.  Note that when
       * brw->needs_unlit_centroid_workaround is set, centroid interpolation
       * uses PIXEL interpolation for unlit pixels and CENTROID interpolation
       * for lit pixels, so we need both sets of barycentric coordinates.
       */
      if (interp_qualifier == INTERP_QUALIFIER_NOPERSPECTIVE) {
         if (is_centroid) {
            barycentric_interp_modes |=
               1 << BRW_WM_NONPERSPECTIVE_CENTROID_BARYCENTRIC;
         } else if (is_sample) {
            barycentric_interp_modes |=
               1 << BRW_WM_NONPERSPECTIVE_SAMPLE_BARYCENTRIC;
         }
         if ((!is_centroid && !is_sample) ||
             devinfo->needs_unlit_centroid_workaround) {
            barycentric_interp_modes |=
               1 << BRW_WM_NONPERSPECTIVE_PIXEL_BARYCENTRIC;
         }
      } else if (interp_qualifier == INTERP_QUALIFIER_SMOOTH ||
                 (!(shade_model_flat && is_gl_Color) &&
                  interp_qualifier == INTERP_QUALIFIER_NONE)) {
         if (is_centroid) {
            barycentric_interp_modes |=
               1 << BRW_WM_PERSPECTIVE_CENTROID_BARYCENTRIC;
         } else if (is_sample) {
            barycentric_interp_modes |=
               1 << BRW_WM_PERSPECTIVE_SAMPLE_BARYCENTRIC;
         }
         if ((!is_centroid && !is_sample) ||
             devinfo->needs_unlit_centroid_workaround) {
            barycentric_interp_modes |=
               1 << BRW_WM_PERSPECTIVE_PIXEL_BARYCENTRIC;
         }
      }
   }

   return barycentric_interp_modes;
}

static void
brw_vs_populate_key(struct brw_context *brw,
                    struct brw_vertex_program *vp,
                    struct brw_vs_prog_key *key)
{
   struct gl_context *ctx = &brw->ctx;
   /* BRW_NEW_VERTEX_PROGRAM */
   struct gl_program *prog = (struct gl_program *) vp;

   memset(key, 0, sizeof(*key));

   /* Just upload the program verbatim for now.  Always send it all
    * the inputs it asks for, whether they are varying or not.
    */
   key->program_string_id = vp->id;

   /* _NEW_POLYGON */
   if (brw->gen < 6) {
      key->copy_edgeflag = (ctx->Polygon.FrontMode != GL_FILL ||
                           ctx->Polygon.BackMode != GL_FILL);
   }

   if (prog->OutputsWritten & (VARYING_BIT_COL0 | VARYING_BIT_COL1 |
                               VARYING_BIT_BFC0 | VARYING_BIT_BFC1)) {
      /* _NEW_LIGHT | _NEW_BUFFERS */
      key->clamp_vertex_color = ctx->Light._ClampVertexColor;
   }

   /* _NEW_POINT */
   if (brw->gen < 6 && ctx->Point.PointSprite) {
      for (int i = 0; i < 8; i++) {
         if (ctx->Point.CoordReplace[i])
            key->point_coord_replace |= (1 << i);
      }
   }
}

static bool
really_do_vs_prog(struct brw_context *brw,
                  struct gl_shader_program *prog,
                  struct brw_vertex_program *vp,
                  struct brw_vs_prog_key *key, struct anv_pipeline *pipeline)
{
   GLuint program_size;
   const GLuint *program;
   struct brw_vs_prog_data *prog_data = &pipeline->vs_prog_data;
   void *mem_ctx;
   struct gl_shader *vs = NULL;

   if (prog)
      vs = prog->_LinkedShaders[MESA_SHADER_VERTEX];

   memset(prog_data, 0, sizeof(*prog_data));

   mem_ctx = ralloc_context(NULL);

   create_params_array(pipeline, vs, &prog_data->base.base);
   anv_nir_apply_dynamic_offsets(pipeline, vs->Program->nir,
                                 &prog_data->base.base);

   GLbitfield64 outputs_written = vp->program.Base.OutputsWritten;
   prog_data->inputs_read = vp->program.Base.InputsRead;

   if (key->copy_edgeflag) {
      outputs_written |= BITFIELD64_BIT(VARYING_SLOT_EDGE);
      prog_data->inputs_read |= VERT_BIT_EDGEFLAG;
   }

   if (brw->gen < 6) {
      /* Put dummy slots into the VUE for the SF to put the replaced
       * point sprite coords in.  We shouldn't need these dummy slots,
       * which take up precious URB space, but it would mean that the SF
       * doesn't get nice aligned pairs of input coords into output
       * coords, which would be a pain to handle.
       */
      for (int i = 0; i < 8; i++) {
         if (key->point_coord_replace & (1 << i))
            outputs_written |= BITFIELD64_BIT(VARYING_SLOT_TEX0 + i);
      }

      /* if back colors are written, allocate slots for front colors too */
      if (outputs_written & BITFIELD64_BIT(VARYING_SLOT_BFC0))
         outputs_written |= BITFIELD64_BIT(VARYING_SLOT_COL0);
      if (outputs_written & BITFIELD64_BIT(VARYING_SLOT_BFC1))
         outputs_written |= BITFIELD64_BIT(VARYING_SLOT_COL1);
   }

   /* In order for legacy clipping to work, we need to populate the clip
    * distance varying slots whenever clipping is enabled, even if the vertex
    * shader doesn't write to gl_ClipDistance.
    */
   if (key->nr_userclip_plane_consts) {
      outputs_written |= BITFIELD64_BIT(VARYING_SLOT_CLIP_DIST0);
      outputs_written |= BITFIELD64_BIT(VARYING_SLOT_CLIP_DIST1);
   }

   brw_compute_vue_map(brw->intelScreen->devinfo,
                       &prog_data->base.vue_map, outputs_written,
                       prog ? prog->SeparateShader : false);

   set_binding_table_layout(&prog_data->base.base, pipeline,
                            VK_SHADER_STAGE_VERTEX);

   /* Emit GEN4 code.
    */
   program = brw_vs_emit(brw, mem_ctx, key, prog_data, &vp->program,
                         prog, -1, &program_size);
   if (program == NULL) {
      ralloc_free(mem_ctx);
      return false;
   }

   const uint32_t offset = upload_kernel(pipeline, program, program_size);
   if (prog_data->base.dispatch_mode == DISPATCH_MODE_SIMD8) {
      pipeline->vs_simd8 = offset;
      pipeline->vs_vec4 = NO_KERNEL;
   } else {
      pipeline->vs_simd8 = NO_KERNEL;
      pipeline->vs_vec4 = offset;
   }

   ralloc_free(mem_ctx);

   return true;
}

void brw_wm_populate_key(struct brw_context *brw,
                         struct brw_fragment_program *fp,
                         struct brw_wm_prog_key *key)
{
   struct gl_context *ctx = &brw->ctx;
   GLuint lookup = 0;
   GLuint line_aa;
   bool program_uses_dfdy = fp->program.UsesDFdy;
   struct gl_framebuffer draw_buffer;
   bool multisample_fbo;

   memset(key, 0, sizeof(*key));

   for (int i = 0; i < MAX_SAMPLERS; i++) {
      /* Assume color sampler, no swizzling. */
      key->tex.swizzles[i] = SWIZZLE_XYZW;
   }

   /* A non-zero framebuffer name indicates that the framebuffer was created by
    * the user rather than the window system. */
   draw_buffer.Name = 1;
   draw_buffer.Visual.samples = 1;
   draw_buffer._NumColorDrawBuffers = 1;
   draw_buffer._NumColorDrawBuffers = 1;
   draw_buffer.Width = 400;
   draw_buffer.Height = 400;
   ctx->DrawBuffer = &draw_buffer;

   multisample_fbo = ctx->DrawBuffer->Visual.samples > 1;

   /* Build the index for table lookup
    */
   if (brw->gen < 6) {
      /* _NEW_COLOR */
      if (fp->program.UsesKill || ctx->Color.AlphaEnabled)
         lookup |= IZ_PS_KILL_ALPHATEST_BIT;

      if (fp->program.Base.OutputsWritten & BITFIELD64_BIT(FRAG_RESULT_DEPTH))
         lookup |= IZ_PS_COMPUTES_DEPTH_BIT;

      /* _NEW_DEPTH */
      if (ctx->Depth.Test)
         lookup |= IZ_DEPTH_TEST_ENABLE_BIT;

      if (ctx->Depth.Test && ctx->Depth.Mask) /* ?? */
         lookup |= IZ_DEPTH_WRITE_ENABLE_BIT;

      /* _NEW_STENCIL | _NEW_BUFFERS */
      if (ctx->Stencil._Enabled) {
         lookup |= IZ_STENCIL_TEST_ENABLE_BIT;

         if (ctx->Stencil.WriteMask[0] ||
             ctx->Stencil.WriteMask[ctx->Stencil._BackFace])
            lookup |= IZ_STENCIL_WRITE_ENABLE_BIT;
      }
      key->iz_lookup = lookup;
   }

   line_aa = AA_NEVER;

   /* _NEW_LINE, _NEW_POLYGON, BRW_NEW_REDUCED_PRIMITIVE */
   if (ctx->Line.SmoothFlag) {
      if (brw->reduced_primitive == GL_LINES) {
         line_aa = AA_ALWAYS;
      }
      else if (brw->reduced_primitive == GL_TRIANGLES) {
         if (ctx->Polygon.FrontMode == GL_LINE) {
            line_aa = AA_SOMETIMES;

            if (ctx->Polygon.BackMode == GL_LINE ||
                (ctx->Polygon.CullFlag &&
                 ctx->Polygon.CullFaceMode == GL_BACK))
               line_aa = AA_ALWAYS;
         }
         else if (ctx->Polygon.BackMode == GL_LINE) {
            line_aa = AA_SOMETIMES;

            if ((ctx->Polygon.CullFlag &&
                 ctx->Polygon.CullFaceMode == GL_FRONT))
               line_aa = AA_ALWAYS;
         }
      }
   }

   key->line_aa = line_aa;

   /* _NEW_HINT */
   key->high_quality_derivatives =
      ctx->Hint.FragmentShaderDerivative == GL_NICEST;

   if (brw->gen < 6)
      key->stats_wm = brw->stats_wm;

   /* _NEW_LIGHT */
   key->flat_shade = (ctx->Light.ShadeModel == GL_FLAT);

   /* _NEW_FRAG_CLAMP | _NEW_BUFFERS */
   key->clamp_fragment_color = ctx->Color._ClampFragmentColor;

   /* _NEW_BUFFERS */
   /*
    * Include the draw buffer origin and height so that we can calculate
    * fragment position values relative to the bottom left of the drawable,
    * from the incoming screen origin relative position we get as part of our
    * payload.
    *
    * This is only needed for the WM_WPOSXY opcode when the fragment program
    * uses the gl_FragCoord input.
    *
    * We could avoid recompiling by including this as a constant referenced by
    * our program, but if we were to do that it would also be nice to handle
    * getting that constant updated at batchbuffer submit time (when we
    * hold the lock and know where the buffer really is) rather than at emit
    * time when we don't hold the lock and are just guessing.  We could also
    * just avoid using this as key data if the program doesn't use
    * fragment.position.
    *
    * For DRI2 the origin_x/y will always be (0,0) but we still need the
    * drawable height in order to invert the Y axis.
    */
   if (fp->program.Base.InputsRead & VARYING_BIT_POS) {
      key->drawable_height = ctx->DrawBuffer->Height;
   }

   if ((fp->program.Base.InputsRead & VARYING_BIT_POS) || program_uses_dfdy) {
      key->render_to_fbo = _mesa_is_user_fbo(ctx->DrawBuffer);
   }

   /* _NEW_BUFFERS */
   key->nr_color_regions = ctx->DrawBuffer->_NumColorDrawBuffers;

   /* _NEW_MULTISAMPLE, _NEW_COLOR, _NEW_BUFFERS */
   key->replicate_alpha = ctx->DrawBuffer->_NumColorDrawBuffers > 1 &&
      (ctx->Multisample.SampleAlphaToCoverage || ctx->Color.AlphaEnabled);

   /* _NEW_BUFFERS _NEW_MULTISAMPLE */
   /* Ignore sample qualifier while computing this flag. */
   key->persample_shading =
      _mesa_get_min_invocations_per_fragment(ctx, &fp->program, true) > 1;
   if (key->persample_shading)
      key->persample_2x = ctx->DrawBuffer->Visual.samples == 2;

   key->compute_pos_offset =
      _mesa_get_min_invocations_per_fragment(ctx, &fp->program, false) > 1 &&
      fp->program.Base.SystemValuesRead & SYSTEM_BIT_SAMPLE_POS;

   key->compute_sample_id =
      multisample_fbo &&
      ctx->Multisample.Enabled &&
      (fp->program.Base.SystemValuesRead & SYSTEM_BIT_SAMPLE_ID);

   /* BRW_NEW_VUE_MAP_GEOM_OUT */
   if (brw->gen < 6 || _mesa_bitcount_64(fp->program.Base.InputsRead &
                                         BRW_FS_VARYING_INPUT_MASK) > 16)
      key->input_slots_valid = brw->vue_map_geom_out.slots_valid;


   /* _NEW_COLOR | _NEW_BUFFERS */
   /* Pre-gen6, the hardware alpha test always used each render
    * target's alpha to do alpha test, as opposed to render target 0's alpha
    * like GL requires.  Fix that by building the alpha test into the
    * shader, and we'll skip enabling the fixed function alpha test.
    */
   if (brw->gen < 6 && ctx->DrawBuffer->_NumColorDrawBuffers > 1 && ctx->Color.AlphaEnabled) {
      key->alpha_test_func = ctx->Color.AlphaFunc;
      key->alpha_test_ref = ctx->Color.AlphaRef;
   }

   /* The unique fragment program ID */
   key->program_string_id = fp->id;

   ctx->DrawBuffer = NULL;
}

static uint8_t
computed_depth_mode(struct gl_fragment_program *fp)
{
   if (fp->Base.OutputsWritten & BITFIELD64_BIT(FRAG_RESULT_DEPTH)) {
      switch (fp->FragDepthLayout) {
      case FRAG_DEPTH_LAYOUT_NONE:
      case FRAG_DEPTH_LAYOUT_ANY:
         return BRW_PSCDEPTH_ON;
      case FRAG_DEPTH_LAYOUT_GREATER:
         return BRW_PSCDEPTH_ON_GE;
      case FRAG_DEPTH_LAYOUT_LESS:
         return BRW_PSCDEPTH_ON_LE;
      case FRAG_DEPTH_LAYOUT_UNCHANGED:
         return BRW_PSCDEPTH_OFF;
      }
   }
   return BRW_PSCDEPTH_OFF;
}

static bool
really_do_wm_prog(struct brw_context *brw,
                  struct gl_shader_program *prog,
                  struct brw_fragment_program *fp,
                  struct brw_wm_prog_key *key, struct anv_pipeline *pipeline)
{
   void *mem_ctx = ralloc_context(NULL);
   struct brw_wm_prog_data *prog_data = &pipeline->wm_prog_data;
   struct gl_shader *fs = NULL;
   unsigned int program_size;
   const uint32_t *program;

   if (prog)
      fs = prog->_LinkedShaders[MESA_SHADER_FRAGMENT];

   memset(prog_data, 0, sizeof(*prog_data));

   /* key->alpha_test_func means simulating alpha testing via discards,
    * so the shader definitely kills pixels.
    */
   prog_data->uses_kill = fp->program.UsesKill || key->alpha_test_func;

   prog_data->computed_depth_mode = computed_depth_mode(&fp->program);

   create_params_array(pipeline, fs, &prog_data->base);
   anv_nir_apply_dynamic_offsets(pipeline, fs->Program->nir, &prog_data->base);

   prog_data->barycentric_interp_modes =
      brw_compute_barycentric_interp_modes(brw->intelScreen->devinfo,
                                           key->flat_shade,
                                           key->persample_shading,
                                           fp->program.Base.nir);

   set_binding_table_layout(&prog_data->base, pipeline,
                            VK_SHADER_STAGE_FRAGMENT);
   /* This needs to come after shader time and pull constant entries, but we
    * don't have those set up now, so just put it after the layout entries.
    */
   prog_data->binding_table.render_target_start = 0;

   program = brw_wm_fs_emit(brw, mem_ctx, key, prog_data,
                            &fp->program, prog, -1, -1, &program_size);
   if (program == NULL) {
      ralloc_free(mem_ctx);
      return false;
   }

   uint32_t offset = upload_kernel(pipeline, program, program_size);

   if (prog_data->no_8)
      pipeline->ps_simd8 = NO_KERNEL;
   else
      pipeline->ps_simd8 = offset;

   if (prog_data->no_8 || prog_data->prog_offset_16) {
      pipeline->ps_simd16 = offset + prog_data->prog_offset_16;
   } else {
      pipeline->ps_simd16 = NO_KERNEL;
   }

   ralloc_free(mem_ctx);

   return true;
}

bool
anv_codegen_gs_prog(struct brw_context *brw,
                    struct gl_shader_program *prog,
                    struct brw_geometry_program *gp,
                    struct brw_gs_prog_key *key,
                    struct anv_pipeline *pipeline)
{
   struct brw_gs_compile c;

   memset(&c, 0, sizeof(c));
   c.key = *key;
   c.gp = gp;

   c.prog_data.include_primitive_id =
      (gp->program.Base.InputsRead & VARYING_BIT_PRIMITIVE_ID) != 0;

   c.prog_data.invocations = gp->program.Invocations;

   set_binding_table_layout(&c.prog_data.base.base,
                            pipeline, VK_SHADER_STAGE_GEOMETRY);

   /* Allocate the references to the uniforms that will end up in the
    * prog_data associated with the compiled program, and which will be freed
    * by the state cache.
    *
    * Note: param_count needs to be num_uniform_components * 4, since we add
    * padding around uniform values below vec4 size, so the worst case is that
    * every uniform is a float which gets padded to the size of a vec4.
    */
   struct gl_shader *gs = prog->_LinkedShaders[MESA_SHADER_GEOMETRY];
   int param_count = gp->program.Base.nir->num_uniforms * 4;

   c.prog_data.base.base.param =
      rzalloc_array(NULL, const gl_constant_value *, param_count);
   c.prog_data.base.base.pull_param =
      rzalloc_array(NULL, const gl_constant_value *, param_count);
   c.prog_data.base.base.image_param =
      rzalloc_array(NULL, struct brw_image_param, gs->NumImages);
   c.prog_data.base.base.nr_params = param_count;
   c.prog_data.base.base.nr_image_params = gs->NumImages;

   brw_nir_setup_glsl_uniforms(gp->program.Base.nir, prog, &gp->program.Base,
                               &c.prog_data.base.base, false);

   if (brw->gen >= 8) {
      c.prog_data.static_vertex_count = !gp->program.Base.nir ? -1 :
         nir_gs_count_vertices(gp->program.Base.nir);
   }

   if (brw->gen >= 7) {
      if (gp->program.OutputType == GL_POINTS) {
         /* When the output type is points, the geometry shader may output data
          * to multiple streams, and EndPrimitive() has no effect.  So we
          * configure the hardware to interpret the control data as stream ID.
          */
         c.prog_data.control_data_format = GEN7_GS_CONTROL_DATA_FORMAT_GSCTL_SID;

         /* We only have to emit control bits if we are using streams */
         if (prog->Geom.UsesStreams)
            c.control_data_bits_per_vertex = 2;
         else
            c.control_data_bits_per_vertex = 0;
      } else {
         /* When the output type is triangle_strip or line_strip, EndPrimitive()
          * may be used to terminate the current strip and start a new one
          * (similar to primitive restart), and outputting data to multiple
          * streams is not supported.  So we configure the hardware to interpret
          * the control data as EndPrimitive information (a.k.a. "cut bits").
          */
         c.prog_data.control_data_format = GEN7_GS_CONTROL_DATA_FORMAT_GSCTL_CUT;

         /* We only need to output control data if the shader actually calls
          * EndPrimitive().
          */
         c.control_data_bits_per_vertex = gp->program.UsesEndPrimitive ? 1 : 0;
      }
   } else {
      /* There are no control data bits in gen6. */
      c.control_data_bits_per_vertex = 0;

      /* If it is using transform feedback, enable it */
      if (prog->TransformFeedback.NumVarying)
         c.prog_data.gen6_xfb_enabled = true;
      else
         c.prog_data.gen6_xfb_enabled = false;
   }
   c.control_data_header_size_bits =
      gp->program.VerticesOut * c.control_data_bits_per_vertex;

   /* 1 HWORD = 32 bytes = 256 bits */
   c.prog_data.control_data_header_size_hwords =
      ALIGN(c.control_data_header_size_bits, 256) / 256;

   GLbitfield64 outputs_written = gp->program.Base.OutputsWritten;

   brw_compute_vue_map(brw->intelScreen->devinfo,
                       &c.prog_data.base.vue_map, outputs_written,
                       prog ? prog->SeparateShader : false);

   /* Compute the output vertex size.
    *
    * From the Ivy Bridge PRM, Vol2 Part1 7.2.1.1 STATE_GS - Output Vertex
    * Size (p168):
    *
    *     [0,62] indicating [1,63] 16B units
    *
    *     Specifies the size of each vertex stored in the GS output entry
    *     (following any Control Header data) as a number of 128-bit units
    *     (minus one).
    *
    *     Programming Restrictions: The vertex size must be programmed as a
    *     multiple of 32B units with the following exception: Rendering is
    *     disabled (as per SOL stage state) and the vertex size output by the
    *     GS thread is 16B.
    *
    *     If rendering is enabled (as per SOL state) the vertex size must be
    *     programmed as a multiple of 32B units. In other words, the only time
    *     software can program a vertex size with an odd number of 16B units
    *     is when rendering is disabled.
    *
    * Note: B=bytes in the above text.
    *
    * It doesn't seem worth the extra trouble to optimize the case where the
    * vertex size is 16B (especially since this would require special-casing
    * the GEN assembly that writes to the URB).  So we just set the vertex
    * size to a multiple of 32B (2 vec4's) in all cases.
    *
    * The maximum output vertex size is 62*16 = 992 bytes (31 hwords).  We
    * budget that as follows:
    *
    *   512 bytes for varyings (a varying component is 4 bytes and
    *             gl_MaxGeometryOutputComponents = 128)
    *    16 bytes overhead for VARYING_SLOT_PSIZ (each varying slot is 16
    *             bytes)
    *    16 bytes overhead for gl_Position (we allocate it a slot in the VUE
    *             even if it's not used)
    *    32 bytes overhead for gl_ClipDistance (we allocate it 2 VUE slots
    *             whenever clip planes are enabled, even if the shader doesn't
    *             write to gl_ClipDistance)
    *    16 bytes overhead since the VUE size must be a multiple of 32 bytes
    *             (see below)--this causes up to 1 VUE slot to be wasted
    *   400 bytes available for varying packing overhead
    *
    * Worst-case varying packing overhead is 3/4 of a varying slot (12 bytes)
    * per interpolation type, so this is plenty.
    *
    */
   unsigned output_vertex_size_bytes = c.prog_data.base.vue_map.num_slots * 16;
   assert(brw->gen == 6 ||
          output_vertex_size_bytes <= GEN7_MAX_GS_OUTPUT_VERTEX_SIZE_BYTES);
   c.prog_data.output_vertex_size_hwords =
      ALIGN(output_vertex_size_bytes, 32) / 32;

   /* Compute URB entry size.  The maximum allowed URB entry size is 32k.
    * That divides up as follows:
    *
    *     64 bytes for the control data header (cut indices or StreamID bits)
    *   4096 bytes for varyings (a varying component is 4 bytes and
    *              gl_MaxGeometryTotalOutputComponents = 1024)
    *   4096 bytes overhead for VARYING_SLOT_PSIZ (each varying slot is 16
    *              bytes/vertex and gl_MaxGeometryOutputVertices is 256)
    *   4096 bytes overhead for gl_Position (we allocate it a slot in the VUE
    *              even if it's not used)
    *   8192 bytes overhead for gl_ClipDistance (we allocate it 2 VUE slots
    *              whenever clip planes are enabled, even if the shader doesn't
    *              write to gl_ClipDistance)
    *   4096 bytes overhead since the VUE size must be a multiple of 32
    *              bytes (see above)--this causes up to 1 VUE slot to be wasted
    *   8128 bytes available for varying packing overhead
    *
    * Worst-case varying packing overhead is 3/4 of a varying slot per
    * interpolation type, which works out to 3072 bytes, so this would allow
    * us to accommodate 2 interpolation types without any danger of running
    * out of URB space.
    *
    * In practice, the risk of running out of URB space is very small, since
    * the above figures are all worst-case, and most of them scale with the
    * number of output vertices.  So we'll just calculate the amount of space
    * we need, and if it's too large, fail to compile.
    *
    * The above is for gen7+ where we have a single URB entry that will hold
    * all the output. In gen6, we will have to allocate URB entries for every
    * vertex we emit, so our URB entries only need to be large enough to hold
    * a single vertex. Also, gen6 does not have a control data header.
    */
   unsigned output_size_bytes;
   if (brw->gen >= 7) {
      output_size_bytes =
         c.prog_data.output_vertex_size_hwords * 32 * gp->program.VerticesOut;
      output_size_bytes += 32 * c.prog_data.control_data_header_size_hwords;
   } else {
      output_size_bytes = c.prog_data.output_vertex_size_hwords * 32;
   }

   /* Broadwell stores "Vertex Count" as a full 8 DWord (32 byte) URB output,
    * which comes before the control header.
    */
   if (brw->gen >= 8)
      output_size_bytes += 32;

   assert(output_size_bytes >= 1);
   int max_output_size_bytes = GEN7_MAX_GS_URB_ENTRY_SIZE_BYTES;
   if (brw->gen == 6)
      max_output_size_bytes = GEN6_MAX_GS_URB_ENTRY_SIZE_BYTES;
   if (output_size_bytes > max_output_size_bytes)
      return false;


   /* URB entry sizes are stored as a multiple of 64 bytes in gen7+ and
    * a multiple of 128 bytes in gen6.
    */
   if (brw->gen >= 7)
      c.prog_data.base.urb_entry_size = ALIGN(output_size_bytes, 64) / 64;
   else
      c.prog_data.base.urb_entry_size = ALIGN(output_size_bytes, 128) / 128;

   /* FIXME: Need to pull this from nir shader. */
   c.prog_data.output_topology = _3DPRIM_TRISTRIP;

   /* The GLSL linker will have already matched up GS inputs and the outputs
    * of prior stages.  The driver does extend VS outputs in some cases, but
    * only for legacy OpenGL or Gen4-5 hardware, neither of which offer
    * geometry shader support.  So we can safely ignore that.
    *
    * For SSO pipelines, we use a fixed VUE map layout based on variable
    * locations, so we can rely on rendezvous-by-location making this work.
    *
    * However, we need to ignore VARYING_SLOT_PRIMITIVE_ID, as it's not
    * written by previous stages and shows up via payload magic.
    */
   GLbitfield64 inputs_read =
      gp->program.Base.InputsRead & ~VARYING_BIT_PRIMITIVE_ID;
   brw_compute_vue_map(brw->intelScreen->devinfo,
                       &c.input_vue_map, inputs_read,
                       prog->SeparateShader);

   /* GS inputs are read from the VUE 256 bits (2 vec4's) at a time, so we
    * need to program a URB read length of ceiling(num_slots / 2).
    */
   c.prog_data.base.urb_read_length = (c.input_vue_map.num_slots + 1) / 2;

   void *mem_ctx = ralloc_context(NULL);
   unsigned program_size;
   const unsigned *program =
      brw_gs_emit(brw, prog, &c, mem_ctx, -1, &program_size);
   if (program == NULL) {
      ralloc_free(mem_ctx);
      return false;
   }

   pipeline->gs_vec4 = upload_kernel(pipeline, program, program_size);
   pipeline->gs_vertex_count = gp->program.VerticesIn;

   ralloc_free(mem_ctx);

   return true;
}

static bool
brw_codegen_cs_prog(struct brw_context *brw,
                    struct gl_shader_program *prog,
                    struct brw_compute_program *cp,
                    struct brw_cs_prog_key *key, struct anv_pipeline *pipeline)
{
   const GLuint *program;
   void *mem_ctx = ralloc_context(NULL);
   GLuint program_size;
   struct brw_cs_prog_data *prog_data = &pipeline->cs_prog_data;

   struct gl_shader *cs = prog->_LinkedShaders[MESA_SHADER_COMPUTE];
   assert (cs);

   memset(prog_data, 0, sizeof(*prog_data));

   set_binding_table_layout(&prog_data->base, pipeline, VK_SHADER_STAGE_COMPUTE);

   create_params_array(pipeline, cs, &prog_data->base);
   anv_nir_apply_dynamic_offsets(pipeline, cs->Program->nir, &prog_data->base);

   program = brw_cs_emit(brw, mem_ctx, key, prog_data,
                         &cp->program, prog, -1, &program_size);
   if (program == NULL) {
      ralloc_free(mem_ctx);
      return false;
   }

   if (unlikely(INTEL_DEBUG & DEBUG_CS))
      fprintf(stderr, "\n");

   pipeline->cs_simd = upload_kernel(pipeline, program, program_size);

   ralloc_free(mem_ctx);

   return true;
}

static void
brw_cs_populate_key(struct brw_context *brw,
                    struct brw_compute_program *bcp, struct brw_cs_prog_key *key)
{
   memset(key, 0, sizeof(*key));

   /* The unique compute program ID */
   key->program_string_id = bcp->id;
}

struct anv_compiler {
   struct anv_device *device;
   struct intel_screen *screen;
   struct brw_context *brw;
   struct gl_pipeline_object pipeline;
};

extern "C" {

struct anv_compiler *
anv_compiler_create(struct anv_device *device)
{
   const struct brw_device_info *devinfo = &device->info;
   struct anv_compiler *compiler;
   struct gl_context *ctx;

   compiler = rzalloc(NULL, struct anv_compiler);
   if (compiler == NULL)
      return NULL;

   compiler->screen = rzalloc(compiler, struct intel_screen);
   if (compiler->screen == NULL)
      goto fail;

   compiler->brw = rzalloc(compiler, struct brw_context);
   if (compiler->brw == NULL)
      goto fail;

   compiler->device = device;

   compiler->brw->gen = devinfo->gen;
   compiler->brw->is_g4x = devinfo->is_g4x;
   compiler->brw->is_baytrail = devinfo->is_baytrail;
   compiler->brw->is_haswell = devinfo->is_haswell;
   compiler->brw->is_cherryview = devinfo->is_cherryview;

   /* We need this at least for CS, which will check brw->max_cs_threads
    * against the work group size. */
   compiler->brw->max_vs_threads = devinfo->max_vs_threads;
   compiler->brw->max_hs_threads = devinfo->max_hs_threads;
   compiler->brw->max_ds_threads = devinfo->max_ds_threads;
   compiler->brw->max_gs_threads = devinfo->max_gs_threads;
   compiler->brw->max_wm_threads = devinfo->max_wm_threads;
   compiler->brw->max_cs_threads = devinfo->max_cs_threads;
   compiler->brw->urb.size = devinfo->urb.size;
   compiler->brw->urb.min_vs_entries = devinfo->urb.min_vs_entries;
   compiler->brw->urb.max_vs_entries = devinfo->urb.max_vs_entries;
   compiler->brw->urb.max_hs_entries = devinfo->urb.max_hs_entries;
   compiler->brw->urb.max_ds_entries = devinfo->urb.max_ds_entries;
   compiler->brw->urb.max_gs_entries = devinfo->urb.max_gs_entries;

   compiler->brw->intelScreen = compiler->screen;
   compiler->screen->devinfo = &device->info;

   brw_process_intel_debug_variable();

   compiler->screen->compiler = brw_compiler_create(compiler, &device->info);

   ctx = &compiler->brw->ctx;
   _mesa_init_shader_object_functions(&ctx->Driver);

   /* brw_select_clip_planes() needs this for bogus reasons. */
   ctx->_Shader = &compiler->pipeline;

   return compiler;

 fail:
   ralloc_free(compiler);
   return NULL;
}

void
anv_compiler_destroy(struct anv_compiler *compiler)
{
   _mesa_free_errors_data(&compiler->brw->ctx);
   ralloc_free(compiler);
}

/* From gen7_urb.c */

/* FIXME: Add to struct intel_device_info */

static const int gen8_push_size = 32 * 1024;

static void
gen7_compute_urb_partition(struct anv_pipeline *pipeline)
{
   const struct brw_device_info *devinfo = &pipeline->device->info;
   bool vs_present = pipeline->vs_simd8 != NO_KERNEL;
   unsigned vs_size = vs_present ? pipeline->vs_prog_data.base.urb_entry_size : 1;
   unsigned vs_entry_size_bytes = vs_size * 64;
   bool gs_present = pipeline->gs_vec4 != NO_KERNEL;
   unsigned gs_size = gs_present ? pipeline->gs_prog_data.base.urb_entry_size : 1;
   unsigned gs_entry_size_bytes = gs_size * 64;

   /* From p35 of the Ivy Bridge PRM (section 1.7.1: 3DSTATE_URB_GS):
    *
    *     VS Number of URB Entries must be divisible by 8 if the VS URB Entry
    *     Allocation Size is less than 9 512-bit URB entries.
    *
    * Similar text exists for GS.
    */
   unsigned vs_granularity = (vs_size < 9) ? 8 : 1;
   unsigned gs_granularity = (gs_size < 9) ? 8 : 1;

   /* URB allocations must be done in 8k chunks. */
   unsigned chunk_size_bytes = 8192;

   /* Determine the size of the URB in chunks. */
   unsigned urb_chunks = devinfo->urb.size * 1024 / chunk_size_bytes;

   /* Reserve space for push constants */
   unsigned push_constant_bytes = gen8_push_size;
   unsigned push_constant_chunks =
      push_constant_bytes / chunk_size_bytes;

   /* Initially, assign each stage the minimum amount of URB space it needs,
    * and make a note of how much additional space it "wants" (the amount of
    * additional space it could actually make use of).
    */

   /* VS has a lower limit on the number of URB entries */
   unsigned vs_chunks =
      ALIGN(devinfo->urb.min_vs_entries * vs_entry_size_bytes,
            chunk_size_bytes) / chunk_size_bytes;
   unsigned vs_wants =
      ALIGN(devinfo->urb.max_vs_entries * vs_entry_size_bytes,
            chunk_size_bytes) / chunk_size_bytes - vs_chunks;

   unsigned gs_chunks = 0;
   unsigned gs_wants = 0;
   if (gs_present) {
      /* There are two constraints on the minimum amount of URB space we can
       * allocate:
       *
       * (1) We need room for at least 2 URB entries, since we always operate
       * the GS in DUAL_OBJECT mode.
       *
       * (2) We can't allocate less than nr_gs_entries_granularity.
       */
      gs_chunks = ALIGN(MAX2(gs_granularity, 2) * gs_entry_size_bytes,
                        chunk_size_bytes) / chunk_size_bytes;
      gs_wants =
         ALIGN(devinfo->urb.max_gs_entries * gs_entry_size_bytes,
               chunk_size_bytes) / chunk_size_bytes - gs_chunks;
   }

   /* There should always be enough URB space to satisfy the minimum
    * requirements of each stage.
    */
   unsigned total_needs = push_constant_chunks + vs_chunks + gs_chunks;
   assert(total_needs <= urb_chunks);

   /* Mete out remaining space (if any) in proportion to "wants". */
   unsigned total_wants = vs_wants + gs_wants;
   unsigned remaining_space = urb_chunks - total_needs;
   if (remaining_space > total_wants)
      remaining_space = total_wants;
   if (remaining_space > 0) {
      unsigned vs_additional = (unsigned)
         round(vs_wants * (((double) remaining_space) / total_wants));
      vs_chunks += vs_additional;
      remaining_space -= vs_additional;
      gs_chunks += remaining_space;
   }

   /* Sanity check that we haven't over-allocated. */
   assert(push_constant_chunks + vs_chunks + gs_chunks <= urb_chunks);

   /* Finally, compute the number of entries that can fit in the space
    * allocated to each stage.
    */
   unsigned nr_vs_entries = vs_chunks * chunk_size_bytes / vs_entry_size_bytes;
   unsigned nr_gs_entries = gs_chunks * chunk_size_bytes / gs_entry_size_bytes;

   /* Since we rounded up when computing *_wants, this may be slightly more
    * than the maximum allowed amount, so correct for that.
    */
   nr_vs_entries = MIN2(nr_vs_entries, devinfo->urb.max_vs_entries);
   nr_gs_entries = MIN2(nr_gs_entries, devinfo->urb.max_gs_entries);

   /* Ensure that we program a multiple of the granularity. */
   nr_vs_entries = ROUND_DOWN_TO(nr_vs_entries, vs_granularity);
   nr_gs_entries = ROUND_DOWN_TO(nr_gs_entries, gs_granularity);

   /* Finally, sanity check to make sure we have at least the minimum number
    * of entries needed for each stage.
    */
   assert(nr_vs_entries >= devinfo->urb.min_vs_entries);
   if (gs_present)
      assert(nr_gs_entries >= 2);

   /* Lay out the URB in the following order:
    * - push constants
    * - VS
    * - GS
    */
   pipeline->urb.vs_start = push_constant_chunks;
   pipeline->urb.vs_size = vs_size;
   pipeline->urb.nr_vs_entries = nr_vs_entries;

   pipeline->urb.gs_start = push_constant_chunks + vs_chunks;
   pipeline->urb.gs_size = gs_size;
   pipeline->urb.nr_gs_entries = nr_gs_entries;
}

static const struct {
   uint32_t token;
   gl_shader_stage stage;
   const char *name;
} stage_info[] = {
   { GL_VERTEX_SHADER, MESA_SHADER_VERTEX, "vertex" },
   { GL_TESS_CONTROL_SHADER, (gl_shader_stage)-1,"tess control" },
   { GL_TESS_EVALUATION_SHADER, (gl_shader_stage)-1, "tess evaluation" },
   { GL_GEOMETRY_SHADER, MESA_SHADER_GEOMETRY, "geometry" },
   { GL_FRAGMENT_SHADER, MESA_SHADER_FRAGMENT, "fragment" },
   { GL_COMPUTE_SHADER, MESA_SHADER_COMPUTE, "compute" },
};

struct spirv_header{
   uint32_t magic;
   uint32_t version;
   uint32_t gen_magic;
};

static void
setup_nir_io(struct gl_shader *mesa_shader,
             nir_shader *shader)
{
   struct gl_program *prog = mesa_shader->Program;
   foreach_list_typed(nir_variable, var, node, &shader->inputs) {
      prog->InputsRead |= BITFIELD64_BIT(var->data.location);
      if (shader->stage == MESA_SHADER_FRAGMENT) {
         struct gl_fragment_program *fprog = (struct gl_fragment_program *)prog;

         fprog->InterpQualifier[var->data.location] =
            (glsl_interp_qualifier)var->data.interpolation;
         if (var->data.centroid)
            fprog->IsCentroid |= BITFIELD64_BIT(var->data.location);
         if (var->data.sample)
            fprog->IsSample |= BITFIELD64_BIT(var->data.location);
      }
   }

   foreach_list_typed(nir_variable, var, node, &shader->outputs) {
      prog->OutputsWritten |= BITFIELD64_BIT(var->data.location);
   }

   shader->info.inputs_read = prog->InputsRead;
   shader->info.outputs_written = prog->OutputsWritten;

   mesa_shader->num_uniform_components = shader->num_uniforms;
}

static void
anv_compile_shader_spirv(struct anv_compiler *compiler,
                         struct gl_shader_program *program,
                         struct anv_pipeline *pipeline, uint32_t stage)
{
   struct brw_context *brw = compiler->brw;
   struct anv_shader *shader = pipeline->shaders[stage];
   struct gl_shader *mesa_shader;
   int name = 0;

   mesa_shader = brw_new_shader(&brw->ctx, name, stage_info[stage].token);
   fail_if(mesa_shader == NULL,
           "failed to create %s shader\n", stage_info[stage].name);

#define CREATE_PROGRAM(stage) \
   _mesa_init_##stage##_program(&brw->ctx, &ralloc(mesa_shader, struct brw_##stage##_program)->program, 0, 0)

   bool is_scalar;
   struct gl_program *prog;
   switch (stage) {
   case VK_SHADER_STAGE_VERTEX:
      prog = CREATE_PROGRAM(vertex);
      is_scalar = compiler->screen->compiler->scalar_vs;
      break;
   case VK_SHADER_STAGE_GEOMETRY:
      prog = CREATE_PROGRAM(geometry);
      is_scalar = false;
      break;
   case VK_SHADER_STAGE_FRAGMENT:
      prog = CREATE_PROGRAM(fragment);
      is_scalar = true;
      break;
   case VK_SHADER_STAGE_COMPUTE:
      prog = CREATE_PROGRAM(compute);
      is_scalar = true;
      break;
   default:
      unreachable("Unsupported shader stage");
   }
   _mesa_reference_program(&brw->ctx, &mesa_shader->Program, prog);

   mesa_shader->Program->Parameters =
      rzalloc(mesa_shader, struct gl_program_parameter_list);

   mesa_shader->Type = stage_info[stage].token;
   mesa_shader->Stage = stage_info[stage].stage;

   struct gl_shader_compiler_options *glsl_options =
      &compiler->screen->compiler->glsl_compiler_options[stage_info[stage].stage];

   if (shader->module->nir) {
      /* Some things such as our meta clear/blit code will give us a NIR
       * shader directly.  In that case, we just ignore the SPIR-V entirely
       * and just use the NIR shader */
      mesa_shader->Program->nir = shader->module->nir;
      mesa_shader->Program->nir->options = glsl_options->NirOptions;
   } else {
      uint32_t *spirv = (uint32_t *) shader->module->data;
      assert(spirv[0] == SPIR_V_MAGIC_NUMBER);
      assert(shader->module->size % 4 == 0);

      mesa_shader->Program->nir =
         spirv_to_nir(spirv, shader->module->size / 4,
                      stage_info[stage].stage, glsl_options->NirOptions);
   }
   nir_validate_shader(mesa_shader->Program->nir);

   brw_process_nir(mesa_shader->Program->nir,
                   compiler->screen->devinfo,
                   NULL, mesa_shader->Stage, is_scalar);

   setup_nir_io(mesa_shader, mesa_shader->Program->nir);

   fail_if(mesa_shader->Program->nir == NULL,
           "failed to translate SPIR-V to NIR\n");

   _mesa_reference_shader(&brw->ctx, &program->Shaders[program->NumShaders],
                          mesa_shader);
   program->NumShaders++;
}

static void
add_compiled_stage(struct anv_pipeline *pipeline, uint32_t stage,
                   struct brw_stage_prog_data *prog_data)
{
   struct brw_device_info *devinfo = &pipeline->device->info;
   uint32_t max_threads[] = {
      [VK_SHADER_STAGE_VERTEX]                  = devinfo->max_vs_threads,
      [VK_SHADER_STAGE_TESS_CONTROL]            = 0,
      [VK_SHADER_STAGE_TESS_EVALUATION]         = 0,
      [VK_SHADER_STAGE_GEOMETRY]                = devinfo->max_gs_threads,
      [VK_SHADER_STAGE_FRAGMENT]                = devinfo->max_wm_threads,
      [VK_SHADER_STAGE_COMPUTE]                 = devinfo->max_cs_threads,
   };

   pipeline->prog_data[stage] = prog_data;
   pipeline->active_stages |= 1 << stage;
   pipeline->scratch_start[stage] = pipeline->total_scratch;
   pipeline->total_scratch =
      align_u32(pipeline->total_scratch, 1024) +
      prog_data->total_scratch * max_threads[stage];
}

int
anv_compiler_run(struct anv_compiler *compiler, struct anv_pipeline *pipeline)
{
   struct gl_shader_program *program;
   int name = 0;
   struct brw_context *brw = compiler->brw;

   pipeline->writes_point_size = false;

   /* When we free the pipeline, we detect stages based on the NULL status
    * of various prog_data pointers.  Make them NULL by default.
    */
   memset(pipeline->prog_data, 0, sizeof(pipeline->prog_data));
   memset(pipeline->scratch_start, 0, sizeof(pipeline->scratch_start));

   brw->use_rep_send = pipeline->use_repclear;
   brw->no_simd8 = pipeline->use_repclear;

   program = _mesa_new_shader_program(name);
   program->Shaders = (struct gl_shader **)
      calloc(VK_SHADER_STAGE_NUM, sizeof(struct gl_shader *));
   fail_if(program == NULL || program->Shaders == NULL,
           "failed to create program\n");

   for (unsigned i = 0; i < VK_SHADER_STAGE_NUM; i++) {
      if (pipeline->shaders[i])
         anv_compile_shader_spirv(compiler, program, pipeline, i);
   }

   for (unsigned i = 0; i < program->NumShaders; i++) {
      struct gl_shader *shader = program->Shaders[i];
      program->_LinkedShaders[shader->Stage] = shader;
   }

   bool success;
   pipeline->active_stages = 0;
   pipeline->total_scratch = 0;

   if (pipeline->shaders[VK_SHADER_STAGE_VERTEX]) {
      struct brw_vs_prog_key vs_key;
      struct gl_vertex_program *vp = (struct gl_vertex_program *)
         program->_LinkedShaders[MESA_SHADER_VERTEX]->Program;
      struct brw_vertex_program *bvp = brw_vertex_program(vp);

      brw_vs_populate_key(brw, bvp, &vs_key);

      success = really_do_vs_prog(brw, program, bvp, &vs_key, pipeline);
      fail_if(!success, "do_wm_prog failed\n");
      add_compiled_stage(pipeline, VK_SHADER_STAGE_VERTEX,
                         &pipeline->vs_prog_data.base.base);

      if (vp->Base.OutputsWritten & VARYING_SLOT_PSIZ)
         pipeline->writes_point_size = true;
   } else {
      memset(&pipeline->vs_prog_data, 0, sizeof(pipeline->vs_prog_data));
      pipeline->vs_simd8 = NO_KERNEL;
      pipeline->vs_vec4 = NO_KERNEL;
   }


   if (pipeline->shaders[VK_SHADER_STAGE_GEOMETRY]) {
      struct brw_gs_prog_key gs_key;
      struct gl_geometry_program *gp = (struct gl_geometry_program *)
         program->_LinkedShaders[MESA_SHADER_GEOMETRY]->Program;
      struct brw_geometry_program *bgp = brw_geometry_program(gp);

      success = anv_codegen_gs_prog(brw, program, bgp, &gs_key, pipeline);
      fail_if(!success, "do_gs_prog failed\n");
      add_compiled_stage(pipeline, VK_SHADER_STAGE_GEOMETRY,
                         &pipeline->gs_prog_data.base.base);

      if (gp->Base.OutputsWritten & VARYING_SLOT_PSIZ)
         pipeline->writes_point_size = true;
   } else {
      pipeline->gs_vec4 = NO_KERNEL;
   }

   if (pipeline->shaders[VK_SHADER_STAGE_FRAGMENT]) {
      struct brw_wm_prog_key wm_key;
      struct gl_fragment_program *fp = (struct gl_fragment_program *)
         program->_LinkedShaders[MESA_SHADER_FRAGMENT]->Program;
      struct brw_fragment_program *bfp = brw_fragment_program(fp);

      brw_wm_populate_key(brw, bfp, &wm_key);

      success = really_do_wm_prog(brw, program, bfp, &wm_key, pipeline);
      fail_if(!success, "do_wm_prog failed\n");
      add_compiled_stage(pipeline, VK_SHADER_STAGE_FRAGMENT,
                         &pipeline->wm_prog_data.base);
   }

   if (pipeline->shaders[VK_SHADER_STAGE_COMPUTE]) {
      struct brw_cs_prog_key cs_key;
      struct gl_compute_program *cp = (struct gl_compute_program *)
         program->_LinkedShaders[MESA_SHADER_COMPUTE]->Program;
      struct brw_compute_program *bcp = brw_compute_program(cp);

      brw_cs_populate_key(brw, bcp, &cs_key);

      success = brw_codegen_cs_prog(brw, program, bcp, &cs_key, pipeline);
      fail_if(!success, "brw_codegen_cs_prog failed\n");
      add_compiled_stage(pipeline, VK_SHADER_STAGE_COMPUTE,
                         &pipeline->cs_prog_data.base);
   }

   _mesa_delete_shader_program(&brw->ctx, program);

   struct anv_device *device = compiler->device;
   while (device->scratch_block_pool.bo.size < pipeline->total_scratch)
      anv_block_pool_alloc(&device->scratch_block_pool);

   gen7_compute_urb_partition(pipeline);

   return 0;
}

/* This badly named function frees the struct anv_pipeline data that the compiler
 * allocates.  Currently just the prog_data structs.
 */
void
anv_compiler_free(struct anv_pipeline *pipeline)
{
   for (uint32_t stage = 0; stage < VK_SHADER_STAGE_NUM; stage++) {
      if (pipeline->prog_data[stage]) {
         free(pipeline->prog_data[stage]->map_entries);
         /* We only ever set up the params array because we don't do
          * non-UBO pull constants
          */
         anv_device_free(pipeline->device, pipeline->prog_data[stage]->param);
      }
   }
}

}