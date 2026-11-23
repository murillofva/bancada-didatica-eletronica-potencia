# Bancada didática de eletrônica de potência

Material complementar do Trabalho de Conclusão de Curso "Desenvolvimento de bancada didática para estudo de eletrônica de potência", do curso de Bacharelado em Engenharia de Controle e Automação do IFSP, campus Catanduva (2026).

A bancada reúne os quatro retificadores monofásicos (meia onda e ponte, não controlados e controlados) e os conversores CC-CC Boost e Buck síncrono, comandados por uma ESP32-S3 DevKitC-1 N16R8. As medidas aparecem num display OLED e num painel web, disponível via Wi-Fi, que também comanda a bancada e registra os dados de cada experimento em CSV.

O texto do trabalho descreve o projeto, os ensaios e os roteiros das práticas (Apêndice A), as instruções de montagem (Apêndice B) e o firmware (Apêndice C). Este repositório traz os arquivos citados no texto como material complementar.

## Conteúdo

| Pasta | O que tem |
| --- | --- |
| `esquemas/placas/` | Esquemas das placas A (conversores CC-CC), B (retificadores) e C (controle), em alta resolução, com a lista de peças de cada placa |
| `cadernos/` | Os quatro cadernos de montagem e ensaio, um por módulo (Boost, Buck, Retificadores, Medição e controle): as folhas de esquema do módulo, com a lista de peças e as conferências, e as folhas de etapa, com os pontos de medição e o passo a passo de cada ensaio; e as quatro planilhas-modelo (XLSX), para registrar as medidas de cada etapa |
| `pci/A/`, `pci/B/`, `pci/C/` | Para cada placa de circuito impresso (face simples, fabricação caseira): o PDF de fabricação (cobre em 1:1, furação, face de cima e mapa de montagem), o projeto do Proteus, os arquivos Gerber e o relatório de conferência |
| `caixas/` | Modelos das caixas para impressão 3D (STL das bases, tampas, portinholas, presilha e gabarito de furos) e o guia de impressão |
| `firmware/` | Código-fonte do firmware da bancada (versão 2.3.5) e o guia do firmware |

## Por onde começar

1. Leia as instruções de montagem no Apêndice B do trabalho e os guias das pastas `firmware/` e `caixas/`.
2. Fabrique as placas com os PDFs de `pci/`. Cada placa cabe numa placa cobreada de 10 cm × 10 cm. Imprima o cobre em tamanho real (100%), numa impressora laser.
3. Monte e confira cada placa com o multímetro antes de energizar, seguindo as folhas de esquema dos cadernos (`cadernos/`).
4. Grave o firmware na ESP32-S3, como descrito em `firmware/LEIA-ME.md`.
5. Imprima primeiro o gabarito de furos e confira as medidas típicas antes de imprimir as caixas (`caixas/LEIA-ME.md`).
6. Ligue uma parte de cada vez, seguindo as etapas dos cadernos: comece pela etapa 1 do caderno de Medição e controle e registre as medidas nas planilhas-modelo.

## Segurança

- O módulo dos retificadores é ligado à rede elétrica de 127 V por um transformador de 15 + 15 V, 36 VA, que isola a bancada da rede. Nunca use um autotransformador (variac) no lugar dele.
- O primário tem fusível rápido de 1 A (F1), chave de dois polos e botão de emergência; o secundário tem outro fusível rápido de 1 A (F2).
- Só um experimento alimentado por vez: ou a fonte CC ajustável dos conversores, ou o transformador dos retificadores.
- Troque os jumpers de configuração só com tudo desligado.

## Firmware

O firmware é um único programa para o ambiente Arduino (pacote esp32 da Espressif, versão 3.3.12), com a biblioteca U8g2 para o display. A rede Wi-Fi própria da bancada e a senha dela estão definidas em `firmware/bancada/config.h` e podem ser trocadas antes da gravação.
