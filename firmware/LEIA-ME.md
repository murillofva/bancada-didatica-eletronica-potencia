# Firmware da bancada didática

Versão **2.3.5**. O número aparece no Monitor Serial ao ligar a placa e na resposta do comando `?`.

Um só programa comanda e mede os cinco experimentos da bancada, numa **ESP32-S3 DevKitC-1 N16R8** (16 MB de flash e 8 MB de PSRAM). A bancada é comandada pelo **painel web**, que abre no navegador de qualquer dispositivo com Wi-Fi (computador, celular ou tablet). O **Monitor Serial** fica de reserva.

| Modo | Experimento | O que a ESP32-S3 faz |
|---|---|---|
| `BOOST` | Conversor Boost | PWM de 100 kHz no GPIO15 para o driver TC4427; D até 0,50 |
| `BUCK` | Conversor Buck síncrono | PWM de 50 kHz no GPIO16 para o IN do IRS2104 e SD (ativo em nível baixo) no GPIO17; D até 0,90 |
| `RET_NC` | Retificadores não controlados (diodos) | só mede |
| `RET_MEIA` | Meia onda controlada (1 SCR) | dispara o SCR A, sincronizado com a rede |
| `RET_PONTE` | Ponte semicontrolada (2 SCRs e 2 diodos) | dispara os SCRs A e B |

**Medidas** (médias de 1 s): nos conversores, Vin, Iin, Vo, Io, Pin, Pout e η, com dois INA226; nos retificadores, Vo e Io médios e eficazes, Pdc, Pca e η, com um ADS1115, um divisor de tensão e um ACS712. Elas aparecem na tela OLED, no painel web, no Monitor Serial e no registro em CSV.

## 1. Arquivos do projeto

```
firmware/
├── LEIA-ME.md             este guia
└── bancada/
    ├── bancada.ino        setup() e loop()
    ├── config.h           pinos, endereços I2C, limites, chaves e valores padrão
    ├── pwm_conversores.*  PWM do Boost e do Buck (periférico LEDC) e rampa de D
    ├── disparo_scr.*      disparo dos SCRs sincronizado com a rede (interrupções e timers)
    ├── medicao.*          drivers do INA226 e do ADS1115, médias, RMS e calibração
    ├── tela.*             tela OLED (biblioteca U8g2)
    ├── painel.*           LEDs e, com PAINEL_MANUAL 1, botões e potenciômetros
    ├── controle.*         modos, liga e desliga com trava, proteções
    ├── comandos.*         Monitor Serial, comando "?" e log CSV
    ├── registro.*         histórico, leituras rápidas e registro CSV na PSRAM
    └── web.*              rede Wi-Fi e painel web
```

Cada módulo tem um `.h` e um `.cpp`. Para mudar um pino, um limite ou um valor padrão, mude só o `config.h`.

## 2. Como compilar e gravar

### 2.1 Programas e biblioteca

1. Instale a IDE do Arduino.
2. No Gerenciador de Placas, instale o pacote **esp32** (Espressif Systems), versão **3.3.12**. Ele já traz o Wi-Fi, o servidor web e o sistema de arquivos FFat usados pelo firmware.
3. Em Gerenciar Bibliotecas, instale a **U8g2** (olikraus). É a única biblioteca externa: o INA226 e o ADS1115 são lidos por drivers do próprio firmware, registrador por registrador.
4. Abra `firmware/bancada/bancada.ino`.

### 2.2 Ajustes da IDE para a N16R8 (menu Ferramentas)

| Opção | Valor |
|---|---|
| Placa | ESP32S3 Dev Module |
| Flash Size | 16MB (128Mb) |
| PSRAM | OPI PSRAM |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
| USB CDC On Boot | Disabled com o cabo no conector COM (UART); Enabled com o cabo no conector USB (nativa) |
| USB Mode | Hardware CDC and JTAG |

- **PSRAM:** guarda o histórico, as leituras rápidas, o registro CSV (4 MB) e o espelho do Monitor Serial usado pelo painel web. Sem a opção OPI PSRAM, o firmware avisa "PSRAM nao encontrada" e usa memórias pequenas na RAM interna.
- **Partition Scheme:** a partição FAT guarda a cópia do registro salva na flash. Ela é formatada no primeiro uso.

### 2.3 Gravação

- **Cabo USB:** a placa tem dois conectores USB-C. No COM (UART), use "USB CDC On Boot" em Disabled. No USB (nativa), use Enabled; com Disabled, a gravação funciona, mas o Monitor Serial fica mudo.
- **Jumper JP8 da placa C (5 V da S3):** fica aberto enquanto a USB estiver ligada ao computador.
- **Se a gravação não começar:** segure **BOOT**, aperte e solte **RST** e solte o **BOOT**. Se a placa ficar parada em "waiting for download" depois da gravação, aperte **RST**.
- **Porta ocupada:** o Monitor Serial aberto segura a porta. Feche-o antes de gravar por outro programa.
- **Monitor Serial:** 115200 baud, final de linha "Nova linha". Ao ligar, a placa mostra a versão, os módulos I2C encontrados, a rede Wi-Fi, a calibração e a lista de comandos.

**Ao ligar ou resetar,** todas as saídas começam desligadas (PWMs em 0, SD do Buck em LOW e gates em LOW) e o modo é `RET_NC` (só medição). Os resistores de 10 kΩ ao GND seguram as saídas em LOW durante o reset.

## 3. Pinos e módulos I2C

### 3.1 Pinos da ESP32-S3

| Função | GPIO | Liga em | Observação |
|---|---|---|---|
| PWM do Boost | 15 | TC4427, IN A | 10 kΩ do pino ao GND |
| PWM do Buck | 16 | IRS2104, IN | 10 kΩ do pino ao GND |
| SD do Buck | 17 | IRS2104, SD | 10 kΩ ao GND; LOW = driver desligado |
| GATE_A | 39 | 220 Ω → LED do MOC3023 do SCR A | 10 kΩ ao GND |
| GATE_B | 40 | 220 Ω → LED do MOC3023 do SCR B | 10 kΩ ao GND |
| ZC (passagem por zero) | 41 | coletor do H11AA1 | pull-up externo de 10 kΩ para 3,3 V |
| POL (polaridade da rede) | 42 | coletor do PC817 | pull-up externo de 10 kΩ para 3,3 V |
| I2C SDA | 8 | SDA dos quatro módulos | barramento de 400 kHz |
| I2C SCL | 9 | SCL dos quatro módulos | — |
| LED "saída ligada" | 14 | 330 Ω a 1 kΩ → LED → GND | aceso com o PWM ou o disparo ligado |
| LED "sincronizado" | 21 | 330 Ω a 1 kΩ → LED → GND | aceso com a rede sincronizada (45 a 65 Hz) |
| Pot 1 (D), opcional | 1 | J31 da placa C | só com `PAINEL_MANUAL 1` |
| Pot 2 (α), opcional | 2 | J32 | só com `PAINEL_MANUAL 1` |
| Pot 3 (página da tela), opcional | 4 | J33 | só com `PAINEL_MANUAL 1` |
| Botão MODO, opcional | 5 | J34 | entre o pino e o GND; só com `PAINEL_MANUAL 1` |
| Botão LIGA-DESLIGA, opcional | 6 | J35 | idem |
| Botão ZERO, opcional | 7 | J36 | idem |

- **ZC e POL:** além do pull-up externo, o firmware liga o pull-up interno, para o pino não flutuar com a placa solta.
- **GATE_A, GATE_B, ZC e POL** precisam ficar em GPIO 32 ou maior, porque o disparo lê e escreve direto no banco de registradores desses pinos. O `config.h` confere isso na compilação.
- **Pinos livres de propósito:** 0, 3, 45 e 46 (boot); 19 e 20 (USB nativa); 26 a 37 (flash e PSRAM octal); 43 e 44 (serial do conector COM); 38 e 48 (LED RGB da placa, conforme a versão).
- **Potenciômetros:** ficam no ADC1, que funciona com o Wi-Fi ligado. Com `PAINEL_MANUAL 0`, o programa não lê os pinos dos potenciômetros nem dos botões, e eles podem ficar soltos.

### 3.2 Módulos I2C

Todos são alimentados com **3,3 V** (pino 3V3 da S3), com GND comum.

| Módulo | Endereço | Como deixar nesse endereço |
|---|---|---|
| OLED SSD1306 128 × 64 | 0x3C | de fábrica; se vier em 0x3D, mude `END_OLED` no `config.h` |
| INA226 #1, entrada do conversor (Vin, Iin) | 0x40 | A0 e A1 no GND (padrão do módulo) |
| INA226 #2, saída do conversor (Vo, Io) | 0x41 | ilha A0 ligada ao VCC; A1 no GND |
| ADS1115, retificadores | 0x48 | pino ADDR no GND |

- **INA226 #2:** A0 e A1 são ilhas de solda que vêm ligadas ao GND. Desfaça a ligação da A0 ao GND antes de ligá-la ao VCC; senão, o 3,3 V vai direto ao GND. Depois, o comando `i2c` deve mostrar 0x40 e 0x41.
- **INA226 no caminho de potência:** em série com o fio positivo, com shunt de 0,1 Ω. A corrente entra pelo IN+ e sai pelo IN−. O pino VBUS (VBS em alguns módulos) vai ao IN− do mesmo módulo. O INA226 mede no máximo 36 V.
- **ADS1115 sempre em 3,3 V, nunca em 5 V:** os pull-ups do módulo levariam 5 V aos pinos da S3.
- **Entradas do ADS1115:** A0 recebe a tensão do retificador por um divisor (100 kΩ em cima; o `config.h` supõe 8,2 kΩ embaixo). A1 recebe a saída do ACS712-5A por um resistor de 1 kΩ em série. O ACS712 é alimentado com 5 V.
- **Módulo ausente:** o firmware procura de novo o INA226 e o ADS1115 a cada 3 s. O comando `i2c` faz uma varredura completa.

## 4. Chaves do config.h

| Chave | Padrão | O que faz |
|---|---|---|
| `PAINEL_MANUAL` | 0 | **0:** sem potenciômetros e sem botões; comando pelo painel web e pela serial, e a tela OLED troca de página sozinha. **1:** Pot 1 (D), Pot 2 (α), Pot 3 (página da tela) e botões MODO, LIGA-DESLIGA e ZERO. O LIGA só funciona com o potenciômetro do modo no início do curso |
| `EXIGIR_INA226` | 1 | **1:** o Boost e o Buck só ligam com os dois INA226 respondendo, e a saída desliga se um deles parar de responder. **0:** liga mesmo sem eles, com aviso; cada limite só vale para o INA226 que responde e, sem nenhum, não há proteção de corrente nem de tensão |
| `OLED_SH1106` | 0 | **0:** display SSD1306. **1:** display SH1106, comum nos de 1,3" |

- **Nunca deixe `PAINEL_MANUAL` em 1 sem os potenciômetros ligados:** o pino solto lê ruído, e o D ou o α pode subir sozinho.
- **Com `EXIGIR_INA226` em 0, limite a corrente na fonte de bancada.**
- **Outros valores do `config.h`:** nome e senha da rede (`WIFI_SSID` e `WIFI_SENHA`), número de dispositivos (`WIFI_MAX_CLIENTES`), tempo do homem-morto (`WEB_HOMEM_MORTO_MS`), limites das proteções (seção 8), frequências e D máximos, e calibrações padrão (seção 9).

Depois de mudar uma chave ou um valor, compile e grave de novo.

## 5. Comando pelo painel web

### 5.1 Conectar

1. Ligue a bancada. A placa cria a rede Wi-Fi **Bancada-TCC**.
   - A senha está no `config.h`, na constante `WIFI_SENHA`, e pode ser trocada (no mínimo 8 caracteres).
   - Ao ligar, a tela OLED mostra o endereço por 5 s. O comando `wifi` mostra a rede, a senha e o endereço.
2. Conecte o dispositivo nessa rede e abra **http://192.168.4.1** no navegador.
3. Até **seis dispositivos** podem ficar conectados ao mesmo tempo.

### 5.2 O que o painel mostra

- **Estado:** modo, saída ligada ou desligada, D ou α (atual e alvo), frequência do PWM ou da rede, quem controla e o último desligamento automático.
- **Sincronismo (retificadores):** frequência da rede, largura W do pulso de ZC e meio período, sinal POL e semiciclos disparados em A e em B.
- **Medidas:** as médias de 1 s, com os valores teóricos ideais ao lado (Vo médio e eficaz com o α atual; Vo com a Vin medida e o D atual).
- **Histórico:** médias de 1 s da última hora, com janela de 2 min, 10 min ou 1 h.
- **Leitura rápida:** os INA226 a cada cerca de 50 ms, nos últimos 30 s (conversores). Mostra transitórios, como a rampa de D.
- **Monitor serial e comandos** (seção 5.5) e **Registro em CSV** (seção 7).

### 5.3 Controle

- **Pegar o controle:** sem o controle, o painel só mostra. Só um dispositivo fica com o controle por vez.
- **Com o controle:** modo, ligar e desligar, D, α, frequência, rampa de α, pulso de gate, carga R, zero do ACS712 e página da tela OLED.
- **Confirmação:** ligar a saída, trocar o modo ou a frequência com a saída ligada e apagar o registro abrem uma faixa no pé da página, com **Confirmar** e **Cancelar**. Sem resposta em 10 s, ela se cancela sozinha.
- **Partida segura:** ligar sempre parte de D = 0 ou α = 180°.
- **Soltar o controle** desliga a saída e libera o controle. Fechar a aba faz o mesmo, na hora.
- **Serial:** continua valendo como reserva. Cada comando do painel aparece nela com `[painel web]` na frente.

### 5.4 DESLIGA TUDO e homem-morto

- **DESLIGA TUDO:** o botão vermelho fica sempre no topo da página e funciona em qualquer dispositivo, mesmo sem o controle.
- **Homem-morto:** o dispositivo com o controle manda um sinal de vida a cada 0,5 s. Sem sinal por **2 s**, a saída desliga e o controle fica livre.
  - Acontece se o Wi-Fi cair ou se o navegador parar a página (num celular, ao bloquear a tela, por exemplo).
  - A saída desliga de 1,5 a 2,0 s depois da queda, mais o tempo que o dispositivo leva para cortar o Wi-Fi.
  - Nada religa sozinho.
- **Saída ligada pela serial:** não há homem-morto. Ela desliga pelo DESLIGA TUDO, pelo comando `0`, pela troca de modo, por uma proteção, pelo botão RST ou pela fonte.
- **Aviso:** o painel e a tela OLED mostram o motivo do último desligamento automático até a saída ligar de novo ou até um dispositivo pegar o controle. O `?` mostra sempre o último.

### 5.5 Monitor serial e caixa de comandos

- **Monitor:** mostra o que a placa escreve na serial, com o mesmo texto. A placa guarda esse texto num buffer circular de 64 kB na PSRAM (cerca de 800 linhas; só o fim fica). O monitor atualiza a cada 0,5 s, sem precisar do controle, e tem os botões **Pausar** e **Limpar tela**.
- **Caixa de comandos:** aceita qualquer comando da serial (seção 6), com as mesmas regras, menos o `pot`.
  - Sem o controle, só os que só mostram: `?`, `lim`, `cal`, `wifi`, `registro` e `ajuda`. O `0` (DESLIGA TUDO) vale sempre.
  - O `l` pede um segundo `l` pela caixa em até 5 s.
  - Uma resposta com mais de cerca de 4 kB chega cortada na caixa; o monitor mostra a resposta inteira.

### 5.6 Tela OLED

O cabeçalho mostra o modo, LIG ou DESL e o valor ajustado (D ou α). As páginas trocam sozinhas a cada 5 s; `tela 0`, `tela 1` ou `tela 2` fixa uma página, e `tela auto` volta a trocar.

| Página | Conversores | Retificadores |
|---|---|---|
| 0 | Vo, Io e η | Vo médio, Io médio, frequência da rede e sincronismo |
| 1 | Vin, Iin, Pin e Pout | Vo e Io eficazes, η e Io = Vo/R |
| 2 | quem controla, módulos I2C, rede, frequência do PWM e último desligamento automático | idem, com a largura do pulso de gate e o R da carga |

## 6. Comando pelo Monitor Serial

115200 baud, final de linha "Nova linha". Os números aceitam vírgula ou ponto.

| Comando | O que faz |
|---|---|
| `modo boost`, `modo buck`, `modo nc`, `modo meia`, `modo ponte` | troca o modo (desliga as saídas antes) |
| `l` | (letra L) liga a saída. Mande **duas vezes**, a segunda em até 5 s; outro comando no meio cancela. Parte de D = 0 ou α = 180° |
| `0` | (zero) desliga tudo na hora |
| `d 0.30` | D do Boost ou do Buck, com a saída ligada. Aceita porcentagem (`d 30`). Limitado a 0,50 no Boost e 0,90 no Buck |
| `a 90` | α em graus, com o disparo ligado. 179° ou mais = sem disparo |
| `f 100000` | frequência do PWM: Boost de 20 000 a 150 000 Hz; Buck de 20 000 a 100 000 Hz. Desliga a saída |
| `p 500` | largura do pulso de gate dos SCRs, de 100 a 2000 µs |
| `rampa 1` | rampa de α, em graus por semiciclo (0 = sem rampa; máximo 30) |
| `r 47` | resistência da carga do retificador, em Ω (para Io = Vo/R) |
| `zero` | zero do ACS712 (saída desligada e sem corrente na carga) |
| `cal` | mostra a calibração, o fundo de escala dos INA226 e as leituras cruas dos sensores |
| `cal rsh1`, `cal rsh2`, `cal div`, `cal acs`, `cal padrao` | calibração (seção 9) |
| `i2c` | procura os módulos I2C de novo (com a saída desligada) |
| `lim` | mostra os limites das proteções, o fundo de escala e o limite efetivo |
| `lim io 0.20`, `lim iin 0.20`, `lim padrao` | limites de teste (seção 8) |
| `log 1000` | uma linha CSV a cada 1000 ms; `log 0` para |
| `tela auto`, `tela 0`, `tela 1`, `tela 2` | página da tela OLED |
| `wifi` | rede, senha, endereço e número de dispositivos conectados |
| `grava 1000` | registro CSV na PSRAM, uma linha a cada 1000 ms; `grava 0` para |
| `registro` | estado do registro; `registro apaga` apaga a memória |
| `salva` | copia o registro para a flash (só com a saída desligada) |
| `pot 1`, `pot 0` | só com `PAINEL_MANUAL 1`: controle pelos potenciômetros ou pela serial |
| `?` | estado completo e medidas |
| `ajuda` | lista de comandos |

- **Rampas:** D muda no máximo 0,5 por segundo. O α muda 1° por semiciclo no padrão.
- **Marca de tempo:** as mensagens de evento e a primeira linha das respostas mais usadas começam com `[t=1234.5 s]`, o tempo desde o reset.
- **O `?`:** mostra o modo, a saída, quem controla, as medidas, os valores teóricos, a calibração, os módulos I2C e o último desligamento automático.

## 7. Registro em CSV

As colunas são as mesmas no `log` da serial e no registro do painel web:

```
tempo_ms;modo;D;alfa;Vin;Iin;Vo;Io;Pin;Pout;eta;Vo_med;Vo_rms;Io_med;Io_rms;f_rede
```

| Coluna | Unidade | Observação |
|---|---|---|
| `tempo_ms` | ms | desde o reset |
| `modo` | — | BOOST, BUCK, RET_NC, RET_MEIA ou RET_PONTE |
| `D`, `alfa` | —, graus | valor aplicado; vazio no modo em que não se aplica |
| `Vin`, `Iin`, `Vo`, `Io` | V, A | INA226 #1 e #2 (conversores) |
| `Pin`, `Pout` | W | média de V × I |
| `eta` | % | Pout/Pin nos conversores; Pdc/Pca nos retificadores |
| `Vo_med`, `Vo_rms`, `Io_med`, `Io_rms` | V, A | ADS1115 (retificadores) |
| `f_rede` | Hz | vazio sem sincronismo |

- **Pela serial (`log`):** separador ponto e vírgula e ponto decimal. Campo vazio = não se aplica ao modo ou sensor ausente. Use `log 1000` ou mais, porque os valores são médias de 1 s. Linhas que começam com `# ` são mensagens: tire-as antes de montar a tabela.
- **Pelo painel web:** **Iniciar** (uma linha a cada 0,2 s a 1 min), **Parar**, **Baixar CSV**, **Salvar na flash**, **Baixar o salvo** e **Apagar**. O arquivo baixado vem com vírgula decimal e abre direto no Excel em português.
- **Pela serial:** `grava` inicia e para o registro, `registro` mostra o estado e `salva` copia o registro para a flash.

| Memória | O que guarda | Tamanho |
|---|---|---|
| PSRAM | histórico de 1 s | 3600 pontos (1 h) |
| PSRAM | leituras rápidas dos INA226 | 2400 pontos (2 min) |
| PSRAM | registro CSV | 4 MB (cerca de 35 mil linhas) |
| PSRAM | espelho do Monitor Serial | 64 kB, circular |
| Flash (FAT) | cópia do registro | até cerca de 9 MB |

- **Durante o experimento, nada vai para a flash:** gravar na flash pausa o processador por alguns milissegundos e atrapalharia o disparo dos SCRs. Por isso, salvar na flash só funciona com a saída desligada.
- **O registro na PSRAM se perde ao desligar a placa.** A cópia na flash sobrevive ao reset.

## 8. Proteções e limites

| Modo | A saída desliga quando | Medida usada |
|---|---|---|
| BOOST | Vo > 30 V; Iin > 0,70 A; um INA226 no fundo de escala; um INA226 parou de responder (com `EXIGIR_INA226 1`) | INA226 #1 e #2, a cada 50 ms |
| BUCK | Iin > 0,70 A; Io > 0,70 A; um INA226 no fundo de escala; um INA226 parou de responder (com `EXIGIR_INA226 1`) | INA226 #1 e #2, a cada 50 ms |
| RET_MEIA e RET_PONTE | nenhum pulso válido de passagem por zero (ZC) por mais de 50 ms; sem sincronismo por mais de 250 ms | detector de ZC (H11AA1, GPIO41) |
| Todos | troca de modo, `0`, `f` nos conversores, DESLIGA TUDO, homem-morto do painel web (2 s) | — |

- **Fundo de escala do INA226:** ele mede no máximo 81,92 mV no shunt. A maior corrente lida é 81,92 mV / Rsh: cerca de 0,82 A com 0,1 Ω e cerca de 0,75 A com um Rsh calibrado de 0,109 Ω. Acima disso a leitura trava e mostra menos do que passa. Nos conversores, com a saída ligada, um Vshunt a 99,5 % do fundo de escala (cerca de 81,5 mV) desliga a saída como sobrecorrente.
- **Limite efetivo:** cada limite de corrente fica no menor valor entre o `config.h` (0,70 A) e 95 % do fundo de escala do INA226 que mede aquela corrente, arredondado para baixo em 0,01 A. O comando `lim` mostra esse valor.
- **INA226 que parou de responder:** três leituras seguidas com erro (cerca de 150 ms) desligam a saída.
- **Disparo dos SCRs:** só liga com pulsos de ZC válidos, de 45 a 65 Hz. A meia onda precisa também do sinal POL. Na ponte, sem POL, o firmware entra no modo reserva: dispara o GATE_A e o GATE_B em todo semiciclo.
- **Depois de uma proteção,** a saída fica desligada até um novo LIGA, e os alvos voltam para D = 0 e α = 180°.
- **Teste das proteções com o `lim`:** `lim io <A>` baixa o limite de Io do Buck, e `lim iin <A>` baixa o de Iin do Boost e do Buck.
  - Aceita de 0,05 A até o limite efetivo, em passos de 0,01 A, e só com a saída desligada.
  - Fica só na RAM: `lim padrao`, o reset ou religar a placa voltam ao `config.h`.
  - O limite de Vo do Boost (30 V) não muda por comando.

**Cuidados:**

- As proteções são por programa e reagem em cerca de 50 a 150 ms. A proteção principal continua sendo o **limite de corrente da fonte de bancada** e os **fusíveis F1 (primário) e F2 (secundário), rápidos de 1 A**.
- **Boost nunca sem carga:** mantenha a carga mínima de 2,2 kΩ na saída. O INA226 mede até 36 V.
- **Só um experimento alimentado por vez:** ou a fonte de bancada (conversores) ou o transformador (retificadores). Todos os circuitos de potência ficam com o mesmo GND da S3.
- **Retificadores:** o −Vo fica ligado ao GND da S3, como referência do divisor. Isso é seguro porque o secundário do transformador de 15 + 15 V, 36 VA é isolado da rede. Nunca use autotransformador (variac) no lugar dele.
- **Osciloscópio:** garra só no −Vo ou no GND, nunca no L ou no N do secundário.
- **Retificadores sem proteção de corrente por programa:** a média do ACS712 é de 1 s. Use a carga prevista e os fusíveis.
- **Buck:** ao ligar, com D = 0, a corrente da fonte tem de ficar perto de zero.
- **Medida eficaz:** cada conversão do ADS1115 é uma média de cerca de 1,16 ms. Em ondas com degrau (disparo do SCR), o valor eficaz medido sai menor que o real, e a diferença cresce com α. Use as médias como medida principal.

## 9. Calibração

Faça com a bancada montada e ligada há cerca de 5 min, nesta ordem: shunts, divisor, ACS712 e carga. O comando `cal` mostra a calibração atual e as leituras cruas de todos os sensores. Os valores ficam gravados na memória da ESP32-S3 (NVS) quando a saída está desligada e sobrevivem ao reset.

| Comando | O que calibra | Padrão do `config.h` | Faixa aceita |
|---|---|---|---|
| `cal rsh1 <Ω>` | shunt do INA226 #1 | 0,100 Ω | 0,001 a 10 Ω |
| `cal rsh2 <Ω>` | shunt do INA226 #2 | 0,100 Ω | 0,001 a 10 Ω |
| `cal div <razão>` | razão do divisor do A0 (Vo = V_A0 × razão) | 13,195 (100 kΩ + 8,2 kΩ) | 1 a 100 |
| `zero` | zero do ACS712 (A1 com 0 A) | 2,500 V | 1,5 a 3,5 V |
| `cal acs <V/A>` | sensibilidade do ACS712; negativo inverte o sentido | 0,185 V/A | 0,01 a 2 V/A |
| `r <Ω>` | carga do retificador, só para Io = Vo/R | 47 Ω | 0,1 a 100 000 Ω |
| `cal padrao` | volta todos os valores ao `config.h` | — | — |

1. **Shunts (`cal rsh1`, `cal rsh2`):**
   - meça um resistor de 47 Ω / 10 W frio, descontando a resistência das pontas → R47;
   - monte, sem conversor: fonte (+) → IN+ do #1 → IN− do #1 → IN+ do #2 → IN− do #2 → 47 Ω → fonte (−), com a fonte em 12 V e limite de 0,5 A;
   - escolha o modo BOOST, com a saída desligada;
   - meça a tensão no 47 Ω → V47, e mande `cal` logo em seguida, com o resistor ainda frio;
   - I_ref = V47 / R47; Rsh = Vshunt / I_ref, com o Vshunt de cada INA226 mostrado pelo `cal`;
   - grave com `cal rsh1 <valor>` e `cal rsh2 <valor>`. O fundo de escala e o limite efetivo mudam junto (confira com `lim`).
2. **Divisor (`cal div`):**
   - modo RET_NC, com o transformador fora da tomada;
   - ligue a fonte de bancada em corrente contínua (cerca de 19,5 V, limite de 0,1 A) na entrada do divisor: + no topo do 100 kΩ e − no GND;
   - meça essa tensão com o multímetro → V_ref, e leia o A0 no `cal`;
   - razão = V_ref / A0 → `cal div <razão>`. Confira com outra tensão;
   - o resistor de baixo pode ter de 3,3 a 8,2 kΩ. Com 10 kΩ ou mais, o pico do retificador passaria de 3,3 V no A0.
3. **Zero do ACS712 (`zero`):**
   - saída desligada e corrente zero na carga (nos retificadores, transformador desligado);
   - o zero leva cerca de 2 s e é recusado se o A1 estiver longe de 2,5 V;
   - refaça o zero antes de cada série e depois de mexer em qualquer fio da montagem.
4. **Sensibilidade do ACS712 (`cal acs`):**
   - monte: fonte (+) → IN+ do INA226 #1 → IN− → IP+ do ACS712 → IP− → 10 Ω / 10 W → fonte (−). O INA226 #1, já calibrado, é o amperímetro;
   - faça o `zero` com a fonte desligada e leia o `cal` com duas correntes (por exemplo, cerca de 0,5 A e 0,25 A);
   - sensibilidade = (A1 na corrente maior − A1 na menor) / (I maior − I menor) → `cal acs <V/A>`;
   - o ACS712 é raciométrico: calibre com a mesma fonte de 5 V usada nas medidas. O datasheet indica 185 mV/A em 5 V.
5. **Carga (`r`):** meça o resistor de carga do retificador frio, descontando as pontas, e mande `r <valor>`.

## 10. Problemas comuns

| Sintoma | Causa provável | O que fazer |
|---|---|---|
| `i2c` não acha nada | sem 3,3 V ou GND nos módulos; SDA e SCL trocados | confira as ligações, um módulo por vez |
| Só aparece um INA226 (0x40) | ilha A0 do INA226 #2 não ligada ao VCC | seção 3.2 |
| LIGA recusado: "INA226 ausente" | um INA226 não responde, com `EXIGIR_INA226 1` | `i2c`; o Boost e o Buck precisam dos dois |
| "PROTECAO: INA226 #1 mudo" (ou #2) | um INA226 parou de responder com a saída ligada | fios SDA e SCL e alimentação do módulo; `i2c` com a saída desligada |
| "PROTECAO: INA226 #1 no fundo de escala" (ou #2) | corrente acima de 81,92 mV / Rsh | carga errada, curto ou D alto demais; confira antes de ligar de novo |
| "ATENCAO: limite de teste ativo" | um `lim io` ou `lim iin` ainda vale | `lim padrao` com a saída desligada, ou reset |
| `l` na serial só pergunta | confirmação do LIGA | mande `l` de novo em até 5 s |
| Caixa de comandos: "Pegue o controle antes" | sem o controle, só os comandos que só mostram | **Pegar o controle**, ou use a serial |
| Monitor do painel sem conexão com a bancada | o dispositivo saiu da rede Bancada-TCC ou a placa está reiniciando | volte à rede; o monitor continua de onde parou |
| `pot 1` recusado | `PAINEL_MANUAL 0` (sem potenciômetros) | use o painel web ou `d` e `a` |
| D ou α mudam sozinhos | `PAINEL_MANUAL 1` sem os potenciômetros ligados | ligue os potenciômetros ou volte a chave para 0 |
| LIGA recusado nos SCRs: "sem sinal de ZC" ou "rede sem sinc." | sem pulsos de ZC válidos no GPIO41 | H11AA1, pull-up de 10 kΩ e transformador ligado |
| LIGA recusado na meia onda: "sem sinal POL" | PC817 ou pull-up do GPIO42 | confira o circuito do POL |
| "media simples (sem sincronismo)" no `?` | detector de ZC desligado | ligue o H11AA1 também no RET_NC |
| `?` avisa que o A0 chegou perto de 3,3 V | pico de Vo alto para o divisor | resistor de baixo menor (até 3,3 kΩ) e `cal div` de novo |
| Zero do ACS712 recusado | A1 longe de 2,5 V (ACS712 sem 5 V, por exemplo) | confira a alimentação e as ligações do ACS712 |
| Iin ou Io negativos | corrente entrando pelo IN− | inverta IN+ e IN− |
| Io do ACS712 negativa | IP+ e IP− invertidos | inverta, ou `cal acs` com valor negativo |
| Vo = 0 no INA226 com tensão no circuito | VBUS solto | ligue o VBUS ao IN− |
| Tela com lixo ou deslocada 2 pixels | display SH1106 | `OLED_SH1106` em 1 no `config.h` |
| "PSRAM nao encontrada" ao ligar | PSRAM desligada na IDE | PSRAM = OPI PSRAM (seção 2.2) e grave de novo |
| A S3 reinicia ao medir nos pinos do INA226 | a ponta de prova encostou ao mesmo tempo no GND e no VCC, que são vizinhos | meça o GND na ponta de fora do fio de GND do módulo |
| A S3 reinicia ao ligar a potência | ruído no GND | GND num ponto só e fios curtos |
