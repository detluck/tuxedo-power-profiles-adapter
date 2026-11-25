#pragma once

#define CONFIG_DIR "/etc/tcc-power-profiles"
#define CONFIG_FILE "config.json"
#define CONFIG_PATH CONFIG_DIR "/" CONFIG_FILE

int config_init(void);
int config_reload(void);

const char *config_tcc_profile(const char *ppd_name);
const char *config_ppd_profile(const char *tcc_name);
