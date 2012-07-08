/*
 * Copyright © 2011 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>

#include "xwayland.h"
#include "xserver-server-protocol.h"
#include "../log.h"


static int
weston_xserver_handle_event(int listen_fd, uint32_t mask, void *data)
{
	struct weston_xserver *mxs = data;
	char display[8], s[8];
	int sv[2], client_fd;

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
		weston_log("socketpair failed\n");
		return 1;
	}

	mxs->process.pid = fork();
	switch (mxs->process.pid) {
	case 0:
		/* SOCK_CLOEXEC closes both ends, so we need to unset
		 * the flag on the client fd. */
		client_fd = dup(sv[1]);
		if (client_fd < 0)
			return 1;

		snprintf(s, sizeof s, "%d", client_fd);
		setenv("WAYLAND_SOCKET", s, 1);

		snprintf(display, sizeof display, ":%d", mxs->display);

		if (execl(XSERVER_PATH,
			  XSERVER_PATH,
			  display,
			  "-wayland",
			  "-rootless",
			  "-retro",
			  "-nolisten", "all",
			  "-terminate",
			  NULL) < 0)
			weston_log("exec failed: %m\n");
		exit(-1);

	default:
		weston_log("forked X server, pid %d\n", mxs->process.pid);

		close(sv[1]);
		mxs->client = wl_client_create(mxs->wl_display, sv[0]);

		weston_watch_process(&mxs->process);

		wl_event_source_remove(mxs->abstract_source);
		wl_event_source_remove(mxs->unix_source);
		break;

	case -1:
		weston_log( "failed to fork\n");
		break;
	}

	return 1;
}

static void
weston_xserver_shutdown(struct weston_xserver *wxs)
{
	char path[256];

	snprintf(path, sizeof path, "/tmp/.X%d-lock", wxs->display);
	unlink(path);
	snprintf(path, sizeof path, "/tmp/.X11-unix/X%d", wxs->display);
	unlink(path);
	if (wxs->process.pid == 0) {
		wl_event_source_remove(wxs->abstract_source);
		wl_event_source_remove(wxs->unix_source);
	}
	close(wxs->abstract_fd);
	close(wxs->unix_fd);
	if (wxs->wm)
		weston_wm_destroy(wxs->wm);
	wxs->loop = NULL;
}

static void
weston_xserver_cleanup(struct weston_process *process, int status)
{
	struct weston_xserver *mxs =
		container_of(process, struct weston_xserver, process);

	mxs->process.pid = 0;
	mxs->client = NULL;
	mxs->resource = NULL;

	mxs->abstract_source =
		wl_event_loop_add_fd(mxs->loop, mxs->abstract_fd,
				     WL_EVENT_READABLE,
				     weston_xserver_handle_event, mxs);

	mxs->unix_source =
		wl_event_loop_add_fd(mxs->loop, mxs->unix_fd,
				     WL_EVENT_READABLE,
				     weston_xserver_handle_event, mxs);

	if (mxs->wm) {
		weston_log("xserver exited, code %d\n", status);
		weston_wm_destroy(mxs->wm);
		mxs->wm = NULL;
	} else {
		/* If the X server crashes before it binds to the
		 * xserver interface, shut down and don't try
		 * again. */
		weston_log("xserver crashing too fast: %d\n", status);
		weston_xserver_shutdown(mxs);
	}
}

static void
bind_xserver(struct wl_client *client,
	     void *data, uint32_t version, uint32_t id)
{
	struct weston_xserver *wxs = data;

	/* If it's a different client than the xserver we launched,
	 * don't start the wm. */
	if (client != wxs->client)
		return;

	wxs->resource = 
		wl_client_add_object(client, &xserver_interface,
				     &xserver_implementation, id, wxs);

	wxs->wm = weston_wm_create(wxs);
	if (wxs->wm == NULL) {
		weston_log("failed to create wm\n");
	}

	xserver_send_listen_socket(wxs->resource, wxs->abstract_fd);
	xserver_send_listen_socket(wxs->resource, wxs->unix_fd);
}

static int
bind_to_abstract_socket(int display)
{
	struct sockaddr_un addr;
	socklen_t size, name_size;
	int fd;

	fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	addr.sun_family = AF_LOCAL;
	name_size = snprintf(addr.sun_path, sizeof addr.sun_path,
			     "%c/tmp/.X11-unix/X%d", 0, display);
	size = offsetof(struct sockaddr_un, sun_path) + name_size;
	if (bind(fd, (struct sockaddr *) &addr, size) < 0) {
		weston_log("failed to bind to @%s: %s\n",
			addr.sun_path + 1, strerror(errno));
		close(fd);
		return -1;
	}

	if (listen(fd, 1) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

static int
bind_to_unix_socket(int display)
{
	struct sockaddr_un addr;
	socklen_t size, name_size;
	int fd;

	fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	addr.sun_family = AF_LOCAL;
	name_size = snprintf(addr.sun_path, sizeof addr.sun_path,
			     "/tmp/.X11-unix/X%d", display) + 1;
	size = offsetof(struct sockaddr_un, sun_path) + name_size;
	unlink(addr.sun_path);
	if (bind(fd, (struct sockaddr *) &addr, size) < 0) {
		weston_log("failed to bind to %s (%s)\n",
			addr.sun_path, strerror(errno));
		close(fd);
		return -1;
	}

	if (listen(fd, 1) < 0) {
		unlink(addr.sun_path);
		close(fd);
		return -1;
	}

	return fd;
}

static int
create_lockfile(int display, char *lockfile, size_t lsize)
{
	char pid[16], *end;
	int fd, size;
	pid_t other;

	snprintf(lockfile, lsize, "/tmp/.X%d-lock", display);
	fd = open(lockfile, O_WRONLY | O_CLOEXEC | O_CREAT | O_EXCL, 0444);
	if (fd < 0 && errno == EEXIST) {
		fd = open(lockfile, O_CLOEXEC, O_RDONLY);
		if (fd < 0 || read(fd, pid, 11) != 11) {
			weston_log("can't read lock file %s: %s\n",
				lockfile, strerror(errno));
			errno = EEXIST;
			return -1;
		}

		other = strtol(pid, &end, 0);
		if (end != pid + 10) {
			weston_log("can't parse lock file %s\n",
				lockfile);
			close(fd);
			errno = EEXIST;
			return -1;
		}

		if (kill(other, 0) < 0 && errno == ESRCH) {
			/* stale lock file; unlink and try again */
			weston_log("unlinking stale lock file %s\n", lockfile);
			close(fd);
			if (unlink(lockfile))
				/* If we fail to unlink, return EEXIST
				   so we try the next display number.*/
				errno = EEXIST;
			else
				errno = EAGAIN;
			return -1;
		}

		close(fd);
		errno = EEXIST;
		return -1;
	} else if (fd < 0) {
		weston_log("failed to create lock file %s: %s\n",
			lockfile, strerror(errno));
		return -1;
	}

	/* Subtle detail: we use the pid of the wayland
	 * compositor, not the xserver in the lock file. */
	size = snprintf(pid, sizeof pid, "%10d\n", getpid());
	if (write(fd, pid, size) != size) {
		unlink(lockfile);
		close(fd);
		return -1;
	}

	close(fd);

	return 0;
}

static void
weston_xserver_destroy(struct wl_listener *l, void *data)
{
	struct weston_xserver *wxs =
		container_of(l, struct weston_xserver, destroy_listener);

	if (!wxs)
		return;

	if (wxs->loop)
		weston_xserver_shutdown(wxs);

	free(wxs);
}

WL_EXPORT int
weston_xserver_init(struct weston_compositor *compositor)
{
	struct wl_display *display = compositor->wl_display;
	struct weston_xserver *mxs;
	char lockfile[256], display_name[8];

	mxs = malloc(sizeof *mxs);
	memset(mxs, 0, sizeof *mxs);

	mxs->process.cleanup = weston_xserver_cleanup;
	mxs->wl_display = display;
	mxs->compositor = compositor;

	mxs->display = 0;

 retry:
	if (create_lockfile(mxs->display, lockfile, sizeof lockfile) < 0) {
		if (errno == EAGAIN) {
			goto retry;
		} else if (errno == EEXIST) {
			mxs->display++;
			goto retry;
		} else {
			free(mxs);
			return -1;
		}
	}

	mxs->abstract_fd = bind_to_abstract_socket(mxs->display);
	if (mxs->abstract_fd < 0 && errno == EADDRINUSE) {
		mxs->display++;
		unlink(lockfile);
		goto retry;
	}

	mxs->unix_fd = bind_to_unix_socket(mxs->display);
	if (mxs->unix_fd < 0) {
		unlink(lockfile);
		close(mxs->abstract_fd);
		free(mxs);
		return -1;
	}

	snprintf(display_name, sizeof display_name, ":%d", mxs->display);
	weston_log("xserver listening on display %s\n", display_name);
	setenv("DISPLAY", display_name, 1);

	mxs->loop = wl_display_get_event_loop(display);
	mxs->abstract_source =
		wl_event_loop_add_fd(mxs->loop, mxs->abstract_fd,
				     WL_EVENT_READABLE,
				     weston_xserver_handle_event, mxs);
	mxs->unix_source =
		wl_event_loop_add_fd(mxs->loop, mxs->unix_fd,
				     WL_EVENT_READABLE,
				     weston_xserver_handle_event, mxs);

	wl_display_add_global(display, &xserver_interface, mxs, bind_xserver);

	mxs->destroy_listener.notify = weston_xserver_destroy;
	wl_signal_add(&compositor->destroy_signal, &mxs->destroy_listener);

	return 0;
}
