# Rover ESP32

Rover com ESP32 e driver de motores TB6612FNG, controlado pelo celular por um site que fala com a placa via Wi-Fi e MQTT.

- `firmware/rover_esp32/rover_esp32.ino`: código do ESP32 (Arduino).
- `site/index.html`: controle que está no ar em https://motores-esp32.vercel.app (projeto `motores-esp32` na Vercel).

## Ligações

| ESP32 | TB6612FNG |
|---|---|
| GPIO 26 | AIN1 |
| GPIO 27 | AIN2 |
| GPIO 25 | PWMA |
| GPIO 14 | BIN1 |
| GPIO 13 | BIN2 |
| GPIO 33 | PWMB |
| 3V3 | VCC e STBY |
| GND | GND (junto com o GND da plaquinha USB) |

VM do driver recebe os 5 V (VBUS) da plaquinha USB. Motor A em AO1/AO2, motor B em BO1/BO2.

## Firmware

1. No Arduino IDE, instale o suporte a ESP32 e a biblioteca **PubSubClient**.
2. Preencha `WIFI_SSID` e `WIFI_SENHA` no topo do arquivo. **Não suba a senha do Wi-Fi para o GitHub**: este repositório é público.
3. Grave na placa (ESP32 Dev Module) e abra o Serial Monitor em 115200.

Compila nos núcleos Arduino-ESP32 2.x e 3.x; a API de PWM certa é escolhida na compilação.

Comandos pelo Serial Monitor:

| Comando | O que faz |
|---|---|
| `frente`, `tras`, `esquerda`, `direita`, `parar` | Movem o rover por 3 s |
| `v200` | Velocidade de 0 a 255 |
| `teste` | Gira cada motor para frente e para trás, sem precisar de Wi-Fi |
| `medir` | Liga os dois motores em 100% por 30 s, para medir com multímetro |
| `status`, `ajuda` | Estado atual e lista de comandos |

Se um motor girar ao contrário ou os lados estiverem trocados, ajuste `INVERTER_MOTOR_A`, `INVERTER_MOTOR_B` ou `MOTOR_A_E_O_ESQUERDO` no topo do código.

## Comunicação

Site e ESP32 se falam pelo broker público `broker.hivemq.com` (o site por WebSocket na porta 8884, o ESP32 na 1883).

- `nfw-motores/6a21b1e8/cmd`: comandos. Exemplo: `v200 1f 2p` (velocidade 200, motor 1 para frente, motor 2 parado; `t` é para trás). `s` para tudo.
- `nfw-motores/6a21b1e8/status`: o ESP32 publica `online`; o broker publica `offline` se ele sumir.

O site repete o comando a cada 250 ms enquanto um botão está apertado. O ESP32 desliga os motores se ficar 600 ms sem comando.

O broker não pede senha: quem souber o nome do tópico consegue mandar comandos.

## Publicar o site

Dentro da pasta `site/`:

```bash
vercel --prod
```

O `index.html` precisa ficar na raiz do que é publicado.
