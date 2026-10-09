#!/bin/sh
# Testes do programa kv: comandos, terminal interativo, códigos de saída,
# trava entre processos e o formato do arquivo conferido pelo zlib do Python.
# Uso: sh tests/cli.sh <pasta do build>

B=${1:-build}
KV="$B/kv"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/kvcli.XXXXXX") || exit 1
PID=""
trap '[ -n "$PID" ] && kill "$PID" 2>/dev/null; rm -rf "$TMP"' EXIT INT TERM
DB="$TMP/dados.kv"

total=0
falhas=0
ok() { total=$((total + 1)); }
falha() { total=$((total + 1)); falhas=$((falhas + 1)); echo "FALHOU: $*"; }
confere() { # confere <descrição> <esperado> <obtido>
    if [ "$2" = "$3" ]; then ok; else falha "$1: esperava [$2], veio [$3]"; fi
}
guarda() { cat "$TMP/erro.txt" >> "$TMP/todos-os-erros.txt"; } # para procurar relatórios de sanitizer no fim

"$KV" -f "$DB" set nome "Maria da Silva"
"$KV" -f "$DB" set cidade Limeira
"$KV" -f "$DB" set "chave com espaço" "valor também"
confere "get" "Maria da Silva" "$("$KV" -f "$DB" get nome)"
confere "get com espaço" "valor também" "$("$KV" -f "$DB" get "chave com espaço")"
"$KV" -f "$DB" set nome Ana
confere "sobrescrever" "Ana" "$("$KV" -f "$DB" get nome)"
"$KV" -f "$DB" del cidade
confere "del" 0 $?
"$KV" -f "$DB" get cidade > /dev/null
confere "get de chave apagada sai com 1" 1 $?
"$KV" -f "$DB" del cidade
confere "del de chave inexistente sai com 1" 1 $?
confere "list" "chave com espaço
nome" "$("$KV" -f "$DB" list)"
confere "list com prefixo" "nome" "$("$KV" -f "$DB" list no)"
if "$KV" -f "$DB" stats | grep -q "^chaves: 2$"; then ok; else falha "stats"; fi

# Bytes quaisquer com -e
"$KV" -f "$DB" -e set 'bin\x00chave' 'a\x00b'
confere "valor com byte zero" "610062" "$("$KV" -f "$DB" -e get 'bin\x00chave' | head -c 3 | od -An -tx1 | tr -d ' \n')"
if "$KV" -f "$DB" list | grep -q '^bin\\x00chave$'; then ok; else falha "list escapa bytes"; fi

# Terminal interativo lendo da entrada padrão
saida=$(printf 'set "x y" "1 2 3"\nget "x y"\nget nada\ndel "x y"\nlist x\nsair\n' | "$KV" -f "$DB")
confere "terminal" "ok
1 2 3
(não encontrada)
ok" "$saida"

# compactar recupera espaço e mantém tudo
for i in 1 2 3 4 5 6 7 8 9 10; do "$KV" -f "$DB" --fsync nunca set contador "$i"; done
antes=$(wc -c < "$DB")
"$KV" -f "$DB" compactar > /dev/null
depois=$(wc -c < "$DB")
if [ "$depois" -lt "$antes" ]; then ok; else falha "compactar não diminuiu o arquivo ($antes -> $depois)"; fi
confere "valor depois de compactar" "10" "$("$KV" -f "$DB" get contador)"
confere "nome depois de compactar" "Ana" "$("$KV" -f "$DB" get nome)"

# Formato do arquivo: assinatura, cabeçalho de 21 bytes e CRC-32 igual ao do zlib
if command -v python3 > /dev/null 2>&1; then
    python3 - "$DB" <<'PY' > "$TMP/formato.txt" 2>&1
import struct, sys, zlib
data = open(sys.argv[1], "rb").read()
assert data[:8] == b"KVLOG001", "assinatura"
off, n = 8, 0
while off < len(data):
    crc, ts, klen, vlen, flags = struct.unpack_from("<IQIIB", data, off)
    end = off + 21 + klen + vlen
    assert end <= len(data), "registro além do fim"
    assert zlib.crc32(data[off + 4:end]) == crc, "CRC diferente do zlib no registro %d" % n
    assert flags in (0, 1), "flags"
    off, n = end, n + 1
print("registros conferidos:", n)
PY
    if [ $? -eq 0 ]; then ok; else falha "formato conferido pelo Python"; cat "$TMP/formato.txt"; fi
fi

# Queda simulada: um pedaço de registro no fim é descartado ao abrir
cp "$DB" "$TMP/cortado.kv"
printf 'restos de uma escrita interrompida' >> "$TMP/cortado.kv"
confere "dado intacto depois do corte" "Ana" "$("$KV" -f "$TMP/cortado.kv" set y z && "$KV" -f "$TMP/cortado.kv" get nome)"
if [ "$(wc -c < "$TMP/cortado.kv")" -lt "$(($(wc -c < "$DB") + 34))" ]; then ok; else falha "fim cortado não foi aparado"; fi

# Estrago no meio: o banco não abre, e reparar salva o resto
cp "$DB" "$TMP/estragado.kv"
printf 'X' | dd of="$TMP/estragado.kv" bs=1 seek=12 conv=notrunc 2>/dev/null
"$KV" -f "$TMP/estragado.kv" get nome > /dev/null 2> "$TMP/erro.txt"
confere "estrago no meio sai com 2" 2 $?
guarda
if grep -q "corrompido" "$TMP/erro.txt"; then ok; else falha "mensagem de corrupção"; fi
if "$KV" -f "$TMP/estragado.kv" reparar | grep -q "reparado"; then ok; else falha "reparar"; fi
confere "depois de reparar" "10" "$("$KV" -f "$TMP/estragado.kv" get contador)"

# Dois escritores: o segundo é recusado na hora
mkfifo "$TMP/fifo"
"$KV" -f "$DB" < "$TMP/fifo" > "$TMP/primeiro.txt" &
PID=$!
exec 3> "$TMP/fifo"
# Espera o primeiro confirmar uma escrita: aí a trava com certeza é dele.
echo "set pronto sim" >&3
i=0
while ! grep -q '^ok$' "$TMP/primeiro.txt" 2>/dev/null && [ $i -lt 200 ]; do
    sleep 0.05
    i=$((i + 1))
done
"$KV" -f "$DB" set outra coisa 2> "$TMP/erro.txt"
confere "segundo escritor sai com 2" 2 $?
guarda
if grep -q "em uso" "$TMP/erro.txt"; then ok; else falha "mensagem de banco em uso"; fi
echo "sair" >&3
exec 3>&-
wait "$PID"
PID=""
"$KV" -f "$DB" set outra coisa
confere "depois que o primeiro fecha" "coisa" "$("$KV" -f "$DB" get outra)"

# Uso errado
"$KV" set a b > /dev/null 2>&1
confere "sem -f" 2 $?
"$KV" -f "$DB" --fsync talvez set a b > /dev/null 2>&1
confere "--fsync inválido" 2 $?
printf 'isto nao e um banco' > "$TMP/texto.kv"
"$KV" -f "$TMP/texto.kv" get a > /dev/null 2>&1
confere "arquivo que não é banco" 2 $?

# Quando compilado com sanitizers, nenhum relatório pode ter aparecido.
touch "$TMP/todos-os-erros.txt"
if grep -Eq "runtime error|AddressSanitizer|LeakSanitizer" "$TMP/todos-os-erros.txt"; then
    falha "relatório de sanitizer"
    cat "$TMP/todos-os-erros.txt"
else
    ok
fi

echo "cli.sh: $total verificações, $falhas falhas"
[ "$falhas" -eq 0 ]
