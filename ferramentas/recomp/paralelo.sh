#!/bin/sh
# Testes ocultos em paralelo com a conferência ligada: 4 cópias do jogo, cada uma com a sua pasta
# de dados (cópia do save de teste) e um roteiro de botões diferente. No fim junta as funções
# desligadas na lista do bin (bin\recomp_desligar.txt) e mostra o resumo de cada uma.
# uso: sh paralelo.sh <pasta de dados de teste> <pasta de trabalho> [segundos]
set -e
DADOS="$1"; TRAB="$2"; SEG="${3:-90}"
BIN="$(cd "$(dirname "$0")/../../bin" && pwd)"
mkdir -p "$TRAB"
ENTRA="W9000,A400,W4000,A400,W4000,A400,W4000,A400,W6000"
R1="$ENTRA,L1500,W500,R1500,W500,D1200,W500,U1200,W800,l3000,W500,r3000,W500,d2000,W500,u2000"
R2="$ENTRA,X400,W1500,A400,W3000,B400,W1500,R400,W500,A400,W3000,B400,W1500,D400,W500,A400,W3000,B400,W1500,B400"
R3="$ENTRA,l4000,W300,u3000,W300,r6000,W300,d3000,W300,l2000,W300,u5000,W300,r2000"
R4="$ENTRA,X400,W1500,D400,W400,D400,W400,A400,W3000,R400,W800,R400,W800,A400,W2500,B400,W1500,B400,W1500,Y400,W3000,B400"
i=0
for R in "$R1" "$R2" "$R3" "$R4"; do
  i=$((i+1))
  rm -rf "$TRAB/dados$i"; cp -r "$DADOS" "$TRAB/dados$i"
  rm -rf "$TRAB/conf$i"
  ( cd "$BIN" && env PORTE_RECOMP=1 PORTE_RECOMP_MISS=1 PORTE_CONFERIR=20 PORTE_CONFERIR_PASTA="$(cygpath -w "$TRAB/conf$i")" \
      PORTE_HIDDEN=1 PORTE_DATA="$(cygpath -w "$TRAB/dados$i")" PORTE_SECONDS="$SEG" PORTE_PATHAT=3 PORTE_PATH="$R" \
      ./PokemonUltraMoon-Recomp.exe > "$TRAB/teste$i.log" 2>&1; echo "teste $i saiu com $?" ) &
done
wait
for i in 1 2 3 4; do
  echo "== teste $i: $(grep -o 't=[0-9]*s [0-9]* FPS' "$TRAB/teste$i.log" | tail -1) | $(grep 'conferência:' "$TRAB/teste$i.log" | tail -1 | cut -c1-120)"
  grep -c "A emulação parou" "$TRAB/teste$i.log" >/dev/null && echo "   CAIU" || true
  [ -f "$TRAB/conf$i/conferencia.txt" ] && grep "^DIF\|^SVC" "$TRAB/conf$i/conferencia.txt" | sed 's/ | .*instruções | entrada:.*//' | cut -c1-160 || true
done
# junta as funções novas na lista do bin
cat "$BIN/recomp_desligar.txt" "$TRAB"/conf*/recomp_desligar.txt 2>/dev/null | sort -u | grep -v '^$' > "$TRAB/lista_nova.txt"
echo "lista: $(wc -l < "$BIN/recomp_desligar.txt") -> $(wc -l < "$TRAB/lista_nova.txt") funções"
cp "$TRAB/lista_nova.txt" "$BIN/recomp_desligar.txt"
# junta os destinos sem tradução (para gerar.py --extras)
cat "$BIN/recomp_faltam.txt" "$TRAB"/conf*/recomp_faltam.txt 2>/dev/null | sort -u | grep -v '^$' > "$TRAB/faltam_novo.txt" || true
echo "faltam: $(wc -l < "$TRAB/faltam_novo.txt") destinos sem tradução"
cp "$TRAB/faltam_novo.txt" "$BIN/recomp_faltam.txt"
