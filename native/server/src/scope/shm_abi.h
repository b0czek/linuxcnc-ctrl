/*
 * LinuxCNC scope_rt shared-memory ABI, pinned to the repository's baseline
 * (LinuxCNC 2.10, src/hal/utils/scope_shm.h). This header is deliberately
 * kept on the native implementation side; transport-facing code sees only
 * the types in scope_controller.hpp. The scope controller contract test
 * checks it against upstream's header.
 */
#pragma once

#include <hal.h>
#include <rtapi.h>

#define LINUXCNC_SCOPE_SHM_KEY 0x130CF406
#define LINUXCNC_SCOPE_CHANNELS 16

enum linuxcnc_scope_state_t {
  LINUXCNC_SCOPE_IDLE = 0,
  LINUXCNC_SCOPE_INIT,
  LINUXCNC_SCOPE_PRE_TRIG,
  LINUXCNC_SCOPE_TRIG_WAIT,
  LINUXCNC_SCOPE_POST_TRIG,
  LINUXCNC_SCOPE_DONE,
  LINUXCNC_SCOPE_RESET,
};

/* One sample of one channel. scope_rt stores the member that matches the
 * channel's HAL type; the other bytes of the cell keep stale data. */
union linuxcnc_scope_data_t {
  rtapi_bool b;
  rtapi_uint u;
  rtapi_sint s;
  rtapi_real r;
};

struct linuxcnc_scope_shm_control_t {
  unsigned long shm_size;
  int buf_len;
  int watchdog;
  char thread_name[HAL_NAME_LEN + 1];
  int mult;
  int rec_len;
  int sample_len;
  int pre_trig;
  int trig_chan;
  linuxcnc_scope_data_t trig_level;
  int trig_edge;
  int force_trig;
  int auto_trig;
  int start;
  int curr;
  int samples;
  linuxcnc_scope_state_t state;
  /* Offset of the channel's value in HAL shared memory, as produced by
   * hal_reference_unmap() and consumed by scope_rt's hal_reference_map(). */
  rtapi_intptr_t data_offset[LINUXCNC_SCOPE_CHANNELS];
  hal_type_t data_type[LINUXCNC_SCOPE_CHANNELS];
  /* Non-zero when the channel is sampled. */
  char data_len[LINUXCNC_SCOPE_CHANNELS];
};

/* scope_rt places the sample buffer after the control block, rounded up to
 * a multiple of 8 bytes. */
#define LINUXCNC_SCOPE_BUFFER_OFFSET \
  ((sizeof(struct linuxcnc_scope_shm_control_t) + 7UL) & ~7UL)
