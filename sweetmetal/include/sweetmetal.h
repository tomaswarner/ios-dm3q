/* sweetmetal: C API over metal2vulkan-runtime (see src/lib.rs). Objects are opaque; free each
 * with its _release. Failures return NULL/false; the reason is in sm_last_error(). */
#ifndef SWEETMETAL_H
#define SWEETMETAL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct sm_device sm_device;
typedef struct sm_buffer sm_buffer;
typedef struct sm_queue sm_queue;
typedef struct sm_command_buffer sm_command_buffer;
typedef struct sm_library sm_library;
typedef struct sm_function sm_function;
typedef struct sm_compute_pipeline sm_compute_pipeline;
typedef struct sm_compute_encoder sm_compute_encoder;

const char *sm_last_error(void);

sm_device *sm_device_create(void);
void sm_device_name(const sm_device *dev, char *buf, size_t len);
void sm_device_release(sm_device *dev);

sm_buffer *sm_buffer_new(const sm_device *dev, uint64_t length);
uint8_t *sm_buffer_contents(const sm_buffer *buf);
uint64_t sm_buffer_length(const sm_buffer *buf);
void sm_buffer_release(sm_buffer *buf);

sm_library *sm_library_new_with_data(const sm_device *dev, const uint8_t *data, size_t len);
sm_library *sm_library_new_with_air(const sm_device *dev, const uint8_t *data, size_t len);
sm_function *sm_library_function(const sm_library *lib, const char *name);
void sm_library_release(sm_library *lib);
void sm_function_release(sm_function *f);

sm_compute_pipeline *sm_compute_pipeline_new(const sm_device *dev, const sm_function *f);
void sm_compute_pipeline_release(sm_compute_pipeline *p);

sm_queue *sm_queue_new(const sm_device *dev);
void sm_queue_release(sm_queue *q);
sm_command_buffer *sm_command_buffer_new(const sm_queue *q);
bool sm_command_buffer_commit(const sm_command_buffer *cb);
bool sm_command_buffer_wait(const sm_command_buffer *cb);
void sm_command_buffer_release(sm_command_buffer *cb);

sm_compute_encoder *sm_compute_encoder_new(const sm_command_buffer *cb);
bool sm_compute_set_pipeline(sm_compute_encoder *enc, const sm_compute_pipeline *p);
bool sm_compute_set_buffer(sm_compute_encoder *enc, const sm_buffer *buf, uint64_t offset, uint32_t index);
bool sm_compute_dispatch_threads(sm_compute_encoder *enc, uint64_t tx, uint64_t ty, uint64_t tz,
                                 uint64_t gx, uint64_t gy, uint64_t gz);
bool sm_compute_end_encoding(sm_compute_encoder *enc); /* frees enc */

/* ---- textures, samplers, render, blit (src/graphics.rs). Metal enums are raw values. ---- */
typedef struct sm_texture sm_texture;
typedef struct sm_sampler sm_sampler;
typedef struct sm_render_pipeline sm_render_pipeline;
typedef struct sm_render_encoder sm_render_encoder;
typedef struct sm_blit_encoder sm_blit_encoder;

typedef struct sm_texture_desc {
	uint32_t texture_type;      /* MTLTextureType */
	uint32_t pixel_format;      /* MTLPixelFormat */
	uint64_t width, height, depth;
	uint64_t mipmap_level_count, sample_count, array_length;
	uint32_t usage;             /* MTLTextureUsage */
	uint64_t resource_options;  /* MTLResourceOptions */
} sm_texture_desc;

sm_texture *sm_texture_new(const sm_device *dev, const sm_texture_desc *d);
uint64_t sm_texture_width(const sm_texture *t);
uint64_t sm_texture_height(const sm_texture *t);
bool sm_texture_replace_region(const sm_texture *t, uint64_t x, uint64_t y, uint64_t w, uint64_t h,
			       uint64_t level, uint64_t slice, const uint8_t *bytes, size_t len,
			       uint64_t bytes_per_row, uint64_t bytes_per_image);
bool sm_texture_get_bytes(const sm_texture *t, uint8_t *bytes, size_t len, uint64_t bytes_per_row,
			  uint64_t bytes_per_image, uint64_t x, uint64_t y, uint64_t w, uint64_t h,
			  uint64_t level, uint64_t slice);
void sm_texture_release(sm_texture *t);

typedef struct sm_sampler_desc {
	uint32_t min_filter, mag_filter, mip_filter;           /* MTLSamplerMinMagFilter / MipFilter */
	uint32_t s_address_mode, t_address_mode, r_address_mode; /* MTLSamplerAddressMode */
	bool normalized_coordinates;
} sm_sampler_desc;

sm_sampler *sm_sampler_new(const sm_device *dev, const sm_sampler_desc *d);
void sm_sampler_release(sm_sampler *s);

typedef struct sm_color_attachment {
	uint32_t pixel_format;  /* 0 = unused */
	bool blending_enabled;
	uint32_t source_rgb_blend_factor, destination_rgb_blend_factor, rgb_blend_operation;
	uint32_t source_alpha_blend_factor, destination_alpha_blend_factor, alpha_blend_operation;
	uint32_t write_mask;    /* MTLColorWriteMask */
} sm_color_attachment;

typedef struct sm_vertex_attribute {
	uint32_t format;        /* MTLVertexFormat; 0 = unset */
	uint32_t offset, buffer_index;
} sm_vertex_attribute;

typedef struct sm_vertex_layout {
	uint32_t stride;        /* 0 = unset, UINT32_MAX = MTLBufferLayoutStrideDynamic */
	uint32_t step_function; /* MTLVertexStepFunction (PerVertex = 1) */
	uint32_t step_rate;
} sm_vertex_layout;

typedef struct sm_vertex_descriptor {
	sm_vertex_attribute attributes[31];
	sm_vertex_layout layouts[31];
} sm_vertex_descriptor;

typedef struct sm_render_pipeline_desc {
	const sm_function *vertex;
	const sm_function *fragment;   /* may be NULL */
	sm_color_attachment color[8];
	uint32_t depth_format, stencil_format, raster_sample_count;  /* 0 = none / default */
	const sm_vertex_descriptor *vertex_descriptor;               /* NULL = nil */
} sm_render_pipeline_desc;

sm_render_pipeline *sm_render_pipeline_new(const sm_device *dev, const sm_render_pipeline_desc *d);
void sm_render_pipeline_release(sm_render_pipeline *p);

typedef struct sm_pass_color {
	const sm_texture *texture;     /* NULL = unused */
	uint64_t level, slice;
	uint32_t load_action, store_action;  /* MTLLoadAction / MTLStoreAction */
	double clear_color[4];
} sm_pass_color;

typedef struct sm_pass_depth_stencil {
	const sm_texture *texture;     /* NULL = unused */
	uint64_t level, slice;
	uint32_t load_action, store_action;
	double clear_depth;            /* depth attachment only */
	uint32_t clear_stencil;        /* stencil attachment only */
} sm_pass_depth_stencil;

typedef struct sm_render_pass_desc {
	sm_pass_color color[8];
	sm_pass_depth_stencil depth, stencil;
} sm_render_pass_desc;

typedef struct sm_depth_stencil sm_depth_stencil;

typedef struct sm_stencil_desc {
	uint32_t compare_function;     /* MTLCompareFunction */
	uint32_t stencil_failure_operation, depth_failure_operation, depth_stencil_pass_operation; /* MTLStencilOperation */
	uint32_t read_mask, write_mask;
} sm_stencil_desc;

typedef struct sm_depth_stencil_desc {
	uint32_t depth_compare_function;
	bool depth_write_enabled;
	sm_stencil_desc front, back;
} sm_depth_stencil_desc;

sm_depth_stencil *sm_depth_stencil_new(const sm_device *dev, const sm_depth_stencil_desc *d);
void sm_depth_stencil_release(sm_depth_stencil *s);

sm_render_encoder *sm_render_encoder_new(const sm_command_buffer *cb, const sm_render_pass_desc *d);
bool sm_render_set_pipeline(sm_render_encoder *e, const sm_render_pipeline *p);
bool sm_render_set_vertex_buffer(sm_render_encoder *e, const sm_buffer *b, uint64_t offset, uint32_t index);
bool sm_render_set_vertex_bytes(sm_render_encoder *e, const void *bytes, size_t len, uint32_t index);
bool sm_render_set_vertex_texture(sm_render_encoder *e, const sm_texture *t, uint32_t index);
bool sm_render_set_fragment_buffer(sm_render_encoder *e, const sm_buffer *b, uint64_t offset, uint32_t index);
bool sm_render_set_fragment_bytes(sm_render_encoder *e, const void *bytes, size_t len, uint32_t index);
bool sm_render_set_fragment_texture(sm_render_encoder *e, const sm_texture *t, uint32_t index);
bool sm_render_set_fragment_sampler(sm_render_encoder *e, const sm_sampler *s, uint32_t index);
bool sm_render_set_viewport(sm_render_encoder *e, double x, double y, double w, double h, double znear, double zfar);
bool sm_render_set_scissor(sm_render_encoder *e, uint64_t x, uint64_t y, uint64_t w, uint64_t h);
bool sm_render_set_depth_stencil(sm_render_encoder *e, const sm_depth_stencil *s); /* NULL = nil */
bool sm_render_set_stencil_reference(sm_render_encoder *e, uint32_t front, uint32_t back);
bool sm_render_set_cull_mode(sm_render_encoder *e, uint32_t mode);          /* MTLCullMode */
bool sm_render_set_front_facing_winding(sm_render_encoder *e, uint32_t w);  /* MTLWinding */
bool sm_render_set_depth_bias(sm_render_encoder *e, float bias, float slope_scale, float clamp);
bool sm_render_set_depth_clip_mode(sm_render_encoder *e, uint32_t mode);    /* MTLDepthClipMode */
bool sm_render_set_triangle_fill_mode(sm_render_encoder *e, uint32_t mode); /* MTLTriangleFillMode */
bool sm_render_set_blend_color(sm_render_encoder *e, float r, float g, float b, float a);
bool sm_render_draw(sm_render_encoder *e, uint32_t primitive, uint32_t start, uint32_t count,
		    uint32_t instances, uint32_t base_instance);
bool sm_render_draw_indexed(sm_render_encoder *e, uint32_t primitive, uint32_t index_count, uint32_t index_type,
			    const sm_buffer *index_buffer, uint64_t index_offset, uint32_t instances,
			    int32_t base_vertex, uint32_t base_instance);
bool sm_render_end_encoding(sm_render_encoder *e); /* frees e */

bool sm_compute_set_texture(sm_compute_encoder *e, const sm_texture *t, uint32_t index);
bool sm_compute_set_bytes(sm_compute_encoder *e, const void *bytes, size_t len, uint32_t index);

sm_blit_encoder *sm_blit_encoder_new(const sm_command_buffer *cb);
bool sm_blit_copy_buffer(sm_blit_encoder *e, const sm_buffer *src, uint64_t src_offset,
			 const sm_buffer *dst, uint64_t dst_offset, uint64_t size);
bool sm_blit_fill_buffer(sm_blit_encoder *e, const sm_buffer *b, uint64_t start, uint64_t len, uint8_t value);
bool sm_blit_buffer_to_texture(sm_blit_encoder *e, const sm_buffer *src, uint64_t offset, uint64_t bytes_per_row,
			       uint64_t bytes_per_image, uint64_t w, uint64_t h, const sm_texture *dst,
			       uint64_t slice, uint64_t level, uint64_t x, uint64_t y);
bool sm_blit_texture_to_buffer(sm_blit_encoder *e, const sm_texture *src, uint64_t slice, uint64_t level,
			       uint64_t x, uint64_t y, uint64_t w, uint64_t h, const sm_buffer *dst,
			       uint64_t offset, uint64_t bytes_per_row, uint64_t bytes_per_image);
bool sm_blit_texture_to_texture(sm_blit_encoder *e, const sm_texture *src, uint64_t src_slice, uint64_t src_level,
				uint64_t sx, uint64_t sy, uint64_t w, uint64_t h, const sm_texture *dst,
				uint64_t dst_slice, uint64_t dst_level, uint64_t dx, uint64_t dy);
bool sm_blit_generate_mipmaps(sm_blit_encoder *e, const sm_texture *t);
bool sm_blit_end_encoding(sm_blit_encoder *e); /* frees e */

#endif
