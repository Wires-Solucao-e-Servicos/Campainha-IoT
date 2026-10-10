// Botões de uma porta do estacionamento (NodeMCU / ESP8266).
// O mesmo código vai nas 2 NodeMCU: o número da porta vem do ambiente escolhido
// no platformio.ini (porta1 ou porta2).
//
// Cada porta tem um botão de entrada e um de saída, ligados entre o pino e o GND.
// Qualquer um dos dois chama
//   GET http://IP_DA_CENTRAL/tocar?origem=portaN-entrada   (ou -saida)
// e a central toca o buzzer e responde "ok". Se a central não responder (Wi-Fi caiu,
// central reiniciando...), continua tentando até PRAZO_AVISO_MS.
//
// O IP da central não é configurado: a NodeMCU manda "CAMPAINHA?" em broadcast UDP
// (255.255.255.255) e usa o IP de quem responder "CAMPAINHA!". Ela procura de novo a cada
// 5 min e sempre que a central não responde "ok" no IP guardado (o IP mudou).
//
// Se a porta e a central acabarem em faixas de IP diferentes (dois aparelhos entregando IP
// na mesma rede, por exemplo), o HTTP não chega. Nesse caso o aviso vai em broadcast:
// "TOCAR <id> <origem>", e a central confirma com "TOCADO <id>", também em broadcast.
//
// O Wi-Fi e o modo de IP (automático ou fixo, com gateway e máscara) ficam guardados na
// flash (EEPROM) e podem ser trocados sem cabo:
//   - pela página da porta (Rede);
//   - pela central, só o Wi-Fi: ela pergunta "CAMPAINHA-PORTAS?" na rede, a porta responde
//     "PORTA N" e a central manda o Wi-Fi novo em POST /salvar-wifi (usuário "admin", senha
//     SENHA_ADMIN);
//   - pela rede de socorro "Campainha-PortaN" (senha SENHA_ADMIN, página em http://192.168.4.1),
//     que fica ligada nos 3 primeiros minutos depois de ligar a porta e sempre que ela ficar
//     mais de 30 s sem Wi-Fi. Serve para corrigir um Wi-Fi ou IP fixo errado.
//
// A página (http://campainha-portaN.local) também mostra o estado dos botões, simula
// apertos, reinicia a placa e atualiza o firmware.
//
// LED: piscando devagar = sem Wi-Fi | piscando rápido = rede de socorro ligada |
//      aceso = aviso entregue | 5 piscadas rápidas = falhou

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <EEPROM.h>
#include <Updater.h>
#include "config.h"

#ifndef NUMERO_PORTA
#error "NUMERO_PORTA não definido: grave usando um dos ambientes porta1 ou porta2"
#endif

// A rede de socorro usa essa senha, e o Wi-Fi só aceita senhas de 8 a 63 caracteres.
static_assert(sizeof(SENHA_ADMIN) - 1 >= 8 && sizeof(SENHA_ADMIN) - 1 <= 63,
              "SENHA_ADMIN precisa ter de 8 a 63 caracteres");

#define TEXTO(x) #x
#define TEXTO_MACRO(x) TEXTO(x)

// Marca gravada no firmware. A atualização pela página só aceita um arquivo com a marca
// desta placa, para não gravar por engano o firmware da outra porta ou da central.
const char MARCA_FIRMWARE[] = "<CAMPAINHA-FW:PORTA" TEXTO_MACRO(NUMERO_PORTA) ">";

const unsigned long AP_DEPOIS_DE_LIGAR_MS = 180000;      // rede de socorro aberta nos 3 primeiros minutos
const unsigned long AP_SEM_WIFI_MS = 30000;              // e quando ficar este tempo sem Wi-Fi
const unsigned long REINICIAR_SEM_WIFI_MS = 600000;      // reinicia se ficar 10 min sem Wi-Fi (e ninguém configurando)
const unsigned long USO_PAGINA_MS = 180000;              // abriu a página há menos que isso = está configurando
const unsigned long INTERVALO_PROCURA_MS = 300000;       // confere a cada 5 min se a central continua no mesmo IP
const unsigned long RECONECTAR_SEM_CENTRAL_MS = 600000;  // 10 min sem resposta da central: reconecta e pede IP de novo
const unsigned long BOTAO_TRAVADO_MS = 10000;            // apertado por mais que isso = fio em curto ou botão travado

// Mensagens UDP: estes valores têm que ser iguais no projeto central-esp01.
const uint16_t PORTA_DESCOBERTA = 4210;
const char PERGUNTA_DESCOBERTA[] = "CAMPAINHA?";
const char RESPOSTA_DESCOBERTA[] = "CAMPAINHA!";
const char PERGUNTA_PORTAS[] = "CAMPAINHA-PORTAS?";
const char RESPOSTA_PORTA[] = "PORTA ";       // seguido do número da porta
const char PEDIDO_TOQUE[] = "TOCAR ";         // seguido de "<id> <origem>"
const char CONFIRMACAO_TOQUE[] = "TOCADO ";   // seguido de "<id>"
// Broadcast geral: chega a todos no mesmo Wi-Fi, seja qual for a faixa de IP de cada um.
const IPAddress BROADCAST_GERAL(255, 255, 255, 255);

struct Config {
  uint32_t assinatura;
  uint32_t idFabrica;  // identifica os valores de fábrica do config.h que estavam valendo
  char ssid[33];
  char senha[65];
  // Campos acrescentados na versão com IP fixo (a anterior só tinha os de cima).
  uint8_t dhcp;        // 1 = IP automático, 0 = usa o IP fixo abaixo
  uint32_t ip, gateway, mascara;
};

const uint32_t ASSINATURA = 0xCA3B0002;
const uint32_t ASSINATURA_SO_WIFI = 0xCA3B0001;  // configuração salva pela versão anterior

Config cfg;
char nome[24];    // "campainha-portaN": nome na rede (http://campainha-portaN.local)
char nomeAP[24];  // "Campainha-PortaN": rede de socorro

ESP8266WebServer servidor(80);
WiFiUDP udp;
IPAddress ipCentral;
bool centralConhecida = false;
unsigned long ultimaProcura = 0;
unsigned long centralRespondeuEm = 0;  // última vez que a central respondeu (procura ou aviso)
unsigned long reconexoes = 0;          // reconexões por falta de resposta da central
unsigned long ultimaReconexao = 0;

struct Botao {
  uint8_t pino;
  const char* nome;
  bool estado;         // HIGH = solto (pull-up), LOW = apertado
  bool ultimaLeitura;
  unsigned long leituraMudouEm;
  unsigned long apertos;        // desde que a placa ligou
  unsigned long apertadoEm;     // quando foi o último aperto
};

Botao botoes[] = {
  {PINO_BOTAO_ENTRADA, "entrada", HIGH, HIGH, 0, 0, 0},
  {PINO_BOTAO_SAIDA, "saida", HIGH, HIGH, 0, 0, 0},
};

// Aviso esperando para ser entregue à central
bool avisoPendente = false;
const char* origemPendente = "";
unsigned long pendenteDesde = 0;
unsigned long ultimaTentativa = 0;

bool jaAvisou = false;
unsigned long ultimoAviso = 0;

// Resultado da última tentativa de avisar a central, para a página
bool houveEnvio = false;
bool ultimoEnvioOk = false;
unsigned long ultimoEnvioMs = 0;
String ultimoEnvioTexto;

bool estavaConectado = false;
unsigned long semWifiDesde = 0;
bool apLigado = false;
bool janelaInicialAcabou = false;
bool buscaPausada = false;
bool acessouPagina = false;
unsigned long ultimoAcessoPagina = 0;

bool reiniciarPedido = false;
unsigned long reiniciarPedidoEm = 0;

// ===== Configuração salva =====

// Muda sempre que o Wi-Fi do config.h muda. Assim, gravar o firmware com outro
// Wi-Fi no config.h faz esse Wi-Fi valer de novo.
uint32_t idDaFabrica() {
  const char* texto = WIFI_SSID "|" WIFI_SENHA;
  uint32_t h = 2166136261u;
  for (const char* c = texto; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
  return h;
}

// Máscara válida tem todos os bits 1 seguidos e depois só 0 (ex: 255.255.255.0).
bool mascaraValida(uint32_t mascara) {
  uint32_t zeros = ~__builtin_bswap32(mascara);  // IPAddress guarda os bytes na ordem da rede
  return mascara != 0 && (zeros & (zeros + 1)) == 0;
}

// O IP tem que estar na mesma faixa do gateway, senão a porta fica fora da rede.
bool redeValida(uint32_t ip, uint32_t gateway, uint32_t mascara) {
  return mascaraValida(mascara) && ip != gateway &&
         (ip & mascara) == (gateway & mascara) &&
         (ip & ~mascara) != 0 && (ip | mascara) != 0xFFFFFFFF;  // nem endereço da rede, nem broadcast
}

// De fábrica a porta usa IP automático; o IP fixo só existe se for configurado pela página.
void carregarFabrica() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.assinatura = ASSINATURA;
  cfg.idFabrica = idDaFabrica();
  strlcpy(cfg.ssid, WIFI_SSID, sizeof(cfg.ssid));
  strlcpy(cfg.senha, WIFI_SENHA, sizeof(cfg.senha));
  cfg.dhcp = 1;
}

void salvarConfig() {
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

void carregarConfig() {
  EEPROM.get(0, cfg);
  cfg.ssid[sizeof(cfg.ssid) - 1] = 0;
  cfg.senha[sizeof(cfg.senha) - 1] = 0;

  // Configuração da versão anterior: mantém o Wi-Fi (pode ter vindo da central) e usa IP automático.
  if (cfg.assinatura == ASSINATURA_SO_WIFI && cfg.idFabrica == idDaFabrica() && cfg.ssid[0]) {
    cfg.assinatura = ASSINATURA;
    cfg.dhcp = 1;
    cfg.ip = cfg.gateway = cfg.mascara = 0;
    salvarConfig();
    return;
  }

  bool valida = cfg.assinatura == ASSINATURA && cfg.idFabrica == idDaFabrica() && cfg.ssid[0] &&
                cfg.dhcp <= 1 && (cfg.dhcp || redeValida(cfg.ip, cfg.gateway, cfg.mascara));
  if (!valida) {
    Serial.println("Usando o Wi-Fi de fábrica do config.h, com IP automático");
    carregarFabrica();
  }
}

// ===== LED e botões =====

void led(bool aceso) {
  if (PINO_LED < 0) return;
  bool nivel = LED_ATIVO_EM_LOW ? !aceso : aceso;
  digitalWrite(PINO_LED, nivel ? HIGH : LOW);
}

void piscarErro() {
  for (int i = 0; i < 5; i++) {
    led(true);
    delay(80);
    led(false);
    delay(80);
  }
}

// Retorna true uma única vez a cada aperto, já sem o ruído do contato.
bool foiApertado(Botao& b) {
  bool leitura = digitalRead(b.pino);
  if (leitura != b.ultimaLeitura) {
    b.ultimaLeitura = leitura;
    b.leituraMudouEm = millis();
  }
  if (leitura != b.estado && millis() - b.leituraMudouEm >= DEBOUNCE_MS) {
    b.estado = leitura;
    if (b.estado == LOW) {
      b.apertos++;
      b.apertadoEm = millis();
      return true;
    }
  }
  return false;
}

// ===== Mensagens UDP =====

bool naMinhaFaixa(IPAddress ip) {
  uint32_t mascara = WiFi.subnetMask();
  return (uint32_t(ip) & mascara) == (uint32_t(WiFi.localIP()) & mascara);
}

// Responde a quem mandou o pacote UDP atual. Se ele estiver em outra faixa de IP, a resposta
// direta iria para o roteador e se perderia; em broadcast ela chega, porque o Wi-Fi é o mesmo.
void responderUdp(const String& texto) {
  IPAddress destino = naMinhaFaixa(udp.remoteIP()) ? udp.remoteIP() : BROADCAST_GERAL;
  udp.beginPacket(destino, udp.remotePort());
  udp.print(texto);
  udp.endPacket();
}

// Lê o pacote UDP que acabou de chegar (depois de parsePacket) como texto.
void lerPacote(char* msg, size_t tamanho) {
  int n = udp.read(msg, tamanho - 1);
  msg[n > 0 ? n : 0] = 0;
}

// Mensagens que podem chegar a qualquer hora, inclusive no meio de uma procura.
void tratarPacoteUdp(const char* msg) {
  if (strcmp(msg, PERGUNTA_PORTAS) == 0) responderUdp(RESPOSTA_PORTA + String(NUMERO_PORTA));
}

void responderCentral() {
  if (udp.parsePacket() <= 0) return;
  char msg[64];
  lerPacote(msg, sizeof(msg));
  tratarPacoteUdp(msg);
}

// ===== Central =====

// Pergunta na rede (broadcast) onde está a central e guarda o IP de quem responder.
bool procurarCentral() {
  ultimaProcura = millis();
  while (udp.parsePacket() > 0) {}  // descarta respostas antigas

  for (int tentativa = 0; tentativa < 3; tentativa++) {
    // Pelo broadcast da própria faixa (caminho certo mesmo com a rede de socorro ligada) e
    // pelo geral (alcança a central se ela estiver em outra faixa de IP).
    for (IPAddress destino : {WiFi.broadcastIP(), BROADCAST_GERAL}) {
      udp.beginPacket(destino, PORTA_DESCOBERTA);
      udp.write(PERGUNTA_DESCOBERTA);
      udp.endPacket();
    }

    unsigned long inicio = millis();
    while (millis() - inicio < 300) {
      if (udp.parsePacket() > 0) {
        char msg[64];
        lerPacote(msg, sizeof(msg));
        if (strcmp(msg, RESPOSTA_DESCOBERTA) == 0) {
          IPAddress ip = udp.remoteIP();
          if (!centralConhecida || ip != ipCentral) {
            Serial.printf("Central encontrada em %s%s\n", ip.toString().c_str(),
                          naMinhaFaixa(ip) ? "" : " (outra faixa de IP: avisos vão por broadcast)");
          }
          ipCentral = ip;
          centralConhecida = true;
          centralRespondeuEm = millis();
          return true;
        }
        tratarPacoteUdp(msg);
      }
      delay(5);
    }
  }
  Serial.println("Central não respondeu à procura");
  return false;
}

void registrarEnvio(bool ok, const String& texto) {
  houveEnvio = true;
  ultimoEnvioOk = ok;
  ultimoEnvioMs = millis();
  ultimoEnvioTexto = texto;
}

// Caminho normal: central na mesma faixa de IP.
bool avisarPorHttp(const String& origemCompleta) {
  WiFiClient cliente;
  HTTPClient http;
  http.setTimeout(TIMEOUT_HTTP_MS);  // no ESP8266 vale para a conexão e para a resposta
  http.begin(cliente, "http://" + ipCentral.toString() + "/tocar?origem=" + origemCompleta);
  int codigo = http.GET();
  String corpo = codigo == HTTP_CODE_OK ? http.getString() : String();
  http.end();

  // Confere a resposta: outro aparelho que ficou com o IP antigo da central poderia
  // responder 200 sem tocar nada, e a porta nunca mais procuraria a central certa.
  if (codigo == HTTP_CODE_OK && corpo == "ok") {
    registrarEnvio(true, origemCompleta + " entregue à central " + ipCentral.toString());
    return true;
  }

  String erro;
  if (codigo < 0) {
    erro = HTTPClient::errorToString(codigo);
  } else if (codigo == HTTP_CODE_OK) {
    erro = F("quem respondeu não é a central");
  } else {
    erro = "resposta HTTP " + String(codigo);
  }
  Serial.printf("Central não respondeu em %s: %s\n", ipCentral.toString().c_str(), erro.c_str());
  registrarEnvio(false, origemCompleta + ": falhou em " + ipCentral.toString() + " (" + erro + ")");
  return false;
}

// Central em outra faixa de IP: o HTTP não chegaria nela, mas o broadcast chega.
bool avisarPorBroadcast(const String& origemCompleta) {
  uint32_t id = ESP.random();
  String pedido = PEDIDO_TOQUE + String(id) + " " + origemCompleta;
  String confirmacao = CONFIRMACAO_TOQUE + String(id);
  while (udp.parsePacket() > 0) {}  // descarta pacotes antigos

  for (int tentativa = 0; tentativa < 3; tentativa++) {
    udp.beginPacket(BROADCAST_GERAL, PORTA_DESCOBERTA);
    udp.print(pedido);
    udp.endPacket();

    unsigned long inicio = millis();
    while (millis() - inicio < 300) {
      if (udp.parsePacket() > 0) {
        char msg[64];
        lerPacote(msg, sizeof(msg));
        if (confirmacao == msg) {
          registrarEnvio(true, origemCompleta + " entregue à central " + ipCentral.toString() +
                                   " por broadcast (ela está em outra faixa de IP)");
          return true;
        }
        tratarPacoteUdp(msg);
      }
      delay(5);
    }
  }
  Serial.printf("Central em outra faixa (%s) não confirmou o aviso por broadcast\n", ipCentral.toString().c_str());
  registrarEnvio(false, origemCompleta + ": a central " + ipCentral.toString() +
                            " (outra faixa de IP) não confirmou o aviso por broadcast");
  return false;
}

bool avisarCentral(const char* origem) {
  String origemCompleta = "porta" + String(NUMERO_PORTA) + "-" + origem;
  if (!centralConhecida && !procurarCentral()) {
    registrarEnvio(false, origemCompleta + ": a central não respondeu à procura na rede");
    return false;
  }

  bool ok = naMinhaFaixa(ipCentral) ? avisarPorHttp(origemCompleta) : avisarPorBroadcast(origemCompleta);
  if (ok) {
    centralRespondeuEm = millis();
  } else {
    centralConhecida = false;  // o IP pode ter mudado: na próxima tentativa procura de novo
  }
  return ok;
}

// ===== Wi-Fi =====

void conectarNaRede() {
  if (cfg.dhcp) {
    WiFi.config(0U, 0U, 0U);  // tudo zero = pedir IP ao roteador (DHCP)
  } else {
    WiFi.config(IPAddress(cfg.ip), IPAddress(cfg.gateway), IPAddress(cfg.mascara));
  }
  WiFi.begin(cfg.ssid, cfg.senha);
}

void gerenciarWifi(unsigned long agora) {
  bool conectado = WiFi.status() == WL_CONNECTED;
  bool acabouDeConectar = conectado && !estavaConectado;
  if (!conectado && estavaConectado) Serial.println("Wi-Fi caiu");
  estavaConectado = conectado;

  if (acabouDeConectar) {
    Serial.printf("Wi-Fi conectado em \"%s\", IP %s, gateway %s, sinal %d dBm\n", cfg.ssid,
                  WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(), WiFi.RSSI());
    // Já procura a central agora, para o primeiro aperto não ter que esperar a procura.
    centralConhecida = false;
    procurarCentral();
  }

  if (conectado) {
    semWifiDesde = 0;
  } else if (semWifiDesde == 0) {
    semWifiDesde = agora;
  }
  bool muitoTempoSemWifi = !conectado && agora - semWifiDesde > AP_SEM_WIFI_MS;
  if (agora > AP_DEPOIS_DE_LIGAR_MS) janelaInicialAcabou = true;

  // Alguém está configurando: tem aparelho na rede de socorro e a página foi aberta há
  // pouco. Só estar conectado não conta: um celular que lembrou a rede de socorro e
  // entrou sozinho nela não pode impedir a volta para o roteador.
  uint8_t clientesAP = apLigado ? WiFi.softAPgetStationNum() : 0;
  bool configurando = clientesAP > 0 && acessouPagina && millis() - ultimoAcessoPagina < USO_PAGINA_MS;

  // Rede de socorro: nos primeiros minutos (para corrigir um IP fixo errado, que conecta no
  // Wi-Fi mas deixa a porta inacessível), sem Wi-Fi, e enquanto alguém estiver configurando.
  bool precisaAP = !janelaInicialAcabou || muitoTempoSemWifi || configurando;
  if (precisaAP && !apLigado) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(nomeAP, SENHA_ADMIN);
    apLigado = true;
    Serial.printf("Rede de socorro \"%s\" ligada (http://%s)\n", nomeAP, WiFi.softAPIP().toString().c_str());
  } else if (!precisaAP && apLigado) {
    WiFi.softAPdisconnect(true);
    apLigado = false;
    Serial.println("Rede de socorro desligada");
  }

  // Enquanto procura o Wi-Fi, o ESP fica trocando de canal e a rede de socorro fica
  // instável. Então, enquanto alguém está configurando e o Wi-Fi não conecta, para de procurar.
  bool deveProcurar = conectado || !configurando;
  if (!deveProcurar && !buscaPausada) {
    WiFi.disconnect();
    buscaPausada = true;
  } else if (deveProcurar && buscaPausada) {
    conectarNaRede();
    buscaPausada = false;
  }

  if (!conectado && !configurando && agora - semWifiDesde > REINICIAR_SEM_WIFI_MS) {
    Serial.println("Muito tempo sem Wi-Fi, reiniciando...");
    ESP.restart();
  }
}

// ===== Atualização de firmware =====

bool atualizacaoAutorizada = false;
bool atualizacaoIniciada = false;
String erroAtualizacao;
size_t marcaCasada = 0;  // quantos caracteres da marca apareceram em sequência no arquivo
bool marcaEncontrada = false;

// Procura MARCA_FIRMWARE no arquivo, pedaço por pedaço. Como o '<' só aparece no começo
// da marca, basta recomeçar a contagem quando um caractere não bate.
void procurarMarca(const uint8_t* dados, size_t tamanho) {
  const size_t tamMarca = sizeof(MARCA_FIRMWARE) - 1;
  for (size_t i = 0; i < tamanho && !marcaEncontrada; i++) {
    if (dados[i] == (uint8_t)MARCA_FIRMWARE[marcaCasada]) {
      marcaEncontrada = ++marcaCasada == tamMarca;
    } else {
      marcaCasada = dados[i] == (uint8_t)MARCA_FIRMWARE[0] ? 1 : 0;
    }
  }
}

// Recebe o arquivo em pedaços enquanto ele chega. A página com o resultado é enviada
// depois, por tratarAtualizacao().
void receberFirmware() {
  HTTPUpload& envio = servidor.upload();
  if (envio.status == UPLOAD_FILE_START) {
    atualizacaoAutorizada = servidor.authenticate("admin", SENHA_ADMIN);
    atualizacaoIniciada = false;
    erroAtualizacao = "";
    marcaCasada = 0;
    marcaEncontrada = false;
    if (!atualizacaoAutorizada) return;
    Serial.printf("Recebendo firmware \"%s\"...\n", envio.filename.c_str());
    uint32_t espaco = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    atualizacaoIniciada = Update.begin(espaco);
    if (!atualizacaoIniciada) erroAtualizacao = Update.getErrorString();
  } else if (envio.status == UPLOAD_FILE_WRITE) {
    if (!atualizacaoIniciada || erroAtualizacao.length()) return;
    procurarMarca(envio.buf, envio.currentSize);
    if (Update.write(envio.buf, envio.currentSize) != envio.currentSize) {
      erroAtualizacao = Update.getErrorString();
    }
  } else if (envio.status == UPLOAD_FILE_END) {
    if (!atualizacaoIniciada || erroAtualizacao.length()) return;
    if (!marcaEncontrada) {
      erroAtualizacao = "esse arquivo não é o firmware da porta " + String(NUMERO_PORTA) +
                        ". Use o firmware.bin da pasta .pio/build/porta" + String(NUMERO_PORTA) + ".";
    } else if (!Update.end(true)) {
      erroAtualizacao = Update.getErrorString();
    } else {
      Serial.printf("Firmware gravado (%u bytes)\n", (unsigned)envio.totalSize);
    }
  } else if (envio.status == UPLOAD_FILE_ABORTED) {
    erroAtualizacao = F("o envio do arquivo foi interrompido");
  }
}

// ===== Página =====

bool autorizado() {
  acessouPagina = true;
  ultimoAcessoPagina = millis();
  if (servidor.authenticate("admin", SENHA_ADMIN)) return true;
  servidor.requestAuthentication(BASIC_AUTH, "Campainha");
  return false;
}

String escapar(const String& texto) {
  String r;
  r.reserve(texto.length() + 8);
  for (size_t i = 0; i < texto.length(); i++) {
    char c = texto[i];
    switch (c) {
      case '&': r += F("&amp;"); break;
      case '<': r += F("&lt;"); break;
      case '>': r += F("&gt;"); break;
      case '"': r += F("&quot;"); break;
      case '\'': r += F("&#39;"); break;
      default: r += c;
    }
  }
  return r;
}

String haQuanto(unsigned long desdeMs) {
  unsigned long s = (millis() - desdeMs) / 1000;
  if (s < 120) return "há " + String(s) + " s";
  if (s < 7200) return "há " + String(s / 60) + " min";
  if (s < 172800) return "há " + String(s / 3600) + " h";
  return "há " + String(s / 86400) + " dias";
}

String motivoDoReinicio() {
  switch (ESP.getResetInfoPtr()->reason) {
    case REASON_DEFAULT_RST: return F("ligou na energia");
    case REASON_WDT_RST: return F("<b>travou</b> (watchdog de hardware)");
    case REASON_EXCEPTION_RST: return F("<b>erro no programa</b> (exceção)");
    case REASON_SOFT_WDT_RST: return F("<b>travou</b> (watchdog de software)");
    case REASON_SOFT_RESTART: return F("reiniciada pelo programa (página, Wi-Fi novo, atualização ou falta de Wi-Fi)");
    case REASON_EXT_SYS_RST: return F("botão RST ou queda rápida de energia");
    default: return ESP.getResetReason();
  }
}

const char ESTILO[] PROGMEM =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Campainha</title><style>"
    "body{font-family:sans-serif;max-width:420px;margin:16px auto;padding:0 16px}"
    "fieldset{border:1px solid #ccc;border-radius:6px;margin:16px 0}"
    "label{display:block;margin-top:8px}"
    "input{display:block;width:100%;box-sizing:border-box;padding:6px;font-size:1em}"
    "input[type=checkbox]{display:inline;width:auto}"
    "input:disabled{color:#999}"
    "button{font-size:1em;padding:8px 16px;margin-top:12px;margin-right:8px}"
    ".aviso{background:#fff3cd;padding:8px;border-radius:6px}"
    "</style></head><body>";

void enviarPagina(const String& aviso = String(), int codigo = 200) {
  unsigned long agora = millis();
  bool conectado = WiFi.status() == WL_CONNECTED;
  String p;
  p.reserve(5632);
  p += FPSTR(ESTILO);
  p += "<h2>Campainha - porta " + String(NUMERO_PORTA) + "</h2>";
  if (aviso.length()) p += "<p class='aviso'>" + aviso + "</p>";

  p += F("<fieldset><legend>Situação</legend><p>");
  if (conectado) {
    p += "Wi-Fi: " + escapar(cfg.ssid) + " (" + String(WiFi.RSSI()) + " dBm)";
    p += "<br>IP: " + WiFi.localIP().toString() + (cfg.dhcp ? " (automático)" : " (fixo)");
    p += "<br>Gateway: " + WiFi.gatewayIP().toString() + " &middot; Máscara: " + WiFi.subnetMask().toString();
  } else {
    p += "Wi-Fi: sem conexão com \"" + escapar(cfg.ssid) + "\"";
  }
  if (centralConhecida) {
    p += "<br>Central: " + ipCentral.toString();
    if (conectado && !naMinhaFaixa(ipCentral)) {
      p += F(" <b>(em outra faixa de IP: os avisos vão por broadcast. "
             "Tem mais de um aparelho entregando IP na rede?)</b>");
    }
  } else {
    p += F("<br>Central: <b>não encontrada</b>");
  }
  if (centralRespondeuEm) p += "<br>Última resposta da central: " + haQuanto(centralRespondeuEm);
  if (reconexoes) {
    p += "<br>Reconexões por falta de resposta da central: " + String(reconexoes) + ", a última " +
         haQuanto(ultimaReconexao);
  }
  if (apLigado) p += "<br>Rede de socorro \"" + String(nomeAP) + "\" ligada";
  p += "<br>Ligada " + haQuanto(0) + "<br>Último reinício: " + motivoDoReinicio();
  p += "<br>Memória livre: " + String(ESP.getFreeHeap()) + " bytes";
  p += F("<br>Firmware: " __DATE__ " " __TIME__ "</p></fieldset>");

  p += F("<fieldset><legend>Botões</legend><p>");
  for (Botao& b : botoes) {
    p += "<b>" + String(b.nome) + "</b> (GPIO" + String(b.pino) + "): ";
    if (b.estado == LOW && agora - b.apertadoEm > BOTAO_TRAVADO_MS) {
      p += "<b>apertado " + haQuanto(b.apertadoEm) + ": fio em curto ou botão travado?</b>";
    } else {
      p += b.estado == LOW ? F("apertado") : F("solto");
    }
    p += "<br>" + String(b.apertos) + " apertos desde que ligou";
    if (b.apertos) p += ", o último " + haQuanto(b.apertadoEm);
    p += F("<br><br>");
  }
  p += F("Último aviso à central: ");
  if (houveEnvio) {
    p += String(ultimoEnvioOk ? F("ok, ") : F("<b>falhou</b>, ")) + escapar(ultimoEnvioTexto) + ", " + haQuanto(ultimoEnvioMs);
  } else {
    p += F("nenhum desde que ligou");
  }
  p += F("</p><form method='post' action='/simular'>"
         "<button name='botao' value='entrada'>Simular entrada</button>"
         "<button name='botao' value='saida'>Simular saída</button></form></fieldset>");

  // No modo automático, os campos de IP fixo já vêm com o IP atual, para facilitar
  // se quiser fixar a porta no IP que ela está usando.
  bool mostrarAtual = cfg.dhcp && conectado;
  IPAddress ip = mostrarAtual ? WiFi.localIP() : IPAddress(cfg.ip);
  IPAddress gateway = mostrarAtual ? WiFi.gatewayIP() : IPAddress(cfg.gateway);
  IPAddress mascara = mostrarAtual ? WiFi.subnetMask() : IPAddress(cfg.mascara);

  p += F("<form method='post' action='/salvar-rede' onsubmit=\"return confirm('Salvar e reiniciar a porta "
         "com esta configuração de rede? Se ela estiver errada, corrija pela rede de socorro da porta, que "
         "abre nos 3 primeiros minutos depois de ligar.')\"><fieldset><legend>Rede</legend>"
         "<p class='aviso'>Para trocar o Wi-Fi de todas as placas, o normal é usar a página da central, "
         "que envia para as portas. Aqui é só para esta porta.</p>"
         "<label>Nome do Wi-Fi<input name='ssid' maxlength='32' required value='");
  p += escapar(cfg.ssid);
  p += F("'></label><label>Senha do Wi-Fi<input type='password' name='senha' maxlength='63' "
         "placeholder='em branco = manter a atual'></label>"
         "<label><input type='checkbox' "
         "onchange=\"this.form.senha.type=this.checked?'text':'password'\"> mostrar senha</label>"
         "<label><input type='checkbox' name='dhcp' id='dhcp' onchange='modoIp()'");
  if (cfg.dhcp) p += F(" checked");
  p += F("> IP automático (DHCP)</label><div id='fixo'>"
         "<label>IP da porta<input name='ip' required value='");
  if (uint32_t(ip)) p += ip.toString();
  p += F("'></label><label>Gateway (IP do roteador)<input name='gateway' required value='");
  if (uint32_t(gateway)) p += gateway.toString();
  p += F("'></label><label>Máscara<input name='mascara' required value='");
  if (uint32_t(mascara)) p += mascara.toString();
  p += F("'></label></div><button>Salvar e reiniciar</button></fieldset></form>"
         "<script>function modoIp(){var d=document.getElementById('dhcp').checked;"
         "document.querySelectorAll('#fixo input').forEach(function(e){e.disabled=d})}modoIp()</script>");

  p += F("<fieldset><legend>Manutenção</legend>"
         "<form method='post' action='/atualizar' enctype='multipart/form-data' "
         "onsubmit=\"return confirm('Gravar o firmware novo e reiniciar a porta?')\">"
         "<label>Atualizar firmware: arquivo firmware.bin da pasta .pio/build/porta" TEXTO_MACRO(NUMERO_PORTA)
         "<input type='file' name='firmware' accept='.bin' required></label>"
         "<button>Enviar firmware</button></form>"
         "<form method='post' action='/reiniciar' onsubmit=\"return confirm('Reiniciar a porta agora?')\">"
         "<button>Reiniciar a porta</button></form></fieldset></body></html>");

  servidor.send(codigo, "text/html; charset=utf-8", p);
}

// Página curta com uma mensagem, usada antes de reiniciar.
void enviarMensagemEReiniciar(const String& mensagem) {
  String p = FPSTR(ESTILO);
  p += "<p>" + mensagem + "</p><p>A porta " + String(NUMERO_PORTA) +
       " está reiniciando. Em uns 15 segundos, <a href='/'>abra a página de novo</a>.</p></body></html>";
  servidor.send(200, "text/html; charset=utf-8", p);
  reiniciarPedido = true;
  reiniciarPedidoEm = millis();
}

void tratarPagina() {
  if (!autorizado()) return;
  enviarPagina();
}

// Confere nome e senha do Wi-Fi recebidos. Devolve a mensagem de erro, ou vazio se estiver tudo certo.
String erroNoWifi(const String& ssid, const String& senha) {
  if (ssid.length() == 0 || ssid.length() > 32) return F("Nome do Wi-Fi inválido (até 32 caracteres).");
  if (senha.length() > 0 && (senha.length() < 8 || senha.length() > 63)) {
    return F("A senha do Wi-Fi precisa ter de 8 a 63 caracteres.");
  }
  return String();
}

// Usado pela central, quando ela envia o Wi-Fi novo para as portas. Só troca o Wi-Fi;
// o modo de IP continua o mesmo. Responde 200 só se salvou, para a central saber se a porta recebeu.
void tratarSalvarWifi() {
  if (!autorizado()) return;
  String ssid = servidor.arg("ssid");
  String senha = servidor.arg("senha");
  String erro = erroNoWifi(ssid, senha);
  if (erro.length()) {
    enviarPagina(erro, 400);
    return;
  }

  strlcpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid));
  if (senha.length()) strlcpy(cfg.senha, senha.c_str(), sizeof(cfg.senha));
  salvarConfig();
  Serial.printf("Wi-Fi novo salvo: \"%s\". Reiniciando...\n", cfg.ssid);
  enviarMensagemEReiniciar("Wi-Fi salvo. A porta vai entrar na rede \"" + escapar(cfg.ssid) + "\".");
}

// Formulário "Rede" desta página: Wi-Fi e modo de IP.
void tratarSalvarRede() {
  if (!autorizado()) return;
  String ssid = servidor.arg("ssid");
  String senha = servidor.arg("senha");
  bool dhcp = servidor.hasArg("dhcp");
  IPAddress ip, gateway, mascara;

  String erro = erroNoWifi(ssid, senha);
  if (erro.length()) {
    enviarPagina(erro, 400);
    return;
  }
  // No modo automático os campos de IP fixo nem são enviados: fica o que já estava salvo.
  if (!dhcp) {
    if (!ip.fromString(servidor.arg("ip")) || !gateway.fromString(servidor.arg("gateway")) ||
        !mascara.fromString(servidor.arg("mascara"))) {
      enviarPagina(F("IP, gateway ou máscara em formato errado (ex: 192.168.16.50). Nada foi salvo."), 400);
      return;
    }
    if (!mascaraValida(mascara)) {
      enviarPagina(F("Máscara inválida (o normal é 255.255.255.0). Nada foi salvo."), 400);
      return;
    }
    if (!redeValida(ip, gateway, mascara)) {
      enviarPagina(F("O IP da porta tem que estar na mesma faixa do gateway e ser diferente dele "
                     "(ex: gateway 192.168.16.1, porta 192.168.16.50). Nada foi salvo."), 400);
      return;
    }
  }

  strlcpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid));
  if (senha.length()) strlcpy(cfg.senha, senha.c_str(), sizeof(cfg.senha));
  cfg.dhcp = dhcp ? 1 : 0;
  if (!dhcp) {
    cfg.ip = ip;
    cfg.gateway = gateway;
    cfg.mascara = mascara;
  }
  salvarConfig();
  Serial.printf("Rede salva: \"%s\", IP %s. Reiniciando...\n", cfg.ssid, dhcp ? "automático" : ip.toString().c_str());

  String mensagem = "Configuração salva. A porta vai entrar na rede \"" + escapar(cfg.ssid) + "\"";
  mensagem += dhcp ? String(F(" com IP automático.")) : " com o IP fixo " + ip.toString() + ".";
  enviarMensagemEReiniciar(mensagem);
}

// Faz o mesmo que um aperto de verdade, mas espera a resposta para mostrar o resultado.
void tratarSimular() {
  if (!autorizado()) return;
  const char* origem = servidor.arg("botao") == "saida" ? "saida-simulado" : "entrada-simulado";
  Serial.printf("Aperto simulado: %s\n", origem);
  if (WiFi.status() != WL_CONNECTED) {
    enviarPagina(F("A porta está sem Wi-Fi, então não deu para avisar a central."));
  } else if (avisarCentral(origem)) {
    enviarPagina("Aperto simulado entregue: " + escapar(ultimoEnvioTexto) + ". O buzzer deve tocar.");
  } else {
    enviarPagina("Aperto simulado <b>falhou</b>: " + escapar(ultimoEnvioTexto));
  }
}

void tratarReiniciar() {
  if (!autorizado()) return;
  Serial.println("Reinício pedido pela página");
  enviarMensagemEReiniciar(F("Reiniciando a pedido da página."));
}

// Chamado depois que o arquivo do firmware chegou inteiro (ver receberFirmware).
void tratarAtualizacao() {
  if (!autorizado()) return;
  if (!atualizacaoIniciada && erroAtualizacao.length() == 0) erroAtualizacao = F("nenhum arquivo recebido.");
  if (erroAtualizacao.length() == 0) {
    enviarMensagemEReiniciar(F("Firmware gravado. Confira a data do firmware na página depois que ela voltar."));
  } else {
    // Reinicia mesmo com erro, para descartar o que foi recebido pela metade.
    enviarMensagemEReiniciar("<b>Firmware não foi gravado:</b> " + escapar(erroAtualizacao) +
                             " A porta continua com o firmware atual.");
  }
}

// ===== Programa =====

void setup() {
  Serial.begin(115200);
  for (Botao& b : botoes) pinMode(b.pino, INPUT_PULLUP);
  if (PINO_LED >= 0) pinMode(PINO_LED, OUTPUT);
  led(false);

  EEPROM.begin(sizeof(Config));
  carregarConfig();

  snprintf(nome, sizeof(nome), "campainha-porta%d", NUMERO_PORTA);
  snprintf(nomeAP, sizeof(nomeAP), "Campainha-Porta%d", NUMERO_PORTA);

  WiFi.persistent(false);  // a config do Wi-Fi fica só na nossa EEPROM
  WiFi.mode(WIFI_STA);
  WiFi.hostname(nome);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);  // sem economia de energia: o aviso sai mais rápido
  WiFi.setAutoReconnect(true);
  conectarNaRede();
  Serial.printf("\nPorta %d - conectando em \"%s\"... (último reinício: %s)\n", NUMERO_PORTA, cfg.ssid,
                ESP.getResetReason().c_str());

  servidor.on("/", HTTP_GET, tratarPagina);
  servidor.on("/salvar-wifi", HTTP_POST, tratarSalvarWifi);
  servidor.on("/salvar-rede", HTTP_POST, tratarSalvarRede);
  servidor.on("/simular", HTTP_POST, tratarSimular);
  servidor.on("/reiniciar", HTTP_POST, tratarReiniciar);
  servidor.on("/atualizar", HTTP_POST, tratarAtualizacao, receberFirmware);
  servidor.onNotFound([]() { servidor.send(404, "text/plain", "nao encontrado"); });
  servidor.begin();

  udp.begin(PORTA_DESCOBERTA);
  MDNS.begin(nome);  // página também em http://campainha-portaN.local
  MDNS.addService("http", "tcp", 80);
}

void loop() {
  unsigned long agora = millis();
  servidor.handleClient();
  responderCentral();
  MDNS.update();
  gerenciarWifi(agora);

  for (Botao& b : botoes) {
    // Lê os dois botões sempre, para o debounce de cada um ficar em dia.
    if (foiApertado(b) && !avisoPendente) {
      Serial.printf("Botão de %s apertado\n", b.nome);
      avisoPendente = true;
      origemPendente = b.nome;
      pendenteDesde = agora;
      ultimaTentativa = agora - ESPERA_ENTRE_TENTATIVAS_MS;  // primeira tentativa sai já
    }
  }

  if (avisoPendente) {
    if (estavaConectado && agora - ultimaTentativa >= ESPERA_ENTRE_TENTATIVAS_MS) {
      ultimaTentativa = agora;
      led(true);
      if (avisarCentral(origemPendente)) {
        Serial.println("Central avisada");
        avisoPendente = false;
        jaAvisou = true;
        ultimoAviso = millis();
      }
    }
    if (avisoPendente && millis() - pendenteDesde > PRAZO_AVISO_MS) {
      Serial.println("Desisti: não consegui avisar a central a tempo");
      avisoPendente = false;
      piscarErro();
    }
  } else if (estavaConectado && millis() - centralRespondeuEm > RECONECTAR_SEM_CENTRAL_MS) {
    // Muito tempo sem resposta da central, nem pela procura em broadcast: reconecta para
    // pedir IP de novo ao roteador. Espera mais 10 min antes de tentar de novo.
    Serial.println("10 min sem resposta da central: reconectando o Wi-Fi para pedir IP de novo");
    reconexoes++;
    ultimaReconexao = millis();
    centralRespondeuEm = millis();
    WiFi.disconnect();
    conectarNaRede();
  } else if (estavaConectado && millis() - ultimaProcura > INTERVALO_PROCURA_MS) {
    procurarCentral();  // confere se a central continua no mesmo IP
  }

  // Espera um pouco antes de reiniciar para a resposta chegar a quem pediu.
  if (reiniciarPedido && millis() - reiniciarPedidoEm > 1500) ESP.restart();

  agora = millis();
  if (!estavaConectado) {
    led((agora / (apLigado ? 150 : 500)) % 2);
  } else {
    led(avisoPendente || (jaAvisou && agora - ultimoAviso < 1000));
  }
}
