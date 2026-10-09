/*
 * kv_cli.c: o programa kv, com comandos avulsos e um terminal interativo.
 *
 * Códigos de saída: 0 deu certo, 1 chave não encontrada, 2 erro.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kv.h"

#define VERSION "1.0.0"
#define MAX_ARGS 8

static void usage(FILE *f)
{
    fputs("uso: kv -f ARQUIVO [opções] [comando]\n"
          "\n"
          "Banco de dados chave-valor num arquivo só, que sobrevive a quedas.\n"
          "\n"
          "Comandos:\n"
          "  set CHAVE VALOR   grava (substitui o valor, se a chave já existir)\n"
          "  get CHAVE         mostra o valor\n"
          "  del CHAVE         apaga a chave\n"
          "  list [PREFIXO]    lista as chaves em ordem\n"
          "  compactar         reescreve o arquivo só com o que ainda vale\n"
          "  stats             mostra números do arquivo\n"
          "  reparar           salva os registros bons de um arquivo danificado\n"
          "Sem comando, abre um terminal interativo.\n"
          "\n"
          "Opções:\n"
          "  -f, --arquivo ARQ  arquivo do banco (criado se não existir)\n"
          "  --fsync MODO       sempre, nunca ou um intervalo em ms (padrão: 100)\n"
          "  -e                 interpreta \\xHH, \\n, \\t e \\\\ nas chaves e valores\n"
          "  -h, --help         mostra esta ajuda\n"
          "  -v, --versao       mostra a versão\n",
          f);
}

/* ---------------------------------------------------------------------------
 * Texto com escapes: o jeito de digitar bytes quaisquer (inclusive o zero)
 * ------------------------------------------------------------------------ */

typedef struct {
    char  *data;
    size_t len;
} bytes;

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Converte escapes no lugar. Devolve false se houver um escape malformado. */
static bool unescape(bytes *b)
{
    size_t o = 0;
    for (size_t i = 0; i < b->len; i++) {
        char c = b->data[i];
        if (c != '\\') {
            b->data[o++] = c;
            continue;
        }
        if (++i >= b->len) return false;
        switch (b->data[i]) {
        case 'n': b->data[o++] = '\n'; break;
        case 't': b->data[o++] = '\t'; break;
        case 'r': b->data[o++] = '\r'; break;
        case '0': b->data[o++] = '\0'; break;
        case '\\': b->data[o++] = '\\'; break;
        case '"': b->data[o++] = '"'; break;
        case ' ': b->data[o++] = ' '; break;
        case 'x': {
            if (i + 2 >= b->len) return false; /* faltam os dois dígitos */
            int hi = hexval((unsigned char)b->data[i + 1]), lo = hexval((unsigned char)b->data[i + 2]);
            if (hi < 0 || lo < 0) return false;
            b->data[o++] = (char)(hi * 16 + lo);
            i += 2;
            break;
        }
        default:
            return false;
        }
    }
    b->len = o;
    return true;
}

/* Escreve bytes de forma legível: imprimíveis como estão, o resto como \xHH. */
static void print_escaped(FILE *f, const void *data, size_t len)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = p[i];
        if (c == '\\') fputs("\\\\", f);
        else if (c == '\n') fputs("\\n", f);
        else if (c == '\t') fputs("\\t", f);
        else if (c < 0x20 || c == 0x7F) fprintf(f, "\\x%02X", c);
        else fputc(c, f); /* inclui os bytes de UTF-8: "ação" sai como ação */
    }
}

/* ---------------------------------------------------------------------------
 * Comandos
 * ------------------------------------------------------------------------ */

static int print_key(const void *key, size_t klen, void *ctx)
{
    (void)ctx;
    print_escaped(stdout, key, klen);
    putchar('\n');
    return 0;
}

static void print_stats(const kv *db, const char *path)
{
    kv_stats st;
    kv_get_stats(db, &st);
    uint64_t garbage = st.file_bytes - 8 - st.live_bytes;
    printf("arquivo: %s\n", path);
    printf("chaves: %zu\n", st.keys);
    printf("registros no arquivo: %llu (%llu obsoletos ou lápides)\n", (unsigned long long)st.records,
           (unsigned long long)(st.records - st.keys));
    printf("tamanho: %llu bytes, %llu em uso", (unsigned long long)st.file_bytes, (unsigned long long)st.live_bytes);
    if (st.file_bytes > 8) {
        printf(" (%.0f%% recuperável com compactar)", 100.0 * (double)garbage / (double)(st.file_bytes - 8));
    }
    putchar('\n');
    if (st.recovered_bytes) {
        printf("recuperação: %llu bytes de uma escrita interrompida foram descartados ao abrir\n",
               (unsigned long long)st.recovered_bytes);
    }
}

/* Executa um comando. args[0] é o nome. Devolve o código de saída. */
static int run(kv *db, const char *path, bytes *args, int n, bool shell)
{
    const char *cmd = args[0].data;
    kv_status st = KV_OK;
    if (strcmp(cmd, "set") == 0 && n == 3) {
        st = kv_put(db, args[1].data, args[1].len, args[2].data, args[2].len);
        if (st == KV_OK && shell) puts("ok");
    } else if (strcmp(cmd, "get") == 0 && n == 2) {
        void *val;
        size_t len;
        st = kv_get(db, args[1].data, args[1].len, &val, &len);
        if (st == KV_OK) {
            if (shell) print_escaped(stdout, val, len);
            else fwrite(val, 1, len, stdout);
            putchar('\n');
            free(val);
        }
    } else if (strcmp(cmd, "del") == 0 && n == 2) {
        st = kv_delete(db, args[1].data, args[1].len);
        if (st == KV_OK && shell) puts("ok");
    } else if (strcmp(cmd, "list") == 0 && n <= 2) {
        st = kv_keys(db, n == 2 ? args[1].data : "", n == 2 ? args[1].len : 0, print_key, NULL);
    } else if (strcmp(cmd, "compactar") == 0 && n == 1) {
        kv_stats a, b;
        kv_get_stats(db, &a);
        st = kv_compact(db);
        kv_get_stats(db, &b);
        if (st == KV_OK) {
            printf("compactado: %llu bytes para %llu bytes\n", (unsigned long long)a.file_bytes,
                   (unsigned long long)b.file_bytes);
        }
    } else if (strcmp(cmd, "stats") == 0 && n == 1) {
        print_stats(db, path);
    } else {
        fprintf(stderr, "kv: comando inválido: %s (veja kv --help)\n", cmd);
        return 2;
    }
    if (st == KV_NOT_FOUND) {
        if (shell) puts("(não encontrada)");
        return 1;
    }
    if (st != KV_OK) {
        fprintf(stderr, "kv: %s\n", kv_strerror(st));
        return 2;
    }
    return 0;
}

/* Quebra uma linha do terminal em palavras. Aspas agrupam espaços; escapes
 * valem sempre (é o único jeito de digitar um byte zero). */
static int split_line(char *line, bytes *args, bool *bad)
{
    int n = 0;
    char *p = line, *o = line;
    *bad = false;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (n == MAX_ARGS) {
            *bad = true;
            return n;
        }
        char *start = o;
        bool quoted = false;
        while (*p && (quoted || (*p != ' ' && *p != '\t'))) {
            if (*p == '"') {
                quoted = !quoted;
                p++;
            } else if (*p == '\\' && p[1]) {
                *o++ = *p++;
                *o++ = *p++;
            } else {
                *o++ = *p++;
            }
        }
        if (quoted) *bad = true;
        bytes b = { start, (size_t)(o - start) };
        if (*p) p++;
        *o++ = '\0'; /* termina a palavra (o texto só encolhe, então cabe) */
        if (!unescape(&b)) *bad = true;
        b.data[b.len] = '\0';
        args[n++] = b;
    }
    return n;
}

static int shell(kv *db, const char *path)
{
    bool tty = isatty(STDIN_FILENO);
    kv_stats st;
    kv_get_stats(db, &st);
    if (tty) printf("Banco %s: %zu chaves. Digite \"ajuda\" para ver os comandos.\n", path, st.keys);
    char *line = NULL;
    size_t cap = 0, len = 0;
    int last = 0;
    for (;;) {
        if (tty) {
            fputs("kv> ", stdout);
            fflush(stdout);
        }
        /* Lê uma linha de qualquer tamanho. */
        len = 0;
        int c;
        while ((c = getchar()) != EOF && c != '\n') {
            if (len + 2 > cap) {
                size_t nc = cap ? cap * 2 : 256;
                char *grown = realloc(line, nc);
                if (!grown) {
                    free(line);
                    return 2;
                }
                line = grown;
                cap = nc;
            }
            line[len++] = (char)c;
        }
        if (c == EOF && len == 0) break;
        if (!line) continue;
        line[len] = '\0';
        if (len && line[len - 1] == '\r') line[--len] = '\0';
        bytes args[MAX_ARGS];
        bool bad;
        int n = split_line(line, args, &bad);
        if (n == 0) continue;
        if (bad) {
            puts("erro: aspas sem fechar ou escape inválido");
            continue;
        }
        if (strcmp(args[0].data, "sair") == 0 || strcmp(args[0].data, "exit") == 0) break;
        if (strcmp(args[0].data, "ajuda") == 0) {
            puts("set CHAVE VALOR | get CHAVE | del CHAVE | list [PREFIXO] | compactar | stats | sair\n"
                 "Use aspas para espaços (\"minha chave\") e \\xHH para bytes quaisquer.");
            continue;
        }
        last = run(db, path, args, n, true);
        fflush(stdout);
    }
    free(line);
    if (tty) putchar('\n');
    return last == 2 ? 2 : 0;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    kv_options opt = { KV_SYNC_INTERVAL, 100, false };
    bool escapes = false;
    int first = argc;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--versao") == 0) {
            printf("kv %s\n", VERSION);
            return 0;
        } else if ((strcmp(a, "-f") == 0 || strcmp(a, "--arquivo") == 0) && i + 1 < argc) {
            path = argv[++i];
        } else if (strcmp(a, "--fsync") == 0 && i + 1 < argc) {
            const char *m = argv[++i];
            char *end;
            long ms = strtol(m, &end, 10);
            if (strcmp(m, "sempre") == 0) opt.sync = KV_SYNC_ALWAYS;
            else if (strcmp(m, "nunca") == 0) opt.sync = KV_SYNC_NEVER;
            else if (*m && !*end && ms >= 1 && ms <= 3600000) {
                opt.sync = KV_SYNC_INTERVAL;
                opt.interval_ms = (int)ms;
            } else {
                fprintf(stderr, "kv: --fsync aceita sempre, nunca ou um intervalo em ms\n");
                return 2;
            }
        } else if (strcmp(a, "-e") == 0) {
            escapes = true;
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "kv: opção desconhecida \"%s\"\n\n", a);
            usage(stderr);
            return 2;
        } else {
            first = i;
            break;
        }
    }
    if (!path) {
        usage(stderr);
        return 2;
    }

    int n = argc - first;
    if (n > 0 && strcmp(argv[first], "reparar") == 0) {
        uint64_t discarded;
        kv_status st = kv_repair(path, &discarded);
        if (st != KV_OK) {
            fprintf(stderr, "kv: %s\n", kv_strerror(st));
            return 2;
        }
        if (discarded) {
            printf("reparado: %llu bytes de registros estragados foram descartados\n",
                   (unsigned long long)discarded);
        } else {
            puts("nada a reparar: todos os registros conferem");
        }
        return 0;
    }

    /* get, list e stats só leem: trava compartilhada, vários ao mesmo tempo. */
    if (n > 0 && (strcmp(argv[first], "get") == 0 || strcmp(argv[first], "list") == 0 ||
                  strcmp(argv[first], "stats") == 0)) {
        if (access(path, F_OK) == 0) opt.read_only = true;
    }
    kv *db;
    kv_status st = kv_open(path, &opt, &db);
    if (st != KV_OK) {
        fprintf(stderr, "kv: %s: %s\n", path, kv_strerror(st));
        return 2;
    }
    int rc;
    if (n == 0) {
        rc = shell(db, path);
    } else if (n > MAX_ARGS) {
        fprintf(stderr, "kv: argumentos demais\n");
        rc = 2;
    } else {
        bytes args[MAX_ARGS];
        for (int i = 0; i < n; i++) {
            args[i].data = argv[first + i];
            args[i].len = strlen(argv[first + i]);
            if (escapes && i > 0 && !unescape(&args[i])) {
                fprintf(stderr, "kv: escape inválido em \"%s\"\n", argv[first + i]);
                kv_close(db);
                return 2;
            }
        }
        rc = run(db, path, args, n, false);
    }
    if (kv_close(db) != KV_OK && rc == 0) {
        fprintf(stderr, "kv: erro ao gravar no disco\n");
        rc = 2;
    }
    return rc;
}
