#pragma once
#include <stddef.h>

int tcc_client_init(void);
void tcc_client_cleanup(void);

int tcc_set_profile(const char *profile_id);

int tcc_get_profiles(char ***profile_ids, size_t *count);
int tcc_get_active_profile(char **profiles);

int log_error(int error, const char *message);
