/*
 * ymusicd — Яндекс Музыка для PureFox (из DigiFox): API для приложения на ПК и свой плеер.
 *
 * HTTP на 0.0.0.0:6601. Ответы — JSON с "ok": true|false (при false — "msg" и код 4xx/5xx).
 * Параметры — в строке запроса или в теле POST (application/x-www-form-urlencoded).
 * Обложки — https-ссылки 400x400, длительности — в секундах. Полный список: GET /
 *
 *   Вход:     GET /status · POST /login token= · POST /logout · POST /quality q=lossless|hq|nq
 *   Музыка:   GET /home · /likes · /search?q= · /album?id= · /artist?id= · /playlist?uid=&kind=
 *   Очередь:  POST /queue ids=id[:альбом],...&start=N[&append=1] · POST /wave seed=
 *             GET /queue?before=5&after=10 · GET /now
 *   Пульт:    /next /prev /toggle /play[?pos=N] /pause /stop /seek?t=|d= · POST /like /dislike id=&a=
 *   Плата:    POST /output — только с 127.0.0.1, от S95ymusic: Фокс включает другой плеер
 *
 * Плеер. Ссылка на файл трека — из /get-file-info (подпись HMAC-SHA256 ключом десктопного
 * приложения), файл в транспорте encraw расшифровывается на лету (AES-128-CTR, ключ из ответа,
 * счётчик с нуля) в окно в памяти; перемотка — запрос к CDN с нужного места, обрыв — докачка.
 * FLAC декодирует libFLAC, MP3 — libmad (24 бита, задержка и добивка LAME — как в MPD).
 * Вывод — в ALSA "default" (/etc/asound.conf — выход, выбранный в Фоксе) без громкости и
 * пересчёта частоты: сэмпл как есть, при нужде в более широком контейнере. Следующий трек того
 * же формата идёт без паузы; начала соседних треков очереди заранее в памяти. Поток ALSA не
 * останавливается ни на паузе, ни при перемотке и смене трека (иначе на I2S гаснут такты и ЦАП
 * потом секунду-другую молчит): несыгранное забирается назад, пустоту заполняет тишина.
 * Для треков «Моей волны» — отзывы как у приложения (trackStarted, при уходе с прошлого
 * trackFinished или skip со временем прослушивания) и подкачка следующей партии в очередь,
 * когда до её конца остаётся меньше 3 треков.
 *
 * Играет, только когда Яндекс Музыка — плеер Фокса (есть /tmp/ymusic/active, его ставит и убирает
 * /etc/rc.pure/S95ymusic). Иначе /queue и /wave переключают Фокс на неё через handle_service.php,
 * как кнопка «Яндекс Музыка» на главной.
 *
 * Сборка:  cc -O2 -o ymusicd ymusicd.c cJSON.c -lcurl -lcrypto -lasound -lFLAC -lmad -lpthread
 * GPL-2.0+, перенесено из DigiFox (github.com/DrModd/DigiFox).
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
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

#include <alsa/asoundlib.h>
#include <curl/curl.h>
#include <FLAC/stream_decoder.h>
#include <mad.h>
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
#define MAX_QUEUE       500                                   /* треков за раз в очередь */
#define MAX_LIST        1000                                  /* треков в «Мне нравится» и плейлисте */
#define COVER_SIZE      "400x400"
#define OAUTH_URL       "https://oauth.yandex.ru/authorize?response_type=token&client_id=23cabbbdc6cd418abb4b39c32c41195d"

/* ---------- настройки ---------- */
static const char *g_listen  = "0.0.0.0";
static int         g_port    = 6601;
static const char *g_token_f = "/var/lib/ymusic/token";
static const char *g_qual_f  = "/var/lib/ymusic/quality";
static const char *g_api     = "https://api.music.yandex.net";
static const char *g_dev     = "default";            /* ALSA */
static const char *g_cover_d = "/tmp/ymusic";
static char        g_active_f[300];                   /* <g_cover_d>/active — Яндекс Музыка выбрана плеером */
static const char *g_transport = "encraw";
static int         g_verbose = 0;

static volatile int g_clients = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_wave_lock = PTHREAD_MUTEX_INITIALIZER;

/* Общие для всех запросов соединения, DNS и TLS-сессии: новое HTTPS-соединение
 * на этом процессоре стоит ~0,5 с (в основном подготовка TLS), повторное — ~30 мс */
static CURLSH *g_share;
static pthread_mutex_t g_share_mx[CURL_LOCK_DATA_LAST];

static void share_lock(CURL *h, curl_lock_data d, curl_lock_access a, void *u)
{
    (void)h; (void)a; (void)u;
    pthread_mutex_lock(&g_share_mx[d]);
}

static void share_unlock(CURL *h, curl_lock_data d, void *u)
{
    (void)h; (void)u;
    pthread_mutex_unlock(&g_share_mx[d]);
}

static void share_init(void)
{
    for (int i = 0; i < CURL_LOCK_DATA_LAST; i++) pthread_mutex_init(&g_share_mx[i], NULL);
    g_share = curl_share_init();
    if (!g_share) return;
    curl_share_setopt(g_share, CURLSHOPT_LOCKFUNC, share_lock);
    curl_share_setopt(g_share, CURLSHOPT_UNLOCKFUNC, share_unlock);
    curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    curl_share_setopt(g_share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
}

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
    curl_easy_setopt(c, CURLOPT_SHARE, g_share);
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
    double tt = 0;
    long nc = 0;
    curl_easy_getinfo(c, CURLINFO_TOTAL_TIME, &tt);
    curl_easy_getinfo(c, CURLINFO_NUM_CONNECTS, &nc);
    curl_slist_free_all(hs);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        logm(0, "API %s %s: %s", method, path_q, curl_easy_strerror(rc));
        return -1;
    }
    logm(1, "API %s %s -> %ld (%zu B, %.3f с%s)", method, path_q, *status, out->n, tt, nc ? ", новое соединение" : "");
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

    /* MP4-контейнер (flac-mp4, aac-mp4) плееру не по зубам, поэтому только flac и mp3 */
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

/* ---------- поток трека: CDN -> (расшифровка) ---------- */
typedef struct {
    int failed;
    long status;                 /* статус текущего ответа CDN */
    long long cr_start;          /* начало из Content-Range */
    long long clen;              /* Content-Length текущего ответа */
    long long total;             /* полный размер файла, если известен */
    long long fetch_from;        /* с какого смещения просили CDN в этом запросе */
    int fetch_started;           /* первый кусок тела этого запроса уже был */
    long long skip;              /* CDN не выполнил Range: столько байт выбросить */
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

/* шифр и ключ потока по ссылке на файл */
static void stream_setup(stream_t *s, const finfo_t *fi)
{
    s->enc = !strcmp(fi->transport, "encraw") && strlen(fi->key) == 32;
    if (s->enc)
        for (int k = 0; k < 16; k++) {
            char h[3] = { fi->key[2 * k], fi->key[2 * k + 1], 0 };
            s->key[k] = (unsigned char)strtol(h, NULL, 16);
        }
}

/* Начала соседних треков очереди (уже расшифрованные) в памяти: «дальше» и
 * «назад» плеер начинает сразу, а соединение с CDN для остального (~0,5–1 с
 * на этом процессоре) открывается, пока играет начало */
#define PRE_BYTES (512 * 1024)
typedef struct {
    char tid[32], quality[12], codec[16];
    unsigned char *data;
    size_t len;
    long long total;
    time_t t;
} pre_t;
static pre_t g_pre[4];

static int pre_have(const char *tid, const char *q)
{
    int r = 0;
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < sizeof g_pre / sizeof g_pre[0] && !r; i++)
        r = g_pre[i].data && !strcmp(g_pre[i].tid, tid) && !strcmp(g_pre[i].quality, q);
    pthread_mutex_unlock(&g_lock);
    return r;
}

/* копия начала трека (data — освободить) или 0 */
static int pre_take(const char *tid, pre_t *out)
{
    char qb[16];
    const char *q = get_quality(qb, sizeof qb);
    int r = 0;
    pthread_mutex_lock(&g_lock);
    for (size_t i = 0; i < sizeof g_pre / sizeof g_pre[0]; i++)
        if (g_pre[i].data && !strcmp(g_pre[i].tid, tid) && !strcmp(g_pre[i].quality, q)) {
            *out = g_pre[i];
            out->data = malloc(g_pre[i].len);
            if (out->data) { memcpy(out->data, g_pre[i].data, g_pre[i].len); r = 1; }
            break;
        }
    pthread_mutex_unlock(&g_lock);
    return r;
}

static size_t cb_pre(char *d, size_t sz, size_t n, void *u)
{
    buf_t *b = u;
    size_t len = sz * n, k = b->n + len > PRE_BYTES ? PRE_BYTES - b->n : len;
    if (k && buf_add(b, d, k)) return 0;
    return k == len ? len : 0;                 /* CDN не выполнил Range — хватит */
}

static void pre_fill(const char *tid)
{
    char qb[16], err[300], range[32];
    const char *q = get_quality(qb, sizeof qb);
    if (pre_have(tid, q)) return;
    finfo_t fi;
    if (file_info(tid, &fi, err, sizeof err)) return;
    stream_t s;
    memset(&s, 0, sizeof s);
    s.total = -1;
    s.clen = -1;
    stream_setup(&s, &fi);
    buf_t b = {0};
    CURL *c = curl_easy_init();
    if (!c) return;
    snprintf(range, sizeof range, "0-%d", PRE_BYTES - 1);
    curl_easy_setopt(c, CURLOPT_SHARE, g_share);
    curl_easy_setopt(c, CURLOPT_URL, fi.url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_RANGE, range);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, cb_hdr);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &s);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cb_pre);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (s.status == 200 && s.total < 0) s.total = s.clen;
    unsigned char *plain = NULL;
    if ((s.status == 200 || s.status == 206) && s.total > 0 && b.n &&
        (b.n == PRE_BYTES || (long long)b.n == s.total) && (plain = malloc(b.n + 32))) {
        int ol = 0;
        if (!s.enc) memcpy(plain, b.p, b.n);
        else if (ctr_init(&s, 0) || !EVP_DecryptUpdate(s.ctx, plain, &ol, (unsigned char *)b.p, (int)b.n) ||
                 ol != (int)b.n) { free(plain); plain = NULL; }
    }
    if (s.ctx) EVP_CIPHER_CTX_free(s.ctx);
    if (plain) {
        pthread_mutex_lock(&g_lock);
        size_t k = 0;
        for (size_t i = 0; i < sizeof g_pre / sizeof g_pre[0]; i++) {
            if (!strcmp(g_pre[i].tid, tid) || !g_pre[i].data) { k = i; break; }
            if (g_pre[i].t < g_pre[k].t) k = i;
        }
        free(g_pre[k].data);
        snprintf(g_pre[k].tid, sizeof g_pre[k].tid, "%s", tid);
        snprintf(g_pre[k].quality, sizeof g_pre[k].quality, "%s", q);
        snprintf(g_pre[k].codec, sizeof g_pre[k].codec, "%s", fi.codec);
        g_pre[k].data = plain;
        g_pre[k].len = b.n;
        g_pre[k].total = s.total;
        g_pre[k].t = time(NULL);
        pthread_mutex_unlock(&g_lock);
        logm(1, "трек %s: начало в памяти (%zu Б из %lld)", tid, b.n, s.total);
    }
    free(b.p);
}

/* ---------- файл трека для декодера ---------- */
/* Окно файла в памяти (кольцо SRC_CAP), его докачивает свой поток; начало из g_pre — сразу,
 * без сети. Чтение с места rpos; место далеко от окна (перемотка) — новый запрос к CDN
 * оттуда. Обрыв со стороны CDN или протухшая ссылка — новая ссылка и докачка с того же места */
#define SRC_CAP   (4 * 1024 * 1024)
#define SRC_BACK  (512 * 1024)                       /* прочитанное, что держим для перемотки назад */
#define SRC_AHEAD (512 * 1024)                       /* перемотка вперёд ближе — дождаться, без запроса */

static volatile unsigned g_gen;                      /* меняет команда плееру (другой трек, стоп) */
static void (*g_src_idle)(void);                     /* пока ждём данных: тишина в ALSA (поток плеера) */

typedef struct {
    char tid[32];
    unsigned gen;                                    /* g_gen при открытии: сменился — читать нечего */
    pthread_mutex_t mx;
    pthread_cond_t cv;
    unsigned char *buf;                              /* байт файла x — в buf[x % SRC_CAP] */
    long long lo, hi, rpos, total;                   /* в памяти [lo, hi), читаем с rpos; total < 0 — неизвестен */
    int restart, closing, done, failed, refs;
    stream_t s;
} src_t;

static void cond_init(pthread_cond_t *c)
{
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(c, &a);
    pthread_condattr_destroy(&a);
}

static void cond_wait_ms(pthread_cond_t *c, pthread_mutex_t *m, int ms)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(c, m, &t);
}

static void src_unref(src_t *s)
{
    pthread_mutex_lock(&s->mx);
    int last = --s->refs == 0;
    pthread_mutex_unlock(&s->mx);
    if (!last) return;
    if (s->s.ctx) EVP_CIPHER_CTX_free(s->s.ctx);
    pthread_cond_destroy(&s->cv);
    pthread_mutex_destroy(&s->mx);
    free(s->buf);
    free(s);
}

/* расшифрованное — в кольцо; -1 — бросить запрос (перемотка, закрытие) */
static int src_put(src_t *s, const unsigned char *d, size_t n)
{
    pthread_mutex_lock(&s->mx);
    while (n) {
        if (s->restart || s->closing) { pthread_mutex_unlock(&s->mx); return -1; }
        long long room = SRC_CAP - (s->hi - s->lo);
        if (!room) {
            long long keep = s->rpos - SRC_BACK;        /* давно прочитанное — освободить */
            if (keep > s->hi) keep = s->hi;
            if (keep > s->lo) s->lo = keep;
            else cond_wait_ms(&s->cv, &s->mx, 200);   /* полно: ждать читателя */
            continue;
        }
        size_t k = n < (size_t)room ? n : (size_t)room, at = (size_t)(s->hi % SRC_CAP);
        size_t first = SRC_CAP - at < k ? SRC_CAP - at : k;
        memcpy(s->buf + at, d, first);
        memcpy(s->buf, d + first, k - first);
        s->hi += (long long)k;
        d += k;
        n -= k;
        pthread_cond_broadcast(&s->cv);
    }
    pthread_mutex_unlock(&s->mx);
    return 0;
}

static size_t cb_src(char *d, size_t sz, size_t n, void *u)
{
    src_t *src = u;
    stream_t *s = &src->s;
    size_t len = sz * n;
    if (s->status != 200 && s->status != 206) { s->failed = 1; return 0; }
    if (!s->fetch_started) {
        /* откуда на самом деле начались данные: 206 — из Content-Range, 200 — с нуля */
        s->fetch_started = 1;
        if (s->status == 200 && s->clen >= 0) s->total = s->clen;
        long long actual = s->status == 206 ? s->cr_start : 0;
        if (actual != s->fetch_from) {
            if (actual > s->fetch_from || ctr_init(s, actual)) { s->failed = 1; return 0; }
            s->skip = s->fetch_from - actual;
        }
        if (s->total > 0) {
            pthread_mutex_lock(&src->mx);
            src->total = s->total;
            pthread_mutex_unlock(&src->mx);
        }
    }
    unsigned char out[65536 + 32];
    size_t off = 0;
    while (off < len) {
        size_t in = len - off > 65536 ? 65536 : len - off, chunk = in;
        const unsigned char *p = (unsigned char *)d + off;
        if (s->enc) {
            int ol = 0;
            if (!EVP_DecryptUpdate(s->ctx, out, &ol, p, (int)chunk)) { s->failed = 1; return 0; }
            p = out; chunk = (size_t)ol;
        }
        if (s->skip > 0) {
            size_t d0 = s->skip > (long long)chunk ? chunk : (size_t)s->skip;
            s->skip -= (long long)d0; p += d0; chunk -= d0;
        }
        if (chunk && src_put(src, p, chunk)) return 0;
        off += in;
    }
    return len;
}

static int cb_src_prog(void *u, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un)
{
    src_t *s = u;
    (void)dt; (void)dn; (void)ut; (void)un;
    return s->restart || s->closing;                 /* перемотка или файл закрыт — бросить запрос */
}

/* один запрос к CDN с from: 0 — ответ дочитан, 1 — брошен (перемотка, закрытие), -1 — ошибка */
static int src_fetch(src_t *src, const finfo_t *fi, long long from)
{
    stream_t *s = &src->s;
    CURL *c = curl_easy_init();
    if (!c) return -1;
    char range[48];
    s->status = 0; s->clen = -1; s->cr_start = 0; s->failed = 0;
    s->fetch_from = from; s->fetch_started = 0; s->skip = 0;
    curl_easy_setopt(c, CURLOPT_SHARE, g_share);
    curl_easy_setopt(c, CURLOPT_URL, fi->url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, cb_hdr);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, s);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cb_src);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, src);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, cb_src_prog);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, src);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);  /* 30 с без данных — докачка новым запросом */
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 65536L);
    if (from > 0) {
        snprintf(range, sizeof range, "%lld-", from);
        curl_easy_setopt(c, CURLOPT_RANGE, range);
    }
    CURLcode rc = curl_easy_perform(c);
    long code = 0, nc = 0, redir = 0;
    double tls = 0, ttfb = 0;
    char *eff = NULL;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_getinfo(c, CURLINFO_NUM_CONNECTS, &nc);
    curl_easy_getinfo(c, CURLINFO_REDIRECT_COUNT, &redir);
    curl_easy_getinfo(c, CURLINFO_APPCONNECT_TIME, &tls);
    curl_easy_getinfo(c, CURLINFO_STARTTRANSFER_TIME, &ttfb);
    curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
    if (g_verbose) {
        char host[128] = "";
        const char *h = eff ? strstr(eff, "://") : NULL;
        if (h) snprintf(host, sizeof host, "%.*s", (int)strcspn(h + 3, "/"), h + 3);
        logm(1, "CDN %s с %lld: соединений %ld, редиректов %ld, TLS %.3f, первый байт %.3f с",
             host, from, nc, redir, tls, ttfb);
    }
    curl_easy_cleanup(c);
    if (rc == CURLE_OK) return 0;
    if (src->restart || src->closing) return 1;
    logm(0, "трек %s: CDN %s (HTTP %ld, с %lld)", src->tid, curl_easy_strerror(rc), code, from);
    return -1;
}

static void *src_thread(void *arg)
{
    src_t *src = arg;
    finfo_t fi;
    char err[300];
    int fails = 0, have_fi = 0;
    pthread_mutex_lock(&src->mx);
    for (;;) {
        while (!src->closing && !src->restart && (src->done || src->failed))
            pthread_cond_wait(&src->cv, &src->mx);
        if (src->closing) break;
        if (src->restart) {                          /* читатель уже поставил lo = hi = rpos */
            src->restart = src->done = src->failed = 0;
            fails = 0;
        }
        long long from = src->hi;
        if (src->total >= 0 && from >= src->total) { src->done = 1; continue; }
        pthread_mutex_unlock(&src->mx);

        int r = -1;
        if (fails) { cache_drop(src->tid); have_fi = 0; }   /* ссылка могла протухнуть — новая */
        if (!have_fi && file_info(src->tid, &fi, err, sizeof err)) {
            logm(0, "трек %s: %s", src->tid, err);
        } else {
            have_fi = 1;
            stream_setup(&src->s, &fi);
            r = ctr_init(&src->s, from) ? -1 : src_fetch(src, &fi, from);
        }

        pthread_mutex_lock(&src->mx);
        if (src->closing || src->restart) continue;
        if (src->total >= 0 ? src->hi >= src->total : r == 0) {
            if (src->total < 0) src->total = src->hi;
            src->done = 1;
        } else {
            if (src->hi > from) fails = 0;
            if (++fails > 4) {
                logm(0, "трек %s: CDN не отдаёт файл", src->tid);
                src->failed = 1;
            } else {
                logm(0, "трек %s: докачка с %lld", src->tid, src->hi);
                pthread_mutex_unlock(&src->mx);
                usleep(300000 * fails);
                pthread_mutex_lock(&src->mx);
            }
        }
        pthread_cond_broadcast(&src->cv);
    }
    pthread_mutex_unlock(&src->mx);
    src_unref(src);
    return NULL;
}

static src_t *src_open(const char *tid, unsigned gen)
{
    src_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    if (!(s->buf = malloc(SRC_CAP))) { free(s); return NULL; }
    snprintf(s->tid, sizeof s->tid, "%s", tid);
    s->gen = gen;
    s->total = s->s.total = s->s.clen = -1;
    s->refs = 2;                                     /* читатель и поток докачки */
    pthread_mutex_init(&s->mx, NULL);
    cond_init(&s->cv);
    pre_t pre;
    if (pre_take(tid, &pre)) {
        size_t k = pre.len < SRC_CAP ? pre.len : SRC_CAP;
        memcpy(s->buf, pre.data, k);
        s->hi = (long long)k;
        s->total = s->s.total = pre.total;
        free(pre.data);
        logm(1, "трек %s: начало из памяти", tid);
    }
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 512 * 1024);
    if (pthread_create(&th, &at, src_thread, s)) { s->refs = 1; s->failed = 1; }
    pthread_attr_destroy(&at);
    return s;
}

/* поток докачки заканчивается сам, когда бросит запрос */
static void src_close(src_t *s)
{
    if (!s) return;
    pthread_mutex_lock(&s->mx);
    s->closing = 1;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mx);
    src_unref(s);
}

/* до n байт с места чтения: сколько прочитано, 0 — конец файла, -1 — файл не идёт или команда плееру */
static long src_read(src_t *s, void *out, size_t n)
{
    long r = -1;
    pthread_mutex_lock(&s->mx);
    while (g_gen == s->gen) {
        if (s->rpos < s->lo || s->rpos > s->hi + SRC_AHEAD) {   /* перемотка мимо окна */
            s->lo = s->hi = s->rpos;
            s->restart = 1;
            pthread_cond_broadcast(&s->cv);
        }
        if (s->rpos < s->hi) {
            size_t k = (size_t)(s->hi - s->rpos) < n ? (size_t)(s->hi - s->rpos) : n;
            size_t at = (size_t)(s->rpos % SRC_CAP), first = SRC_CAP - at < k ? SRC_CAP - at : k;
            memcpy(out, s->buf + at, first);
            memcpy((unsigned char *)out + first, s->buf, k - first);
            s->rpos += (long long)k;
            pthread_cond_broadcast(&s->cv);
            r = (long)k;
            break;
        }
        if (s->total >= 0 && s->rpos >= s->total) { r = 0; break; }
        if (s->failed) break;
        if (g_src_idle) {
            pthread_mutex_unlock(&s->mx);
            g_src_idle();
            pthread_mutex_lock(&s->mx);
        }
        cond_wait_ms(&s->cv, &s->mx, 50);
    }
    pthread_mutex_unlock(&s->mx);
    return r;
}

static long src_read_full(src_t *s, void *out, size_t n)
{
    size_t got = 0;
    while (got < n) {
        long r = src_read(s, (unsigned char *)out + got, n - got);
        if (r < 0) return -1;
        if (!r) break;
        got += (size_t)r;
    }
    return (long)got;
}

static void src_seek(src_t *s, long long pos)
{
    pthread_mutex_lock(&s->mx);
    s->rpos = pos < 0 ? 0 : pos;
    pthread_cond_broadcast(&s->cv);                  /* докачка ждёт места в кольце — оно могло появиться */
    pthread_mutex_unlock(&s->mx);
}

static long long src_tell(src_t *s)
{
    pthread_mutex_lock(&s->mx);
    long long r = s->rpos;
    pthread_mutex_unlock(&s->mx);
    return r;
}

static long long src_len(src_t *s)
{
    pthread_mutex_lock(&s->mx);
    long long r = s->total;
    pthread_mutex_unlock(&s->mx);
    return r;
}

/* ---------- декодеры: FLAC (libFLAC), MP3 (libmad) — в int32, каналы чередуются ---------- */
#define MP3_IN 32768

typedef struct {
    src_t *src;
    int flac;
    unsigned rate, ch, bits;                         /* bits — значащих в сэмпле int32 */
    double dur;                                      /* с, 0 — неизвестна */
    int kbps;
    int32_t *pcm;                                    /* декодированное */
    size_t pn, pcap;                                 /* кадров в pcm; вместимость, кадров */
    int held;                                        /* pcm от перемотки FLAC — отдать до декодирования */
    FLAC__StreamDecoder *fl;
    FLAC__uint64 samples;
    struct mad_stream ms;
    struct mad_frame mf;
    struct mad_synth sy;
    int mad, first, eof;                             /* first — первый кадр разобран, ещё не сыгран */
    unsigned char *in;
    long long data0;                                 /* начало кадров (после ID3v2) */
    unsigned long xframes;                           /* кадров по заголовку Xing, 0 — нет */
    unsigned char toc[100];
    int toc_ok;
    long long drop, left, total_out;                 /* сэмплов: выбросить в начале; осталось выдать (-1 — не считать) */
} dec_t;

static int pcm_room(dec_t *d, size_t frames)
{
    if (d->pn + frames <= d->pcap) return 0;
    size_t c = d->pcap ? d->pcap : 4096;
    while (c < d->pn + frames) c *= 2;
    int32_t *p = realloc(d->pcm, c * d->ch * sizeof *p);
    if (!p) return -1;
    d->pcm = p;
    d->pcap = c;
    return 0;
}

static FLAC__StreamDecoderReadStatus fl_read(const FLAC__StreamDecoder *f, FLAC__byte b[], size_t *n, void *u)
{
    (void)f;
    long r = src_read(((dec_t *)u)->src, b, *n);
    *n = r > 0 ? (size_t)r : 0;
    return r < 0 ? FLAC__STREAM_DECODER_READ_STATUS_ABORT
         : r ? FLAC__STREAM_DECODER_READ_STATUS_CONTINUE : FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
}

static FLAC__StreamDecoderSeekStatus fl_seek(const FLAC__StreamDecoder *f, FLAC__uint64 off, void *u)
{
    (void)f;
    src_seek(((dec_t *)u)->src, (long long)off);
    return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}

static FLAC__StreamDecoderTellStatus fl_tell(const FLAC__StreamDecoder *f, FLAC__uint64 *off, void *u)
{
    (void)f;
    *off = (FLAC__uint64)src_tell(((dec_t *)u)->src);
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

static FLAC__StreamDecoderLengthStatus fl_len(const FLAC__StreamDecoder *f, FLAC__uint64 *len, void *u)
{
    (void)f;
    long long t = src_len(((dec_t *)u)->src);
    if (t < 0) return FLAC__STREAM_DECODER_LENGTH_STATUS_UNSUPPORTED;
    *len = (FLAC__uint64)t;
    return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}

static FLAC__bool fl_eof(const FLAC__StreamDecoder *f, void *u)
{
    (void)f;
    src_t *s = ((dec_t *)u)->src;
    long long t = src_len(s);
    return t >= 0 && src_tell(s) >= t;
}

static FLAC__StreamDecoderWriteStatus fl_write(const FLAC__StreamDecoder *f, const FLAC__Frame *fr,
                                              const FLAC__int32 *const buf[], void *u)
{
    dec_t *d = u;
    (void)f;
    unsigned n = fr->header.blocksize;
    if (fr->header.channels != d->ch || fr->header.bits_per_sample != d->bits || pcm_room(d, n))
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    int32_t *o = d->pcm + d->pn * d->ch;
    for (unsigned i = 0; i < n; i++)
        for (unsigned c = 0; c < d->ch; c++) *o++ = buf[c][i];
    d->pn += n;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void fl_meta(const FLAC__StreamDecoder *f, const FLAC__StreamMetadata *m, void *u)
{
    dec_t *d = u;
    (void)f;
    if (m->type != FLAC__METADATA_TYPE_STREAMINFO) return;
    d->rate = m->data.stream_info.sample_rate;
    d->ch = m->data.stream_info.channels;
    d->bits = m->data.stream_info.bits_per_sample;
    d->samples = m->data.stream_info.total_samples;
}

static void fl_error(const FLAC__StreamDecoder *f, FLAC__StreamDecoderErrorStatus st, void *u)
{
    (void)f; (void)u;
    logm(1, "FLAC: %s", FLAC__StreamDecoderErrorStatusString[st]);
}

/* как MPD: округлить, обрезать, 24 бита */
static int32_t mad24(mad_fixed_t s)
{
    s += 1L << (MAD_F_FRACBITS - 24);
    if (s >= MAD_F_ONE) s = MAD_F_ONE - 1;
    else if (s < -MAD_F_ONE) s = -MAD_F_ONE;
    return s >> (MAD_F_FRACBITS + 1 - 24);
}

/* ещё данных для libmad: недоразобранное — в начало буфера, дочитать; 0 — конец уже отдан, -1 — ошибка */
static int mp3_fill(dec_t *d)
{
    if (d->eof) return 0;
    size_t keep = 0;
    if (d->ms.next_frame) {
        keep = (size_t)(d->ms.bufend - d->ms.next_frame);
        if (keep >= MP3_IN) keep = 0;                /* мусор без кадров — мимо */
        memmove(d->in, d->ms.next_frame, keep);
    }
    long r = src_read(d->src, d->in + keep, MP3_IN - keep);
    if (r < 0) return -1;
    if (!r) {                                        /* конец: libmad нужны нули за последним кадром */
        memset(d->in + keep, 0, MAD_BUFFER_GUARD);
        keep += MAD_BUFFER_GUARD;
        d->eof = 1;
    } else {
        keep += (size_t)r;
    }
    mad_stream_buffer(&d->ms, d->in, keep);
    d->ms.error = MAD_ERROR_NONE;
    return 1;
}

/* следующий кадр в d->mf: 1, конец файла — 0, ошибка — -1 */
static int mp3_frame(dec_t *d)
{
    for (;;) {
        if (!d->ms.buffer || d->ms.error == MAD_ERROR_BUFLEN) {
            int r = mp3_fill(d);
            if (r <= 0) return r;
        }
        if (!mad_frame_decode(&d->mf, &d->ms)) return 1;
        if (d->ms.error != MAD_ERROR_BUFLEN && !MAD_RECOVERABLE(d->ms.error)) return -1;
    }
}

/* заголовок Xing/Info в первом кадре (разбор как в MPD): кадров, оглавление; через 120 байт
 * от его начала — тег LAME с задержкой и добивкой кодера. 1 — кадр служебный, без звука */
static int mp3_xing(dec_t *d, unsigned spf)
{
    struct mad_bitptr p = d->ms.anc_ptr;
    int bl = (int)d->ms.anc_bitlen, bl0 = bl;
    if (bl < 16) return 0;
    unsigned long b = mad_bit_read(&p, 16);
    bl -= 16;
    if (b == ('X' << 8 | 'i') || b == ('I' << 8 | 'n')) {
        if (bl < 16 || mad_bit_read(&p, 16) != (b == ('X' << 8 | 'i') ? ('n' << 8 | 'g') : ('f' << 8 | 'o'))) return 0;
        bl -= 16;
    } else if (b != ('n' << 8 | 'g') && b != ('f' << 8 | 'o')) {
        return 0;
    }
    if (bl < 32) return 0;
    unsigned long fl = mad_bit_read(&p, 32);
    bl -= 32;
    if (fl & 1) { if (bl < 32) return 0; d->xframes = mad_bit_read(&p, 32); bl -= 32; }
    if (fl & 2) { if (bl < 32) return 0; mad_bit_skip(&p, 32); bl -= 32; }
    if (fl & 4) {
        if (bl < 800) return 0;
        for (int i = 0; i < 100; i++) d->toc[i] = (unsigned char)mad_bit_read(&p, 8);
        d->toc_ok = 1;
        bl -= 800;
    }
    if (fl & 8) { if (bl < 32) return 0; mad_bit_skip(&p, 32); bl -= 32; }
    int rest = 960 - (bl0 - bl);
    if (rest < 0) return 1;
    mad_bit_skip(&p, (unsigned)rest);
    bl -= rest;
    if (bl < 288 || !d->xframes) return 1;
    char enc[10];
    unsigned ma, mi;
    for (int i = 0; i < 9; i++) enc[i] = (char)mad_bit_read(&p, 8);
    enc[9] = 0;
    if (strncmp(enc, "LAME", 4) || sscanf(enc + 4, "%u.%u", &ma, &mi) != 2) return 1;
    mad_bit_skip(&p, 16 + 32 + 16 + 16 + 16);       /* версия тега, пик, усиления, флаги */
    unsigned delay = mad_bit_read(&p, 12), pad = mad_bit_read(&p, 12);
    /* libmad добавляет 529 сэмплов тишины в начало и убирает столько же в конце */
    d->drop = delay + 529;
    d->total_out = (long long)d->xframes * spf - d->drop - (pad > 529 ? pad - 529 : 0);
    if (d->total_out < 0) d->total_out = 0;
    d->left = d->total_out;
    return 1;
}

static int dec_open_mp3(dec_t *d, char *err, size_t esz)
{
    unsigned char h[10];
    if (!(d->in = malloc(MP3_IN + MAD_BUFFER_GUARD))) return -1;
    mad_stream_init(&d->ms);
    mad_frame_init(&d->mf);
    mad_synth_init(&d->sy);
    d->mad = 1;
    d->left = d->total_out = -1;
    if (src_read_full(d->src, h, sizeof h) == (long)sizeof h && !memcmp(h, "ID3", 3))   /* ID3v2 — мимо */
        d->data0 = 10 + ((h[6] & 0x7f) << 21 | (h[7] & 0x7f) << 14 | (h[8] & 0x7f) << 7 | (h[9] & 0x7f))
                 + (h[5] & 0x10 ? 10 : 0);
    src_seek(d->src, d->data0);
    if (mp3_frame(d) <= 0) { snprintf(err, esz, "MP3: нет кадров"); return -1; }
    d->rate = d->mf.header.samplerate;
    d->ch = MAD_NCHANNELS(&d->mf.header);
    d->bits = 24;
    d->kbps = (int)(d->mf.header.bitrate / 1000);
    unsigned spf = 32 * MAD_NSBSAMPLES(&d->mf.header);
    long long size = src_len(d->src);
    if (mp3_xing(d, spf)) {
        if (d->xframes) d->dur = (double)d->xframes * spf / d->rate;
        if (d->dur > 0 && size > d->data0) d->kbps = (int)((size - d->data0) * 8 / d->dur / 1000);
    } else {
        d->first = 1;                                /* обычный кадр — сыграть и его */
        if (d->kbps && size > d->data0) d->dur = (double)(size - d->data0) * 8 / (d->kbps * 1000.0);
    }
    return 0;
}

/* формат по первым байтам: fLaC — FLAC, иначе MP3 */
static int dec_open(dec_t *d, char *err, size_t esz)
{
    unsigned char m[4];
    long r = src_read_full(d->src, m, sizeof m);
    if (r < (long)sizeof m) { snprintf(err, esz, r < 0 ? "файл не идёт" : "файл пустой"); return -1; }
    src_seek(d->src, 0);
    d->flac = !memcmp(m, "fLaC", 4);
    if (!d->flac) return dec_open_mp3(d, err, esz);
    if (!(d->fl = FLAC__stream_decoder_new()) ||
        FLAC__stream_decoder_init_stream(d->fl, fl_read, fl_seek, fl_tell, fl_len, fl_eof, fl_write, fl_meta,
                                         fl_error, d) != FLAC__STREAM_DECODER_INIT_STATUS_OK ||
        !FLAC__stream_decoder_process_until_end_of_metadata(d->fl) || !d->rate || !d->ch || d->bits < 4) {
        snprintf(err, esz, "FLAC не читается");
        return -1;
    }
    if (d->samples) d->dur = (double)d->samples / d->rate;
    long long size = src_len(d->src);
    if (size > 0 && d->dur > 0) d->kbps = (int)(size * 8 / d->dur / 1000);
    return 0;
}

/* следующая порция в d->pcm: кадров (d->pn), 0 — конец, -1 — ошибка или команда плееру */
static long dec_read(dec_t *d)
{
    if (d->held) { d->held = 0; return (long)d->pn; }
    d->pn = 0;
    if (d->flac) {
        while (!d->pn) {
            if (FLAC__stream_decoder_get_state(d->fl) == FLAC__STREAM_DECODER_END_OF_STREAM) return 0;
            if (!FLAC__stream_decoder_process_single(d->fl)) return -1;
        }
        return (long)d->pn;
    }
    for (;;) {
        if (d->first) d->first = 0;
        else {
            int r = mp3_frame(d);
            if (r <= 0) return r;
        }
        mad_synth_frame(&d->sy, &d->mf);
        size_t n = d->sy.pcm.length, i = 0;
        if (d->drop > 0) {
            i = d->drop < (long long)n ? (size_t)d->drop : n;
            d->drop -= (long long)i;
        }
        if (d->left >= 0) {
            if ((long long)(n - i) > d->left) n = i + (size_t)d->left;
            d->left -= (long long)(n - i);
        }
        if (i >= n) {
            if (!d->left) return 0;
            continue;
        }
        if (pcm_room(d, n - i)) return -1;
        int32_t *o = d->pcm;
        unsigned sc = d->sy.pcm.channels;
        for (size_t j = i; j < n; j++)
            for (unsigned c = 0; c < d->ch; c++) *o++ = mad24(d->sy.pcm.samples[c < sc ? c : 0][j]);
        d->pn = n - i;
        return (long)d->pn;
    }
}

/* перемотка на t с от начала; 0 — получилось */
static int dec_seek(dec_t *d, double t)
{
    d->pn = 0;
    d->held = 0;
    if (t < 0) t = 0;
    if (d->flac) {
        FLAC__uint64 s = (FLAC__uint64)(t * d->rate);
        if (d->samples && s >= d->samples) s = d->samples - 1;
        if (FLAC__stream_decoder_seek_absolute(d->fl, s)) { d->held = d->pn > 0; return 0; }
        if (FLAC__stream_decoder_get_state(d->fl) == FLAC__STREAM_DECODER_SEEK_ERROR) FLAC__stream_decoder_flush(d->fl);
        d->pn = 0;
        return -1;
    }
    long long size = src_len(d->src), pos;
    if (d->dur <= 0 || size <= d->data0) return -1;
    double f = t / d->dur > 1 ? 1 : t / d->dur;
    if (d->toc_ok) {                                 /* VBR: оглавление Xing */
        double pc = f * 100;
        int a = pc > 99 ? 99 : (int)pc;
        double fa = d->toc[a], fb = a < 99 ? d->toc[a + 1] : 256;
        pos = d->data0 + (long long)((fa + (fb - fa) * (pc - a)) / 256 * (double)(size - d->data0));
    } else {
        pos = d->data0 + (long long)(f * (double)(size - d->data0));
    }
    src_seek(d->src, pos);
    mad_stream_finish(&d->ms);
    mad_stream_init(&d->ms);
    mad_frame_mute(&d->mf);
    mad_synth_mute(&d->sy);
    d->eof = d->first = 0;
    d->drop = 0;
    if (d->total_out >= 0) {
        d->left = d->total_out - (long long)(t * d->rate);
        if (d->left < 0) d->left = 0;
    }
    return 0;
}

static void dec_close(dec_t *d)
{
    if (d->fl) FLAC__stream_decoder_delete(d->fl);
    if (d->mad) {
        mad_synth_finish(&d->sy);
        mad_frame_finish(&d->mf);
        mad_stream_finish(&d->ms);
    }
    free(d->in);
    free(d->pcm);
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

/* ---------- очередь ---------- */
typedef struct {
    qtrack_t t;
    long id;                                         /* номер песни в очереди, растёт */
    char sid[160], batch[160], seed[160];            /* трек волны: сессия, партия, сид; иначе sid пусто */
} qent_t;

enum { PS_STOP, PS_PLAY, PS_PAUSE };
#define W_NONE (1LL << 62)                           /* w0: трек ещё не начат */

/* Очередь и что играет — под g_pl_mx. cur — песня, которую декодирует плеер; aud — что звучит:
 * при смене трека без паузы cur меняется раньше, aud — когда доиграет буфер ALSA */
static struct {
    qent_t *q;
    int n, cap;
    long next_id, cur, aud;
    int state;
    double seek;                                     /* >= 0 — перемотать cur сюда, с */
    double s0;                                       /* место aud, с которого идёт счёт, с */
    long long w0, played;                            /* кадров записано в ALSA к началу aud; сыграно */
    unsigned rate, ch, bits;                         /* формат aud */
    int kbps;
    double dur;
    char codec[8];
    int idle;                                        /* вывод ALSA закрыт */
} g_pl = { .next_id = 1, .cur = -1, .aud = -1, .seek = -1, .w0 = W_NONE, .idle = 1 };
static pthread_mutex_t g_pl_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pl_cv;                       /* команды плееру, вывод закрыт */
static pthread_cond_t g_ev_cv;                       /* сменилось, что звучит, — сторожу волны */

/* дальше — под g_pl_mx */
static int q_idx(long id)
{
    if (id < 0) return -1;
    for (int i = 0; i < g_pl.n; i++)
        if (g_pl.q[i].id == id) return i;
    return -1;
}

static int q_add(const qtrack_t *v, int n, const char *sid, const char *batch, const char *seed)
{
    if (g_pl.n + n > g_pl.cap) {
        int c = g_pl.cap ? g_pl.cap : 64;
        while (c < g_pl.n + n) c *= 2;
        qent_t *nq = realloc(g_pl.q, (size_t)c * sizeof *nq);
        if (!nq) return 0;
        g_pl.q = nq;
        g_pl.cap = c;
    }
    for (int i = 0; i < n; i++) {
        qent_t *e = &g_pl.q[g_pl.n++];
        memset(e, 0, sizeof *e);
        e->t = v[i];
        e->id = g_pl.next_id++;
        if (sid) {
            snprintf(e->sid, sizeof e->sid, "%s", sid);
            snprintf(e->batch, sizeof e->batch, "%s", batch ? batch : "");
            snprintf(e->seed, sizeof e->seed, "%s", seed ? seed : "");
        }
    }
    return n;
}

/* место aud, с */
static double pl_el(void)
{
    if (g_pl.state == PS_STOP) return 0;
    long long f = g_pl.played - g_pl.w0;
    return g_pl.s0 + (g_pl.rate && f > 0 ? (double)f / g_pl.rate : 0);
}

/* играть песню i с места t (state — PS_PLAY или PS_PAUSE) */
static void pl_go(int i, double t, int state)
{
    g_pl.cur = g_pl.aud = g_pl.q[i].id;
    g_pl.state = state;
    g_pl.seek = t;
    g_pl.s0 = t;
    g_pl.w0 = W_NONE;
    g_pl.rate = g_pl.ch = g_pl.bits = 0;
    g_pl.kbps = 0;
    g_pl.dur = 0;
    g_pl.codec[0] = 0;
    g_gen++;
    pthread_cond_broadcast(&g_pl_cv);
    pthread_cond_broadcast(&g_ev_cv);
}

static void pl_stop(void)
{
    g_pl.state = PS_STOP;
    g_pl.seek = -1;
    g_pl.s0 = 0;
    g_pl.w0 = W_NONE;
    g_gen++;
    pthread_cond_broadcast(&g_pl_cv);
    pthread_cond_broadcast(&g_ev_cv);
}

static void q_clear(void)
{
    g_pl.n = 0;
    g_pl.cur = g_pl.aud = -1;
    pl_stop();
}

/* ---------- вывод в ALSA (только поток плеера) ---------- */
static struct {
    snd_pcm_t *pcm;
    unsigned rate, ch, bits;                         /* поток */
    int bps, shift;                                  /* байт на сэмпл ALSA, сдвиг сэмпла влево */
    snd_pcm_uframes_t buffer, period;
    long long w;                                     /* кадров записано с открытия или сброса */
    unsigned char *cb;
    size_t ccap;
    int32_t *hist;                                   /* последние записанные кадры (буфер ALSA) — для паузы */
    size_t hcap, hpos, hn;                           /* вместимость, куда писать, сколько есть — в кадрах */
} g_out;

static void out_close(void)
{
    if (!g_out.pcm) return;
    snd_pcm_drop(g_out.pcm);
    snd_pcm_close(g_out.pcm);
    g_out.pcm = NULL;
    g_out.w = 0;
    logm(1, "ALSA: закрыт");
    pthread_mutex_lock(&g_pl_mx);
    g_pl.idle = 1;
    g_pl.played = 0;
    pthread_cond_broadcast(&g_pl_cv);
    pthread_mutex_unlock(&g_pl_mx);
}

/* Открыть g_dev под формат потока, как MPD с auto_format/auto_resample no: частота — только
 * своя, сэмпл — без изменений, при нужде в более широком контейнере (младшие биты — нули) */
static int out_open(unsigned rate, unsigned ch, unsigned bits, char *err, size_t esz)
{
    if (g_out.pcm && g_out.rate == rate && g_out.ch == ch && g_out.bits == bits) return 0;
    out_close();
    static const struct { snd_pcm_format_t f; int bytes, width; } fm[] = {
        { SND_PCM_FORMAT_S16_LE, 2, 16 }, { SND_PCM_FORMAT_S24_LE, 4, 24 },
        { SND_PCM_FORMAT_S32_LE, 4, 32 }, { SND_PCM_FORMAT_S24_3LE, 3, 24 },
    };
    static const int o16[] = { 0, 2, 1, 3, -1 }, o24[] = { 1, 2, 3, -1 }, o32[] = { 2, -1 };
    const int *ord = bits <= 16 ? o16 : bits <= 24 ? o24 : o32;
    snd_pcm_t *p;
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;
    int e = snd_pcm_open(&p, g_dev, SND_PCM_STREAM_PLAYBACK, 0), k = -1;
    if (e < 0) { snprintf(err, esz, "ALSA %s: %s", g_dev, snd_strerror(e)); return -1; }
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(p, hw);
    snd_pcm_hw_params_set_rate_resample(p, hw, 0);
    snd_pcm_hw_params_set_access(p, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (snd_pcm_hw_params_set_channels(p, hw, ch) < 0) {
        snprintf(err, esz, "ALSA: %u канала не поддерживаются", ch);
        goto fail;
    }
    for (const int *o = ord; *o >= 0 && k < 0; o++)
        if (!snd_pcm_hw_params_test_format(p, hw, fm[*o].f)) k = *o;
    if (k < 0 || snd_pcm_hw_params_set_format(p, hw, fm[k].f) < 0) {
        snprintf(err, esz, "ALSA: нет формата для %u бит", bits);
        goto fail;
    }
    if (snd_pcm_hw_params_set_rate(p, hw, rate, 0) < 0) {
        snprintf(err, esz, "ALSA: частота %u Гц не поддерживается", rate);
        goto fail;
    }
    unsigned bt = 500000, pt;                        /* буфер и период — как у MPD */
    snd_pcm_hw_params_set_buffer_time_near(p, hw, &bt, NULL);
    pt = bt / 4;
    snd_pcm_hw_params_set_period_time_near(p, hw, &pt, NULL);
    if ((e = snd_pcm_hw_params(p, hw)) < 0) {
        snprintf(err, esz, "ALSA: %s", snd_strerror(e));
        goto fail;
    }
    snd_pcm_hw_params_get_buffer_size(hw, &g_out.buffer);
    snd_pcm_hw_params_get_period_size(hw, &g_out.period, NULL);
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(p, sw);
    snd_pcm_sw_params_set_start_threshold(p, sw, g_out.buffer - g_out.period);
    snd_pcm_sw_params_set_avail_min(p, sw, g_out.period);
    snd_pcm_sw_params(p, sw);
    g_out.pcm = p;
    g_out.rate = rate;
    g_out.ch = ch;
    g_out.bits = bits;
    g_out.bps = fm[k].bytes;
    g_out.shift = fm[k].width - (int)bits;
    g_out.w = 0;
    g_out.hpos = g_out.hn = 0;
    if (g_out.hcap < g_out.buffer * ch) {
        free(g_out.hist);
        g_out.hist = malloc(g_out.buffer * ch * sizeof *g_out.hist);
        g_out.hcap = g_out.hist ? g_out.buffer * ch : 0;
    }
    logm(0, "ALSA %s: %u Гц, %u бит в %s, буфер %lu, период %lu", g_dev, rate, bits,
         snd_pcm_format_name(fm[k].f), (unsigned long)g_out.buffer, (unsigned long)g_out.period);
    pthread_mutex_lock(&g_pl_mx);
    g_pl.idle = 0;
    pthread_mutex_unlock(&g_pl_mx);
    return 0;
fail:
    snd_pcm_close(p);
    return -1;
}

/* кадры int32 -> формат ALSA -> устройство; -1 — устройство отказало */
static int out_write(const int32_t *s, size_t frames)
{
    size_t ns = frames * g_out.ch, need = ns * (size_t)g_out.bps;
    if (need > g_out.ccap) {
        unsigned char *nb = realloc(g_out.cb, need);
        if (!nb) return -1;
        g_out.cb = nb;
        g_out.ccap = need;
    }
    unsigned char *o = g_out.cb;
    int sh = g_out.shift;
    for (size_t i = 0; i < ns; i++) {
        uint32_t v = (uint32_t)s[i] << sh;
        o[0] = (unsigned char)v;
        o[1] = (unsigned char)(v >> 8);
        if (g_out.bps > 2) o[2] = (unsigned char)(v >> 16);
        if (g_out.bps > 3) o[3] = (unsigned char)(v >> 24);
        o += g_out.bps;
    }
    size_t hf = g_out.hcap / g_out.ch;               /* копия — в кольцо hist */
    for (size_t i = frames > hf ? frames - hf : 0; hf && i < frames; ) {
        size_t k = frames - i < hf - g_out.hpos ? frames - i : hf - g_out.hpos;
        memcpy(g_out.hist + g_out.hpos * g_out.ch, s + i * g_out.ch, k * g_out.ch * sizeof *s);
        g_out.hpos = (g_out.hpos + k) % hf;
        g_out.hn = g_out.hn + k > hf ? hf : g_out.hn + k;
        i += k;
    }
    o = g_out.cb;
    while (frames) {
        snd_pcm_sframes_t w = snd_pcm_writei(g_out.pcm, o, frames);
        if (w == -EAGAIN || w == -EINTR) continue;
        if (w < 0) {
            if (snd_pcm_recover(g_out.pcm, (int)w, 1) < 0) {
                logm(0, "ALSA: %s", snd_strerror((int)w));
                return -1;
            }
            logm(0, "ALSA: %s, дальше", snd_strerror((int)w));
            continue;
        }
        o += (size_t)w * g_out.ch * (size_t)g_out.bps;
        frames -= (size_t)w;
        g_out.w += w;
    }
    return 0;
}

/* Поток ALSA не останавливаем, пока играем: остановка (и пауза ALSA) на I2S гасит такты, ЦАП
 * потом секунду-другую ловит их заново и молчит. Поэтому записанное, но не сыгранное забираем
 * назад (snd_pcm_rewind), а когда писать нечего — подкладываем тишину */

/* забрать назад несыгранное, кроме периода и ещё 20 мс: указатель DMA на этом драйвере сдвигается
 * раз в период, и на столько DMA может уже уйти вперёд; keep — копия забранного (освободить).
 * Сколько кадров забрано */
static size_t out_unwrite(int32_t **keep)
{
    if (keep) *keep = NULL;
    if (!g_out.pcm) return 0;
    snd_pcm_sframes_t r = snd_pcm_rewindable(g_out.pcm), m = (snd_pcm_sframes_t)g_out.period + g_out.rate / 50;
    r = r > m ? r - m : 0;
    if (r > (snd_pcm_sframes_t)g_out.hn) r = (snd_pcm_sframes_t)g_out.hn;
    if (r > 0) r = snd_pcm_rewind(g_out.pcm, (snd_pcm_uframes_t)r);
    if (r <= 0) return 0;
    size_t n = (size_t)r, hf = g_out.hcap / g_out.ch, at = (g_out.hpos + hf - n) % hf;
    if (keep && (*keep = malloc(n * g_out.ch * sizeof **keep)))
        for (size_t i = 0, a = at; i < n; ) {
            size_t k = n - i < hf - a ? n - i : hf - a;
            memcpy(*keep + i * g_out.ch, g_out.hist + a * g_out.ch, k * g_out.ch * sizeof **keep);
            i += k;
            a = (a + k) % hf;
        }
    g_out.hpos = at;
    g_out.hn -= n;
    g_out.w -= r;
    return n;
}

/* в устройстве меньше периода и 0,2 с — дописать тишины до этого; сколько дописано */
static size_t out_idle(void)
{
    if (!g_out.pcm || snd_pcm_state(g_out.pcm) != SND_PCM_STATE_RUNNING) return 0;
    snd_pcm_sframes_t av = snd_pcm_avail_update(g_out.pcm), want = (snd_pcm_sframes_t)g_out.period + g_out.rate / 5;
    if (av < 0) return 0;
    snd_pcm_sframes_t q = (snd_pcm_sframes_t)g_out.buffer - av, n = want - q;
    if (n <= 0) return 0;
    if (n > av) n = av;
    int32_t *z = calloc((size_t)n * g_out.ch, sizeof *z);
    if (!z || out_write(z, (size_t)n)) n = 0;
    free(z);
    return (size_t)n;
}

/* ---------- плеер ---------- */
/* трек, который плеер начал писать в ALSA: станет aud, когда доиграет буфер прошлого */
static struct {
    int on;
    long id;
    unsigned gen;
    long long w;                                     /* g_out.w в его начале */
    double s0, dur;
    unsigned rate, ch, bits;
    int kbps;
    char codec[8];
} g_pend;

static void pend_set(const qent_t *e, const dec_t *d, unsigned gen, double s0)
{
    g_pend.on = 1;
    g_pend.id = e->id;
    g_pend.gen = gen;
    g_pend.w = g_out.w;
    g_pend.s0 = s0;
    g_pend.dur = d->dur;
    g_pend.rate = d->rate;
    g_pend.ch = d->ch;
    g_pend.bits = d->bits;
    g_pend.kbps = d->kbps;
    snprintf(g_pend.codec, sizeof g_pend.codec, "%s", d->flac ? "FLAC" : "MP3");
}

/* сколько сыграно; начатый трек зазвучал (или выбран командой) — он теперь aud */
static void pl_progress(void)
{
    snd_pcm_sframes_t dl = 0;
    if (!g_out.pcm || snd_pcm_delay(g_out.pcm, &dl) < 0 || dl < 0) dl = 0;
    pthread_mutex_lock(&g_pl_mx);
    g_pl.played = g_out.w - dl;
    if (g_pend.on && g_pend.gen == g_gen && (g_pl.aud == g_pend.id || g_pl.played >= g_pend.w)) {
        g_pend.on = 0;
        g_pl.aud = g_pend.id;
        g_pl.s0 = g_pend.s0;
        g_pl.w0 = g_pend.w;
        g_pl.dur = g_pend.dur;
        g_pl.rate = g_pend.rate;
        g_pl.ch = g_pend.ch;
        g_pl.bits = g_pend.bits;
        g_pl.kbps = g_pend.kbps;
        snprintf(g_pl.codec, sizeof g_pl.codec, "%s", g_pend.codec);
        pthread_cond_broadcast(&g_ev_cv);
    }
    pthread_mutex_unlock(&g_pl_mx);
}

/* доиграть записанное; 1 — пришла команда */
static int out_drain(unsigned gen)
{
    while (g_out.pcm) {
        if (g_gen != gen) return 1;
        snd_pcm_state_t st = snd_pcm_state(g_out.pcm);
        snd_pcm_sframes_t dl = 0;
        if (st == SND_PCM_STATE_PREPARED) snd_pcm_start(g_out.pcm);   /* короткий трек не набрал порога */
        else if (st != SND_PCM_STATE_RUNNING || snd_pcm_delay(g_out.pcm, &dl) < 0 || dl <= 0) return 0;
        pl_progress();
        usleep(20000);
    }
    return 0;
}

/* тишина, пока плеер ждёт данных трека: место звучащего трека на неё не сдвигается */
static void src_idle(void)
{
    size_t n = out_idle();
    if (!n || g_pend.on) return;
    pthread_mutex_lock(&g_pl_mx);
    if (g_pl.w0 != W_NONE) g_pl.w0 += (long long)n;
    pthread_mutex_unlock(&g_pl_mx);
}

/* Пауза: несыгранное — назад, пока пауза — тишина; при продолжении тишину — назад, несыгранное
 * пишем заново. Такты I2S всё время идут, ЦАП не замолкает */
static void pl_pause(unsigned gen)
{
    int32_t *keep;
    snd_pcm_sframes_t dl = 0;
    if (snd_pcm_delay(g_out.pcm, &dl) < 0) dl = -1;
    size_t n = out_unwrite(&keep);
    long long wr = g_out.w;                          /* сюда вернётся несыгранное */
    logm(1, "пауза: в буфере %ld, забрано назад %zu", (long)dl, n);
    pthread_mutex_lock(&g_pl_mx);
    long long w0 = g_pl.w0;
    if (w0 != W_NONE) {                              /* на паузе место стоит */
        if (g_pl.rate) g_pl.s0 += (double)(wr - w0) / g_pl.rate;
        g_pl.w0 = W_NONE;
    }
    while (g_pl.state == PS_PAUSE && g_gen == gen && g_pl.seek < 0) {
        pthread_mutex_unlock(&g_pl_mx);
        out_idle();
        pthread_mutex_lock(&g_pl_mx);
        cond_wait_ms(&g_pl_cv, &g_pl_mx, 50);
    }
    int go = g_pl.state == PS_PLAY && g_gen == gen && g_pl.seek < 0;
    pthread_mutex_unlock(&g_pl_mx);
    out_unwrite(NULL);                               /* тишину — назад */
    if (go) {
        long long at = g_out.w;
        if (g_pend.on && g_pend.w >= wr) g_pend.w += at - wr;
        if (keep) out_write(keep, n);
        pthread_mutex_lock(&g_pl_mx);
        if (w0 != W_NONE && g_pl.w0 == W_NONE) g_pl.w0 = at;
        pthread_mutex_unlock(&g_pl_mx);
        pl_progress();
    }
    free(keep);
}

/* играть трек до конца: 0 — доигран, 1 — команда, -1 — трек не играется, -2 — нет вывода */
static int play_track(const qent_t *e, unsigned gen, double seek)
{
    char err[300] = "";
    dec_t d;
    memset(&d, 0, sizeof d);
    if (access(g_active_f, F_OK)) { logm(0, "Яндекс Музыка не выбрана плеером Фокса"); return -2; }
    if (!(d.src = src_open(e->t.tid, gen))) return -1;
    int rc = dec_open(&d, err, sizeof err);
    if (!rc && seek > 0 && dec_seek(&d, seek)) seek = 0;
    if (!rc && g_out.pcm && (g_out.rate != d.rate || g_out.ch != d.ch || g_out.bits != d.bits)) {
        rc = out_drain(gen);                         /* формат другой: сначала доиграть прошлый */
        if (!rc) out_close();
    }
    if (!rc && out_open(d.rate, d.ch, d.bits, err, sizeof err)) rc = -2;
    if (!rc) {
        pend_set(e, &d, gen, seek);
        pl_progress();
    }
    while (!rc && g_gen == gen) {
        pthread_mutex_lock(&g_pl_mx);
        int st = g_pl.state;
        double sk = g_pl.seek;
        g_pl.seek = -1;
        pthread_mutex_unlock(&g_pl_mx);
        if (sk >= 0) {                               /* перемотка в этом же треке */
            out_unwrite(NULL);
            if (dec_seek(&d, sk)) logm(0, "трек %s: перемотка на %.0f с не вышла", e->t.tid, sk);
            pend_set(e, &d, gen, sk);
            pl_progress();
            continue;
        }
        if (st == PS_PAUSE) { pl_pause(gen); continue; }
        if (st != PS_PLAY) break;
        long n = dec_read(&d);
        if (n <= 0) { rc = n ? -1 : 0; break; }
        for (size_t off = 0; off < d.pn && g_gen == gen; ) {   /* по периоду: команды не ждут */
            size_t k = d.pn - off < g_out.period ? d.pn - off : g_out.period;
            if (out_write(d.pcm + off * d.ch, k)) { rc = -2; break; }
            off += k;
            pl_progress();
        }
    }
    if (g_gen != gen) {
        out_unwrite(NULL);                           /* дальше — следующий трек или тишина */
        rc = 1;
    } else if (rc == -1 || (rc == -2 && err[0])) {
        logm(0, "трек %s: %s", e->t.tid, err[0] ? err : "оборвался");
    }
    dec_close(&d);
    src_close(d.src);
    return rc;
}

static void *player_thread(void *arg)
{
    (void)arg;
    int errs = 0;
    pthread_mutex_lock(&g_pl_mx);
    for (;;) {
        int i = q_idx(g_pl.cur);
        if (g_pl.state != PS_PLAY || i < 0) {
            if (g_pl.state == PS_PLAY) pl_stop();
            if (g_pl.state == PS_STOP && g_out.pcm) {
                pthread_mutex_unlock(&g_pl_mx);
                out_close();
                pthread_mutex_lock(&g_pl_mx);
                continue;
            }
            if (g_pl.state == PS_PAUSE && g_out.pcm) {   /* пауза между треками: такты не гасить */
                pthread_mutex_unlock(&g_pl_mx);
                out_idle();
                pthread_mutex_lock(&g_pl_mx);
                cond_wait_ms(&g_pl_cv, &g_pl_mx, 50);
                continue;
            }
            pthread_cond_wait(&g_pl_cv, &g_pl_mx);
            continue;
        }
        qent_t e = g_pl.q[i];
        unsigned gen = g_gen;
        double seek = g_pl.seek > 0 ? g_pl.seek : 0;
        g_pl.seek = -1;
        pthread_mutex_unlock(&g_pl_mx);
        int r = play_track(&e, gen, seek);
        pthread_mutex_lock(&g_pl_mx);
        if (r == 1 || g_gen != gen) continue;        /* команда: что играть, уже решено */
        errs = r ? errs + 1 : 0;
        if (r == -2 || errs >= 3) {                  /* нет вывода или три трека подряд не играют */
            errs = 0;
            pl_stop();
            continue;
        }
        i = q_idx(e.id);
        if (i >= 0 && i + 1 < g_pl.n) {              /* дальше по очереди, без паузы */
            g_pl.cur = g_pl.q[i + 1].id;
            continue;
        }
        pthread_mutex_unlock(&g_pl_mx);               /* очередь кончилась: доиграть буфер */
        int c = out_drain(gen);
        pthread_mutex_lock(&g_pl_mx);
        if (!c && g_gen == gen) pl_stop();
    }
    return NULL;
}

/* Фокс сейчас за Яндекс Музыкой (файл ставит и убирает /etc/rc.pure/S95ymusic) */
static int player_active(void)
{
    return !access(g_active_f, F_OK);
}

/* активный плеер Фокса (naa, scream, ymusic...) */
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

/* Яндекс Музыка должна быть плеером Фокса (как кнопка «Яндекс Музыка» на главной) */
static int ensure_player(void)
{
    char svc[32];
    struct stat sb;
    if (player_active() && stat("/etc/usb_to_i2s.state", &sb)) return 0;
    active_service(svc, sizeof svc);
    logm(0, "переключение на Яндекс Музыку (был %s)", svc[0] ? svc : "?");
    CURL *c = curl_easy_init();
    if (c) {
        buf_t b = {0};
        curl_easy_setopt(c, CURLOPT_URL, "http://127.0.0.1/handle_service.php");
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, "service=ymusic");
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cb_mem);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_perform(c);
        curl_easy_cleanup(c);
        free(b.p);
    }
    for (int i = 0; i < 50 && !player_active(); i++) usleep(100000);
    return player_active() ? 0 : -1;
}

/* очередь: заменить (или дописать) и играть с v[start]; sid — треки волны.
 * Число добавленных, -1 — нечего играть, -2 — плеер не включился */
static int queue_play(qtrack_t *v, int n, int start, int append, const char *sid, const char *batch,
                      const char *seed, char *err, size_t esz)
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
    if (ensure_player()) { snprintf(err, esz, "не удалось включить Яндекс Музыку"); return -2; }
    pthread_mutex_lock(&g_pl_mx);
    if (!append) q_clear();
    int base = g_pl.n, k = q_add(v, m, sid, batch, seed);
    if (k && !append) pl_go(st, 0, PS_PLAY);
    else if (k && g_pl.state != PS_PLAY) pl_go(base, 0, PS_PLAY);
    pthread_mutex_unlock(&g_pl_mx);
    if (!k) { snprintf(err, esz, "нет памяти под очередь"); return -1; }
    return k;
}

/* песня очереди для /now и /queue; e NULL — пустая */
static cJSON *ent_json(const qent_t *e, int pos)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "song", pos > 0 ? pos : 0);
    cJSON_AddBoolToObject(o, "ym", e != NULL);
    cJSON_AddStringToObject(o, "id", e ? e->t.tid : "");
    cJSON_AddStringToObject(o, "a", e ? e->t.aid : "");
    cJSON_AddStringToObject(o, "title", e ? e->t.title : "");
    cJSON_AddStringToObject(o, "artist", e ? e->t.artist : "");
    cJSON_AddStringToObject(o, "album", e ? e->t.album : "");
    cJSON_AddNumberToObject(o, "dur", e ? e->t.dur : 0);
    cJSON_AddStringToObject(o, "cover", e ? e->t.cover : "");
    return o;
}

/* ---------- Моя волна ---------- */
/* Как приложение Яндекс Музыки: trackStarted в начале трека, при уходе с него — trackFinished
 * (дослушан) или skip (пропущен) со временем прослушивания; queue в запросе партии — сыгранное
 * до текущего включительно (последние WAVE_HIST). Очередь при пропуске не трогаем — как
 * приложение в режиме wave queue: пропуск влияет на следующие партии через отзыв. */
#define WAVE_HIST       100
#define WAVE_KEEP       100                                   /* сыгранного в очереди после чистки */
#define WAVE_KEEP_MAX   300                                   /* чистка, когда сыграно больше */

enum { WV_NONE, WV_FWD, WV_BACK, WV_DISLIKE };       /* как ушли с трека через наш API */

/* что играет в волне и что уже сыграно — под g_wv_mx */
static struct {
    long songid;                                     /* что звучит — по сторожу */
    double pos, pos_t;                               /* её место при последнем событии и когда */
    int playing;
    char hist[WAVE_HIST][80];                        /* id[:альбом], старые первыми */
    int hn;
    char cur[80], cur_sid[160], cur_batch[160];      /* songid — трек волны; иначе cur пусто */
    double cur_dur;
    int mark;                                        /* WV_*: ушли с cur через API */
    double mark_played, mark_t;
} g_wv = { .songid = -2 };
static pthread_mutex_t g_wv_mx = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    char sid[160], batch[160], seed[160];
    char hist[WAVE_HIST][80];
    int hn;
} wave_t;

static double mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* трек tid среди id[:альбом] */
static int id_in(char (*v)[80], int n, const char *tid)
{
    size_t l = strlen(tid);
    for (int i = 0; i < n; i++)
        if (!strncmp(v[i], tid, l) && (v[i][l] == 0 || v[i][l] == ':')) return 1;
    return 0;
}

/* отзыв в сессию волны; played < 0 — без totalPlayedSeconds, len <= 0 — без trackLengthSeconds */
static void rotor_feedback(const char *sid, const char *batch, const char *type, const char *id,
                           double played, double len)
{
    char ts[40], path[256];
    iso_now(ts, sizeof ts);
    cJSON *o = cJSON_CreateObject(), *ev = cJSON_AddObjectToObject(o, "event");
    cJSON_AddStringToObject(ev, "type", type);
    cJSON_AddStringToObject(ev, "timestamp", ts);
    if (id && id[0]) cJSON_AddStringToObject(ev, "trackId", id);
    if (played >= 0) cJSON_AddNumberToObject(ev, "totalPlayedSeconds", (long long)(played * 1000) / 1000.0);
    if (len > 0) cJSON_AddNumberToObject(ev, "trackLengthSeconds", len);
    if (batch && batch[0]) cJSON_AddStringToObject(o, "batchId", batch);
    char *body = cJSON_PrintUnformatted(o);
    char *esid = url_esc(sid);
    snprintf(path, sizeof path, "/rotor/session/%s/feedback", esid);
    cJSON_Delete(api_json("POST", path, ANDROID_CLIENT, body));
    if (played >= 0) logm(1, "волна: %s %s (%.0f с)", type, id ? id : "", played);
    else logm(1, "волна: %s %s", type, id ? id : "");
    free(esid); free(body); cJSON_Delete(o);
}

static void *wave_thread(void *arg)
{
    wave_t *w = arg;
    pthread_mutex_lock(&g_wave_lock);

    char (*next)[80] = NULL;                         /* что ещё в очереди после текущей */
    int nn = 0;
    cJSON *queue = NULL, *body = NULL, *r = NULL;
    pthread_mutex_lock(&g_pl_mx);
    int len = g_pl.n, song = q_idx(g_pl.aud);
    if (song >= 0 && len - song - 1 < WAVE_LOW && (next = calloc((size_t)len + 1, sizeof *next)))
        for (int i = song + 1; i < len; i++) snprintf(next[nn++], sizeof next[0], "%s", g_pl.q[i].t.tid);
    pthread_mutex_unlock(&g_pl_mx);
    if (!next) goto done;

    /* сыгранное до текущего включительно — как queue у приложения */
    queue = cJSON_CreateArray();
    for (int i = 0; i < w->hn; i++) cJSON_AddItemToArray(queue, cJSON_CreateString(w->hist[i]));

    char path[300];
    body = cJSON_CreateObject();
    cJSON_AddItemReferenceToObject(body, "queue", queue);
    char *esid = url_esc(w->sid);
    snprintf(path, sizeof path, "/rotor/session/%s/tracks", esid);
    free(esid);
    char *bs = cJSON_PrintUnformatted(body);
    r = api_json("POST", path, ANDROID_CLIENT, bs);
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
    qlist_t ql = {0};
    cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(res, "sequence")) ql_push(&ql, cJSON_GetObjectItem(it, "track"));
    /* уже игранное и уже стоящее в очереди — мимо; если там одни повторы — лучше они, чем тишина */
    int m = 0;
    for (int pass = 0; pass < 2 && !m; pass++)
        for (int i = 0; i < ql.n; i++)
            if (ql.v[i].ok && (pass || !(id_in(w->hist, w->hn, ql.v[i].tid) || id_in(next, nn, ql.v[i].tid))))
                ql.v[m++] = ql.v[i];
    pthread_mutex_lock(&g_pl_mx);
    if (m) {
        int k = q_add(ql.v, m, sid, cJSON_GetStringValue(cJSON_GetObjectItem(res, "batchId")), w->seed);
        logm(1, "волна: +%d в очередь (прислано %d)", k, ql.n);
    }
    /* очередь не растёт бесконечно, но и не сдвигается под приложением: сыгранное
     * убираем редко (за WAVE_KEEP_MAX — примерно сутки подряд), оставляя WAVE_KEEP */
    song = q_idx(g_pl.aud);
    if (song > WAVE_KEEP_MAX) {
        int k = song - WAVE_KEEP;
        memmove(g_pl.q, g_pl.q + k, (size_t)(g_pl.n - k) * sizeof *g_pl.q);
        g_pl.n -= k;
    }
    pthread_mutex_unlock(&g_pl_mx);
    free(ql.v);
done:
    cJSON_Delete(r);
    cJSON_Delete(body);
    cJSON_Delete(queue);
    free(next);
    pthread_mutex_unlock(&g_wave_lock);
    free(w);
    return NULL;
}

/* Соседи песни в очереди — заранее: начало файла в памяти (pre_fill), заодно
 * ссылка в кэше и соединения в общем пуле */
static void *prefetch_thread(void *arg)
{
    long id = (long)(intptr_t)arg;
    char next[32] = "", prev[32] = "";
    sleep(3);                                  /* не мешать началу текущего трека */
    pthread_mutex_lock(&g_pl_mx);
    int i = q_idx(id);
    if (i >= 0 && i + 1 < g_pl.n) snprintf(next, sizeof next, "%s", g_pl.q[i + 1].t.tid);
    if (i > 0) snprintf(prev, sizeof prev, "%s", g_pl.q[i - 1].t.tid);
    pthread_mutex_unlock(&g_pl_mx);
    if (next[0]) pre_fill(next);
    if (prev[0]) pre_fill(prev);
    return NULL;
}

static void prefetch_around(long id)
{
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 256 * 1024);
    pthread_create(&th, &at, prefetch_thread, (void *)(intptr_t)id);
    pthread_attr_destroy(&at);
}

/* зазвучала другая песня (e, NULL — нет песни): отзыв об уходе с прошлого трека волны,
 * trackStarted новому, подкачка. el, playing, now — её место и состояние */
static void wave_song(long songid, const qent_t *e, double dur, double el, int playing, double now)
{
    char id[80] = "";
    char prev[80] = "", psid[160] = "", pbatch[160] = "";
    const char *ptype = NULL;                        /* NULL — об уходе не сообщать */
    double pplayed = -1, plen = 0;
    wave_t *w = calloc(1, sizeof *w);
    if (!w) return;
    int wave = e && e->sid[0];
    if (wave) {
        snprintf(w->sid, sizeof w->sid, "%s", e->sid);
        snprintf(w->batch, sizeof w->batch, "%s", e->batch);
        snprintf(w->seed, sizeof w->seed, "%s", e->seed);
        snprintf(id, sizeof id, "%s%s%s", e->t.tid, e->t.aid[0] ? ":" : "", e->t.aid);
    }

    pthread_mutex_lock(&g_wv_mx);
    if (g_wv.cur[0]) {
        /* место прошлой при смене: последнее известное плюс время игры с тех пор (паузы — мимо) */
        double len = g_wv.cur_dur, pos = g_wv.pos + (g_wv.playing ? now - g_wv.pos_t : 0);
        int k = g_wv.mark && now - g_wv.mark_t < 15 ? g_wv.mark : WV_NONE;
        snprintf(prev, sizeof prev, "%s", g_wv.cur);
        snprintf(psid, sizeof psid, "%s", g_wv.cur_sid);
        snprintf(pbatch, sizeof pbatch, "%s", g_wv.cur_batch);
        if (k != WV_NONE) {                          /* next / prev / play pos / dislike */
            ptype = k == WV_DISLIKE ? NULL : "skip"; /* за dislike отзыв уже ушёл */
            pplayed = g_wv.mark_played;
        } else if (len <= 0 || pos >= len - 5) {
            ptype = "trackFinished";
            pplayed = len > 0 ? len : pos;
            plen = len;
        } else {                                     /* очередь заменили, стоп */
            ptype = "skip";
            pplayed = pos;
        }
    }
    g_wv.songid = songid;
    g_wv.pos = el;
    g_wv.pos_t = now;
    g_wv.playing = playing;
    g_wv.cur[0] = 0;
    g_wv.mark = WV_NONE;
    if (wave) {
        snprintf(g_wv.cur, sizeof g_wv.cur, "%s", id);
        snprintf(g_wv.cur_sid, sizeof g_wv.cur_sid, "%s", w->sid);
        snprintf(g_wv.cur_batch, sizeof g_wv.cur_batch, "%s", w->batch);
        g_wv.cur_dur = dur;
        if (!id_in(g_wv.hist, g_wv.hn, e->t.tid)) {
            if (g_wv.hn == WAVE_HIST) {
                memmove(g_wv.hist, g_wv.hist + 1, (WAVE_HIST - 1) * sizeof g_wv.hist[0]);
                g_wv.hn--;
            }
            snprintf(g_wv.hist[g_wv.hn++], sizeof g_wv.hist[0], "%s", id);
        }
        memcpy(w->hist, g_wv.hist, (size_t)g_wv.hn * sizeof g_wv.hist[0]);
        w->hn = g_wv.hn;
    }
    pthread_mutex_unlock(&g_wv_mx);

    /* отзывы — здесь, по порядку; подкачка — в своём потоке */
    if (ptype) rotor_feedback(psid, pbatch, ptype, prev, pplayed, plen);
    if (!wave) { free(w); return; }
    rotor_feedback(w->sid, w->batch, "trackStarted", id, -1, 0);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 256 * 1024);
    if (pthread_create(&th, &at, wave_thread, w)) free(w);
    pthread_attr_destroy(&at);
}

/* Сторож: зазвучала другая песня (не та, что плеер открыл заранее) — отзывы волны, соседей
 * в память; иначе запоминает место и состояние для отзыва при смене */
static void *wave_watch(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_pl_mx);
    for (;;) {
        long id = g_pl.aud;
        int i = q_idx(id), playing = g_pl.state == PS_PLAY;
        double now = mono_now(), el = pl_el();
        pthread_mutex_lock(&g_wv_mx);
        int changed = id != g_wv.songid;
        if (!changed) {
            g_wv.pos = el;
            g_wv.pos_t = now;
            g_wv.playing = playing;
        }
        pthread_mutex_unlock(&g_wv_mx);
        if (!changed) {
            cond_wait_ms(&g_ev_cv, &g_pl_mx, 5000);
            continue;
        }
        static qent_t e;                             /* только этот поток */
        if (i >= 0) e = g_pl.q[i];
        double dur = g_pl.dur > 0 ? g_pl.dur : i >= 0 ? e.t.dur : 0;
        pthread_mutex_unlock(&g_pl_mx);
        wave_song(id, i >= 0 ? &e : NULL, dur, el, playing, now);
        if (i >= 0) prefetch_around(id);
        pthread_mutex_lock(&g_pl_mx);                /* пока шли отзывы, могло смениться ещё */
    }
    return NULL;
}

/* что звучит — трек волны, о котором знает сторож */
typedef struct { char id[80], sid[160], batch[160]; double played; long song; } wcur_t;

static int wave_cur(wcur_t *c)
{
    memset(c, 0, sizeof *c);
    pthread_mutex_lock(&g_pl_mx);
    long songid = g_pl.aud;
    c->song = q_idx(songid);
    c->played = pl_el();
    pthread_mutex_unlock(&g_pl_mx);
    pthread_mutex_lock(&g_wv_mx);
    int ok = c->song >= 0 && songid == g_wv.songid && g_wv.cur[0];
    if (ok) {
        snprintf(c->id, sizeof c->id, "%s", g_wv.cur);
        snprintf(c->sid, sizeof c->sid, "%s", g_wv.cur_sid);
        snprintf(c->batch, sizeof c->batch, "%s", g_wv.cur_batch);
    }
    pthread_mutex_unlock(&g_wv_mx);
    return ok;
}

/* уходим с трека через наш API: сторож при смене пошлёт skip с этим временем, а не trackFinished */
static void wave_mark(const wcur_t *c, int kind)
{
    pthread_mutex_lock(&g_wv_mx);
    if (kind != WV_NONE && c->id[0] && !strcmp(g_wv.cur, c->id)) {
        g_wv.mark = kind;
        g_wv.mark_played = c->played;
        g_wv.mark_t = mono_now();
    }
    pthread_mutex_unlock(&g_wv_mx);
}

/* очередь заменяют (/queue, /wave): треку волны — skip, как приложение при смене контекста;
 * история волны — заново */
static void wave_leave(void)
{
    wcur_t c;
    if (wave_cur(&c)) rotor_feedback(c.sid, c.batch, "skip", c.id, c.played, 0);
    pthread_mutex_lock(&g_wv_mx);                    /* songid и место — сторожу, как были */
    g_wv.hn = 0;
    g_wv.cur[0] = 0;
    g_wv.mark = WV_NONE;
    pthread_mutex_unlock(&g_wv_mx);
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
    cJSON *o;
    char svc[32], v[64];
    if (!player_active()) {
        o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "state", "off");
        send_obj(r, o);
        return;
    }
    pthread_mutex_lock(&g_pl_mx);
    int i = q_idx(g_pl.aud), st = g_pl.state;
    o = ent_json(i >= 0 ? &g_pl.q[i] : NULL, i);
    cJSON_AddStringToObject(o, "state", st == PS_PLAY ? "play" : st == PS_PAUSE ? "pause" : "stop");
    cJSON_AddBoolToObject(o, "wave", i >= 0 && g_pl.q[i].sid[0]);
    cJSON_AddNumberToObject(o, "pos", pl_el());
    if (g_pl.dur > 0) cJSON_ReplaceItemInObject(o, "dur", cJSON_CreateNumber(g_pl.dur));
    cJSON_AddNumberToObject(o, "n", g_pl.n);
    v[0] = 0;
    if (st != PS_STOP && g_pl.rate) snprintf(v, sizeof v, "%u:%u:%u", g_pl.rate, g_pl.bits, g_pl.ch);
    cJSON_AddStringToObject(o, "audio", v);
    cJSON_AddNumberToObject(o, "kbps", st != PS_STOP ? g_pl.kbps : 0);
    cJSON_AddStringToObject(o, "codec", g_pl.codec);
    pthread_mutex_unlock(&g_pl_mx);
    active_service(svc, sizeof svc);
    cJSON_AddStringToObject(o, "svc", svc);
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
    cJSON *prev = cJSON_AddArrayToObject(o, "prev"), *next = cJSON_AddArrayToObject(o, "next");
    if (!player_active()) {
        cJSON_AddStringToObject(o, "state", "off");
        cJSON_AddNumberToObject(o, "song", -1);
        cJSON_AddNumberToObject(o, "n", 0);
        cJSON_AddNullToObject(o, "current");
        send_obj(r, o);
        return;
    }
    pthread_mutex_lock(&g_pl_mx);
    int n = g_pl.n, song = q_idx(g_pl.aud), st = g_pl.state;
    int from = song - nb > 0 ? song - nb : 0, to = song + 1 + na < n ? song + 1 + na : n;
    cJSON_AddStringToObject(o, "state", st == PS_PLAY ? "play" : st == PS_PAUSE ? "pause" : "stop");
    cJSON_AddNumberToObject(o, "song", song);
    cJSON_AddNumberToObject(o, "n", n);
    for (int i = from; i < to; i++) {                /* без текущей (song = -1) — начало очереди */
        if (i == song) continue;
        cJSON_AddItemToArray(i < song ? prev : next, ent_json(&g_pl.q[i], i));
    }
    if (song >= 0) cJSON_AddItemToObject(o, "current", ent_json(&g_pl.q[song], song));
    else cJSON_AddNullToObject(o, "current");
    pthread_mutex_unlock(&g_pl_mx);
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
    if (!append) wave_leave();
    int k = queue_play(ql.v, ql.n, s, append, NULL, NULL, NULL, err, sizeof err);
    free(ql.v);
    if (k < 0) { send_fail(r->fd, k == -2 ? 503 : 400, err); return; }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "added", k);
    send_obj(r, o);
}

/* «Моя волна» (seed=user:onyourwave) или станция / альбом / исполнитель (seed из ответов) */
static void h_wave(req_t *r)
{
    char seed[160] = "", err[300], sid[160] = "", batch[160] = "";
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
    wave_leave();
    rotor_feedback(sid, NULL, "radioStarted", NULL, -1, 0);   /* как приложение: без batchId */
    int k = queue_play(ql.v, ql.n, 0, 0, sid, batch, seed, err, sizeof err);
    free(ql.v);
    if (k < 0) { send_fail(r->fd, k == -2 ? 503 : 400, err); return; }
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
    /* играющий трек волны — отзыв и в её сессию, как в приложении */
    wcur_t wc;
    int wv = wave_cur(&wc), cur = wv && id_in(&wc.id, 1, id);
    if (cur) rotor_feedback(wc.sid, wc.batch, like ? "like" : "dislike", wc.id, like ? -1 : wc.played, 0);
    if (!like && player_active()) {
        if (wv) wave_mark(&wc, cur ? WV_DISLIKE : WV_FWD);
        pthread_mutex_lock(&g_pl_mx);
        int i = q_idx(g_pl.aud);
        if (i >= 0 && i + 1 < g_pl.n) pl_go(i + 1, 0, PS_PLAY);
        else if (i >= 0) pl_stop();
        pthread_mutex_unlock(&g_pl_mx);
    }
    send_ok(r->fd, NULL);
}

/* секунды: 12, 12.5; со знаком (sign) — и -5, +5 */
static int secs_ok(const char *s, int sign)
{
    if (sign && (*s == '-' || *s == '+')) s++;
    if (!isdigit((unsigned char)*s)) return 0;
    while (isdigit((unsigned char)*s)) s++;
    if (*s == '.') { s++; if (!isdigit((unsigned char)*s)) return 0; while (isdigit((unsigned char)*s)) s++; }
    return !*s;
}

/* next | prev | toggle | play [pos] | pause | stop | seek t|d */
static void h_ctl(req_t *r)
{
    const char *c = r->path + 1;
    char v[16], m[120] = "";
    long to = -1;                                    /* play pos */
    double t = 0;
    int rel = 0;
    if (!player_active()) { send_fail(r->fd, 503, "Яндекс Музыка не выбрана плеером"); return; }
    if (!strcmp(c, "seek")) {
        /* t — с начала трека, d — от текущего места (+ в адресе — %2B, но и без знака вперёд) */
        if (prm(r, "t", v, sizeof v) && secs_ok(v, 0)) t = strtod(v, NULL);
        else if (prm(r, "d", v, sizeof v) && secs_ok(v, 1)) { t = strtod(v, NULL); rel = 1; }
        else { send_fail(r->fd, 400, "нужно t=<секунды> или d=<±секунды>"); return; }
    } else if (!strcmp(c, "play") && prm(r, "pos", v, sizeof v) && all_digits(v)) {
        to = strtol(v, NULL, 10);
    }
    /* уход с трека волны: сторож пошлёт skip, а не trackFinished */
    int kind = !strcmp(c, "next") ? WV_FWD : !strcmp(c, "prev") ? WV_BACK : WV_NONE;
    wcur_t wc;
    if ((kind != WV_NONE || to >= 0) && wave_cur(&wc)) {
        if (to >= 0) kind = to > wc.song ? WV_FWD : to < wc.song ? WV_BACK : WV_NONE;
        wave_mark(&wc, kind);
    }
    pthread_mutex_lock(&g_pl_mx);
    int i = q_idx(g_pl.aud), st = g_pl.state;
    if (!strcmp(c, "next")) {
        if (i >= 0 && i + 1 < g_pl.n) pl_go(i + 1, 0, PS_PLAY);
        else pl_stop();
    } else if (!strcmp(c, "prev")) {
        if (g_pl.n) pl_go(i > 0 ? i - 1 : 0, 0, PS_PLAY);
    } else if (!strcmp(c, "play") && to >= 0) {
        if (to < g_pl.n) pl_go((int)to, 0, PS_PLAY);
        else snprintf(m, sizeof m, "нет песни %ld в очереди", to);
    } else if (!strcmp(c, "play") || (!strcmp(c, "toggle") && st != PS_PLAY)) {
        if (st == PS_PAUSE) {
            g_pl.state = PS_PLAY;
            pthread_cond_broadcast(&g_pl_cv);
            pthread_cond_broadcast(&g_ev_cv);
        } else if (st == PS_STOP) {
            if (g_pl.n) pl_go(i >= 0 ? i : 0, 0, PS_PLAY);
            else snprintf(m, sizeof m, "очередь пуста");
        }
    } else if (!strcmp(c, "pause") || !strcmp(c, "toggle")) {
        if (st == PS_PLAY) {
            g_pl.state = PS_PAUSE;
            pthread_cond_broadcast(&g_pl_cv);
            pthread_cond_broadcast(&g_ev_cv);
        }
    } else if (!strcmp(c, "stop")) {
        pl_stop();
    } else if (st == PS_STOP || i < 0) {             /* seek */
        snprintf(m, sizeof m, "ничего не играет");
    } else {
        if (rel) t += pl_el();
        if (t < 0) t = 0;
        if (g_pl.dur > 0 && t > g_pl.dur - 1) t = g_pl.dur > 1 ? g_pl.dur - 1 : 0;
        if (g_pl.cur == g_pl.aud) {                  /* плеер в этом треке: перемотать на месте */
            g_pl.seek = t;
            g_pl.s0 = t;
            g_pl.w0 = W_NONE;
            pthread_cond_broadcast(&g_pl_cv);
            pthread_cond_broadcast(&g_ev_cv);
        } else {
            pl_go(i, t, st);
        }
    }
    pthread_mutex_unlock(&g_pl_mx);
    if (m[0]) send_fail(r->fd, 400, m);
    else send_ok(r->fd, NULL);
}

static int is_loopback(int fd);

/* POST, только с платы: /etc/rc.pure/S95ymusic убрал g_active_f (Фокс включает другой плеер) —
 * остановиться и закрыть ALSA; ответ, когда устройство свободно */
static void h_output(req_t *r)
{
    if (!is_loopback(r->fd)) { send_fail(r->fd, 403, "только с платы"); return; }
    if (player_active()) { send_ok(r->fd, "on"); return; }
    pthread_mutex_lock(&g_pl_mx);
    if (g_pl.state != PS_STOP) pl_stop();
    for (int i = 0; i < 50 && !g_pl.idle; i++) cond_wait_ms(&g_pl_cv, &g_pl_mx, 100);
    int idle = g_pl.idle;
    pthread_mutex_unlock(&g_pl_mx);
    if (idle) send_ok(r->fd, "off");
    else send_fail(r->fd, 503, "ALSA ещё занят");
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
    { "/seek",     h_ctl,      "?t=<сек> — с начала трека | ?d=<±сек> — от текущего места" },
    { "/output",   h_output,   "POST, только с платы: Фокс включил другой плеер — освободить ALSA" },
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
    long clen = 0;
    for (char *l = strstr(req, "\r\n"); l; l = strstr(l + 2, "\r\n")) {
        char *h = l + 2;
        if (!strncasecmp(h, "Content-Length:", 15)) clen = strtol(h + 15, NULL, 10);
    }
    char *query = strchr(target, '?');
    if (query) *query++ = 0;
    logm(1, "%s %s%s%s", method, target, query ? "?" : "", query ? query : "");

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
        "ymusicd " YM_VERSION " — Яндекс Музыка: плеер и API для приложения на ПК\n"
        "  -l адрес     слушать (0.0.0.0)\n"
        "  -p порт      (6601)\n"
        "  -t файл      токен OAuth (/var/lib/ymusic/token)\n"
        "  -q файл      качество lossless|hq|nq (/var/lib/ymusic/quality)\n"
        "  -d устройство ALSA (default)\n"
        "  -c папка     служебные файлы: active — Яндекс Музыка выбрана плеером (/tmp/ymusic)\n"
        "  -a URL       адрес API (https://api.music.yandex.net)\n"
        "  -T raw|encraw транспорт (encraw)\n"
        "  -v           подробный журнал\n");
}

int main(int argc, char **argv)
{
    int o;
    while ((o = getopt(argc, argv, "l:p:t:q:d:c:a:T:vh")) != -1) {
        switch (o) {
        case 'l': g_listen = optarg; break;
        case 'p': g_port = atoi(optarg); break;
        case 't': g_token_f = optarg; break;
        case 'q': g_qual_f = optarg; break;
        case 'd': g_dev = optarg; break;
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
    share_init();
    snprintf(g_active_f, sizeof g_active_f, "%s/active", g_cover_d);
    cond_init(&g_pl_cv);
    cond_init(&g_ev_cv);

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
    pthread_t th;
    g_src_idle = src_idle;
    if (pthread_create(&th, &at, player_thread, NULL)) { logm(0, "плеер не запущен"); return 1; }
    if (pthread_create(&th, &at, wave_watch, NULL)) logm(0, "сторож волны не запущен");
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
