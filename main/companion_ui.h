#pragma once

#include "companion_model.h"

#ifdef __cplusplus
extern "C" {
#endif

void companion_ui_init(void);
void companion_ui_render(const companion_state_t *state);

#ifdef __cplusplus
}
#endif
