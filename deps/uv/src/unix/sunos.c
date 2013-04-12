/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv.h"
#include "internal.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#ifndef SUNOS_NO_IFADDRS
# include <ifaddrs.h>
#endif
#include <net/if.h>

#include <sys/loadavg.h>
#include <sys/time.h>
#include <unistd.h>
#include <kstat.h>
#include <fcntl.h>

#include <sys/port.h>
#include <port.h>

#define PORT_FIRED 0x69
#define PORT_UNUSED 0x0
#define PORT_LOADED 0x99
#define PORT_DELETED -1

#if (!defined(_LP64)) && (_FILE_OFFSET_BITS - 0 == 64)
#define PROCFS_FILE_OFFSET_BITS_HACK 1
#undef _FILE_OFFSET_BITS
#else
#define PROCFS_FILE_OFFSET_BITS_HACK 0
#endif

#include <procfs.h>

#if (PROCFS_FILE_OFFSET_BITS_HACK - 0 == 1)
#define _FILE_OFFSET_BITS 64
#endif


int uv__platform_loop_init(uv_loop_t* loop, int default_loop) {
  loop->fs_fd = -1;
  loop->backend_fd = port_create();

  if (loop->backend_fd == -1)
    return -1;

  uv__cloexec(loop->backend_fd, 1);

  return 0;
}


void uv__platform_loop_delete(uv_loop_t* loop) {
  if (loop->fs_fd != -1) {
    close(loop->fs_fd);
    loop->fs_fd = -1;
  }

  if (loop->backend_fd != -1) {
    close(loop->backend_fd);
    loop->backend_fd = -1;
  }
}


void uv__io_poll(uv_loop_t* loop, int timeout) {
  struct port_event events[1024];
  struct port_event* pe;
  struct timespec spec;
  QUEUE* q;
  uv__io_t* w;
  uint64_t base;
  uint64_t diff;
  unsigned int nfds;
  unsigned int i;
  int saved_errno;
  int nevents;
  int count;
  int fd;

  if (loop->nfds == 0) {
    assert(QUEUE_EMPTY(&loop->watcher_queue));
    return;
  }

  while (!QUEUE_EMPTY(&loop->watcher_queue)) {
    q = QUEUE_HEAD(&loop->watcher_queue);
    QUEUE_REMOVE(q);
    QUEUE_INIT(q);

    w = QUEUE_DATA(q, uv__io_t, watcher_queue);
    assert(w->pevents != 0);

    if (port_associate(loop->backend_fd, PORT_SOURCE_FD, w->fd, w->pevents, 0))
      abort();

    w->events = w->pevents;
  }

  assert(timeout >= -1);
  base = loop->time;
  count = 48; /* Benchmarks suggest this gives the best throughput. */

  for (;;) {
    if (timeout != -1) {
      spec.tv_sec = timeout / 1000;
      spec.tv_nsec = (timeout % 1000) * 1000000;
    }

    /* Work around a kernel bug where nfds is not updated. */
    events[0].portev_source = 0;

    nfds = 1;
    saved_errno = 0;
    if (port_getn(loop->backend_fd,
                  events,
                  ARRAY_SIZE(events),
                  &nfds,
                  timeout == -1 ? NULL : &spec)) {
      /* Work around another kernel bug: port_getn() may return events even
       * on error.
       */
      if (errno == EINTR || errno == ETIME)
        saved_errno = errno;
      else
        abort();
    }

    /* Update loop->time unconditionally. It's tempting to skip the update when
     * timeout == 0 (i.e. non-blocking poll) but there is no guarantee that the
     * operating system didn't reschedule our process while in the syscall.
     */
    SAVE_ERRNO(uv__update_time(loop));

    if (events[0].portev_source == 0) {
      if (timeout == 0)
        return;

      if (timeout == -1)
        continue;

      goto update_timeout;
    }

    if (nfds == 0) {
      assert(timeout != -1);
      return;
    }

    nevents = 0;

    for (i = 0; i < nfds; i++) {
      pe = events + i;
      fd = pe->portev_object;

      assert(fd >= 0);
      assert((unsigned) fd < loop->nwatchers);

      w = loop->watchers[fd];

      /* File descriptor that we've stopped watching, ignore. */
      if (w == NULL)
        continue;

      w->cb(loop, w, pe->portev_events);
      nevents++;

      /* Events Ports operates in oneshot mode, rearm timer on next run. */
      if (w->pevents != 0 && QUEUE_EMPTY(&w->watcher_queue))
        QUEUE_INSERT_TAIL(&loop->watcher_queue, &w->watcher_queue);
    }

    if (nevents != 0) {
      if (nfds == ARRAY_SIZE(events) && --count != 0) {
        /* Poll for more events but don't block this time. */
        timeout = 0;
        continue;
      }
      return;
    }

    if (saved_errno == ETIME) {
      assert(timeout != -1);
      return;
    }

    if (timeout == 0)
      return;

    if (timeout == -1)
      continue;

update_timeout:
    assert(timeout > 0);

    diff = loop->time - base;
    if (diff >= (uint64_t) timeout)
      return;

    timeout -= diff;
  }
}


uint64_t uv__hrtime(void) {
  return gethrtime();
}


/*
 * We could use a static buffer for the path manipulations that we need outside
 * of the function, but this function could be called by multiple consumers and
 * we don't want to potentially create a race condition in the use of snprintf.
 */
int uv_exepath(char* buffer, size_t* size) {
  ssize_t res;
  char buf[128];

  if (buffer == NULL)
    return (-1);

  if (size == NULL)
    return (-1);

  (void) snprintf(buf, sizeof(buf), "/proc/%lu/path/a.out", (unsigned long) getpid());
  res = readlink(buf, buffer, *size - 1);

  if (res < 0)
    return (res);

  buffer[res] = '\0';
  *size = res;
  return (0);
}


uint64_t uv_get_free_memory(void) {
  return (uint64_t) sysconf(_SC_PAGESIZE) * sysconf(_SC_AVPHYS_PAGES);
}


uint64_t uv_get_total_memory(void) {
  return (uint64_t) sysconf(_SC_PAGESIZE) * sysconf(_SC_PHYS_PAGES);
}


void uv_loadavg(double avg[3]) {
  (void) getloadavg(avg, 3);
}


#if defined(PORT_SOURCE_FILE)

static void uv__fs_event_rearm(uv_fs_event_t *handle) {
  if (handle->fd == -1)
    return;

  if (port_associate(handle->loop->fs_fd,
                     PORT_SOURCE_FILE,
                     (uintptr_t) &handle->fo,
                     FILE_ATTRIB | FILE_MODIFIED,
                     handle) == -1) {
    uv__set_sys_error(handle->loop, errno);
  }
  handle->fd = PORT_LOADED;
}


static void uv__fs_event_read(uv_loop_t* loop,
                              uv__io_t* w,
                              unsigned int revents) {
  uv_fs_event_t *handle = NULL;
  timespec_t timeout;
  port_event_t pe;
  int events;
  int r;

  (void) w;
  (void) revents;

  do {
    uint_t n = 1;

    /*
     * Note that our use of port_getn() here (and not port_get()) is deliberate:
     * there is a bug in event ports (Sun bug 6456558) whereby a zeroed timeout
     * causes port_get() to return success instead of ETIME when there aren't
     * actually any events (!); by using port_getn() in lieu of port_get(),
     * we can at least workaround the bug by checking for zero returned events
     * and treating it as we would ETIME.
     */
    do {
      memset(&timeout, 0, sizeof timeout);
      r = port_getn(loop->fs_fd, &pe, 1, &n, &timeout);
    }
    while (r == -1 && errno == EINTR);

    if ((r == -1 && errno == ETIME) || n == 0)
      break;

    handle = (uv_fs_event_t*) pe.portev_user;
    assert((r == 0) && "unexpected port_get() error");

    events = 0;
    if (pe.portev_events & (FILE_ATTRIB | FILE_MODIFIED))
      events |= UV_CHANGE;
    if (pe.portev_events & ~(FILE_ATTRIB | FILE_MODIFIED))
      events |= UV_RENAME;
    assert(events != 0);
    handle->fd = PORT_FIRED;
    handle->cb(handle, NULL, events, 0);
  }
  while (handle->fd != PORT_DELETED);

  if (handle != NULL && handle->fd != PORT_DELETED)
    uv__fs_event_rearm(handle);
}


int uv_fs_event_init(uv_loop_t* loop,
                     uv_fs_event_t* handle,
                     const char* filename,
                     uv_fs_event_cb cb,
                     int flags) {
  int portfd;
  int first_run = 0;

  if (loop->fs_fd == -1) {
    if ((portfd = port_create()) == -1) {
      uv__set_sys_error(loop, errno);
      return -1;
    }
    loop->fs_fd = portfd;
    first_run = 1;
  }

  uv__handle_init(loop, (uv_handle_t*)handle, UV_FS_EVENT);
  uv__handle_start(handle); /* FIXME shouldn't start automatically */
  handle->filename = strdup(filename);
  handle->fd = PORT_UNUSED;
  handle->cb = cb;

  memset(&handle->fo, 0, sizeof handle->fo);
  handle->fo.fo_name = handle->filename;
  uv__fs_event_rearm(handle);

  if (first_run) {
    uv__io_init(&loop->fs_event_watcher, uv__fs_event_read, portfd);
    uv__io_start(loop, &loop->fs_event_watcher, UV__POLLIN);
  }

  return 0;
}


void uv__fs_event_close(uv_fs_event_t* handle) {
  if (handle->fd == PORT_FIRED || handle->fd == PORT_LOADED) {
    port_dissociate(handle->loop->fs_fd, PORT_SOURCE_FILE, (uintptr_t)&handle->fo);
  }
  handle->fd = PORT_DELETED;
  free(handle->filename);
  handle->filename = NULL;
  handle->fo.fo_name = NULL;
  uv__handle_stop(handle);
}

#else /* !defined(PORT_SOURCE_FILE) */

int uv_fs_event_init(uv_loop_t* loop,
                     uv_fs_event_t* handle,
                     const char* filename,
                     uv_fs_event_cb cb,
                     int flags) {
  uv__set_sys_error(loop, ENOSYS);
  return -1;
}


void uv__fs_event_close(uv_fs_event_t* handle) {
  UNREACHABLE();
}

#endif /* defined(PORT_SOURCE_FILE) */


char** uv_setup_args(int argc, char** argv) {
  return argv;
}


uv_err_t uv_set_process_title(const char* title) {
  return uv_ok_;
}


uv_err_t uv_get_process_title(char* buffer, size_t size) {
  if (size > 0) {
    buffer[0] = '\0';
  }
  return uv_ok_;
}


uv_err_t uv_resident_set_memory(size_t* rss) {
  psinfo_t psinfo;
  uv_err_t err;
  int fd;

  fd = open("/proc/self/psinfo", O_RDONLY);
  if (fd == -1)
    return uv__new_sys_error(errno);

  err = uv_ok_;

  if (read(fd, &psinfo, sizeof(psinfo)) == sizeof(psinfo))
    *rss = (size_t)psinfo.pr_rssize * 1024;
  else
    err = uv__new_sys_error(EINVAL);

  close(fd);

  return err;
}


uv_err_t uv_uptime(double* uptime) {
  kstat_ctl_t   *kc;
  kstat_t       *ksp;
  kstat_named_t *knp;

  long hz = sysconf(_SC_CLK_TCK);

  if ((kc = kstat_open()) == NULL)
    return uv__new_sys_error(errno);

  ksp = kstat_lookup(kc, (char*) "unix", 0, (char*) "system_misc");

  if (kstat_read(kc, ksp, NULL) == -1) {
    *uptime = -1;
  } else {
    knp = (kstat_named_t*)  kstat_data_lookup(ksp, (char*) "clk_intr");
    *uptime = knp->value.ul / hz;
  }

  kstat_close(kc);

  return uv_ok_;
}


uv_err_t uv_cpu_info(uv_cpu_info_t** cpu_infos, int* count) {
  int           lookup_instance;
  kstat_ctl_t   *kc;
  kstat_t       *ksp;
  kstat_named_t *knp;
  uv_cpu_info_t* cpu_info;

  if ((kc = kstat_open()) == NULL) {
    return uv__new_sys_error(errno);
  }

  /* Get count of cpus */
  lookup_instance = 0;
  while ((ksp = kstat_lookup(kc, (char*) "cpu_info", lookup_instance, NULL))) {
    lookup_instance++;
  }

  *cpu_infos = (uv_cpu_info_t*)
    malloc(lookup_instance * sizeof(uv_cpu_info_t));
  if (!(*cpu_infos)) {
    return uv__new_artificial_error(UV_ENOMEM);
  }

  *count = lookup_instance;

  cpu_info = *cpu_infos;
  lookup_instance = 0;
  while ((ksp = kstat_lookup(kc, (char*) "cpu_info", lookup_instance, NULL))) {
    if (kstat_read(kc, ksp, NULL) == -1) {
      cpu_info->speed = 0;
      cpu_info->model = NULL;
    } else {
      knp = (kstat_named_t*)  kstat_data_lookup(ksp, (char*) "clock_MHz");
      assert(knp->data_type == KSTAT_DATA_INT32 ||
             knp->data_type == KSTAT_DATA_INT64);
      cpu_info->speed = (knp->data_type == KSTAT_DATA_INT32) ? knp->value.i32
                                                             : knp->value.i64;

      knp = (kstat_named_t*)  kstat_data_lookup(ksp, (char*) "brand");
      assert(knp->data_type == KSTAT_DATA_STRING);
      cpu_info->model = strdup(KSTAT_NAMED_STR_PTR(knp));
    }

    lookup_instance++;
    cpu_info++;
  }

  cpu_info = *cpu_infos;
  lookup_instance = 0;
  while ((ksp = kstat_lookup(kc, (char*) "cpu", lookup_instance, (char*) "sys"))){

    if (kstat_read(kc, ksp, NULL) == -1) {
      cpu_info->cpu_times.user = 0;
      cpu_info->cpu_times.nice = 0;
      cpu_info->cpu_times.sys = 0;
      cpu_info->cpu_times.idle = 0;
      cpu_info->cpu_times.irq = 0;
    } else {
      knp = (kstat_named_t*)  kstat_data_lookup(ksp, (char*) "cpu_ticks_user");
      assert(knp->data_type == KSTAT_DATA_UINT64);
      cpu_info->cpu_times.user = knp->value.ui64;

      knp = (kstat_named_t*)  kstat_data_lookup(ksp, (char*) "cpu_ticks_kernel");
      assert(knp->data_type == KSTAT_DATA_UINT64);
      cpu_info->cpu_times.sys = knp->value.ui64;

      knp = (kstat_named_t*)  kstat_data_lookup(ksp, (char*) "cpu_ticks_idle");
      assert(knp->data_type == KSTAT_DATA_UINT64);
      cpu_info->cpu_times.idle = knp->value.ui64;

      knp = (kstat_named_t*)  kstat_data_lookup(ksp, (char*) "intr");
      assert(knp->data_type == KSTAT_DATA_UINT64);
      cpu_info->cpu_times.irq = knp->value.ui64;
      cpu_info->cpu_times.nice = 0;
    }

    lookup_instance++;
    cpu_info++;
  }

  kstat_close(kc);

  return uv_ok_;
}


void uv_free_cpu_info(uv_cpu_info_t* cpu_infos, int count) {
  int i;

  for (i = 0; i < count; i++) {
    free(cpu_infos[i].model);
  }

  free(cpu_infos);
}


uv_err_t uv_interface_addresses(uv_interface_address_t** addresses,
  int* count) {
#ifdef SUNOS_NO_IFADDRS
  return uv__new_artificial_error(UV_ENOSYS);
#else
  struct ifaddrs *addrs, *ent;
  char ip[INET6_ADDRSTRLEN];
  uv_interface_address_t* address;

  if (getifaddrs(&addrs) != 0) {
    return uv__new_sys_error(errno);
  }

  *count = 0;

  /* Count the number of interfaces */
  for (ent = addrs; ent != NULL; ent = ent->ifa_next) {
    if (!(ent->ifa_flags & IFF_UP && ent->ifa_flags & IFF_RUNNING) ||
        (ent->ifa_addr == NULL) ||
        (ent->ifa_addr->sa_family == PF_PACKET)) {
      continue;
    }

    (*count)++;
  }

  *addresses = (uv_interface_address_t*)
    malloc(*count * sizeof(uv_interface_address_t));
  if (!(*addresses)) {
    return uv__new_artificial_error(UV_ENOMEM);
  }

  address = *addresses;

  for (ent = addrs; ent != NULL; ent = ent->ifa_next) {
    memset(&ip, 0, sizeof(ip));

    if (!(ent->ifa_flags & IFF_UP && ent->ifa_flags & IFF_RUNNING)) {
      continue;
    }

    if (ent->ifa_addr == NULL) {
      continue;
    }

    address->name = strdup(ent->ifa_name);

    if (ent->ifa_addr->sa_family == AF_INET6) {
      address->address.address6 = *((struct sockaddr_in6*) ent->ifa_addr);
    } else {
      address->address.address4 = *((struct sockaddr_in*) ent->ifa_addr);
    }

    if (ent->ifa_netmask->sa_family == AF_INET6) {
      address->netmask.netmask6 = *((struct sockaddr_in6*) ent->ifa_netmask);
    } else {
      address->netmask.netmask4 = *((struct sockaddr_in*) ent->ifa_netmask);
    }

    address->is_internal = ent->ifa_flags & IFF_PRIVATE || ent->ifa_flags &
	IFF_LOOPBACK ? 1 : 0;

    address++;
  }

  freeifaddrs(addrs);

  return uv_ok_;
#endif  /* SUNOS_NO_IFADDRS */
}


void uv_free_interface_addresses(uv_interface_address_t* addresses,
  int count) {
  int i;

  for (i = 0; i < count; i++) {
    free(addresses[i].name);
  }

  free(addresses);
}
