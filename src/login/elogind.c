/***
  This file is part of elogind.

  Copyright 2017 Sven Eden

  elogind is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  elogind is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with elogind; If not, see <http://www.gnu.org/licenses/>.
***/


#include "bus-util.h"
#include "cgroup.h"
#include "elogind.h"
#include "fd-util.h"
#include "fileio.h"
#include "fs-util.h"
#include "mount-setup.h"
#include "process-util.h"
#include "socket-util.h"
#include "stdio-util.h"
#include "string-util.h"
#include "strv.h"
#include "umask-util.h"


#define CGROUPS_AGENT_RCVBUF_SIZE (8*1024*1024)
#ifndef ELOGIND_PID_FILE
#  define ELOGIND_PID_FILE "/run/elogind.pid"
#endif // ELOGIND_PID_FILE

static void remove_pid_file(void) {
        if (access(ELOGIND_PID_FILE, F_OK) == 0)
                unlink_noerrno(ELOGIND_PID_FILE);
}


/* daemonize elogind by double forking.
   The grand child returns 0.
   The parent and child return their forks PID.
   On error, a value < 0 is returned.
*/
int elogind_daemonize(void) {
        char c[DECIMAL_STR_MAX(pid_t) + 2];
        pid_t child;
        pid_t grandchild;
        pid_t SID;
        int r;

        child = fork();

        if (child < 0)
                return log_error_errno(errno, "Failed to fork: %m");

        if (child) {
                /* Wait for the child to terminate, so the decoupling
                 * is guaranteed to succeed.
                 */
                r = wait_for_terminate_and_warn("elogind control child", child, true);
                if (r < 0)
                        return r;
                return child;
        }

        /* The first child has to become a new session leader. */
        close_all_fds(NULL, 0);
        SID = setsid();
        if ((pid_t)-1 == SID)
                return log_error_errno(errno, "Failed to create new SID: %m");
        umask(0022);

        /* Now the grandchild, the true daemon, can be created. */
        grandchild = fork();

        if (grandchild < 0)
                return log_error_errno(errno, "Failed to double fork: %m");

        if (grandchild)
                /* Exit immediately! */
                return grandchild;

        close_all_fds(NULL, 0);
        umask(0022);

        /* Take care of our PID-file now */
        grandchild = getpid_cached();

        xsprintf(c, PID_FMT "\n", grandchild);

        r = write_string_file(ELOGIND_PID_FILE, c,
                              WRITE_STRING_FILE_CREATE |
                              WRITE_STRING_FILE_VERIFY_ON_FAILURE);
        if (r < 0)
                log_error_errno(-r, "Failed to write PID file %s: %m",
                                ELOGIND_PID_FILE);

        /* Make sure the PID file gets cleaned up on exit! */
        atexit(remove_pid_file);

        return 0;
}


static int manager_dispatch_cgroups_agent_fd(sd_event_source *source, int fd, uint32_t revents, void *userdata) {
        Manager *m = userdata;
        char buf[PATH_MAX+1];
        ssize_t n;

        n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0)
                return log_error_errno(errno, "Failed to read cgroups agent message: %m");
        if (n == 0) {
                log_error("Got zero-length cgroups agent message, ignoring.");
                return 0;
        }
        if ((size_t) n >= sizeof(buf)) {
                log_error("Got overly long cgroups agent message, ignoring.");
                return 0;
        }

        if (memchr(buf, 0, n)) {
                log_error("Got cgroups agent message with embedded NUL byte, ignoring.");
                return 0;
        }
        buf[n] = 0;

        manager_notify_cgroup_empty(m, buf);

        return 0;
}

/// Add-On for manager_connect_bus()
/// Original: src/core/manager.c:manager_setup_cgroups_agent()
int elogind_setup_cgroups_agent(Manager *m) {

        static const union sockaddr_union sa = {
                .un.sun_family = AF_UNIX,
                .un.sun_path = "/run/systemd/cgroups-agent",
        };
        int r = 0;

        /* This creates a listening socket we receive cgroups agent messages on. We do not use D-Bus for delivering
         * these messages from the cgroups agent binary to PID 1, as the cgroups agent binary is very short-living, and
         * each instance of it needs a new D-Bus connection. Since D-Bus connections are SOCK_STREAM/AF_UNIX, on
         * overloaded systems the backlog of the D-Bus socket becomes relevant, as not more than the configured number
         * of D-Bus connections may be queued until the kernel will start dropping further incoming connections,
         * possibly resulting in lost cgroups agent messages. To avoid this, we'll use a private SOCK_DGRAM/AF_UNIX
         * socket, where no backlog is relevant as communication may take place without an actual connect() cycle, and
         * we thus won't lose messages.
         *
         * Note that PID 1 will forward the agent message to system bus, so that the user systemd instance may listen
         * to it. The system instance hence listens on this special socket, but the user instances listen on the system
         * bus for these messages. */

        if (m->test_run)
                return 0;

        if (!MANAGER_IS_SYSTEM(m))
                return 0;

        r = cg_unified_controller(SYSTEMD_CGROUP_CONTROLLER);
        if (r < 0)
                return log_error_errno(r, "Failed to determine whether unified cgroups hierarchy is used: %m");
        if (r > 0) /* We don't need this anymore on the unified hierarchy */
                return 0;

        if (m->cgroups_agent_fd < 0) {
                _cleanup_close_ int fd = -1;

                /* First free all secondary fields */
                m->cgroups_agent_event_source = sd_event_source_unref(m->cgroups_agent_event_source);

                fd = socket(AF_UNIX, SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK, 0);
                if (fd < 0)
                        return log_error_errno(errno, "Failed to allocate cgroups agent socket: %m");

                fd_inc_rcvbuf(fd, CGROUPS_AGENT_RCVBUF_SIZE);

                (void) unlink(sa.un.sun_path);

                /* Only allow root to connect to this socket */
                RUN_WITH_UMASK(0077)
                        r = bind(fd, &sa.sa, SOCKADDR_UN_LEN(sa.un));
                if (r < 0)
                        return log_error_errno(errno, "bind(%s) failed: %m", sa.un.sun_path);

                m->cgroups_agent_fd = fd;
                fd = -1;
        }

        if (!m->cgroups_agent_event_source) {
                r = sd_event_add_io(m->event, &m->cgroups_agent_event_source, m->cgroups_agent_fd, EPOLLIN, manager_dispatch_cgroups_agent_fd, m);
                if (r < 0)
                        return log_error_errno(r, "Failed to allocate cgroups agent event source: %m");

                /* Process cgroups notifications early, but after having processed service notification messages or
                 * SIGCHLD signals, so that a cgroup running empty is always just the last safety net of notification,
                 * and we collected the metadata the notification and SIGCHLD stuff offers first. Also see handling of
                 * cgroup inotify for the unified cgroup stuff. */
                r = sd_event_source_set_priority(m->cgroups_agent_event_source, SD_EVENT_PRIORITY_NORMAL-5);
                if (r < 0)
                        return log_error_errno(r, "Failed to set priority of cgroups agent event source: %m");

                (void) sd_event_source_set_description(m->cgroups_agent_event_source, "manager-cgroups-agent");
        }

        return 0;
}

/// Add-On for manager_free()
void elogind_manager_free(Manager* m) {

        manager_shutdown_cgroup(m, true);

        sd_event_source_unref(m->cgroups_agent_event_source);

        safe_close(m->cgroups_agent_fd);

        strv_free(m->suspend_mode);
        strv_free(m->suspend_state);
        strv_free(m->hibernate_mode);
        strv_free(m->hibernate_state);
        strv_free(m->hybrid_sleep_mode);
        strv_free(m->hybrid_sleep_state);
}

/// Add-On for manager_new()
int elogind_manager_new(Manager* m) {
        int r = 0;

        m->cgroups_agent_fd = -1;
        m->pin_cgroupfs_fd  = -1;
        m->test_run         = false;

        /* Init sleep modes and states */
        m->suspend_mode       = NULL;
        m->suspend_state      = NULL;
        m->hibernate_mode     = NULL;
        m->hibernate_state    = NULL;
        m->hybrid_sleep_mode  = NULL;
        m->hybrid_sleep_state = NULL;

        /* If elogind should be its own controller, mount its cgroup */
        if (streq(SYSTEMD_CGROUP_CONTROLLER, "_elogind")) {
                m->is_system = true;
                r = mount_setup(true);
        } else
                m->is_system = false;

        /* Make cgroups */
        if (r > -1)
                r = manager_setup_cgroup(m);

        return r;
}

/// Add-On for manager_reset_config()
void elogind_manager_reset_config(Manager* m) {

#ifdef ENABLE_DEBUG_ELOGIND
        int dbg_cnt;
#endif // ENABLE_DEBUG_ELOGIND

        /* Set default Sleep config if not already set by logind.conf */
        if (!m->suspend_state)
                m->suspend_state = strv_new("mem", "standby", "freeze", NULL);
        if (!m->hibernate_mode)
                m->hibernate_mode = strv_new("platform", "shutdown", NULL);
        if (!m->hibernate_state)
                m->hibernate_state = strv_new("disk", NULL);
        if (!m->hybrid_sleep_mode)
                m->hybrid_sleep_mode = strv_new("suspend", "platform", "shutdown", NULL);
        if (!m->hybrid_sleep_state)
                m->hybrid_sleep_state = strv_new("disk", NULL);

#ifdef ENABLE_DEBUG_ELOGIND
        dbg_cnt = -1;
        while (m->suspend_mode && m->suspend_mode[++dbg_cnt])
                log_debug_elogind("suspend_mode[%d] = %s",
                                  dbg_cnt, m->suspend_mode[dbg_cnt]);
        dbg_cnt = -1;
        while (m->suspend_state[++dbg_cnt])
                log_debug_elogind("suspend_state[%d] = %s",
                                  dbg_cnt, m->suspend_state[dbg_cnt]);
        dbg_cnt = -1;
        while (m->hibernate_mode[++dbg_cnt])
                log_debug_elogind("hibernate_mode[%d] = %s",
                                  dbg_cnt, m->hibernate_mode[dbg_cnt]);
        dbg_cnt = -1;
        while (m->hibernate_state[++dbg_cnt])
                log_debug_elogind("hibernate_state[%d] = %s",
                                  dbg_cnt, m->hibernate_state[dbg_cnt]);
        dbg_cnt = -1;
        while (m->hybrid_sleep_mode[++dbg_cnt])
                log_debug_elogind("hybrid_sleep_mode[%d] = %s",
                                  dbg_cnt, m->hybrid_sleep_mode[dbg_cnt]);
        dbg_cnt = -1;
        while (m->hybrid_sleep_state[++dbg_cnt])
                log_debug_elogind("hybrid_sleep_state[%d] = %s",
                                  dbg_cnt, m->hybrid_sleep_state[dbg_cnt]);
#endif // ENABLE_DEBUG_ELOGIND
}
