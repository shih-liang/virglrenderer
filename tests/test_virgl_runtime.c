/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include "virgl_util.h"
#include "util/anon_file.h"

#define CHECK(expr) do { if (!(expr)) { \
   fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #expr, errno); \
   exit(1); \
} } while (0)

static int transfer_fd(int fd)
{
   int pair[2];
   CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
   char byte = 0;
   struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
   union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control;
   memset(&control, 0, sizeof(control));
   struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
      .msg_control = &control, .msg_controllen = sizeof(control) };
   struct cmsghdr *header = CMSG_FIRSTHDR(&msg);
   header->cmsg_level = SOL_SOCKET;
   header->cmsg_type = SCM_RIGHTS;
   header->cmsg_len = CMSG_LEN(sizeof(fd));
   memcpy(CMSG_DATA(header), &fd, sizeof(fd));
   CHECK(sendmsg(pair[0], &msg, 0) == 1);
   memset(&control, 0, sizeof(control));
   CHECK(recvmsg(pair[1], &msg, 0) == 1);
   header = CMSG_FIRSTHDR(&msg);
   CHECK(header && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS);
   int received;
   memcpy(&received, CMSG_DATA(header), sizeof(received));
   close(pair[0]);
   close(pair[1]);
   return received;
}

int main(void)
{
   char runtime[] = "/tmp/vg-XXXXXX";
   CHECK(mkdtemp(runtime));
   CHECK(setenv("XDG_RUNTIME_DIR", runtime, 1) == 0);
   CHECK(setenv("TMPDIR", runtime, 1) == 0);
   CHECK(has_eventfd());
   int fd = create_eventfd(0);
   CHECK(fd >= 0);
   CHECK(fcntl(fd, F_GETFL) & O_NONBLOCK);
   CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
   struct pollfd event = { .fd = fd, .events = POLLIN };
   CHECK(poll(&event, 1, 0) == 0);
   int received = transfer_fd(fd);
   CHECK(write_eventfd(received, 1) == 0);
   CHECK(poll(&event, 1, 100) == 1 && (event.revents & POLLIN));
   flush_eventfd(fd);
   CHECK(poll(&event, 1, 0) == 0);
   close(received);
   close(fd);
   fd = create_eventfd(1);
   CHECK(fd >= 0);
   event.fd = fd;
   CHECK(poll(&event, 1, 0) == 1);
   flush_eventfd(fd);
   CHECK(poll(&event, 1, 0) == 0);
   close(fd);

   CHECK(unsetenv("XDG_RUNTIME_DIR") == 0);
   fd = os_create_anonymous_file(16384, "runtime-test");
   CHECK(fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
   char *shared = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   CHECK(shared != MAP_FAILED);
   received = transfer_fd(fd);
   char *peer = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_SHARED, received, 0);
   CHECK(peer != MAP_FAILED);
   shared[16383] = 42;
   CHECK(peer[16383] == 42);
   munmap(peer, 16384);
   munmap(shared, 16384);
   close(received);
   close(fd);

#ifdef __APPLE__
   CHECK(setenv("XDG_RUNTIME_DIR", "/nonexistent/virgl-runtime-test", 1) == 0);
   CHECK(create_eventfd(0) < 0 && errno == ENOENT);
   char overlong[256];
   memset(overlong, 'a', sizeof(overlong) - 1);
   overlong[sizeof(overlong) - 1] = 0;
   CHECK(setenv("XDG_RUNTIME_DIR", overlong, 1) == 0);
   CHECK(create_eventfd(0) < 0 && errno == ENAMETOOLONG);
   CHECK(unsetenv("XDG_RUNTIME_DIR") == 0);
   CHECK(setenv("TMPDIR", "/nonexistent/virgl-runtime-test", 1) == 0);
   CHECK(create_eventfd(0) < 0 && errno == ENOENT);
   CHECK(os_create_anonymous_file(16384, "runtime-test") < 0 && errno == ENOENT);
#endif
   CHECK(rmdir(runtime) == 0);
   puts("PASS: runtime directory, eventfd SCM_RIGHTS, shared mmap, and failure paths");
   return 0;
}
