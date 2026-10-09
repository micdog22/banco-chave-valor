/*
 * test_queda.c: queda de verdade. Um processo filho grava sem parar e avisa
 * o pai, por um pipe, a cada gravação confirmada. O pai mata o filho com
 * SIGKILL num momento qualquer (sem chance de fechar nada) e depois confere
 * que tudo o que foi confirmado está no banco.
 *
 * SIGKILL derruba o processo, não o sistema: o que já passou por write()
 * está no cache do sistema operacional. Quedas de energia, em que o cache
 * se perde, são simuladas em test_kv.c cortando o arquivo em cada posição.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE

#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kv.h"
#include "teste.h"

static void value_for(int i, char *out, size_t cap)
{
    /* Valores de tamanhos variados, para os cortes caírem em lugares diferentes. */
    int n = snprintf(out, cap, "valor-%d-", i);
    for (int k = 0; k < i % 50 && n + 1 < (int)cap; k++) out[n++] = (char)('a' + (i + k) % 26);
    out[n] = '\0';
}

static void child(const char *path, int out_fd)
{
    kv *db;
    kv_options o = { KV_SYNC_NEVER, 0, false };
    if (kv_open(path, &o, &db) != KV_OK) _exit(3);
    for (int i = 0;; i++) {
        char key[32], val[96];
        snprintf(key, sizeof key, "chave-%d", i);
        value_for(i, val, sizeof val);
        if (kv_put(db, key, strlen(key), val, strlen(val)) != KV_OK) _exit(4);
        if (i % 3 == 2) {
            snprintf(key, sizeof key, "chave-%d", i - 1);
            if (kv_delete(db, key, strlen(key)) != KV_OK) _exit(5);
        }
        /* Só depois de a escrita voltar com sucesso o pai fica sabendo. */
        if (write(out_fd, &i, sizeof i) != (ssize_t)sizeof i) _exit(6);
    }
}

int main(void)
{
    const char *t = getenv("TMPDIR");
    char dir[512], path[600];
    snprintf(dir, sizeof dir, "%s/kvqueda.XXXXXX", t && *t ? t : "/tmp");
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(path, sizeof path, "%s/banco.kv", dir);
    srand(2026); /* os pontos de parada variam; o momento exato do SIGKILL, não */

    for (int round = 0; round < 8; round++) {
        unlink(path);
        int fds[2];
        if (pipe(fds) != 0) return 2;
        pid_t pid = fork();
        if (pid < 0) return 2;
        if (pid == 0) {
            close(fds[0]);
            child(path, fds[1]);
        }
        close(fds[1]);
        int target = 200 + rand() % 3000, last = -1, got;
        while (last < target && read(fds[0], &got, sizeof got) == (ssize_t)sizeof got) last = got;
        kill(pid, SIGKILL); /* no meio do que estiver fazendo */
        while (read(fds[0], &got, sizeof got) == (ssize_t)sizeof got) last = got; /* avisos que já estavam no pipe */
        close(fds[0]);
        int status;
        waitpid(pid, &status, 0);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);

        kv *db;
        kv_status st = kv_open(path, NULL, &db);
        CHECK_EQ(st, KV_OK);
        if (st != KV_OK) continue;
        int lost = 0;
        for (int i = 0; i <= last; i++) {
            char key[32], val[96];
            snprintf(key, sizeof key, "chave-%d", i);
            value_for(i, val, sizeof val);
            /* chave-(3k+1) é apagada logo depois de chave-(3k+2) ser gravada.
             * Se a última confirmação foi a de 3k+1, o filho pode ter morrido
             * depois de apagá-la e antes de avisar: as duas respostas valem. */
            bool deleted = i % 3 == 1 && i + 1 <= last;
            bool either = i % 3 == 1 && i == last;
            void *v;
            size_t len;
            kv_status g = kv_get(db, key, strlen(key), &v, &len);
            bool right_value = g == KV_OK && len == strlen(val) && memcmp(v, val, len) == 0;
            if (deleted) {
                if (g != KV_NOT_FOUND) lost++;
            } else if (either) {
                if (!right_value && g != KV_NOT_FOUND) lost++;
            } else if (!right_value) {
                lost++;
            }
            if (g == KV_OK) free(v);
        }
        kv_stats s;
        kv_get_stats(db, &s);
        kv_close(db);
        printf("  rodada %d: morto depois de %d gravações confirmadas, %llu bytes descartados ao abrir, %d perdidas\n",
               round + 1, last + 1, (unsigned long long)s.recovered_bytes, lost);
        CHECK_EQ(lost, 0);
    }
    unlink(path);
    rmdir(dir);
    return teste_resumo("test_queda");
}
