#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <sys/wait.h>
#include <omp.h>
#include <pthread.h>

typedef struct Nodo {
    int clave;
    struct Nodo *siguiente;
} Nodo;

typedef struct {
    Nodo *cabeza;
    omp_lock_t global;
    pthread_rwlock_t rw;
} Lista;

enum { SECUENCIAL, SIN_SINC, GLOBAL, RWLOCK, NUM_ESTRATEGIAS };
static const char *NOMBRES[] = {"Secuencial", "Sin sincronizacion", "Bloqueo global", "Rwlock"};

enum { BUSCAR, ELIMINAR };

typedef struct {
    int tipo;
    int clave;
} Operacion;

typedef struct {
    double tiempo;
    long cantidad;
    long esperada;
    int termina;
    int ordenada;
    int presentes_ok;
    long elim_ok;
    long elim_esperadas;
    long busq_mal;
    int correcto;
} Resultado;

static unsigned long long estado_rng = 88172645463325252ULL;

static unsigned long long numero_aleatorio(void) {
    estado_rng ^= estado_rng << 13;
    estado_rng ^= estado_rng >> 7;
    estado_rng ^= estado_rng << 17;
    return estado_rng;
}

static Nodo *nuevo_nodo(int clave, Nodo *siguiente) {
    Nodo *n = malloc(sizeof(Nodo));
    n->clave = clave;
    n->siguiente = siguiente;
    return n;
}

static Lista *lista_vacia(void) {
    Lista *l = malloc(sizeof(Lista));
    l->cabeza = nuevo_nodo(INT_MIN, NULL);
    omp_init_lock(&l->global);
    pthread_rwlock_init(&l->rw, NULL);
    return l;
}

static void lista_destruir(Lista *l) {
    Nodo *p = l->cabeza;
    while (p != NULL) {
        Nodo *sig = p->siguiente;
        free(p);
        p = sig;
    }
    omp_destroy_lock(&l->global);
    pthread_rwlock_destroy(&l->rw);
    free(l);
}

/* lista inicial con las claves 0, 2, 4, ..., 2(m-1) */
static Lista *lista_inicial(int m) {
    Lista *l = lista_vacia();
    Nodo *cola = l->cabeza;
    for (int i = 0; i < m; i++) {
        cola->siguiente = nuevo_nodo(2 * i, NULL);
        cola = cola->siguiente;
    }
    return l;
}

/* operaciones secuenciales */
static int buscar_sec(Lista *l, int clave) {
    Nodo *p = l->cabeza->siguiente;
    while (p != NULL && p->clave < clave) {
        p = p->siguiente;
    }
    return p != NULL && p->clave == clave;
}

static int insertar_sec(Lista *l, int clave) {
    Nodo *prev = l->cabeza;
    Nodo *curr = prev->siguiente;
    while (curr != NULL && curr->clave < clave) {
        prev = curr;
        curr = curr->siguiente;
    }
    if (curr != NULL && curr->clave == clave) {
        return 0;
    }
    prev->siguiente = nuevo_nodo(clave, curr);
    return 1;
}

static int eliminar_sec(Lista *l, int clave, int liberar) {
    Nodo *prev = l->cabeza;
    Nodo *curr = prev->siguiente;
    while (curr != NULL && curr->clave < clave) {
        prev = curr;
        curr = curr->siguiente;
    }
    if (curr == NULL || curr->clave != clave) {
        return 0;
    }
    prev->siguiente = curr->siguiente;
    if (liberar) {
        free(curr);
    }
    return 1;
}

static long recorrer(Lista *l, long limite, int *termina, int *ordenada, char *visto, int m) {
    long cantidad = 0;
    int ultimo = INT_MIN;
    *ordenada = 1;
    Nodo *p = l->cabeza->siguiente;
    while (p != NULL && cantidad <= limite) {
        cantidad++;
        if (p->clave <= ultimo) {
            *ordenada = 0;
        }
        ultimo = p->clave;
        if (visto != NULL && p->clave >= 0 && p->clave % 2 == 0 && p->clave / 2 < m) {
            visto[p->clave / 2] = 1;
        }
        p = p->siguiente;
    }
    *termina = (p == NULL);
    return cantidad;
}

static int buscar(Lista *l, int estrategia, int clave) {
    int r = 0;
    switch (estrategia) {
    case SECUENCIAL:
    case SIN_SINC:
        r = buscar_sec(l, clave);
        break;
    case GLOBAL:
        omp_set_lock(&l->global);
        r = buscar_sec(l, clave);
        omp_unset_lock(&l->global);
        break;
    case RWLOCK:
        pthread_rwlock_rdlock(&l->rw);
        r = buscar_sec(l, clave);
        pthread_rwlock_unlock(&l->rw);
        break;
    }
    return r;
}

static int eliminar(Lista *l, int estrategia, int clave) {
    int r = 0;
    switch (estrategia) {
    case SECUENCIAL:
        r = eliminar_sec(l, clave, 1);
        break;
    case SIN_SINC:
        /* no se libera para que el programa no se caiga y se vea la corrupcion */
        r = eliminar_sec(l, clave, 0);
        break;
    case GLOBAL:
        omp_set_lock(&l->global);
        r = eliminar_sec(l, clave, 1);
        omp_unset_lock(&l->global);
        break;
    case RWLOCK:
        pthread_rwlock_wrlock(&l->rw);
        r = eliminar_sec(l, clave, 1);
        pthread_rwlock_unlock(&l->rw);
        break;
    }
    return r;
}

static void procesar(Lista *l, int estrategia, const Operacion *op, const char *eliminada,
                     long *elim_ok, long *busq_mal) {
    if (op->tipo == BUSCAR) {
        int r = buscar(l, estrategia, op->clave);
        if (op->clave % 2 != 0) {
            if (r) {
                (*busq_mal)++;
            }
        } else if (!eliminada[op->clave / 2]) {
            if (!r) {
                (*busq_mal)++;
            }
        }
    } else {
        if (eliminar(l, estrategia, op->clave)) {
            (*elim_ok)++;
        }
    }
}

static Resultado ejecutar(int estrategia, int T, const Operacion *ops, long k, int m,
                          const char *eliminada, long nelim) {
    Resultado res;
    memset(&res, 0, sizeof(res));
    Lista *l = lista_inicial(m);
    long elim_ok = 0;
    long busq_mal = 0;

    double inicio = omp_get_wtime();
    if (estrategia == SECUENCIAL) {
        for (long i = 0; i < k; i++) {
            procesar(l, estrategia, &ops[i], eliminada, &elim_ok, &busq_mal);
        }
    } else {
#pragma omp parallel for num_threads(T) schedule(dynamic, 64) reduction(+ : elim_ok, busq_mal)
        for (long i = 0; i < k; i++) {
            procesar(l, estrategia, &ops[i], eliminada, &elim_ok, &busq_mal);
        }
    }
    res.tiempo = omp_get_wtime() - inicio;

    char *visto = calloc(m, 1);
    res.cantidad = recorrer(l, m + 1, &res.termina, &res.ordenada, visto, m);
    res.esperada = m - nelim;
    res.presentes_ok = 1;
    for (int i = 0; i < m; i++) {
        if ((visto[i] != 0) == (eliminada[i] != 0)) {
            res.presentes_ok = 0;
        }
    }
    res.elim_ok = elim_ok;
    res.elim_esperadas = nelim;
    res.busq_mal = busq_mal;
    res.correcto = res.termina && res.ordenada && res.presentes_ok &&
                   res.cantidad == res.esperada && elim_ok == nelim && busq_mal == 0;
    free(visto);
    lista_destruir(l);
    return res;
}

static Resultado ejecutar_aislado(int estrategia, int T, const Operacion *ops, long k, int m,
                                  const char *eliminada, long nelim) {
    int fd[2];
    Resultado res;
    memset(&res, 0, sizeof(res));
    if (pipe(fd) != 0) {
        return ejecutar(estrategia, T, ops, k, m, eliminada, nelim);
    }
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        close(fd[0]);
        Resultado r = ejecutar(estrategia, T, ops, k, m, eliminada, nelim);
        if (write(fd[1], &r, sizeof(r)) != (ssize_t)sizeof(r)) {
            _exit(1);
        }
        _exit(0);
    }
    close(fd[1]);
    ssize_t n = read(fd[0], &res, sizeof(res));
    close(fd[0]);
    int estado = 0;
    waitpid(pid, &estado, 0);
    if (n != (ssize_t)sizeof(res)) {
        memset(&res, 0, sizeof(res));
        res.tiempo = 1e9;
        res.correcto = 0;
    }
    return res;
}

static void generar_operaciones(Operacion *ops, long k, int m, long nelim, char *eliminada) {
    int *perm = malloc(m * sizeof(int));
    for (int i = 0; i < m; i++) {
        perm[i] = i;
    }
    for (int i = m - 1; i > 0; i--) {
        int j = (int)(numero_aleatorio() % (i + 1));
        int t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
    }
    memset(eliminada, 0, m);
    long pos = 0;
    for (long j = 0; j < nelim; j++) {
        eliminada[perm[j]] = 1;
        ops[pos].tipo = ELIMINAR;
        ops[pos].clave = 2 * perm[j];
        pos++;
    }
    while (pos < k) {
        ops[pos].tipo = BUSCAR;
        ops[pos].clave = (int)(numero_aleatorio() % (2 * (unsigned long long)m));
        pos++;
    }
    for (long i = k - 1; i > 0; i--) {
        long j = (long)(numero_aleatorio() % (i + 1));
        Operacion t = ops[i];
        ops[i] = ops[j];
        ops[j] = t;
    }
    free(perm);
}

static void imprimir_lista(Lista *l) {
    printf("[");
    for (Nodo *p = l->cabeza->siguiente; p != NULL; p = p->siguiente) {
        printf("%d%s", p->clave, p->siguiente ? ", " : "");
    }
    printf("]\n");
}

static void demo_secuencial(void) {
    printf("demo secuencial\n");
    Lista *l = lista_vacia();
    int claves[] = {40, 10, 30, 20, 50, 30};
    for (int i = 0; i < 6; i++) {
        int r = insertar_sec(l, claves[i]);
        printf("insertar(%d) -> %d  lista: ", claves[i], r);
        imprimir_lista(l);
    }
    printf("buscar(20) -> %d\n", buscar_sec(l, 20));
    printf("buscar(25) -> %d\n", buscar_sec(l, 25));
    printf("eliminar(20) -> %d  lista: ", eliminar_sec(l, 20, 1));
    imprimir_lista(l);
    printf("eliminar(99) -> %d  lista: ", eliminar_sec(l, 99, 1));
    imprimir_lista(l);
    printf("buscar(20) -> %d\n", buscar_sec(l, 20));
    int termina, ordenada;
    long c = recorrer(l, 100, &termina, &ordenada, NULL, 0);
    printf("recorrido: nodos=%ld termina=%d ordenada=%d\n", c, termina, ordenada);
    lista_destruir(l);
}

int main(int argc, char **argv) {
    int m = 10000;
    long k = 20000;
    int reps = 3;
    if (argc > 1) m = atoi(argv[1]);
    if (argc > 2) k = atol(argv[2]);
    if (argc > 3) reps = atoi(argv[3]);

    int hilos[] = {1, 2, 4, 8};
    int pct_elim[] = {5, 25, 50};
    int n_hilos = 4;
    int n_pct = 3;

    printf("parametros\n");
    printf("nodos iniciales M=%d  operaciones K=%ld  repeticiones=%d\n", m, k, reps);
    demo_secuencial();

    Operacion *ops = malloc(k * sizeof(Operacion));
    char *eliminada = malloc(m);

    printf("resultados\n");
    for (int p = 0; p < n_pct; p++) {
        long nelim = k * pct_elim[p] / 100;
        if (nelim > m) nelim = m;
        estado_rng = 88172645463325252ULL;
        generar_operaciones(ops, k, m, nelim, eliminada);
        printf("\neliminaciones=%d%% busquedas=%d%% (elim=%ld busq=%ld)\n", pct_elim[p],
               100 - pct_elim[p], nelim, k - nelim);

        ejecutar_aislado(SECUENCIAL, 1, ops, k, m, eliminada, nelim);
        double t_sec = 0;
        for (int e = 0; e < NUM_ESTRATEGIAS; e++) {
            int nt = (e == SECUENCIAL) ? 1 : n_hilos;
            for (int h = 0; h < nt; h++) {
                int T = (e == SECUENCIAL) ? 1 : hilos[h];
                double suma = 0;
                int todo_ok = 1;
                Resultado ult;
                for (int r = 0; r < reps; r++) {
                    ult = ejecutar_aislado(e, T, ops, k, m, eliminada, nelim);
                    suma += ult.tiempo;
                    if (!ult.correcto) todo_ok = 0;
                }
                double t = suma / reps;
                if (e == SECUENCIAL) t_sec = t;
                double sp = t_sec / t;
                printf("%-19s T=%d tiempo=%.6f speedup=%.3f efic=%.3f | nodos=%ld esperados=%ld "
                       "elim_ok=%ld/%ld busq_mal=%ld termina=%d ordenada=%d presentes_ok=%d | "
                       "correcto=%s\n",
                       NOMBRES[e], T, t, sp, sp / T, ult.cantidad, ult.esperada, ult.elim_ok,
                       ult.elim_esperadas, ult.busq_mal, ult.termina, ult.ordenada,
                       ult.presentes_ok, todo_ok ? "Si" : "No");
                fflush(stdout);
            }
        }
    }
    free(ops);
    free(eliminada);
    return 0;
}
