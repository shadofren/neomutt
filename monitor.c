/**
 * @file
 * Monitor files for changes
 *
 * @authors
 * Copyright (C) 2018 Gero Treuer <gero@70t.de>
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @page monitor Monitor files for changes
 *
 * Monitor files for changes
 */

#include "config.h"
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include "mutt/mutt.h"
#include "monitor.h"
#include "context.h"
#include "curs_lib.h"
#include "globals.h"
#include "mailbox.h"
#include "mutt_curses.h"
#include "mx.h"

int MonitorFilesChanged = 0;
int MonitorContextChanged = 0;

static int INotifyFd = -1;
static struct Monitor *Monitor = NULL;
static size_t PollFdsCount = 0;
static size_t PollFdsLen = 0;
static struct pollfd *PollFds = NULL;

static int MonitorContextDescriptor = -1;

#define INOTIFY_MASK_DIR (IN_MOVED_TO | IN_ATTRIB | IN_CLOSE_WRITE | IN_ISDIR)
#define INOTIFY_MASK_FILE IN_CLOSE_WRITE

#define EVENT_BUFLEN MAX(4096, sizeof(struct inotify_event) + NAME_MAX + 1)

#define RESOLVERES_FAIL_NOMAILBOX -3
#define RESOLVERES_FAIL_NOMAGIC -2
#define RESOLVERES_FAIL_STAT -1
#define RESOLVERES_OK_NOTEXISTING 0
#define RESOLVERES_OK_EXISTING 1

/**
 * struct Monitor - A watch on a file
 */
struct Monitor
{
  struct Monitor *next;
  char *mh_backup_path;
  dev_t st_dev;
  ino_t st_ino;
  enum MailboxType magic;
  int desc;
};

/**
 * struct MonitorInfo - Information about a monitored file
 */
struct MonitorInfo
{
  enum MailboxType magic;
  short isdir;
  const char *path;
  dev_t st_dev;
  ino_t st_ino;
  struct Monitor *monitor;
  struct Buffer *path_buf; /* access via path only (maybe not initialized) */
};

/**
 * mutt_poll_fd_add - Add a file to the watch list
 * @param fd     File to watch
 * @param events Events to listen for, e.g. POLLIN
 */
static void mutt_poll_fd_add(int fd, short events)
{
  int i = 0;
  for (; (i < PollFdsCount) && (PollFds[i].fd != fd); i++)
    ;

  if (i == PollFdsCount)
  {
    if (PollFdsCount == PollFdsLen)
    {
      PollFdsLen += 2;
      mutt_mem_realloc(&PollFds, PollFdsLen * sizeof(struct pollfd));
    }
    PollFdsCount++;
    PollFds[i].fd = fd;
    PollFds[i].events = events;
  }
  else
    PollFds[i].events |= events;
}

/**
 * mutt_poll_fd_remove - Remove a file from the watch list
 * @param fd File to remove
 * @retval  0 Success
 * @retval -1 Error
 */
static int mutt_poll_fd_remove(int fd)
{
  int i = 0;
  for (; (i < PollFdsCount) && (PollFds[i].fd != fd); i++)
    ;
  if (i == PollFdsCount)
    return -1;
  int d = PollFdsCount - i - 1;
  if (d != 0)
    memmove(&PollFds[i], &PollFds[i + 1], d * sizeof(struct pollfd));
  PollFdsCount--;
  return 0;
}

/**
 * monitor_init - Set up file monitoring
 * @retval  0 Success
 * @retval -1 Error
 */
static int monitor_init(void)
{
  if (INotifyFd == -1)
  {
    INotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (INotifyFd == -1)
    {
      mutt_debug(LL_DEBUG2, "inotify_init1 failed, errno=%d %s\n", errno, strerror(errno));
      return -1;
    }
    mutt_poll_fd_add(0, POLLIN);
    mutt_poll_fd_add(INotifyFd, POLLIN);
  }
  return 0;
}

/**
 * monitor_check_free - Close down file monitoring
 */
static void monitor_check_free(void)
{
  if (!Monitor && (INotifyFd != -1))
  {
    mutt_poll_fd_remove(INotifyFd);
    close(INotifyFd);
    INotifyFd = -1;
    MonitorFilesChanged = 0;
  }
}

/**
 * monitor_new - Create a new file monitor
 * @param info       Details of file to monitor
 * @param descriptor Watch descriptor
 * @retval ptr Newly allocated Monitor
 */
static struct Monitor *monitor_new(struct MonitorInfo *info, int descriptor)
{
  struct Monitor *monitor = mutt_mem_calloc(1, sizeof(struct Monitor));
  monitor->magic = info->magic;
  monitor->st_dev = info->st_dev;
  monitor->st_ino = info->st_ino;
  monitor->desc = descriptor;
  monitor->next = Monitor;
  if (info->magic == MUTT_MH)
    monitor->mh_backup_path = mutt_str_strdup(info->path);

  Monitor = monitor;

  return monitor;
}

/**
 * monitor_info_init - Set up a file monitor
 * @param info Monitor to initialise
 */
static void monitor_info_init(struct MonitorInfo *info)
{
  memset(info, 0, sizeof(*info));
}

/**
 * monitor_info_free - Shutdown a file monitor
 * @param info Monitor to shut down
 */
static void monitor_info_free(struct MonitorInfo *info)
{
  mutt_buffer_free(&info->path_buf);
}

/**
 * monitor_delete - Free a file monitor
 * @param monitor Monitor to free
 */
static void monitor_delete(struct Monitor *monitor)
{
  if (!monitor)
    return;

  struct Monitor **ptr = &Monitor;

  while (true)
  {
    if (!*ptr)
      return;
    if (*ptr == monitor)
      break;
    ptr = &(*ptr)->next;
  }

  FREE(&monitor->mh_backup_path);
  monitor = monitor->next;
  FREE(ptr);
  *ptr = monitor;
}

/**
 * monitor_handle_ignore - Listen for when a backup file is closed
 * @param desc Watch descriptor
 * @retval >=0 New descriptor
 * @retval  -1 Error
 */
static int monitor_handle_ignore(int desc)
{
  int new_desc = -1;
  struct Monitor *iter = Monitor;
  struct stat sb;

  while (iter && (iter->desc != desc))
    iter = iter->next;

  if (iter)
  {
    if ((iter->magic == MUTT_MH) && (stat(iter->mh_backup_path, &sb) == 0))
    {
      new_desc = inotify_add_watch(INotifyFd, iter->mh_backup_path, INOTIFY_MASK_FILE);
      if (new_desc == -1)
      {
        mutt_debug(LL_DEBUG2, "inotify_add_watch failed for '%s', errno=%d %s\n",
                   iter->mh_backup_path, errno, strerror(errno));
      }
      else
      {
        mutt_debug(LL_DEBUG3, "inotify_add_watch descriptor=%d for '%s'\n",
                   desc, iter->mh_backup_path);
        iter->st_dev = sb.st_dev;
        iter->st_ino = sb.st_ino;
        iter->desc = new_desc;
      }
    }
    else
    {
      mutt_debug(LL_DEBUG3, "cleanup watch (implicitly removed) - descriptor=%d\n", desc);
    }

    if (MonitorContextDescriptor == desc)
      MonitorContextDescriptor = new_desc;

    if (new_desc == -1)
    {
      monitor_delete(iter);
      monitor_check_free();
    }
  }

  return new_desc;
}

/**
 * monitor_resolve - Get the monitor for a mailbox
 * @param[out] info Details of the mailbox's monitor
 * @param[in]  m    Mailbox
 * @retval >=0 mailbox is valid and locally accessible:
 *               0: no monitor / 1: preexisting monitor
 * @retval  -3 no mailbox (MonitorInfo: no fields set)
 * @retval  -2 magic not set
 * @retval  -1 stat() failed (see errno; MonitorInfo fields: magic, isdir, path)
 *
 * If m is NULL, the current mailbox (Context) is used.
 */
static int monitor_resolve(struct MonitorInfo *info, struct Mailbox *m)
{
  char *fmt = NULL;
  struct stat sb;

  if (m)
  {
    info->magic = m->magic;
    info->path = m->realpath;
  }
  else if (Context && Context->mailbox)
  {
    info->magic = Context->mailbox->magic;
    info->path = Context->mailbox->realpath;
  }
  else
  {
    return RESOLVERES_FAIL_NOMAILBOX;
  }

  if (info->magic == MUTT_UNKNOWN)
  {
    return RESOLVERES_FAIL_NOMAGIC;
  }
  else if (info->magic == MUTT_MAILDIR)
  {
    info->isdir = 1;
    fmt = "%s/new";
  }
  else
  {
    info->isdir = 0;
    if (info->magic == MUTT_MH)
      fmt = "%s/.mh_sequences";
  }

  if (fmt)
  {
    if (!info->path_buf)
      info->path_buf = mutt_buffer_new();
    mutt_buffer_printf(info->path_buf, fmt, info->path);
    info->path = mutt_b2s(info->path_buf);
  }
  if (stat(info->path, &sb) != 0)
    return RESOLVERES_FAIL_STAT;

  struct Monitor *iter = Monitor;
  while (iter && ((iter->st_ino != sb.st_ino) || (iter->st_dev != sb.st_dev)))
    iter = iter->next;

  info->st_dev = sb.st_dev;
  info->st_ino = sb.st_ino;
  info->monitor = iter;

  return iter ? RESOLVERES_OK_EXISTING : RESOLVERES_OK_NOTEXISTING;
}

/**
 * mutt_monitor_poll - Check for filesystem changes
 * @retval -3 unknown/unexpected events: poll timeout / fds not handled by us
 * @retval -2 monitor detected changes, no STDIN input
 * @retval -1 error (see errno)
 * @retval  0 (1) input ready from STDIN, or (2) monitoring inactive -> no poll()
 *
 * Wait for I/O ready file descriptors or signals.
 *
 * MonitorFilesChanged also reflects changes to monitored files.
 *
 * Only STDIN and INotify file handles currently expected/supported.
 * More would ask for common infrastructure (sockets?).
 */
int mutt_monitor_poll(void)
{
  int rc = 0;
  char buf[EVENT_BUFLEN] __attribute__((aligned(__alignof__(struct inotify_event))));

  MonitorFilesChanged = 0;

  if (INotifyFd != -1)
  {
    int fds = poll(PollFds, PollFdsLen, MuttGetchTimeout);

    if (fds == -1)
    {
      rc = -1;
      if (errno != EINTR)
      {
        mutt_debug(LL_DEBUG2, "poll() failed, errno=%d %s\n", errno, strerror(errno));
      }
    }
    else
    {
      bool input_ready = false;
      for (int i = 0; fds && (i < PollFdsCount); i++)
      {
        if (PollFds[i].revents)
        {
          fds--;
          if (PollFds[i].fd == 0)
          {
            input_ready = true;
          }
          else if (PollFds[i].fd == INotifyFd)
          {
            MonitorFilesChanged = 1;
            mutt_debug(LL_DEBUG3, "file change(s) detected\n");
            char *ptr = buf;
            const struct inotify_event *event = NULL;

            while (true)
            {
              int len = read(INotifyFd, buf, sizeof(buf));
              if (len == -1)
              {
                if (errno != EAGAIN)
                  mutt_debug(LL_DEBUG2, "read inotify events failed, errno=%d %s\n",
                             errno, strerror(errno));
                break;
              }

              while (ptr < (buf + len))
              {
                event = (const struct inotify_event *) ptr;
                mutt_debug(LL_DEBUG3, "+ detail: descriptor=%d mask=0x%x\n",
                           event->wd, event->mask);
                if (event->mask & IN_IGNORED)
                  monitor_handle_ignore(event->wd);
                else if (event->wd == MonitorContextDescriptor)
                  MonitorContextChanged = 1;
                ptr += sizeof(struct inotify_event) + event->len;
              }
            }
          }
        }
      }
      if (!input_ready)
        rc = MonitorFilesChanged ? -2 : -3;
    }
  }

  return rc;
}

/**
 * mutt_monitor_add - Add a watch for a mailbox
 * @param m Mailbox to watch
 * @retval  0 success: new or already existing monitor
 * @retval -1 failed:  no mailbox, inaccessible file, create monitor/watcher failed
 *
 * If mailbox is NULL, the current mailbox (Context) is used.
 */
int mutt_monitor_add(struct Mailbox *m)
{
  struct MonitorInfo info;
  monitor_info_init(&info);

  int rc = 0;
  int desc = monitor_resolve(&info, m);
  if (desc != RESOLVERES_OK_NOTEXISTING)
  {
    if (!m && (desc == RESOLVERES_OK_EXISTING))
      MonitorContextDescriptor = info.monitor->desc;
    rc = (desc == RESOLVERES_OK_EXISTING) ? 0 : -1;
    goto cleanup;
  }

  uint32_t mask = info.isdir ? INOTIFY_MASK_DIR : INOTIFY_MASK_FILE;
  if (((INotifyFd == -1) && (monitor_init() == -1)) ||
      ((desc = inotify_add_watch(INotifyFd, info.path, mask)) == -1))
  {
    mutt_debug(LL_DEBUG2, "inotify_add_watch failed for '%s', errno=%d %s\n",
               info.path, errno, strerror(errno));
    rc = -1;
    goto cleanup;
  }

  mutt_debug(LL_DEBUG3, "inotify_add_watch descriptor=%d for '%s'\n", desc, info.path);
  if (!m)
    MonitorContextDescriptor = desc;

  monitor_new(&info, desc);

cleanup:
  monitor_info_free(&info);
  return rc;
}

/**
 * mutt_monitor_remove - Remove a watch for a mailbox
 * @param m Mailbox
 * @retval 0 monitor removed (not shared)
 * @retval 1 monitor not removed (shared)
 * @retval 2 no monitor
 *
 * If mailbox is NULL, the current mailbox (Context) is used.
 */
int mutt_monitor_remove(struct Mailbox *m)
{
  struct MonitorInfo info, info2;
  int rc = 0;

  monitor_info_init(&info);
  monitor_info_init(&info2);

  if (!m)
  {
    MonitorContextDescriptor = -1;
    MonitorContextChanged = 0;
  }

  if (monitor_resolve(&info, m) != RESOLVERES_OK_EXISTING)
  {
    rc = 2;
    goto cleanup;
  }

  if (Context && Context->mailbox)
  {
    if (m)
    {
      if ((monitor_resolve(&info2, NULL) == RESOLVERES_OK_EXISTING) &&
          (info.st_ino == info2.st_ino) && (info.st_dev == info2.st_dev))
      {
        rc = 1;
        goto cleanup;
      }
    }
    else
    {
      if (mutt_mailbox_find(Context->mailbox->realpath))
      {
        rc = 1;
        goto cleanup;
      }
    }
  }

  inotify_rm_watch(info.monitor->desc, INotifyFd);
  mutt_debug(LL_DEBUG3, "inotify_rm_watch for '%s' descriptor=%d\n", info.path,
             info.monitor->desc);

  monitor_delete(info.monitor);
  monitor_check_free();

cleanup:
  monitor_info_free(&info);
  monitor_info_free(&info2);
  return rc;
}
