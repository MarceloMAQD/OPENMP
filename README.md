# Laboratorio OpenMP: lista enlazada y Odd-Even Sort

Curso de Computación Paralela y Distribuida, UNSA.

Dos programas en C con OpenMP, cada uno con su versión secuencial y su versión paralela.

## Archivos

| Archivo | Qué hace |
|---|---|
| `parte1_lista.c` | Lista enlazada con búsquedas y eliminaciones concurrentes. Compara tres estrategias: sin sincronización, bloqueo global y rwlock. |
| `parte2_oddeven.c` | Odd-Even Transposition Sort paralelo. Compara las políticas de planificación `static`, `dynamic` y `guided`. |

## Compilación

```bash
gcc -O2 -fopenmp -pthread parte1_lista.c -o parte1_lista
gcc -O2 -fopenmp parte2_oddeven.c -o parte2_oddeven
```

## Ejecución

```bash
./parte1_lista
./parte2_oddeven
```
