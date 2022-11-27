#ifndef CONFIGURATION_H_
#define CONFIGURATION_H_


#include <stdbool.h>
#include <string.h>

#define VERSION_IDENT "0.1"
#define MAX_NUM_CORES 128

struct launchpad_configuration {
  char * executable_filename;
  bool active_cores[MAX_NUM_CORES];
  bool all_cores_active, reset, display_config;
};

struct launchpad_configuration* readConfiguration(int, char*[]);

#endif
