/* doh-proxy.c — minimal DNS-over-HTTPS to UDP DNS proxy */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <curl/curl.h>

#define DOH_URL "https://cloudflare-dns.com/dns-query"
#define LISTEN_PORT 53
#define BUF_SIZE 4096

struct buf { char *data; size_t len; };

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *user) {
    struct buf *b = user;
    size_t total = size * nmemb;
    b->data = realloc(b->data, b->len + total + 1);
    memcpy(b->data + b->len, ptr, total);
    b->len += total;
    b->data[b->len] = 0;
    return total;
}

int main(void) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("doh-proxy: socket");
        return 1;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = htons(LISTEN_PORT)};
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("doh-proxy: bind 127.0.0.1:53");
        close(sock);
        return 1;
    }

    curl_global_init(CURL_GLOBAL_ALL);

    CURL *c = curl_easy_init();
    struct curl_slist *h = NULL;
    struct curl_slist *resolve = NULL;
    if (!c) return 1;

    h = curl_slist_append(h, "Content-Type: application/dns-message");
    h = curl_slist_append(h, "Accept: application/dns-message");
    resolve = curl_slist_append(resolve, "cloudflare-dns.com:443:1.1.1.1");
    curl_easy_setopt(c, CURLOPT_URL, DOH_URL);
    curl_easy_setopt(c, CURLOPT_RESOLVE, resolve);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 2000L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 1000L);

    for (;;) {
        unsigned char qbuf[512], rbuf[BUF_SIZE];
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        ssize_t qlen = recvfrom(sock, qbuf, sizeof(qbuf), 0, (struct sockaddr *)&client, &clen);
        if (qlen < 12) continue;

        struct buf response = {0};
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, qbuf);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)qlen);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);

        if (curl_easy_perform(c) == CURLE_OK && response.len > 0) {
            sendto(sock, response.data, response.len, 0, (struct sockaddr *)&client, clen);
        }
        free(response.data);
    }

    curl_slist_free_all(h);
    curl_slist_free_all(resolve);
    curl_easy_cleanup(c);
}
