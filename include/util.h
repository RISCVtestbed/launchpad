#ifndef LAUNCHPAD_UTIL_H_
#define LAUNCHPAD_UTIL_H_

#include <stdbool.h>
#include "launchpad_common.h"
#include "configuration.h"

struct current_device_status {
  bool initialised, running;
  bool * cores_active;
};

void transfer_executable_to_device(struct launchpad_configuration*, struct device_configuration*, struct device_drivers*);
int start_cores(struct launchpad_configuration*, struct device_configuration*, struct device_drivers*, struct current_device_status*);
void check_device_status(LP_STATUS_CODE);

#endif
