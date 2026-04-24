#ifndef REC_TRIGGER_H
#define REC_TRIGGER_H

#include "rec_defs.h"
#include "vfr_ipc_types.h"

typedef void (*rec_trigger_cb_t)(void               *ctx,
                                 rec_trigger_type_t  type,
                                 uint64_t            timestamp_ns,
                                 float               confidence,
                                 const char         *label);

typedef struct rec_trigger rec_trigger_t;

rec_trigger_t *rec_trigger_create(const char       *stream_name,
                                  rec_trigger_cb_t  cb,
                                  void             *cb_ctx);

void rec_trigger_destroy(rec_trigger_t **trig);

int rec_trigger_get_fd(const rec_trigger_t *trig);

/*
 * Drain all pending connections from the abstract Unix socket and invoke the
 * callback for every well-formed event that matches this stream.
 */
int rec_trigger_handle_readable(rec_trigger_t *trig);

#endif /* REC_TRIGGER_H */
