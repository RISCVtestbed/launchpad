#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <ncurses.h>
#include <stdbool.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include "uart_interactive.h"
#include "launchpad_common.h"
#include "configuration.h"
#include "util.h"

#define MAX_BUFFER_SIZE 2048
#define OUT_PAUSED_BUFFER_SIZE 1048576

enum handle_command_status { COMMAND_SUCCESS, COMMAND_NOT_RECOGNISED, COMMAND_ERROR, COMMAND_NEW_SCREEN, COMMAND_IGNORE };

// Denotes whether we can update the screen or not (e.g. pause updates if in escape mode)
_Atomic bool screenUpdateOk, continuePoll, killBufferedOutput;

sem_t device_semaphore;

int main_screen_row, main_screen_col;

void * poll_uart_thread(void*);
static void write_uart_data(struct launchpad_configuration*, struct device_configuration*, struct device_drivers*, char);
static int get_number_active_cores(struct launchpad_configuration*, struct device_configuration*);
static void poll_core_for_uart(int core_id, struct device_drivers*, int, char**, unsigned int*, char *, unsigned int*);
static enum handle_command_status handle_command(struct launchpad_configuration*, struct device_configuration*, struct device_drivers*, struct current_device_status*, char*);
static void display_config(struct device_configuration*, struct device_drivers*);
static enum handle_command_status handle_disable_cores(struct launchpad_configuration*, struct device_configuration*, struct current_device_status*, char*);
static enum handle_command_status handle_enable_specify_executable(struct launchpad_configuration*, struct current_device_status*, char*);
static enum handle_command_status handle_enable_cores(struct launchpad_configuration*, struct device_configuration*, struct current_device_status*, char*, bool);
static int check_enabled_cores(struct launchpad_configuration*, struct device_configuration*);
static enum handle_command_status handle_start_cores(struct launchpad_configuration*, struct device_configuration*, struct device_drivers*, struct current_device_status*);
static void reset_device(struct device_drivers*, struct device_configuration*, struct current_device_status*);
static int get_num_active_cores(struct launchpad_configuration*, struct device_configuration*);
static enum handle_command_status handle_stop_cores(struct device_drivers*, struct device_configuration*, struct current_device_status*);
static void display_help_screen();
static void display_status_screen(struct launchpad_configuration*, struct device_configuration*, struct current_device_status*);
static void display_message(char*);
static bool check_command_portion(char*, char*);
static char * get_arg_portion(char*);
static void display_command_error_message(char*);

struct ThreadArgsStruct {
  struct launchpad_configuration * config;
  struct device_configuration * device_config;
  struct device_drivers * active_device_drivers;
};

void interactive_uart(struct launchpad_configuration * config, struct device_configuration * device_config,
      struct device_drivers * active_device_drivers, struct current_device_status * device_status) {
  struct ThreadArgsStruct * threadArgs=(struct ThreadArgsStruct*) malloc(sizeof(struct ThreadArgsStruct));
  threadArgs->config=config;
  threadArgs->device_config=device_config;
  threadArgs->active_device_drivers=active_device_drivers;

  sem_init(&device_semaphore, 0, 1);

  screenUpdateOk=true;
  continuePoll=true;
  killBufferedOutput=false;

  // Initialise ncurses
  initscr();
  //newterm(NULL, stderr, stdin);
  cbreak();
  noecho();
  timeout(1);
  scrollok(stdscr, true);
  use_default_colors();
  start_color();

  init_pair(1, COLOR_WHITE, COLOR_RED);
  init_pair(2, COLOR_BLUE, COLOR_GREEN);
  init_pair(3, COLOR_GREEN, -1);
  
  attron(COLOR_PAIR(3));
  if (!device_status->running) printw("Launchpad> Launchpad started but cores idle, use ':h' command for help\n");
  if (config->executable_filename == NULL) printw("Launchpad> No executable specified, provide one via the ':exe' command\n");
  if (get_num_active_cores(config, device_config) == 0) printw("Launchpad> No cores enabled, enable these via the ':e' or ':c' commands\n");
  attroff(COLOR_PAIR(3));

  pthread_t threadId;
  int err = pthread_create(&threadId, NULL, &poll_uart_thread, threadArgs);
  if (err) {
    fprintf(stderr, "Error calling device function\n");
    raise(SIGABRT);
    exit(-1);
  }

  char command_buffer[50];
  bool escapeMode=false;
  int x_pos;
  while(1==1) {
    char ch=getch();
    if (ch != ERR) {
      if (ch == 27 && !escapeMode) {
        screenUpdateOk=false;
        escapeMode=true;
        x_pos=0;
        getyx(stdscr, main_screen_row, main_screen_col);
        move(LINES-1, 0);
        deleteln();
        printw("> ");
        refresh();
      } else if (ch == 27) {
        // Ignore, just an extra escape
      } else if (escapeMode && ch == '\n') {
        escapeMode=false;
        enum handle_command_status command_status=COMMAND_IGNORE;
        if (x_pos > 0) {
          command_buffer[x_pos]='\0';
          command_status=handle_command(config, device_config, active_device_drivers, device_status, command_buffer);
          if (command_status == COMMAND_NOT_RECOGNISED) {
            display_command_error_message("Command not recognised");
          } else if (command_status == COMMAND_SUCCESS) {
            deleteln();
          }
        }
        if (command_status == COMMAND_NEW_SCREEN) {
          move(0,0);
        } else {
          move(main_screen_row, main_screen_col);
        }
        screenUpdateOk=true;
      } else if (escapeMode && (ch == KEY_BACKSPACE || ch == KEY_DC || ch == 127)) {
        // Handle backspace for escape mode command
        int my_row, my_col;
        getyx(stdscr, my_row, my_col);
        if (my_col > 2) {
          mvprintw(my_row, my_col-1, " ");
          move(my_row, my_col-1);
          x_pos--;
        }
      } else {
        printw("%c", ch);
        refresh();
        if (escapeMode) {
          command_buffer[x_pos]=ch;
          x_pos++;
        } else {
          // If not in escape mode then write the character to UART
          write_uart_data(config, device_config, active_device_drivers, ch);
        }
      }
    }
  }
  pthread_join(threadId, NULL);
}

void * poll_uart_thread(void * args) {
  struct ThreadArgsStruct * threadArgs = (struct ThreadArgsStruct*) args;

  int num_active_cores=get_number_active_cores(threadArgs->config, threadArgs->device_config);
  char ** output_buffers=NULL;
  unsigned int * output_buffer_locals=NULL;
  if (num_active_cores > 1) {
    // Only buffer and output if number cores is greater than one, otherwise just dumb display
    output_buffers=(char**) malloc(sizeof(char*) * threadArgs->device_config->number_cores);
    output_buffer_locals=(unsigned int*) malloc(sizeof(unsigned int) * threadArgs->device_config->number_cores);
    for (int i=0;i<threadArgs->device_config->number_cores;i++) {
      if (threadArgs->config->active_cores[i]) {
        output_buffers[i]=(char*) malloc(sizeof(char) * MAX_BUFFER_SIZE);
        output_buffer_locals[i]=0;
      }
    }
  }

  char * out_paused_buffer=(char*) malloc(sizeof(char*) * OUT_PAUSED_BUFFER_SIZE);
  unsigned int out_paused_buffer_idx=0;
  while (1==1) {
    for (int i=0;i<threadArgs->device_config->number_cores;i++) {
      if (threadArgs->config->active_cores[i]) {
        poll_core_for_uart(i, threadArgs->active_device_drivers, num_active_cores, output_buffers, output_buffer_locals, out_paused_buffer, &out_paused_buffer_idx);
      }
    }
  }
  return NULL;
}

static void write_uart_data(struct launchpad_configuration * config, struct device_configuration * device_config, struct device_drivers * active_device_drivers, char data) {
  for (int i=0;i<device_config->number_cores;i++) {
    if (config->active_cores[i]) {
      active_device_drivers->device_write_uart(i, data);
    }
  }
}

static int get_number_active_cores(struct launchpad_configuration * config, struct device_configuration * device_config) {
  int num_active_cores=0;
  for (int i=0;i<device_config->number_cores;i++) {
    if (config->active_cores[i]) num_active_cores++;
  }
  return num_active_cores;
}

static void poll_core_for_uart(int core_id, struct device_drivers * active_device_drivers, int num_active_cores, char ** output_buffers, unsigned int * output_buffer_locals,
      char * out_paused_buffer, unsigned int * out_paused_buffer_idx) {
  if (*out_paused_buffer_idx > 0 && screenUpdateOk) {
    out_paused_buffer[*out_paused_buffer_idx]='\0';
    if (!killBufferedOutput) {
      printw("%s", out_paused_buffer);
      refresh();
    }
    *out_paused_buffer_idx=0;
  }
  if (!continuePoll) return;
  int uart_data_present=0;
  sem_wait(&device_semaphore);
  check_device_status(active_device_drivers->device_uart_has_data(core_id, &uart_data_present));
  if (uart_data_present) {
    char data=0x0;
    check_device_status(active_device_drivers->device_read_uart(core_id, &data));
    sem_post(&device_semaphore);
    if (num_active_cores > 1) {
      if (output_buffer_locals[core_id] < MAX_BUFFER_SIZE) {
        if (data != '\r') {
          output_buffers[core_id][output_buffer_locals[core_id]]=data;
          output_buffer_locals[core_id]++;
        }
        if (data == '\n') {
          // This is a flush
          if (screenUpdateOk) {
            output_buffers[core_id][output_buffer_locals[core_id]]='\0';
            printw("[%d]: %s", core_id, output_buffers[core_id]);
            refresh();
          } else {
            memcpy(&out_paused_buffer[*out_paused_buffer_idx], output_buffers[core_id], output_buffer_locals[core_id]);
            (*out_paused_buffer_idx)+=output_buffer_locals[core_id];
          }
          output_buffers[core_id][0]='\0';
          output_buffer_locals[core_id]=0;
        }
      } else {
        printf("Warning, UART buffer length of %d bytes exceeded for core %d, ignoring data '%c'\n", MAX_BUFFER_SIZE, core_id, data);
      }
    } else {
      if (data != '\r') {
        if (screenUpdateOk) {
          printw("%c", data);
          refresh();
        } else {
          out_paused_buffer[*out_paused_buffer_idx]=data;
          (*out_paused_buffer_idx)++;
        }
      }
    }
  } else {
    sem_post(&device_semaphore);
  }
}

static enum handle_command_status handle_command(struct launchpad_configuration * config, struct device_configuration * device_config,
        struct device_drivers * active_device_drivers, struct current_device_status * device_status, char * buffer) {
  if (strcmp(buffer, ":q")==0 || strcmp(buffer, ":quit")==0) {
    endwin();
    exit(0);
  } else if (strcmp(buffer, ":clear")==0) {
    clear();
    refresh();
    return COMMAND_NEW_SCREEN;
  } else if (strcmp(buffer, ":h")==0 || strcmp(buffer, ":help")==0) {
    display_help_screen();
    return COMMAND_SUCCESS;
  } else if (strcmp(buffer, ":status")==0) {
    display_status_screen(config, device_config, device_status);
    return COMMAND_SUCCESS;
  } else if (strcmp(buffer, ":config")==0) {
    display_config(device_config, active_device_drivers);
    return COMMAND_SUCCESS;
  } else if (strcmp(buffer, ":reset")==0) {
    reset_device(active_device_drivers, device_config, device_status);
    return COMMAND_SUCCESS;
  } else if (strcmp(buffer, ":stop")==0) {
    return handle_stop_cores(active_device_drivers, device_config, device_status);
  } else if (strcmp(buffer, ":start")==0) {
    return handle_start_cores(config, device_config, active_device_drivers, device_status);
  } else if (check_command_portion(buffer, ":e") || check_command_portion(buffer, ":enable")) {
    return handle_enable_cores(config, device_config, device_status, buffer, true);
  } else if (check_command_portion(buffer, ":c") || check_command_portion(buffer, ":cores")) {
    return handle_enable_cores(config, device_config, device_status, buffer, false);
  } else if (check_command_portion(buffer, ":d") || check_command_portion(buffer, ":disable")) {
    return handle_disable_cores(config, device_config, device_status, buffer);
  } else if (check_command_portion(buffer, ":bin") || check_command_portion(buffer, ":exe")) {
    return handle_enable_specify_executable(config, device_status, buffer);
  }
  return COMMAND_NOT_RECOGNISED;
}

static void display_config(struct device_configuration * device_config, struct device_drivers * active_device_drivers) {
  char * config_str=(char*) malloc(sizeof(char) * CONFIGURATION_STR_SIZE);
  generate_device_configuration(device_config, active_device_drivers, config_str);
  int row, col;
  getyx(stdscr, row, col);
  move(main_screen_row+(main_screen_col == 0 ? 0 : 1), 0);
  printw("%s", config_str);
  free(config_str);
  refresh();
  getyx(stdscr, main_screen_row, main_screen_col);
  main_screen_col=0;
  move(row, col);
}

static enum handle_command_status handle_enable_specify_executable(struct launchpad_configuration * config, struct current_device_status * device_status, char * buffer) {
  if (device_status->running) {
    display_command_error_message("Can only change executable in a stopped state, stop running cores first");
    return COMMAND_ERROR;
  }
  char * args=get_arg_portion(buffer);
  if (args == NULL) {
    display_command_error_message("Must provide arguments with enable or core command");
    return COMMAND_ERROR;
  } else {
    if (access(args, F_OK) == 0) {
      if (config->executable_filename != NULL) free(config->executable_filename);
      config->executable_filename=(char*) malloc(sizeof(char) * strlen(args)+1);
      strcpy(config->executable_filename, args);
      char message[250];
      sprintf(message, "Successfully changed executable to '%s'", config->executable_filename);
      display_message(message);
      return COMMAND_SUCCESS;
    } else {
      display_command_error_message("Specified file does not exist");
      return COMMAND_ERROR;
    }
  }
}

static enum handle_command_status handle_disable_cores(struct launchpad_configuration * config, struct device_configuration * device_config,
      struct current_device_status * device_status, char * buffer) {
  if (device_status->running) {
    display_command_error_message("Can only change active cores in a stopped state, stop running cores first");
    return COMMAND_ERROR;
  }
  char * args=get_arg_portion(buffer);
  if (args == NULL) {
    display_command_error_message("Must provide arguments with enable or core command");
    return COMMAND_ERROR;
  } else {
    bool disabledCores[device_config->number_cores];
    parseCoreInfoString(args, disabledCores, device_config->number_cores);
    bool hasDisabled=false;
    for (int i=0;i<device_config->number_cores;i++) {
      if (disabledCores[i]) {
        if (config->active_cores[i]) hasDisabled=true;
        config->active_cores[i]=false;
      }
    }
    int num_active=check_enabled_cores(config, device_config);
    char message[100];
    if (hasDisabled) {
      sprintf(message, "Core(s) disabled, there are now %d cores enabled", num_active);
    } else {
      sprintf(message, "No cores in disable list were enabled, there are still %d cores enabled", num_active);
    }
    display_message(message);
    return COMMAND_SUCCESS;
  }
}

static enum handle_command_status handle_enable_cores(struct launchpad_configuration * config, struct device_configuration * device_config,
      struct current_device_status * device_status, char * buffer, bool additive) {
  if (device_status->running) {
    display_command_error_message("Can only change active cores in a stopped state, stop running cores first");
    return COMMAND_ERROR;
  }
  char * args=get_arg_portion(buffer);
  if (args == NULL) {
    display_command_error_message("Must provide arguments with enable or core command");
    return COMMAND_ERROR;
  } else {
    bool activeCorePrev[device_config->number_cores];
    if (additive) {
      memcpy(activeCorePrev, config->active_cores, sizeof(bool) * device_config->number_cores);
    }
    parseCoreActiveInfo(config, args);
    if (additive) {
      // Applies AND relation here as we are enabling in additive fashion
      for (int i=0;i<device_config->number_cores;i++) {
        if (activeCorePrev[i]) config->active_cores[i]=true;
      }
    }
    int num_active=check_enabled_cores(config, device_config);
    char message[50];
    sprintf(message, "There are now %d cores enabled", num_active);
    display_message(message);
    return COMMAND_SUCCESS;
  }
}

static int check_enabled_cores(struct launchpad_configuration * config, struct device_configuration * device_config) {
  for (int i=device_config->number_cores;i<MAX_NUM_CORES;i++) {
    if (config->active_cores[i]) {
      char message[100];
      sprintf(message, "Core %d enabled but the device only has %d cores, will be ignored", i, device_config->number_cores);
      display_message(message);
    }
  }

  return get_num_active_cores(config, device_config);
}

static int get_num_active_cores(struct launchpad_configuration * config, struct device_configuration * device_config) {
  int num_active_cores=0;
  for (int i=0;i<device_config->number_cores;i++) {
    if (config->active_cores[i]) {
      num_active_cores++;
    }
  }
  return num_active_cores;
}

static enum handle_command_status handle_start_cores(struct launchpad_configuration * config, struct device_configuration * device_config,
      struct device_drivers * active_device_drivers, struct current_device_status * device_status) {
  if (device_status->running) {
    display_command_error_message("Cores are already running");
    return COMMAND_ERROR;
  }
  int num_enabled_cores=get_num_active_cores(config, device_config);
  if (num_enabled_cores == 0) {
    display_command_error_message("No cores are enabled, enable at-least one before starting");
    return COMMAND_ERROR;
  }
  if (config->executable_filename == NULL) {
    display_command_error_message("No executable file has been specified, you must provide this to start the cores");
    return COMMAND_ERROR;
  }
  killBufferedOutput=false;
  sem_wait(&device_semaphore);
  transfer_executable_to_device(config, device_config, active_device_drivers);
  int num_started=start_cores(config, device_config, active_device_drivers, device_status);
  continuePoll=true;
  sem_post(&device_semaphore);
  char message[25];
  sprintf(message, "%d cores started", num_started);
  display_message(message);
  return COMMAND_SUCCESS;
}

static enum handle_command_status handle_stop_cores(struct device_drivers * active_device_drivers, struct device_configuration * device_config, struct current_device_status * device_status) {
    if (!device_status->running) {
    display_command_error_message("Cores are already stopped");
    return COMMAND_ERROR;
  }
  sem_wait(&device_semaphore);
  check_device_status(active_device_drivers->device_stop_allcores());
  continuePoll=false;
  sem_post(&device_semaphore);
  for (int i=0;i<device_config->number_cores;i++) device_status->cores_active[i]=false;
  device_status->running=false;
  display_message("All cores stopped and idle");
  killBufferedOutput=true;
  return COMMAND_SUCCESS;
}

static void display_command_error_message(char * error_message) {
  attron(COLOR_PAIR(1));
  mvprintw(LINES-1,0, "Error: %s", error_message);
  attroff(COLOR_PAIR(1));
  refresh();
}

static void reset_device(struct device_drivers * active_device_drivers, struct device_configuration * device_config, struct current_device_status * device_status) {
  attron(COLOR_PAIR(2));
  mvprintw(LINES-1,0, "Please wait - resetting soft cores");
  attroff(COLOR_PAIR(2));
  refresh();
  sem_wait(&device_semaphore);
  check_device_status(active_device_drivers->device_reset());
  continuePoll=false;
  // Reinitialise the drivers as the user will probably want to do more interaction
  check_device_status(active_device_drivers->device_initialise());
  sem_post(&device_semaphore);
  for (int i=0;i<device_config->number_cores;i++) device_status->cores_active[i]=false;
  display_message("Reset successful, cores all idle");
}

static void display_message(char * message) {
  int row, col;
  getyx(stdscr, row, col);
  move(main_screen_row+(main_screen_col == 0 ? 0 : 1), 0);
  attron(COLOR_PAIR(3));
  printw("Launchpad> %s\n", message);
  attroff(COLOR_PAIR(3));
  refresh();
  getyx(stdscr, main_screen_row, main_screen_col);
  main_screen_col=0;
  move(row, col);
}

static void display_help_screen() {
  int row, col;
  getyx(stdscr, row, col);
  move(main_screen_row+1, 0);
  printw("Launchpad Interactive Help\n");
  printw("--------------------------\n");
  printw("Escape key to enter command mode, the following commands apply:\n");
  printw(":status      - Display current soft core status including active and enabled cores\n");
  printw(":config      - Display soft core CPU and board configuration and status\n");
  printw(":clear       - Clears the output screen\n");
  printw(":stop        - Stop all cores\n");
  printw(":start       - Start all enabled cores\n");
  printw(":exe, :bin   - Specify the binary executable that cores should run\n");
  printw(":e, :enable  - Enables core(s) provided as a singleton, list or range (does not start)\n");
  printw(":c, :cores   - Sets core(s) provided as a singleton, list or range as the active set (does not start)\n");
  printw(":d, :disable - Disables core(s) provided as a singleton, list or range (does not stop)\n");
  printw(":reset       - Reset device and stop all cores\n");
  printw(":h, :help    - Display this help message\n");
  printw(":q, :quit    - Quit Launchpad\n");
  printw("\nEnter (empty command) quits command mode without a command\n");
  refresh();
  getyx(stdscr, main_screen_row, main_screen_col);
  main_screen_col=0;
  move(row, col);
}

static void display_status_screen(struct launchpad_configuration * config, struct device_configuration * device_config, struct current_device_status * device_status) {
  int row, col;
  getyx(stdscr, row, col);
  move(main_screen_row+1, 0);
  printw("Launchpad Current Status\n");
  printw("------------------------\n");
  if (device_status->running) {
    printw("Soft cores currently running");
  } else {
    printw("Soft cores currently stopped");
  }
  for (int i=0;i<device_config->number_cores;i++) {
    printw("Core %d: %s (%s)\n", i, device_status->cores_active[i] ? "active" : "inactive", config->active_cores[i] ? "enabled" : "disabled");
  }
  printw("Executable: %s\n", config->executable_filename);
  refresh();
  getyx(stdscr, main_screen_row, main_screen_col);
  main_screen_col=0;
  move(row, col);
}

static bool check_command_portion(char * buffer, char * command) {
  if (strncmp(buffer, command, strlen(command)) != 0) return false;
  // This ensures there is whitespace next, i.e. this is the command portion and not just part of the command
  return buffer[strlen(command)] == ' ';
}

static char * get_arg_portion(char * buffer) {
  char * space=strchr(buffer, ' ');
  if (space == NULL) return NULL;
  return space+1;
}
