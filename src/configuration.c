#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include "configuration.h"

static void parseCommandLineArguments(struct launchpad_configuration*, int, char**);
static void parseCoreActiveInfo(struct launchpad_configuration*, char*);
static int areStringsEqualIgnoreCase(char*, char*);
static void displayHelp(void);

/**
 * Given the command line arguments this will read the configuration and return the configuration structure
 * which has the appropriate flags set and contains strings etc
 */
struct launchpad_configuration* readConfiguration(int argc, char *argv[]) {
  int i;
  struct launchpad_configuration* configuration=(struct launchpad_configuration*) malloc(sizeof(struct launchpad_configuration));  
  configuration->executable_filename=NULL;
  configuration->poll_uart=false;
  configuration->reset=false;
  configuration->all_cores_active=false;
  configuration->total_cores_on_device=-1;
  for (int i=0;i<MAX_NUM_CORES;i++) configuration->active_cores[i]=false;  
  parseCommandLineArguments(configuration, argc, argv);
  return configuration;
}

/**
 * Parses command line arguments
 */
static void parseCommandLineArguments(struct launchpad_configuration* configuration, int argc, char *argv[]) {
  if (argc == 1) {
    displayHelp();
    exit(0);
  } else { 
    int i;
    for (i=1;i<argc;i++) {
      if (areStringsEqualIgnoreCase(argv[i], "-bin") || areStringsEqualIgnoreCase(argv[i], "-exe")) {
        configuration->executable_filename=argv[++i];      
      } else if (areStringsEqualIgnoreCase(argv[i], "-help")) {
        displayHelp();
        exit(0);      
      } else if (areStringsEqualIgnoreCase(argv[i], "-uart")) {
        configuration->poll_uart=true;
      } else if (areStringsEqualIgnoreCase(argv[i], "-reset")) {
        configuration->reset=true;
      } else if (areStringsEqualIgnoreCase(argv[i], "-c")) {
        if (i+1 ==argc) {
          fprintf(stderr, "When specifying active cores you must provide arguments\n");
          exit(0);
        } else {          
          parseCoreActiveInfo(configuration, argv[++i]);
        }
      }
    }
    if (configuration->executable_filename == NULL) {
      fprintf(stderr, "You must supply an executable file to run as an argument, see -h for details\n");
      exit(0);
    }
    if (!configuration->all_cores_active) {
      bool none_active=true;
      for (int i=0;i<MAX_NUM_CORES;i++) {
        if (configuration->active_cores[i]) {
          none_active=false;
          break;
        }
      }
      if (none_active) {
        fprintf(stderr, "No cores configured to be active, you must specify at-least one core with the -c argument\n");
        exit(0);
      }
    }
  }
}

/**
 * Determines the active cores if the user supplied -c n, can be a single integer, a list, a range or
 * all to select all cores
 */
static void parseCoreActiveInfo(struct launchpad_configuration* configuration, char * info) {
  int i;
  if (areStringsEqualIgnoreCase(info, "all")) {    
    configuration->all_cores_active=true;
  } else {
    if (strchr(info, ',') != NULL) {
      char vn[5];
      int s;
      for (i=0;i<MAX_NUM_CORES;i++) configuration->active_cores[i]=0;
      while (strchr(info, ',') != NULL) {
        s=strchr(info, ',')-info;
        memcpy(vn, info, s);
        vn[s]='\0';
        configuration->active_cores[atoi(vn)]=1;
        info=strchr(info, ',')+1;
      }
      configuration->active_cores[atoi(info)]=1;
    } else if (strchr(info, ':') != NULL) {
      char vn[5];
      int s;
      s=strchr(info, ':')-info;
      memcpy(vn, info, s);
      vn[s]='\0';
      int from=atoi(vn);
      int to=atoi(strchr(info, ':')+1);
      for (i=0;i<MAX_NUM_CORES;i++) {
        if (i >= from && i<= to) {
          configuration->active_cores[i]=1;
        } else {
          configuration->active_cores[i]=0;
        }
      }
    } else {
      for (i=0;i<MAX_NUM_CORES;i++) configuration->active_cores[i]=0;
      configuration->active_cores[atoi(info)]=1;
    }
  }
}

/**
 * Displays the help message with usage information
 */
static void displayHelp() {
  printf("Launchpad version %s\n", VERSION_IDENT);
  printf("launchpad [arguments]\n\nArguments\n--------\n");
  printf("-bin/-exe arg  Provides the binary executable file to be loaded and executed\n");
  printf("-c list        Specify active cores; can be a single id, all, a range (a:b) or a list (a,b,c,d)\n");  
  printf("-uart          Poll UART for output after starting code on core(s)\n");
  printf("-reset         Reset device\n");
  printf("-help          Display this help and quit\n");
}

/**
 * Tests two strings for equality, ignoring the case - this is for case insensitive variable name matching
 */
static int areStringsEqualIgnoreCase(char * s1, char * s2) {
  size_t s1_len=strlen(s1), s2_len=strlen(s2), i;
  if (s1_len != s2_len) return 0;
  for (i=0;i<s1_len;i++) {
    if (tolower(s1[i]) != tolower(s2[i])) return 0;
  }
  return 1;
}
