#pragma once

#define CONFIG_PATH "/etc/tcc-power-profiles/config.json"

int config_init(void);
void config_reload(void);

const char *config_tcc_profile(const char *ppd_name);
