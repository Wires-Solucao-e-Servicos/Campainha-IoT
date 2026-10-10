#pragma once

// ===== Wi-Fi (valores de fábrica) =====
// Tem que ser o mesmo Wi-Fi da central. O IP da central não precisa ser configurado:
// a NodeMCU pergunta na rede onde ela está.
// De fábrica a porta usa IP automático (DHCP). Depois de gravado, o Wi-Fi e o IP (automático
// ou fixo) podem ser trocados sem cabo: pela página da porta, pela página da central (que
// envia só o Wi-Fi novo para as portas) ou pela rede de socorro da porta. O que for salvo
// assim fica na flash e vale mais que os valores daqui. Se você mudar o Wi-Fi daqui e gravar
// o firmware de novo, ele volta a valer (com IP automático).
#define WIFI_SSID  "EspCampainha"
#define WIFI_SENHA "Wires01#"

// ===== Acesso =====
// Senha da página da porta (usuário "admin") e da rede de socorro "Campainha-PortaN".
// Tem que ser IGUAL ao SENHA_ADMIN da central: é com ela que a central envia o Wi-Fi
// novo para as portas. De 8 a 63 caracteres.
#define SENHA_ADMIN "Wires01#"

// ===== Pinos =====
// Cada botão vai entre o seu pino e o GND (usa o pull-up interno).
// Outros pinos seguros na NodeMCU: 4 (D2), 5 (D1), 13 (D7).
// Evite 0 (D3), 2 (D4) e 15 (D8), que atrapalham o boot, e 16 (D0), que não tem pull-up interno.
#define PINO_BOTAO_ENTRADA 14  // D5
#define PINO_BOTAO_SAIDA   12  // D6

// LED de status. O LED azul do módulo ESP-12 fica no GPIO2 e acende com LOW. Use -1 para desativar.
#define PINO_LED          2
#define LED_ATIVO_EM_LOW  true

// ===== Tempos =====
#define DEBOUNCE_MS                 50     // filtra o ruído do contato dos botões
#define TIMEOUT_HTTP_MS             1500   // quanto espera a central responder em cada tentativa
#define ESPERA_ENTRE_TENTATIVAS_MS  1000   // se a central não respondeu, espera isso e tenta de novo
#define PRAZO_AVISO_MS              20000  // desiste do aviso se não conseguir entregar neste tempo
