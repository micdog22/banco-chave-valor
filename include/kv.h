/*
 * kv.h: banco de dados chave-valor persistente, no estilo do Bitcask.
 *
 * Toda escrita vai para o fim de um único arquivo (um log que só cresce).
 * Na memória fica um índice: para cada chave, onde está o registro mais
 * recente dela no arquivo. Ler é uma consulta ao índice e uma leitura no
 * disco; escrever é um append. Se o processo ou a máquina cair no meio de
 * uma escrita, ao abrir de novo o registro incompleto do fim é detectado
 * pelo CRC e descartado, e tudo o que foi gravado antes continua lá.
 *
 * Chaves e valores são bytes quaisquer (inclusive o byte zero).
 * Um kv* não deve ser usado por duas threads ao mesmo tempo.
 */
#ifndef KV_H
#define KV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KV_MAX_KEY   (1u << 20)   /* 1 MB */
#define KV_MAX_VALUE (1u << 30)   /* 1 GB */

typedef struct kv kv;

typedef enum {
    KV_OK = 0,
    KV_NOT_FOUND,      /* a chave não existe */
    KV_ERR_IO,         /* erro de leitura ou escrita no disco */
    KV_ERR_LOCKED,     /* outro processo está usando o banco */
    KV_ERR_CORRUPT,    /* registro inválido no meio do arquivo (não é só o fim cortado) */
    KV_ERR_FORMAT,     /* o arquivo não é um banco deste formato */
    KV_ERR_TOO_BIG,    /* chave ou valor maior que o limite */
    KV_ERR_READONLY,   /* banco aberto só para leitura */
    KV_ERR_NOMEM       /* memória insuficiente */
} kv_status;

typedef enum {
    KV_SYNC_ALWAYS,    /* fsync depois de cada escrita: nada se perde, mais lento */
    KV_SYNC_INTERVAL,  /* fsync no máximo a cada interval_ms */
    KV_SYNC_NEVER      /* deixa o sistema operacional decidir quando gravar */
} kv_sync;

typedef struct {
    kv_sync sync;
    int     interval_ms;
    bool    read_only;   /* várias leituras ao mesmo tempo; nenhuma escrita */
} kv_options;

typedef struct {
    size_t   keys;              /* chaves vivas */
    uint64_t records;           /* registros no arquivo, inclusive obsoletos e apagados */
    uint64_t file_bytes;        /* tamanho do arquivo */
    uint64_t live_bytes;        /* bytes dos registros que ainda valem */
    uint64_t recovered_bytes;   /* bytes de um fim incompleto descartados ao abrir */
} kv_stats;

/* Abre (ou cria) o banco. opt pode ser NULL: fsync a cada 100 ms. */
kv_status kv_open(const char *path, const kv_options *opt, kv **out);

/* Grava o que estiver pendente e fecha. Sempre libera o kv*. */
kv_status kv_close(kv *db);

kv_status kv_put(kv *db, const void *key, size_t klen, const void *val, size_t vlen);

/* Em KV_OK, *val aponta para uma cópia do valor (libere com free) e *vlen
 * recebe o tamanho. O CRC do registro é conferido a cada leitura. */
kv_status kv_get(kv *db, const void *key, size_t klen, void **val, size_t *vlen);

kv_status kv_delete(kv *db, const void *key, size_t klen);

/* Reescreve o arquivo só com os valores vivos e troca o antigo pelo novo de
 * forma atômica (rename). Recupera o espaço de valores antigos e apagados. */
kv_status kv_compact(kv *db);

/* Força a gravação no disco agora, qualquer que seja a política. */
kv_status kv_sync_now(kv *db);

/* Visita as chaves que começam com prefix, em ordem crescente de bytes.
 * Se visit devolver diferente de zero, a visita para. */
typedef int (*kv_visit)(const void *key, size_t klen, void *ctx);
kv_status kv_keys(kv *db, const void *prefix, size_t plen, kv_visit visit, void *ctx);

void kv_get_stats(const kv *db, kv_stats *st);

/* Para um banco que não abre (KV_ERR_CORRUPT): reescreve o arquivo só com
 * os registros que conferem, pulando os trechos estragados. Se um registro
 * perdido era uma atualização ou remoção, a chave volta ao valor anterior.
 * *discarded recebe quantos bytes foram descartados. */
kv_status kv_repair(const char *path, uint64_t *discarded);

const char *kv_strerror(kv_status s);

/* CRC-32 (o mesmo do zip, do gzip e do PNG), exposto para os testes. */
uint32_t kv_crc32(const void *data, size_t len);

#endif
