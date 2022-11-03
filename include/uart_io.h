#ifndef UART_H_
#define UART_H_

#include "launchpad_common.h"
#include "configuration.h"

void poll_for_uart_across_cores(struct launchpad_configuration*, struct device_drivers);

#endif
