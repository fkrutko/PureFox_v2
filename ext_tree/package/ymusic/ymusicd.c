/*
 * ymusicd — Яндекс Музыка для PureFox (из DigiFox).
 *
 * Маленький HTTP-сервер на 127.0.0.1:6601 (только локально):
 *
 *   GET|POST /api/<путь>?<запрос>
 *       Пересылает запрос в https://api.music.yandex.net/<путь>?<запрос> с токеном
 *       из файла (-t). Тело POST и его Content-Type передаются как есть.
 *       Нужен веб-интерфейсу: в PHP Фокса нет HTTPS, а здесь есть libcurl + OpenSSL.
 *
 *   GET|HEAD /track/<id>.<flac|mp3>[?a=<альбом>&w=<сессия волны>&b=<партия>&s=<сид>]
 *       Аудиопоток трека для MPD. Ссылка берётся из /get-file-info (подпись HMAC-SHA256
 *       ключом десктопного приложения), поток в транспорте encraw расшифровывается
 *       на лету (AES-128-CTR, ключ из ответа, счётчик с нуля). Range поддерживается
 *       (перемотка в MPD), обрыв со стороны CDN — докачка с того же места.
 *       Content-Type: audio/flac | audio/mpeg — по нему MPD выбирает декодер.
 *       Для треков «Моей волны» (w=...) — отзыв trackStarted и подкачка следующей
 *       партии в очередь MPD, когда до конца очереди остаётся меньше 3 треков.
 *
 *   GET /status  -> {"ok":true,"token":true|false,"quality":"lossless"}
 *
 * Сборка:  cc -O2 -o ymusicd ymusicd.c cJSON.c -lcurl -lcrypto -lpthread
 * GPL-2.0+, перенесено из DigiFox (github.com/DrModd/DigiFox).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "cJSON.h"

#define YM_VERSION      "1.0"
#define SIGN_KEY        "kzqU4XhfCaY6B6JTHODeq5"            /* ключ десктопного клиента */
#define DESKTOP_CLIENT  "YandexMusicDesktopAppWindows/5.95.0" /* к нему привязан ключ */
#define ANDROID_CLIENT  "YandexMusicAndroid/24023621"
#define USER_AGENT      "PureFox-ymusicd/" YM_VERSION
#define MAX_CLIENTS     24
#define INFO_TTL        300                                   /* с, кэш ссылок на файлы */
#define WAVE_LOW        3                                     /* подкачка, если осталось < 3 */

/* ---------- настройки ---------- */
static const char *g_listen  = "127.0.0.1";
static int         g_port    = 6601;
static const char *g_token_f = "/var/lib/ymusic/token";
static const char *g_qual_f  = "/var/lib/ymusic/quality";
static const char *g_api     = "https://api.music.yandex.net";
static const char *g_mpd     = "127.0.0.1";
static int         g_mpd_port = 6600;
static const char *g_cover_d = "/tmp/ymusic";
static const char *g_transport = "encraw";
static int         g_verbose = 0;

static volatile int g_clients = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_wave_lock = PTHREAD_MUTEX_INITIALIZER;

static void logm(int verbose_only, const char *fmt, ...)
{
    if (verbose_only && !g_verbose) return;
    char ts[32];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%H:%M:%S", &tm);
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s ymusicd: ", ts);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ---------- мелочи ---------- */
static int read_small_file(const char *path, char *buf, size_t sz)
{
    FILE *f = fopen(path, "r");
    if (!f) { buf[0] = 0; return -1; }
    size_t n = fread(buf, 1, sz - 1, f);
    fclose(f);
    buf[n] = 0;
    while (n && isspace((unsigned char)buf[n - 1])) buf[--n] = 0;
    size_t s = 0;
    while (buf[s] && isspace((unsigned char)buf[s])) s++;
    if (s) memmove(buf, buf + s, n - s + 1);
    return 0;
}

static void get_token(char *tok, size_t sz)
{
    read_small_file(g_token_f, tok, sz);
    for (char *p = tok; *p; p++)                 /* токен — одна строка без пробелов */
        if (isspace((unsigned char)*p)) { *p = 0; break; }
}

static const char *get_quality(char *buf, size_t sz)
{
    read_small_file(g_qual_f, buf, sz);
    if (strcmp(buf, "hq") && strcmp(buf, "nq")) strcpy(buf, "lossless");
    return buf;
}

typedef struct { char *p; size_t n, cap; } buf_t;

static int buf_add(buf_t *b, const void *d, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->n + n + 1) c *= 2;
        if (c > 32u * 1024 * 1024) return -1;   /* ответ API больше 32 МБ — что-то не так */
        char *np = realloc(b->p, c);
        if (!np) return -1;
        b->p = np; b->cap = c;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
    b->p[b->n] = 0;
    return 0;
}

static size_t cb_mem(char *d, size_t s, size_t n, void *u)
{
    return buf_add((buf_t *)u, d, s * n) ? 0 : s * n;
}

static int send_all(int fd, const void *d, size_t n)
{
    const char *p = d;
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int sendf(int fd, const char *fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n >= sizeof tmp) n = sizeof tmp - 1;
    return send_all(fd, tmp, (size_t)n);
}

static void reply(int fd, int code, const char *ctype, const char *body, size_t len)
{
    const char *txt = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 404 ? "Not Found" :
                      code == 401 ? "Unauthorized" : code == 502 ? "Bad Gateway" : "Error";
    sendf(fd, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
              "Cache-Control: no-store\r\nConnection: close\r\n\r\n", code, txt, ctype, len);
    if (len) send_all(fd, body, len);
}

static void reply_json_err(int fd, int code, const char *msg)
{
    char b[512];
    int n = snprintf(b, sizeof b, "{\"error\":{\"name\":\"ymusicd\",\"message\":\"%s\"}}", msg);
    reply(fd, code, "application/json; charset=utf-8", b, (size_t)n);
}

static char *url_esc(const char *s)
{
    CURL *c = curl_easy_init();
    char *e = c ? curl_easy_escape(c, s, 0) : NULL;
    char *r = e ? strdup(e) : strdup("");
    if (e) curl_free(e);
    if (c) curl_easy_cleanup(c);
    return r;
}

/* значение параметра запроса (?a=1&b=2), декодированное; 0 — нет такого */
static int query_get(const char *q, const char *key, char *out, size_t sz)
{
    size_t kl = strlen(key);
    out[0] = 0;
    if (!q) return 0;
    const char *p = q;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            size_t j = 0;
            for (size_t i = kl + 1; i < seg && j + 1 < sz; i++) {
                char c = p[i];
                if (c == '+') c = ' ';
                else if (c == '%' && i + 2 < seg + 1 && isxdigit((unsigned char)p[i + 1]) &&
                         isxdigit((unsigned char)p[i + 2])) {
                    char h[3] = { p[i + 1], p[i + 2], 0 };
                    c = (char)strtol(h, NULL, 16);
                    i += 2;
                }
                out[j++] = c;
            }
            out[j] = 0;
            return 1;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return 0;
}

static void iso_now(char *b, size_t sz)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    size_t n = strftime(b, sz, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(b + n, sz - n, ".%03ldZ", ts.tv_nsec / 1000000);
}

/* ---------- запросы к API ---------- */
static struct curl_slist *api_headers(const char *client, const char *ctype)
{
    char tok[512], h[600];
    struct curl_slist *hs = NULL;
    get_token(tok, sizeof tok);
    if (tok[0]) {
        snprintf(h, sizeof h, "Authorization: OAuth %s", tok);
        hs = curl_slist_append(hs, h);
    }
    snprintf(h, sizeof h, "X-Yandex-Music-Client: %s", client);
    hs = curl_slist_append(hs, h);
    hs = curl_slist_append(hs, "Accept-Language: ru");
    hs = curl_slist_append(hs, "Accept: application/json");
    if (ctype && *ctype) {
        snprintf(h, sizeof h, "Content-Type: %s", ctype);
        hs = curl_slist_append(hs, h);
    }
    hs = curl_slist_append(hs, "Expect:");
    return hs;
}

/* method: "GET"/"POST"; path_q начинается с "/" */
static int api_call(const char *method, const char *path_q, const char *client,
                    const char *ctype, const char *body, size_t blen, buf_t *out, long *status)
{
    char url[8800];
    snprintf(url, sizeof url, "%s%s", g_api, path_q);
    CURL *c = curl_easy_init();
    if (!c) return -1;
    struct curl_slist *hs = api_headers(client, ctype);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hs);
    curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cb_mem);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    if (!strcmp(method, "POST")) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body ? body : "");
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)(body ? blen : 0));
    }
    CURLcode rc = curl_easy_perform(c);
    *status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, status);
    curl_slist_free_all(hs);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        logm(0, "API %s %s: %s", method, path_q, curl_easy_strerror(rc));
        return -1;
    }
    logm(1, "API %s %s -> %ld (%zu B)", method, path_q, *status, out->n);
    return 0;
}

static cJSON *api_json(const char *method, const char *path_q, const char *client, const char *jbody)
{
    buf_t b = {0};
    long st = 0;
    cJSON *j = NULL;
    if (api_call(method, path_q, client, jbody ? "application/json" : NULL,
                 jbody, jbody ? strlen(jbody) : 0, &b, &st) == 0 && b.p)
        j = cJSON_Parse(b.p);
    if (j && st >= 400) logm(0, "API %s %s -> HTTP %ld", method, path_q, st);
    free(b.p);
    return j;
}

/* ---------- ссылка на файл трека ---------- */
typedef struct {
    char tid[32], quality[12], codec[16], transport[12], key[72];
    char url[2048];
    time_t t;
} finfo_t;

static finfo_t g_cache[24];

static int b64(const unsigned char *in, int n, char *out, size_t sz)
{
    if ((size_t)(4 * ((n + 2) / 3) + 1) > sz) return -1;
    return EVP_EncodeBlock((unsigned char *)out, in, n);
}

static void cache_drop(const char *tid)
{
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < sizeof g_cache / sizeof g_cache[0]; i++)
        if (!strcmp(g_cache[i].tid, tid)) g_cache[i].tid[0] = 0;
    pthread_mutex_unlock(&g_lock);
}

static int file_info(const char *tid, finfo_t *fi, char *err, size_t esz)
{
    char qbuf[16];
    const char *q = get_quality(qbuf, sizeof qbuf);
    time_t now = time(NULL);

    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < sizeof g_cache / sizeof g_cache[0]; i++)
        if (!strcmp(g_cache[i].tid, tid) && !strcmp(g_cache[i].quality, q) &&
            now - g_cache[i].t < INFO_TTL) {
            *fi = g_cache[i];
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
    pthread_mutex_unlock(&g_lock);

    /* MPD без ffmpeg не открывает MP4-контейнер, поэтому только flac и mp3 */
    const char *codecs = !strcmp(q, "lossless") ? "flac,mp3" : "mp3";
    char codecs_nc[32], msg[256], sig[64];
    size_t j = 0;
    for (const char *p = codecs; *p && j + 1 < sizeof codecs_nc; p++)
        if (*p != ',') codecs_nc[j++] = *p;
    codecs_nc[j] = 0;

    long ts = (long)now;
    snprintf(msg, sizeof msg, "%ld%s%s%s%s", ts, tid, q, codecs_nc, g_transport);
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int ml = 0;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    HMAC(EVP_sha256(), SIGN_KEY, (int)strlen(SIGN_KEY), (unsigned char *)msg, strlen(msg), mac, &ml);
#pragma GCC diagnostic pop
    int sl = b64(mac, (int)ml, sig, sizeof sig);
    if (sl > 0 && sig[sl - 1] == '=') sig[sl - 1] = 0;  /* как в официальном клиенте: без последнего символа */

    char *esig = url_esc(sig);
    char path[512];
    snprintf(path, sizeof path, "/get-file-info?ts=%ld&trackId=%s&quality=%s&codecs=%s&transports=%s&sign=%s",
             ts, tid, q, codecs, g_transport, esig);
    free(esig);

    cJSON *r = api_json("GET", path, DESKTOP_CLIENT, NULL);
    if (!r) { snprintf(err, esz, "нет ответа от API"); return -1; }
    cJSON *di = cJSON_GetObjectItem(cJSON_GetObjectItem(r, "result"), "downloadInfo");
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(di, "url"));
    if (!url) {
        cJSON *urls = cJSON_GetObjectItem(di, "urls");
        url = cJSON_GetStringValue(cJSON_GetArrayItem(urls, 0));
    }
    if (!url) {
        const char *m = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(r, "error"), "message"));
        const char *n = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(r, "error"), "name"));
        snprintf(err, esz, "get-file-info: %s", m ? m : n ? n : "нет ссылки (подписка? токен?)");
        cJSON_Delete(r);
        return -1;
    }
    memset(fi, 0, sizeof *fi);
    snprintf(fi->tid, sizeof fi->tid, "%s", tid);
    snprintf(fi->quality, sizeof fi->quality, "%s", q);
    snprintf(fi->url, sizeof fi->url, "%s", url);
    const char *s;
    if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(di, "codec"))))     snprintf(fi->codec, sizeof fi->codec, "%s", s);
    if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(di, "transport")))) snprintf(fi->transport, sizeof fi->transport, "%s", s);
    if ((s = cJSON_GetStringValue(cJSON_GetObjectItem(di, "key"))))       snprintf(fi->key, sizeof fi->key, "%s", s);
    fi->t = now;
    cJSON_Delete(r);
    logm(1, "трек %s: %s %s%s", tid, fi->codec, fi->transport, fi->key[0] ? " (шифр)" : "");

    pthread_mutex_lock(&g_lock);
    size_t oldest = 0;
    for (size_t i = 0; i < sizeof g_cache / sizeof g_cache[0]; i++) {
        if (!g_cache[i].tid[0]) { oldest = i; break; }
        if (g_cache[i].t < g_cache[oldest].t) oldest = i;
    }
    g_cache[oldest] = *fi;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* ---------- поток: CDN -> (расшифровка) -> MPD ---------- */
typedef struct {
    int fd, head_only, headers_sent, failed, cut;
    long status;                 /* статус текущего ответа CDN */
    long long cr_start;          /* начало из Content-Range */
    long long clen;              /* Content-Length текущего ответа */
    long long total;             /* полный размер файла, если известен */
    long long req_start, req_end;/* что просил клиент */
    long long sent;              /* сколько тела отдано клиенту */
    int ranged;                  /* клиент просил Range */
    long long fetch_from;        /* с какого смещения просили CDN в этом запросе */
    int fetch_started;           /* первый кусок тела этого запроса уже был */
    long long skip;              /* CDN не выполнил Range: столько байт выбросить */
    const char *mime;
    EVP_CIPHER_CTX *ctx;
    unsigned char key[16];
    int enc;
} stream_t;

static int ctr_init(stream_t *s, long long offset)
{
    if (!s->enc) return 0;
    unsigned char iv[16] = {0};
    unsigned long long blk = (unsigned long long)offset / 16;
    for (int i = 15; i >= 8 && blk; i--, blk >>= 8) iv[i] = (unsigned char)(blk & 0xff);
    if (!s->ctx) s->ctx = EVP_CIPHER_CTX_new();
    if (!s->ctx || !EVP_DecryptInit_ex(s->ctx, EVP_aes_128_ctr(), NULL, s->key, iv)) return -1;
    int skip = (int)(offset % 16), ol;
    unsigned char z[16] = {0}, o[32];
    if (skip && !EVP_DecryptUpdate(s->ctx, o, &ol, z, skip)) return -1;
    return 0;
}

static size_t cb_hdr(char *d, size_t sz, size_t n, void *u)
{
    stream_t *s = u;
    size_t len = sz * n;
    if (len >= 5 && !strncasecmp(d, "HTTP/", 5)) {        /* новый ответ (в т.ч. после редиректа) */
        s->status = 0; s->cr_start = 0; s->clen = -1;
        const char *sp = memchr(d, ' ', len);
        if (sp) s->status = strtol(sp + 1, NULL, 10);
    } else if (len > 15 && !strncasecmp(d, "Content-Length:", 15)) {
        s->clen = strtoll(d + 15, NULL, 10);
    } else if (len > 14 && !strncasecmp(d, "Content-Range:", 14)) {
        /* Content-Range: bytes 100-999/1000 */
        const char *p = d + 14;
        while (p < d + len && !isdigit((unsigned char)*p)) p++;
        s->cr_start = strtoll(p, NULL, 10);
        const char *sl = memchr(d, '/', len);
        if (sl && isdigit((unsigned char)sl[1])) s->total = strtoll(sl + 1, NULL, 10);
    }
    return len;
}

static int send_head(stream_t *s)
{
    if (s->headers_sent) return 0;
    s->headers_sent = 1;
    if (s->status == 200 && s->clen >= 0 && s->total < 0) s->total = s->clen;
    long long tot = s->total;
    if (s->ranged && tot > 0) {
        long long a = s->req_start, b = s->req_end >= 0 && s->req_end < tot ? s->req_end : tot - 1;
        if (a >= tot)
            return sendf(s->fd, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%lld\r\n"
                                "Connection: close\r\n\r\n", tot), -1;
        return sendf(s->fd, "HTTP/1.1 206 Partial Content\r\nContent-Type: %s\r\nAccept-Ranges: bytes\r\n"
                            "Content-Range: bytes %lld-%lld/%lld\r\nContent-Length: %lld\r\n"
                            "Connection: close\r\n\r\n", s->mime, a, b, tot, b - a + 1);
    }
    if (tot > 0)
        return sendf(s->fd, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nAccept-Ranges: bytes\r\n"
                            "Content-Length: %lld\r\nConnection: close\r\n\r\n", s->mime, tot);
    return sendf(s->fd, "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nConnection: close\r\n\r\n", s->mime);
}

static size_t cb_body(char *d, size_t sz, size_t n, void *u)
{
    stream_t *s = u;
    size_t len = sz * n;
    if (s->status >= 400 || (s->status != 200 && s->status != 206)) { s->failed = 1; return 0; }
    if (!s->headers_sent) {
        if (send_head(s)) { s->cut = 1; return 0; }
        if (s->head_only) { s->cut = 1; return 0; }
    }
    if (!s->fetch_started) {
        /* откуда на самом деле начались данные: 206 — из Content-Range, 200 — с нуля */
        s->fetch_started = 1;
        long long actual = s->status == 206 ? s->cr_start : 0;
        if (actual != s->fetch_from) {
            if (actual > s->fetch_from || ctr_init(s, actual)) { s->failed = 1; return 0; }
            s->skip = s->fetch_from - actual;
        }
    }
    unsigned char out[65536 + 32];
    size_t off = 0;
    while (off < len) {
        size_t in = len - off > 65536 ? 65536 : len - off, chunk = in;
        const unsigned char *src = (unsigned char *)d + off;
        if (s->enc) {
            int ol = 0;
            if (!EVP_DecryptUpdate(s->ctx, out, &ol, src, (int)chunk)) { s->failed = 1; return 0; }
            src = out; chunk = (size_t)ol;
        }
        if (s->skip > 0) {
            size_t d0 = s->skip > (long long)chunk ? chunk : (size_t)s->skip;
            s->skip -= (long long)d0; src += d0; chunk -= d0;
        }
        if (chunk && send_all(s->fd, src, chunk)) { s->cut = 1; return 0; }   /* MPD закрыл соединение */
        s->sent += (long long)chunk;
        off += in;
    }
    return len;
}

/* один запрос к CDN с позиции from (абсолютное смещение в файле); 0 — дочитано до конца */
static int cdn_fetch(stream_t *s, const finfo_t *fi, long long from)
{
    CURL *c = curl_easy_init();
    if (!c) return -1;
    char range[64];
    s->status = 0; s->clen = -1; s->cr_start = 0;
    s->fetch_from = from; s->fetch_started = 0; s->skip = 0;
    curl_easy_setopt(c, CURLOPT_URL, fi->url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, cb_hdr);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, s);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cb_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, s);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 65536L);
    if (s->head_only && !s->ranged) {
        /* HEAD: первый байт, чтобы узнать размер из Content-Range (HEAD CDN может не уметь) */
        curl_easy_setopt(c, CURLOPT_RANGE, "0-0");
    } else if (from > 0 || s->ranged) {
        if (s->req_end >= 0) snprintf(range, sizeof range, "%lld-%lld", from, s->req_end);
        else snprintf(range, sizeof range, "%lld-", from);
        curl_easy_setopt(c, CURLOPT_RANGE, range);
    }
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (rc == CURLE_OK) return 0;
    if (s->cut) return 1;                                    /* клиент ушёл — не ошибка */
    logm(0, "CDN: %s (HTTP %ld, отдано %lld)", curl_easy_strerror(rc), code, s->sent);
    return -1;
}

static void wave_hook_start(const char *query, const char *tid);

static void serve_track(int fd, const char *method, const char *path, const char *query, const char *range)
{
    /* /track/<id>.<ext> */
    char tid[32] = {0};
    const char *p = path + 7;
    size_t i = 0;
    while (*p && *p != '.' && *p != '/' && i + 1 < sizeof tid) {
        if (!isdigit((unsigned char)*p)) { reply_json_err(fd, 400, "bad track id"); return; }
        tid[i++] = *p++;
    }
    if (!tid[0]) { reply_json_err(fd, 400, "bad track id"); return; }

    stream_t s;
    memset(&s, 0, sizeof s);
    s.fd = fd;
    s.head_only = !strcmp(method, "HEAD");
    s.total = -1;
    s.req_end = -1;
    if (range && !strncasecmp(range, "bytes=", 6)) {
        const char *r = range + 6;
        if (isdigit((unsigned char)*r)) {
            s.ranged = 1;
            s.req_start = strtoll(r, (char **)&r, 10);
            if (*r == '-' && isdigit((unsigned char)r[1])) s.req_end = strtoll(r + 1, NULL, 10);
        }
    }

    /* «Моя волна»: отзыв и подкачка — как только MPD начал трек (не при перемотке) */
    if (!s.head_only && (!s.ranged || s.req_start == 0)) wave_hook_start(query, tid);

    finfo_t fi;
    char err[300];
    for (int attempt = 0; attempt < 2; attempt++) {
        if (file_info(tid, &fi, err, sizeof err)) {
            logm(0, "трек %s: %s", tid, err);
            reply_json_err(fd, 502, err);
            return;
        }
        s.mime = !strcmp(fi.codec, "mp3") ? "audio/mpeg" : !strncmp(fi.codec, "flac", 4) ? "audio/flac"
               : !strcmp(fi.codec, "aac") ? "audio/aac" : "application/octet-stream";
        s.enc = !strcmp(fi.transport, "encraw") && strlen(fi.key) == 32;
        if (s.enc)
            for (int k = 0; k < 16; k++) {
                char h[3] = { fi.key[2 * k], fi.key[2 * k + 1], 0 };
                s.key[k] = (unsigned char)strtol(h, NULL, 16);
            }
        long long start = s.ranged ? s.req_start : 0;
        if (ctr_init(&s, start)) { reply_json_err(fd, 500, "AES init"); return; }
        s.failed = 0;
        int r = cdn_fetch(&s, &fi, start);
        if (s.headers_sent || r == 1) break;
        /* до клиента ещё ничего не ушло: ссылка протухла — взять новую */
        logm(0, "трек %s: CDN HTTP %ld, новая ссылка", tid, s.status);
        cache_drop(tid);
        if (attempt == 1) { reply_json_err(fd, 502, "CDN error"); goto out; }
    }

    if (!s.head_only && s.headers_sent && !s.cut) {
        /* обрыв со стороны CDN: докачать с того же места (до 4 раз) */
        long long base = s.ranged ? s.req_start : 0;
        long long want = s.total > 0 ? (s.req_end >= 0 && s.req_end < s.total ? s.req_end + 1 : s.total) - base : -1;
        for (int retry = 0; retry < 4 && want > 0 && s.sent < want && !s.cut; retry++) {
            long long pos = base + s.sent;
            logm(0, "трек %s: докачка с %lld", tid, pos);
            cache_drop(tid);
            if (file_info(tid, &fi, err, sizeof err)) break;
            if (ctr_init(&s, pos)) break;
            s.ranged = 1;                     /* заголовки уже отправлены, дальше только тело */
            if (cdn_fetch(&s, &fi, pos) == 1) break;
        }
    }
out:
    if (s.ctx) EVP_CIPHER_CTX_free(s.ctx);
}

/* ---------- MPD ---------- */
static int mpd_connect(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)g_mpd_port) };
    inet_pton(AF_INET, g_mpd, &a.sin_addr);
    struct timeval tv = { 5, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); return -1; }
    char b[128];
    ssize_t n = recv(fd, b, sizeof b - 1, 0);                 /* OK MPD x.y.z */
    if (n < 6 || strncmp(b, "OK MPD", 6)) { close(fd); return -1; }
    return fd;
}

/* команда; ответ до OK/ACK в out (может быть NULL). 0 — OK */
static int mpd_cmd(int fd, const char *cmd, buf_t *out)
{
    if (send_all(fd, cmd, strlen(cmd)) || send_all(fd, "\n", 1)) return -1;
    buf_t tmp = {0};
    buf_t *b = out ? out : &tmp;
    char rb[4096];
    int rc = -1;
    for (;;) {
        ssize_t n = recv(fd, rb, sizeof rb, 0);
        if (n <= 0) break;
        if (buf_add(b, rb, (size_t)n)) break;
        /* ответ кончается строкой OK\n или ACK ...\n */
        if (b->n >= 3 && b->p[b->n - 1] == '\n') {
            char *last = b->p + b->n - 1;
            while (last > b->p && last[-1] != '\n') last--;
            if (!strncmp(last, "OK\n", 3)) { rc = 0; break; }
            if (!strncmp(last, "ACK", 3)) { logm(0, "MPD: %s -> %s", cmd, last); break; }
        }
    }
    free(tmp.p);
    return rc;
}

static void mpd_quote(char *dst, size_t sz, const char *src)
{
    size_t j = 0;
    if (sz < 3) { dst[0] = 0; return; }
    dst[j++] = '"';
    for (; *src && j + 3 < sz; src++) {
        if (*src == '"' || *src == '\\') dst[j++] = '\\';
        dst[j++] = (*src == '\n' || *src == '\r') ? ' ' : *src;
    }
    dst[j++] = '"';
    dst[j] = 0;
}

static long kv_long(const char *resp, const char *key)
{
    size_t kl = strlen(key);
    for (const char *p = resp; p && *p; ) {
        if (!strncmp(p, key, kl) && p[kl] == ':' && p[kl + 1] == ' ') return strtol(p + kl + 2, NULL, 10);
        p = strchr(p, '\n');
        if (p) p++;
    }
    return -1;
}

static void write_cover(const char *tid, const char *cover_uri)
{
    if (!cover_uri || !*cover_uri) return;
    char path[512], url[1024];
    mkdir(g_cover_d, 0755);
    snprintf(path, sizeof path, "%s/%s", g_cover_d, tid);
    const char *pp = strstr(cover_uri, "%%");
    if (pp) snprintf(url, sizeof url, "https://%.*s400x400%s", (int)(pp - cover_uri), cover_uri, pp + 2);
    else snprintf(url, sizeof url, "https://%s", cover_uri);
    FILE *f = fopen(path, "w");
    if (f) { fputs(url, f); fclose(f); }
}

/* добавить трек из JSON API в очередь MPD с тегами; wave_q — хвост запроса волны */
static int mpd_add_track(int fd, cJSON *t, const char *wave_q)
{
    cJSON *jid = cJSON_GetObjectItem(t, "id");
    char tid[32];
    if (cJSON_IsString(jid)) snprintf(tid, sizeof tid, "%s", jid->valuestring);
    else if (cJSON_IsNumber(jid)) snprintf(tid, sizeof tid, "%.0f", jid->valuedouble);
    else return -1;
    if (cJSON_IsFalse(cJSON_GetObjectItem(t, "available"))) return -1;

    cJSON *alb = cJSON_GetArrayItem(cJSON_GetObjectItem(t, "albums"), 0);
    char aid[32] = "";
    cJSON *ja = cJSON_GetObjectItem(alb, "id");
    if (cJSON_IsNumber(ja)) snprintf(aid, sizeof aid, "%.0f", ja->valuedouble);
    else if (cJSON_IsString(ja)) snprintf(aid, sizeof aid, "%s", ja->valuestring);

    char artists[512] = "";
    cJSON *ar;
    cJSON_ArrayForEach(ar, cJSON_GetObjectItem(t, "artists")) {
        const char *nm = cJSON_GetStringValue(cJSON_GetObjectItem(ar, "name"));
        if (!nm) continue;
        size_t l = strlen(artists);
        snprintf(artists + l, sizeof artists - l, "%s%s", l ? ", " : "", nm);
    }
    const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(t, "title"));
    const char *ver = cJSON_GetStringValue(cJSON_GetObjectItem(t, "version"));
    const char *album = cJSON_GetStringValue(cJSON_GetObjectItem(alb, "title"));
    char qb[16];
    const char *ext = !strcmp(get_quality(qb, sizeof qb), "lossless") ? "flac" : "mp3";

    char cmd[2048], q[1200], full[700];
    snprintf(cmd, sizeof cmd, "addid \"http://127.0.0.1:%d/track/%s.%s?a=%s%s\"", g_port, tid, ext, aid, wave_q ? wave_q : "");
    buf_t r = {0};
    if (mpd_cmd(fd, cmd, &r)) { free(r.p); return -1; }
    long id = kv_long(r.p, "Id");
    free(r.p);
    if (id < 0) return -1;
    if (title) {
        if (ver && *ver) snprintf(full, sizeof full, "%s (%s)", title, ver);
        else snprintf(full, sizeof full, "%s", title);
        mpd_quote(q, sizeof q, full);
        snprintf(cmd, sizeof cmd, "addtagid %ld Title %s", id, q);
        mpd_cmd(fd, cmd, NULL);
    }
    if (artists[0]) {
        mpd_quote(q, sizeof q, artists);
        snprintf(cmd, sizeof cmd, "addtagid %ld Artist %s", id, q);
        mpd_cmd(fd, cmd, NULL);
    }
    if (album) {
        mpd_quote(q, sizeof q, album);
        snprintf(cmd, sizeof cmd, "addtagid %ld Album %s", id, q);
        mpd_cmd(fd, cmd, NULL);
    }
    write_cover(tid, cJSON_GetStringValue(cJSON_GetObjectItem(t, "coverUri")));
    return 0;
}

/* ---------- Моя волна ---------- */
typedef struct { char sid[160], batch[160], seed[160], tid[32], aid[32]; } wave_t;

static void wave_feedback(const wave_t *w)
{
    char ts[40], path[256];
    iso_now(ts, sizeof ts);
    cJSON *o = cJSON_CreateObject(), *ev = cJSON_AddObjectToObject(o, "event");
    char id[80];
    snprintf(id, sizeof id, "%s%s%s", w->tid, w->aid[0] ? ":" : "", w->aid);
    cJSON_AddStringToObject(ev, "type", "trackStarted");
    cJSON_AddStringToObject(ev, "timestamp", ts);
    cJSON_AddStringToObject(ev, "trackId", id);
    if (w->batch[0]) cJSON_AddStringToObject(o, "batchId", w->batch);
    char *body = cJSON_PrintUnformatted(o);
    char *esid = url_esc(w->sid);
    snprintf(path, sizeof path, "/rotor/session/%s/feedback", esid);
    cJSON *r = api_json("POST", path, ANDROID_CLIENT, body);
    cJSON_Delete(r);
    free(esid); free(body); cJSON_Delete(o);
}

static void *wave_thread(void *arg)
{
    wave_t *w = arg;
    if (w->sid[0]) wave_feedback(w);
    if (pthread_mutex_trylock(&g_wave_lock)) { free(w); return NULL; }

    int fd = mpd_connect();
    if (fd < 0) { logm(0, "волна: нет связи с MPD"); goto done; }
    buf_t st = {0}, pl = {0};
    if (mpd_cmd(fd, "status", &st)) goto close_mpd;
    long len = kv_long(st.p, "playlistlength"), song = kv_long(st.p, "song");
    if (len < 0 || song < 0) goto close_mpd;
    if (len - song - 1 >= WAVE_LOW) goto close_mpd;

    /* что уже в очереди от этой волны — чтобы сервер не повторял */
    cJSON *queue = cJSON_CreateArray();
    if (!mpd_cmd(fd, "playlistinfo", &pl)) {
        for (char *line = pl.p; line && *line; ) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            if (!strncmp(line, "file: ", 6) && strstr(line, "/track/")) {
                char *t = strstr(line, "/track/") + 7, *qm = strchr(line, '?');
                char tid[32] = "", aid[32] = "";
                size_t i = 0;
                while (isdigit((unsigned char)*t) && i + 1 < sizeof tid) tid[i++] = *t++;
                tid[i] = 0;
                if (qm) query_get(qm + 1, "a", aid, sizeof aid);
                if (tid[0]) {
                    char id[80];
                    snprintf(id, sizeof id, "%s%s%s", tid, aid[0] ? ":" : "", aid);
                    cJSON_AddItemToArray(queue, cJSON_CreateString(id));
                }
            }
            line = nl ? nl + 1 : NULL;
        }
    }
    while (cJSON_GetArraySize(queue) > 30) cJSON_DeleteItemFromArray(queue, 0);

    char path[300];
    cJSON *body = cJSON_CreateObject();
    cJSON_AddItemReferenceToObject(body, "queue", queue);
    char *esid = url_esc(w->sid);
    snprintf(path, sizeof path, "/rotor/session/%s/tracks", esid);
    free(esid);
    char *bs = cJSON_PrintUnformatted(body);
    cJSON *r = api_json("POST", path, ANDROID_CLIENT, bs);
    free(bs);
    cJSON *res = cJSON_GetObjectItem(r, "result");
    if (!res || cJSON_IsTrue(cJSON_GetObjectItem(res, "unknownSession")) ||
        !cJSON_GetArraySize(cJSON_GetObjectItem(res, "sequence"))) {
        /* сессия устарела — новая по тому же сиду */
        logm(0, "волна: сессия %s устарела, новая (%s)", w->sid, w->seed[0] ? w->seed : "user:onyourwave");
        cJSON_Delete(r);
        cJSON *nb = cJSON_CreateObject(), *seeds = cJSON_AddArrayToObject(nb, "seeds");
        cJSON_AddItemToArray(seeds, cJSON_CreateString(w->seed[0] ? w->seed : "user:onyourwave"));
        cJSON_AddItemReferenceToObject(nb, "queue", queue);
        cJSON_AddBoolToObject(nb, "includeTracksInResponse", 1);
        bs = cJSON_PrintUnformatted(nb);
        r = api_json("POST", "/rotor/session/new", ANDROID_CLIENT, bs);
        free(bs);
        cJSON_Delete(nb);
        res = cJSON_GetObjectItem(r, "result");
    }
    const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(res, "radioSessionId"));
    const char *batch = cJSON_GetStringValue(cJSON_GetObjectItem(res, "batchId"));
    if (!sid) sid = w->sid;
    char *e1 = url_esc(sid), *e2 = url_esc(batch ? batch : ""), *e3 = url_esc(w->seed);
    char wq[800];
    snprintf(wq, sizeof wq, "&w=%s&b=%s&s=%s", e1, e2, e3);
    free(e1); free(e2); free(e3);
    int added = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(res, "sequence")) {
        cJSON *t = cJSON_GetObjectItem(it, "track");
        if (t && mpd_add_track(fd, t, wq) == 0) added++;
    }
    logm(1, "волна: +%d в очередь", added);
    /* очередь не растёт бесконечно: сыгранное старше 20 треков убрать */
    if (song > 40) {
        char cmd[64];
        snprintf(cmd, sizeof cmd, "delete 0:%ld", song - 20);
        mpd_cmd(fd, cmd, NULL);
    }
    cJSON_Delete(r);
    cJSON_Delete(body);
    cJSON_Delete(queue);
close_mpd:
    free(st.p); free(pl.p);
    close(fd);
done:
    pthread_mutex_unlock(&g_wave_lock);
    free(w);
    return NULL;
}

static void wave_hook_start(const char *query, const char *tid)
{
    char sid[160];
    if (!query || !query_get(query, "w", sid, sizeof sid) || !sid[0]) return;
    wave_t *w = calloc(1, sizeof *w);
    if (!w) return;
    snprintf(w->sid, sizeof w->sid, "%s", sid);
    snprintf(w->tid, sizeof w->tid, "%s", tid);
    query_get(query, "b", w->batch, sizeof w->batch);
    query_get(query, "s", w->seed, sizeof w->seed);
    query_get(query, "a", w->aid, sizeof w->aid);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 256 * 1024);
    if (pthread_create(&th, &at, wave_thread, w)) free(w);
    pthread_attr_destroy(&at);
}

/* ---------- HTTP-сервер ---------- */
static void serve_api(int fd, const char *method, const char *path, const char *query,
                      const char *ctype, const char *body, size_t blen)
{
    char pq[8448];
    snprintf(pq, sizeof pq, "%s%s%s", path + 4, query ? "?" : "", query ? query : "");
    buf_t b = {0};
    long st = 0;
    if (api_call(method, pq, ANDROID_CLIENT, ctype, body, blen, &b, &st)) {
        reply_json_err(fd, 502, "нет связи с api.music.yandex.net");
    } else {
        reply(fd, st ? (int)st : 502, "application/json; charset=utf-8", b.p ? b.p : "", b.n);
    }
    free(b.p);
}

static void *client_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char req[16384];
    size_t n = 0;
    char *hend = NULL;
    struct timeval tv = { 15, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (n + 1 < sizeof req) {
        ssize_t r = recv(fd, req + n, sizeof req - 1 - n, 0);
        if (r <= 0) goto out;
        n += (size_t)r;
        req[n] = 0;
        if ((hend = strstr(req, "\r\n\r\n"))) break;
    }
    if (!hend) goto out;
    *hend = 0;
    char *bodyp = hend + 4;
    size_t have = n - (size_t)(bodyp - req);

    char method[8] = "", target[8192] = "";
    if (sscanf(req, "%7s %8191s", method, target) != 2) goto out;
    char range[128] = "", ctype[128] = "";
    long clen = 0;
    for (char *l = strstr(req, "\r\n"); l; l = strstr(l + 2, "\r\n")) {
        char *h = l + 2;
        if (!strncasecmp(h, "Range:", 6)) sscanf(h + 6, " %127[^\r\n]", range);
        else if (!strncasecmp(h, "Content-Type:", 13)) sscanf(h + 13, " %127[^\r\n]", ctype);
        else if (!strncasecmp(h, "Content-Length:", 15)) clen = strtol(h + 15, NULL, 10);
    }
    char *query = strchr(target, '?');
    if (query) *query++ = 0;
    logm(1, "%s %s%s%s %s", method, target, query ? "?" : "", query ? query : "", range);

    if (!strncmp(target, "/api/", 5)) {
        if (strcmp(method, "GET") && strcmp(method, "POST")) { reply_json_err(fd, 400, "method"); goto out; }
        char *body = NULL;
        if (!strcmp(method, "POST") && clen > 0) {
            if (clen > 1024 * 1024) { reply_json_err(fd, 400, "body too big"); goto out; }
            body = malloc((size_t)clen + 1);
            if (!body) goto out;
            size_t got = have > (size_t)clen ? (size_t)clen : have;
            memcpy(body, bodyp, got);
            while (got < (size_t)clen) {
                ssize_t r = recv(fd, body + got, (size_t)clen - got, 0);
                if (r <= 0) break;
                got += (size_t)r;
            }
            body[got] = 0;
            clen = (long)got;
        }
        serve_api(fd, method, target, query, ctype, body, body ? (size_t)clen : 0);
        free(body);
    } else if (!strncmp(target, "/track/", 7)) {
        if (strcmp(method, "GET") && strcmp(method, "HEAD")) { reply_json_err(fd, 400, "method"); goto out; }
        struct timeval tv2 = { 0, 0 };                    /* поток может идти долго */
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv2, sizeof tv2);
        serve_track(fd, method, target, query, range[0] ? range : NULL);
    } else if (!strcmp(target, "/status")) {
        char tok[512], qb[16], b[160];
        get_token(tok, sizeof tok);
        int l = snprintf(b, sizeof b, "{\"ok\":true,\"version\":\"%s\",\"token\":%s,\"quality\":\"%s\"}",
                         YM_VERSION, tok[0] ? "true" : "false", get_quality(qb, sizeof qb));
        reply(fd, 200, "application/json", b, (size_t)l);
    } else {
        reply_json_err(fd, 404, "not found");
    }
out:
    shutdown(fd, SHUT_RDWR);
    close(fd);
    __sync_fetch_and_sub(&g_clients, 1);
    return NULL;
}

static void usage(void)
{
    fprintf(stderr,
        "ymusicd " YM_VERSION " — Яндекс Музыка для MPD\n"
        "  -l адрес     слушать (127.0.0.1)\n"
        "  -p порт      (6601)\n"
        "  -t файл      токен OAuth (/var/lib/ymusic/token)\n"
        "  -q файл      качество lossless|hq|nq (/var/lib/ymusic/quality)\n"
        "  -m хост:порт MPD (127.0.0.1:6600)\n"
        "  -c папка     обложки текущих треков (/tmp/ymusic)\n"
        "  -a URL       адрес API (https://api.music.yandex.net)\n"
        "  -T raw|encraw транспорт (encraw)\n"
        "  -v           подробный журнал\n");
}

int main(int argc, char **argv)
{
    int o;
    static char mpdhost[64];
    while ((o = getopt(argc, argv, "l:p:t:q:m:c:a:T:vh")) != -1) {
        switch (o) {
        case 'l': g_listen = optarg; break;
        case 'p': g_port = atoi(optarg); break;
        case 't': g_token_f = optarg; break;
        case 'q': g_qual_f = optarg; break;
        case 'm': {
            snprintf(mpdhost, sizeof mpdhost, "%s", optarg);
            char *c = strrchr(mpdhost, ':');
            if (c) { *c = 0; g_mpd_port = atoi(c + 1); }
            g_mpd = mpdhost;
            break;
        }
        case 'c': g_cover_d = optarg; break;
        case 'a': g_api = optarg; break;
        case 'T': g_transport = optarg; break;
        case 'v': g_verbose = 1; break;
        default: usage(); return o == 'h' ? 0 : 1;
        }
    }
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stderr, NULL, _IOLBF, 0);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)g_port) };
    if (inet_pton(AF_INET, g_listen, &a.sin_addr) != 1) { fprintf(stderr, "bad -l\n"); return 1; }
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 16)) {
        perror("ymusicd: bind");
        return 1;
    }
    mkdir(g_cover_d, 0755);
    logm(0, "версия %s, %s:%d, API %s", YM_VERSION, g_listen, g_port, g_api);

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 512 * 1024);
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; usleep(100000); continue; }
        if (g_clients >= MAX_CLIENTS) { reply_json_err(fd, 503, "busy"); close(fd); continue; }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        __sync_fetch_and_add(&g_clients, 1);
        pthread_t th;
        if (pthread_create(&th, &at, client_thread, (void *)(intptr_t)fd)) {
            __sync_fetch_and_sub(&g_clients, 1);
            close(fd);
        }
    }
}
