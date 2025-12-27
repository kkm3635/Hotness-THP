// pebs.h
#pragma once
#include <sys/types.h>   // pid_t

#ifdef __cplusplus
extern "C" {
#endif

void set_target_pid(pid_t pid);
void sampler_init(void);
void sampler_deinit(void);
void set_shared_arr(uint64_t *p);
extern int sampler_stop;   // main에서 직접 참조함

#ifdef __cplusplus
}
#endif

