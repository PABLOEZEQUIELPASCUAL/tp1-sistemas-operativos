/*
 * tp1_ej1_streaming_fixed.c
 *
 * Versión final: pipeline concurrente con manejo de Ctrl+C (SIGINT),
 * buffers acotados de tamaño 3 entre cada etapa, y lectura de un lote
 * más grande desde formularios.txt.
 *
 * Compilar en Ubuntu (o cualquier Linux con GCC):
 *   gcc -o tp1_ej1_streaming_fixed tp1_ej1_streaming_fixed.c -lrt
 *
 * Ejecutar:
 *   ./tp1_ej1_streaming_fixed
 *
 * Durante la ejecución:
 *   - En otra terminal podés usar `ps aux | grep tp1_ej1_streaming_fixed`
 *     para ver los procesos (padre + 4 hijos).
 *   - Si presionás Ctrl+C, el programa imprimirá los resultados procesados
 *     hasta ese momento y liberará los recursos IPC antes de salir.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#define MAX_FORMULARIOS 100
#define BUF_SIZE 3
#define PATH_FORMULARIOS "formularios.txt"

/* Índices de semáforos (9 en total, 3 por cada buffer) */
#define SEM_EMPTY_CV 0   /* espacios libres en buf_cv  (cargar → validar) */
#define SEM_FULL_CV  1   /* elementos disponibles en buf_cv */
#define SEM_MUTEX_CV 2   /* mutex para buf_cv                */

#define SEM_EMPTY_VE 3   /* espacios libres en buf_ve (validar → encriptar) */
#define SEM_FULL_VE  4   /* elementos disponibles en buf_ve */
#define SEM_MUTEX_VE 5   /* mutex para buf_ve               */

#define SEM_EMPTY_EC 6   /* espacios libres en buf_ec (encriptar → clasificar) */
#define SEM_FULL_EC  7   /* elementos disponibles en buf_ec */
#define SEM_MUTEX_EC 8   /* mutex para buf_ec               */

/* Clave IPC para ftok */
#define FTOK_PATH "/tmp"
#define FTOK_ID   'S'

typedef struct {
    int id;                     /* id = -1 → formulario sentinel */
    long int dni;
    char nombre[30];
    char apellido[30];
    char fechaNac[11];          /* Formato "DD/MM/YYYY" */
    char nroTelefono[20];
    char tipoForm[20];          /* Se completará en “clasificar” */
    char descripcion[200];
} Formulario;

/* Estructura en memoria compartida */
typedef struct {
    /* Buffer CV: cargar → validar */
    Formulario buf_cv[BUF_SIZE];
    int in_cv, out_cv;

    /* Buffer VE: validar → encriptar */
    Formulario buf_ve[BUF_SIZE];
    int in_ve, out_ve;

    /* Buffer EC: encriptar → clasificar */
    Formulario buf_ec[BUF_SIZE];
    int in_ec, out_ec;

    /* Resultados finales (almacén de formularios ya clasificados) */
    Formulario resultados[MAX_FORMULARIOS];
    int countResultados;
} DatosCompartidos;

/* Variables globales IPC */
int shmid = -1;
int semid = -1;
DatosCompartidos *datos = NULL;

/* Prototipos */
struct sembuf P(int sem);
struct sembuf V(int sem);
void invertir_cadena(char *s);
void cargar_formularios();
void validar_formularios();
void encriptar_formularios();
void clasificar_formularios();
void quitar_ipc();
void manejar_sigint(int sig);

/* Las funciones P (wait) y V (signal) devuelven un struct sembuf por valor */
struct sembuf P(int sem) {
    struct sembuf op;
    op.sem_num = sem;
    op.sem_op  = -1;
    op.sem_flg = 0;
    return op;
}
struct sembuf V(int sem) {
    struct sembuf op;
    op.sem_num = sem;
    op.sem_op  = +1;
    op.sem_flg = 0;
    return op;
}

/* Invierte una cadena in-place (usado en “encriptar”) */
void invertir_cadena(char *s) {
    size_t len = strlen(s);
    for (size_t i = 0; i < len/2; i++) {
        char tmp = s[i];
        s[i] = s[len - 1 - i];
        s[len - 1 - i] = tmp;
    }
}

/* Maneja Ctrl+C en el proceso padre */
void manejar_sigint(int sig) {
    (void)sig;  // evitar advertencia
    printf("\n\n[!] Interrupción recibida (Ctrl+C)\n");

    if (datos) {
        printf("\n--- Resultados parciales (%d formularios) ---\n",
               datos->countResultados);
        for (int i = 0; i < datos->countResultados; i++) {
            Formulario *f = &datos->resultados[i];
            printf("ID:%3d | DNI(encriptado):%8ld | Nombre: %-10s %-10s | FechaNac:%10s | Tel(encriptado):%-10s | Tipo:%-8s | Desc:%s\n",
                   f->id, f->dni,
                   f->nombre, f->apellido,
                   f->fechaNac,
                   f->nroTelefono,
                   f->tipoForm,
                   f->descripcion);
        }
    }

    quitar_ipc();
    printf("[!] Recursos IPC liberados. Saliendo.\n");
    exit(EXIT_SUCCESS);
}

int main() {
    /* Instalar manejador para Ctrl+C */
    signal(SIGINT, manejar_sigint);

    key_t key = ftok(FTOK_PATH, FTOK_ID);
    if (key == -1) {
        perror("ftok");
        exit(EXIT_FAILURE);
    }

    /* 1) Crear y adjuntar memoria compartida */
    shmid = shmget(key, sizeof(DatosCompartidos), IPC_CREAT | 0666);
    if (shmid < 0) {
        perror("shmget");
        exit(EXIT_FAILURE);
    }
    datos = (DatosCompartidos *) shmat(shmid, NULL, 0);
    if (datos == (void *) -1) {
        perror("shmat");
        shmctl(shmid, IPC_RMID, NULL);
        exit(EXIT_FAILURE);
    }

    /* Inicializar índices y contador de resultados */
    datos->in_cv   = datos->out_cv   = 0;
    datos->in_ve   = datos->out_ve   = 0;
    datos->in_ec   = datos->out_ec   = 0;
    datos->countResultados = 0;

    /* 2) Crear 9 semáforos */
    semid = semget(key, 9, IPC_CREAT | 0666);
    if (semid < 0) {
        perror("semget");
        shmdt(datos);
        shmctl(shmid, IPC_RMID, NULL);
        exit(EXIT_FAILURE);
    }

    /* 3) Inicializar valores de semáforos: sem_empty = 3, sem_full = 0, sem_mutex = 1 */
    unsigned short init_vals[9] = {
        /* CV */ BUF_SIZE, 0, 1,
        /* VE */ BUF_SIZE, 0, 1,
        /* EC */ BUF_SIZE, 0, 1
    };
    if (semctl(semid, 0, SETALL, init_vals) < 0) {
        perror("semctl SETALL");
        quitar_ipc();
        exit(EXIT_FAILURE);
    }

    /* 4) Crear los 4 procesos hijos */
    for (int i = 0; i < 4; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            quitar_ipc();
            exit(EXIT_FAILURE);
        }
        if (pid == 0) {
            /* Cada hijo hereda “datos” y “semid” */
            switch (i) {
                case 0:  cargar_formularios();    break;
                case 1:  validar_formularios();   break;
                case 2:  encriptar_formularios(); break;
                case 3:  clasificar_formularios();break;
            }
            /* No debe llegar aquí, cada función hace exit() */
            exit(EXIT_SUCCESS);
        }
        /* El padre continúa al siguiente fork() */
    }

    /* 5) Padre espera a que terminen los 4 hijos */
    for (int i = 0; i < 4; i++) {
        wait(NULL);
    }

    /* 6) Todos los hijos terminaron; el padre imprime resultados */
    printf("\n--- Resultados finales (%d formularios) ---\n",
           datos->countResultados);
    for (int i = 0; i < datos->countResultados; i++) {
        Formulario *f = &datos->resultados[i];
        printf("ID:%3d | DNI(encriptado):%8ld | Nombre: %-10s %-10s | FechaNac:%10s | Tel(encriptado):%-10s | Tipo:%-8s | Desc:%s\n",
               f->id, f->dni,
               f->nombre, f->apellido,
               f->fechaNac,
               f->nroTelefono,
               f->tipoForm,
               f->descripcion);
    }

    /* 7) Limpiar IPC */
    quitar_ipc();
    return 0;
}

/* -----------------------------------------------
   Hijo 0: cargar_formularios()
   - Lee “formularios.txt” línea a línea,
     guarda en arreglo temporal y produce en buf_cv.
   - Finalmente envía formulario sentinel (id = -1).
   ----------------------------------------------- */
void cargar_formularios() {
    FILE *fp = fopen(PATH_FORMULARIOS, "r");
    if (!fp) {
        perror("fopen formularios.txt");
        /* Aunque falle la apertura, enviaremos solo el sentinel */
    }

    /* 1) Leer TODO el archivo a un arreglo temporal */
    Formulario temp[MAX_FORMULARIOS];
    int total_leidos = 0;
    char linea[512];

    while (fp && fgets(linea, sizeof(linea), fp) != NULL && total_leidos < MAX_FORMULARIOS) {
        Formulario f;
        char *token;

        /* Parsear CSV: id,dni,nombre,apellido,fechaNac,nroTelefono,descripcion */
        token = strtok(linea, ",");
        if (!token) continue;
        f.id = atoi(token);

        token = strtok(NULL, ",");  if (!token) continue;
        f.dni = atol(token);

        token = strtok(NULL, ",");  if (!token) continue;
        strncpy(f.nombre, token, sizeof(f.nombre)-1);
        f.nombre[sizeof(f.nombre)-1] = '\0';

        token = strtok(NULL, ",");  if (!token) continue;
        strncpy(f.apellido, token, sizeof(f.apellido)-1);
        f.apellido[sizeof(f.apellido)-1] = '\0';

        token = strtok(NULL, ",");  if (!token) continue;
        strncpy(f.fechaNac, token, sizeof(f.fechaNac)-1);
        f.fechaNac[sizeof(f.fechaNac)-1] = '\0';

        token = strtok(NULL, ",");  if (!token) continue;
        strncpy(f.nroTelefono, token, sizeof(f.nroTelefono)-1);
        f.nroTelefono[sizeof(f.nroTelefono)-1] = '\0';

        token = strtok(NULL, "\n");
        if (!token) token = "";
        strncpy(f.descripcion, token, sizeof(f.descripcion)-1);
        f.descripcion[sizeof(f.descripcion)-1] = '\0';

        /* Inicializar tipoForm vacío */
        f.tipoForm[0] = '\0';

        temp[total_leidos++] = f;
    }

    if (fp) fclose(fp);

    printf(">> [CARGAR] Leídos %d formularios en etapa 1 (arreglo temporal)\n", total_leidos);

    /* 2) Producir uno a uno en buf_cv */
    for (int i = 0; i < total_leidos; i++) {
        Formulario f = temp[i];
        struct sembuf op;

        /* 2.1) Esperar espacio libre en buf_cv */
        op = P(SEM_EMPTY_CV);
        semop(semid, &op, 1);

        /* 2.2) Entrar sección crítica buf_cv */
        op = P(SEM_MUTEX_CV);
        semop(semid, &op, 1);

        /* 2.3) Escribir en buf_cv[in_cv] */
        datos->buf_cv[datos->in_cv] = f;
        datos->in_cv = (datos->in_cv + 1) % BUF_SIZE;

        /* 2.4) Salir sección crítica buf_cv */
        op = V(SEM_MUTEX_CV);
        semop(semid, &op, 1);

        /* 2.5) Señalar que hay un formulario listo en buf_cv */
        op = V(SEM_FULL_CV);
        semop(semid, &op, 1);

        printf(">> [CARGAR] Formulario ID %d producido en buf_cv.\n", f.id);
        sleep(1);  /* para poder visualizar la concurrencia */
    }

    /* 3) Enviar sentinel (id = -1) */
    Formulario sentinel;
    sentinel.id = -1;

    {
        struct sembuf op;
        op = P(SEM_EMPTY_CV);
        semop(semid, &op, 1);

        op = P(SEM_MUTEX_CV);
        semop(semid, &op, 1);

        datos->buf_cv[datos->in_cv] = sentinel;
        datos->in_cv = (datos->in_cv + 1) % BUF_SIZE;

        op = V(SEM_MUTEX_CV);
        semop(semid, &op, 1);

        op = V(SEM_FULL_CV);
        semop(semid, &op, 1);
    }

    printf(">> [CARGAR] Sentinel enviado. Etapa CARGAR finalizada.\n");
    exit(EXIT_SUCCESS);
}

/* -----------------------------------------------
   Hijo 1: validar_formularios()
   - Consume de buf_cv, valida y produce en buf_ve.
   - Propaga sentinel al detectar id = -1.
   ----------------------------------------------- */
void validar_formularios() {
    while (1) {
        Formulario f;
        struct sembuf op;

        /* Consumir de buf_cv */
        op = P(SEM_FULL_CV);
        semop(semid, &op, 1);

        op = P(SEM_MUTEX_CV);
        semop(semid, &op, 1);

        f = datos->buf_cv[datos->out_cv];
        datos->out_cv = (datos->out_cv + 1) % BUF_SIZE;

        op = V(SEM_MUTEX_CV);
        semop(semid, &op, 1);

        op = V(SEM_EMPTY_CV);
        semop(semid, &op, 1);

        /* Si es sentinel, propagar y terminar */
        if (f.id == -1) {
            op = P(SEM_EMPTY_VE);
            semop(semid, &op, 1);

            op = P(SEM_MUTEX_VE);
            semop(semid, &op, 1);

            datos->buf_ve[datos->in_ve] = f;
            datos->in_ve = (datos->in_ve + 1) % BUF_SIZE;

            op = V(SEM_MUTEX_VE);
            semop(semid, &op, 1);

            op = V(SEM_FULL_VE);
            semop(semid, &op, 1);

            printf(">> [VALIDAR] Sentinel detectado. Saliendo.\n");
            break;
        }

        /* Validar campos: si hay error, avisar pero producir igual */
        int error = 0;
        if (f.dni <= 0)                error = 1;
        if (strlen(f.nombre) == 0)     error = 1;
        if (strlen(f.apellido) == 0)   error = 1;
        if (strlen(f.fechaNac) == 0)   error = 1;
        if (strlen(f.nroTelefono) == 0)error = 1;
        if (strlen(f.descripcion) == 0)error = 1;

        if (error) {
            printf(">> [VALIDAR] Formulario ID %d inválido.\n", f.id);
        } else {
            printf(">> [VALIDAR] Formulario ID %d válido.\n", f.id);
        }

        /* Producir en buf_ve */
        op = P(SEM_EMPTY_VE);
        semop(semid, &op, 1);

        op = P(SEM_MUTEX_VE);
        semop(semid, &op, 1);

        datos->buf_ve[datos->in_ve] = f;
        datos->in_ve = (datos->in_ve + 1) % BUF_SIZE;

        op = V(SEM_MUTEX_VE);
        semop(semid, &op, 1);

        op = V(SEM_FULL_VE);
        semop(semid, &op, 1);
    }

    exit(EXIT_SUCCESS);
}

/* -----------------------------------------------
   Hijo 2: encriptar_formularios()
   - Consume de buf_ve, encripta (invirtiendo DNI y teléfono),
     y produce en buf_ec.
   - Propaga sentinel al detectar id = -1.
   ----------------------------------------------- */
void encriptar_formularios() {
    while (1) {
        Formulario f;
        struct sembuf op;

        /* Consumir de buf_ve */
        op = P(SEM_FULL_VE);
        semop(semid, &op, 1);

        op = P(SEM_MUTEX_VE);
        semop(semid, &op, 1);

        f = datos->buf_ve[datos->out_ve];
        datos->out_ve = (datos->out_ve + 1) % BUF_SIZE;

        op = V(SEM_MUTEX_VE);
        semop(semid, &op, 1);

        op = V(SEM_EMPTY_VE);
        semop(semid, &op, 1);

        /* Si es sentinel, propagar y terminar */
        if (f.id == -1) {
            op = P(SEM_EMPTY_EC);
            semop(semid, &op, 1);

            op = P(SEM_MUTEX_EC);
            semop(semid, &op, 1);

            datos->buf_ec[datos->in_ec] = f;
            datos->in_ec = (datos->in_ec + 1) % BUF_SIZE;

            op = V(SEM_MUTEX_EC);
            semop(semid, &op, 1);

            op = V(SEM_FULL_EC);
            semop(semid, &op, 1);

            printf(">> [ENCRIPTAR] Sentinel detectado. Saliendo.\n");
            break;
        }

        /* Encriptar */
        {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%ld", f.dni);
            invertir_cadena(buffer);
            f.dni = atol(buffer);
        }
        invertir_cadena(f.nroTelefono);
        printf(">> [ENCRIPTAR] Formulario ID %d encriptado.\n", f.id);

        /* Producir en buf_ec */
        op = P(SEM_EMPTY_EC);
        semop(semid, &op, 1);

        op = P(SEM_MUTEX_EC);
        semop(semid, &op, 1);

        datos->buf_ec[datos->in_ec] = f;
        datos->in_ec = (datos->in_ec + 1) % BUF_SIZE;

        op = V(SEM_MUTEX_EC);
        semop(semid, &op, 1);

        op = V(SEM_FULL_EC);
        semop(semid, &op, 1);
    }

    exit(EXIT_SUCCESS);
}

/* -----------------------------------------------
   Hijo 3: clasificar_formularios()
   - Consume de buf_ec, clasifica según “descripcion”
     y guarda en resultados[].
   - Termina al detectar sentinel (id = -1).
   ----------------------------------------------- */
void clasificar_formularios() {
    while (1) {
        Formulario f;
        struct sembuf op;

        /* Consumir de buf_ec */
        op = P(SEM_FULL_EC);
        semop(semid, &op, 1);

        op = P(SEM_MUTEX_EC);
        semop(semid, &op, 1);

        f = datos->buf_ec[datos->out_ec];
        datos->out_ec = (datos->out_ec + 1) % BUF_SIZE;

        op = V(SEM_MUTEX_EC);
        semop(semid, &op, 1);

        op = V(SEM_EMPTY_EC);
        semop(semid, &op, 1);

        /* Si es sentinel, terminar */
        if (f.id == -1) {
            printf(">> [CLASIFICAR] Sentinel detectado. Saliendo.\n");
            break;
        }

        /* Clasificar */
        if (strstr(f.descripcion, "reclamo") != NULL 
         || strstr(f.descripcion, "Reclamo") != NULL) {
            strncpy(f.tipoForm, "Reclamo", sizeof(f.tipoForm)-1);
            f.tipoForm[sizeof(f.tipoForm)-1] = '\0';
        }
        else if (strstr(f.descripcion, "pedido") != NULL 
              || strstr(f.descripcion, "Pedido") != NULL) {
            strncpy(f.tipoForm, "Pedido", sizeof(f.tipoForm)-1);
            f.tipoForm[sizeof(f.tipoForm)-1] = '\0';
        }
        else if (strstr(f.descripcion, "consulta") != NULL 
              || strstr(f.descripcion, "Consulta") != NULL) {
            strncpy(f.tipoForm, "Consulta", sizeof(f.tipoForm)-1);
            f.tipoForm[sizeof(f.tipoForm)-1] = '\0';
        }
        else {
            strncpy(f.tipoForm, "Otros", sizeof(f.tipoForm)-1);
            f.tipoForm[sizeof(f.tipoForm)-1] = '\0';
        }

        printf(">> [CLASIFICAR] Formulario ID %d clasificado como %s.\n",
               f.id, f.tipoForm);

        /* Guardar en resultados[] */
        int idx = datos->countResultados;
        if (idx < MAX_FORMULARIOS) {
            datos->resultados[idx] = f;
            datos->countResultados++;
        } else {
            fprintf(stderr,
                ">> [CLASIFICAR] ¡Capacidad excedida! Descartando ID %d.\n",
                f.id);
        }
    }

    exit(EXIT_SUCCESS);
}

/* -----------------------------------------------
   Función para liberar memoria compartida y semáforos
   ----------------------------------------------- */
void quitar_ipc() {
    if (datos != NULL) {
        shmdt(datos);
        datos = NULL;
    }
    if (shmid >= 0) {
        shmctl(shmid, IPC_RMID, NULL);
        shmid = -1;
    }
    if (semid >= 0) {
        semctl(semid, 0, IPC_RMID);
        semid = -1;
    }
}
