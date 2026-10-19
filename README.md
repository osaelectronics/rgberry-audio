# RGBerry SCART – audio en Raspberry Pi 5 (GPIO18/19)

En Pi 2/3/4 el audio PWM del SoC se remapeaba a GPIO18/19 con `audremap`.
En Pi 5 el bloque de audio del RP1 (AUDIO_OUT) sólo sale por GPIO12/13, que en
la placa son vídeo. Este paquete genera el audio PWM estéreo en **GPIO18/19**
con el **PIO del RP1**, sin cambios de hardware.

## Instalación (usuario final)

En la Raspberry Pi 5, con conexión a Internet:

```bash
curl -fsSL https://raw.githubusercontent.com/osaelectronics/rgberry-audio/main/get.sh | sudo bash
```

Descarga el código, lo compila e instala el servicio. En RePlay OS no hay que
tocar ningún menú: el audio sale por el SCART con la opción por defecto.

Opciones: una versión concreta con `... | sudo RGBERRY_REF=v2.4.0 bash`, o
un tono de prueba al terminar con `... | sudo bash -s -- --test`.

Instalación manual desde una copia del repositorio: `sudo ./install.sh`.
Desinstalar: `sudo ./uninstall.sh`.

La misma tarjeta SD sigue funcionando en Pi 2/3/4: al arrancar,
`rgberry-audio-select` instala la configuración ALSA de Pi 5 sólo en una Pi 5 y
restaura la original en las demás; el servicio sólo arranca si existe `/dev/pio0`.

**Importante:** en `config.txt`, el overlay de audio de Pi 4 debe ir bajo `[pi4]`:

```
[pi4]
dtoverlay=audremap,pins_18_19
[all]
```

## RePlay OS

RePlay abre la salida "Primary HDMI" con el PCM `replay_hdmi0_pi45`. En la
Pi 5 el instalador lo redirige al SCART, así que funciona con la opción por
defecto del menú. "Secondary HDMI" sigue sacando audio por HDMI. El bloque
se añade al `/etc/asound.conf` de RePlay sin sustituirlo, y en Pi 2..4 se
restaura el original al arrancar.

## Cómo funciona

```
aplicación (RePlay/SDL, ALSA default…)
   → plugin ALSA "rgberry" (libasound_module_pcm_rgberry.so, dentro de la app)
   → anillo en memoria compartida /dev/shm/rgberry-audio
   → daemon rgberry-audio → PWM doble con noise shaping → PIO SM0 + DMA → GPIO18/19 → filtro RC
```

* **Un solo reloj**: el daemon toma un periodo del anillo cada vez que la DMA
  acepta uno, así que la posición de reproducción que ve la aplicación avanza
  exactamente al ritmo del cristal de la RP1, como en una tarjeta de sonido
  real. Sin loopback, sin dmix y sin control de deriva. (Las versiones 1.x/2.x
  usaban `snd-aloop`: su reloj a saltos de 4 ms hacía que el control de ritmo
  de los emuladores produjera tirones.)
* `pcm.rgberry_hw` acepta S16_LE estéreo a 48 kHz; `rgberry` / `default`
  añaden conversión de formato y frecuencia. Una sola aplicación a la vez.
* Si el daemon no está en marcha, el plugin sigue el tiempo por su cuenta y la
  aplicación nunca se bloquea.
* **Portadora 192 kHz**, ~1042 ciclos de 200 MHz por periodo (≈10 bits), noise
  shaping de 2.º orden con dither TPDF, interpolación ×4 desde 48 kHz.
* **Linealización del PWM de flanco simple**: sin ella el 2.º armónico queda en
  −43 dB a 1 kHz; con la predistorsión de 2.º orden baja a −85 dB.
* **Un solo word de 32 bits por periodo para los dos canales**:
  `t1[9:0] | p1[11:10] | t2[21:12] | t3[31:22]`.
* **Una sola state machine**: con DMA cíclica en kernels que la tienen
  (rpi-6.18 reciente) o transferencias encoladas; la cola se mantiene llena.
  `pull noblock` repite el último frame si la FIFO se vaciara.
* **Arranque/parada suaves** (rampa de 350 ms) para no dar golpe en los
  condensadores de acoplo.

## Resultados del simulador (sin hardware)

`make test` compila un emulador ciclo a ciclo del programa PIO, alimentado por
el codificador real (medido tras un pasa-bajos de 80 kHz), y prueba el plugin
ALSA de extremo a extremo con un daemon simulado:

| Prueba | Resultado |
|---|---|
| Codificación de instrucciones vs. `pio_instructions.h` de piolib | idéntica |
| Periodo PWM | 1041/1042 ciclos, sin saltos |
| 1 kHz −1 dBFS | SINAD 66 dB, THD −85 dB |
| 3,1 kHz / 6 kHz | THD −66 / −56 dB |
| Diafonía L↔R | < −120 dB |
| Plugin ALSA + anillo (cliente a 60 fps, 8 s) | 0 discontinuidades, 0 xruns |

## Opciones (`/etc/default/rgberry-audio`)

```
RGBERRY_ARGS="-s"        # intercambiar L/R
RGBERRY_ARGS="-g 0.40"   # bajar nivel de salida (por defecto 0.46)
RGBERRY_ARGS="-v"        # estadísticas (journalctl -u rgberry-audio, si el sistema guarda log)
```

Prueba en banco:

```bash
sudo systemctl stop rgberry-audio
sudo rgberry-audio -d                      # onda cuadrada 1 kHz, sin DMA (prueba de pines)
sudo rgberry-audio -T                      # autotest de 3 s con informe
sudo rgberry-audio -t 1000 --tone-ch L -v  # tono por el canal izquierdo
```

## Pendiente de validar en hardware

* Ajuste del nivel (`-g`) para igualar el de la Pi 4 con tu filtro RC.
* Modos entrelazados en RePlay (comparten el PIO con su "interlace fixer").
* Volumen: no hay control de mezclador propio; RePlay ajusta el volumen en
  su propio audio.

## Licencias

`third_party/piolib` © Raspberry Pi Ltd, BSD-3-Clause (ver `LICENCE`).
