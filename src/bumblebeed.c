/*
 * Copyright (C) 2011 Bumblebee Project
 * Author: Joaquín Ignacio Aramendía <samsagax@gmail.com>
 * Author: Jaron Viëtor AKA "Thulinma" <jaron@vietors.com>
 *
 * This file is part of Bumblebee.
 *
 * Bumblebee is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Bumblebee is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Bumblebee. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * C-coded version of the Bumblebee daemon and optirun.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#ifdef WITH_PIDFILE
#ifdef HAVE_LIBBSD_020
#include <libutil.h>
#else
#include <bsd/libutil.h>
#endif
#endif
#include "bbconfig.h"
#include "bbsocket.h"
#include "bblogger.h"
#include "bbsecondary.h"
#include "bbrun.h"
#include "pci.h"
#include "driver.h"
#include "switch/switching.h"
#include "connections-handler.h"
#include "dbus/dbus.h"

/**
 * Change GID and umask of the daemon
 * @return EXIT_SUCCESS if the gid could be changed, EXIT_FAILURE otherwise
 */
static int bb_chgid(void) {
  /* Change the Group ID of bumblebee */
  struct group *gp;
  errno = 0;
  gp = getgrnam(bb_config.gid_name);
  if (gp == NULL) {
    int error_num = errno;
    bb_log(LOG_ERR, "%s\n", strerror(error_num));
    bb_log(LOG_ERR, "There is no \"%s\" group\n", bb_config.gid_name);
    return EXIT_FAILURE;
  }
  if (setgid(gp->gr_gid) != 0) {
    bb_log(LOG_ERR, "Could not set the GID of bumblebee: %s\n", strerror(errno));
    return EXIT_FAILURE;
  }
  /* Change the file mode mask */
  umask(027);
  return EXIT_SUCCESS;
}

/**
 * Fork to the background, and exit parent.
 * @return EXIT_SUCCESS if the daemon could fork, EXIT_FAILURE otherwise. Note
 * that the parent exits and the child continues to run
 */
static int daemonize(void) {
  /* Fork off the parent process */
  pid_t bb_pid = fork();
  if (bb_pid < 0) {
    bb_log(LOG_ERR, "Could not fork to background\n");
    return EXIT_FAILURE;
  }

  /* If we got a good PID, then we can exit the parent process. */
  if (bb_pid > 0) {
    exit(EXIT_SUCCESS);
  }

  /* Create a new SID for the child process */
  pid_t bb_sid = setsid();
  if (bb_sid < 0) {
    bb_log(LOG_ERR, "Could not set SID: %s\n", strerror(errno));
    return EXIT_FAILURE;
  }

  /* Change the current working directory */
  if ((chdir("/")) < 0) {
    bb_log(LOG_ERR, "Could not change to root directory: %s\n", strerror(errno));
    return EXIT_FAILURE;
  }

  /* Reroute standard file descriptors to /dev/null */
  int devnull = open("/dev/null", O_RDWR);
  if (devnull < 0){
    bb_log(LOG_ERR, "Could not open /dev/null: %s\n", strerror(errno));
    return EXIT_FAILURE;
  }
  dup2(devnull, STDIN_FILENO);
  dup2(devnull, STDOUT_FILENO);
  dup2(devnull, STDERR_FILENO);
  return EXIT_SUCCESS;
}

/**
 *  Handle recieved signals - except SIGCHLD, which is handled in bbrun.c
 */
static void handle_signal(int sig) {
  static int sigpipes = 0;

  switch (sig) {
    case SIGHUP:
      bb_log(LOG_WARNING, "Received %s signal (ignoring...)\n", strsignal(sig));
      break;
    case SIGPIPE:
      /* if bb_log generates a SIGPIPE, i.e. when bumblebeed runs like
       * bumblebeed 2>&1 | cat and the pipe is killed, don't die infinitely */
      if (sigpipes <= 10) {
        bb_log(LOG_WARNING, "Received %s signal %i (signals 10> are ignored)\n",
                strsignal(sig), ++sigpipes);
      }
      break;
    case SIGINT:
    case SIGQUIT:
      bb_log(LOG_WARNING, "Received %s signal.\n", strsignal(sig));
      socketClose(&bb_status.bb_socket); //closing the socket terminates the server
      break;
    case SIGTERM:
      bb_log(LOG_WARNING, "Received %s signal.\n", strsignal(sig));
      socketClose(&bb_status.bb_socket); //closing the socket terminates the server
      bb_run_stopwaiting(); //speed up shutdown by not waiting for processes anymore
      break;
    default:
      bb_log(LOG_WARNING, "Unhandled signal %s\n", strsignal(sig));
      break;
  }
}

/**
 * Returns the option string for this program
 * @return An option string which can be used for getopt
 */
const char *bbconfig_get_optstr(void) {
  return BBCONFIG_COMMON_OPTSTR "Dx:g:m:k:";
}

/**
 * Returns the long options for this program
 * @return A option struct which can be used for getopt_long
 */
const struct option *bbconfig_get_lopts(void) {
  static struct option longOpts[] = {
    {"daemon", 0, 0, 'D'},
    {"xconf", 1, 0, 'x'},
    {"group", 1, 0, 'g'},
    {"module-path", 1, 0, 'm'},
    {"driver-module", 1, 0, 'k'},
    {"driver", 1, 0, OPT_DRIVER},
#ifdef WITH_PIDFILE
    {"pidfile", 1, 0, OPT_PIDFILE},
#endif
    {"use-syslog", 0, 0, OPT_USE_SYSLOG},
    {"pm-method", 1, 0, OPT_PM_METHOD},
    BBCONFIG_COMMON_LOPTS
  };
  return longOpts;
}

/**
 * Parses local command line options
 * @param opt The short option
 * @param value Value for the option if any
 * @return 1 if the option has been processed, 0 otherwise
 */
int bbconfig_parse_options(int opt, char *value) {
  switch (opt) {
    case OPT_USE_SYSLOG:
      /* already processed in bbconfig.c */
      break;
    case 'D'://daemonize
      bb_status.runmode = BB_RUN_DAEMON;
      break;
    case 'x'://xorg.conf path
      set_string_value(&bb_config.x_conf_file, value);
      break;
    case 'g'://group name to use
      set_string_value(&bb_config.gid_name, value);
      break;
    case 'm'://modulepath
      set_string_value(&bb_config.mod_path, value);
      break;
    case OPT_DRIVER://driver
      set_string_value(&bb_config.driver, value);
      break;
    case 'k'://kernel module
      set_string_value(&bb_config.module_name, value);
      break;
    case OPT_PM_METHOD:
      bb_config.pm_method = bb_pm_method_from_string(value);
      break;
#ifdef WITH_PIDFILE
    case OPT_PIDFILE:
      set_string_value(&bb_config.pid_file, value);
      break;
#endif
    default:
      /* no options parsed */
      return 0;
  }
  return 1;
}

int main(int argc, char* argv[]) {
#ifdef WITH_PIDFILE
  struct pidfh *pfh = NULL;
  pid_t otherpid;
#endif

  /* the logs needs to be ready before the signal handlers */
  init_early_config(argc, argv, BB_RUN_SERVER);
  bbconfig_parse_opts(argc, argv, PARSE_STAGE_LOG);
  bb_init_log();

  /* Setup signal handling before anything else. Note that messages are not
   * shown until init_config has set bb_status.verbosity
   */
  signal(SIGHUP, handle_signal);
  signal(SIGTERM, handle_signal);
  signal(SIGINT, handle_signal);
  signal(SIGQUIT, handle_signal);
  signal(SIGPIPE, handle_signal);

  /* first load the config to make the logging verbosity level available */
  init_config(argc, argv);
  bbconfig_parse_opts(argc, argv, PARSE_STAGE_PRECONF);

  pci_bus_id_discrete = pci_find_gfx_by_vendor(PCI_VENDOR_ID_NVIDIA);
  if (!pci_bus_id_discrete) {
    bb_log(LOG_ERR, "No nVidia graphics card found, quitting.\n");
    return (EXIT_FAILURE);
  }
  struct pci_bus_id *pci_id_igd = pci_find_gfx_by_vendor(PCI_VENDOR_ID_INTEL);
  if (!pci_id_igd) {
    bb_log(LOG_ERR, "No Optimus system detected, quitting.\n");
    return (EXIT_FAILURE);
  }
  free(pci_id_igd);

  GKeyFile *bbcfg = bbconfig_parse_conf();
  bbconfig_parse_opts(argc, argv, PARSE_STAGE_DRIVER);
  driver_detect();
  if (bbcfg) {
    bbconfig_parse_conf_driver(bbcfg, bb_config.driver);
    g_key_file_free(bbcfg);
  }
  bbconfig_parse_opts(argc, argv, PARSE_STAGE_OTHER);
  check_pm_method();

  /* dump the config after detecting the driver */
  config_dump();
  if (config_validate() != 0) {
    return (EXIT_FAILURE);
  }

#ifdef WITH_PIDFILE
  /* only write PID if a pid file has been set */
  if (bb_config.pid_file[0]) {
    pfh = pidfile_open(bb_config.pid_file, 0644, &otherpid);
    if (pfh == NULL) {
      if (errno == EEXIST) {
        bb_log(LOG_ERR, "Daemon already running, pid %d\n", otherpid);
      } else {
        bb_log(LOG_ERR, "Cannot open or write pidfile %s.\n", bb_config.pid_file);
      }
      bb_closelog();
      exit(EXIT_FAILURE);
    }
  }
#endif

  /* Change GID and mask according to configuration */
  if ((bb_config.gid_name != 0) && (bb_config.gid_name[0] != 0)) {
    int retval = bb_chgid();
    if (retval != EXIT_SUCCESS) {
      bb_closelog();
#ifdef WITH_PIDFILE
      pidfile_remove(pfh);
#endif
      exit(retval);
    }
  }

  bb_log(LOG_NOTICE, "%s %s started\n", bb_status.program_name, GITVERSION);

  /* Daemonized if daemon flag is activated */
  if (bb_status.runmode == BB_RUN_DAEMON) {
    int retval = daemonize();
    if (retval != EXIT_SUCCESS) {
      bb_closelog();
#ifdef WITH_PIDFILE
      pidfile_remove(pfh);
#endif
      exit(retval);
    }
  }

#ifdef WITH_PIDFILE
  /* write PID after daemonizing */
  pidfile_write(pfh);
#endif

  bb_dbus_init();

  /* Initialize communication socket, enter main loop */
  bb_status.bb_socket = socketServer(bb_config.socket_path, SOCK_NOBLOCK);
  stop_secondary(); //turn off card, nobody is connected right now.

  /* handle dbus and socket stuff */
  bb_log(LOG_INFO, "Initialization completed - now handling client requests\n");
  GMainLoop *main_loop = g_main_loop_new(NULL, FALSE);
  g_timeout_add(100, handle_connection, main_loop);
  g_main_loop_run(main_loop);

  connections_fini();
  unlink(bb_config.socket_path);
  bb_status.runmode = BB_RUN_EXIT; //make sure all methods understand we are shutting down
  if (bb_config.card_shutdown_state) {
    //if shutdown state = 1, turn on card
    start_secondary();
  } else {
    //if shutdown state = 0, turn off card
    stop_secondary();
  }
  bb_dbus_fini();
  bb_closelog();
#ifdef WITH_PIDFILE
  pidfile_remove(pfh);
#endif
  bb_stop_all(); //stop any started processes that are left
  /* close xorg standard output if there is an open one */
  if (bb_status.x_err_fd != -1) {
    close(bb_status.x_err_fd);
    bb_status.x_err_fd = -1;
  }
  return (EXIT_SUCCESS);
}
