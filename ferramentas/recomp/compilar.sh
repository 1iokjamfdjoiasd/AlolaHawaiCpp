#!/bin/sh
# uso: sh compilar.sh <pasta gerada>   -> momiji_nativo.dll (só recompila os .o mais velhos que o .c)
set -e
cd "$1"
export PATH=/c/msys64/ucrt64/bin:$PATH
F="-O1 -fwrapv -fno-strict-aliasing -ffp-contract=off -frounding-math -fno-math-errno -msse2 -w"
J=8   # 8 threads com prioridade baixa (Idle): sobram 4 para o resto do PC (pedido do usuário)
: > gcc.log
ls funcoes_*.c runtime_host.c tabela.c | while read c; do
  if [ ! -f "$c.o" ] || [ "$c" -nt "$c.o" ] || [ runtime.h -nt "$c.o" ]; then echo "$c"; fi
done > .recompilar
echo "recompilar: $(wc -l < .recompilar) de $(ls funcoes_*.c runtime_host.c tabela.c | wc -l)"
nice -n 19 xargs -P "$J" -I{} sh -c "gcc $F -c {} -o {}.o 2>> gcc.log || echo ERRO {}" < .recompilar
if grep -q "error" gcc.log; then grep -m5 error gcc.log; exit 1; fi
# os .o de arquivos que não existem mais (menos arquivos que antes) ficam de fora
O=$(ls funcoes_*.c | sed 's/$/.o/')
gcc -shared -s -o momiji_nativo.dll $O tabela.c.o runtime_host.c.o
ls -la momiji_nativo.dll
