/*
 * ymusicd — Яндекс Музыка для PureFox (из DigiFox): API для приложения на ПК и потоки для MPD.
 *
 * HTTP на 0.0.0.0:6601. Ответы — JSON с "ok": true|false (при false — "msg" и код 4xx/5xx).
 * Параметры — в строке запроса или в теле POST (application/x-www-form-urlencoded).
 * Обложки — https-ссылки 400x400, длительности — в секундах. Полный список: GET /
 *
 *   Вход:     GET /status · POST /login token= · POST /logout · POST /quality q=lossless|hq|nq
 *   Музыка:   GET /home · /likes · /search?q= · /album?id= · /artist?id= · /playlist?uid=&kind=
 *   Очередь:  POST /queue ids=id[:альбом],...&start=N[&append=1] · POST /wave seed=
 *             GET /queue?before=5&after=10 · GET /now
 *   Пульт:    /next /prev /toggle /play[?pos=N] /pause /stop · POST /like /dislike id=&a=
 *
 *   GET|HEAD /track/<id>.<flac|mp3>[?a=<альбом>&w=<сессия волны>&b=<партия>&s=<сид>]
 *       Только с 127.0.0.1 — аудиопоток трека для MPD. Ссылка берётся из /get-file-info
 *       (подпись HMAC-SHA256 ключом десктопного приложения), поток в транспорте encraw
 *       расшифровывается на лету (AES-128-CTR, ключ из ответа, счётчик с нуля). Range
 *       поддерживается (перемотка в MPD), обрыв со стороны CDN — докачка с того же места.
 *       Content-Type: audio/flac | audio/mpeg — по нему MPD выбирает декодер.
 *       Для треков «Моей волны» (w=...) — отзыв trackStarted и подкачка следующей
 *       партии в очередь MPD, когда до конца очереди остаётся меньше 3 треков.
 *
 * Играть начинает MPD: если активен другой плеер, /queue и /wave переключают на MPD
 * через handle_service.php, как кнопка «Яндекс Музыка» на главной.
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

#define YM_VERSION      "2.0"
#define SIGN_KEY        "kzqU4XhfCaY6B6JTHODeq5"            /* ключ десктопного клиента */
#define DESKTOP_CLIENT  "YandexMusicDesktopAppWindows/5.95.0" /* к нему привязан ключ */
#define ANDROID_CLIENT  "YandexMusicAndroid/24023621"
#define USER_AGENT      "PureFox-ymusicd/" YM_VERSION
#define MAX_CLIENTS     24
#define INFO_TTL        300                                   /* с, кэш ссылок на файлы */
#define WAVE_LOW        3                                     /* подкачка, если осталось < 3 */
#define MAX_QUEUE       500                                   /* треков за раз в очередь MPD */
#define MAX_LIST        1000                                  /* треков в «Мне нравится» и плейлисте */
#define COVER_SIZE      "400x400"
#define OAUTH_URL       "https://oauth.yandex.ru/authorize?response_type=token&client_id=23cabbbdc6cd418abb4b39c32c41195d"

/* ---------- настройки ---------- */
static const char *g_listen  = "0.0.0.0";
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
    const char *txt = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 401 ? "Unauthorized" :
                      code == 403 ? "Forbidden" : code == 404 ? "Not Found" : code == 405 ? "Method Not Allowed" :
                      code == 502 ? "Bad Gateway" : code == 503 ? "Service Unavailable" : "Error";
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

/* формат, который на самом деле отдаёт CDN (flac, mp3...): для «сейчас играет» */
static void write_codec(const char *tid, const char *codec)
{
    char path[512];
    mkdir(g_cover_d, 0755);
    snprintf(path, sizeof path, "%s/%s.codec", g_cover_d, tid);
    FILE *f = fopen(path, "w");
    if (f) { fputs(codec, f); fclose(f); }
}

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
        if (!s.head_only) write_codec(tid, fi.codec);
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

/* ---------- JSON: мелочи ---------- */
static const char *jstr(cJSON *o, const char *k)
{
    return cJSON_GetStringValue(cJSON_GetObjectItem(o, k));
}

/* id из JSON: строка или число */
static void jtxt(cJSON *v, char *out, size_t sz)
{
    if (cJSON_IsString(v)) snprintf(out, sz, "%s", v->valuestring);
    else if (cJSON_IsNumber(v)) snprintf(out, sz, "%.0f", v->valuedouble);
    else out[0] = 0;
}

static int all_digits(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

static void bput(buf_t *b, const char *s)
{
    buf_add(b, s, strlen(s));
}

/* для тела application/x-www-form-urlencoded */
static void bput_esc(buf_t *b, const char *s)
{
    char h[4];
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') buf_add(b, s, 1);
        else { snprintf(h, sizeof h, "%%%02X", c); buf_add(b, h, 3); }
    }
}

/* обложка: "avatars.yandex.net/.../%%" -> "https://avatars.yandex.net/.../400x400" */
static void img(const char *uri, char *out, size_t sz)
{
    out[0] = 0;
    if (!uri || !*uri) return;
    const char *pre = strncmp(uri, "http", 4) ? "https://" : "";
    const char *pp = strstr(uri, "%%");
    if (pp) snprintf(out, sz, "%s%.*s%s%s", pre, (int)(pp - uri), uri, COVER_SIZE, pp + 2);
    else snprintf(out, sz, "%s%s", pre, uri);
}

static void names(cJSON *arr, char *out, size_t sz)
{
    out[0] = 0;
    cJSON *a;
    cJSON_ArrayForEach(a, arr) {
        const char *n = jstr(a, "name");
        if (!n || !*n) continue;
        size_t l = strlen(out);
        snprintf(out + l, sz - l, "%s%s", l ? ", " : "", n);
    }
}

static void write_file(const char *name, const char *s)
{
    char path[512];
    mkdir(g_cover_d, 0755);
    snprintf(path, sizeof path, "%s/%s", g_cover_d, name);
    FILE *f = fopen(path, "w");
    if (f) { fputs(s, f); fclose(f); }
}

/* ---------- треки ---------- */
typedef struct {
    char tid[32], aid[32], title[512], artist[512], album[512], cover[512];
    double dur;                   /* с */
    int ok;                       /* можно играть */
} qtrack_t;

typedef struct { qtrack_t *v; int n, cap; } qlist_t;

static int track_fields(cJSON *t, qtrack_t *q)
{
    memset(q, 0, sizeof *q);
    jtxt(cJSON_GetObjectItem(t, "id"), q->tid, sizeof q->tid);
    if (!q->tid[0]) return -1;
    cJSON *alb = cJSON_GetArrayItem(cJSON_GetObjectItem(t, "albums"), 0);
    jtxt(cJSON_GetObjectItem(alb, "id"), q->aid, sizeof q->aid);
    const char *ti = jstr(t, "title"), *ver = jstr(t, "version"), *al = jstr(alb, "title");
    if (ver && *ver) snprintf(q->title, sizeof q->title, "%s (%s)", ti ? ti : "", ver);
    else snprintf(q->title, sizeof q->title, "%s", ti ? ti : "");
    names(cJSON_GetObjectItem(t, "artists"), q->artist, sizeof q->artist);
    snprintf(q->album, sizeof q->album, "%s", al ? al : "");
    const char *cu = jstr(t, "coverUri");
    img(cu ? cu : jstr(alb, "coverUri"), q->cover, sizeof q->cover);
    cJSON *d = cJSON_GetObjectItem(t, "durationMs");
    q->dur = cJSON_IsNumber(d) ? d->valuedouble / 1000 : 0;
    q->ok = !cJSON_IsFalse(cJSON_GetObjectItem(t, "available")) && all_digits(q->tid) &&
            (!q->aid[0] || all_digits(q->aid));
    return 0;
}

static void ql_push(qlist_t *l, cJSON *t)
{
    if (!t) return;
    if (l->n == l->cap) {
        int c = l->cap ? l->cap * 2 : 64;
        qtrack_t *nv = realloc(l->v, (size_t)c * sizeof *nv);
        if (!nv) return;
        l->v = nv; l->cap = c;
    }
    if (track_fields(t, &l->v[l->n]) == 0) l->n++;
}

static void ql_arr(qlist_t *l, cJSON *arr)
{
    cJSON *t;
    cJSON_ArrayForEach(t, arr) ql_push(l, t);
}

static cJSON *ql_json(const qlist_t *l)
{
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < l->n; i++) {
        const qtrack_t *q = &l->v[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", q->tid);
        cJSON_AddStringToObject(o, "a", q->aid);
        cJSON_AddStringToObject(o, "title", q->title);
        cJSON_AddStringToObject(o, "artist", q->artist);
        cJSON_AddStringToObject(o, "album", q->album);
        cJSON_AddStringToObject(o, "cover", q->cover);
        cJSON_AddNumberToObject(o, "dur", q->dur);
        cJSON_AddBoolToObject(o, "ok", q->ok);
        cJSON_AddItemToArray(a, o);
    }
    return a;
}

/* обложка и длительность трека — для /now и /queue */
static void write_track_files(const qtrack_t *t)
{
    char nm[48], d[32];
    if (t->cover[0]) write_file(t->tid, t->cover);
    if (t->dur > 0) {
        snprintf(nm, sizeof nm, "%s.dur", t->tid);
        snprintf(d, sizeof d, "%.3f", t->dur);
        write_file(nm, d);
    }
}

/* ---------- запросы к Яндексу для клиентов ---------- */
#define FORM "application/x-www-form-urlencoded"

/* result из ответа API; NULL — ошибка, текст в err. *root удаляет вызывающий (может быть NULL) */
static cJSON *ym(const char *method, const char *path_q, const char *ctype, const char *body,
                 cJSON **root, char *err, size_t esz)
{
    *root = NULL;
    buf_t b = {0};
    long st = 0;
    if (api_call(method, path_q, ANDROID_CLIENT, ctype, body, body ? strlen(body) : 0, &b, &st)) {
        free(b.p);
        snprintf(err, esz, "нет связи с api.music.yandex.net");
        return NULL;
    }
    cJSON *j = b.p ? cJSON_Parse(b.p) : NULL;
    free(b.p);
    if (!j) { snprintf(err, esz, "Яндекс: непонятный ответ (HTTP %ld)", st); return NULL; }
    *root = j;
    cJSON *e = cJSON_GetObjectItem(j, "error");
    if (e) {
        const char *n = jstr(e, "name"), *m = cJSON_IsString(e) ? e->valuestring : jstr(e, "message");
        if (n && !strcmp(n, "session-expired")) m = "токен недействителен — войдите заново";
        snprintf(err, esz, "Яндекс: %s", m ? m : n ? n : "ошибка");
        return NULL;
    }
    cJSON *r = cJSON_GetObjectItem(j, "result");
    if (!r) snprintf(err, esz, "Яндекс: пустой ответ (HTTP %ld)", st);
    return r;
}

/* треки по списку "id" или "id:альбом", пачками по 200 */
static int tracks_by_ids(char (*ids)[64], int n, qlist_t *l, char *err, size_t esz)
{
    for (int i = 0; i < n; i += 200) {
        buf_t b = {0};
        bput(&b, "with-positions=false&track-ids=");
        for (int j = i; j < n && j < i + 200; j++) {
            if (j > i) bput(&b, "%2C");
            bput_esc(&b, ids[j]);
        }
        cJSON *root, *r = ym("POST", "/tracks", FORM, b.p, &root, err, esz);
        free(b.p);
        if (r) ql_arr(l, r);
        cJSON_Delete(root);
        if (!r) return -1;
    }
    return 0;
}

/* uid владельца токена (кэш по токену); -1 — нет токена, -2 — Яндекс не ответил */
static char g_uid[32], g_uid_tok[512];

static void uid_store(const char *tok, const char *uid)
{
    pthread_mutex_lock(&g_lock);
    snprintf(g_uid_tok, sizeof g_uid_tok, "%s", tok);
    snprintf(g_uid, sizeof g_uid, "%s", uid);
    pthread_mutex_unlock(&g_lock);
}

static int get_uid(char *uid, size_t sz, char *err, size_t esz)
{
    char tok[512];
    get_token(tok, sizeof tok);
    if (!tok[0]) { snprintf(err, esz, "нужно войти в Яндекс (POST /login)"); return -1; }
    pthread_mutex_lock(&g_lock);
    int hit = g_uid[0] && !strcmp(tok, g_uid_tok);
    if (hit) snprintf(uid, sz, "%s", g_uid);
    pthread_mutex_unlock(&g_lock);
    if (hit) return 0;
    cJSON *root, *r = ym("GET", "/account/status", NULL, NULL, &root, err, esz);
    jtxt(cJSON_GetObjectItem(cJSON_GetObjectItem(r, "account"), "uid"), uid, sz);
    cJSON_Delete(root);
    if (!uid[0]) {
        if (r) snprintf(err, esz, "Яндекс не вернул пользователя — войдите заново");
        return -2;
    }
    uid_store(tok, uid);
    return 0;
}

static cJSON *pl_json(cJSON *p)
{
    cJSON *o = cJSON_CreateObject(), *own = cJSON_GetObjectItem(p, "owner"), *cv = cJSON_GetObjectItem(p, "cover");
    char s[512];
    const char *t;
    jtxt(cJSON_GetObjectItem(p, "uid"), s, sizeof s);
    if (!s[0]) jtxt(cJSON_GetObjectItem(own, "uid"), s, sizeof s);
    cJSON_AddStringToObject(o, "uid", s);
    jtxt(cJSON_GetObjectItem(p, "kind"), s, sizeof s);
    cJSON_AddStringToObject(o, "kind", s);
    cJSON_AddStringToObject(o, "title", (t = jstr(p, "title")) ? t : "");
    double n = cJSON_GetNumberValue(cJSON_GetObjectItem(p, "trackCount"));
    cJSON_AddNumberToObject(o, "n", n > 0 ? n : 0);
    t = jstr(own, "name");
    cJSON_AddStringToObject(o, "owner", t ? t : (t = jstr(own, "login")) ? t : "");
    t = jstr(cv, "uri");
    if (!t) t = cJSON_GetStringValue(cJSON_GetArrayItem(cJSON_GetObjectItem(cv, "itemsUri"), 0));
    if (!t) t = jstr(p, "ogImage");
    img(t, s, sizeof s);
    cJSON_AddStringToObject(o, "cover", s);
    return o;
}

static cJSON *alb_json(cJSON *a)
{
    cJSON *o = cJSON_CreateObject();
    char s[512];
    const char *t = jstr(a, "title"), *v = jstr(a, "version");
    jtxt(cJSON_GetObjectItem(a, "id"), s, sizeof s);
    cJSON_AddStringToObject(o, "id", s);
    if (v && *v) snprintf(s, sizeof s, "%s (%s)", t ? t : "", v);
    else snprintf(s, sizeof s, "%s", t ? t : "");
    cJSON_AddStringToObject(o, "title", s);
    names(cJSON_GetObjectItem(a, "artists"), s, sizeof s);
    cJSON_AddStringToObject(o, "artist", s);
    double y = cJSON_GetNumberValue(cJSON_GetObjectItem(a, "year"));
    cJSON_AddNumberToObject(o, "year", y > 0 ? y : 0);
    img(jstr(a, "coverUri"), s, sizeof s);
    cJSON_AddStringToObject(o, "cover", s);
    double n = cJSON_GetNumberValue(cJSON_GetObjectItem(a, "trackCount"));
    cJSON_AddNumberToObject(o, "n", n > 0 ? n : 0);
    return o;
}

static cJSON *art_json(cJSON *a)
{
    cJSON *o = cJSON_CreateObject();
    char s[512];
    const char *t = jstr(a, "name"), *c = jstr(cJSON_GetObjectItem(a, "cover"), "uri");
    jtxt(cJSON_GetObjectItem(a, "id"), s, sizeof s);
    cJSON_AddStringToObject(o, "id", s);
    cJSON_AddStringToObject(o, "name", t ? t : "");
    img(c ? c : jstr(a, "ogImage"), s, sizeof s);
    cJSON_AddStringToObject(o, "cover", s);
    return o;
}

/* первые max элементов массива через f */
static cJSON *map_json(cJSON *arr, cJSON *(*f)(cJSON *), int max)
{
    cJSON *a = cJSON_CreateArray(), *it;
    int i = 0;
    cJSON_ArrayForEach(it, arr) {
        if (max && i++ >= max) break;
        cJSON_AddItemToArray(a, f(it));
    }
    return a;
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

static int mpd_up(void)
{
    int fd = mpd_connect();
    if (fd < 0) return 0;
    close(fd);
    return 1;
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
            if (!strncmp(last, "ACK", 3)) { logm(0, "MPD: %.60s -> %s", cmd, last); break; }
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

/* значение ключа (первое) из ответа MPD; 0 — нет */
static int kv_str(const char *resp, const char *key, char *out, size_t sz)
{
    size_t kl = strlen(key);
    out[0] = 0;
    for (const char *p = resp; p && *p; ) {
        const char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        if (ll >= kl + 2 && !strncmp(p, key, kl) && p[kl] == ':' && p[kl + 1] == ' ') {
            snprintf(out, sz, "%.*s", (int)(ll - kl - 2), p + kl + 2);
            return 1;
        }
        p = nl ? nl + 1 : NULL;
    }
    return 0;
}

static long kv_long(const char *resp, const char *key)
{
    char v[32];
    return kv_str(resp, key, v, sizeof v) && v[0] ? strtol(v, NULL, 10) : -1;
}

static double kv_num(const char *resp, const char *key)
{
    char v[32];
    return kv_str(resp, key, v, sizeof v) ? strtod(v, NULL) : 0;
}

/* треки в очередь MPD: все addid одной пачкой, затем все теги; wave_q — хвост запроса волны.
 * Число добавленных (ACK посередине — добавлено то, что до него) */
static int mpd_add_tracks(int fd, const qtrack_t *v, int n, const char *wave_q)
{
    if (n <= 0) return 0;
    char qb[16], line[2048], q[1100];
    const char *ext = !strcmp(get_quality(qb, sizeof qb), "lossless") ? "flac" : "mp3";
    buf_t c = {0}, r = {0};
    bput(&c, "command_list_ok_begin\n");
    for (int i = 0; i < n; i++) {
        snprintf(line, sizeof line, "addid \"http://127.0.0.1:%d/track/%s.%s?a=%s%s\"\n",
                 g_port, v[i].tid, ext, v[i].aid, wave_q ? wave_q : "");
        bput(&c, line);
    }
    bput(&c, "command_list_end");
    mpd_cmd(fd, c.p, &r);
    free(c.p);
    long *ids = calloc((size_t)n, sizeof *ids);
    int k = 0;
    for (char *p = r.p; ids && p && *p && k < n; ) {
        if (!strncmp(p, "Id: ", 4)) ids[k++] = strtol(p + 4, NULL, 10);
        p = strchr(p, '\n');
        if (p) p++;
    }
    free(r.p);
    c = (buf_t){0};
    bput(&c, "command_list_begin\n");
    for (int i = 0; i < k; i++) {
        const qtrack_t *t = &v[i];
        const char *tag[3] = { "Title", "Artist", "Album" }, *val[3] = { t->title, t->artist, t->album };
        for (int j = 0; j < 3; j++) {
            if (!val[j][0]) continue;
            mpd_quote(q, sizeof q, val[j]);
            snprintf(line, sizeof line, "addtagid %ld %s %s\n", ids[i], tag[j], q);
            bput(&c, line);
        }
        write_track_files(t);
    }
    bput(&c, "command_list_end");
    if (k) mpd_cmd(fd, c.p, NULL);
    free(c.p);
    free(ids);
    return k;
}

/* активный плеер Фокса (naa, scream, mpd...) */
static void active_service(char *out, size_t sz)
{
    char b[16384];
    out[0] = 0;
    if (!read_small_file("/tmp/system_status.json", b, sizeof b)) {
        cJSON *j = cJSON_Parse(b);
        const char *s = jstr(j, "active_service");
        if (s) snprintf(out, sz, "%s", s);
        cJSON_Delete(j);
    }
    if (!out[0]) read_small_file("/tmp/active_service", out, sz);
}

/* MPD должен быть активным плеером (как кнопка «Яндекс Музыка» на главной) */
static int ensure_mpd(void)
{
    char svc[32];
    struct stat sb;
    active_service(svc, sizeof svc);
    if (!strcmp(svc, "mpd") && stat("/etc/usb_to_i2s.state", &sb) && mpd_up()) return 0;
    logm(0, "переключение на MPD (был %s)", svc[0] ? svc : "?");
    CURL *c = curl_easy_init();
    if (c) {
        buf_t b = {0};
        curl_easy_setopt(c, CURLOPT_URL, "http://127.0.0.1/handle_service.php");
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, "service=mpd");
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cb_mem);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_perform(c);
        curl_easy_cleanup(c);
        free(b.p);
    }
    for (int i = 0; i < 50 && !mpd_up(); i++) usleep(100000);
    if (!mpd_up()) return -1;
    usleep(300000);
    return 0;
}

/* очередь MPD: заменить (или дописать) и включить с v[start]; число добавленных или -1 */
static int queue_play(qtrack_t *v, int n, int start, int append, const char *wave_q, char *err, size_t esz)
{
    int m = 0, st = 0;
    for (int i = 0; i < n; i++) {                      /* недоступные — вон */
        if (i == start) st = m;
        if (v[i].ok) v[m++] = v[i];
    }
    if (!m) { snprintf(err, esz, "нет доступных треков"); return -1; }
    if (st >= m) st = m - 1;
    if (m > MAX_QUEUE) {                                /* окно с выбранного трека */
        int from = st < m - MAX_QUEUE ? st : m - MAX_QUEUE;
        v += from; m = MAX_QUEUE; st -= from;
    }
    if (ensure_mpd()) { snprintf(err, esz, "не удалось включить MPD"); return -1; }
    int fd = mpd_connect();
    if (fd < 0) { snprintf(err, esz, "MPD не отвечает"); return -1; }
    buf_t sb = {0};
    char state[16] = "";
    if (!append) mpd_cmd(fd, "clear", NULL);
    mpd_cmd(fd, "status", &sb);
    long base = kv_long(sb.p, "playlistlength");
    kv_str(sb.p, "state", state, sizeof state);
    free(sb.p);
    int k = mpd_add_tracks(fd, v, m, wave_q);
    char c[32] = "";
    if (k && !append) snprintf(c, sizeof c, "play %d", st < k ? st : k - 1);
    else if (k && strcmp(state, "play")) snprintf(c, sizeof c, "play %ld", base > 0 ? base : 0);
    if (c[0]) mpd_cmd(fd, c, NULL);
    close(fd);
    if (!k) { snprintf(err, esz, "MPD не принял треки"); return -1; }
    return k;
}

/* песня из ответа MPD (currentsong или кусок playlistinfo) */
static cJSON *song_json(const char *blk)
{
    char file[2048], tid[32] = "", aid[32] = "", v[2048], nm[48];
    kv_str(blk, "file", file, sizeof file);
    const char *tr = strstr(file, "127.0.0.1:") ? strstr(file, "/track/") : NULL;
    if (tr) {
        size_t i = 0;
        for (tr += 7; isdigit((unsigned char)*tr) && i + 1 < sizeof tid; ) tid[i++] = *tr++;
        tid[i] = 0;
        const char *qm = strchr(file, '?');
        if (qm) query_get(qm + 1, "a", aid, sizeof aid);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "song", kv_long(blk, "Pos") > 0 ? kv_long(blk, "Pos") : 0);
    cJSON_AddBoolToObject(o, "ym", tid[0] != 0);
    cJSON_AddStringToObject(o, "id", tid);
    cJSON_AddStringToObject(o, "a", aid);
    if (!kv_str(blk, "Title", v, sizeof v)) {
        const char *sl = strrchr(file, '/');
        snprintf(v, sizeof v, "%s", sl ? sl + 1 : file);
    }
    cJSON_AddStringToObject(o, "title", v);
    kv_str(blk, "Artist", v, sizeof v);
    cJSON_AddStringToObject(o, "artist", v);
    kv_str(blk, "Album", v, sizeof v);
    cJSON_AddStringToObject(o, "album", v);
    double d = kv_num(blk, "duration");
    if (d <= 0) d = kv_num(blk, "Time");
    v[0] = 0;
    if (tid[0]) {                                     /* MPD узнаёт длину потока, только начав его */
        char p[512];
        snprintf(nm, sizeof nm, "%s.dur", tid);
        snprintf(p, sizeof p, "%s/%s", g_cover_d, nm);
        if (d <= 0 && !read_small_file(p, v, sizeof v)) d = strtod(v, NULL);
        snprintf(p, sizeof p, "%s/%s", g_cover_d, tid);
        read_small_file(p, v, sizeof v);
    }
    cJSON_AddNumberToObject(o, "dur", d);
    cJSON_AddStringToObject(o, "cover", v);
    return o;
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

/* хвост ссылки на трек волны: &w=сессия&b=партия&s=сид */
static void wave_query(char *out, size_t sz, const char *sid, const char *batch, const char *seed)
{
    char *e1 = url_esc(sid), *e2 = url_esc(batch ? batch : ""), *e3 = url_esc(seed);
    snprintf(out, sz, "&w=%s&b=%s&s=%s", e1, e2, e3);
    free(e1); free(e2); free(e3);
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
    if (!sid) sid = w->sid;
    char wq[800];
    wave_query(wq, sizeof wq, sid, cJSON_GetStringValue(cJSON_GetObjectItem(res, "batchId")), w->seed);
    qlist_t ql = {0};
    cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(res, "sequence")) ql_push(&ql, cJSON_GetObjectItem(it, "track"));
    int m = 0;
    for (int i = 0; i < ql.n; i++) if (ql.v[i].ok) ql.v[m++] = ql.v[i];
    logm(1, "волна: +%d в очередь", mpd_add_tracks(fd, ql.v, m, wq));
    free(ql.v);
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

/* ---------- API для клиентов (приложение на ПК) ---------- */
typedef struct {
    int fd;
    const char *method, *path, *query;
    const char *body;                 /* application/x-www-form-urlencoded */
} req_t;

static void send_json(int fd, int code, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    reply(fd, code, "application/json; charset=utf-8", s ? s : "{}", s ? strlen(s) : 2);
    free(s);
    cJSON_Delete(o);
}

static void send_fail(int fd, int code, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", 0);
    cJSON_AddStringToObject(o, "msg", msg);
    send_json(fd, code, o);
}

static void send_ok(int fd, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", 1);
    if (msg) cJSON_AddStringToObject(o, "msg", msg);
    send_json(fd, 200, o);
}

/* параметр из строки запроса или тела формы, без пробелов по краям; 0 — нет */
static int prm(const req_t *r, const char *k, char *out, size_t sz)
{
    if (!query_get(r->query, k, out, sz) && !(r->body && query_get(r->body, k, out, sz))) return 0;
    size_t n = strlen(out), s = 0;
    while (n && isspace((unsigned char)out[n - 1])) out[--n] = 0;
    while (out[s] && isspace((unsigned char)out[s])) s++;
    if (s) memmove(out, out + s, n - s + 1);
    return 1;
}

/* ok:true и объект out: собрать ответ и отправить */
static void send_obj(const req_t *r, cJSON *o)
{
    cJSON_AddBoolToObject(o, "ok", 1);
    send_json(r->fd, 200, o);
}

static void h_status(req_t *r)
{
    char tok[512], qb[16], err[300], uid[32];
    get_token(tok, sizeof tok);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "version", YM_VERSION);
    cJSON_AddBoolToObject(o, "token", tok[0] != 0);
    cJSON_AddStringToObject(o, "quality", get_quality(qb, sizeof qb));
    cJSON_AddStringToObject(o, "oauth", OAUTH_URL);
    if (tok[0]) {
        cJSON *root, *res = ym("GET", "/account/status", NULL, NULL, &root, err, sizeof err);
        cJSON *acc = cJSON_GetObjectItem(res, "account");
        jtxt(cJSON_GetObjectItem(acc, "uid"), uid, sizeof uid);
        if (uid[0]) {
            const char *nm = jstr(acc, "displayName");
            if (!nm) nm = jstr(acc, "login");
            cJSON_AddStringToObject(o, "login", nm ? nm : "");
            cJSON_AddBoolToObject(o, "plus", cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(res, "plus"), "hasPlus")));
            uid_store(tok, uid);
        } else {
            if (root) cJSON_AddBoolToObject(o, "bad_token", 1);   /* Яндекс ответил, но токен не принял */
            cJSON_AddStringToObject(o, "msg", res ? "токен не принят" : err);
        }
        cJSON_Delete(root);
    }
    send_obj(r, o);
}

static void h_login(req_t *r)
{
    char v[2048], err[300], uid[32] = "";
    if (!prm(r, "token", v, sizeof v)) { send_fail(r->fd, 400, "нужен token"); return; }
    char *p = strstr(v, "access_token=");                /* можно весь адрес страницы после входа */
    if (p) {
        p += 13;
        size_t n = strcspn(p, "&# \t\r\n");
        memmove(v, p, n);
        v[n] = 0;
    }
    size_t n = strlen(v);
    int good = n >= 20 && n <= 400;
    for (size_t i = 0; good && i < n; i++)
        good = isalnum((unsigned char)v[i]) || v[i] == '_' || v[i] == '.' || v[i] == '-';
    if (!good) { send_fail(r->fd, 400, "это не похоже на токен: нужен адрес страницы целиком или только access_token"); return; }
    char dir[512];
    snprintf(dir, sizeof dir, "%s", g_token_f);
    if ((p = strrchr(dir, '/'))) { *p = 0; mkdir(dir, 0755); }
    FILE *f = fopen(g_token_f, "w");
    if (!f || fputs(v, f) < 0) { if (f) fclose(f); send_fail(r->fd, 500, "не удалось сохранить токен"); return; }
    fclose(f);
    chmod(g_token_f, 0600);
    cJSON *root, *res = ym("GET", "/account/status", NULL, NULL, &root, err, sizeof err);
    cJSON *acc = cJSON_GetObjectItem(res, "account");
    jtxt(cJSON_GetObjectItem(acc, "uid"), uid, sizeof uid);
    if (uid[0]) {
        const char *nm = jstr(acc, "displayName");
        if (!nm) nm = jstr(acc, "login");
        uid_store(v, uid);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "login", nm ? nm : "");
        cJSON_AddBoolToObject(o, "plus", cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(res, "plus"), "hasPlus")));
        send_obj(r, o);
    } else {
        char m[400];
        snprintf(m, sizeof m, "токен сохранён, но Яндекс его не принял: %s", res ? "нет пользователя" : err);
        send_fail(r->fd, 401, m);
    }
    cJSON_Delete(root);
}

static void h_logout(req_t *r)
{
    unlink(g_token_f);
    uid_store("", "");
    send_ok(r->fd, NULL);
}

static void h_quality(req_t *r)
{
    char v[16];
    if (!prm(r, "q", v, sizeof v) || (strcmp(v, "lossless") && strcmp(v, "hq") && strcmp(v, "nq"))) {
        send_fail(r->fd, 400, "q: lossless | hq | nq");
        return;
    }
    char dir[512], *p;
    snprintf(dir, sizeof dir, "%s", g_qual_f);
    if ((p = strrchr(dir, '/'))) { *p = 0; mkdir(dir, 0755); }
    FILE *f = fopen(g_qual_f, "w");
    if (!f) { send_fail(r->fd, 500, "не удалось сохранить"); return; }
    fprintf(f, "%s\n", v);
    fclose(f);
    send_ok(r->fd, NULL);
}

/* uid или ответ с ошибкой; 0 — можно продолжать */
static int need_uid(req_t *r, char *uid, size_t sz)
{
    char err[300];
    int rc = get_uid(uid, sz, err, sizeof err);
    if (rc) send_fail(r->fd, rc == -1 ? 401 : 502, err);
    return rc;
}

static void h_home(req_t *r)
{
    char uid[32], err[300], path[128], s[512];
    if (need_uid(r, uid, sizeof uid)) return;
    snprintf(path, sizeof path, "/users/%s/playlists/list", uid);
    cJSON *root, *res = ym("GET", path, NULL, NULL, &root, err, sizeof err);
    if (!res) { cJSON_Delete(root); send_fail(r->fd, 502, err); return; }
    cJSON *o = cJSON_CreateObject(), *it;
    cJSON_AddItemToObject(o, "playlists", map_json(res, pl_json, 0));
    cJSON_Delete(root);

    cJSON *pers = cJSON_AddArrayToObject(o, "personal");     /* «Для вас» — не страшно, если нет */
    res = ym("GET", "/landing3?blocks=personalplaylists", NULL, NULL, &root, err, sizeof err);
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(res, "blocks"), 0), "entities")) {
        cJSON *p = cJSON_GetObjectItem(cJSON_GetObjectItem(it, "data"), "data");
        if (cJSON_IsObject(p)) cJSON_AddItemToArray(pers, pl_json(p));
    }
    cJSON_Delete(root);

    cJSON *sts = cJSON_AddArrayToObject(o, "stations");
    res = ym("GET", "/rotor/stations/dashboard", NULL, NULL, &root, err, sizeof err);
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(res, "stations")) {
        cJSON *st = cJSON_GetObjectItem(it, "station"), *id = cJSON_GetObjectItem(st, "id");
        cJSON *icon = cJSON_GetObjectItem(st, "icon");
        const char *ty = jstr(id, "type"), *tag = jstr(id, "tag"), *nm = jstr(st, "name"), *bg = jstr(icon, "backgroundColor");
        if (!ty || !tag) continue;
        cJSON *x = cJSON_CreateObject();
        snprintf(s, sizeof s, "%s:%s", ty, tag);
        cJSON_AddStringToObject(x, "seed", s);
        cJSON_AddStringToObject(x, "name", nm ? nm : "");
        img(jstr(icon, "imageUrl"), s, sizeof s);
        cJSON_AddStringToObject(x, "cover", s);
        cJSON_AddStringToObject(x, "color", bg ? bg : "");
        cJSON_AddItemToArray(sts, x);
    }
    cJSON_Delete(root);
    send_obj(r, o);
}

static void h_likes(req_t *r)
{
    char uid[32], err[300], path[128];
    if (need_uid(r, uid, sizeof uid)) return;
    snprintf(path, sizeof path, "/users/%s/likes/tracks", uid);
    cJSON *root, *res = ym("GET", path, NULL, NULL, &root, err, sizeof err), *it;
    if (!res) { cJSON_Delete(root); send_fail(r->fd, 502, err); return; }
    cJSON *lt = cJSON_GetObjectItem(cJSON_GetObjectItem(res, "library"), "tracks");
    int n = cJSON_GetArraySize(lt);
    if (n > MAX_LIST) n = MAX_LIST;
    char (*ids)[64] = calloc((size_t)n + 1, sizeof *ids);
    int k = 0;
    cJSON_ArrayForEach(it, lt) {
        if (!ids || k >= n) break;
        char id[32], al[32];
        jtxt(cJSON_GetObjectItem(it, "id"), id, sizeof id);
        jtxt(cJSON_GetObjectItem(it, "albumId"), al, sizeof al);
        if (id[0]) snprintf(ids[k++], sizeof ids[0], "%s%s%s", id, al[0] ? ":" : "", al);
    }
    cJSON_Delete(root);
    qlist_t ql = {0};
    if (ids && tracks_by_ids(ids, k, &ql, err, sizeof err)) { free(ids); free(ql.v); send_fail(r->fd, 502, err); return; }
    free(ids);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "title", "Мне нравится");
    cJSON_AddItemToObject(o, "tracks", ql_json(&ql));
    free(ql.v);
    send_obj(r, o);
}

static void h_search(req_t *r)
{
    char q[512], err[300], path[2048];
    cJSON *o = cJSON_CreateObject();
    if (!prm(r, "q", q, sizeof q) || !q[0]) {
        cJSON_AddArrayToObject(o, "tracks");
        cJSON_AddArrayToObject(o, "albums");
        cJSON_AddArrayToObject(o, "artists");
        cJSON_AddArrayToObject(o, "playlists");
        send_obj(r, o);
        return;
    }
    char *e = url_esc(q);
    snprintf(path, sizeof path, "/search?text=%s&type=all&page=0&nocorrect=false", e);
    free(e);
    cJSON *root, *res = ym("GET", path, NULL, NULL, &root, err, sizeof err);
    if (!res) { cJSON_Delete(root); cJSON_Delete(o); send_fail(r->fd, 502, err); return; }
    qlist_t ql = {0};
    ql_arr(&ql, cJSON_GetObjectItem(cJSON_GetObjectItem(res, "tracks"), "results"));
    cJSON_AddItemToObject(o, "tracks", ql_json(&ql));
    free(ql.v);
    cJSON_AddItemToObject(o, "albums", map_json(cJSON_GetObjectItem(cJSON_GetObjectItem(res, "albums"), "results"), alb_json, 12));
    cJSON_AddItemToObject(o, "artists", map_json(cJSON_GetObjectItem(cJSON_GetObjectItem(res, "artists"), "results"), art_json, 12));
    cJSON_AddItemToObject(o, "playlists", map_json(cJSON_GetObjectItem(cJSON_GetObjectItem(res, "playlists"), "results"), pl_json, 12));
    cJSON_AddBoolToObject(o, "corrected", cJSON_IsTrue(cJSON_GetObjectItem(res, "misspellCorrected")));
    const char *t = jstr(res, "text");
    cJSON_AddStringToObject(o, "text", t ? t : q);
    cJSON_Delete(root);
    send_obj(r, o);
}

static void h_album(req_t *r)
{
    char id[32], err[300], path[128], s[600];
    if (!prm(r, "id", id, sizeof id) || !all_digits(id)) { send_fail(r->fd, 400, "нужен id альбома"); return; }
    snprintf(path, sizeof path, "/albums/%s/with-tracks", id);
    cJSON *root, *res = ym("GET", path, NULL, NULL, &root, err, sizeof err), *vol;
    if (!res) { cJSON_Delete(root); send_fail(r->fd, 502, err); return; }
    qlist_t ql = {0};
    cJSON_ArrayForEach(vol, cJSON_GetObjectItem(res, "volumes")) ql_arr(&ql, vol);
    cJSON *o = alb_json(res);                       /* id, title, artist, year, cover, n */
    cJSON_AddItemToObject(o, "tracks", ql_json(&ql));
    free(ql.v);
    snprintf(s, sizeof s, "album:%s", id);
    cJSON_AddStringToObject(o, "seed", s);
    cJSON_Delete(root);
    send_obj(r, o);
}

static void h_artist(req_t *r)
{
    char id[32], err[300], path[128], s[512];
    if (!prm(r, "id", id, sizeof id) || !all_digits(id)) { send_fail(r->fd, 400, "нужен id исполнителя"); return; }
    snprintf(path, sizeof path, "/artists/%s/brief-info", id);
    cJSON *root, *res = ym("GET", path, NULL, NULL, &root, err, sizeof err);
    if (!res) { cJSON_Delete(root); send_fail(r->fd, 502, err); return; }
    cJSON *o = art_json(cJSON_GetObjectItem(res, "artist"));   /* id, name, cover */
    qlist_t ql = {0};
    ql_arr(&ql, cJSON_GetObjectItem(res, "popularTracks"));
    cJSON_AddItemToObject(o, "tracks", ql_json(&ql));
    free(ql.v);
    cJSON_AddItemToObject(o, "albums", map_json(cJSON_GetObjectItem(res, "albums"), alb_json, 0));
    snprintf(s, sizeof s, "artist:%s", id);
    cJSON_AddStringToObject(o, "seed", s);
    cJSON_Delete(root);
    send_obj(r, o);
}

static void h_playlist(req_t *r)
{
    char uid[64], kind[32], err[300], path[160];
    int good = prm(r, "uid", uid, sizeof uid) && uid[0] && prm(r, "kind", kind, sizeof kind) && all_digits(kind);
    for (char *p = uid; good && *p; p++) good = isalnum((unsigned char)*p) || *p == '_' || *p == '.' || *p == '-';
    if (!good) { send_fail(r->fd, 400, "нужны uid и kind плейлиста"); return; }
    snprintf(path, sizeof path, "/users/%s/playlists/%s", uid, kind);
    cJSON *root, *res = ym("GET", path, NULL, NULL, &root, err, sizeof err), *it;
    if (!res) { cJSON_Delete(root); send_fail(r->fd, 502, err); return; }
    cJSON *items = cJSON_GetObjectItem(res, "tracks");
    int n = cJSON_GetArraySize(items), k = 0;
    char (*miss)[64] = calloc((size_t)n + 1, sizeof *miss);
    qlist_t ql = {0};
    cJSON_ArrayForEach(it, items) {                  /* без вложенного трека — дозапросить */
        cJSON *t = cJSON_GetObjectItem(it, "track");
        if (t) { ql_push(&ql, t); continue; }
        char id[32], al[32];
        jtxt(cJSON_GetObjectItem(it, "id"), id, sizeof id);
        jtxt(cJSON_GetObjectItem(it, "albumId"), al, sizeof al);
        if (miss && id[0] && k < MAX_LIST) snprintf(miss[k++], sizeof miss[0], "%s%s%s", id, al[0] ? ":" : "", al);
    }
    cJSON *o = pl_json(res);                         /* uid, kind, title, n, owner, cover */
    cJSON_Delete(root);
    if (k && tracks_by_ids(miss, k, &ql, err, sizeof err)) {
        free(miss); free(ql.v); cJSON_Delete(o);
        send_fail(r->fd, 502, err);
        return;
    }
    free(miss);
    cJSON_AddItemToObject(o, "tracks", ql_json(&ql));
    free(ql.v);
    send_obj(r, o);
}

static void h_now(req_t *r)
{
    int fd = mpd_connect();
    cJSON *o;
    if (fd < 0) {
        o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "state", "off");
        send_obj(r, o);
        return;
    }
    buf_t st = {0}, cs = {0};
    char v[256], file[2048], svc[32], p[512];
    mpd_cmd(fd, "status", &st);
    mpd_cmd(fd, "currentsong", &cs);
    close(fd);
    o = song_json(cs.p ? cs.p : "");
    kv_str(cs.p, "file", file, sizeof file);
    kv_str(st.p, "state", v, sizeof v);
    cJSON_AddStringToObject(o, "state", v[0] ? v : "stop");
    active_service(svc, sizeof svc);
    cJSON_AddStringToObject(o, "svc", svc);
    cJSON_AddBoolToObject(o, "wave", strstr(file, "&w=") != NULL);
    cJSON_AddNumberToObject(o, "pos", kv_num(st.p, "elapsed"));
    if (kv_num(st.p, "duration") > 0) cJSON_ReplaceItemInObject(o, "dur", cJSON_CreateNumber(kv_num(st.p, "duration")));
    cJSON_AddNumberToObject(o, "n", kv_long(st.p, "playlistlength") > 0 ? kv_long(st.p, "playlistlength") : 0);
    cJSON_ReplaceItemInObject(o, "song", cJSON_CreateNumber(kv_long(st.p, "song") > 0 ? kv_long(st.p, "song") : 0));
    kv_str(st.p, "audio", v, sizeof v);
    cJSON_AddStringToObject(o, "audio", v);
    cJSON_AddNumberToObject(o, "kbps", kv_long(st.p, "bitrate") > 0 ? kv_long(st.p, "bitrate") : 0);
    const char *tid = cJSON_GetStringValue(cJSON_GetObjectItem(o, "id"));
    v[0] = 0;
    if (tid && tid[0]) {
        snprintf(p, sizeof p, "%s/%s.codec", g_cover_d, tid);
        read_small_file(p, v, sizeof v);
        for (char *c = v; *c; c++) *c = (char)toupper((unsigned char)*c);
    }
    cJSON_AddStringToObject(o, "codec", v);
    free(st.p); free(cs.p);
    send_obj(r, o);
}

/* GET: соседи текущей песни в очереди (prev — до неё, ближние в конце; next — после) */
static void h_queue_get(req_t *r)
{
    char v[16];
    int nb = prm(r, "before", v, sizeof v) ? atoi(v) : 5, na = prm(r, "after", v, sizeof v) ? atoi(v) : 10;
    nb = nb < 0 ? 0 : nb > 100 ? 100 : nb;
    na = na < 0 ? 0 : na > 100 ? 100 : na;
    cJSON *o = cJSON_CreateObject();
    cJSON *prev = cJSON_AddArrayToObject(o, "prev"), *next = cJSON_AddArrayToObject(o, "next"), *cur = NULL;
    int fd = mpd_connect();
    if (fd < 0) {
        cJSON_AddStringToObject(o, "state", "off");
        cJSON_AddNumberToObject(o, "song", -1);
        cJSON_AddNumberToObject(o, "n", 0);
        cJSON_AddNullToObject(o, "current");
        send_obj(r, o);
        return;
    }
    buf_t st = {0}, pl = {0};
    mpd_cmd(fd, "status", &st);
    long n = kv_long(st.p, "playlistlength"), song = kv_long(st.p, "song");
    if (n < 0) n = 0;
    long from = song - nb > 0 ? song - nb : 0, to = song + 1 + na < n ? song + 1 + na : n;
    if (to > from) {                                   /* без текущей (song = -1) — начало очереди */
        char c[64];
        snprintf(c, sizeof c, "playlistinfo %ld:%ld", from, to);
        mpd_cmd(fd, c, &pl);
    }
    close(fd);
    kv_str(st.p, "state", v, sizeof v);
    cJSON_AddStringToObject(o, "state", v[0] ? v : "stop");
    cJSON_AddNumberToObject(o, "song", (double)song);
    cJSON_AddNumberToObject(o, "n", (double)n);
    /* playlistinfo: каждая песня начинается со строки file: */
    for (char *p = pl.p; p && *p; ) {
        char *e = strstr(p + 1, "\nfile: ");
        if (e) *e = 0;
        if (!strncmp(p, "file: ", 6)) {
            cJSON *s = song_json(p);
            long pos = (long)cJSON_GetNumberValue(cJSON_GetObjectItem(s, "song"));
            if (pos < song) cJSON_AddItemToArray(prev, s);
            else if (pos == song && !cur) cur = s;
            else cJSON_AddItemToArray(next, s);
        }
        p = e ? e + 1 : NULL;
    }
    if (cur) cJSON_AddItemToObject(o, "current", cur);
    else cJSON_AddNullToObject(o, "current");
    free(st.p); free(pl.p);
    send_obj(r, o);
}

/* POST: поставить треки (ids=id[:альбом],...) — заменить очередь и играть с start, или append=1 */
static void h_queue_post(req_t *r)
{
    size_t sz = (r->query ? strlen(r->query) : 0) + (r->body ? strlen(r->body) : 0) + 1;
    char *list = malloc(sz), v[16], err[300];
    if (!list || !prm(r, "ids", list, sz) || !list[0]) { free(list); send_fail(r->fd, 400, "нужен ids=id[:альбом],..."); return; }
    int start = prm(r, "start", v, sizeof v) ? atoi(v) : 0, append = prm(r, "append", v, sizeof v) && atoi(v) > 0;
    char (*ids)[64] = calloc(MAX_LIST, sizeof *ids);
    int n = 0;
    char *sp = NULL;
    for (char *tok = strtok_r(list, ", ", &sp); ids && tok && n < MAX_LIST; tok = strtok_r(NULL, ", ", &sp)) {
        if (strlen(tok) >= sizeof ids[0] || strspn(tok, "0123456789:") != strlen(tok) || !isdigit((unsigned char)*tok)) {
            free(list); free(ids);
            send_fail(r->fd, 400, "ids: числа вида 123 или 123:456 через запятую");
            return;
        }
        snprintf(ids[n++], sizeof ids[0], "%s", tok);
    }
    free(list);
    if (start < 0 || start >= n) start = 0;
    qlist_t ql = {0};
    if (!ids || tracks_by_ids(ids, n, &ql, err, sizeof err)) {
        free(ids); free(ql.v);
        send_fail(r->fd, 502, ids ? err : "нет памяти");
        return;
    }
    /* Яндекс может не вернуть часть треков: start — по id выбранного */
    char want[32];
    snprintf(want, sizeof want, "%.*s", (int)strcspn(ids[start], ":"), ids[start]);
    free(ids);
    int s = 0;
    for (int i = 0; i < ql.n; i++) if (!strcmp(ql.v[i].tid, want)) { s = i; break; }
    int k = queue_play(ql.v, ql.n, s, append, NULL, err, sizeof err);
    free(ql.v);
    if (k < 0) { send_fail(r->fd, strstr(err, "MPD") ? 503 : 400, err); return; }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "added", k);
    send_obj(r, o);
}

/* «Моя волна» (seed=user:onyourwave) или станция / альбом / исполнитель (seed из ответов) */
static void h_wave(req_t *r)
{
    char seed[160] = "", err[300], sid[160] = "", batch[160] = "", path[300], ts[40];
    if (!prm(r, "seed", seed, sizeof seed) || !seed[0]) snprintf(seed, sizeof seed, "user:onyourwave");
    size_t sl = strlen(seed);
    int good = sl >= 3 && sl <= 120;
    for (size_t i = 0; good && i < sl; i++)
        good = isalnum((unsigned char)seed[i]) || strchr("_.:-", seed[i]);
    if (!good) { send_fail(r->fd, 400, "плохой seed"); return; }
    cJSON *b = cJSON_CreateObject();
    cJSON_AddItemToArray(cJSON_AddArrayToObject(b, "seeds"), cJSON_CreateString(seed));
    cJSON_AddArrayToObject(b, "queue");
    cJSON_AddBoolToObject(b, "includeTracksInResponse", 1);
    char *bs = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    cJSON *root, *res = ym("POST", "/rotor/session/new", "application/json", bs, &root, err, sizeof err), *it;
    free(bs);
    const char *s1 = jstr(res, "radioSessionId"), *s2 = jstr(res, "batchId");
    snprintf(sid, sizeof sid, "%s", s1 ? s1 : "");
    snprintf(batch, sizeof batch, "%s", s2 ? s2 : "");
    qlist_t ql = {0};
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(res, "sequence")) ql_push(&ql, cJSON_GetObjectItem(it, "track"));
    cJSON_Delete(root);
    if (!sid[0] || !ql.n) {
        free(ql.v);
        send_fail(r->fd, 502, res ? "волна не запустилась (нужна подписка Плюс?)" : err);
        return;
    }
    b = cJSON_CreateObject();
    cJSON *ev = cJSON_AddObjectToObject(b, "event");
    iso_now(ts, sizeof ts);
    cJSON_AddStringToObject(ev, "type", "radioStarted");
    cJSON_AddStringToObject(ev, "timestamp", ts);
    cJSON_AddStringToObject(b, "batchId", batch);
    bs = cJSON_PrintUnformatted(b);
    cJSON_Delete(b);
    char *esid = url_esc(sid);
    snprintf(path, sizeof path, "/rotor/session/%s/feedback", esid);
    free(esid);
    cJSON_Delete(api_json("POST", path, ANDROID_CLIENT, bs));
    free(bs);
    char wq[800];
    wave_query(wq, sizeof wq, sid, batch, seed);
    int k = queue_play(ql.v, ql.n, 0, 0, wq, err, sizeof err);
    free(ql.v);
    if (k < 0) { send_fail(r->fd, strstr(err, "MPD") ? 503 : 400, err); return; }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "added", k);
    send_obj(r, o);
}

/* like / dislike (dislike ещё и пропускает трек) */
static void h_like(req_t *r)
{
    char id[32], al[32] = "", uid[32], err[300], path[160];
    int like = !strcmp(r->path, "/like");
    if (!prm(r, "id", id, sizeof id) || !all_digits(id) || (prm(r, "a", al, sizeof al) && al[0] && !all_digits(al))) {
        send_fail(r->fd, 400, "нужен id трека (и a — альбом)");
        return;
    }
    if (need_uid(r, uid, sizeof uid)) return;
    snprintf(path, sizeof path, "/users/%s/%s/tracks/add-multiple", uid, like ? "likes" : "dislikes");
    buf_t b = {0};
    bput(&b, "track-ids=");
    bput(&b, id);
    if (al[0]) { bput(&b, "%3A"); bput(&b, al); }
    cJSON *root, *res = ym("POST", path, FORM, b.p, &root, err, sizeof err);
    free(b.p);
    cJSON_Delete(root);
    if (!res) { send_fail(r->fd, 502, err); return; }
    if (!like) {
        int fd = mpd_connect();
        if (fd >= 0) { mpd_cmd(fd, "next", NULL); close(fd); }
    }
    send_ok(r->fd, NULL);
}

/* next | prev | toggle | play [pos] | pause | stop */
static void h_ctl(req_t *r)
{
    const char *c = r->path + 1;
    char cmd[32], v[16];
    int fd = mpd_connect();
    if (fd < 0) { send_fail(r->fd, 503, "MPD не отвечает"); return; }
    if (!strcmp(c, "toggle")) {
        buf_t st = {0};
        mpd_cmd(fd, "status", &st);
        kv_str(st.p, "state", v, sizeof v);
        free(st.p);
        snprintf(cmd, sizeof cmd, "%s", strcmp(v, "play") ? "play" : "pause 1");
    } else if (!strcmp(c, "play")) {
        if (prm(r, "pos", v, sizeof v) && all_digits(v)) snprintf(cmd, sizeof cmd, "play %s", v);
        else snprintf(cmd, sizeof cmd, "play");
    } else {
        snprintf(cmd, sizeof cmd, "%s", !strcmp(c, "prev") ? "previous" : !strcmp(c, "pause") ? "pause 1" : c);
    }
    buf_t res = {0};
    int rc = mpd_cmd(fd, cmd, &res);
    close(fd);
    if (rc) {
        char m[300];
        char *nl = res.p ? strrchr(res.p, '\n') : NULL;
        if (nl) *nl = 0;
        char *last = res.p ? strrchr(res.p, '\n') : NULL;
        snprintf(m, sizeof m, "MPD: %s", last ? last + 1 : res.p ? res.p : "нет ответа");
        free(res.p);
        send_fail(r->fd, 400, m);
        return;
    }
    free(res.p);
    send_ok(r->fd, NULL);
}

static void h_index(req_t *r);

static const struct { const char *path; void (*fn)(req_t *); const char *help; } g_routes[] = {
    { "/",         h_index,    "этот список" },
    { "/status",   h_status,   "версия, вход (token, login, plus), качество, ссылка oauth" },
    { "/login",    h_login,    "POST token=<access_token или адрес страницы после входа>" },
    { "/logout",   h_logout,   "POST: удалить токен" },
    { "/quality",  h_quality,  "POST q=lossless|hq|nq" },
    { "/home",     h_home,     "плейлисты, «Для вас», станции" },
    { "/likes",    h_likes,    "«Мне нравится»" },
    { "/search",   h_search,   "?q=" },
    { "/album",    h_album,    "?id=" },
    { "/artist",   h_artist,   "?id=" },
    { "/playlist", h_playlist, "?uid=&kind=" },
    { "/now",      h_now,      "что играет" },
    { "/queue",    NULL,       "GET ?before=5&after=10 — соседи текущей; POST ids=id[:альбом],...&start=N[&append=1]" },
    { "/wave",     h_wave,     "POST seed=user:onyourwave | станция | album:<id> | artist:<id>" },
    { "/like",     h_like,     "POST id=&a=" },
    { "/dislike",  h_like,     "POST id=&a= — и следующий трек" },
    { "/next",     h_ctl,      "" },
    { "/prev",     h_ctl,      "" },
    { "/toggle",   h_ctl,      "пауза / игра" },
    { "/play",     h_ctl,      "?pos=N — песня очереди" },
    { "/pause",    h_ctl,      "" },
    { "/stop",     h_ctl,      "" },
};

static void h_index(req_t *r)
{
    cJSON *o = cJSON_CreateObject(), *e;
    cJSON_AddStringToObject(o, "name", "ymusicd");
    cJSON_AddStringToObject(o, "version", YM_VERSION);
    e = cJSON_AddObjectToObject(o, "endpoints");
    for (size_t i = 1; i < sizeof g_routes / sizeof g_routes[0]; i++)
        cJSON_AddStringToObject(e, g_routes[i].path, g_routes[i].help);
    send_obj(r, o);
}

static void serve_client_api(req_t *r)
{
    if (strcmp(r->method, "GET") && strcmp(r->method, "POST")) { send_fail(r->fd, 405, "GET или POST"); return; }
    if (!strcmp(r->path, "/queue")) {
        if (!strcmp(r->method, "POST")) h_queue_post(r);
        else h_queue_get(r);
        return;
    }
    for (size_t i = 0; i < sizeof g_routes / sizeof g_routes[0]; i++)
        if (g_routes[i].fn && !strcmp(r->path, g_routes[i].path)) { g_routes[i].fn(r); return; }
    send_fail(r->fd, 404, "нет такого адреса, список — GET /");
}

/* ---------- HTTP-сервер ---------- */
static int is_loopback(int fd)
{
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    if (getpeername(fd, (struct sockaddr *)&a, &l) || a.sin_family != AF_INET) return 0;
    return (ntohl(a.sin_addr.s_addr) >> 24) == 127;
}

static void *client_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char req[16384];
    char *body = NULL;
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
    char range[128] = "";
    long clen = 0;
    for (char *l = strstr(req, "\r\n"); l; l = strstr(l + 2, "\r\n")) {
        char *h = l + 2;
        if (!strncasecmp(h, "Range:", 6)) sscanf(h + 6, " %127[^\r\n]", range);
        else if (!strncasecmp(h, "Content-Length:", 15)) clen = strtol(h + 15, NULL, 10);
    }
    char *query = strchr(target, '?');
    if (query) *query++ = 0;
    logm(1, "%s %s%s%s %s", method, target, query ? "?" : "", query ? query : "", range);

    if (!strncmp(target, "/track/", 7)) {
        /* потоки — только для MPD на этой же плате: в них подписка владельца токена */
        if (!is_loopback(fd)) { reply_json_err(fd, 403, "только для MPD"); goto out; }
        if (strcmp(method, "GET") && strcmp(method, "HEAD")) { reply_json_err(fd, 400, "method"); goto out; }
        struct timeval tv2 = { 0, 0 };                    /* поток может идти долго */
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv2, sizeof tv2);
        serve_track(fd, method, target, query, range[0] ? range : NULL);
        goto out;
    }
    if (!strcmp(method, "POST") && clen > 0) {
        if (clen > 1024 * 1024) { send_fail(fd, 400, "слишком большое тело"); goto out; }
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
    }
    req_t rq = { fd, method, target, query, body };
    serve_client_api(&rq);
out:
    free(body);
    shutdown(fd, SHUT_RDWR);
    close(fd);
    __sync_fetch_and_sub(&g_clients, 1);
    return NULL;
}

static void usage(void)
{
    fprintf(stderr,
        "ymusicd " YM_VERSION " — Яндекс Музыка для MPD, API для приложения на ПК\n"
        "  -l адрес     слушать (0.0.0.0)\n"
        "  -p порт      (6601)\n"
        "  -t файл      токен OAuth (/var/lib/ymusic/token)\n"
        "  -q файл      качество lossless|hq|nq (/var/lib/ymusic/quality)\n"
        "  -m хост:порт MPD (127.0.0.1:6600)\n"
        "  -c папка     обложки и длительности треков очереди (/tmp/ymusic)\n"
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
