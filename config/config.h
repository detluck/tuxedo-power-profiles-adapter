#pragma once

#define CONFIG_PATH "/etc/tcc-power-profiles/config.json"
#define CONFIG_FILE "config.json"

int config_init(void);
int config_reload(void);

const char *config_tcc_profile(const char *ppd_name);
const char *config_ppd_profile(const char *tcc_name);
