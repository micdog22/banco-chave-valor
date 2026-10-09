/*
 * test_kv.c: o banco contra quedas, corrupção e um modelo de referência.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kv.h"
#include "teste.h"

static char dir[512], path[600], other[600];

/* ---------------------------------------------------------------------------
 * Utilidades
 * ------------------------------------------------------------------------ */

static kv *open_db(const char *p, kv_sync sync)
{
    kv *db = NULL;
    kv_options o = { sync, 20, false };
    kv_status st = kv_open(p, &o, &db);
    if (st != KV_OK) fprintf(stderr, "  kv_open(%s): %s\n", p, kv_strerror(st));
    CHECK_EQ(st, KV_OK);
    return db;
}

static uint64_t file_size(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (uint64_t)st.st_size : 0;
}

static uint8_t *read_file(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)len + 1);
    *n = fread(data, 1, (size_t)len, f);
    fclose(f);
    return data;
}

static void write_file(const char *p, const void *data, size_t n)
{
    FILE *f = fopen(p, "wb");
    if (!f) return;
    fwrite(data, 1, n, f);
    fclose(f);
}

/* Compara o valor guardado com o esperado (NULL: a chave não pode existir). */
static bool has_value(kv *db, const void *key, size_t klen, const void *want, size_t wlen)
{
    void *val;
    size_t len;
    kv_status st = kv_get(db, key, klen, &val, &len);
    if (!want) return st == KV_NOT_FOUND;
    bool ok = st == KV_OK && len == wlen && memcmp(val, want, wlen) == 0;
    free(val);
    return ok;
}

static bool has_str(kv *db, const char *key, const char *want)
{
    return has_value(db, key, strlen(key), want, want ? strlen(want) : 0);
}

/* ---------------------------------------------------------------------------
 * CRC, operações básicas e persistência
 * ------------------------------------------------------------------------ */

static void test_crc(void)
{
    CHECK_EQ(kv_crc32("", 0), 0x00000000u);
    CHECK_EQ(kv_crc32("a", 1), 0xE8B7BE43u);
    CHECK_EQ(kv_crc32("123456789", 9), 0xCBF43926u);   /* o valor de verificação clássico */
    CHECK_EQ(kv_crc32("The quick brown fox jumps over the lazy dog", 43), 0x414FA339u);
}

static void test_crud(void)
{
    unlink(path);
    kv *db = open_db(path, KV_SYNC_NEVER);
    if (!db) return;
    CHECK(has_str(db, "nome", NULL));
    CHECK_EQ(kv_put(db, "nome", 4, "Ana", 3), KV_OK);
    CHECK(has_str(db, "nome", "Ana"));
    CHECK_EQ(kv_put(db, "nome", 4, "Beatriz", 7), KV_OK);   /* sobrescrever: vale o último */
    CHECK(has_str(db, "nome", "Beatriz"));
    CHECK_EQ(kv_put(db, "cidade", 6, "Limeira", 7), KV_OK);
    CHECK_EQ(kv_delete(db, "nome", 4), KV_OK);
    CHECK(has_str(db, "nome", NULL));
    CHECK_EQ(kv_delete(db, "nome", 4), KV_NOT_FOUND);       /* apagar duas vezes */
    CHECK_EQ(kv_put(db, "vazio", 5, "", 0), KV_OK);          /* valor vazio é diferente de ausente */
    CHECK(has_str(db, "vazio", ""));
    CHECK_EQ(kv_put(db, "", 0, "chave vazia", 11), KV_OK);
    CHECK(has_str(db, "", "chave vazia"));
    kv_stats st;
    kv_get_stats(db, &st);
    CHECK_EQ(st.keys, 3);
    CHECK_EQ(st.records, 6);
    CHECK_EQ(kv_close(db), KV_OK);

    /* Tudo continua lá depois de reabrir, inclusive a remoção. */
    db = open_db(path, KV_SYNC_NEVER);
    if (!db) return;
    CHECK(has_str(db, "nome", NULL));
    CHECK(has_str(db, "cidade", "Limeira"));
    CHECK(has_str(db, "vazio", ""));
    CHECK(has_str(db, "", "chave vazia"));
    kv_get_stats(db, &st);
    CHECK_EQ(st.keys, 3);
    CHECK_EQ(st.recovered_bytes, 0);
    kv_close(db);
}

static void test_binary(void)
{
    unlink(path);
    kv *db = open_db(path, KV_SYNC_NEVER);
    if (!db) return;
    const char k1[3] = { 'a', 0, 'b' }, k2[3] = { 'a', 0, 'c' }, k3[1] = { 0 };
    const unsigned char v1[6] = { 0, 1, 0, 255, 0, 0 };
    CHECK_EQ(kv_put(db, k1, 3, v1, 6), KV_OK);
    CHECK_EQ(kv_put(db, k2, 3, "outro", 5), KV_OK);
    CHECK_EQ(kv_put(db, k3, 1, "zero", 4), KV_OK);
    CHECK(has_value(db, k1, 3, v1, 6));
    CHECK(has_value(db, k2, 3, "outro", 5));
    CHECK(has_value(db, "a", 1, NULL, 0));                 /* "a" não é "a\0b" */
    kv_close(db);
    db = open_db(path, KV_SYNC_NEVER);
    if (!db) return;
    CHECK(has_value(db, k1, 3, v1, 6));
    CHECK(has_value(db, k3, 1, "zero", 4));

    /* Um valor de 3 MB: maior que o bloco de leitura usado ao abrir. */
    size_t big = 3u << 20;
    unsigned char *v = malloc(big);
    for (size_t i = 0; i < big; i++) v[i] = (unsigned char)(i * 2654435761u >> 24);
    CHECK_EQ(kv_put(db, "grande", 6, v, big), KV_OK);
    kv_close(db);
    db = open_db(path, KV_SYNC_NEVER);
    if (db) {
        CHECK(has_value(db, "grande", 6, v, big));
        kv_close(db);
    }
    free(v);
}

static void test_limits_and_errors(void)
{
    unlink(path);
    kv *db = open_db(path, KV_SYNC_NEVER);
    if (!db) return;
    char *huge = calloc(KV_MAX_KEY + 1, 1);
    CHECK_EQ(kv_put(db, huge, KV_MAX_KEY + 1, "x", 1), KV_ERR_TOO_BIG);
    free(huge);
    kv_close(db);

    write_file(other, "isto nao e um banco", 19);
    CHECK_EQ(kv_open(other, NULL, &db), KV_ERR_FORMAT);
    unlink(other);
    kv_options ro = { KV_SYNC_NEVER, 0, true };
    CHECK_EQ(kv_open(other, &ro, &db), KV_ERR_IO);           /* só leitura não cria arquivo */
    CHECK(kv_strerror(KV_ERR_LOCKED)[0] != '\0');
}

static int collect(const void *key, size_t klen, void *ctx)
{
    char *out = ctx;
    size_t n = strlen(out);
    out[n] = '[';
    for (size_t i = 0; i < klen; i++) out[n + 1 + i] = ((const char *)key)[i] ? ((const char *)key)[i] : '0';
    out[n + 1 + klen] = ']';
    out[n + 2 + klen] = '\0';
    return 0;
}

static void test_keys_order(void)
{
    unlink(path);
    kv *db = open_db(path, KV_SYNC_NEVER);
    if (!db) return;
    kv_put(db, "b", 1, "", 0);
    kv_put(db, "a", 1, "", 0);
    kv_put(db, "ab", 2, "", 0);
    kv_put(db, "a\0", 2, "", 0);
    kv_put(db, "", 0, "", 0);
    char out[128] = "";
    kv_keys(db, "", 0, collect, out);
    CHECK_STR(out, "[][a][a0][ab][b]");                      /* ordem de bytes; mais curta primeiro */
    out[0] = '\0';
    kv_keys(db, "a", 1, collect, out);
    CHECK_STR(out, "[a][a0][ab]");
    kv_close(db);
}

/* ---------------------------------------------------------------------------
 * Quedas: o arquivo cortado em cada posição possível
 * ------------------------------------------------------------------------ */

typedef struct {
    const char *key;
    const char *val; /* NULL: remoção */
} op;

static const op OPS[] = {
    { "a", "1" }, { "b", "22" }, { "c", "333" }, { "a", "4444" }, { "b", NULL },
    { "d", "" }, { "e", "um valor um pouco maior, com acentuação" }, { "a", NULL },
    { "b", "de novo" }, { "f", "fim" }, { "c", NULL }, { "g", "último" },
};
#define NOPS (int)(sizeof OPS / sizeof OPS[0])
static const char *const KEYS[] = { "a", "b", "c", "d", "e", "f", "g" };
#define NKEYS 7

/* Valor de key depois das operações [0, upto], pulando a de índice skip. */
static const char *model(const char *key, int upto, int skip)
{
    const char *v = NULL;
    for (int i = 0; i <= upto; i++) {
        if (i != skip && strcmp(OPS[i].key, key) == 0) v = OPS[i].val;
    }
    return v;
}

static bool state_matches(kv *db, int upto, int skip)
{
    for (int k = 0; k < NKEYS; k++) {
        if (!has_str(db, KEYS[k], model(KEYS[k], upto, skip))) return false;
    }
    return true;
}

static void test_crash_truncation(void)
{
    unlink(path);
    kv *db = open_db(path, KV_SYNC_NEVER);
    if (!db) return;
    uint64_t ends[NOPS];
    for (int i = 0; i < NOPS; i++) {
        if (OPS[i].val) kv_put(db, OPS[i].key, strlen(OPS[i].key), OPS[i].val, strlen(OPS[i].val));
        else kv_delete(db, OPS[i].key, strlen(OPS[i].key));
        kv_stats st;
        kv_get_stats(db, &st);
        ends[i] = st.file_bytes; /* onde termina o registro da operação i */
    }
    kv_close(db);
    size_t size;
    uint8_t *full = read_file(path, &size);
    CHECK_EQ(size, ends[NOPS - 1]);

    /* Corta o arquivo em cada byte: tudo que foi escrito por inteiro antes do
     * corte tem de estar lá; o registro cortado some; o arquivo é aparado. */
    int wrong = 0;
    for (size_t t = 0; t <= size; t++) {
        write_file(other, full, t);
        kv *d;
        if (kv_open(other, NULL, &d) != KV_OK) {
            wrong++;
            continue;
        }
        int upto = -1;
        while (upto + 1 < NOPS && ends[upto + 1] <= t) upto++;
        uint64_t expect_size = upto >= 0 ? ends[upto] : 8;
        kv_stats st;
        kv_get_stats(d, &st);
        if (!state_matches(d, upto, -1) || st.file_bytes != expect_size ||
            (t >= 8 && st.recovered_bytes != t - expect_size)) {
            wrong++;
        }
        kv_close(d);
        if (file_size(other) != expect_size) wrong++;
    }
    printf("  arquivo cortado em %zu posições diferentes\n", size + 1);
    CHECK_EQ(wrong, 0);

    /* Lixo no fim (escrita interrompida no meio de um setor): descartado. */
    srand(7);
    wrong = 0;
    for (int trial = 0; trial < 50; trial++) {
        size_t extra = 1 + (size_t)(rand() % 200);
        uint8_t *junk = malloc(size + extra);
        memcpy(junk, full, size);
        for (size_t i = 0; i < extra; i++) junk[size + i] = (uint8_t)rand();
        write_file(other, junk, size + extra);
        free(junk);
        kv *d;
        if (kv_open(other, NULL, &d) != KV_OK) {
            wrong++;
            continue;
        }
        kv_stats st;
        kv_get_stats(d, &st);
        if (!state_matches(d, NOPS - 1, -1) || st.recovered_bytes != extra) wrong++;
        kv_close(d);
    }
    CHECK_EQ(wrong, 0);

    /* Um byte trocado no MEIO do arquivo não é queda: é estrago. O banco se
     * recusa a abrir em vez de jogar fora os registros bons que vêm depois,
     * e kv_repair salva todos os que conferem. */
    wrong = 0;
    int repaired_wrong = 0;
    for (int j = 0; j < NOPS - 1; j++) {
        uint64_t start = j ? ends[j - 1] : 8, len = ends[j] - start;
        for (int trial = 0; trial < 4; trial++) {
            uint8_t *bad = malloc(size);
            memcpy(bad, full, size);
            size_t at = (size_t)(start + (uint64_t)rand() % len);
            bad[at] ^= (uint8_t)(1 + rand() % 255);
            write_file(other, bad, size);
            free(bad);
            kv *d;
            if (kv_open(other, NULL, &d) != KV_ERR_CORRUPT) {
                if (d) kv_close(d);
                wrong++;
                continue;
            }
            uint64_t discarded;
            if (kv_repair(other, &discarded) != KV_OK || discarded != len) {
                repaired_wrong++;
                continue;
            }
            if (kv_open(other, NULL, &d) != KV_OK) {
                repaired_wrong++;
                continue;
            }
            if (!state_matches(d, NOPS - 1, j)) repaired_wrong++; /* só a operação j se perdeu */
            kv_close(d);
        }
    }
    CHECK_EQ(wrong, 0);
    CHECK_EQ(repaired_wrong, 0);

    /* O último registro estragado (comprimento certo, CRC errado) é queda. */
    uint8_t *bad = malloc(size);
    memcpy(bad, full, size);
    bad[size - 2] ^= 0x55;
    write_file(other, bad, size);
    free(bad);
    if (kv_open(other, NULL, &db) == KV_OK) {
        CHECK(state_matches(db, NOPS - 2, -1));
        kv_close(db);
    } else {
        CHECK(0);
    }

    /* Só para leitura: lê o que confere, mas não mexe no arquivo. */
    write_file(other, full, size - 3);
    kv_options ro = { KV_SYNC_NEVER, 0, true };
    if (kv_open(other, &ro, &db) == KV_OK) {
        CHECK(state_matches(db, NOPS - 2, -1));
        CHECK_EQ(kv_put(db, "x", 1, "y", 1), KV_ERR_READONLY);
        CHECK_EQ(kv_delete(db, "g", 1), KV_ERR_READONLY);
        CHECK_EQ(kv_compact(db), KV_ERR_READONLY);
        kv_close(db);
        CHECK_EQ(file_size(other), size - 3);
    } else {
        CHECK(0);
    }
    free(full);
}

static void test_corruption_on_read(void)
{
    unlink(path);
    kv *db = open_db(path, KV_SYNC_NEVER);
    if (!db) return;
    kv_put(db, "chave", 5, "valor original", 14);
    kv_put(db, "outra", 5, "x", 1);
    /* Um bit troca no disco depois que o banco já abriu: o get percebe. */
    int fd = open(path, O_WRONLY);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK_EQ(pwrite(fd, "V", 1, 8 + 21 + 5), 1);
        close(fd);
    }
    void *val;
    size_t len;
    CHECK_EQ(kv_get(db, "chave", 5, &val, &len), KV_ERR_CORRUPT);
    CHECK(has_str(db, "outra", "x"));
    kv_close(db);
}

/* ---------------------------------------------------------------------------
 * Compactação
 * ------------------------------------------------------------------------ */

static void test_compaction(void)
{
    unlink(path);
    kv *db = open_db(path, KV_SYNC_NEVER);
    if (!db) return;
    char key[16], val[64];
    for (int round = 0; round < 50; round++) {
        for (int k = 0; k < 10; k++) {
            snprintf(key, sizeof key, "chave%d", k);
            snprintf(val, sizeof val, "valor %d da rodada %d", k, round);
            kv_put(db, key, strlen(key), val, strlen(val));
        }
    }
    for (int k = 0; k < 10; k += 3) {
        snprintf(key, sizeof key, "chave%d", k);
        kv_delete(db, key, strlen(key));
    }
    kv_stats before, after;
    kv_get_stats(db, &before);
    CHECK_EQ(kv_compact(db), KV_OK);
    kv_get_stats(db, &after);
    printf("  compactação: %llu bytes para %llu bytes\n", (unsigned long long)before.file_bytes,
           (unsigned long long)after.file_bytes);
    CHECK_EQ(after.file_bytes, 8 + before.live_bytes);   /* sobrou exatamente o que vale */
    CHECK_EQ(after.records, 6);
    CHECK_EQ(file_size(path), after.file_bytes);
    CHECK(after.file_bytes * 20 < before.file_bytes);
    bool ok = true;
    for (int k = 0; k < 10; k++) {
        snprintf(key, sizeof key, "chave%d", k);
        snprintf(val, sizeof val, "valor %d da rodada 49", k);
        if (!has_str(db, key, k % 3 == 0 ? NULL : val)) ok = false;
    }
    CHECK(ok);
    /* Escrever depois de compactar continua funcionando (o arquivo é outro). */
    CHECK_EQ(kv_put(db, "nova", 4, "depois", 6), KV_OK);
    kv_close(db);

    /* Uma sobra de compactação interrompida é ignorada e apagada ao abrir. */
    char stale[700];
    snprintf(stale, sizeof stale, "%s.compactando", path);
    write_file(stale, "lixo", 4);
    db = open_db(path, KV_SYNC_NEVER);
    if (!db) return;
    CHECK(access(stale, F_OK) != 0);
    ok = true;
    for (int k = 0; k < 10; k++) {
        snprintf(key, sizeof key, "chave%d", k);
        snprintf(val, sizeof val, "valor %d da rodada 49", k);
        if (!has_str(db, key, k % 3 == 0 ? NULL : val)) ok = false;
    }
    CHECK(ok);
    CHECK(has_str(db, "nova", "depois"));
    kv_close(db);
}

/* ---------------------------------------------------------------------------
 * Trava entre processos e políticas de fsync
 * ------------------------------------------------------------------------ */

static void test_locking(void)
{
    unlink(path);
    kv *a = open_db(path, KV_SYNC_NEVER), *b = NULL;
    kv_options ro = { KV_SYNC_NEVER, 0, true };
    CHECK_EQ(kv_open(path, NULL, &b), KV_ERR_LOCKED);      /* segundo escritor */
    CHECK_EQ(kv_open(path, &ro, &b), KV_ERR_LOCKED);       /* leitor com escritor aberto */
    kv_close(a);
    kv *r1 = NULL, *r2 = NULL;
    CHECK_EQ(kv_open(path, &ro, &r1), KV_OK);              /* vários leitores juntos */
    CHECK_EQ(kv_open(path, &ro, &r2), KV_OK);
    CHECK_EQ(kv_open(path, NULL, &b), KV_ERR_LOCKED);      /* escritor com leitores abertos */
    kv_close(r1);
    kv_close(r2);
}

static void test_sync_policies(void)
{
    static const kv_sync modes[] = { KV_SYNC_ALWAYS, KV_SYNC_INTERVAL, KV_SYNC_NEVER };
    for (int m = 0; m < 3; m++) {
        unlink(path);
        kv *db = open_db(path, modes[m]);
        if (!db) return;
        bool ok = true;
        for (int i = 0; i < 20; i++) {
            char key[8];
            snprintf(key, sizeof key, "k%d", i);
            if (kv_put(db, key, strlen(key), "v", 1) != KV_OK) ok = false;
        }
        CHECK(ok);
        CHECK_EQ(kv_sync_now(db), KV_OK);
        CHECK_EQ(kv_close(db), KV_OK);
    }
}

/* ---------------------------------------------------------------------------
 * 100 mil operações aleatórias contra um modelo na memória
 * ------------------------------------------------------------------------ */

#define POOL 500

typedef struct {
    bool          present;
    unsigned char val[64];
    size_t        len;
} entry;

static size_t pool_key(int i, unsigned char *out)
{
    if (i == 0) return 0;                                       /* a chave vazia */
    if (i % 7 == 0) {                                           /* chaves binárias, com zeros */
        out[0] = 0;
        out[1] = (unsigned char)i;
        out[2] = 0;
        out[3] = (unsigned char)(i >> 8);
        return 4;
    }
    return (size_t)sprintf((char *)out, "usuario:%d", i);
}

static uint32_t rng_state = 2026;

static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static bool check_all(kv *db, const entry *m)
{
    unsigned char key[32];
    for (int i = 0; i < POOL; i++) {
        size_t klen = pool_key(i, key);
        if (!has_value(db, key, klen, m[i].present ? m[i].val : NULL, m[i].len)) return false;
    }
    return true;
}

static void test_random_model(void)
{
    unlink(path);
    static entry m[POOL];
    memset(m, 0, sizeof m);
    kv *db = open_db(path, KV_SYNC_NEVER);
    if (!db) return;
    int mismatches = 0, reopens = 0, compactions = 0;
    unsigned char key[32];
    for (int it = 0; it < 100000; it++) {
        int i = (int)(rnd() % POOL);
        size_t klen = pool_key(i, key);
        uint32_t r = rnd() % 1000;
        if (r < 450) {
            unsigned char val[64];
            size_t len = rnd() % 64;
            for (size_t j = 0; j < len; j++) val[j] = (unsigned char)rnd(); /* inclui zeros */
            if (kv_put(db, key, klen, val, len) != KV_OK) mismatches++;
            m[i].present = true;
            memcpy(m[i].val, val, len);
            m[i].len = len;
        } else if (r < 750) {
            if (!has_value(db, key, klen, m[i].present ? m[i].val : NULL, m[i].len)) mismatches++;
        } else if (r < 900) {
            kv_status st = kv_delete(db, key, klen);
            if (st != (m[i].present ? KV_OK : KV_NOT_FOUND)) mismatches++;
            m[i].present = false;
            m[i].len = 0;
        } else if (r < 902) {
            kv_close(db);                                      /* fecha e reabre no meio */
            db = open_db(path, KV_SYNC_NEVER);
            if (!db) return;
            reopens++;
        } else if (r == 902 && rnd() % 4 == 0) {
            if (kv_compact(db) != KV_OK) mismatches++;
            compactions++;
        }
    }
    CHECK_EQ(mismatches, 0);
    CHECK(check_all(db, m));
    size_t live = 0;
    for (int i = 0; i < POOL; i++) live += m[i].present;
    kv_stats st;
    kv_get_stats(db, &st);
    CHECK_EQ(st.keys, live);
    kv_close(db);
    db = open_db(path, KV_SYNC_NEVER);
    if (!db) return;
    CHECK(check_all(db, m));
    kv_close(db);
    printf("  100000 operações aleatórias (%d reaberturas, %d compactações) iguais ao modelo\n", reopens,
           compactions);
}

int main(void)
{
    const char *t = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/kvteste.XXXXXX", t && *t ? t : "/tmp");
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(path, sizeof path, "%s/banco.kv", dir);
    snprintf(other, sizeof other, "%s/copia.kv", dir);

    test_crc();
    test_crud();
    test_binary();
    test_limits_and_errors();
    test_keys_order();
    test_crash_truncation();
    test_corruption_on_read();
    test_compaction();
    test_locking();
    test_sync_policies();
    test_random_model();

    char stale[700];
    snprintf(stale, sizeof stale, "%s.compactando", path);
    unlink(path);
    unlink(other);
    unlink(stale);
    snprintf(stale, sizeof stale, "%s.compactando", other);
    unlink(stale);
    rmdir(dir);
    return teste_resumo("test_kv");
}
