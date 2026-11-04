#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>

static unsigned long long estado_rng = 88172645463325252ULL;

static unsigned long long numero_aleatorio(void) {
    estado_rng ^= estado_rng << 13;
    estado_rng ^= estado_rng >> 7;
    estado_rng ^= estado_rng << 17;
    return estado_rng;
}

static void intercambiar(int *a, int *b) {
    int t = *a;
    *a = *b;
    *b = t;
}

static void oddeven_secuencial(int *v, long n, int verbose) {
    for (long fase = 0; fase < n; fase++) {
        long inicio = fase % 2;
        for (long i = inicio; i + 1 < n; i += 2) {
            if (v[i] > v[i + 1]) {
                intercambiar(&v[i], &v[i + 1]);
            }
        }
        if (verbose) {
            printf("fase %ld (%s):", fase + 1, inicio == 0 ? "par" : "impar");
            for (long i = 0; i < n; i++) printf(" %d", v[i]);
            printf("\n");
        }
    }
}

static void oddeven_paralelo(int *v, long n, int T) {
#pragma omp parallel num_threads(T) shared(v, n)
    {
        for (long fase = 0; fase < n; fase++) {
            long inicio = fase % 2;
            long pares = (n - inicio) / 2;
#pragma omp for schedule(runtime)
            for (long j = 0; j < pares; j++) {
                long i = inicio + 2 * j;
                if (v[i] > v[i + 1]) {
                    intercambiar(&v[i], &v[i + 1]);
                }
            }
        }
    }
}

/* version incorrecta a proposito: sin barrera entre fases (nowait) */
static void oddeven_sin_barrera(int *v, long n, int T) {
#pragma omp parallel num_threads(T) shared(v, n)
    {
        for (long fase = 0; fase < n; fase++) {
            long inicio = fase % 2;
            long pares = (n - inicio) / 2;
#pragma omp for schedule(static) nowait
            for (long j = 0; j < pares; j++) {
                long i = inicio + 2 * j;
                if (v[i] > v[i + 1]) {
                    intercambiar(&v[i], &v[i + 1]);
                }
            }
        }
    }
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a;
    int y = *(const int *)b;
    return (x > y) - (x < y);
}

static int esta_ordenado(const int *v, long n) {
    for (long i = 0; i + 1 < n; i++) {
        if (v[i] > v[i + 1]) return 0;
    }
    return 1;
}

static int mismos_elementos(const int *res, const int *orig, long n) {
    int *c = malloc(n * sizeof(int));
    memcpy(c, orig, n * sizeof(int));
    qsort(c, n, sizeof(int), cmp_int);
    int ok = memcmp(c, res, n * sizeof(int)) == 0;
    free(c);
    return ok;
}

enum { ALEATORIO, DUPLICADOS, INVERSO, ORDENADO };
static const char *NOM_DATOS[] = {"aleatorio", "duplicados (0-9)", "orden inverso", "ya ordenado"};

static void generar(int *v, long n, int tipo) {
    for (long i = 0; i < n; i++) {
        switch (tipo) {
        case ALEATORIO: v[i] = (int)(numero_aleatorio() % 1000000); break;
        case DUPLICADOS: v[i] = (int)(numero_aleatorio() % 10); break;
        case INVERSO: v[i] = (int)(n - i); break;
        default: v[i] = (int)i; break;
        }
    }
}

static void demo_fases(void) {
    printf("demo de fases (n=8 y n=7)\n");
    int a[8] = {7, 3, 8, 1, 6, 2, 5, 4};
    int b[7] = {5, 1, 4, 2, 7, 3, 6};
    printf("n=8 inicial:");
    for (int i = 0; i < 8; i++) printf(" %d", a[i]);
    printf("\n");
    oddeven_secuencial(a, 8, 1);
    printf("n=7 inicial:");
    for (int i = 0; i < 7; i++) printf(" %d", b[i]);
    printf("\n");
    oddeven_secuencial(b, 7, 1);
}

static void pruebas_correctitud(void) {
    printf("correctitud\n");
    long tams[] = {1, 2, 3, 100, 1001, 2000, 2001};
    int hilos[] = {1, 2, 4, 8};
    for (int d = 0; d < 4; d++) {
        for (int s = 0; s < 7; s++) {
            long n = tams[s];
            int *orig = malloc(n * sizeof(int));
            generar(orig, n, d);
            int *v = malloc(n * sizeof(int));
            memcpy(v, orig, n * sizeof(int));
            oddeven_secuencial(v, n, 0);
            int ok_sec = esta_ordenado(v, n) && mismos_elementos(v, orig, n);
            int ok_par = 1;
            for (int h = 0; h < 4; h++) {
                memcpy(v, orig, n * sizeof(int));
                oddeven_paralelo(v, n, hilos[h]);
                if (!(esta_ordenado(v, n) && mismos_elementos(v, orig, n))) ok_par = 0;
            }
            printf("datos=%-17s n=%-5ld (%s) secuencial=%s paralelo(1,2,4,8 hilos)=%s\n",
                   NOM_DATOS[d], n, n % 2 == 0 ? "par" : "impar", ok_sec ? "Si" : "No",
                   ok_par ? "Si" : "No");
            free(v);
            free(orig);
        }
    }
}

static void demo_sin_barrera(void) {
    printf("demo sin barrera entre fases (nowait)\n");
    long n = 20000;
    int *orig = malloc(n * sizeof(int));
    int *v = malloc(n * sizeof(int));
    generar(orig, n, ALEATORIO);
    for (int h = 0; h < 3; h++) {
        int T = h == 0 ? 2 : (h == 1 ? 4 : 8);
        int fallos = 0;
        for (int r = 0; r < 5; r++) {
            memcpy(v, orig, n * sizeof(int));
            oddeven_sin_barrera(v, n, T);
            if (!(esta_ordenado(v, n) && mismos_elementos(v, orig, n))) fallos++;
        }
        printf("n=%ld T=%d: %d de 5 ejecuciones salieron mal\n", n, T, fallos);
    }
    free(v);
    free(orig);
}

static double medir_sec(const int *orig, int *v, long n, int reps) {
    double suma = 0;
    for (int r = 0; r < reps; r++) {
        memcpy(v, orig, n * sizeof(int));
        double t0 = omp_get_wtime();
        oddeven_secuencial(v, n, 0);
        suma += omp_get_wtime() - t0;
    }
    return suma / reps;
}

int main(int argc, char **argv) {
    long tams[16] = {2000, 10000, 40000};
    int ntams = 3;
    int reps = 3;
    if (argc > 1) {
        ntams = 0;
        for (int i = 1; i < argc && i <= 15; i++) tams[ntams++] = atol(argv[i]);
    }
    if (getenv("REPS") != NULL) reps = atoi(getenv("REPS"));

    int hilos[] = {1, 2, 4, 8};

    printf("parametros\n");
    printf("tamanos:");
    for (int i = 0; i < ntams; i++) printf(" %ld", tams[i]);
    printf("  repeticiones=%d\n", reps);

    demo_fases();
    pruebas_correctitud();
    demo_sin_barrera();

    printf("resultados\n");
    omp_sched_t tipos[] = {omp_sched_static, omp_sched_dynamic, omp_sched_guided};
    int chunks[] = {0, 64, 0};
    const char *nom_sched[] = {"static", "dynamic,64", "guided"};

    for (int d = 0; d < 2; d++) {
        int tipo = d == 0 ? ALEATORIO : INVERSO;
        for (int s = 0; s < ntams; s++) {
            long n = tams[s];
            int *orig = malloc(n * sizeof(int));
            int *v = malloc(n * sizeof(int));
            estado_rng = 88172645463325252ULL;
            generar(orig, n, tipo);
            double t_sec = medir_sec(orig, v, n, reps);
            printf("\ndatos=%s n=%ld secuencial=%.6f\n", NOM_DATOS[tipo], n, t_sec);
            for (int p = 0; p < 3; p++) {
                if (d == 1 && p > 0) continue;
                omp_set_schedule(tipos[p], chunks[p]);
                for (int h = 0; h < 4; h++) {
                    double suma = 0;
                    int ok = 1;
                    for (int r = 0; r < reps; r++) {
                        memcpy(v, orig, n * sizeof(int));
                        double t0 = omp_get_wtime();
                        oddeven_paralelo(v, n, hilos[h]);
                        suma += omp_get_wtime() - t0;
                        if (!(esta_ordenado(v, n) && mismos_elementos(v, orig, n))) ok = 0;
                    }
                    double t = suma / reps;
                    double sp = t_sec / t;
                    printf("%-10s T=%d tiempo=%.6f speedup=%.3f efic=%.3f correcto=%s\n",
                           nom_sched[p], hilos[h], t, sp, sp / hilos[h], ok ? "Si" : "No");
                    fflush(stdout);
                }
            }
            free(v);
            free(orig);
        }
    }
    return 0;
}
