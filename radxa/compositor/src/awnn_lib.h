/*
 * awnn_lib.h — Public API for awnn VIPLite wrapper
 *
 * Vendored from ZIFENG278/ai-sdk (examples/libawnn_viplite/)
 */

#ifndef __AWNN_LIB_H__
#define __AWNN_LIB_H__
#ifdef __cplusplus
       extern "C" {
#endif

typedef struct Awnn_Context Awnn_Context_t;

void awnn_init(void);
void awnn_uninit(void);
Awnn_Context_t *awnn_create(const char *nbg);
void awnn_destroy(Awnn_Context_t *context);
void awnn_set_input_buffers(Awnn_Context_t *context, void **input_buffers);
float **awnn_get_output_buffers(Awnn_Context_t *context);
void *awnn_get_output_buffer(Awnn_Context_t *info, int i);
void awnn_run(Awnn_Context_t *context);
void awnn_dump_io(Awnn_Context_t *context, const char *path);

#ifdef __cplusplus
       }
#endif

#endif
