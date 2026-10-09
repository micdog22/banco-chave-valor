# Banco Chave-Valor

Um banco de dados chave-valor persistente escrito do zero em C, no estilo do Bitcask (o motor de armazenamento do Riak): um único arquivo que só cresce no fim, um índice na memória e recuperação automática depois de quedas. Vem como biblioteca (`kv.h`) e como o programa `kv`, com comandos avulsos e um terminal interativo.

O objetivo é mostrar, com código pequeno e testes pesados, como um banco consegue não perder dados quando a energia acaba no meio de uma escrita.

## Recursos

- **Log que só cresce**: cada escrita é um registro `[CRC-32][timestamp][tamanho da chave][tamanho do valor][flags][chave][valor]` no fim do arquivo; apagar é escrever uma "lápide".
- **Índice na memória**: tabela hash com endereçamento aberto apontando para a posição de cada chave no arquivo. Ler é uma consulta e uma leitura no disco.
- **Recuperação**: ao abrir, o log é relido e conferido pelo CRC. Um registro incompleto no fim (escrita interrompida) é descartado; tudo o que foi gravado antes fica.
- **Estrago no meio do arquivo** não é confundido com queda: o banco se recusa a abrir em vez de jogar fora dados bons, e o comando `reparar` salva todos os registros que conferem.
- **Política de fsync**: `sempre`, a cada N ms ou `nunca`. No macOS, usa `F_FULLFSYNC`, que também esvazia o cache do disco.
- **Compactação**: reescreve o arquivo só com os valores vivos e troca o antigo pelo novo de forma atômica.
- **Trava entre processos** com `flock`: um escritor ou vários leitores.
- Chaves e valores binários (inclusive com o byte zero). Sem dependências além da biblioteca padrão e de POSIX. Roda em Linux e macOS; no Windows, use o WSL.

## Como usar

```
$ make
$ ./build/kv -f dados.kv set usuario:1 "Ana Souza"
$ ./build/kv -f dados.kv set usuario:2 "Bruno Lima"
$ ./build/kv -f dados.kv set config:tema escuro
$ ./build/kv -f dados.kv get usuario:1
Ana Souza
$ ./build/kv -f dados.kv list usuario:
usuario:1
usuario:2
$ ./build/kv -f dados.kv set usuario:1 "Ana Souza Lima"
$ ./build/kv -f dados.kv del usuario:2
$ ./build/kv -f dados.kv stats
arquivo: dados.kv
chaves: 2
registros no arquivo: 5 (3 obsoletos ou lápides)
tamanho: 199 bytes, 82 em uso (57% recuperável com compactar)
$ ./build/kv -f dados.kv compactar
compactado: 199 bytes para 90 bytes
```

Sem comando, `kv` abre um terminal interativo (aspas agrupam espaços e `\xHH` digita qualquer byte):

```
$ ./build/kv -f dados.kv
Banco dados.kv: 2 chaves. Digite "ajuda" para ver os comandos.
kv> set "chave com espaço" "valor\x00binário"
ok
kv> get "chave com espaço"
valor\x00binário
kv> sair
```

Opções e comandos:

```
uso: kv -f ARQUIVO [opções] [comando]

Comandos:
  set CHAVE VALOR   grava (substitui o valor, se a chave já existir)
  get CHAVE         mostra o valor
  del CHAVE         apaga a chave
  list [PREFIXO]    lista as chaves em ordem
  compactar         reescreve o arquivo só com o que ainda vale
  stats             mostra números do arquivo
  reparar           salva os registros bons de um arquivo danificado

Opções:
  -f, --arquivo ARQ  arquivo do banco (criado se não existir)
  --fsync MODO       sempre, nunca ou um intervalo em ms (padrão: 100)
  -e                 interpreta \xHH, \n, \t e \\ nas chaves e valores
```

O código de saída é 0 quando deu certo, 1 quando a chave não existe e 2 em caso de erro. `get`, `list` e `stats` abrem o banco só para leitura, então vários podem rodar juntos.

### Depois de uma queda

Um pedaço de registro no fim do arquivo (o que sobra quando a energia acaba no meio de uma escrita) é detectado e descartado sozinho:

```
$ ./build/kv -f cortado.kv stats
...
recuperação: 7 bytes de uma escrita interrompida foram descartados ao abrir
```

Já um byte trocado no meio do arquivo é outro problema, e o banco não decide sozinho:

```
$ ./build/kv -f dados.kv get config:tema
kv: dados.kv: o arquivo está corrompido no meio (use o comando reparar)
$ ./build/kv -f dados.kv reparar
reparado: 38 bytes de registros estragados foram descartados
$ ./build/kv -f dados.kv list
usuario:1
```

### Como biblioteca

```c
#include <stdio.h>
#include <stdlib.h>
#include "kv.h"

int main(void)
{
    kv *db;
    kv_options opt = { KV_SYNC_ALWAYS, 0, false };   /* fsync a cada escrita */
    if (kv_open("dados.kv", &opt, &db) != KV_OK) return 1;

    kv_put(db, "visitas", 7, "42", 2);

    void *valor;
    size_t tamanho;
    if (kv_get(db, "visitas", 7, &valor, &tamanho) == KV_OK) {
        printf("%.*s\n", (int)tamanho, (char *)valor);
        free(valor);
    }
    kv_close(db);
    return 0;
}
```

Compile com `-Iinclude` e ligue com `build/libkv.a`.

## Como compilar

Com make (a biblioteca `libkv.a` e o programa `kv` ficam em `build/`):

```sh
make
make test
make clean
```

Com CMake:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Precisa de um compilador C11 (clang ou gcc) e de um sistema POSIX (`pwrite`, `fsync`, `flock`).

## Testes

```sh
make test
```

- `tests/test_kv.c`:
  - CRC-32 com os valores de referência (`"123456789"` dá `CBF43926`);
  - gravar, ler, sobrescrever, apagar, chave e valor vazios, chaves binárias com zeros e um valor de 3 MB, tudo conferido também depois de reabrir;
  - **quedas**: um log de 12 operações é cortado em cada uma das suas 341 posições possíveis; em cada corte, o banco abre, tudo o que foi escrito por inteiro antes do corte está lá, o registro cortado some e o arquivo é aparado no lugar certo;
  - lixo no fim do arquivo, em 50 tamanhos diferentes;
  - um byte trocado no meio de cada registro: o banco recusa abrir, `kv_repair` descarta exatamente aquele registro e o resto confere;
  - leitura que detecta um bit trocado depois de o banco abrir;
  - compactação (o arquivo fica com exatamente os bytes vivos) e sobra de compactação interrompida;
  - travas: segundo escritor recusado, vários leitores juntos;
  - **100 mil operações aleatórias** (semente fixa) contra um modelo na memória, com reaberturas e compactações no meio.
- `tests/test_queda.c`: queda de verdade. Um processo filho grava sem parar e o pai o mata com SIGKILL em momentos diferentes; depois, cada gravação que o filho confirmou tem de estar no banco.
- `tests/cli.sh`: os comandos do `kv`, o terminal interativo, códigos de saída, a trava entre dois processos e o formato do arquivo conferido pelo `zlib.crc32` do Python.

Com AddressSanitizer e UndefinedBehaviorSanitizer:

```sh
make clean
make test CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
          LDFLAGS="-fsanitize=address,undefined"
```

## Como funciona

### Armazenamento estruturado em log

Um banco tradicional atualiza os dados no lugar: para mudar um valor, sobrescreve a página onde ele está. Um banco **estruturado em log** nunca sobrescreve nada: toda escrita vai para o fim de um arquivo. Para saber o valor atual de uma chave, basta saber onde está o registro mais recente dela, e é isso que o índice na memória guarda:

```
arquivo:  KVLOG001 | a=1 | b=2 | a=3 | lápide(b) | c=4 |
                                   ^                  ^
índice:   a -> posição do "a=3"    c -> posição do "c=4"     (b não existe mais)
```

Escrever é um único append (o tipo mais rápido de escrita que um disco faz). Ler é uma consulta à tabela hash e uma leitura na posição certa. Ao abrir, o índice é reconstruído lendo o log do começo ao fim.

### Formato do registro

Depois da assinatura `KVLOG001` (8 bytes), cada registro tem um cabeçalho de 21 bytes, com inteiros em little-endian:

| Bytes | Campo |
|---|---|
| 4 | CRC-32 de tudo o que vem depois dele no registro |
| 8 | timestamp (microssegundos desde 1970) |
| 4 | tamanho da chave |
| 4 | tamanho do valor |
| 1 | flags (bit 0: lápide) |
| ... | chave, depois valor |

Os tamanhos ficam antes dos dados, então a chave e o valor podem ter qualquer byte, inclusive zero.

### Por que o append sobrevive a quedas

Se a energia acaba no meio de uma escrita, o estrago fica confinado ao fim do arquivo: o último registro pode ter ficado pela metade, mas nenhum registro anterior foi tocado, porque nada é sobrescrito. Na hora de abrir, basta reconhecer o pedaço incompleto e cortá-lo. Esse é o grande trunfo do append-only: a recuperação é simples e não precisa de um diário separado.

O teste de queda deste projeto pega isso acontecendo de verdade: como a escrita do cabeçalho, da chave e do valor são três chamadas, o SIGKILL às vezes cai entre elas, e o banco descarta os 20 e poucos bytes do registro cortado ao abrir.

### CRC-32

Como reconhecer o registro incompleto? Pelo tamanho, se ele passa do fim do arquivo. Mas uma escrita interrompida também pode deixar lixo com o tamanho certo. Para isso existe o **CRC-32**: um código de 32 bits calculado sobre o conteúdo, que muda quando qualquer bit muda.

Por dentro, o CRC trata os bytes como um polinômio enorme com coeficientes 0 ou 1 e calcula o resto da divisão dele por um polinômio fixo (o 0x04C11DB7, o mesmo do zip, do gzip, do PNG e da Ethernet). Na prática, isso vira uma tabela de 256 entradas e uma operação por byte:

```c
crc = TABELA[(crc ^ byte) & 0xFF] ^ (crc >> 8);
```

O CRC-32 detecta qualquer erro em até 32 bits seguidos e todo erro de um só bit. A chance de um lixo qualquer passar por um registro válido é de uma em 4 bilhões. Não é uma assinatura criptográfica: protege contra acidentes, não contra quem quer forjar dados.

### Queda ou estrago?

Um registro que não confere pode ser duas coisas bem diferentes:

- **a última escrita interrompida**: só pode ser o último registro do arquivo;
- **um estrago no meio** (setor ruim, bit trocado, alguém editou o arquivo): há registros bons depois dele.

Cortar o arquivo no primeiro registro ruim, sem pensar, transformaria um byte estragado no começo em perda de tudo o que vem depois. Por isso, ao achar um registro ruim, o banco procura, byte a byte, se existe algum registro válido mais adiante. Se não existe, era uma queda: corta e segue. Se existe, recusa abrir com `KV_ERR_CORRUPT`, e o `kv_repair` reescreve o arquivo com todos os registros que conferem, pulando os trechos estragados.

### fsync: quando o dado está mesmo no disco

O `write()` volta assim que os dados chegam ao cache do sistema operacional. Se o processo morrer, eles ainda chegam ao disco; se a máquina desligar, se perdem. `fsync()` espera o disco confirmar a gravação, e custa caro (milissegundos). Daí as políticas:

| Política | Garantia | Custo |
|---|---|---|
| `sempre` | toda escrita confirmada sobrevive a queda de energia | um fsync por escrita |
| a cada N ms | perde no máximo os últimos N ms numa queda de energia | um fsync de vez em quando |
| `nunca` | sobrevive a queda do processo, não da máquina | nenhum |

No macOS, o `fsync()` entrega os dados ao disco, mas o próprio disco pode segurá-los num cache volátil. Por isso o banco usa `fcntl(F_FULLFSYNC)` quando ele existe.

### Compactação

Valores sobrescritos e chaves apagadas continuam ocupando espaço no log. A compactação copia só os registros vivos para um arquivo novo e troca os dois:

1. escreve `dados.kv.compactando` com a assinatura e os registros vivos (na ordem em que estavam, para a leitura ser sequencial), conferindo o CRC de cada um;
2. faz fsync do arquivo novo;
3. `rename()` por cima do antigo: no POSIX, a troca é **atômica**, então quem abrir o caminho vê o arquivo antigo inteiro ou o novo inteiro, nunca uma mistura;
4. faz fsync da pasta, para o rename também ficar gravado.

Se a queda acontecer antes do rename, o arquivo antigo continua intacto e a sobra `.compactando` é apagada na próxima abertura.

### Índice: tabela hash com endereçamento aberto

O índice é uma tabela hash com sondagem linear: a chave vai para a posição `hash & (capacidade - 1)` e, se estiver ocupada, para a seguinte. Com no máximo 70% de ocupação, a busca quase sempre termina em uma ou duas posições, e os dados ficam contíguos na memória.

Apagar é a parte delicada: deixar a posição vazia quebraria a sequência de sondagem de quem veio depois. Em vez de marcar uma "lápide" na tabela, o banco usa o algoritmo de Knuth: depois de esvaziar uma posição, as entradas seguintes do mesmo aglomerado voltam uma casa quando a posição ideal delas permite.

### Trava entre processos

Dois processos escrevendo no mesmo log ao mesmo tempo misturariam os registros. Ao abrir, o banco pede `flock(LOCK_EX | LOCK_NB)`: se outro processo estiver com o banco aberto, a resposta é um erro na hora ("o banco está em uso por outro processo"), em vez de esperar ou corromper. Quem só lê pede `LOCK_SH`, a trava compartilhada, e vários leitores convivem.

`flock` vem do BSD e não faz parte do POSIX estrito, mas existe no Linux e no macOS. Ele não funciona de forma confiável em sistemas de arquivos de rede como o NFS, e a trava é por arquivo aberto: um mesmo `kv*` não deve ser usado por duas threads ao mesmo tempo.

### Comparação com árvores B

| | Log com índice na memória (este projeto) | Árvore B (SQLite, InnoDB) |
|---|---|---|
| Escrita | um append, sequencial | atualiza páginas no lugar, escrita aleatória |
| Leitura de uma chave | uma consulta na memória e uma leitura no disco | alguns níveis de páginas |
| Memória | todas as chaves precisam caber na RAM | só as páginas em uso |
| Busca por intervalo | precisa ordenar as chaves (o `list` faz isso) | natural, as chaves já estão em ordem |
| Recuperação | relê o log e corta o fim | precisa de um diário (WAL) à parte |
| Espaço | cresce até compactar | estável |

Logs como este brilham com muitas escritas e chaves que cabem na memória. Árvores B ganham em buscas por intervalo e em bancos maiores que a RAM. Bancos como o LevelDB e o RocksDB (árvores LSM) combinam as duas ideias: log para escrever rápido, arquivos ordenados para buscar por intervalo.

## Contribuindo

Issues e pull requests são bem-vindos.

## Licença

MIT. Veja [LICENSE](LICENSE).
