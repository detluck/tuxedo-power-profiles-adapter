#pragma once

#include <systemd/sd-bus.h>

int ppd_init(sd_bus *bus);
void ppd_reapply(void);
