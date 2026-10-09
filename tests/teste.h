/*
 * teste.h: um mini framework de testes, sem dependências.
 * CHECK conta uma verificação e, se falhar, mostra arquivo, linha e expressão.
 */
#ifndef TESTE_H
#define TESTE_H

#include <stdio.h>
#include <string.h>

static int teste_total;
static int teste_falhas;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        teste_total++;                                                              \
        if (!(cond)) {                                                              \
            teste_falhas++;                                                         \
            fprintf(stderr, "%s:%d: falhou: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                           \
    } while (0)

#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        long long check_a_ = (long long)(a), check_b_ = (long long)(b);             \
        teste_total++;                                                              \
        if (check_a_ != check_b_) {                                                 \
            teste_falhas++;                                                         \
            fprintf(stderr, "%s:%d: falhou: %s == %s (0x%llX != 0x%llX)\n",         \
                    __FILE__, __LINE__, #a, #b, check_a_, check_b_);                \
        }                                                                           \
    } while (0)

#define CHECK_STR(a, b)                                                             \
    do {                                                                            \
        const char *check_a_ = (a), *check_b_ = (b);                                \
        teste_total++;                                                              \
        if (strcmp(check_a_, check_b_) != 0) {                                      \
            teste_falhas++;                                                         \
            fprintf(stderr, "%s:%d: falhou: \"%s\" == \"%s\"\n",                    \
                    __FILE__, __LINE__, check_a_, check_b_);                        \
        }                                                                           \
    } while (0)

/* Imprime o resumo e devolve o código de saída do programa de teste. */
static int teste_resumo(const char *nome)
{
    printf("%s: %d verificações, %d falhas\n", nome, teste_total, teste_falhas);
    return teste_falhas ? 1 : 0;
}

#endif
