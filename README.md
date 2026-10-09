# Campainha do estacionamento

2 NodeMCU (uma em cada porta, cada uma com botão de **entrada** e de **saída**)
avisam pela rede Wi-Fi uma central ESP-01 com módulo relé. Qualquer botão
liga o buzzer por 3 segundos.

```
 Porta 1 (NodeMCU) [entrada] [saída] ─┐
                                      ├── Wi-Fi "EspCampainha" ──> Central (ESP-01 + relé) ──> buzzer 3 s
 Porta 2 (NodeMCU) [entrada] [saída] ─┘   GET /tocar
```

## Como funciona

- Os ESPs nunca falam diretamente entre si: tudo passa pelo roteador. A distância
  entre a central e as portas não importa, só o sinal de cada uma até o roteador.
- Todos os ESPs pegam **IP automático** do roteador (DHCP). Nada de IP fixo.
- Quando a NodeMCU conecta no Wi-Fi, ela pergunta para a rede toda onde está a
  central (mensagem `CAMPAINHA?` em broadcast UDP para 255.255.255.255, porta 4210).
  A central responde e a NodeMCU guarda o IP dela. Ela confere de novo a cada 5 min.
- Quando um botão é apertado, a NodeMCU acessa
  `http://IP_DA_CENTRAL/tocar?origem=porta1-entrada` (ou `porta2-saida` etc.).
  A central responde "ok" e liga o buzzer pelo tempo configurado (3 s de fábrica).
- Se a central não responder "ok" no IP guardado (por exemplo, o roteador deu outro IP
  para ela), a NodeMCU procura de novo e tenta outra vez, por até 20 s.
- **Faixas de IP diferentes:** se a porta e a central acabarem em faixas diferentes
  (por exemplo, dois aparelhos entregando IP na mesma rede), o HTTP não chega. Nesse
  caso a porta manda o aviso em broadcast (`TOCAR <id> <origem>`), que chega a todos
  no mesmo Wi-Fi seja qual for a faixa, e a central confirma em broadcast
  (`TOCADO <id>`). A campainha continua funcionando, e as páginas avisam do problema.
- Se a porta ficar 10 min sem nenhuma resposta da central, ela reconecta o Wi-Fi
  para pedir IP de novo.
- Apertos durante o toque são ignorados: o buzzer desliga no tempo certo e o
  próximo aperto depois disso toca na hora.
- A origem do aviso não muda o toque. Ela só aparece na página de status da central.
- O Wi-Fi de todos fica salvo na flash e pode ser trocado sem cabo
  (veja [Trocar o Wi-Fi ou o roteador](#trocar-o-wi-fi-ou-o-roteador)).

**Por que HTTP:** a NodeMCU recebe a confirmação da central e sabe se precisa
tentar de novo, não precisa de nenhum servidor extra (como um broker MQTT) e dá
para testar a central pelo navegador do celular.

## Estrutura

| Pasta | Placa | O que faz |
|---|---|---|
| `central-esp01/` | ESP-01 / ESP-01S + relé | Recebe os avisos, liga o buzzer e distribui o Wi-Fi novo |
| `porta-esp8266/` | NodeMCU | Lê os 2 botões e avisa a central (mesmo código nas 2 portas) |

Abra o arquivo `Campainha-Portas.code-workspace` no VSCode para ver os dois
projetos na barra do PlatformIO.

## Rede (D-Link DIR-615 em modo Access Point)

- O DIR-615 fica em modo **Access Point**: cria o Wi-Fi `EspCampainha` (WPA2) e
  liga os ESPs à rede do **roteador principal**, que é quem distribui os IPs (DHCP)
  para o D-Link e para os ESPs. Não precisa reservar IP para ninguém.
- O nome `espcampainha.local` é do **D-Link**. A central usa outro nome,
  `campainha-central.local`, para os dois não se confundirem.
- A campainha depende do roteador principal estar ligado e conectado ao D-Link:
  sem ele, ninguém recebe IP.
- O isolamento de clientes ("AP isolation") precisa ficar desligado no D-Link,
  senão os ESPs não se enxergam.

## Passo a passo

### 1. Configurar a central (`central-esp01/include/config.h`)

1. `WIFI_SSID` e `WIFI_SENHA` já estão com `EspCampainha` / `Wires01#`.
2. `USAR_DHCP true` (padrão). Os campos de IP fixo só valem se mudar para `false`.
3. `SENHA_ADMIN`: senha da página e da rede de socorro. **Tem que ser igual nas
   NodeMCU**, porque é com ela que a central envia o Wi-Fi novo para as portas.
4. Escolha o tipo do módulo relé:
   - **`RELE_VIA_GPIO`**: placa pequena "ESP-01S Relay", só com o relé, um
     transistor e alguns componentes. O relé fica no GPIO0.
   - **`RELE_VIA_SERIAL`**: placa da LC Technology, que tem um chip **STC15F104**
     perto do relé. O ESP-01 manda comandos pela serial (9600 baud; se não
     funcionar, tente `RELE_BAUD 115200`).
5. No modo GPIO, se o relé ficar ligado o tempo todo e desligar no toque, troque
   `RELE_ATIVO_EM_LOW` para `false`.

### 2. Gravar a central

O ESP-01 precisa estar num adaptador USB com o **GPIO0 no GND** ao ligar
(adaptador com chave PROG/UART ou um jumper). Depois de gravar, volte a chave
para UART (ou tire o jumper), coloque o ESP-01 no módulo relé e ligue.

No PlatformIO: **central > Upload**. Se o seu ESP-01 for o antigo de 512 KB
(placa azul), troque `board = esp01_1m` para `board = esp01` no `platformio.ini`.

### 3. Testar a central pelo navegador

Com o celular ou o PC na mesma rede, abra **`http://campainha-central.local`**.
Usuário `admin`, senha `SENHA_ADMIN` do config.h (padrão: `campainha`).

O nome `.local` funciona no Windows, iPhone e Mac. Se não abrir (alguns Android),
procure `campainha-central` na lista de clientes DHCP do roteador principal e abra
o IP que aparece lá.

A página mostra o último aviso, de onde ele veio, o IP atual, o sinal do Wi-Fi e
tem um botão **Testar buzzer**.

### 4. Gravar as NodeMCU (`porta-esp8266/include/config.h`)

1. Confira se `WIFI_SSID`, `WIFI_SENHA` e `SENHA_ADMIN` são os mesmos da central.
   Não há IP para configurar.
2. Grave cada placa com o ambiente da sua porta:
   - Barra do PlatformIO: **porta1 > Upload** e **porta2 > Upload**
   - ou pelo terminal do PlatformIO: `pio run -e porta1 -t upload`

A única diferença entre os 2 firmwares é o número da porta, que vem do
`platformio.ini` (`-D NUMERO_PORTA=1` e `2`). No monitor serial (115200) a NodeMCU
mostra `Central encontrada em ...` quando acha a central.

Cada NodeMCU também tem uma página (`http://campainha-porta1.local`, usuário
`admin`, senha `SENHA_ADMIN`) que mostra o Wi-Fi, o IP e se ela achou a central,
e permite trocar o Wi-Fi dela.

## Página de configuração da central

Em `http://campainha-central.local` (usuário `admin`, senha `SENHA_ADMIN`) dá para mudar:

- **Tempo do toque**: de 0,1 a 60 s. Vale na hora, sem reiniciar.
- **Wi-Fi**: a central envia o Wi-Fi novo para as portas, mostra quais receberam e
  reinicia. Deixe a senha em branco para manter a atual; use "mostrar senha" para
  conferir o que digitou.
- **IP automático (DHCP) ou fixo**: desmarcando "IP automático", aparecem IP, gateway
  e máscara (já preenchidos com o IP atual). A página não aceita IP fora da faixa do
  roteador nem máscara inválida. Mesmo com IP fixo, as NodeMCU continuam encontrando
  a central sozinhas.
- **Restaurar valores do firmware**: volta ao Wi-Fi, modo de IP e tempo do `config.h`
  (se o Wi-Fi mudar, as portas recebem também).

O que é salvo pelas páginas fica guardado na flash e continua valendo depois de
desligar. Se você mudar algum valor de fábrica no `config.h` e gravar o firmware
de novo, os valores do `config.h` voltam a valer.

## Manutenção remota

As páginas da central e das portas têm uma seção **Manutenção** e mostram dados para
diagnóstico. Tudo isso funciona de longe, pela rede.

**Na página de cada porta:**
- **Botões:** estado de cada botão (solto/apertado), quantos apertos desde que ligou e
  quando foi o último. Um botão "apertado" há mais de 10 s aparece em destaque: é fio
  em curto ou botão travado.
- **Último aviso à central:** se deu certo ou o erro (por exemplo, central não encontrada).
- **Simular entrada / Simular saída:** faz o mesmo caminho de um aperto de verdade e
  mostra se a central respondeu. Na central, o aviso aparece como `porta1-entrada-simulado`.
- **Rede:** IP, gateway e máscara da porta, IP da central e quando ela respondeu pela
  última vez. Se a central estiver em outra faixa de IP, aparece em destaque, assim
  como as reconexões feitas por falta de resposta da central.

**Nas duas páginas:**
- **Situação:** há quanto tempo está ligada, o motivo do último reinício (queda de
  energia, travamento, reinício pelo programa...), memória livre e a data do firmware.
- **Reiniciar:** reinicia a placa.
- **Atualizar firmware:** envie o arquivo `firmware.bin` gerado pelo PlatformIO:

  | Placa | Arquivo |
  |---|---|
  | Central | `central-esp01/.pio/build/central/firmware.bin` |
  | Porta 1 | `porta-esp8266/.pio/build/porta1/firmware.bin` |
  | Porta 2 | `porta-esp8266/.pio/build/porta2/firmware.bin` |

  Cada placa só aceita o próprio firmware: o arquivo da porta 2 enviado para a porta 1
  (ou o da central para uma porta) é recusado, e a placa continua com o firmware atual.
  Depois de gravar, a placa reinicia; confira a data do firmware na página.

**Como saber se um defeito é físico:** com alguém apertando o botão no local, olhe a
página da porta. Se o número de apertos não aumenta, o aperto não chega na placa
(botão, fio ou o pino). Se aumenta mas o "último aviso" falha, o problema é a
comunicação. Se o aviso dá certo, a porta está boa.

## Trocar o Wi-Fi ou o roteador

**Roteador novo com o mesmo nome e senha de Wi-Fi:** não precisa fazer nada. Os ESPs
conectam sozinhos, pegam IP novo e se encontram de novo, mesmo que a faixa de IP mude.

**Troca planejada (o Wi-Fi atual ainda está funcionando):**

1. Abra a página da central e coloque o nome e a senha do Wi-Fi **novo**.
2. Clique em **Salvar e reiniciar**. A central procura as portas, envia o Wi-Fi novo
   e mostra a lista: cada porta aparece como "recebeu" ou "não confirmou".
3. Todos reiniciam e passam a procurar o Wi-Fi novo. Enquanto ele não existir,
   cada um abre a sua rede de socorro (é normal). Assim que o roteador novo estiver
   no ar, todos conectam sozinhos.

**O roteador antigo já foi trocado, ou alguma porta não recebeu:** use a rede de
socorro de cada aparelho (abaixo), indo até ele com o celular.

### Redes de socorro

Cada aparelho que fica **mais de 30 s sem conseguir entrar no Wi-Fi** abre a própria
rede, com a senha `SENHA_ADMIN`:

| Aparelho | Rede de socorro | Página |
|---|---|---|
| Central | `Campainha-Central` | `http://192.168.4.1` |
| Porta 1 | `Campainha-Porta1` | `http://192.168.4.1` |
| Porta 2 | `Campainha-Porta2` | `http://192.168.4.1` |

Conecte o celular nessa rede, abra a página, entre com `admin` / `SENHA_ADMIN` e
coloque o Wi-Fi certo. O aparelho reinicia e entra na rede.

Com a rede de socorro aberta, o aparelho **continua procurando o roteador**: se o
roteador só caiu e voltou, ele reconecta sozinho e a rede de socorro fecha. A busca
só pausa enquanto alguém está usando a página pela rede de socorro (até 3 min depois
do último acesso), para a página não ficar instável. Um celular que só ficou conectado
na rede de socorro, sem abrir a página, não atrapalha a reconexão. Como garantia extra,
depois de 10 min sem Wi-Fi o aparelho reinicia e tenta de novo.

A central também abre a rede de socorro nos **3 primeiros minutos** depois de ligar.
Se perder o acesso a ela por qualquer motivo (um IP fixo errado, por exemplo),
desligue e ligue a central de novo.

A rede de socorro só alcança alguns metros a partir do aparelho: é preciso ir até
cada porta com o celular.

## Antes de instalar

- Troque a senha padrão `campainha` do `SENHA_ADMIN` nos dois `config.h`. Ela tem que
  ser **igual** na central e nas portas e ter de 8 a 63 caracteres (o código não
  compila fora disso).
- Grave cada NodeMCU com o ambiente da sua porta (`porta1`, `porta2`). Duas placas
  com o mesmo número usam o mesmo nome na rede e se confundem na página da central.
- As redes de socorro usam a faixa `192.168.4.x`. Se a rede do roteador principal
  também for `192.168.4.x`, os ESPs se confundem enquanto a rede de socorro está
  ligada: nesse caso, a faixa da rede de socorro precisa ser trocada no código.
- As versões da plataforma ficam fixas no `platformio.ini` (`espressif8266@4.2.1`),
  para uma atualização do PlatformIO não mudar o comportamento.
- A atualização pela página só existe a partir da versão que a tem: a primeira
  gravação dessa versão tem que ser pelo USB, nas 3 placas. A central também mudou o
  layout da flash (`eagle.flash.1m.ld`, sem área de arquivos) para caber a atualização;
  a configuração salva não é apagada por isso.

## Ligações

**NodeMCU (cada porta):**

```
Botão de entrada: um terminal no D5 (GPIO14), o outro no GND
Botão de saída:   um terminal no D6 (GPIO12), o outro no GND
```

Se o fio até os botões for longo (alguns metros), coloque um resistor de 10 kΩ
entre o pino e o 3V3 e, se ainda houver toques falsos, um capacitor de 100 nF
entre o pino e o GND.

**Central:** o buzzer é alimentado pelos contatos do relé (COM e NA/NO), em série
com a fonte do buzzer. O relé só funciona como chave: a tensão do buzzer pode ser
diferente da do módulo.

## LED das NodeMCU

| LED | Significado |
|---|---|
| Piscando devagar | Sem Wi-Fi |
| Piscando rápido | Rede de socorro `Campainha-PortaN` ligada |
| Aceso | Enviando ou aviso entregue |
| 5 piscadas rápidas | Não conseguiu avisar a central em 20 s |

## Problemas comuns

- **NodeMCU não conecta no Wi-Fi:** confira nome e senha e se a rede é 2,4 GHz.
  Veja as mensagens no monitor serial (115200) ou use a rede de socorro dela.
- **NodeMCU mostra "Central não respondeu à procura":** a central está desligada,
  em outra rede, ou o roteador está bloqueando a comunicação entre aparelhos
  (opção "isolamento de clientes"/"AP isolation", que precisa ficar desligada).
- **Página mostra uma placa "em outra faixa de IP":** tem mais de um aparelho entregando
  IP na rede (por exemplo, o D-Link com o DHCP ligado, distribuindo 192.168.0.x). A
  campainha continua funcionando por broadcast, mas corrija a rede: deixe só o roteador
  principal entregando IP e reinicie a placa pela página. Enquanto isso, a troca de
  Wi-Fi pela central não chega nas portas que estão em outra faixa.
- **Porta "não confirmou" o Wi-Fi novo:** confira se o `SENHA_ADMIN` dela é igual ao
  da central. Ela continua no Wi-Fi antigo; quando ele for desligado, use a rede de
  socorro dela.
- **`campainha-central.local` não abre:** use o IP que aparece na lista de clientes
  DHCP do roteador principal.
- **Relé não aciona (modo GPIO):** confirme o tipo da placa. Se tiver o chip STC,
  use `RELE_VIA_SERIAL`.
- **No modo serial o monitor fica sem mensagens:** é esperado, a serial é usada
  para falar com o relé.
