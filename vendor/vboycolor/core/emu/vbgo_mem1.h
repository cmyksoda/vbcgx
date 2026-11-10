#pragma once
/* The Wii U's MEM1, where a miss costs less than in MEM2 (as on the Wii): the frontend's, called before main by the
 * statics that live there. Zeroed, as statics are; MEM2 once MEM1 has no room. */
#include <stddef.h>
#ifdef __cplusplus
extern "C"
#endif
void *vbgo_mem1(size_t bytes);
