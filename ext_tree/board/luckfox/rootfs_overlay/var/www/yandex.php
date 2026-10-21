<?php
// yandex.php — Яндекс Музыка в веб-интерфейсе PureFox (перенесено из DigiFox, github.com/DrModd/DigiFox).
// Запросы к API идут через локальный демон ymusicd (127.0.0.1:6601): у PHP Фокса нет HTTPS.
// Играет MPD: в очередь ставятся ссылки http://127.0.0.1:6601/track/<id>.flac?a=<альбом>,
// ymusicd отдаёт по ним расшифрованный поток. «Моя волна» подкачивается демоном сама.
//   GET  ?api=status                    -> {"token":bool,"daemon":bool,"login":..,"plus":bool,"quality":..}
//   GET  ?api=home                      -> {"likes":n,"playlists":[..],"personal":[..],"stations":[..]}
//   GET  ?api=search&q=                 -> {"tracks":[..],"albums":[..],"artists":[..],"playlists":[..]}
//   GET  ?api=album&id= | artist&id= | playlist&uid=&kind= | likes
//   GET  ?api=now                       -> что играет MPD
//   POST op=token value | op=logout | op=quality q | op=play tracks start append | op=wave seed
//   POST op=ctl cmd=prev|toggle|next | op=like id a
require_once 'config.php';

const YM_D      = 'http://127.0.0.1:6601';
const YM_TOKEN  = '/var/lib/ymusic/token';      // /var/lib переживает онлайн-обновление
const YM_QUAL   = '/var/lib/ymusic/quality';
const YM_TMP    = '/tmp/ymusic';
const YM_OAUTH  = 'https://oauth.yandex.ru/authorize?response_type=token&client_id=23cabbbdc6cd418abb4b39c32c41195d';
const MPD_HOST  = '127.0.0.1';
const MPD_PORT  = 6600;
const MAX_QUEUE = 500;

function out($a) {
    header('Content-Type: application/json; charset=utf-8');
    header('Cache-Control: no-store');
    echo json_encode($a, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
    exit;
}
function fail($m) {
    out(['ok' => false, 'msg' => $m]);
}

// ---------------- ymusicd / API ----------------
function ym_raw($path, $post = null, $ctype = 'application/x-www-form-urlencoded', $timeout = 25) {
    $opt = ['http' => ['timeout' => $timeout, 'ignore_errors' => true, 'header' => "Connection: close\r\n"]];
    if ($post !== null) {
        $opt['http']['method'] = 'POST';
        $opt['http']['header'] .= "Content-Type: $ctype\r\n";
        $opt['http']['content'] = $post;
    }
    $r = @file_get_contents(YM_D . $path, false, stream_context_create($opt));
    return $r === false ? null : $r;
}
// ответ API: ['result'=>..] или ['error'=>..]; null — демон не отвечает
function api($path, $post = null, $json = false) {
    $body = $post;
    if (is_array($post)) $body = $json ? json_encode($post, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES) : http_build_query($post);
    $r = ym_raw('/api' . $path, $body, $json ? 'application/json' : 'application/x-www-form-urlencoded');
    if ($r === null) return null;
    $j = json_decode($r, true);
    return is_array($j) ? $j : ['error' => ['message' => 'bad json']];
}
function res($path, $post = null, $json = false) {
    $j = api($path, $post, $json);
    if ($j === null) fail('Служба ymusicd не отвечает');
    if (isset($j['error'])) {
        $e = $j['error'];
        $m = is_array($e) ? ($e['message'] ?? $e['name'] ?? 'ошибка') : (string)$e;
        if (is_array($e) && ($e['name'] ?? '') === 'session-expired') $m = 'Токен недействителен — войдите заново';
        fail('Яндекс: ' . $m);
    }
    return $j['result'] ?? null;
}
function has_token() { return trim((string)@file_get_contents(YM_TOKEN)) !== ''; }
function quality() {
    $q = trim((string)@file_get_contents(YM_QUAL));
    return in_array($q, ['lossless', 'hq', 'nq'], true) ? $q : 'lossless';
}
function uid() {
    $tok = trim((string)@file_get_contents(YM_TOKEN));
    if ($tok === '') fail('Нужно войти в Яндекс');
    $cache = YM_TMP . '/uid';
    $c = @json_decode((string)@file_get_contents($cache), true);
    if (is_array($c) && ($c['t'] ?? '') === md5($tok)) return $c['uid'];
    $r = res('/account/status');
    $uid = $r['account']['uid'] ?? null;
    if (!$uid) fail('Яндекс не вернул пользователя — войдите заново');
    @mkdir(YM_TMP, 0755, true);
    @file_put_contents($cache, json_encode(['t' => md5($tok), 'uid' => $uid, 'acc' => $r]));
    return $uid;
}

// ---------------- приведение объектов API ----------------
function img($uri, $size = '200x200') {
    if (!$uri) return '';
    $u = str_replace('%%', $size, $uri);
    return strpos($u, 'http') === 0 ? $u : 'https://' . $u;
}
function names($arr) {
    $n = [];
    foreach ((array)$arr as $a) if (!empty($a['name'])) $n[] = $a['name'];
    return implode(', ', $n);
}
function tr($t) {
    if (!is_array($t) || !isset($t['id'])) return null;
    $alb = $t['albums'][0] ?? [];
    $title = (string)($t['title'] ?? '');
    if (!empty($t['version'])) $title .= ' (' . $t['version'] . ')';
    return ['id' => (string)$t['id'], 'a' => isset($alb['id']) ? (string)$alb['id'] : '',
            'title' => $title, 'artist' => names($t['artists'] ?? []), 'album' => (string)($alb['title'] ?? ''),
            'cover' => $t['coverUri'] ?? ($alb['coverUri'] ?? ''), 'dur' => (int)($t['durationMs'] ?? 0),
            'ok' => ($t['available'] ?? true) !== false];
}
function tracks($list) {
    $r = [];
    foreach ((array)$list as $t) { $x = tr($t); if ($x) $r[] = $x; }
    return $r;
}
function alb($a) {
    return ['id' => (string)($a['id'] ?? ''), 'title' => (string)($a['title'] ?? '') . (!empty($a['version']) ? ' (' . $a['version'] . ')' : ''),
            'artist' => names($a['artists'] ?? []), 'year' => $a['year'] ?? '', 'cover' => img($a['coverUri'] ?? '', '100x100'),
            'n' => (int)($a['trackCount'] ?? 0)];
}
function art($a) {
    return ['id' => (string)($a['id'] ?? ''), 'name' => (string)($a['name'] ?? ''),
            'cover' => img($a['cover']['uri'] ?? ($a['ogImage'] ?? ''), '100x100')];
}
function pl($p) {
    $c = $p['cover'] ?? [];
    $u = $c['uri'] ?? ($c['itemsUri'][0] ?? ($p['ogImage'] ?? ''));
    return ['uid' => (string)($p['uid'] ?? ($p['owner']['uid'] ?? '')), 'kind' => (string)($p['kind'] ?? ''),
            'title' => (string)($p['title'] ?? ''), 'n' => (int)($p['trackCount'] ?? 0),
            'owner' => (string)($p['owner']['name'] ?? ($p['owner']['login'] ?? '')), 'cover' => img($u, '100x100')];
}
// треки по списку id ("123" или "123:456"), пачками
function tracks_by_ids($ids) {
    $out = [];
    foreach (array_chunk($ids, 200) as $chunk) {
        $r = res('/tracks', ['track-ids' => implode(',', $chunk), 'with-positions' => 'false']);
        foreach ((array)$r as $t) { $x = tr($t); if ($x) $out[] = $x; }
    }
    return $out;
}

// ---------------- MPD ----------------
class Mpd {
    private $s;
    function __construct() {
        $this->s = @fsockopen(MPD_HOST, MPD_PORT, $en, $es, 2.0);
        if (!$this->s) throw new Exception('MPD не отвечает');
        stream_set_timeout($this->s, 10);
        fgets($this->s);
    }
    // ответ: массив строк без OK; ошибка ACK — исключение
    function cmd($c) {
        fwrite($this->s, $c . "\n");
        $lines = [];
        while (($l = fgets($this->s)) !== false) {
            $l = rtrim($l, "\n");
            if ($l === 'OK') return $lines;
            if (strpos($l, 'ACK') === 0) throw new Exception('MPD: ' . $l);
            $lines[] = $l;
        }
        throw new Exception('MPD: нет ответа');
    }
    function kv($c) {
        $kv = [];
        foreach ($this->cmd($c) as $l) {
            $p = strpos($l, ': ');
            if ($p !== false && !isset($kv[substr($l, 0, $p)])) $kv[substr($l, 0, $p)] = substr($l, $p + 2);
        }
        return $kv;
    }
    function __destruct() { if ($this->s) { @fwrite($this->s, "close\n"); fclose($this->s); } }
}
function q($s) { return '"' . addcslashes(str_replace(["\r", "\n"], ' ', (string)$s), '"\\') . '"'; }

function active_service() {
    $j = @json_decode((string)@file_get_contents('/tmp/system_status.json'), true);
    $s = is_array($j) ? (string)($j['active_service'] ?? '') : '';
    if ($s === '') $s = trim((string)@file_get_contents('/tmp/active_service'));
    return $s;
}
function mpd_up() {
    $s = @fsockopen(MPD_HOST, MPD_PORT, $en, $es, 0.5);
    if ($s) { fclose($s); return true; }
    return false;
}
// MPD должен быть активным плеером (как кнопка MPD на главной)
function ensure_mpd() {
    if (active_service() === 'mpd' && mpd_up() && !file_exists('/etc/usb_to_i2s.state')) return;
    $ctx = stream_context_create(['http' => ['method' => 'POST', 'timeout' => 30, 'ignore_errors' => true,
        'header' => "Content-Type: application/x-www-form-urlencoded\r\nConnection: close\r\n",
        'content' => 'service=mpd']]);
    @file_get_contents('http://127.0.0.1/handle_service.php', false, $ctx);
    for ($i = 0; $i < 50 && !mpd_up(); $i++) usleep(100000);
    if (!mpd_up()) fail('Не удалось включить MPD');
    usleep(300000);
}

function track_url($t, $wave = '') {
    $ext = quality() === 'lossless' ? 'flac' : 'mp3';
    return 'http://127.0.0.1:6601/track/' . preg_replace('/\D/', '', $t['id']) . '.' . $ext .
           '?a=' . rawurlencode((string)($t['a'] ?? '')) . $wave;
}
function save_cover($t) {
    if (empty($t['cover'])) return;
    @mkdir(YM_TMP, 0755, true);
    @file_put_contents(YM_TMP . '/' . preg_replace('/\D/', '', $t['id']), img($t['cover'], '400x400'));
}
// очередь MPD: заменить (или дописать) и включить с позиции start
function queue($list, $start, $append, $wave = '') {
    $list = array_values(array_filter($list, function ($t) { return is_array($t) && !empty($t['id']) && ($t['ok'] ?? true); }));
    if (!$list) fail('Нет доступных треков');
    if (count($list) > MAX_QUEUE) {                    // окно вокруг выбранного трека
        $from = max(0, min($start, count($list) - MAX_QUEUE));
        $list = array_slice($list, $from, MAX_QUEUE);
        $start -= $from;
    }
    ensure_mpd();
    try {
        $m = new Mpd();
        if (!$append) $m->cmd('clear');
        $st = $m->kv('status');
        $base = (int)($st['playlistlength'] ?? 0);
        $c = "command_list_ok_begin\n";
        foreach ($list as $t) $c .= 'addid ' . q(track_url($t, $wave)) . "\n";
        $ids = [];
        foreach ($m->cmd($c . 'command_list_end') as $l)
            if (strpos($l, 'Id: ') === 0) $ids[] = (int)substr($l, 4);
        $c = "command_list_begin\n";
        foreach ($list as $i => $t) {
            if (!isset($ids[$i])) break;
            $id = $ids[$i];
            if ($t['title'] !== '')  $c .= "addtagid $id Title " . q($t['title']) . "\n";
            if ($t['artist'] !== '') $c .= "addtagid $id Artist " . q($t['artist']) . "\n";
            if ($t['album'] !== '')  $c .= "addtagid $id Album " . q($t['album']) . "\n";
            save_cover($t);
        }
        $m->cmd($c . 'command_list_end');
        if (!$append) $m->cmd('play ' . (int)max(0, min($start, count($ids) - 1)));
        elseif (($st['state'] ?? '') !== 'play') $m->cmd('play ' . $base);
        return count($ids);
    } catch (Exception $e) {
        fail($e->getMessage());
    }
}

// треки плейлиста: [ответ API, треки]
function playlist_tracks($uid, $kind) {
    $r = res("/users/$uid/playlists/$kind");
    $t = []; $miss = [];
    foreach ((array)($r['tracks'] ?? []) as $it) {
        if (isset($it['track'])) { $x = tr($it['track']); if ($x) $t[] = $x; }
        elseif (isset($it['id'])) $miss[] = $it['id'] . (!empty($it['albumId']) ? ':' . $it['albumId'] : '');
    }
    if ($miss) $t = array_merge($t, tracks_by_ids(array_slice($miss, 0, 1000)));
    return [$r, $t];
}

// «Моя волна» или станция: новая сессия rotor и первая партия треков в MPD
function ym_wave($seed) {
    if (!preg_match('/^[\w.:\-]{3,120}$/u', $seed)) fail('bad seed');
    $r = res('/rotor/session/new', ['seeds' => [$seed], 'queue' => [], 'includeTracksInResponse' => true], true);
    $sid = (string)($r['radioSessionId'] ?? '');
    $batch = (string)($r['batchId'] ?? '');
    $list = [];
    foreach ((array)($r['sequence'] ?? []) as $it) { $x = tr($it['track'] ?? null); if ($x) $list[] = $x; }
    if ($sid === '' || !$list) fail('Волна не запустилась (нужна подписка Плюс?)');
    api("/rotor/session/" . rawurlencode($sid) . "/feedback",
        ['event' => ['type' => 'radioStarted', 'timestamp' => gmdate('Y-m-d\TH:i:s.000\Z')], 'batchId' => $batch], true);
    $wave = '&w=' . rawurlencode($sid) . '&b=' . rawurlencode($batch) . '&s=' . rawurlencode($seed);
    return queue($list, 0, false, $wave);
}

// ---------------- API ----------------
if (isset($_GET['api'])) {
    @session_write_close();
    $a = $_GET['api'];

    if ($a === 'status') {
        $d = @json_decode((string)ym_raw('/status', null, '', 3), true);
        $r = ['daemon' => is_array($d), 'token' => has_token(), 'quality' => quality(), 'oauth' => YM_OAUTH];
        if ($r['daemon'] && $r['token']) {
            $j = api('/account/status');
            $acc = $j['result']['account'] ?? null;
            if ($acc) {
                $r['login'] = $acc['displayName'] ?? ($acc['login'] ?? '');
                $r['plus'] = !empty($j['result']['plus']['hasPlus']);
                @mkdir(YM_TMP, 0755, true);
                @file_put_contents(YM_TMP . '/uid', json_encode(['t' => md5(trim((string)@file_get_contents(YM_TOKEN))), 'uid' => $acc['uid'] ?? '']));
            } else {
                $r['bad_token'] = true;
                $r['msg'] = $j === null ? 'нет связи с Яндексом' : ($j['error']['message'] ?? ($j['error']['name'] ?? 'токен не принят'));
            }
        }
        out($r);
    }

    if ($a === 'home') {
        $uid = uid();
        $r = ['playlists' => [], 'personal' => [], 'stations' => []];
        foreach ((array)res("/users/$uid/playlists/list") as $p) $r['playlists'][] = pl($p);
        $l = api("/landing3?blocks=personalplaylists");
        foreach ((array)($l['result']['blocks'][0]['entities'] ?? []) as $e) {
            $p = $e['data']['data'] ?? null;
            if (is_array($p)) $r['personal'][] = pl($p);
        }
        $d = api('/rotor/stations/dashboard');
        foreach ((array)($d['result']['stations'] ?? []) as $s) {
            $st = $s['station'] ?? [];
            if (!isset($st['id']['type'], $st['id']['tag'])) continue;
            $r['stations'][] = ['seed' => $st['id']['type'] . ':' . $st['id']['tag'], 'name' => (string)($st['name'] ?? ''),
                                'cover' => img($st['icon']['imageUrl'] ?? '', '100x100'), 'color' => $st['icon']['backgroundColor'] ?? ''];
        }
        out($r);
    }

    if ($a === 'likes') {
        $uid = uid();
        $lib = res("/users/$uid/likes/tracks");
        $ids = [];
        foreach ((array)($lib['library']['tracks'] ?? []) as $t)
            $ids[] = $t['id'] . (!empty($t['albumId']) ? ':' . $t['albumId'] : '');
        $ids = array_slice($ids, 0, 1000);
        out(['title' => 'Мне нравится', 'tracks' => tracks_by_ids($ids)]);
    }

    if ($a === 'search') {
        $qq = trim((string)($_GET['q'] ?? ''));
        if ($qq === '') out(['tracks' => [], 'albums' => [], 'artists' => [], 'playlists' => []]);
        $r = res('/search?' . http_build_query(['text' => $qq, 'type' => 'all', 'page' => 0, 'nocorrect' => 'false']));
        out(['tracks'    => tracks($r['tracks']['results'] ?? []),
             'albums'    => array_map('alb', array_slice((array)($r['albums']['results'] ?? []), 0, 12)),
             'artists'   => array_map('art', array_slice((array)($r['artists']['results'] ?? []), 0, 12)),
             'playlists' => array_map('pl',  array_slice((array)($r['playlists']['results'] ?? []), 0, 12)),
             'corrected' => $r['misspellCorrected'] ?? false, 'text' => $r['text'] ?? $qq]);
    }

    if ($a === 'album') {
        $id = preg_replace('/\D/', '', (string)($_GET['id'] ?? ''));
        $r = res("/albums/$id/with-tracks");
        $t = [];
        foreach ((array)($r['volumes'] ?? []) as $v) $t = array_merge($t, tracks($v));
        $info = alb($r);
        out(['title' => $info['title'], 'sub' => trim($info['artist'] . ($info['year'] ? ' · ' . $info['year'] : '')),
             'cover' => img($r['coverUri'] ?? '', '200x200'), 'tracks' => $t, 'seed' => "album:$id"]);
    }

    if ($a === 'artist') {
        $id = preg_replace('/\D/', '', (string)($_GET['id'] ?? ''));
        $r = res("/artists/$id/brief-info");
        out(['title' => (string)($r['artist']['name'] ?? ''), 'cover' => img($r['artist']['cover']['uri'] ?? '', '200x200'),
             'tracks' => tracks($r['popularTracks'] ?? []), 'albums' => array_map('alb', (array)($r['albums'] ?? [])),
             'seed' => "artist:$id"]);
    }

    if ($a === 'playlist') {
        $uid = preg_replace('/[^\w.-]/', '', (string)($_GET['uid'] ?? ''));
        $kind = preg_replace('/\D/', '', (string)($_GET['kind'] ?? ''));
        list($r, $t) = playlist_tracks($uid, $kind);
        $p = pl($r);
        out(['title' => $p['title'], 'sub' => $p['owner'], 'cover' => img(($r['cover']['uri'] ?? ($r['cover']['itemsUri'][0] ?? '')), '200x200'),
             'tracks' => $t]);
    }

    if ($a === 'now') {
        if (!mpd_up()) out(['state' => 'off']);
        try {
            $m = new Mpd();
            $st = $m->kv('status');
            $cs = $m->kv('currentsong');
        } catch (Exception $e) { out(['state' => 'off']); }
        $file = $cs['file'] ?? '';
        $tid = preg_match('#127\.0\.0\.1:\d+/track/(\d+)#', $file, $mm) ? $mm[1] : '';
        $aid = preg_match('#[?&]a=(\d+)#', $file, $ma) ? $ma[1] : '';
        out(['state' => $st['state'] ?? 'stop', 'svc' => active_service(), 'ym' => $tid !== '',
             'id' => $tid, 'a' => $aid, 'wave' => strpos($file, '&w=') !== false,
             'title' => $cs['Title'] ?? ($file !== '' ? basename($file) : ''), 'artist' => $cs['Artist'] ?? '',
             'cover' => $tid !== '' ? str_replace('400x400', '200x200', (string)@file_get_contents(YM_TMP . "/$tid")) : '',
             'pos' => (float)($st['elapsed'] ?? 0), 'dur' => (float)($st['duration'] ?? 0),
             'n' => (int)($st['playlistlength'] ?? 0), 'song' => (int)($st['song'] ?? 0),
             'audio' => $st['audio'] ?? '']);
    }
    fail('bad api');
}

if ($_SERVER['REQUEST_METHOD'] === 'POST' && isset($_POST['op'])) {
    @session_write_close();
    $op = $_POST['op'];
    $P = function ($k) { return trim((string)($_POST[$k] ?? '')); };

    if ($op === 'token') {
        $v = $P('value');
        if (preg_match('/access_token=([^&#\s]+)/', $v, $m)) $v = urldecode($m[1]);
        if (!preg_match('/^[A-Za-z0-9_.\-]{20,400}$/', $v)) fail('Это не похоже на токен: вставьте адрес страницы целиком или только access_token');
        @mkdir(dirname(YM_TOKEN), 0755, true);
        if (@file_put_contents(YM_TOKEN, $v) === false) fail('Не удалось сохранить токен');
        @chmod(YM_TOKEN, 0600);
        @unlink(YM_TMP . '/uid');
        $j = api('/account/status');
        if ($j === null) out(['ok' => true, 'msg' => 'Токен сохранён, но служба ymusicd не отвечает']);
        $acc = $j['result']['account'] ?? null;
        if (!$acc || empty($acc['uid'])) out(['ok' => false, 'msg' => 'Токен сохранён, но Яндекс его не принял: ' .
                                                    ($j['error']['message'] ?? ($j['error']['name'] ?? 'нет пользователя'))]);
        out(['ok' => true, 'msg' => 'Вход выполнен: ' . ($acc['displayName'] ?? ($acc['login'] ?? ''))]);
    }

    if ($op === 'logout') {
        @unlink(YM_TOKEN);
        @unlink(YM_TMP . '/uid');
        out(['ok' => true, 'msg' => 'Токен удалён']);
    }

    if ($op === 'quality') {
        $v = $P('q');
        if (!in_array($v, ['lossless', 'hq', 'nq'], true)) fail('bad quality');
        @mkdir(dirname(YM_QUAL), 0755, true);
        @file_put_contents(YM_QUAL, $v . "\n");
        out(['ok' => true, 'msg' => 'Качество сохранено — для следующих треков']);
    }

    if ($op === 'play') {
        $list = json_decode((string)($_POST['tracks'] ?? ''), true);
        if (!is_array($list)) fail('Нет треков');
        $n = queue($list, (int)$P('start'), $P('append') === '1');
        out(['ok' => true, 'msg' => $P('append') === '1' ? "Добавлено в очередь: $n" : "В очереди: $n"]);
    }

    if ($op === 'wave') {
        ym_wave($P('seed') !== '' ? $P('seed') : 'user:onyourwave');
        out(['ok' => true, 'msg' => 'Волна запущена']);
    }

    if ($op === 'ctl') {
        $map = ['prev' => 'previous', 'next' => 'next', 'stop' => 'stop'];
        $c = $P('cmd');
        try {
            $m = new Mpd();
            if ($c === 'toggle') {
                $st = $m->kv('status');
                $m->cmd(($st['state'] ?? '') === 'play' ? 'pause 1' : 'play');
            } elseif (isset($map[$c])) {
                $m->cmd($map[$c]);
            } else fail('bad cmd');
        } catch (Exception $e) { fail($e->getMessage()); }
        out(['ok' => true]);
    }

    if ($op === 'like' || $op === 'dislike') {
        $id = preg_replace('/\D/', '', $P('id'));
        $al = preg_replace('/\D/', '', $P('a'));
        if ($id === '') fail('Нет трека');
        $uid = uid();
        $kind = $op === 'like' ? 'likes' : 'dislikes';
        res("/users/$uid/$kind/tracks/add-multiple", ['track-ids' => $id . ($al !== '' ? ":$al" : '')]);
        if ($op === 'dislike') { try { (new Mpd())->cmd('next'); } catch (Exception $e) {} }
        out(['ok' => true, 'msg' => $op === 'like' ? 'Добавлено в «Мне нравится»' : 'Больше не будет звучать']);
    }
    fail('bad op');
}
?><!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
    <meta name="theme-color" content="#2d2d2d">
    <title>Яндекс Музыка</title>
    <link rel="icon" href="favicon.ico" sizes="any">
    <style>
        :root { --bg: #2d2d2d; --raise: #3d3d3d; --hover: #4d4d4d; --fg: #e0e0e0; --dim: #aaa; --faint: #666;
                --line: #444; --live: #90ee90; --mono: ui-monospace, Menlo, Consolas, monospace; }
        *, *::before, *::after { box-sizing: border-box; }
        html, body { margin: 0; min-height: 100%; }
        body { font-family: Arial, sans-serif; color: var(--fg);
               background: radial-gradient(circle at center, #383838 0%, #1a1a1a 50%, #0a0a0a 100%) fixed; }
        .wrap { max-width: 600px; margin: 20px auto; padding: 20px; background: var(--bg); border-radius: 10px;
                box-shadow: 0 16px 64px rgba(0, 0, 0, .8), 0 8px 32px rgba(0, 0, 0, .6); }
        @media (max-width: 480px) { .wrap { margin: 10px; padding: 14px; } }
        .head { display: flex; align-items: center; gap: 12px; margin-bottom: 16px; }
        .head .back { display: flex; text-decoration: none; }
        .head .back img { width: 28px; height: 28px; }
        .brand { flex: 1; text-align: center; font-size: 22px; font-weight: bold; letter-spacing: 1px; }
        .dot { width: 10px; height: 10px; border-radius: 50%; background: #8a4a4a; flex: none; }
        .dot.on { background: var(--live); }
        .panel { background: #333; border-radius: 8px; padding: 14px; margin-top: 12px; }
        .panel:first-of-type { margin-top: 0; }
        .cap { color: var(--dim); font-size: 11px; letter-spacing: 1.5px; margin-bottom: 4px; }
        .grow { flex: 1; min-width: 0; }
        .hint { color: var(--dim); font-size: 12px; line-height: 1.4; margin-top: 10px; }
        .hifi { height: 44px; padding: 0 14px; border: none; border-radius: 8px; background: var(--raise); color: var(--fg);
                font: inherit; font-size: 13px; letter-spacing: 1px; cursor: pointer; transition: background .2s; }
        .hifi:hover:not(:disabled) { background: var(--hover); }
        .hifi:disabled { opacity: .4; cursor: default; }
        .hifi.on { background: #5d5d5d; }
        .hifi.wide { width: 100%; margin-top: 10px; }
        .row3 { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; margin-top: 10px; }
        .row3 .hifi { padding: 0 4px; }
        select { width: 100%; height: 40px; padding: 0 8px; margin-top: 4px; background: #2a2a2a; color: var(--fg);
                 border: 1px solid #555; border-radius: 4px; font: inherit; }
        .cover { border-radius: 4px; object-fit: cover; background: var(--line); flex: none; }
        .status { display: flex; align-items: center; justify-content: center; gap: 8px; min-height: 20px; margin-top: 12px;
                  color: var(--dim); font-size: 13px; }
        .spin { width: 14px; height: 14px; border: 2px solid var(--faint); border-top-color: var(--fg); border-radius: 50%;
                animation: spin 1s linear infinite; }
        @keyframes spin { to { transform: rotate(360deg); } }
        .foot { text-align: center; color: var(--faint); font-size: 12px; margin-top: 14px; }
        input.txt {
            width: 100%; min-width: 0; height: 46px; padding: 0 10px; margin-top: 6px;
            background: var(--raise); color: var(--fg); border: 1px solid var(--line); border-radius: 4px;
            font: inherit; font-size: 16px; box-sizing: border-box;
        }
        input.txt:focus-visible { outline: 2px solid var(--dim); outline-offset: 2px; }
        .msg { min-height: 18px; margin-top: 8px; font-size: 13px; color: var(--dim); }
        .msg.ok { color: var(--live); }
        .msg.err { color: #e5675e; }
        .msg:empty { display: none; }
        .steps { margin: 8px 0 0; padding-left: 20px; color: var(--dim); font-size: 13px; line-height: 1.5; }
        .steps b { color: var(--fg); font-weight: 500; }
        a.hifi { display: flex; align-items: center; justify-content: center; text-decoration: none; }
        .acc { display: flex; align-items: center; gap: 10px; }
        .acc .who { flex: 1; min-width: 0; font-size: 15px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
        .acc .tag { color: var(--dim); font-size: 11px; letter-spacing: 1px; }
        .acc .hifi { flex: none; width: 96px; }
        .np { display: flex; align-items: center; gap: 12px; }
        .np .cover { width: 56px; height: 56px; }
        .np .t { font-size: 15px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
        .np .s { color: var(--dim); font-size: 13px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
        .np .meta { color: var(--dim); font-size: 11px; font-family: var(--mono); margin-top: 2px; }
        .ctl { display: grid; grid-template-columns: repeat(5, 1fr); gap: 8px; margin-top: 10px; }
        .ctl .hifi { padding: 0; font-size: 16px; letter-spacing: 0; }
        .search { display: flex; gap: 8px; }
        .search input { margin-top: 0; }
        .search .hifi { flex: none; width: 96px; }
        .crumb { display: flex; align-items: center; gap: 10px; }
        .crumb .hifi { flex: none; width: 46px; padding: 0; font-size: 18px; letter-spacing: 0; }
        .crumb .grow .t { font-size: 17px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
        .crumb .grow .s { color: var(--dim); font-size: 13px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
        .crumb img { width: 56px; height: 56px; border-radius: 4px; object-fit: cover; background: var(--line); flex: none; }
        .sec { margin-top: 14px; }
        .sec:first-child { margin-top: 0; }
        .row { display: flex; align-items: center; gap: 10px; padding: 8px 0; border-top: 1px solid var(--line);
               cursor: pointer; background: none; border-left: 0; border-right: 0; border-bottom: 0; width: 100%;
               color: inherit; font: inherit; text-align: left; }
        .list .row:first-child { border-top: 0; }
        .row:hover .nm b { text-decoration: underline; text-decoration-color: var(--faint); }
        .row img, .row .ph { width: 42px; height: 42px; border-radius: 3px; object-fit: cover; background: var(--line); flex: none; }
        .row.round img, .row.round .ph { border-radius: 50%; }
        .row .nm { flex: 1; min-width: 0; }
        .row .nm b { display: block; font-weight: 500; font-size: 15px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
        .row .nm span { display: block; color: var(--dim); font-size: 12px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
        .row .d { color: var(--dim); font-size: 12px; font-family: var(--mono); flex: none; }
        .row.na { opacity: .45; cursor: default; }
        .row.cur .nm b { color: var(--live); }
        .wave-btn { width: 100%; height: 64px; font-size: 14px; letter-spacing: 3px; margin-top: 10px; }
        .chips { display: flex; flex-wrap: wrap; gap: 6px; margin-top: 10px; }
        .chip { height: 34px; padding: 0 12px; border: 1px solid var(--line); border-radius: 17px; background: var(--raise);
                color: var(--fg); font: inherit; font-size: 13px; cursor: pointer; }
        .chip:hover { border-color: var(--dim); }
        .more { width: 100%; margin-top: 8px; }
        .empty { color: var(--dim); font-size: 14px; padding: 10px 0; }
        [hidden] { display: none !important; }
    </style>
</head>
<body>
<main class="wrap">
    <header class="head">
        <a class="back" href="index.php" aria-label="Назад">
            <img src="assets/img/home.svg" alt="">
        </a>
        <div class="brand">ЯНДЕКС МУЗЫКА</div>
        <span class="dot" id="dot"></span>
    </header>

    <!-- вход -->
    <section class="panel" id="login" hidden>
        <div class="cap">ВХОД В ЯНДЕКС</div>
        <ol class="steps">
            <li>Нажмите <b>«Открыть вход Яндекса»</b> и войдите в свой аккаунт (с подпиской Плюс).</li>
            <li>Яндекс перебросит на страницу music.yandex.ru — в её адресе есть <b>access_token=…</b>.
                Скопируйте адрес страницы целиком.</li>
            <li>Вставьте его сюда и нажмите <b>«Сохранить»</b>.</li>
        </ol>
        <a class="hifi wide" id="oauth" href="#" target="_blank" rel="noopener">ОТКРЫТЬ ВХОД ЯНДЕКСА</a>
        <input class="txt" id="tok" type="text" placeholder="https://music.yandex.ru/#access_token=…" autocomplete="off" spellcheck="false">
        <button class="hifi wide" id="tok-save">СОХРАНИТЬ</button>
        <div class="msg" id="login-msg"></div>
        <div class="hint">Пароль на Фокс не передаётся: хранится только токен (в /var/lib/ymusic/token). Он действует около года.
            Удобнее делать это с телефона или компьютера, где вы уже вошли в Яндекс.</div>
    </section>

    <!-- аккаунт -->
    <section class="panel" id="account" hidden>
        <div class="acc">
            <div class="grow">
                <div class="cap">АККАУНТ</div>
                <div class="who" id="who">—</div>
            </div>
            <button class="hifi" id="logout">ВЫЙТИ</button>
        </div>
        <div class="cap" style="margin-top:12px">КАЧЕСТВО</div>
        <select id="quality">
            <option value="lossless">Lossless FLAC (если есть, иначе MP3 320)</option>
            <option value="hq">MP3 320 кбит/с</option>
            <option value="nq">MP3 обычное</option>
        </select>
        <div class="msg" id="acc-msg"></div>
    </section>

    <!-- сейчас играет -->
    <section class="panel" id="now" hidden>
        <div class="np">
            <img class="cover" id="np-cover" alt="" hidden onerror="this.hidden=true;this.dataset.bad=this.getAttribute('src')">
            <div class="grow">
                <div class="cap" id="np-cap">MPD</div>
                <div class="t" id="np-title"></div>
                <div class="s" id="np-artist"></div>
                <div class="meta" id="np-meta"></div>
            </div>
        </div>
        <div class="ctl">
            <button class="hifi" data-c="prev" title="Предыдущий">⏮</button>
            <button class="hifi" data-c="toggle" id="np-play" title="Пауза / играть">⏯</button>
            <button class="hifi" data-c="next" title="Следующий">⏭</button>
            <button class="hifi" id="np-like" title="Мне нравится">♥</button>
            <button class="hifi" id="np-dislike" title="Не нравится (пропустить)">✕</button>
        </div>
        <div class="msg" id="np-msg"></div>
    </section>

    <!-- обзор -->
    <section class="panel" id="browse" hidden>
        <div class="search">
            <input class="txt" id="q" type="search" placeholder="Трек, альбом, исполнитель" autocomplete="off" enterkeyhint="search">
            <button class="hifi" id="q-go">НАЙТИ</button>
        </div>
        <div id="view" style="margin-top:12px"></div>
        <div class="msg" id="view-msg"></div>
    </section>

    <div class="status" id="status"></div>
    <footer class="foot">PureFox v<?php echo htmlspecialchars(VERSION); ?>f</footer>
</main>

<script>
(function () {
    var $ = function (id) { return document.getElementById(id); };
    var stack = [], cur = null, now = null, busy = false;

    function msg(id, text, cls) { var e = $(id); e.textContent = text || ''; e.className = 'msg' + (cls ? ' ' + cls : ''); }
    function getj(q) {
        return fetch('yandex.php?' + q, { cache: 'no-store' }).then(function (r) { return r.json(); })
            .catch(function () { return { ok: false, msg: 'Нет связи с устройством' }; });
    }
    function post(data) {
        var fd = new FormData();
        for (var k in data) fd.append(k, data[k]);
        return fetch('yandex.php', { method: 'POST', body: fd, cache: 'no-store' }).then(function (r) { return r.json(); })
            .catch(function () { return { ok: false, msg: 'Нет связи с устройством' }; });
    }
    function el(tag, cls, text) { var e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; }
    function mmss(ms) { var s = Math.round(ms / 1000); return Math.floor(s / 60) + ':' + ('0' + s % 60).slice(-2); }
    function setStatus(t) { var s = $('status'); s.innerHTML = ''; if (t) { s.appendChild(el('span', 'spin')); s.appendChild(el('span', '', t)); } }

    // ---------- вход / аккаунт ----------
    function loadStatus() {
        return getj('api=status').then(function (r) {
            $('dot').className = 'dot' + (r.daemon ? ' on' : '');
            $('oauth').href = r.oauth || '#';
            if (!r.daemon) { $('login').hidden = true; $('account').hidden = true; $('browse').hidden = true;
                msg('view-msg', ''); setStatus(''); $('status').textContent = 'Служба ymusicd не запущена'; return; }
            var logged = r.token && !r.bad_token;
            $('login').hidden = logged;
            $('account').hidden = !r.token;
            $('browse').hidden = !logged;
            $('quality').value = r.quality || 'lossless';
            $('who').textContent = r.token ? (r.bad_token ? 'Токен не принят: ' + (r.msg || '') : (r.login || '—') + (r.plus ? '' : ' · без Плюса')) : '—';
            if (logged && !cur) home();
        });
    }
    $('tok-save').addEventListener('click', function () {
        var v = $('tok').value.trim();
        if (!v) return;
        msg('login-msg', 'Проверяю…');
        post({ op: 'token', value: v }).then(function (r) {
            msg('login-msg', r.msg, r.ok ? 'ok' : 'err');
            if (r.ok) { $('tok').value = ''; loadStatus(); }
        });
    });
    $('logout').addEventListener('click', function () {
        if (!confirm('Удалить токен Яндекса с устройства?')) return;
        post({ op: 'logout' }).then(function () { cur = null; stack = []; $('view').innerHTML = ''; loadStatus(); });
    });
    $('quality').addEventListener('change', function () {
        post({ op: 'quality', q: $('quality').value }).then(function (r) { msg('acc-msg', r.msg, r.ok ? 'ok' : 'err'); });
    });

    // ---------- сейчас играет ----------
    function pollNow() {
        getj('api=now').then(function (r) {
            now = r;
            var on = r && r.state && r.state !== 'off' && r.n > 0;
            $('now').hidden = !on;
            if (!on) return;
            $('np-cap').textContent = (r.svc && r.svc !== 'mpd' ? 'MPD (активен другой плеер)' : r.wave ? 'МОЯ ВОЛНА' : 'ОЧЕРЕДЬ ' + (r.song + 1) + ' / ' + r.n);
            $('np-title').textContent = r.title || '—';
            $('np-artist').textContent = r.artist || '';
            var a = (r.audio || '').split(':'), fmt = '';
            if (a.length === 3) fmt = (a[1] === 'f' ? '32f' : a[1]) + ' бит / ' + (a[0] / 1000) + ' кГц · ';
            $('np-meta').textContent = (r.state === 'play' ? '▶ ' : r.state === 'pause' ? '❚❚ ' : '■ ') + fmt +
                mmss(r.pos * 1000) + (r.dur ? ' / ' + mmss(r.dur * 1000) : '');
            var c = $('np-cover');
            if (r.cover && c.dataset.bad !== r.cover) { if (c.getAttribute('src') !== r.cover) c.src = r.cover; c.hidden = false; } else c.hidden = true;
            $('np-like').disabled = $('np-dislike').disabled = !r.ym;
            markCurrent();
        });
    }
    Array.prototype.forEach.call(document.querySelectorAll('.ctl [data-c]'), function (b) {
        b.addEventListener('click', function () {
            post({ op: 'ctl', cmd: b.getAttribute('data-c') }).then(function (r) { if (!r.ok) msg('np-msg', r.msg, 'err'); setTimeout(pollNow, 300); });
        });
    });
    function likeOp(op) {
        if (!now || !now.ym) return;
        post({ op: op, id: now.id, a: now.a }).then(function (r) { msg('np-msg', r.msg, r.ok ? 'ok' : 'err'); setTimeout(pollNow, 500); });
    }
    $('np-like').addEventListener('click', function () { likeOp('like'); });
    $('np-dislike').addEventListener('click', function () { likeOp('dislike'); });

    // ---------- воспроизведение ----------
    function play(tracks, start, append) {
        if (busy) return;
        busy = true;
        setStatus(append ? 'Добавляю в очередь…' : 'Запускаю…');
        post({ op: 'play', tracks: JSON.stringify(tracks), start: start || 0, append: append ? 1 : 0 }).then(function (r) {
            busy = false; setStatus('');
            msg('view-msg', r.msg, r.ok ? 'ok' : 'err');
            setTimeout(pollNow, 600);
        });
    }
    function wave(seed, name) {
        if (busy) return;
        busy = true;
        setStatus('Запускаю ' + (name || 'волну') + '…');
        post({ op: 'wave', seed: seed || '' }).then(function (r) {
            busy = false; setStatus('');
            msg('view-msg', r.ok ? (name || 'Моя волна') + ': играет' : r.msg, r.ok ? 'ok' : 'err');
            setTimeout(pollNow, 800);
        });
    }

    // ---------- навигация ----------
    function go(q, title) {
        if (cur) stack.push(cur);
        open(q, title);
    }
    function back() { if (stack.length) open(stack.pop().q); }
    function open(q, title) {
        cur = { q: q };
        msg('view-msg', '');
        var v = $('view');
        v.innerHTML = '';
        v.appendChild(el('div', 'empty', 'Загрузка…'));
        if (q === 'home') return getj('api=home').then(renderHome);
        getj(q).then(function (r) {
            if (cur.q !== q) return;
            if (r.ok === false) { v.innerHTML = ''; if (stack.length) v.appendChild(crumb(title || '', '')); msg('view-msg', r.msg, 'err'); return; }
            if (q.indexOf('api=search') === 0) renderSearch(r); else renderList(r);
        });
    }
    function home() { stack = []; open('home'); }

    function crumb(title, sub, cover) {
        var c = el('div', 'crumb');
        var b = el('button', 'hifi', '‹'); b.title = 'Назад'; b.addEventListener('click', back);
        c.appendChild(b);
        if (cover) { var i = el('img'); i.alt = ''; i.onerror = function () { i.remove(); }; i.src = cover; c.appendChild(i); }
        var g = el('div', 'grow'); g.appendChild(el('div', 't', title)); if (sub) g.appendChild(el('div', 's', sub));
        c.appendChild(g);
        return c;
    }
    function row(img, title, sub, right, onClick, round) {
        var r = el('button', 'row' + (round ? ' round' : ''));
        r.type = 'button';
        if (img) {
            var i = el('img'); i.loading = 'lazy'; i.alt = '';
            i.onerror = function () { if (i.parentNode) i.parentNode.replaceChild(el('span', 'ph'), i); };
            i.src = img; r.appendChild(i);
        } else r.appendChild(el('span', 'ph'));
        var n = el('span', 'nm'); n.appendChild(el('b', '', title)); if (sub) n.appendChild(el('span', '', sub));
        r.appendChild(n);
        if (right) r.appendChild(el('span', 'd', right));
        if (onClick) r.addEventListener('click', onClick); else r.disabled = true;
        return r;
    }
    function section(cap, nodes, more) {
        var s = el('div', 'sec');
        s.appendChild(el('div', 'cap', cap));
        var l = el('div', 'list');
        nodes.forEach(function (n) { l.appendChild(n); });
        s.appendChild(l);
        if (more) s.appendChild(more);
        return s;
    }
    function trackRows(tracks, limit) {
        var nodes = [];
        tracks.forEach(function (t, i) {
            if (limit && i >= limit) return;
            var r = row(t.cover ? 'https://' + t.cover.replace('%%', '100x100').replace(/^https?:\/\//, '') : '',
                        t.title, t.artist + (t.album ? ' · ' + t.album : ''), t.dur ? mmss(t.dur) : '',
                        t.ok ? function () { play(tracks, i, false); } : null);
            r.setAttribute('data-tid', t.id);
            if (!t.ok) r.classList.add('na');
            nodes.push(r);
        });
        return nodes;
    }
    function markCurrent() {
        var rows = document.querySelectorAll('.row[data-tid]');
        for (var i = 0; i < rows.length; i++) rows[i].classList.toggle('cur', !!(now && now.ym && rows[i].getAttribute('data-tid') === now.id));
    }
    function albumRows(a) {
        return a.map(function (x) { return row(x.cover, x.title, [x.artist, x.year].filter(Boolean).join(' · '), '', function () { go('api=album&id=' + x.id, x.title); }); });
    }
    function artistRows(a) {
        return a.map(function (x) { return row(x.cover, x.name, '', '', function () { go('api=artist&id=' + x.id, x.name); }, true); });
    }
    function plRows(a) {
        return a.map(function (x) {
            return row(x.cover, x.title, (x.owner ? x.owner + ' · ' : '') + (x.n ? x.n + ' тр.' : ''), '',
                       function () { go('api=playlist&uid=' + encodeURIComponent(x.uid) + '&kind=' + encodeURIComponent(x.kind), x.title); });
        });
    }

    function renderHome(r) {
        if (cur.q !== 'home') return;
        var v = $('view');
        v.innerHTML = '';
        if (r.ok === false) { msg('view-msg', r.msg, 'err'); return; }
        var top = el('div', 'sec');
        var wb = el('button', 'hifi on wave-btn', '▶  МОЯ ВОЛНА');
        wb.addEventListener('click', function () { wave('user:onyourwave', 'Моя волна'); });
        top.appendChild(wb);
        var lk = el('button', 'hifi wide', '♥  МНЕ НРАВИТСЯ');
        lk.addEventListener('click', function () { go('api=likes', 'Мне нравится'); });
        top.appendChild(lk);
        if (r.stations && r.stations.length) {
            var chips = el('div', 'chips');
            r.stations.slice(0, 12).forEach(function (s) {
                var c = el('button', 'chip', s.name);
                c.title = 'Волна: ' + s.name;
                c.addEventListener('click', function () { wave(s.seed, s.name); });
                chips.appendChild(c);
            });
            top.appendChild(chips);
        }
        v.appendChild(top);
        if (r.personal && r.personal.length) v.appendChild(section('ДЛЯ ВАС', plRows(r.personal)));
        if (r.playlists && r.playlists.length) v.appendChild(section('МОИ ПЛЕЙЛИСТЫ', plRows(r.playlists)));
    }

    function renderList(r) {
        var v = $('view');
        v.innerHTML = '';
        v.appendChild(crumb(r.title || '', r.sub || (r.tracks ? r.tracks.length + ' треков' : ''), r.cover));
        var ok = (r.tracks || []).filter(function (t) { return t.ok; });
        var btns = el('div', 'row3');
        var b1 = el('button', 'hifi', 'ИГРАТЬ'); b1.disabled = !ok.length;
        b1.addEventListener('click', function () { play(r.tracks, r.tracks.indexOf(ok[0]), false); });
        var b2 = el('button', 'hifi', 'ВПЕРЕМЕШКУ'); b2.disabled = ok.length < 2;
        b2.addEventListener('click', function () {
            var s = ok.slice();
            for (var i = s.length - 1; i > 0; i--) { var j = Math.floor(Math.random() * (i + 1)), t = s[i]; s[i] = s[j]; s[j] = t; }
            play(s, 0, false);
        });
        var b3 = el('button', 'hifi', r.seed ? 'ВОЛНА' : 'В ОЧЕРЕДЬ');
        b3.title = r.seed ? 'Волна по этому' : 'Добавить в конец очереди';
        b3.addEventListener('click', function () { if (r.seed) wave(r.seed, 'Волна «' + (r.title || '') + '»'); else play(ok, 0, true); });
        btns.appendChild(b1); btns.appendChild(b2); btns.appendChild(b3);
        v.appendChild(btns);
        if (r.tracks) {
            var LIM = 150, rows = trackRows(r.tracks, LIM), more = null;
            if (r.tracks.length > LIM) {
                more = el('button', 'hifi more', 'ПОКАЗАТЬ ВСЕ (' + r.tracks.length + ')');
                more.addEventListener('click', function () {
                    var l = more.previousSibling; l.innerHTML = '';
                    trackRows(r.tracks).forEach(function (n) { l.appendChild(n); });
                    more.remove(); markCurrent();
                });
            }
            v.appendChild(section(r.albums ? 'ПОПУЛЯРНЫЕ ТРЕКИ' : 'ТРЕКИ', rows.length ? rows : [el('div', 'empty', 'Пусто')], more));
        }
        if (r.albums && r.albums.length) v.appendChild(section('АЛЬБОМЫ', albumRows(r.albums)));
        markCurrent();
    }

    function renderSearch(r) {
        var v = $('view');
        v.innerHTML = '';
        v.appendChild(crumb('Поиск', r.corrected ? 'Исправлено: ' + r.text : r.text));
        var any = false;
        if (r.tracks && r.tracks.length) { any = true; v.appendChild(section('ТРЕКИ', trackRows(r.tracks))); }
        if (r.artists && r.artists.length) { any = true; v.appendChild(section('ИСПОЛНИТЕЛИ', artistRows(r.artists))); }
        if (r.albums && r.albums.length) { any = true; v.appendChild(section('АЛЬБОМЫ', albumRows(r.albums))); }
        if (r.playlists && r.playlists.length) { any = true; v.appendChild(section('ПЛЕЙЛИСТЫ', plRows(r.playlists))); }
        if (!any) v.appendChild(el('div', 'empty', 'Ничего не нашлось'));
        markCurrent();
    }

    function doSearch() {
        var t = $('q').value.trim();
        if (!t) { home(); return; }
        if (!cur || cur.q.indexOf('api=search') !== 0) { if (cur) stack.push(cur); }
        open('api=search&q=' + encodeURIComponent(t));
        $('q').blur();
    }
    $('q-go').addEventListener('click', doSearch);
    $('q').addEventListener('keydown', function (e) { if (e.key === 'Enter') doSearch(); });

    loadStatus();
    pollNow();
    setInterval(function () { if (!document.hidden) pollNow(); }, 3000);
})();
</script>
</body>
</html>
