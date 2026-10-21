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

#endif
