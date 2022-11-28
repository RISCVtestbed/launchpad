#ifndef UART_H_
#define UART_H_

#include "launchpad_common.h"
#include "configuration.h"
#include "util.h"

void interactive_uart(struct launchpad_configuration*, struct device_configuration*, struct device_drivers*, struct current_device_status*);

#endif
