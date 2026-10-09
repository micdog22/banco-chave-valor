/*
 * kv.c: o motor do banco: log em disco, índice na memória, recuperação,
 * compactação e trava de processo.
 *
 * Formato do arquivo:
 *   "KVLOG001"                                   assinatura (8 bytes)
 *   registro, registro, registro, ...            só cresce no fim
 *
 * Cada registro (inteiros em little-endian):
 *   CRC-32     4 bytes   de tudo o que vem depois dele no registro
 *   timestamp  8 bytes   microssegundos desde 1970
 *   tam. chave 4 bytes
 *   tam. valor 4 bytes
 *   flags      1 byte    bit 0: lápide (a chave foi apagada)
 *   chave, valor
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE

#include "kv.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAGIC          "KVLOG001"
#define MAGIC_LEN      8
#define HEADER         21
#define FLAG_TOMBSTONE 0x01
#define CHUNK          65536

/* ---------------------------------------------------------------------------
 * CRC-32 (IEEE 802.3), por tabela: um byte por passo
 * ------------------------------------------------------------------------ */

static const uint32_t CRC_TABLE[256] = {
    0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu, 0x076DC419u, 0x706AF48Fu,
    0xE963A535u, 0x9E6495A3u, 0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u,
    0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u, 0x1DB71064u, 0x6AB020F2u,
    0xF3B97148u, 0x84BE41DEu, 0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
    0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu, 0x14015C4Fu, 0x63066CD9u,
    0xFA0F3D63u, 0x8D080DF5u, 0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u,
    0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu, 0x35B5A8FAu, 0x42B2986Cu,
    0xDBBBC9D6u, 0xACBCF940u, 0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
    0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u, 0x21B4F4B5u, 0x56B3C423u,
    0xCFBA9599u, 0xB8BDA50Fu, 0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u,
    0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du, 0x76DC4190u, 0x01DB7106u,
    0x98D220BCu, 0xEFD5102Au, 0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
    0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u, 0x7F6A0DBBu, 0x086D3D2Du,
    0x91646C97u, 0xE6635C01u, 0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu,
    0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u, 0x65B0D9C6u, 0x12B7E950u,
    0x8BBEB8EAu, 0xFCB9887Cu, 0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
    0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u, 0x4ADFA541u, 0x3DD895D7u,
    0xA4D1C46Du, 0xD3D6F4FBu, 0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u,
    0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u, 0x5005713Cu, 0x270241AAu,
    0xBE0B1010u, 0xC90C2086u, 0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
    0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u, 0x59B33D17u, 0x2EB40D81u,
    0xB7BD5C3Bu, 0xC0BA6CADu, 0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au,
    0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u, 0xE3630B12u, 0x94643B84u,
    0x0D6D6A3Eu, 0x7A6A5AA8u, 0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
    0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu, 0xF762575Du, 0x806567CBu,
    0x196C3671u, 0x6E6B06E7u, 0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu,
    0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u, 0xD6D6A3E8u, 0xA1D1937Eu,
    0x38D8C2C4u, 0x4FDFF252u, 0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
    0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u, 0xDF60EFC3u, 0xA867DF55u,
    0x316E8EEFu, 0x4669BE79u, 0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u,
    0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu, 0xC5BA3BBEu, 0xB2BD0B28u,
    0x2BB45A92u, 0x5CB36A04u, 0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
    0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au, 0x9C0906A9u, 0xEB0E363Fu,
    0x72076785u, 0x05005713u, 0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u,
    0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u, 0x86D3D2D4u, 0xF1D4E242u,
    0x68DDB3F8u, 0x1FDA836Eu, 0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
    0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu, 0x8F659EFFu, 0xF862AE69u,
    0x616BFFD3u, 0x166CCF45u, 0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u,
    0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu, 0xAED16A4Au, 0xD9D65ADCu,
    0x40DF0B66u, 0x37D83BF0u, 0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
    0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u, 0xBAD03605u, 0xCDD70693u,
    0x54DE5729u, 0x23D967BFu, 0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u,
    0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du,
};

static uint32_t crc_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) crc = CRC_TABLE[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

uint32_t kv_crc32(const void *data, size_t len)
{
    return crc_update(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}

/* ---------------------------------------------------------------------------
 * Pequenas utilidades
 * ------------------------------------------------------------------------ */

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static bool pwrite_all(int fd, const void *data, size_t len, uint64_t off)
{
    const char *p = data;
    while (len > 0) {
        ssize_t n = pwrite(fd, p, len, (off_t)off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        len -= (size_t)n;
        off += (uint64_t)n;
    }
    return true;
}

static bool pread_all(int fd, void *data, size_t len, uint64_t off)
{
    char *p = data;
    while (len > 0) {
        ssize_t n = pread(fd, p, len, (off_t)off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
        off += (uint64_t)n;
    }
    return true;
}

/* No macOS, fsync entrega os dados ao disco, mas o disco pode segurá-los no
 * cache dele. F_FULLFSYNC pede que o cache também seja gravado. */
static int full_sync(int fd)
{
#ifdef F_FULLFSYNC
    if (fcntl(fd, F_FULLFSYNC) == 0) return 0;
#endif
    return fsync(fd);
}

/* ---------------------------------------------------------------------------
 * Índice: tabela hash com endereçamento aberto e sondagem linear
 * ------------------------------------------------------------------------ */

typedef struct {
    uint64_t hash;
    char    *key;      /* NULL: posição vazia */
    uint32_t klen;
    uint32_t vlen;
    uint64_t offset;   /* onde começa o registro no arquivo */
} slot;

struct kv {
    int        fd;
    char      *path;
    kv_options opt;
    slot      *slots;
    size_t     cap, count;   /* cap é potência de 2 */
    uint64_t   end;          /* fim do log: onde vai o próximo registro */
    uint64_t   records, live_bytes, recovered;
    long long  last_sync;
    bool       dirty;
};

static uint64_t hash_key(const void *key, size_t len)
{
    /* FNV-1a e uma mistura final (do MurmurHash3) para espalhar os bits. */
    const uint8_t *p = key;
    uint64_t h = 0xcbf29ce484222325u;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3u;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdu;
    h ^= h >> 33;
    return h;
}

static slot *find(const kv *db, const void *key, size_t klen, uint64_t h)
{
    size_t mask = db->cap - 1;
    for (size_t i = h & mask;; i = (i + 1) & mask) {
        slot *s = &db->slots[i];
        if (!s->key) return NULL;
        if (s->hash == h && s->klen == klen && memcmp(s->key, key, klen) == 0) return s;
    }
}

static bool grow(kv *db)
{
    size_t cap = db->cap ? db->cap * 2 : 64;
    slot *n = calloc(cap, sizeof *n);
    if (!n) return false;
    for (size_t i = 0; i < db->cap; i++) {
        if (!db->slots[i].key) continue;
        size_t j = db->slots[i].hash & (cap - 1);
        while (n[j].key) j = (j + 1) & (cap - 1);
        n[j] = db->slots[i];
    }
    free(db->slots);
    db->slots = n;
    db->cap = cap;
    return true;
}

/* Garante espaço para mais uma chave (carga máxima de 70%). */
static bool reserve(kv *db)
{
    return (db->count + 1) * 10 <= db->cap * 7 || grow(db);
}

/* Remove uma posição sem deixar "lápide" na tabela: as entradas seguintes
 * do mesmo aglomerado voltam uma casa quando podem (algoritmo R de Knuth). */
static void remove_slot(kv *db, slot *s)
{
    size_t mask = db->cap - 1, i = (size_t)(s - db->slots);
    free(db->slots[i].key);
    for (;;) {
        db->slots[i].key = NULL;
        size_t j = i;
        for (;;) {
            j = (j + 1) & mask;
            if (!db->slots[j].key) return;
            size_t k = db->slots[j].hash & mask; /* posição ideal de j */
            /* j só pode ir para i se a posição ideal dele não estiver em (i, j]. */
            bool stays = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
            if (!stays) break;
        }
        db->slots[i] = db->slots[j];
        i = j;
    }
}

static uint64_t record_size(uint32_t klen, uint32_t vlen)
{
    return (uint64_t)HEADER + klen + vlen;
}

/* Aplica um registro ao índice (ao abrir e depois de cada escrita). copy,
 * se não for NULL, é uma cópia da chave já alocada (e espaço já reservado):
 * assim, depois de gravar no disco, atualizar o índice não pode falhar. */
static kv_status apply(kv *db, const void *key, uint32_t klen, uint32_t vlen, uint8_t flags, uint64_t off,
                       char *copy)
{
    uint64_t h = hash_key(key, klen);
    slot *s = db->cap ? find(db, key, klen, h) : NULL;
    if (flags & FLAG_TOMBSTONE) {
        free(copy);
        if (s) {
            db->live_bytes -= record_size(s->klen, s->vlen);
            remove_slot(db, s);
            db->count--;
        }
        return KV_OK;
    }
    if (s) {
        free(copy);
        db->live_bytes -= record_size(s->klen, s->vlen); /* o valor antigo vira lixo */
    } else {
        if (!copy) {
            if (!reserve(db)) return KV_ERR_NOMEM;
            copy = malloc(klen ? klen : 1);
            if (!copy) return KV_ERR_NOMEM;
            memcpy(copy, key, klen);
        }
        size_t mask = db->cap - 1, i = h & mask;
        while (db->slots[i].key) i = (i + 1) & mask;
        s = &db->slots[i];
        s->hash = h;
        s->key = copy;
        s->klen = klen;
        db->count++;
    }
    s->vlen = vlen;
    s->offset = off;
    db->live_bytes += record_size(klen, vlen);
    return KV_OK;
}

/* ---------------------------------------------------------------------------
 * Leitura do log ao abrir
 * ------------------------------------------------------------------------ */

typedef struct {
    int      fd;
    uint8_t  buf[CHUNK];
    size_t   len, pos;
} reader;

/* Copia até n bytes da posição atual (dst NULL: só passa pelo CRC). */
static size_t take(reader *r, void *dst, size_t n, uint32_t *crc)
{
    size_t done = 0;
    while (done < n) {
        if (r->pos == r->len) {
            ssize_t got = read(r->fd, r->buf, CHUNK);
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) break;
            r->len = (size_t)got;
            r->pos = 0;
        }
        size_t k = r->len - r->pos;
        if (k > n - done) k = n - done;
        if (crc) *crc = crc_update(*crc, r->buf + r->pos, k);
        if (dst) memcpy((uint8_t *)dst + done, r->buf + r->pos, k);
        r->pos += k;
        done += k;
    }
    return done;
}

typedef enum { SCAN_CLEAN, SCAN_TORN, SCAN_CORRUPT, SCAN_NOMEM, SCAN_IO } scan_result;

static bool header_ok(const uint8_t *h, uint64_t at, uint64_t size, uint64_t *total)
{
    uint32_t klen = get32(h + 12), vlen = get32(h + 16);
    uint8_t flags = h[20];
    if (klen > KV_MAX_KEY || vlen > KV_MAX_VALUE || (flags & ~FLAG_TOMBSTONE) || ((flags & FLAG_TOMBSTONE) && vlen)) {
        return false;
    }
    *total = record_size(klen, vlen);
    return at + *total <= size;
}

/* Confere o registro que começa em at (lendo direto do arquivo). */
static bool record_valid_at(int fd, uint64_t at, uint64_t size, uint64_t *total, uint8_t *buf)
{
    uint8_t h[HEADER];
    if (at + HEADER > size || !pread_all(fd, h, HEADER, at) || !header_ok(h, at, size, total)) return false;
    uint32_t crc = crc_update(0xFFFFFFFFu, h + 4, HEADER - 4);
    for (uint64_t p = at + HEADER; p < at + *total;) {
        size_t n = at + *total - p > CHUNK ? CHUNK : (size_t)(at + *total - p);
        if (!pread_all(fd, buf, n, p)) return false;
        crc = crc_update(crc, buf, n);
        p += n;
    }
    return (crc ^ 0xFFFFFFFFu) == get32(h);
}

typedef enum { RESYNC_NONE, RESYNC_FOUND, RESYNC_GAVE_UP } resync_result;

/* Procura um registro válido começando em alguma posição depois de from.
 * É assim que se separa uma escrita interrompida (nada válido depois dela)
 * de um estrago no meio do arquivo (há registros bons depois). Um registro
 * aleatório só passaria se acertasse o CRC: uma chance em 4 bilhões. */
static resync_result resync(int fd, uint64_t from, uint64_t size, uint64_t *found)
{
    uint8_t *buf = malloc(CHUNK);
    if (!buf) return RESYNC_GAVE_UP;
    uint64_t budget = 64u << 20; /* no máximo 64 MB conferidos: depois disso, desiste */
    resync_result r = RESYNC_NONE;
    for (uint64_t p = from; p + HEADER <= size; p++) {
        uint8_t h[HEADER];
        uint64_t total;
        if (!pread_all(fd, h, HEADER, p)) break;
        if (!header_ok(h, p, size, &total)) continue;
        if (total > budget) {
            r = RESYNC_GAVE_UP;
            break;
        }
        budget -= total;
        if (record_valid_at(fd, p, size, &total, buf)) {
            *found = p;
            r = RESYNC_FOUND;
            break;
        }
    }
    free(buf);
    return r;
}

/*
 * Percorre os registros a partir da assinatura e monta o índice. Para no
 * primeiro registro que não confere (cabeçalho cortado, tamanho absurdo,
 * CRC errado) e classifica o problema:
 *   SCAN_TORN    não existe nenhum registro válido depois dele: a última
 *                escrita foi interrompida (queda de energia, processo morto).
 *                É seguro cortar o arquivo ali.
 *   SCAN_CORRUPT existem registros válidos depois: o arquivo foi danificado,
 *                e cortar perderia dados bons. Isso fica para o kv_repair.
 * *stop recebe a posição do registro problemático (ou o fim do arquivo).
 */
static scan_result scan(kv *db, int fd, uint64_t size, uint64_t *stop, uint64_t *count)
{
    reader *r = malloc(sizeof *r);
    if (!r) return SCAN_NOMEM;
    r->fd = fd;
    r->len = r->pos = 0;
    if (lseek(fd, MAGIC_LEN, SEEK_SET) < 0) {
        free(r);
        return SCAN_IO;
    }
    uint64_t off = MAGIC_LEN;
    scan_result res = SCAN_CLEAN;
    char *key = NULL;
    *count = 0;
    while (off < size) {
        uint8_t h[HEADER];
        uint64_t total;
        bool bad = take(r, h, HEADER, NULL) < HEADER || !header_ok(h, off, size, &total);
        if (!bad) {
            uint32_t klen = get32(h + 12), vlen = get32(h + 16);
            uint32_t crc = crc_update(0xFFFFFFFFu, h + 4, HEADER - 4);
            char *grown = realloc(key, klen ? klen : 1);
            if (!grown) {
                res = SCAN_NOMEM;
                break;
            }
            key = grown;
            bad = take(r, key, klen, &crc) < klen || take(r, NULL, vlen, &crc) < vlen ||
                  (crc ^ 0xFFFFFFFFu) != get32(h);
            if (!bad) {
                if (db && apply(db, key, klen, vlen, h[20], off, NULL) != KV_OK) {
                    res = SCAN_NOMEM;
                    break;
                }
                (*count)++;
                off += total;
                continue;
            }
        }
        uint64_t next;
        res = resync(fd, off + 1, size, &next) == RESYNC_NONE ? SCAN_TORN : SCAN_CORRUPT;
        break;
    }
    *stop = off;
    free(key);
    free(r);
    return res;
}

/* ---------------------------------------------------------------------------
 * Abrir e fechar
 * ------------------------------------------------------------------------ */

static void free_index(kv *db)
{
    for (size_t i = 0; i < db->cap; i++) free(db->slots[i].key);
    free(db->slots);
    db->slots = NULL;
    db->cap = db->count = 0;
}

static void destroy(kv *db)
{
    if (db->fd >= 0) close(db->fd); /* fechar também solta a trava */
    free_index(db);
    free(db->path);
    free(db);
}

static char *tmp_path(const char *path)
{
    size_t n = strlen(path);
    char *t = malloc(n + 13);
    if (t) {
        memcpy(t, path, n);
        memcpy(t + n, ".compactando", 13);
    }
    return t;
}

kv_status kv_open(const char *path, const kv_options *opt, kv **out)
{
    *out = NULL;
    kv *db = calloc(1, sizeof *db);
    if (!db) return KV_ERR_NOMEM;
    db->fd = -1;
    db->opt = opt ? *opt : (kv_options){ KV_SYNC_INTERVAL, 100, false };
    db->path = strdup(path);
    if (!db->path) {
        destroy(db);
        return KV_ERR_NOMEM;
    }
    bool ro = db->opt.read_only;
    db->fd = open(path, (ro ? O_RDONLY : O_RDWR | O_CREAT) | O_CLOEXEC, 0644);
    if (db->fd < 0) {
        destroy(db);
        return KV_ERR_IO;
    }
    /* Um escritor (trava exclusiva) ou vários leitores (trava compartilhada).
     * LOCK_NB: se não der, avisa na hora em vez de esperar. */
    if (flock(db->fd, (ro ? LOCK_SH : LOCK_EX) | LOCK_NB) != 0) {
        kv_status st = (errno == EWOULDBLOCK || errno == EAGAIN) ? KV_ERR_LOCKED : KV_ERR_IO;
        destroy(db);
        return st;
    }
    if (!ro) {
        /* Sobra de uma compactação interrompida: o original continua valendo. */
        char *t = tmp_path(path);
        if (t) unlink(t);
        free(t);
    }

    struct stat st;
    if (fstat(db->fd, &st) != 0) {
        destroy(db);
        return KV_ERR_IO;
    }
    uint64_t size = (uint64_t)st.st_size;
    char magic[MAGIC_LEN];
    if (size < MAGIC_LEN) {
        /* Arquivo novo, ou que caiu durante a criação (só um pedaço da assinatura). */
        if (size > 0 && (!pread_all(db->fd, magic, (size_t)size, 0) || memcmp(magic, MAGIC, (size_t)size) != 0)) {
            destroy(db);
            return KV_ERR_FORMAT;
        }
        if (ro) {
            destroy(db);
            return size == 0 ? KV_ERR_FORMAT : KV_ERR_CORRUPT;
        }
        if (!pwrite_all(db->fd, MAGIC, MAGIC_LEN, 0) || ftruncate(db->fd, MAGIC_LEN) != 0 ||
            full_sync(db->fd) != 0) {
            destroy(db);
            return KV_ERR_IO;
        }
        size = MAGIC_LEN;
    } else if (!pread_all(db->fd, magic, MAGIC_LEN, 0) || memcmp(magic, MAGIC, MAGIC_LEN) != 0) {
        destroy(db);
        return KV_ERR_FORMAT;
    }

    uint64_t stop, count;
    scan_result res = scan(db, db->fd, size, &stop, &count);
    if (res == SCAN_CORRUPT || res == SCAN_NOMEM || res == SCAN_IO) {
        destroy(db);
        return res == SCAN_CORRUPT ? KV_ERR_CORRUPT : res == SCAN_NOMEM ? KV_ERR_NOMEM : KV_ERR_IO;
    }
    if (res == SCAN_TORN) {
        /* Recuperação: o fim incompleto é descartado. Quem só lê ignora esses
         * bytes sem mexer no arquivo; quem escreve corta de verdade. */
        db->recovered = size - stop;
        if (!ro && (ftruncate(db->fd, (off_t)stop) != 0 || full_sync(db->fd) != 0)) {
            destroy(db);
            return KV_ERR_IO;
        }
    }
    db->end = stop;
    db->records = count;
    db->last_sync = now_ms();
    *out = db;
    return KV_OK;
}

static kv_status sync_now(kv *db)
{
    if (full_sync(db->fd) != 0) return KV_ERR_IO;
    db->dirty = false;
    db->last_sync = now_ms();
    return KV_OK;
}

kv_status kv_sync_now(kv *db)
{
    return db->opt.read_only ? KV_OK : sync_now(db);
}

kv_status kv_close(kv *db)
{
    if (!db) return KV_OK;
    kv_status st = KV_OK;
    if (!db->opt.read_only && db->dirty && db->opt.sync != KV_SYNC_NEVER) st = sync_now(db);
    destroy(db);
    return st;
}

/* ---------------------------------------------------------------------------
 * Escrita, leitura e remoção
 * ------------------------------------------------------------------------ */

static kv_status append(kv *db, const void *key, uint32_t klen, const void *val, uint32_t vlen, uint8_t flags,
                        char *copy)
{
    uint8_t h[HEADER];
    put64(h + 4, now_us());
    put32(h + 12, klen);
    put32(h + 16, vlen);
    h[20] = flags;
    uint32_t crc = crc_update(0xFFFFFFFFu, h + 4, HEADER - 4);
    crc = crc_update(crc, key, klen);
    crc = crc_update(crc, val, vlen);
    put32(h, crc ^ 0xFFFFFFFFu);

    uint64_t off = db->end;
    if (!pwrite_all(db->fd, h, HEADER, off) || !pwrite_all(db->fd, key, klen, off + HEADER) ||
        !pwrite_all(db->fd, val, vlen, off + HEADER + klen)) {
        /* Escrita pela metade (disco cheio, por exemplo): desfaz o pedaço. */
        if (ftruncate(db->fd, (off_t)off) != 0) {
            /* sem como desfazer: ao abrir de novo, o CRC acusa o fim cortado */
        }
        free(copy);
        return KV_ERR_IO;
    }
    apply(db, key, klen, vlen, flags, off, copy); /* não falha: memória já reservada */
    db->end = off + record_size(klen, vlen);
    db->records++;
    db->dirty = true;
    if (db->opt.sync == KV_SYNC_ALWAYS ||
        (db->opt.sync == KV_SYNC_INTERVAL && now_ms() - db->last_sync >= db->opt.interval_ms)) {
        return sync_now(db);
    }
    return KV_OK;
}

kv_status kv_put(kv *db, const void *key, size_t klen, const void *val, size_t vlen)
{
    if (db->opt.read_only) return KV_ERR_READONLY;
    if (klen > KV_MAX_KEY || vlen > KV_MAX_VALUE) return KV_ERR_TOO_BIG;
    /* Chave nova: reserva espaço no índice e copia a chave antes de escrever.
     * Se faltasse memória depois, o disco e o índice ficariam diferentes. */
    char *copy = NULL;
    if (!db->cap || !find(db, key, klen, hash_key(key, klen))) {
        if (!reserve(db)) return KV_ERR_NOMEM;
        copy = malloc(klen ? klen : 1);
        if (!copy) return KV_ERR_NOMEM;
        memcpy(copy, key, klen);
    }
    return append(db, key, (uint32_t)klen, val, (uint32_t)vlen, 0, copy);
}

kv_status kv_delete(kv *db, const void *key, size_t klen)
{
    if (db->opt.read_only) return KV_ERR_READONLY;
    if (klen > KV_MAX_KEY) return KV_NOT_FOUND;
    if (!db->cap || !find(db, key, klen, hash_key(key, klen))) return KV_NOT_FOUND;
    return append(db, key, (uint32_t)klen, "", 0, FLAG_TOMBSTONE, NULL); /* a lápide */
}

kv_status kv_get(kv *db, const void *key, size_t klen, void **val, size_t *vlen)
{
    *val = NULL;
    *vlen = 0;
    if (klen > KV_MAX_KEY || !db->cap) return KV_NOT_FOUND;
    const slot *s = find(db, key, klen, hash_key(key, klen));
    if (!s) return KV_NOT_FOUND;
    uint8_t *head = malloc(HEADER + (size_t)s->klen);
    uint8_t *value = malloc(s->vlen ? s->vlen : 1);
    if (!head || !value) {
        free(head);
        free(value);
        return KV_ERR_NOMEM;
    }
    if (!pread_all(db->fd, head, HEADER + (size_t)s->klen, s->offset) ||
        !pread_all(db->fd, value, s->vlen, s->offset + HEADER + s->klen)) {
        free(head);
        free(value);
        return KV_ERR_IO;
    }
    /* O CRC é conferido a cada leitura: um bit trocado no disco vira erro,
     * não um valor errado devolvido em silêncio. */
    uint32_t crc = crc_update(0xFFFFFFFFu, head + 4, HEADER - 4 + s->klen);
    crc = crc_update(crc, value, s->vlen);
    bool ok = (crc ^ 0xFFFFFFFFu) == get32(head) && get32(head + 12) == s->klen && get32(head + 16) == s->vlen &&
              memcmp(head + HEADER, key, klen) == 0;
    free(head);
    if (!ok) {
        free(value);
        return KV_ERR_CORRUPT;
    }
    *val = value;
    *vlen = s->vlen;
    return KV_OK;
}

/* ---------------------------------------------------------------------------
 * Listagem, estatísticas e compactação
 * ------------------------------------------------------------------------ */

static int cmp_slots(const void *a, const void *b)
{
    const slot *x = *(const slot *const *)a, *y = *(const slot *const *)b;
    size_t n = x->klen < y->klen ? x->klen : y->klen;
    int c = memcmp(x->key, y->key, n);
    if (c) return c;
    return x->klen < y->klen ? -1 : x->klen > y->klen ? 1 : 0;
}

kv_status kv_keys(kv *db, const void *prefix, size_t plen, kv_visit visit, void *ctx)
{
    slot **list = malloc((db->count ? db->count : 1) * sizeof *list);
    if (!list) return KV_ERR_NOMEM;
    size_t n = 0;
    for (size_t i = 0; i < db->cap; i++) {
        const slot *s = &db->slots[i];
        if (s->key && s->klen >= plen && (plen == 0 || memcmp(s->key, prefix, plen) == 0)) list[n++] = &db->slots[i];
    }
    qsort(list, n, sizeof *list, cmp_slots);
    for (size_t i = 0; i < n; i++) {
        if (visit(list[i]->key, list[i]->klen, ctx)) break;
    }
    free(list);
    return KV_OK;
}

void kv_get_stats(const kv *db, kv_stats *st)
{
    st->keys = db->count;
    st->records = db->records;
    st->file_bytes = db->end;
    st->live_bytes = db->live_bytes;
    st->recovered_bytes = db->recovered;
}

static int cmp_offset(const void *a, const void *b)
{
    const slot *x = *(const slot *const *)a, *y = *(const slot *const *)b;
    return x->offset < y->offset ? -1 : x->offset > y->offset ? 1 : 0;
}

/* Faz fsync da pasta, para o rename também ficar gravado. */
static void sync_dir(const char *path)
{
    char *copy = strdup(path);
    if (!copy) return;
    int fd = open(dirname(copy), O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
    free(copy);
}

kv_status kv_compact(kv *db)
{
    if (db->opt.read_only) return KV_ERR_READONLY;
    char *tmp = tmp_path(db->path);
    slot **order = malloc((db->count ? db->count : 1) * sizeof *order);
    uint64_t *offsets = malloc((db->count ? db->count : 1) * sizeof *offsets);
    uint8_t *buf = NULL;
    int nfd = -1;
    kv_status st = KV_ERR_NOMEM;
    if (!tmp || !order || !offsets) goto fail;

    /* Os registros vivos são copiados na ordem em que estão no arquivo
     * antigo: a leitura fica sequencial. */
    size_t n = 0;
    for (size_t i = 0; i < db->cap; i++) {
        if (db->slots[i].key) order[n++] = &db->slots[i];
    }
    qsort(order, n, sizeof *order, cmp_offset);

    st = KV_ERR_IO;
    nfd = open(tmp, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (nfd < 0 || flock(nfd, LOCK_EX | LOCK_NB) != 0) goto fail;
    if (!pwrite_all(nfd, MAGIC, MAGIC_LEN, 0)) goto fail;
    uint64_t end = MAGIC_LEN;
    size_t cap = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t size = record_size(order[i]->klen, order[i]->vlen);
        if (size > cap) {
            uint8_t *grown = realloc(buf, (size_t)size);
            if (!grown) {
                st = KV_ERR_NOMEM;
                goto fail;
            }
            buf = grown;
            cap = (size_t)size;
        }
        /* O registro é copiado byte a byte (é autossuficiente), mas o CRC é
         * conferido antes: compactar não pode espalhar um registro estragado. */
        if (!pread_all(db->fd, buf, (size_t)size, order[i]->offset)) goto fail;
        if (kv_crc32(buf + 4, (size_t)size - 4) != get32(buf)) {
            st = KV_ERR_CORRUPT;
            goto fail;
        }
        if (!pwrite_all(nfd, buf, (size_t)size, end)) goto fail;
        offsets[i] = end;
        end += size;
    }
    /* Primeiro o arquivo novo inteiro no disco; só então a troca. O rename é
     * atômico: quem abrir o caminho vê o arquivo antigo ou o novo, nunca uma
     * mistura. Se cair antes do rename, o antigo continua intacto. */
    if (full_sync(nfd) != 0 || rename(tmp, db->path) != 0) goto fail;
    sync_dir(db->path);

    close(db->fd);
    db->fd = nfd;
    for (size_t i = 0; i < n; i++) order[i]->offset = offsets[i];
    db->end = end;
    db->records = n;
    db->live_bytes = end - MAGIC_LEN;
    db->dirty = false;
    db->last_sync = now_ms();
    free(tmp);
    free(order);
    free(offsets);
    free(buf);
    return KV_OK;

fail:
    if (nfd >= 0) {
        close(nfd);
        unlink(tmp);
    }
    free(tmp);
    free(order);
    free(offsets);
    free(buf);
    return st;
}

kv_status kv_repair(const char *path, uint64_t *discarded)
{
    *discarded = 0;
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return KV_ERR_IO;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return KV_ERR_LOCKED;
    }
    struct stat st;
    char magic[MAGIC_LEN];
    if (fstat(fd, &st) != 0 || (uint64_t)st.st_size < MAGIC_LEN || !pread_all(fd, magic, MAGIC_LEN, 0) ||
        memcmp(magic, MAGIC, MAGIC_LEN) != 0) {
        close(fd);
        return KV_ERR_FORMAT;
    }
    uint64_t size = (uint64_t)st.st_size;
    char *tmp = tmp_path(path);
    uint8_t *buf = malloc(CHUNK);
    int nfd = tmp ? open(tmp, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644) : -1;
    kv_status result = KV_ERR_IO;
    if (!tmp || !buf) result = KV_ERR_NOMEM;
    if (nfd < 0 || !buf || !pwrite_all(nfd, MAGIC, MAGIC_LEN, 0)) goto done;

    /* Copia todo registro que confere; pula os trechos estragados até o
     * próximo registro válido. */
    uint64_t in = MAGIC_LEN, out = MAGIC_LEN;
    while (in < size) {
        uint64_t total;
        if (record_valid_at(fd, in, size, &total, buf)) {
            for (uint64_t p = 0; p < total;) {
                size_t n = total - p > CHUNK ? CHUNK : (size_t)(total - p);
                if (!pread_all(fd, buf, n, in + p) || !pwrite_all(nfd, buf, n, out + p)) goto done;
                p += n;
            }
            in += total;
            out += total;
            continue;
        }
        uint64_t next;
        if (resync(fd, in + 1, size, &next) != RESYNC_FOUND) break;
        in = next;
    }
    if (full_sync(nfd) != 0 || rename(tmp, path) != 0) goto done;
    sync_dir(path);
    *discarded = size - out;
    result = KV_OK;
done:
    if (nfd >= 0) close(nfd);
    if (result != KV_OK && tmp) unlink(tmp);
    close(fd);
    free(tmp);
    free(buf);
    return result;
}

const char *kv_strerror(kv_status s)
{
    switch (s) {
    case KV_OK: return "ok";
    case KV_NOT_FOUND: return "chave não encontrada";
    case KV_ERR_IO: return "erro de leitura ou escrita no disco";
    case KV_ERR_LOCKED: return "o banco está em uso por outro processo";
    case KV_ERR_CORRUPT: return "o arquivo está corrompido no meio (use o comando reparar)";
    case KV_ERR_FORMAT: return "o arquivo não é um banco deste formato";
    case KV_ERR_TOO_BIG: return "chave ou valor maior que o limite";
    case KV_ERR_READONLY: return "o banco foi aberto só para leitura";
    case KV_ERR_NOMEM: return "memória insuficiente";
    }
    return "erro desconhecido";
}
