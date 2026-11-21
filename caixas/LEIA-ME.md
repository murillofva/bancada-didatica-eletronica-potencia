# Caixas impressas em 3D

A bancada fica em três caixas impressas em 3D, lado a lado e presas umas às outras por parafusos M3 nas laterais. As medidas são externas (largura × profundidade × altura):

```
   A — CC-CC (PETG)   |   C — Controle (PLA)   |   B — Retificadores (PETG)
   125 × 154 × 70 mm  |   125 × 154 × 70 mm    |   163 × 154 × 70 mm
                  conjunto: 413 × 154 × 70 mm
```

- A caixa de controle fica no meio. O cabo J10 (8 vias) passa entre A e C, e o J20 (9 vias), entre C e B, por entalhes alinhados nas laterais vizinhas. O lado da rede fica na ponta direita, longe do controle.
- Cada caixa tem uma **base** (fundo e paredes, 67 mm de altura) e uma **tampa**, que é o painel, com os furos e os textos gravados. A tampa tem 3 mm de painel (os 9 mm da tabela abaixo incluem os ressaltos das portinholas, por baixo) e é presa por parafusos M3 nos pilares dos cantos.
- As placas ficam deitadas no fundo, presas em torres impressas de 10 mm.

## 1. Arquivos desta pasta

| Arquivo | Peça | Tamanho na mesa (mm) | Material |
|---|---|---|---|
| `caixa_A_base.stl` | base da caixa A | 125 × 154 × 67 | PETG |
| `caixa_A_tampa.stl` | tampa da caixa A | 125 × 154 × 9 | PETG |
| `caixa_A_portinhola_esq.stl` | portinhola dos jumpers JP4 e JP7 | 22 × 34,5 × 2,4 | PETG |
| `caixa_A_portinhola_dir.stl` | portinhola dos jumpers JP5 e JP6 (dá acesso também ao J2) | 36 × 57 × 2,4 | PETG |
| `caixa_B_base.stl` | base da caixa B | 163 × 154 × 67 | PETG |
| `caixa_B_tampa.stl` | tampa da caixa B | 163 × 154 × 9 | PETG |
| `caixa_B_portinhola_jp.stl` | portinhola dos jumpers JP1, JP2 e JP3 | 45 × 33 × 2,4 | PETG |
| `caixa_B_presilha_cabo.stl` | presilha do cabo de força (alívio de tração) | 36 × 10 × 5 | PETG |
| `caixa_C_base.stl` | base da caixa C | 125 × 154 × 67 | PLA |
| `caixa_C_tampa.stl` | tampa da caixa C | 125 × 154 × 9 | PLA |
| `caixa_C_portinhola_jp8.stl` | portinhola do jumper JP8 (5 V da S3) | 21 × 31 × 2,4 | PLA |
| `gabarito_furos.stl` | placa de teste dos furos de painel | 130 × 72 × 2 | PETG e PLA |

## 2. O que cada caixa leva

**Caixa A — conversores CC-CC (placa A, 95 × 80 mm):**

- a placa fica sem girar, encostada no fundo da caixa;
- na tampa: bornes ENTRADA 12 V (J1) e CARGA (J2), na faixa da frente, longe dos dissipadores; ventilação sobre o Boost e o Buck; duas portinholas de jumpers;
- entalhe do cabo J10 na lateral direita (lado da caixa C). As cargas de 10 W ficam **fora** da caixa, ligadas nos bornes CARGA.

**Caixa C — controle (placa C, 100 × 60 mm, com a ESP32-S3):**

- a placa fica girada e encostada na frente, com a ponta da S3 que tem as USB-C voltada para uma janela de 50 × 16 mm na parede da frente;
- na tampa: bornes ENTRADA 12 V (J40); janela do OLED com os LEDs SAÍDA e SINC.; furos de 3 potenciômetros (D, α, PÁGINA) e de 3 botões (MODO, LIGA, ZERO); portinhola do JP8;
- o OLED é preso por baixo do painel, com 4 parafusos M2 que entram pela frente, e o vidro fica num rebaixo de 1,5 mm. Os entalhes dos cabos J10 e J20 ficam nas duas laterais;
- o JP8 fica aberto enquanto a USB estiver ligada ao computador. Os potenciômetros e os botões só são lidos com a chave `PAINEL_MANUAL` em 1 no firmware (veja `firmware/LEIA-ME.md`).

**Caixa B — retificadores (placa B, 100 × 75 mm):**

- a placa fica girada: o J20 encosta na lateral esquerda (lado da caixa C), e o J21 (secundário), na parede da rede;
- uma parede interna inteira, do fundo à tampa, fecha o lado da rede. À direita dela ficam o prensa-cabo, o fusível F1 (primário), a chave gangorra, o botão de emergência e o transformador de 15 + 15 V, 36 VA, preso por rasgos no fundo;
- os fios do secundário passam por um furo de 8 mm nessa parede, até o J21. É o único furo da parede;
- os rasgos de ventilação têm 2 mm no lado da rede, para não passar dedo nem ferramenta, e 3 mm no lado de baixa tensão;
- na tampa: bornes CARGA (Vo) (J22) e a janela dos jumpers, com portinhola; no lado da rede, a chave gangorra, o porta-fusível e o botão de emergência. A tampa tem 6 parafusos, 2 deles nos pilares da parede interna;
- a carga de 47 Ω / 50 W fica **fora** da caixa, numa chapa de metal.

**Jumpers:** cada portinhola é presa por 2 parafusos M3 e abre sem tirar a tampa. Na caixa B, a tampa do lado da rede continua fechada enquanto se mexe nos jumpers.

## 3. Como imprimir

- **Imprima primeiro o gabarito de furos** (seção 4) e só depois as tampas.
- **Posição:** cada STL já está na posição certa. As bases ficam de pé, e as tampas e as portinholas, com a face do painel virada para a mesa. **Nenhuma peça precisa de suporte:** os pilares dos parafusos da tampa têm uma rampa de 45° por baixo.
- **Mesa:** todas as peças cabem numa mesa de 260 mm × 260 mm. A maior é a base da caixa B (163 mm × 154 mm).
- **Janela da USB (caixa C):** faz uma ponte de 50 mm. Use o ventilador a 100 % nas pontes.
- **Textos dos painéis:** ficam gravados na face que encosta na mesa e saem legíveis sem retoque. Em mesa texturizada, o painel fica fosco.
- **Compensação de furos (XY) do fatiador:** deixe desligada, como no gabarito, para os furos saírem do tamanho testado.

| Peças | Material | Configuração |
|---|---|---|
| Bases A e B | PETG | camada de 0,2 mm; 3 paredes; topo e fundo com 4 camadas; preenchimento de 20 a 25 % (giroide); bico de 235 a 245 °C; mesa de 75 a 80 °C; aba (brim) de 5 mm, para os cantos não levantarem |
| Tampas A e B, portinholas de A e B e presilha | PETG | camada de 0,2 mm; 3 paredes; preenchimento de 25 %; primeira camada devagar (textos) |
| Base C | PLA | camada de 0,2 mm; 3 paredes; preenchimento de 20 %; bico de 205 a 215 °C; mesa de 60 °C |
| Tampa C e portinhola do JP8 | PLA | camada de 0,2 mm; 3 paredes; preenchimento de 25 %; primeira camada devagar |

- **PETG em mesa de PEI lisa:** passe cola bastão antes, porque o PETG gruda demais e pode arrancar a película.
- **Sem PETG:** a caixa A pode sair em PLA, porque a placa fica longe das paredes e a ventilação é boa. A caixa B, com o transformador, fica em PETG.
- **Filamento:** no máximo cerca de 0,62 kg de PETG e 0,25 kg de PLA (peças maciças); com 20 a 25 % de preenchimento, gasta menos.

## 4. Gabarito de furos

O gabarito tem 2 mm de espessura, que é o painel que a trava da chave gangorra costuma aceitar. Na tampa, o painel tem 3 mm e é afinado para 2 mm só em volta da chave. Os números ao lado de cada furo estão gravados na face de cima, em mm.

Imprima uma placa em PETG e outra em PLA (cerca de 20 min cada), porque os furos mudam um pouco de um filamento para o outro. Deitada, com os números para cima, sem suporte, camada de 0,2 mm, 3 paredes, 20 % de preenchimento e sem compensação de furos. Para cada peça, veja qual furo ficou melhor: a peça entra sem forçar e sem folga.

| Posição na placa | Furos (mm) | Para testar | Usado nas tampas (mm) |
|---|---|---|---|
| Embaixo, à esquerda | 5,0 · 5,2 · 5,4 | LED de 5 mm | 5,2 |
| Embaixo, no meio | 7,0 · 7,3 · 7,6 (ao lado do 7,3, o pino anti-giro de 2,6 mm, a 7,8 mm do centro) | potenciômetro e push-button PBS-110 | 7,3 |
| Embaixo, à direita | 6,4 · 8,2 · 8,5 | borne banana | 8,2 |
| No meio, à esquerda | 12,0 · 12,3 · 12,6 | porta-fusível 5 × 20 de painel | 12,3 |
| No meio, à direita | 22,0 · 22,4 | botão de emergência | 22,4 |
| Em cima | 19,0 × 12,8 · 19,3 × 13,1 · 19,6 × 13,4 | chave gangorra KCD1-104N | 19,3 × 13,1 |
| Em cima, à direita | 3,2 · 3,4 · 2,6 | parafuso M3: passagem folgada, passagem justa e furo para rosquear no plástico | 3,4 e 2,6 |

**Ajuste dos furos de painel:** se o furo que serviu melhor for o da última coluna, imprima as tampas como estão. Se não for, ajuste o furo correspondente nas tampas antes de montar as peças.

## 5. Parafusos e peças extras

| Para quê | Quantidade | Peça |
|---|---|---|
| Tampas (A: 4, C: 4, B: 6) | 14 | parafuso M3 × 12, cabeça panela |
| Placas nas torres (4 por placa) | 12 | parafuso M3 × 8, rosqueado direto no furo de 2,6 mm |
| Portinholas (A: 4, B: 2, C: 2) | 8 | parafuso M3 × 10 (cabeça recartilhada ou borboleta abre sem chave) |
| União das caixas (A–C: 2, C–B: 2) | 4 | parafuso M3 × 10, porca e arruela |
| Transformador, pelos rasgos do fundo | 2 | parafuso M3 × 16, porca e arruela (cabeça por baixo do fundo) |
| Presilha do cabo de força | 2 | parafuso M3 × 10 |
| OLED | 4 | parafuso M2 × 8 e porca M2 (ou fita dupla face) |
| Pés | 12 | pé de borracha adesivo de 3 mm ou mais (4 por caixa; na B, longe das cabeças dos parafusos do transformador) |
| Cabo de força | 1 | prensa-cabo PG9 (cabo de 4 a 8 mm) |
| Fios do secundário | 1 | passa-fio de borracha de 8 mm (ou termorretrátil em volta dos fios) |

O plano de GND chega a 3,3 mm do centro dos furos de fixação nas placas A e C, e a 2,8 mm na placa B: uma porca ou arruela de metal encosta no GND. O contato do parafuso só com o GND não causa problema; para isolar, use arruela de náilon.

## 6. Ordem de montagem

1. **Gabarito:** imprima e confira os furos (seção 4) antes de imprimir as tampas.
2. **Bases:** cole os pés de borracha. Na caixa B, prenda o transformador pelos rasgos do fundo, com os parafusos de baixo para cima.
3. **Placas:** aparafuse cada placa nas torres com M3 × 8. Coloque os quatro parafusos soltos e aperte no fim.
4. **Lado da rede (caixa B), com tudo fora da tomada:**
   1. passe o cabo pelo prensa-cabo e prenda-o com a presilha nas duas torres;
   2. monte o porta-fusível, a chave gangorra e o botão de emergência na tampa;
   3. ligue na ordem cabo → fusível F1 (rápido de 1 A) → chave → emergência → primário, com o terra na carcaça do transformador;
   4. passe os fios do secundário pelo furo da parede interna, com o passa-fio, até o J21.
5. **Painéis:** monte os bornes, os potenciômetros, os botões, os LEDs e o OLED nas tampas. Ligue os fios às placas com folga para abrir a tampa.
6. **Caixas lado a lado:** encoste A, C e B e una as laterais com os 4 parafusos M3 × 10 e as porcas, pelos furos de 3,4 mm. Passe os cabos J10 e J20 pelos entalhes de cima das laterais.
7. **Fechamento:** feche as tampas (M3 × 12) e as portinholas (M3 × 10).
8. **Cargas, fora das caixas:** os resistores de 10 W nos bornes CARGA da caixa A; o de 47 Ω / 50 W, na chapa de metal, nos bornes CARGA (Vo) da caixa B.

## 7. Medidas para conferir antes de imprimir

As caixas foram desenhadas com os valores típicos abaixo. Confira as peças com o paquímetro: uma peça maior que o valor da tabela pode não caber na caixa ou no furo.

| Peça | O que conferir | Valor típico |
|---|---|---|
| Transformador (15 + 15 V, 36 VA) | largura × profundidade × altura | 67 × 31 × 56 mm |
| Transformador | furos de fixação | 2 furos M3, a 56 mm entre centros (os rasgos aceitam ±6 mm) |
| Botão de emergência | profundidade atrás do painel, com o bloco de contato | 45 mm |
| Botão de emergência | diâmetro do cogumelo e da placa amarela | 40 mm e 60 mm |
| Botão de emergência | chaveta anti-giro | não prevista |
| Chave gangorra KCD1-104N | espessura de painel que as travas aceitam | 2,0 mm |
| Porta-fusível 5 × 20 | lado chato na rosca; profundidade atrás do painel | sem lado chato; 40 mm |
| Potenciômetro 10 kΩ | pino anti-giro: distância ao centro e diâmetro | 7,8 mm e 2,6 mm (furo cego, por baixo do painel) |
| OLED 0,96" | tamanho da placa | 27 × 27,5 mm |
| OLED | furos M2, distância entre centros | 23 × 23,5 mm |
| OLED | área visível e distância até a borda de cima da placa | 22 × 11 mm, a 4,5 mm |
| OLED | espessura do vidro | 1,6 mm |
| Cabo de força | diâmetro externo | 7,5 mm |
| Peças mais altas de cada placa | altura acima da placa | A: 30 mm (toroide em pé, eletrolítico de 100 µF / 63 V, TO-220 com dissipador); B: 24 mm; C: 20 mm |
| ESP32-S3 na placa C | altura da barra fêmea; centro da USB-C | 8,5 mm; cerca de 26 mm acima do fundo, por fora |
| Placas A, B e C | tamanho; furos M3 | A 95 × 80 mm, B 100 × 75 mm, C 100 × 60 mm; furos a 5,0 mm das bordas |
| Plugue USB-C | passagem até a S3 | a ponta da S3 fica 27 mm para dentro da borda da placa C, com o J35, o J36, o J40 e o R26 no caminho: confira se o plugue passa com os cabos dos botões ligados |
