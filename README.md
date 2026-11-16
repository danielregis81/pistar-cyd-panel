# pistar-cyd-panel

Painel **"quem está falando agora"** do Pi-Star numa tela **CYD 2.8"** (ESP32-2432S028R,
a "Cheap Yellow Display").

Mostra a estação que está transmitindo no seu hotspot DMR: **bandeira do país, indicativo,
nome, país, talkgroup, timeslot, origem (RF local ou rede), cronômetro do QSO, BER e perda**,
mais os últimos ouvidos e a contagem de estações e países do dia.

```
┌──────────────────────────────────────┐
│ PY0XXX DMR                     14:32 │
│ ┌──────────────────────────────────┐ │
│ │ [bandeira]  PY2XXX      ( NO AR )│ │
│ │             Joao                 │ │
│ │             Brazil               │ │
│ │ TG 72453            DMR TS2      │ │
│ │                     Net          │ │
│ │ 00:47          BER 0.0%  perda 0%│ │
│ └──────────────────────────────────┘ │
│ ┌ Ultimos        hoje: 89 / 31 ────┐ │
│ │ ZL1AG 14:30  VE9HI 14:29  ...    │ │
│ └──────────────────────────────────┘ │
└──────────────────────────────────────┘
```

## Como funciona

```
Pi-Star (MMDVMHost)                         CYD (ESP32)
  /var/log/pi-star/MMDVM-*.log                 │
        │  pistar-cyd-panel.py (serviço)       │  a cada 1-2 s
        ▼                                      │
  /run/pistar-cyd-panel/status.json  ◄─────────┘  GET http://<pi-star>/cyd/status.json
  /usr/local/share/.../flags/<cc>.565 ◄─────────  GET http://<pi-star>/cyd/flags/br.565
```

- O **coletor** roda no próprio Pi-Star e lê o log do MMDVMHost. Não depende do HTML do
  dashboard, então continua funcionando se o dashboard for atualizado ou personalizado.
- **Nome e país** vêm das bases do RadioID que o Pi-Star já baixa todo dia
  (`/usr/local/etc/nextionUsers.csv`; reserva: `DMRIds.dat` + prefixo MCC do DMR ID).
- O JSON fica em **memória** (`/run`), então nada é gravado no cartão SD.
- O nginx do Pi-Star já serve os arquivos: nenhuma porta nova, nenhuma regra de firewall.
- Ele não muda nenhum arquivo do dashboard. Só adiciona o link `/var/www/dashboard/cyd`.

## Requisitos

- Pi-Star 4.x com DMR (testado no **Pi-Star 4.2.6**, Raspberry Pi Zero W, Python 3.9).
- CYD 2.8" **ESP32-2432S028R** (tela ILI9341, 320x240).
- Para gravar: um computador com Chrome/Edge (gravação pelo navegador) ou o `esptool`.

## 1. Instalar no Pi-Star

Entre no Pi-Star por SSH (usuário `pi-star`) e rode:

```bash
cd /tmp
curl -L -o pcp.tar.gz https://github.com/danielregis81/pistar-cyd-panel/archive/refs/heads/main.tar.gz
tar xzf pcp.tar.gz
cd pistar-cyd-panel-main/pistar
sudo ./install.sh
```

No fim, o instalador mostra o endereço, por exemplo `OK: dados em http://192.168.0.100/cyd/status.json`.
Anote o IP: você pode precisar dele na configuração da CYD.

Para conferir, abra esse endereço no navegador. Deve aparecer um JSON com `"ok":true`.

O instalador:
- copia o coletor para `/usr/local/lib/pistar-cyd-panel/` e as bandeiras para
  `/usr/local/share/pistar-cyd-panel/flags/`;
- cria o serviço `pistar-cyd-panel` (sobe no boot e reinicia sozinho se cair);
- cria o link `/var/www/dashboard/cyd`;
- devolve o sistema de arquivos para somente leitura no fim, se ele estava assim.

Remover: `sudo ./uninstall.sh` (na mesma pasta).

## 2. Gravar a CYD (sem compilar)

O firmware pronto está em [`bin/pistar-cyd-panel.bin`](bin/pistar-cyd-panel.bin)
(imagem completa: grave no endereço **0x0**).

**Pelo navegador (Chrome ou Edge no computador):**
1. Ligue a CYD no USB.
2. Abra https://espressif.github.io/esptool-js/ e clique em **Connect**, escolhendo a porta da CYD.
3. Em **Flash Address** deixe `0x0`, escolha o arquivo `pistar-cyd-panel.bin` e clique em **Program**.

**Pelo terminal (esptool):**
```bash
pip install esptool
esptool.py --chip esp32 write_flash 0x0 bin/pistar-cyd-panel.bin
```

## 3. Configurar pelo celular

1. Ligue a CYD. No primeiro uso ela mostra na tela uma rede WiFi chamada **`PiStar-CYD-XXXX`**.
2. No celular, conecte nessa rede e abra **http://192.168.4.1** (muitos celulares abrem sozinhos).
3. Escolha a rede da casa, digite a senha, digite o **IP do Pi-Star** e escolha o fuso. Toque em **Salvar**.
4. A CYD reinicia, conecta e começa a mostrar quem está falando.

**Endereço do Pi-Star:** use o IP que o `install.sh` mostrou no final (por exemplo `192.168.0.100`).
O nome `pi-star.local` também funciona, mas nem toda rede resolve nomes `.local` de forma confiável.
Dica: reserve um IP fixo para o Pi-Star no seu roteador, assim ele não muda.

**Para mudar a configuração depois:** segure o botão **BOOT** da CYD por 3 segundos
(ou ligue a CYD segurando BOOT). A rede `PiStar-CYD-XXXX` aparece de novo.

Se a CYD não conseguir conectar no WiFi em 30 s, ela abre a configuração sozinha.
A configuração fica gravada na placa e não se perde quando desliga.

## Compilar você mesmo (opcional)

1. Instale o core **esp32 3.x** e as bibliotecas **TFT_eSPI** e **ArduinoJson 7**.
2. Copie [`firmware/User_Setup.h`](firmware/User_Setup.h) para a pasta da biblioteca TFT_eSPI,
   substituindo o `User_Setup.h` que está lá
   (Linux/Mac: `~/Arduino/libraries/TFT_eSPI/`, Windows: `Documentos\Arduino\libraries\TFT_eSPI\`).
3. Placa **ESP32 Dev Module**:
   ```bash
   arduino-cli compile --fqbn esp32:esp32:esp32 --export-binaries firmware/pistar-cyd-panel
   arduino-cli upload  --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 firmware/pistar-cyd-panel
   ```

## Problemas comuns

| Sintoma | O que fazer |
|---|---|
| Cores trocadas (verde fica roxo) ou "negativo" | No `User_Setup.h`, troque `TFT_BGR` por `TFT_RGB` ou comente `TFT_INVERSION_ON`. |
| Tela branca ou preta | Sua CYD pode ser a variante com ST7789 (2 portas USB). Troque `ILI9341_2_DRIVER` por `ST7789_DRIVER`. |
| "Procurando o Pi-Star..." | Confira se `http://<ip-do-pi-star>/cyd/status.json` abre no navegador. Se abrir, segure BOOT 3 s e confira o endereço digitado (prefira o IP). |
| Estação sem nome ou país | O indicativo não está na base do RadioID. O nome do Talker Alias é usado quando existe. |
| JSON não aparece depois de instalar | `sudo journalctl -u pistar-cyd-panel -n 30` no Pi-Star. A primeira leitura pode levar alguns segundos num Pi Zero. |

## Limitações

- Feito para **hotspot simplex de um canal só (single slot)**, que monitora **um timeslot**
  por vez. A tela mostra a última transmissão, seja de qual slot for. Num hotspot **duplex**
  (TS1 e TS2 ao mesmo tempo) ele funciona, mas mostra só um QSO por vez.
- Só DMR, por enquanto (D-Star, YSF, P25 e NXDN não aparecem).
- O painel é só leitura, sem toque.
- A contagem do dia recomeça quando o Pi-Star reinicia (o log fica em memória).

## Ideias para o futuro

- Painel para **hotspot duplex**: TS1 e TS2 lado a lado, cada um com o seu chamador.
- Outros modos (YSF, D-Star, NXDN).

## Para quem mantém o projeto

As bandeiras (`pistar/flags/*.565`) e o mapa de países (`pistar/countries.json`) são gerados por:

```bash
pip install pillow
python3 tools/make_flags.py
```

## Créditos e licença

- Código: licença MIT (veja [LICENSE](LICENSE)).
- Bandeiras: [flagcdn.com](https://flagcdn.com), imagens de domínio público (Wikimedia Commons).
- Tabela MCC → país: [pbakondy/mcc-mnc-list](https://github.com/pbakondy/mcc-mnc-list) (MIT).
- Dados de estações: [RadioID.net](https://radioid.net), baixados pelo próprio Pi-Star.

73!
