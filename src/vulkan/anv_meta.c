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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "anv_private.h"
#include "anv_meta_spirv_autogen.h"

static void
anv_device_init_meta_clear_state(struct anv_device *device)
{
   /* We don't use a vertex shader for clearing, but instead build and pass
    * the VUEs directly to the rasterization backend.  However, we do need
    * to provide GLSL source for the vertex shader so that the compiler
    * does not dead-code our inputs.
    */
   VkShaderModule vsm = GLSL_VK_SHADER_MODULE(device, VERTEX,
      in vec2 a_pos;
      in vec4 a_color;
      flat out vec4 v_color;
      void main()
      {
         v_color = a_color;
         gl_Position = vec4(a_pos, 0, 1);
      }
   );

   VkShaderModule fsm = GLSL_VK_SHADER_MODULE(device, FRAGMENT,
      out vec4 f_color;
      flat in vec4 v_color;
      void main()
      {
         f_color = v_color;
      }
   );

   VkShader vs;
   anv_CreateShader(anv_device_to_handle(device),
      &(VkShaderCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO,
         .module = vsm,
         .pName = "main",
      }, &vs);

   VkShader fs;
   anv_CreateShader(anv_device_to_handle(device),
      &(VkShaderCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO,
         .module = fsm,
         .pName = "main",
      }, &fs);

   /* We use instanced rendering to clear multiple render targets. We have two
    * vertex buffers: the first vertex buffer holds per-vertex data and
    * provides the vertices for the clear rectangle. The second one holds
    * per-instance data, which consists of the VUE header (which selects the
    * layer) and the color (Vulkan supports per-RT clear colors).
    */
   VkPipelineVertexInputStateCreateInfo vi_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .bindingCount = 2,
      .pVertexBindingDescriptions = (VkVertexInputBindingDescription[]) {
         {
            .binding = 0,
            .strideInBytes = 12,
            .stepRate = VK_VERTEX_INPUT_STEP_RATE_VERTEX
         },
         {
            .binding = 1,
            .strideInBytes = 32,
            .stepRate = VK_VERTEX_INPUT_STEP_RATE_INSTANCE
         },
      },
      .attributeCount = 3,
      .pVertexAttributeDescriptions = (VkVertexInputAttributeDescription[]) {
         {
            /* VUE Header */
            .location = 0,
            .binding = 1,
            .format = VK_FORMAT_R32G32B32A32_UINT,
            .offsetInBytes = 0
         },
         {
            /* Position */
            .location = 1,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offsetInBytes = 0
         },
         {
            /* Color */
            .location = 2,
            .binding = 1,
            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
            .offsetInBytes = 16
         }
      }
   };

   anv_graphics_pipeline_create(anv_device_to_handle(device),
      &(VkGraphicsPipelineCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,

         .stageCount = 2,
         .pStages = (VkPipelineShaderStageCreateInfo[]) {
            {
               .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
               .stage = VK_SHADER_STAGE_VERTEX,
               .shader = vs,
               .pSpecializationInfo = NULL
            }, {
               .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
               .stage = VK_SHADER_STAGE_FRAGMENT,
               .shader = fs,
               .pSpecializationInfo = NULL,
            }
         },
         .pVertexInputState = &vi_create_info,
         .pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
            .primitiveRestartEnable = false,
         },
         .pRasterState = &(VkPipelineRasterStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTER_STATE_CREATE_INFO,
            .depthClipEnable = true,
            .rasterizerDiscardEnable = false,
            .fillMode = VK_FILL_MODE_SOLID,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_CCW
         },
         .pDepthStencilState = &(VkPipelineDepthStencilStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = true,
            .depthWriteEnable = true,
            .depthCompareOp = VK_COMPARE_OP_ALWAYS,
            .depthBoundsEnable = false,
            .stencilTestEnable = true,
            .front = (VkStencilOpState) {
               .stencilPassOp = VK_STENCIL_OP_REPLACE,
               .stencilCompareOp = VK_COMPARE_OP_ALWAYS,
            },
            .back = (VkStencilOpState) {
               .stencilPassOp = VK_STENCIL_OP_REPLACE,
               .stencilCompareOp = VK_COMPARE_OP_ALWAYS,
            },
         },
         .pColorBlendState = &(VkPipelineColorBlendStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = (VkPipelineColorBlendAttachmentState []) {
               { .channelWriteMask = VK_CHANNEL_A_BIT |
                    VK_CHANNEL_R_BIT | VK_CHANNEL_G_BIT | VK_CHANNEL_B_BIT },
            }
         },
         .flags = 0,
      },
      &(struct anv_graphics_pipeline_create_info) {
         .use_repclear = true,
         .disable_viewport = true,
         .disable_vs = true,
         .use_rectlist = true
      },
      &device->meta_state.clear.pipeline);

   anv_DestroyShaderModule(anv_device_to_handle(device), vsm);
   anv_DestroyShaderModule(anv_device_to_handle(device), fsm);
   anv_DestroyShader(anv_device_to_handle(device), vs);
   anv_DestroyShader(anv_device_to_handle(device), fs);
}

#define NUM_VB_USED 2
struct anv_saved_state {
   struct anv_vertex_binding old_vertex_bindings[NUM_VB_USED];
   struct anv_descriptor_set *old_descriptor_set0;
   struct anv_pipeline *old_pipeline;
   struct anv_dynamic_cb_state *old_cb_state;
};

static void
anv_cmd_buffer_save(struct anv_cmd_buffer *cmd_buffer,
                    struct anv_saved_state *state)
{
   state->old_pipeline = cmd_buffer->state.pipeline;
   state->old_descriptor_set0 = cmd_buffer->state.descriptors[0].set;
   memcpy(state->old_vertex_bindings, cmd_buffer->state.vertex_bindings,
          sizeof(state->old_vertex_bindings));
   state->old_cb_state = cmd_buffer->state.cb_state;
}

static void
anv_cmd_buffer_restore(struct anv_cmd_buffer *cmd_buffer,
                       const struct anv_saved_state *state)
{
   cmd_buffer->state.pipeline = state->old_pipeline;
   cmd_buffer->state.descriptors[0].set = state->old_descriptor_set0;
   memcpy(cmd_buffer->state.vertex_bindings, state->old_vertex_bindings,
          sizeof(state->old_vertex_bindings));

   cmd_buffer->state.vb_dirty |= (1 << NUM_VB_USED) - 1;
   cmd_buffer->state.dirty |= ANV_CMD_BUFFER_PIPELINE_DIRTY;
   cmd_buffer->state.descriptors_dirty |= VK_SHADER_STAGE_VERTEX_BIT;

   if (cmd_buffer->state.cb_state != state->old_cb_state) {
      cmd_buffer->state.cb_state = state->old_cb_state;
      cmd_buffer->state.dirty |= ANV_CMD_BUFFER_CB_DIRTY;
   }
}

struct vue_header {
   uint32_t Reserved;
   uint32_t RTAIndex;
   uint32_t ViewportIndex;
   float PointWidth;
};

struct clear_instance_data {
   struct vue_header vue_header;
   VkClearColorValue color;
};

static void
meta_emit_clear(struct anv_cmd_buffer *cmd_buffer,
                int num_instances,
                struct clear_instance_data *instance_data,
                VkClearDepthStencilValue ds_clear_value)
{
   struct anv_device *device = cmd_buffer->device;
   struct anv_framebuffer *fb = cmd_buffer->state.framebuffer;
   struct anv_state state;
   uint32_t size;

   const float vertex_data[] = {
      /* Rect-list coordinates */
            0.0,        0.0, ds_clear_value.depth,
      fb->width,        0.0, ds_clear_value.depth,
      fb->width, fb->height, ds_clear_value.depth,

      /* Align to 16 bytes */
      0.0, 0.0, 0.0,
   };

   size = sizeof(vertex_data) + num_instances * sizeof(*instance_data);
   state = anv_state_stream_alloc(&cmd_buffer->surface_state_stream, size, 16);

   /* Copy in the vertex and instance data */
   memcpy(state.map, vertex_data, sizeof(vertex_data));
   memcpy(state.map + sizeof(vertex_data), instance_data,
          num_instances * sizeof(*instance_data));

   struct anv_buffer vertex_buffer = {
      .device = cmd_buffer->device,
      .size = size,
      .bo = &device->surface_state_block_pool.bo,
      .offset = state.offset
   };

   anv_CmdBindVertexBuffers(anv_cmd_buffer_to_handle(cmd_buffer), 0, 2,
      (VkBuffer[]) {
         anv_buffer_to_handle(&vertex_buffer),
         anv_buffer_to_handle(&vertex_buffer)
      },
      (VkDeviceSize[]) {
         0,
         sizeof(vertex_data)
      });

   if (cmd_buffer->state.pipeline != anv_pipeline_from_handle(device->meta_state.clear.pipeline))
      anv_CmdBindPipeline(anv_cmd_buffer_to_handle(cmd_buffer),
                          VK_PIPELINE_BIND_POINT_GRAPHICS,
                          device->meta_state.clear.pipeline);

   /* We don't need anything here, only set if not already set. */
   if (cmd_buffer->state.rs_state == NULL)
      anv_CmdBindDynamicRasterState(anv_cmd_buffer_to_handle(cmd_buffer),
                                    device->meta_state.shared.rs_state);

   if (cmd_buffer->state.vp_state == NULL)
      anv_CmdBindDynamicViewportState(anv_cmd_buffer_to_handle(cmd_buffer),
                                      cmd_buffer->state.framebuffer->vp_state);

   if (cmd_buffer->state.ds_state == NULL)
      anv_CmdBindDynamicDepthStencilState(anv_cmd_buffer_to_handle(cmd_buffer),
                                          device->meta_state.shared.ds_state);

   if (cmd_buffer->state.cb_state == NULL)
      anv_CmdBindDynamicColorBlendState(anv_cmd_buffer_to_handle(cmd_buffer),
                                        device->meta_state.shared.cb_state);

   driver_layer->CmdDraw(anv_cmd_buffer_to_handle(cmd_buffer), 0, 3, 0, num_instances);
}

void
anv_cmd_buffer_clear_attachments(struct anv_cmd_buffer *cmd_buffer,
                                 struct anv_render_pass *pass,
                                 const VkClearValue *clear_values)
{
   struct anv_saved_state saved_state;

   if (pass->has_stencil_clear_attachment)
      anv_finishme("stencil clear");

   if (pass->num_color_clear_attachments == 0)
      return;

   struct clear_instance_data instance_data[pass->num_color_clear_attachments];
   uint32_t color_attachments[pass->num_color_clear_attachments];
   uint32_t ds_attachment = VK_ATTACHMENT_UNUSED;
   VkClearDepthStencilValue ds_clear_value = {0};

   int layer = 0;
   for (uint32_t i = 0; i < pass->attachment_count; i++) {
      const struct anv_render_pass_attachment *att = &pass->attachments[i];

      if (att->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
         if (anv_format_is_color(att->format)) {
            instance_data[layer] = (struct clear_instance_data) {
               .vue_header = {
                  .RTAIndex = i,
                  .ViewportIndex = 0,
                  .PointWidth = 0.0
               },
               .color = clear_values[i].color,
            };
            color_attachments[layer] = i;
            layer++;
         } else if (att->format->depth_format) {
            assert(ds_attachment == VK_ATTACHMENT_UNUSED);
            ds_attachment = i;
            ds_clear_value= clear_values[ds_attachment].ds;
         }
      } else if (att->stencil_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
         assert(att->format->has_stencil);
         anv_finishme("stencil clear");
      }
   }

   anv_cmd_buffer_save(cmd_buffer, &saved_state);

   struct anv_subpass subpass = {
      .input_count = 0,
      .color_count = pass->num_color_clear_attachments,
      .color_attachments = color_attachments,
      .depth_stencil_attachment = ds_attachment,
   };

   anv_cmd_buffer_begin_subpass(cmd_buffer, &subpass);

   meta_emit_clear(cmd_buffer, pass->num_color_clear_attachments,
                   instance_data, ds_clear_value);

   /* Restore API state */
   anv_cmd_buffer_restore(cmd_buffer, &saved_state);
}

static void
anv_device_init_meta_blit_state(struct anv_device *device)
{
   /* We don't use a vertex shader for clearing, but instead build and pass
    * the VUEs directly to the rasterization backend.  However, we do need
    * to provide GLSL source for the vertex shader so that the compiler
    * does not dead-code our inputs.
    */
   VkShaderModule vsm = GLSL_VK_SHADER_MODULE(device, VERTEX,
      in vec2 a_pos;
      in vec2 a_tex_coord;
      out vec4 v_tex_coord;
      void main()
      {
         v_tex_coord = vec4(a_tex_coord, 0, 1);
         gl_Position = vec4(a_pos, 0, 1);
      }
   );

   VkShaderModule fsm = GLSL_VK_SHADER_MODULE(device, FRAGMENT,
      out vec4 f_color;
      in vec4 v_tex_coord;
      layout(set = 0, binding = 0) uniform sampler2D u_tex;
      void main()
      {
         f_color = texture(u_tex, v_tex_coord.xy);
      }
   );

   VkShader vs;
   anv_CreateShader(anv_device_to_handle(device),
      &(VkShaderCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO,
         .module = vsm,
         .pName = "main",
      }, &vs);

   VkShader fs;
   anv_CreateShader(anv_device_to_handle(device),
      &(VkShaderCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO,
         .module = fsm,
         .pName = "main",
      }, &fs);

   VkPipelineVertexInputStateCreateInfo vi_create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
      .bindingCount = 2,
      .pVertexBindingDescriptions = (VkVertexInputBindingDescription[]) {
         {
            .binding = 0,
            .strideInBytes = 0,
            .stepRate = VK_VERTEX_INPUT_STEP_RATE_VERTEX
         },
         {
            .binding = 1,
            .strideInBytes = 16,
            .stepRate = VK_VERTEX_INPUT_STEP_RATE_VERTEX
         },
      },
      .attributeCount = 3,
      .pVertexAttributeDescriptions = (VkVertexInputAttributeDescription[]) {
         {
            /* VUE Header */
            .location = 0,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32A32_UINT,
            .offsetInBytes = 0
         },
         {
            /* Position */
            .location = 1,
            .binding = 1,
            .format = VK_FORMAT_R32G32_SFLOAT,
            .offsetInBytes = 0
         },
         {
            /* Texture Coordinate */
            .location = 2,
            .binding = 1,
            .format = VK_FORMAT_R32G32_SFLOAT,
            .offsetInBytes = 8
         }
      }
   };

   VkDescriptorSetLayoutCreateInfo ds_layout_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .count = 1,
      .pBinding = (VkDescriptorSetLayoutBinding[]) {
         {
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .arraySize = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = NULL
         },
      }
   };
   anv_CreateDescriptorSetLayout(anv_device_to_handle(device), &ds_layout_info,
                                 &device->meta_state.blit.ds_layout);

   anv_CreatePipelineLayout(anv_device_to_handle(device),
      &(VkPipelineLayoutCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
         .descriptorSetCount = 1,
         .pSetLayouts = &device->meta_state.blit.ds_layout,
      },
      &device->meta_state.blit.pipeline_layout);

   anv_graphics_pipeline_create(anv_device_to_handle(device),
      &(VkGraphicsPipelineCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
         .stageCount = 2,
         .pStages = (VkPipelineShaderStageCreateInfo[]) {
            {
               .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
               .stage = VK_SHADER_STAGE_VERTEX,
               .shader = vs,
               .pSpecializationInfo = NULL
            }, {
               .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
               .stage = VK_SHADER_STAGE_FRAGMENT,
               .shader = fs,
               .pSpecializationInfo = NULL
            },
         },
         .pVertexInputState = &vi_create_info,
         .pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
            .primitiveRestartEnable = false,
         },
         .pRasterState = &(VkPipelineRasterStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTER_STATE_CREATE_INFO,
            .depthClipEnable = true,
            .rasterizerDiscardEnable = false,
            .fillMode = VK_FILL_MODE_SOLID,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_CCW
         },
         .pColorBlendState = &(VkPipelineColorBlendStateCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = (VkPipelineColorBlendAttachmentState []) {
               { .channelWriteMask = VK_CHANNEL_A_BIT |
                    VK_CHANNEL_R_BIT | VK_CHANNEL_G_BIT | VK_CHANNEL_B_BIT },
            }
         },
         .flags = 0,
         .layout = device->meta_state.blit.pipeline_layout,
      },
      &(struct anv_graphics_pipeline_create_info) {
         .use_repclear = false,
         .disable_viewport = true,
         .disable_scissor = true,
         .disable_vs = true,
         .use_rectlist = true
      },
      &device->meta_state.blit.pipeline);

   anv_DestroyShaderModule(anv_device_to_handle(device), vsm);
   anv_DestroyShader(anv_device_to_handle(device), vs);
   anv_DestroyShaderModule(anv_device_to_handle(device), fsm);
   anv_DestroyShader(anv_device_to_handle(device), fs);
}

static void
meta_prepare_blit(struct anv_cmd_buffer *cmd_buffer,
                  struct anv_saved_state *saved_state)
{
   struct anv_device *device = cmd_buffer->device;

   anv_cmd_buffer_save(cmd_buffer, saved_state);

   if (cmd_buffer->state.pipeline != anv_pipeline_from_handle(device->meta_state.blit.pipeline))
      anv_CmdBindPipeline(anv_cmd_buffer_to_handle(cmd_buffer),
                          VK_PIPELINE_BIND_POINT_GRAPHICS,
                          device->meta_state.blit.pipeline);

   /* We don't need anything here, only set if not already set. */
   if (cmd_buffer->state.rs_state == NULL)
      anv_CmdBindDynamicRasterState(anv_cmd_buffer_to_handle(cmd_buffer),
                                    device->meta_state.shared.rs_state);
   if (cmd_buffer->state.ds_state == NULL)
      anv_CmdBindDynamicDepthStencilState(anv_cmd_buffer_to_handle(cmd_buffer),
                                          device->meta_state.shared.ds_state);

   anv_CmdBindDynamicColorBlendState(anv_cmd_buffer_to_handle(cmd_buffer),
                                     device->meta_state.shared.cb_state);
}

struct blit_region {
   VkOffset3D src_offset;
   VkExtent3D src_extent;
   VkOffset3D dest_offset;
   VkExtent3D dest_extent;
};

static void
meta_emit_blit(struct anv_cmd_buffer *cmd_buffer,
               struct anv_image_view *src,
               VkOffset3D src_offset,
               VkExtent3D src_extent,
               struct anv_color_attachment_view *dest,
               VkOffset3D dest_offset,
               VkExtent3D dest_extent)
{
   struct anv_device *device = cmd_buffer->device;
   VkDescriptorPool dummy_desc_pool = { .handle = 1 };

   struct blit_vb_data {
      float pos[2];
      float tex_coord[2];
   } *vb_data;

   unsigned vb_size = sizeof(struct vue_header) + 3 * sizeof(*vb_data);

   struct anv_state vb_state =
      anv_state_stream_alloc(&cmd_buffer->surface_state_stream, vb_size, 16);
   memset(vb_state.map, 0, sizeof(struct vue_header));
   vb_data = vb_state.map + sizeof(struct vue_header);

   vb_data[0] = (struct blit_vb_data) {
      .pos = {
         dest_offset.x + dest_extent.width,
         dest_offset.y + dest_extent.height,
      },
      .tex_coord = {
         (float)(src_offset.x + src_extent.width) / (float)src->extent.width,
         (float)(src_offset.y + src_extent.height) / (float)src->extent.height,
      },
   };

   vb_data[1] = (struct blit_vb_data) {
      .pos = {
         dest_offset.x,
         dest_offset.y + dest_extent.height,
      },
      .tex_coord = {
         (float)src_offset.x / (float)src->extent.width,
         (float)(src_offset.y + src_extent.height) / (float)src->extent.height,
      },
   };

   vb_data[2] = (struct blit_vb_data) {
      .pos = {
         dest_offset.x,
         dest_offset.y,
      },
      .tex_coord = {
         (float)src_offset.x / (float)src->extent.width,
         (float)src_offset.y / (float)src->extent.height,
      },
   };

   struct anv_buffer vertex_buffer = {
      .device = device,
      .size = vb_size,
      .bo = &device->surface_state_block_pool.bo,
      .offset = vb_state.offset,
   };

   anv_CmdBindVertexBuffers(anv_cmd_buffer_to_handle(cmd_buffer), 0, 2,
      (VkBuffer[]) {
         anv_buffer_to_handle(&vertex_buffer),
         anv_buffer_to_handle(&vertex_buffer)
      },
      (VkDeviceSize[]) {
         0,
         sizeof(struct vue_header),
      });

   uint32_t count;
   VkDescriptorSet set;
   anv_AllocDescriptorSets(anv_device_to_handle(device), dummy_desc_pool,
                           VK_DESCRIPTOR_SET_USAGE_ONE_SHOT,
                           1, &device->meta_state.blit.ds_layout, &set, &count);
   anv_UpdateDescriptorSets(anv_device_to_handle(device),
      1, /* writeCount */
      (VkWriteDescriptorSet[]) {
         {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .destSet = set,
            .destBinding = 0,
            .destArrayElement = 0,
            .count = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .pDescriptors = (VkDescriptorInfo[]) {
               {
                  .imageView = anv_image_view_to_handle(src),
                  .imageLayout = VK_IMAGE_LAYOUT_GENERAL
               },
            }
         }
      }, 0, NULL);

   VkFramebuffer fb;
   anv_CreateFramebuffer(anv_device_to_handle(device),
      &(VkFramebufferCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
         .attachmentCount = 1,
         .pAttachments = (VkAttachmentBindInfo[]) {
            {
               .view = anv_attachment_view_to_handle(&dest->base),
               .layout = VK_IMAGE_LAYOUT_GENERAL
            }
         },
         .width = dest->base.extent.width,
         .height = dest->base.extent.height,
         .layers = 1
      }, &fb);

   VkRenderPass pass;
   anv_CreateRenderPass(anv_device_to_handle(device),
      &(VkRenderPassCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
         .attachmentCount = 1,
         .pAttachments = &(VkAttachmentDescription) {
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION,
            .format = dest->view.format->vk_format,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
            .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
         },
         .subpassCount = 1,
         .pSubpasses = &(VkSubpassDescription) {
            .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION,
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .inputCount = 0,
            .colorCount = 1,
            .colorAttachments = &(VkAttachmentReference) {
               .attachment = 0,
               .layout = VK_IMAGE_LAYOUT_GENERAL,
            },
            .resolveAttachments = NULL,
            .depthStencilAttachment = (VkAttachmentReference) {
               .attachment = VK_ATTACHMENT_UNUSED,
               .layout = VK_IMAGE_LAYOUT_GENERAL,
            },
            .preserveCount = 1,
            .preserveAttachments = &(VkAttachmentReference) {
               .attachment = 0,
               .layout = VK_IMAGE_LAYOUT_GENERAL,
            },
         },
         .dependencyCount = 0,
      }, &pass);

   driver_layer->CmdBeginRenderPass(anv_cmd_buffer_to_handle(cmd_buffer),
      &(VkRenderPassBeginInfo) {
         .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
         .renderPass = pass,
         .framebuffer = fb,
         .renderArea = {
            .offset = { dest_offset.x, dest_offset.y },
            .extent = { dest_extent.width, dest_extent.height },
         },
         .attachmentCount = 1,
         .pAttachmentClearValues = NULL,
      }, VK_RENDER_PASS_CONTENTS_INLINE);

   anv_CmdBindDynamicViewportState(anv_cmd_buffer_to_handle(cmd_buffer),
                                   anv_framebuffer_from_handle(fb)->vp_state);

   anv_CmdBindDescriptorSets(anv_cmd_buffer_to_handle(cmd_buffer),
                             VK_PIPELINE_BIND_POINT_GRAPHICS,
                             device->meta_state.blit.pipeline_layout, 0, 1,
                             &set, 0, NULL);

   driver_layer->CmdDraw(anv_cmd_buffer_to_handle(cmd_buffer), 0, 3, 0, 1);

   driver_layer->CmdEndRenderPass(anv_cmd_buffer_to_handle(cmd_buffer));

   /* At the point where we emit the draw call, all data from the
    * descriptor sets, etc. has been used.  We are free to delete it.
    */
   anv_descriptor_set_destroy(device, anv_descriptor_set_from_handle(set));
   anv_DestroyFramebuffer(anv_device_to_handle(device), fb);
   anv_DestroyRenderPass(anv_device_to_handle(device), pass);
}

static void
meta_finish_blit(struct anv_cmd_buffer *cmd_buffer,
                 const struct anv_saved_state *saved_state)
{
   anv_cmd_buffer_restore(cmd_buffer, saved_state);
}

static VkFormat
vk_format_for_cpp(int cpp)
{
   switch (cpp) {
   case 1: return VK_FORMAT_R8_UINT;
   case 2: return VK_FORMAT_R8G8_UINT;
   case 3: return VK_FORMAT_R8G8B8_UINT;
   case 4: return VK_FORMAT_R8G8B8A8_UINT;
   case 6: return VK_FORMAT_R16G16B16_UINT;
   case 8: return VK_FORMAT_R16G16B16A16_UINT;
   case 12: return VK_FORMAT_R32G32B32_UINT;
   case 16: return VK_FORMAT_R32G32B32A32_UINT;
   default:
      unreachable("Invalid format cpp");
   }
}

static void
do_buffer_copy(struct anv_cmd_buffer *cmd_buffer,
               struct anv_bo *src, uint64_t src_offset,
               struct anv_bo *dest, uint64_t dest_offset,
               int width, int height, VkFormat copy_format)
{
   VkDevice vk_device = anv_device_to_handle(cmd_buffer->device);

   VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = copy_format,
      .extent = {
         .width = width,
         .height = height,
         .depth = 1,
      },
      .mipLevels = 1,
      .arraySize = 1,
      .samples = 1,
      .tiling = VK_IMAGE_TILING_LINEAR,
      .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
      .flags = 0,
   };

   VkImage src_image, dest_image;
   anv_CreateImage(vk_device, &image_info, &src_image);
   anv_CreateImage(vk_device, &image_info, &dest_image);

   /* We could use a vk call to bind memory, but that would require
    * creating a dummy memory object etc. so there's really no point.
    */
   anv_image_from_handle(src_image)->bo = src;
   anv_image_from_handle(src_image)->offset = src_offset;
   anv_image_from_handle(dest_image)->bo = dest;
   anv_image_from_handle(dest_image)->offset = dest_offset;

   struct anv_image_view src_view;
   anv_image_view_init(&src_view, cmd_buffer->device,
      &(VkImageViewCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
         .image = src_image,
         .viewType = VK_IMAGE_VIEW_TYPE_2D,
         .format = copy_format,
         .channels = {
            VK_CHANNEL_SWIZZLE_R,
            VK_CHANNEL_SWIZZLE_G,
            VK_CHANNEL_SWIZZLE_B,
            VK_CHANNEL_SWIZZLE_A
         },
         .subresourceRange = {
            .aspect = VK_IMAGE_ASPECT_COLOR,
            .baseMipLevel = 0,
            .mipLevels = 1,
            .baseArraySlice = 0,
            .arraySize = 1
         },
      },
      cmd_buffer);

   struct anv_color_attachment_view dest_view;
   anv_color_attachment_view_init(&dest_view, cmd_buffer->device,
      &(VkAttachmentViewCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_ATTACHMENT_VIEW_CREATE_INFO,
         .image = dest_image,
         .format = copy_format,
         .mipLevel = 0,
         .baseArraySlice = 0,
         .arraySize = 1,
      },
      cmd_buffer);

   meta_emit_blit(cmd_buffer,
                  &src_view,
                  (VkOffset3D) { 0, 0, 0 },
                  (VkExtent3D) { width, height, 1 },
                  &dest_view,
                  (VkOffset3D) { 0, 0, 0 },
                  (VkExtent3D) { width, height, 1 });

   anv_DestroyImage(vk_device, src_image);
   anv_DestroyImage(vk_device, dest_image);
}

void anv_CmdCopyBuffer(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    srcBuffer,
    VkBuffer                                    destBuffer,
    uint32_t                                    regionCount,
    const VkBufferCopy*                         pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_buffer, src_buffer, srcBuffer);
   ANV_FROM_HANDLE(anv_buffer, dest_buffer, destBuffer);

   struct anv_saved_state saved_state;

   meta_prepare_blit(cmd_buffer, &saved_state);

   for (unsigned r = 0; r < regionCount; r++) {
      uint64_t src_offset = src_buffer->offset + pRegions[r].srcOffset;
      uint64_t dest_offset = dest_buffer->offset + pRegions[r].destOffset;
      uint64_t copy_size = pRegions[r].copySize;

      /* First, we compute the biggest format that can be used with the
       * given offsets and size.
       */
      int cpp = 16;

      int fs = ffs(src_offset) - 1;
      if (fs != -1)
         cpp = MIN2(cpp, 1 << fs);
      assert(src_offset % cpp == 0);

      fs = ffs(dest_offset) - 1;
      if (fs != -1)
         cpp = MIN2(cpp, 1 << fs);
      assert(dest_offset % cpp == 0);

      fs = ffs(pRegions[r].copySize) - 1;
      if (fs != -1)
         cpp = MIN2(cpp, 1 << fs);
      assert(pRegions[r].copySize % cpp == 0);

      VkFormat copy_format = vk_format_for_cpp(cpp);

      /* This is maximum possible width/height our HW can handle */
      uint64_t max_surface_dim = 1 << 14;

      /* First, we make a bunch of max-sized copies */
      uint64_t max_copy_size = max_surface_dim * max_surface_dim * cpp;
      while (copy_size > max_copy_size) {
         do_buffer_copy(cmd_buffer, src_buffer->bo, src_offset,
                        dest_buffer->bo, dest_offset,
                        max_surface_dim, max_surface_dim, copy_format);
         copy_size -= max_copy_size;
         src_offset += max_copy_size;
         dest_offset += max_copy_size;
      }

      uint64_t height = copy_size / (max_surface_dim * cpp);
      assert(height < max_surface_dim);
      if (height != 0) {
         uint64_t rect_copy_size = height * max_surface_dim * cpp;
         do_buffer_copy(cmd_buffer, src_buffer->bo, src_offset,
                        dest_buffer->bo, dest_offset,
                        max_surface_dim, height, copy_format);
         copy_size -= rect_copy_size;
         src_offset += rect_copy_size;
         dest_offset += rect_copy_size;
      }

      if (copy_size != 0) {
         do_buffer_copy(cmd_buffer, src_buffer->bo, src_offset,
                        dest_buffer->bo, dest_offset,
                        copy_size / cpp, 1, copy_format);
      }
   }

   meta_finish_blit(cmd_buffer, &saved_state);
}

void anv_CmdCopyImage(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     destImage,
    VkImageLayout                               destImageLayout,
    uint32_t                                    regionCount,
    const VkImageCopy*                          pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_image, src_image, srcImage);
   ANV_FROM_HANDLE(anv_image, dest_image, destImage);

   struct anv_saved_state saved_state;

   meta_prepare_blit(cmd_buffer, &saved_state);

   for (unsigned r = 0; r < regionCount; r++) {
      struct anv_image_view src_view;
      anv_image_view_init(&src_view, cmd_buffer->device,
         &(VkImageViewCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = srcImage,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = src_image->format->vk_format,
            .channels = {
               VK_CHANNEL_SWIZZLE_R,
               VK_CHANNEL_SWIZZLE_G,
               VK_CHANNEL_SWIZZLE_B,
               VK_CHANNEL_SWIZZLE_A
            },
            .subresourceRange = {
               .aspect = pRegions[r].srcSubresource.aspect,
               .baseMipLevel = pRegions[r].srcSubresource.mipLevel,
               .mipLevels = 1,
               .baseArraySlice = pRegions[r].srcSubresource.arraySlice,
               .arraySize = 1
            },
         },
         cmd_buffer);

      struct anv_color_attachment_view dest_view;
      anv_color_attachment_view_init(&dest_view, cmd_buffer->device,
         &(VkAttachmentViewCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_VIEW_CREATE_INFO,
            .image = destImage,
            .format = dest_image->format->vk_format,
            .mipLevel = pRegions[r].destSubresource.mipLevel,
            .baseArraySlice = pRegions[r].destSubresource.arraySlice,
            .arraySize = 1,
         },
         cmd_buffer);

      meta_emit_blit(cmd_buffer,
                     &src_view,
                     pRegions[r].srcOffset,
                     pRegions[r].extent,
                     &dest_view,
                     pRegions[r].destOffset,
                     pRegions[r].extent);
   }

   meta_finish_blit(cmd_buffer, &saved_state);
}

void anv_CmdBlitImage(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     destImage,
    VkImageLayout                               destImageLayout,
    uint32_t                                    regionCount,
    const VkImageBlit*                          pRegions,
    VkTexFilter                                 filter)

{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_image, src_image, srcImage);
   ANV_FROM_HANDLE(anv_image, dest_image, destImage);

   struct anv_saved_state saved_state;

   anv_finishme("respect VkTexFilter");

   meta_prepare_blit(cmd_buffer, &saved_state);

   for (unsigned r = 0; r < regionCount; r++) {
      struct anv_image_view src_view;
      anv_image_view_init(&src_view, cmd_buffer->device,
         &(VkImageViewCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = srcImage,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = src_image->format->vk_format,
            .channels = {
               VK_CHANNEL_SWIZZLE_R,
               VK_CHANNEL_SWIZZLE_G,
               VK_CHANNEL_SWIZZLE_B,
               VK_CHANNEL_SWIZZLE_A
            },
            .subresourceRange = {
               .aspect = pRegions[r].srcSubresource.aspect,
               .baseMipLevel = pRegions[r].srcSubresource.mipLevel,
               .mipLevels = 1,
               .baseArraySlice = pRegions[r].srcSubresource.arraySlice,
               .arraySize = 1
            },
         },
         cmd_buffer);

      struct anv_color_attachment_view dest_view;
      anv_color_attachment_view_init(&dest_view, cmd_buffer->device,
         &(VkAttachmentViewCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_VIEW_CREATE_INFO,
            .image = destImage,
            .format = dest_image->format->vk_format,
            .mipLevel = pRegions[r].destSubresource.mipLevel,
            .baseArraySlice = pRegions[r].destSubresource.arraySlice,
            .arraySize = 1,
         },
         cmd_buffer);

      meta_emit_blit(cmd_buffer,
                     &src_view,
                     pRegions[r].srcOffset,
                     pRegions[r].srcExtent,
                     &dest_view,
                     pRegions[r].destOffset,
                     pRegions[r].destExtent);
   }

   meta_finish_blit(cmd_buffer, &saved_state);
}

void anv_CmdCopyBufferToImage(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    srcBuffer,
    VkImage                                     destImage,
    VkImageLayout                               destImageLayout,
    uint32_t                                    regionCount,
    const VkBufferImageCopy*                    pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_buffer, src_buffer, srcBuffer);
   ANV_FROM_HANDLE(anv_image, dest_image, destImage);
   VkDevice vk_device = anv_device_to_handle(cmd_buffer->device);
   struct anv_saved_state saved_state;

   meta_prepare_blit(cmd_buffer, &saved_state);

   for (unsigned r = 0; r < regionCount; r++) {
      if (pRegions[r].bufferRowLength != 0)
         anv_finishme("bufferRowLength not supported in CopyBufferToImage");
      if (pRegions[r].bufferImageHeight != 0)
         anv_finishme("bufferImageHeight not supported in CopyBufferToImage");

      VkImage srcImage;
      anv_CreateImage(vk_device,
         &(VkImageCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = dest_image->format->vk_format,
            .extent = {
               .width = pRegions[r].imageExtent.width,
               .height = pRegions[r].imageExtent.height,
               .depth = 1,
            },
            .mipLevels = 1,
            .arraySize = 1,
            .samples = 1,
            .tiling = VK_IMAGE_TILING_LINEAR,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
            .flags = 0,
         }, &srcImage);

      ANV_FROM_HANDLE(anv_image, src_image, srcImage);

      /* We could use a vk call to bind memory, but that would require
       * creating a dummy memory object etc. so there's really no point.
       */
      src_image->bo = src_buffer->bo;
      src_image->offset = src_buffer->offset + pRegions[r].bufferOffset;

      struct anv_image_view src_view;
      anv_image_view_init(&src_view, cmd_buffer->device,
         &(VkImageViewCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = anv_image_to_handle(src_image),
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = dest_image->format->vk_format,
            .channels = {
               VK_CHANNEL_SWIZZLE_R,
               VK_CHANNEL_SWIZZLE_G,
               VK_CHANNEL_SWIZZLE_B,
               VK_CHANNEL_SWIZZLE_A
            },
            .subresourceRange = {
               .aspect = pRegions[r].imageSubresource.aspect,
               .baseMipLevel = 0,
               .mipLevels = 1,
               .baseArraySlice = 0,
               .arraySize = 1
            },
         },
         cmd_buffer);

      struct anv_color_attachment_view dest_view;
      anv_color_attachment_view_init(&dest_view, cmd_buffer->device,
         &(VkAttachmentViewCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_VIEW_CREATE_INFO,
            .image = anv_image_to_handle(dest_image),
            .format = dest_image->format->vk_format,
            .mipLevel = pRegions[r].imageSubresource.mipLevel,
            .baseArraySlice = pRegions[r].imageSubresource.arraySlice,
            .arraySize = 1,
         },
         cmd_buffer);

      meta_emit_blit(cmd_buffer,
                     &src_view,
                     (VkOffset3D) { 0, 0, 0 },
                     pRegions[r].imageExtent,
                     &dest_view,
                     pRegions[r].imageOffset,
                     pRegions[r].imageExtent);

      anv_DestroyImage(vk_device, srcImage);
   }

   meta_finish_blit(cmd_buffer, &saved_state);
}

void anv_CmdCopyImageToBuffer(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkBuffer                                    destBuffer,
    uint32_t                                    regionCount,
    const VkBufferImageCopy*                    pRegions)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_image, src_image, srcImage);
   ANV_FROM_HANDLE(anv_buffer, dest_buffer, destBuffer);
   VkDevice vk_device = anv_device_to_handle(cmd_buffer->device);
   struct anv_saved_state saved_state;

   meta_prepare_blit(cmd_buffer, &saved_state);

   for (unsigned r = 0; r < regionCount; r++) {
      if (pRegions[r].bufferRowLength != 0)
         anv_finishme("bufferRowLength not supported in CopyBufferToImage");
      if (pRegions[r].bufferImageHeight != 0)
         anv_finishme("bufferImageHeight not supported in CopyBufferToImage");

      struct anv_image_view src_view;
      anv_image_view_init(&src_view, cmd_buffer->device,
         &(VkImageViewCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = srcImage,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = src_image->format->vk_format,
            .channels = {
               VK_CHANNEL_SWIZZLE_R,
               VK_CHANNEL_SWIZZLE_G,
               VK_CHANNEL_SWIZZLE_B,
               VK_CHANNEL_SWIZZLE_A
            },
            .subresourceRange = {
               .aspect = pRegions[r].imageSubresource.aspect,
               .baseMipLevel = pRegions[r].imageSubresource.mipLevel,
               .mipLevels = 1,
               .baseArraySlice = pRegions[r].imageSubresource.arraySlice,
               .arraySize = 1
            },
         },
         cmd_buffer);

      VkImage destImage;
      anv_CreateImage(vk_device,
         &(VkImageCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = src_image->format->vk_format,
            .extent = {
               .width = pRegions[r].imageExtent.width,
               .height = pRegions[r].imageExtent.height,
               .depth = 1,
            },
            .mipLevels = 1,
            .arraySize = 1,
            .samples = 1,
            .tiling = VK_IMAGE_TILING_LINEAR,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
            .flags = 0,
         }, &destImage);

      ANV_FROM_HANDLE(anv_image, dest_image, destImage);

      /* We could use a vk call to bind memory, but that would require
       * creating a dummy memory object etc. so there's really no point.
       */
      dest_image->bo = dest_buffer->bo;
      dest_image->offset = dest_buffer->offset + pRegions[r].bufferOffset;

      struct anv_color_attachment_view dest_view;
      anv_color_attachment_view_init(&dest_view, cmd_buffer->device,
         &(VkAttachmentViewCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_VIEW_CREATE_INFO,
            .image = destImage,
            .format = src_image->format->vk_format,
            .mipLevel = 0,
            .baseArraySlice = 0,
            .arraySize = 1,
         },
         cmd_buffer);

      meta_emit_blit(cmd_buffer,
                     &src_view,
                     pRegions[r].imageOffset,
                     pRegions[r].imageExtent,
                     &dest_view,
                     (VkOffset3D) { 0, 0, 0 },
                     pRegions[r].imageExtent);

      anv_DestroyImage(vk_device, destImage);
   }

   meta_finish_blit(cmd_buffer, &saved_state);
}

void anv_CmdUpdateBuffer(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    destBuffer,
    VkDeviceSize                                destOffset,
    VkDeviceSize                                dataSize,
    const uint32_t*                             pData)
{
   stub();
}

void anv_CmdFillBuffer(
    VkCmdBuffer                                 cmdBuffer,
    VkBuffer                                    destBuffer,
    VkDeviceSize                                destOffset,
    VkDeviceSize                                fillSize,
    uint32_t                                    data)
{
   stub();
}

void anv_CmdClearColorImage(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     _image,
    VkImageLayout                               imageLayout,
    const VkClearColorValue*                    pColor,
    uint32_t                                    rangeCount,
    const VkImageSubresourceRange*              pRanges)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, cmdBuffer);
   ANV_FROM_HANDLE(anv_image, image, _image);
   struct anv_saved_state saved_state;

   anv_cmd_buffer_save(cmd_buffer, &saved_state);

   for (uint32_t r = 0; r < rangeCount; r++) {
      for (uint32_t l = 0; l < pRanges[r].mipLevels; l++) {
         for (uint32_t s = 0; s < pRanges[r].arraySize; s++) {
            struct anv_color_attachment_view view;
            anv_color_attachment_view_init(&view, cmd_buffer->device,
               &(VkAttachmentViewCreateInfo) {
                  .sType = VK_STRUCTURE_TYPE_ATTACHMENT_VIEW_CREATE_INFO,
                  .image = _image,
                  .format = image->format->vk_format,
                  .mipLevel = pRanges[r].baseMipLevel + l,
                  .baseArraySlice = pRanges[r].baseArraySlice + s,
                  .arraySize = 1,
               },
               cmd_buffer);

            VkFramebuffer fb;
            anv_CreateFramebuffer(anv_device_to_handle(cmd_buffer->device),
               &(VkFramebufferCreateInfo) {
                  .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                  .attachmentCount = 1,
                  .pAttachments = (VkAttachmentBindInfo[]) {
                     {
                        .view = anv_attachment_view_to_handle(&view.base),
                        .layout = VK_IMAGE_LAYOUT_GENERAL
                     }
                  },
                  .width = view.base.extent.width,
                  .height = view.base.extent.height,
                  .layers = 1
               }, &fb);

            VkRenderPass pass;
            anv_CreateRenderPass(anv_device_to_handle(cmd_buffer->device),
               &(VkRenderPassCreateInfo) {
                  .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                  .attachmentCount = 1,
                  .pAttachments = &(VkAttachmentDescription) {
                     .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION,
                     .format = view.view.format->vk_format,
                     .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                     .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
                     .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
                  },
                  .subpassCount = 1,
                  .pSubpasses = &(VkSubpassDescription) {
                     .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION,
                     .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                     .inputCount = 0,
                     .colorCount = 1,
                     .colorAttachments = &(VkAttachmentReference) {
                        .attachment = 0,
                        .layout = VK_IMAGE_LAYOUT_GENERAL,
                     },
                     .resolveAttachments = NULL,
                     .depthStencilAttachment = (VkAttachmentReference) {
                        .attachment = VK_ATTACHMENT_UNUSED,
                        .layout = VK_IMAGE_LAYOUT_GENERAL,
                     },
                     .preserveCount = 1,
                     .preserveAttachments = &(VkAttachmentReference) {
                        .attachment = 0,
                        .layout = VK_IMAGE_LAYOUT_GENERAL,
                     },
                  },
                  .dependencyCount = 0,
               }, &pass);

            driver_layer->CmdBeginRenderPass(anv_cmd_buffer_to_handle(cmd_buffer),
               &(VkRenderPassBeginInfo) {
                  .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                  .renderArea = {
                     .offset = { 0, 0, },
                     .extent = {
                        .width = view.base.extent.width,
                        .height = view.base.extent.height,
                     },
                  },
                  .renderPass = pass,
                  .framebuffer = fb,
                  .attachmentCount = 1,
                  .pAttachmentClearValues = NULL,
               }, VK_RENDER_PASS_CONTENTS_INLINE);

            struct clear_instance_data instance_data = {
               .vue_header = {
                  .RTAIndex = 0,
                  .ViewportIndex = 0,
                  .PointWidth = 0.0
               },
               .color = *pColor,
            };

            meta_emit_clear(cmd_buffer, 1, &instance_data,
                            (VkClearDepthStencilValue) {0});

            driver_layer->CmdEndRenderPass(anv_cmd_buffer_to_handle(cmd_buffer));
         }
      }
   }

   /* Restore API state */
   anv_cmd_buffer_restore(cmd_buffer, &saved_state);
}

void anv_CmdClearDepthStencilImage(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     image,
    VkImageLayout                               imageLayout,
    float                                       depth,
    uint32_t                                    stencil,
    uint32_t                                    rangeCount,
    const VkImageSubresourceRange*              pRanges)
{
   stub();
}

void anv_CmdClearColorAttachment(
    VkCmdBuffer                                 cmdBuffer,
    uint32_t                                    colorAttachment,
    VkImageLayout                               imageLayout,
    const VkClearColorValue*                    pColor,
    uint32_t                                    rectCount,
    const VkRect3D*                             pRects)
{
   stub();
}

void anv_CmdClearDepthStencilAttachment(
    VkCmdBuffer                                 cmdBuffer,
    VkImageAspectFlags                          imageAspectMask,
    VkImageLayout                               imageLayout,
    float                                       depth,
    uint32_t                                    stencil,
    uint32_t                                    rectCount,
    const VkRect3D*                             pRects)
{
   stub();
}

void anv_CmdResolveImage(
    VkCmdBuffer                                 cmdBuffer,
    VkImage                                     srcImage,
    VkImageLayout                               srcImageLayout,
    VkImage                                     destImage,
    VkImageLayout                               destImageLayout,
    uint32_t                                    regionCount,
    const VkImageResolve*                       pRegions)
{
   stub();
}

void
anv_device_init_meta(struct anv_device *device)
{
   anv_device_init_meta_clear_state(device);
   anv_device_init_meta_blit_state(device);

   anv_CreateDynamicRasterState(anv_device_to_handle(device),
      &(VkDynamicRasterStateCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_DYNAMIC_RASTER_STATE_CREATE_INFO,
      },
      &device->meta_state.shared.rs_state);

   anv_CreateDynamicColorBlendState(anv_device_to_handle(device),
      &(VkDynamicColorBlendStateCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_DYNAMIC_COLOR_BLEND_STATE_CREATE_INFO
      },
      &device->meta_state.shared.cb_state);

   anv_CreateDynamicDepthStencilState(anv_device_to_handle(device),
      &(VkDynamicDepthStencilStateCreateInfo) {
         .sType = VK_STRUCTURE_TYPE_DYNAMIC_DEPTH_STENCIL_STATE_CREATE_INFO
      },
      &device->meta_state.shared.ds_state);
}

void
anv_device_finish_meta(struct anv_device *device)
{
   /* Clear */
   anv_DestroyPipeline(anv_device_to_handle(device),
                       device->meta_state.clear.pipeline);

   /* Blit */
   anv_DestroyPipeline(anv_device_to_handle(device),
                       device->meta_state.blit.pipeline);
   anv_DestroyPipelineLayout(anv_device_to_handle(device),
                             device->meta_state.blit.pipeline_layout);
   anv_DestroyDescriptorSetLayout(anv_device_to_handle(device),
                                  device->meta_state.blit.ds_layout);

   /* Shared */
   anv_DestroyDynamicRasterState(anv_device_to_handle(device),
                                 device->meta_state.shared.rs_state);
   anv_DestroyDynamicColorBlendState(anv_device_to_handle(device),
                                     device->meta_state.shared.cb_state);
   anv_DestroyDynamicDepthStencilState(anv_device_to_handle(device),
                                       device->meta_state.shared.ds_state);
}