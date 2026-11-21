/*
  registro.cpp - Historico, leituras rapidas e registro CSV na PSRAM.
  Ver registro.h.
*/
#include <Arduino.h>
#include "config.h"
#include "registro.h"
#include <FFat.h>
#include <math.h>
#include "medicao.h"
#include "controle.h"
#include "pwm_conversores.h"
#include "disparo_scr.h"
#include "comandos.h"

// ---------------- memorias ----------------
static PontoHist   *hist     = nullptr;
static PontoRapido *rapido   = nullptr;
static char        *texto    = nullptr;
static uint32_t     histCap  = 0, rapidoCap = 0;
static size_t       textoCap = 0;
static bool         psramOk  = false;

// "Publicados" pelo escritor (loop) depois de escrever o dado: os leitores so
// leem o que ja foi publicado. Escritas de 32 bits sao atomicas no ESP32.
static volatile uint32_t histTotal   = 0;   // pontos ja escritos (o ultimo tem id = histUlt)
static volatile uint32_t histUlt     = 0;
static volatile uint32_t rapidoTotal = 0;
static volatile size_t   textoTam    = 0;
static volatile uint32_t linhas      = 0;
static volatile bool     gravando    = false;
static volatile bool     cheio       = false;
static volatile uint32_t periodoMs   = REG_PERIODO_PADRAO_MS;

// Pedidos vindos do painel web (outro nucleo), atendidos no loop
static volatile bool     pedIniciar = false, pedParar = false, pedApagar = false, pedSalvar = false;
static volatile uint32_t pedPeriodo = REG_PERIODO_PADRAO_MS;
static char              mensagem[64] = "";

// Copia para a flash, em pedacos (nao trava o loop)
static bool   salvando   = false;
static File   arqSalvar;
static size_t posSalvar  = 0, fimSalvar = 0;
static bool   ffatOk     = false;
static volatile bool   temSalvo   = false;
static volatile size_t tamSalvo   = 0;

static uint32_t ultimoIdRes  = 0;
static uint32_t ultimoNIna   = 0;
static uint32_t ultimaLinhaMs = 0;

static void *alocar(size_t bytes) {
  void *p = nullptr;
  if (psramFound()) p = ps_malloc(bytes);
  return p;
}

void registroIniciar() {
  psramOk = psramFound();
  if (psramOk) {
    histCap = HIST_PONTOS; rapidoCap = RAPIDO_PONTOS; textoCap = REG_BYTES_MAX;
    hist   = (PontoHist *)alocar(sizeof(PontoHist) * histCap);
    rapido = (PontoRapido *)alocar(sizeof(PontoRapido) * rapidoCap);
    texto  = (char *)alocar(textoCap);
  }
  if (!hist || !rapido || !texto) {
    // Sem PSRAM (placa sem PSRAM ou "PSRAM: Disabled" na IDE): memorias pequenas na RAM interna.
    psramOk = false;
    if (hist) free(hist);
    if (rapido) free(rapido);
    if (texto) free(texto);
    histCap = 300; rapidoCap = 400; textoCap = 32 * 1024;
    hist   = (PontoHist *)malloc(sizeof(PontoHist) * histCap);
    rapido = (PontoRapido *)malloc(sizeof(PontoRapido) * rapidoCap);
    texto  = (char *)malloc(textoCap);
    con().println("AVISO: PSRAM nao encontrada (na IDE: PSRAM = \"OPI PSRAM\"). Historico e registro ficam pequenos.");
  }
  textoTam = 0; linhas = 0;
  ffatOk = FFat.begin(true);  // formata na primeira vez (particao "16M Flash (3MB APP/9.9MB FATFS)")
  if (ffatOk && FFat.exists(REG_ARQ_SALVO)) {
    File f = FFat.open(REG_ARQ_SALVO, "r");
    if (f) { temSalvo = true; tamSalvo = f.size(); f.close(); }
  }
  if (!ffatOk) con().println("AVISO: sistema de arquivos FFat indisponivel (na IDE: Partition Scheme = \"16M Flash (3MB APP/9.9MB FATFS)\"). \"salva\" nao funciona.");
  con().printf("Memorias: historico %lu pontos, rapido %lu pontos, registro CSV %u kB (%s)\n",
                (unsigned long)histCap, (unsigned long)rapidoCap, (unsigned)(textoCap / 1024), psramOk ? "PSRAM" : "RAM interna");
}

bool registroPsramOk() { return psramOk; }

// ---------------- escrita (so no loop) ----------------
static void anexarTexto(const char *s, size_t n) {
  size_t t = textoTam;
  if (t + n + 1 > textoCap) {  // nao cabe: para de gravar
    if (gravando) { gravando = false; cheio = true; snprintf(mensagem, sizeof mensagem, "registro cheio (%u kB): gravacao parada", (unsigned)(textoCap / 1024)); }
    return;
  }
  memcpy(texto + t, s, n);
  __sync_synchronize();        // o texto fica pronto antes de publicar o novo tamanho
  textoTam = t + n;
}

static void anexarLinha(const String &s) {
  anexarTexto(s.c_str(), s.length());
  anexarTexto("\n", 1);
}

static void iniciarGravacao(uint32_t ms) {
  if (ms < LOG_MIN_MS) ms = LOG_MIN_MS;
  if (ms > LOG_MAX_MS) ms = LOG_MAX_MS;
  periodoMs = ms;
  cheio = false;
  if (textoTam == 0) { anexarTexto(csvCabecalho(), strlen(csvCabecalho())); anexarTexto("\n", 1); }
  gravando = true;
  ultimaLinhaMs = millis() - ms;  // a primeira linha sai na hora
  snprintf(mensagem, sizeof mensagem, "gravando: uma linha a cada %lu ms", (unsigned long)ms);
}

static void apagar() {
  gravando = false; cheio = false;
  textoTam = 0; linhas = 0;
  snprintf(mensagem, sizeof mensagem, "registro apagado");
}

static void passoSalvar() {
  if (!salvando) return;
  if (saidaLigada()) {  // alguem ligou a saida no meio: aborta (o arquivo fica incompleto)
    arqSalvar.close(); salvando = false;
    snprintf(mensagem, sizeof mensagem, "salvamento interrompido: saida ligada");
    return;
  }
  size_t n = fimSalvar - posSalvar; if (n > 4096) n = 4096;   // 4 kB por volta do loop
  if (n > 0) { arqSalvar.write((const uint8_t *)texto + posSalvar, n); posSalvar += n; }
  if (posSalvar >= fimSalvar) {
    arqSalvar.close(); salvando = false;
    File f = FFat.open(REG_ARQ_SALVO, "r");
    if (f) { tamSalvo = f.size(); f.close(); temSalvo = true; }
    snprintf(mensagem, sizeof mensagem, "salvo na flash: %u bytes", (unsigned)tamSalvo);
  }
}

static void comecarSalvar() {
  if (!ffatOk) { snprintf(mensagem, sizeof mensagem, "flash sem sistema de arquivos: veja o Partition Scheme"); return; }
  if (saidaLigada()) { snprintf(mensagem, sizeof mensagem, "desligue a saida antes de salvar"); return; }
  if (salvando) return;
  arqSalvar = FFat.open(REG_ARQ_SALVO, "w");
  if (!arqSalvar) { snprintf(mensagem, sizeof mensagem, "erro ao criar o arquivo na flash"); return; }
  posSalvar = 0; fimSalvar = textoTam; salvando = true;
  snprintf(mensagem, sizeof mensagem, "salvando %u bytes na flash...", (unsigned)fimSalvar);
}

void registroAtualizar() {
  // 1) Pedidos do painel web ou da serial
  if (pedApagar) { pedApagar = false; if (!salvando) apagar(); }
  if (pedParar)  { pedParar = false; if (gravando) { gravando = false; snprintf(mensagem, sizeof mensagem, "gravacao parada: %lu linhas", (unsigned long)linhas); } }
  if (pedIniciar){ pedIniciar = false; iniciarGravacao(pedPeriodo); }
  if (pedSalvar) { pedSalvar = false; comecarSalvar(); }
  passoSalvar();

  // 2) Historico: um ponto por janela de 1 s
  const Resultados &r = medicaoResultados();
  if (r.id != ultimoIdRes && hist) {
    ultimoIdRes = r.id;
    Modo m = modoAtual();
    ScrResumo sr = scrResumo();
    PontoHist p;
    p.id = r.id; p.tms = millis(); p.modo = (uint8_t)m; p.ligado = saidaLigada() ? 1 : 0;
    p.d = modoConversor(m) ? pwmDAtual() : NAN;
    p.alfa = modoScr(m) ? (sr.armado ? sr.alfaAtual : 180.0f) : NAN;
    p.vin = r.vin; p.iin = r.iin; p.vo = r.vo; p.io = r.io; p.pin = r.pin; p.pout = r.pout; p.eta = r.eta;
    p.voMed = r.voMed; p.voRms = r.voRms; p.ioMed = r.ioMed; p.ioRms = r.ioRms; p.etaRet = r.etaRet;
    p.fRede = sr.sincronizado ? sr.freqRede : 0.0f;
    hist[histTotal % histCap] = p;
    __sync_synchronize();
    histUlt = p.id;
    histTotal = histTotal + 1;
  }

  // 3) Rapido: cada leitura dos INA226 (~50 ms)
  uint32_t nIna = medicaoInaContador();
  if (nIna != ultimoNIna && rapido) {
    ultimoNIna = nIna;
    const LeituraIna &a = medicaoIna(0), &b = medicaoIna(1);
    PontoRapido q;
    q.n = nIna; q.tms = millis();
    q.vin = a.ok ? a.v : NAN; q.iin = a.ok ? a.i : NAN;
    q.vo = b.ok ? b.v : NAN;  q.io = b.ok ? b.i : NAN;
    rapido[rapidoTotal % rapidoCap] = q;
    __sync_synchronize();
    rapidoTotal = rapidoTotal + 1;
  }

  // 4) Registro CSV
  if (gravando && !salvando) {
    uint32_t agora = millis();
    if (agora - ultimaLinhaMs >= periodoMs) {
      ultimaLinhaMs += periodoMs;
      if (agora - ultimaLinhaMs >= periodoMs) ultimaLinhaMs = agora;
      size_t antes = textoTam;
      anexarLinha(csvLinha());
      if (textoTam != antes) linhas = linhas + 1;
    }
  }
}

// ---------------- consultas ----------------
bool     registroGravando()   { return gravando; }
uint32_t registroPeriodoMs()  { return periodoMs; }
uint32_t registroLinhas()     { return linhas; }
size_t   registroBytes()      { return textoTam; }
size_t   registroCapacidade() { return textoCap; }
bool     registroCheio()      { return cheio; }
bool     registroSalvando()   { return salvando; }
bool     registroTemSalvo()   { return temSalvo; }
size_t   registroTamanhoSalvo() { return tamSalvo; }
const char *registroMensagem() { return mensagem; }

void registroPedirInicio(uint32_t ms) { pedPeriodo = ms; pedIniciar = true; }
void registroPedirParada()  { pedParar = true; }
void registroPedirApagar()  { pedApagar = true; }
bool registroPedirSalvar()  { if (saidaLigada()) return false; pedSalvar = true; return true; }

size_t registroLerTexto(size_t pos, char *dst, size_t max) {
  size_t tam = textoTam;          // so le o que ja foi publicado
  if (pos >= tam) return 0;
  size_t n = tam - pos; if (n > max) n = max;
  memcpy(dst, texto + pos, n);
  return n;
}

uint32_t histUltimoId() { return histTotal ? histUlt : 0; }

uint32_t histCopiar(uint32_t desdeId, PontoHist *dst, uint32_t max) {
  uint32_t total = histTotal;
  if (total == 0 || !hist) return 0;
  uint32_t primeiro = (total > histCap) ? total - histCap + 4 : 0;  // 4 de folga: o escritor pode estar no proximo
  uint32_t n = 0;
  for (uint32_t k = primeiro; k < total && n < max; k++) {
    PontoHist p = hist[k % histCap];
    if (p.id <= desdeId) continue;
    dst[n++] = p;
  }
  // Se o escritor deu a volta durante a copia, os primeiros podem ter sido sobrescritos: descarta-os.
  uint32_t agora = histTotal;
  if (agora > histCap && agora - histCap > primeiro) {
    uint32_t perdidos = agora - histCap - primeiro;
    if (perdidos >= n) return 0;
    memmove(dst, dst + perdidos, sizeof(PontoHist) * (n - perdidos));
    n -= perdidos;
  }
  return n;
}

uint32_t rapidoUltimoN() { return rapidoTotal ? rapido[(rapidoTotal - 1) % rapidoCap].n : 0; }

uint32_t rapidoCopiar(uint32_t desdeN, PontoRapido *dst, uint32_t max) {
  uint32_t total = rapidoTotal;
  if (total == 0 || !rapido) return 0;
  uint32_t primeiro = (total > rapidoCap) ? total - rapidoCap + 4 : 0;
  uint32_t n = 0;
  for (uint32_t k = primeiro; k < total && n < max; k++) {
    PontoRapido q = rapido[k % rapidoCap];
    if (q.n <= desdeN) continue;
    dst[n++] = q;
  }
  uint32_t agora = rapidoTotal;
  if (agora > rapidoCap && agora - rapidoCap > primeiro) {
    uint32_t perdidos = agora - rapidoCap - primeiro;
    if (perdidos >= n) return 0;
    memmove(dst, dst + perdidos, sizeof(PontoRapido) * (n - perdidos));
    n -= perdidos;
  }
  return n;
}
