# cpp-irq-watch — IRQ Watch (Linux)

Monitor de **interrupciones del kernel de Linux** en **C++23** que lee `/proc/interrupts` y muestra en pantalla la distribución de interrupciones por núcleo de CPU: tabla de IRQs con conteos por CPU, semáforo de color por actividad, tasas por segundo en modo watch (delta entre snapshots), y resumen con la CPU más ocupada. Cero dependencias externas: compilación directa con `g++`.

## Features

- **Parseo robusto de `/proc/interrupts`**: header de CPUs, filas IRQ con conteos por núcleo, entradas nombradas (`NMI`, `LOC`, `RES`, ...), entradas de conteo único (`ERR`, `MIS` — se rellenan con ceros) y **líneas de continuación multi-línea** (filas que envuelven en máquinas con muchos CPUs)
- **Tabla top-N por actividad**: ordena por total (o por tasa en watch), con columna por CPU y total; nombres truncados a 18 chars con alineación de columnas
- **Semáforo de color**: verde < 500 int/s, amarillo < 5000, rojo ≥ 5000 (dim = inactivo); colores ANSI desactivables (`--no-color`)
- **Modo watch** (`--watch N`): refresco en vivo cada N segundos con **tasas delta** (interrupciones por segundo por IRQ y por CPU), limpieza de pantalla y `std::flush` (salida completa incluso con `timeout`/kill)
- **Modo demo** (`--demo`): usa una muestra sintética embebida con contadores que crecen (LCG) — permite ver el tool en contenedores/VMs donde `/proc/interrupts` está vacío o enmascarado
- Salida **JSON** completa para scripting (`--json`): `{cpus, irqs[{irq,name,counts,total|rate}], summary{irqs,cpus,total,total_rate}}`
- **19 tests de auto-verificación** (`--selftest`): parseo sintético (header, nombradas, ERR/MIS, continuaciones), deltas, formateo y smoke test condicional contra `/proc` real
- Formato de números: `12.3k`, `987.7M`, `1.2G` (conteos y tasas)

## Build

```bash
g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o irq-watch
```

## Usage

```bash
./irq-watch                 # snapshot: top 10 IRQs por total
./irq-watch --top 6         # solo los 6 más activos
./irq-watch --watch 2       # refresco en vivo cada 2s con tasas por segundo
./irq-watch --json          # JSON completo para scripting
./irq-watch --demo          # datos sintéticos (contenedores/VMs)
./irq-watch --no-color      # sin ANSI (pipes/logs)
./irq-watch --selftest      # auto-verificación
```

## Ejemplo (demo)

```
IRQ WATCH — Linux interrupt monitor (/proc/interrupts)
  -----------------------------------------------------------------------------
  TOP INTERRUPTS (6 of 23)
   IRQ     NAME               CPU0     CPU1     CPU2     CPU3    TOTAL
    LOC  Local timer interr   12.3G     9.9G     1.2G     9.9G    33.3G
    130  PCI-MSI 327682-edg   76.5M   876.5M   987.7M   123.5M     2.1G
     24  PCI-MSI 327680-edg  123.5M   987.7M   234.6M   123.5M     1.5G
     27  PCI-MSI 524289-edg   98.8M   123.5M    87.7M   987.7M     1.3G
      0  IR-IO-APIC 2-edge   212.9M   127.4M   123.5M    98.8M   562.5M
    RES  Rescheduling inter   12.3M    23.5M    34.6M    45.7M   116.0M

  SUMMARY  irqs 23   cpus 4   total 38.9G   busiest CPU 0
```

Con `--watch 1` las columnas muestran tasas por segundo:

```
  SUMMARY  irqs 23   cpus 4   total 587.7k int/s   busiest CPU 0
```

En sistemas reales, `/proc/interrupts` siempre tiene contenido. En contenedores donde el archivo está vacío/enmascarado, el tool indica el error y `--demo` muestra el modo con datos sintéticos.

## Estructura

```
src/main.cpp   — todo el código (monolítico, stdlib-only)
```

## Notas técnicas

- El **header** de `/proc/interrupts` se detecta por tokens `CPU*`; las filas se parsean como `id:` + tokens numéricos (conteos por CPU) + resto (nombre del dispositivo). `ERR`/`MIS` tienen un único conteo total — se rellena con ceros hasta `num_cpus`.
- Las **líneas de continuación** (filas que envuelven en máquinas con muchos CPUs) se detectan por ausencia de `id:` — sus tokens numéricos completan los conteos y el resto se concatena al nombre.
- `std::from_chars` **no salta whitespace** — los tokens se trimean antes de parsear (los conteos vienen separados por espacios).
- Las **tasas delta** se calculan como `(cur - prev) / dt` entre snapshots consecutivos en modo watch; con `--demo` los contadores crecen con un LCG para que las tasas sean visibles.
- En `--watch`, la salida se flushea (`std::cout << std::flush`) al final de cada frame — con redirección a pipe/archivo y `timeout`/kill no se pierde la salida.
- El smoke test de `/proc/interrupts` real es condicional: en contenedores con el archivo vacío se reporta `SKIP`, nunca un fallo.
