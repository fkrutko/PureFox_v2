/* rinetd.c — TCP forwarder with timeout */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>

#define MAX_CLIENTS 32
#define BUFSZ 8192

static void die(const char *s) { perror(s); exit(1); }

struct pair { int client, remote; };

static int fd_ok(int fd) { return fd > 0 && fcntl(fd, F_GETFD) >= 0; }

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "Usage: rinetd local_ip local_port remote_ip [remote_port]\n"); return 1; }
    signal(SIGPIPE, SIG_IGN);

    int server = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in laddr = {.sin_family = AF_INET, .sin_port = htons(atoi(argv[2]))};
    inet_pton(AF_INET, argv[1], &laddr.sin_addr);
    if (bind(server, (struct sockaddr *)&laddr, sizeof(laddr)) < 0) die("bind");
    if (listen(server, 5) < 0) die("listen");

    char *rip = argv[3];
    int rport = argc > 4 ? atoi(argv[4]) : 443;

    struct pair pairs[MAX_CLIENTS];
    memset(pairs, 0, sizeof(pairs));

    for (;;) {
        fd_set rfds;
        int maxfd = server;
        FD_ZERO(&rfds);
        FD_SET(server, &rfds);

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!fd_ok(pairs[i].client)) { pairs[i].client = 0; pairs[i].remote = 0; }
            if (pairs[i].client) {
                FD_SET(pairs[i].client, &rfds);
                if (pairs[i].client > maxfd) maxfd = pairs[i].client;
            }
            if (fd_ok(pairs[i].remote)) {
                FD_SET(pairs[i].remote, &rfds);
                if (pairs[i].remote > maxfd) maxfd = pairs[i].remote;
            }
        }

        struct timeval tv = {30, 0};
        int n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) { if (errno == EINTR) continue; die("select"); }

        if (FD_ISSET(server, &rfds)) {
            int client = accept(server, NULL, NULL);
            if (client >= 0) {
                struct sockaddr_in raddr = {.sin_family = AF_INET, .sin_port = htons(rport)};
                inet_pton(AF_INET, rip, &raddr.sin_addr);
                int remote = socket(AF_INET, SOCK_STREAM, 0);
                if (remote >= 0) { struct timeval ct = {5,0}; setsockopt(remote, SOL_SOCKET, SO_SNDTIMEO, &ct, sizeof(ct)); }
                if (remote < 0 || connect(remote, (struct sockaddr *)&raddr, sizeof(raddr)) < 0) {
                    close(client); if (remote >= 0) close(remote);
                } else {
                    int placed = 0;
                    for (int i = 0; i < MAX_CLIENTS; i++) {
                        if (pairs[i].client == 0) { pairs[i].client = client; pairs[i].remote = remote; placed = 1; break; }
                    }
                    if (!placed) { close(client); close(remote); }
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!pairs[i].client) continue;
            char buf[BUFSZ];
            if (FD_ISSET(pairs[i].client, &rfds)) {
                ssize_t r = read(pairs[i].client, buf, BUFSZ);
                if (r <= 0 || !fd_ok(pairs[i].remote) || write(pairs[i].remote, buf, r) < 0) {
                    close(pairs[i].client); close(pairs[i].remote); pairs[i].client = 0;
                }
            }
            if (pairs[i].client && FD_ISSET(pairs[i].remote, &rfds)) {
                ssize_t r = read(pairs[i].remote, buf, BUFSZ);
                if (r <= 0 || write(pairs[i].client, buf, r) < 0) {
                    close(pairs[i].client); close(pairs[i].remote); pairs[i].client = 0;
                }
            }
        }
    }
}
