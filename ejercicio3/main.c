// Includes
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
// Constantes y estructuras
#define MAX_FORMULARIOS 100
#define SHM_KEY 1234 //Identificador unico para memoria compartida. Usado por shmget
#define SEM_KEY 5678 //Identificador unico para el conjunto de semáforos. Usado por semget
#define NUM_SEMS 4 //Cantidad de semaforos que se usan.

#define SEM_CARGAR     0
#define SEM_VALIDAR    1
#define SEM_ENCRIPTAR  2
#define SEM_CLASIFICAR 3
#define NUM_HIJOS 4

#define DEBUG_SLEEP() //sleep(1) // Para simular procesamiento, se puede comentar al ejecutar tambien.

volatile sig_atomic_t terminar = 0; // Flag global para señal. 
                                    //volatile le dice al compilador que la variable puede cambiar en cualquier momento
                                    //sig_atomic_t es un tipo entero que garantiza operaciones atómicas, o sea que se puede leer/escribir sin riesgo de 
                                    //interrumpir el proceso a la mitad
                                    //Sirve como bandera que cambia dentro del handler para que el padre se entere de forma segura.
typedef struct 
{
    int id;
    long int dni;
    char nombre[30];
    char apellido[30];
    char fechaNac[20];
    char nroTelefono[20];
    char tipoForm[20];
    char descripcion[200];
} Formulario;

typedef struct 
{
    int cantidad;
    Formulario formularios[MAX_FORMULARIOS];

    int totalProcesados;
    int cantidadReclamos;
    int cantidadPedidos;
    int cantidadConsultas;
    int cantidadOtros;

    volatile int finalizar; //volatile le dice al compilador que la variable puede cambiar en cualquier momento
    volatile int ultimo; // Indica si se llegó al final del archivo
} DatosCompartidos;

// Variables globales necesarias para señales
int shmid, semid;
DatosCompartidos* datos;

// Declaraciones prototipo funciones
void cargarFormulario(DatosCompartidos* datos, int semid);
void validarFormulario(DatosCompartidos* datos, int semid);
void encriptarFormulario(DatosCompartidos* datos, int semid);
void clasificarFormulario(DatosCompartidos* datos, int semid);
void cifradoCesar(char* texto, int desplazamiento);

void P(int semid, int semnum);
void V(int semid, int semnum);

DatosCompartidos* inicializar_memoria_compartida(int* shmid);
void liberar_memoria(int shmid, DatosCompartidos* datos);

int crear_semaforos();
void liberar_semaforos(int semid);

void crear_hijos(int shmid, int semid, pid_t pids[]);

void finalizar();
void handler_SIGINT(int sig);


// Funciones auxiliares: P, V, crear memoria, etc.
void P(int semid, int semnum) 
{
    struct sembuf op = {semnum, -1, 0};
    semop(semid, &op, 1);
}

void V(int semid, int semnum)
{
    struct sembuf op = {semnum, 1, 0};
    semop(semid, &op, 1);
}

//Otros
void finalizar() 
{
    datos->finalizar = 1;
    for (int i = 0; i < NUM_SEMS; i++) 
        V(semid, i); // Desbloquear hijos
}

void handler_SIGINT(int sig) 
{
    (void)sig; // Evitar warning de parámetro no usado
    terminar = 1;
}

int esSoloLetras(char* texto) 
{
    for (int i = 0; texto[i] != '\0'; i++) 
    {
        if (!isalpha(texto[i]))
            return 0;
    }
    return 1;
}

int esSoloNumeros(char* texto) 
{
    for (int i = 0; texto[i] != '\0'; i++) {
        if (!isdigit((unsigned char)texto[i]))
            return 0;
    }
    return 1;
}

//Cifrado
void cifradoCesar(char* texto, int desplazamiento)
{
    for (int i = 0; texto[i] != '\0'; i++) {
        if (texto[i] >= 'A' && texto[i] <= 'Z')
            texto[i] = 'A' + (texto[i] - 'A' + desplazamiento) % 26;
        else if (texto[i] >= 'a' && texto[i] <= 'z')
            texto[i] = 'a' + (texto[i] - 'a' + desplazamiento) % 26;
        else if (texto[i] >= '0' && texto[i] <= '9')
            texto[i] = '0' + (texto[i] - '0' + desplazamiento) % 10;
    }
}

//Manejo de memoria compartida
// Crear y obtener memoria compartida, inicializar en cero
DatosCompartidos* inicializar_memoria_compartida(int* shmid) 
{
    *shmid = shmget(SHM_KEY, sizeof(DatosCompartidos), IPC_CREAT | 0666);
    if (*shmid == -1) 
    {
        perror("shmget");
        exit(1);
    }
    DatosCompartidos* datos = (DatosCompartidos*)shmat(*shmid, NULL, 0);
    if (datos == (void*)-1) 
    {
        perror("shmat");
        exit(1);
    }
    // Inicializar estructura a cero
    memset(datos, 0, sizeof(DatosCompartidos));
    return datos;
}

void liberar_memoria(int shmid, DatosCompartidos* datos) 
{
    if (shmdt(datos) == -1)
        perror("shmdt");

    if (shmctl(shmid, IPC_RMID, NULL) == -1) 
        perror("shmctl IPC_RMID");
    
}

//Manejo de semaforos
int crear_semaforos() 
{
    int semid = semget(SEM_KEY, NUM_SEMS, IPC_CREAT | 0666);
    if (semid == -1) 
    {
        perror("semget");
        exit(1);
    }
    // Inicializar semaforos
    unsigned short vals[NUM_SEMS] = {1,0,0,0};
    if (semctl(semid, 0, SETALL, vals) == -1) 
    {
        perror("semctl SETALL");
        exit(1);
    }
    return semid;
}

void liberar_semaforos(int semid) 
{
    if (semctl(semid, 0, IPC_RMID) == -1) 
        perror("semctl IPC_RMID");
}

// Funciones de los procesos hijos (una por etapa)
void cargarFormulario(DatosCompartidos* datos, int semid)
{
    FILE* archivo = fopen("formularios.txt", "rt");
    if (!archivo) 
    {
        perror("No se pudo abrir el archivo de formularios");
        datos->finalizar = 1;
        for (int i = 0; i < NUM_SEMS; i++) 
            V(semid, i); // liberar a los demás
        return;
    }

    char linea[512];
    while (!terminar && !datos->finalizar) 
    {
        P(semid, SEM_CARGAR); //P(cargar)
        if (datos->finalizar) 
            break;

        if (!fgets(linea, sizeof(linea), archivo)) 
        {
             if (feof(archivo)) 
                printf("\033[1;33mCargar: Fin de archivo alcanzado. Esperando orden de finalización del padre... [CTRL C]\033[0m\n\n");// Llegó al final del archivo
             else 
                perror("Error al leer el archivo");// Otro error en la lectura

            break;
        }

        Formulario f;
        f.id = datos->cantidad + 1;
        int cant = sscanf(linea, "%ld %29s %29s %19s %19s %[^\n]",
                            &f.dni, f.nombre, f.apellido,
                            f.fechaNac, f.nroTelefono, f.descripcion);
        // printf("DEBUG: línea leída: '%s'\n", linea); PARA DEBUG
        // printf("DEBUG: sscanf leyó %d campos\n", cant);
        if (cant < 6) 
        {
            fprintf(stderr, "Formato incorrecto en línea: %s\n", linea);
            V(semid, SEM_CARGAR); // Para evitar deadlock si falla
            continue;
        }
        strcpy(f.tipoForm, "");

        
        if (datos->cantidad < MAX_FORMULARIOS) 
        {
            datos->formularios[datos->cantidad] = f;
            datos->cantidad++;
            printf("Cargar: Formulario %d cargado. Total: %d\n", f.id, datos->cantidad);
        } 
        else
        {
            fprintf(stderr, "Memoria llena, no se pueden cargar más formularios.\n");
            datos->finalizar = 1;
        }

        // Leer la próxima línea para saber si es el último
        long pos = ftell(archivo);
        char test[2];
        if (!fgets(test, sizeof(test), archivo))
            datos->ultimo = 1;
        else
            fseek(archivo, pos, SEEK_SET);

        DEBUG_SLEEP(); // Simulamos procesamiento

        V(semid, SEM_VALIDAR);  // V(validar)
    }

    fclose(archivo);

    // Si se pedia el que esten los hijos procesando constantemente (a pesar que en este caso ya no hay formularios
    // que procesar por lo que no tendria mucho sentido), se podria hacer un V(semid, SEM_CARGAR) un if en lugar del
    // while.

    while (!datos->finalizar) // Utilizamos semaforos para evitar busy wait y retrasos innecesarios
    {
        P(semid, SEM_CARGAR); // Espera hasta que se libere el semaforo
    }

    printf("cargarFormulario finalizó.\n");
    exit(EXIT_SUCCESS);
}


void validarFormulario(DatosCompartidos* datos, int semid) 
{
    int esperando_final = 0;
    while (!terminar && !datos->finalizar) 
    {
        P(semid, SEM_VALIDAR);  // Espera turno

        if (datos->finalizar)
        {
            printf("validarFormulario finalizó.\n");
            exit(EXIT_SUCCESS);
        }
        
        if (esperando_final) 
            continue;// Ya procesé el último, solo espero la orden del padre

        int idx = datos->cantidad - 1;
        Formulario* f = &datos->formularios[idx]; //Nos posicionamos en el nro de formulario que corresponde.

        // Validacion simplificada
        if (f->id <= 0 || f->dni < 1000000 || f->dni > 100000000 || strlen(f->nombre) == 0 || strlen(f->apellido) == 0 || strlen(f->fechaNac) == 0 ||
            !esSoloLetras(f->nombre) || !esSoloLetras(f->apellido) || strlen(f->nroTelefono) == 0 || !esSoloNumeros(f->nroTelefono) || strlen(f->descripcion) == 0) 
        {
            
            printf("Validar: Formulario %d invalido. Se elimina.\n", f->id);
            // Eliminar formulario invalido (simplemente reducimos cantidad para mantener consistencia de id)
            datos->cantidad--;
            V(semid, SEM_CARGAR); //Habilito la carga de un nuevo formulario.
            continue;
        } 
        else 
        {
            printf("Validar: Formulario %d valido.\n", f->id);
        }

        DEBUG_SLEEP(); // Simulamos procesamiento
        
        V(semid, SEM_ENCRIPTAR);  // Paso al siguiente proceso

        if (datos->ultimo && idx + 1 == datos->cantidad) 
            esperando_final = 1;

    }
    printf("validarFormulario finalizó.\n");
    exit(EXIT_SUCCESS);
}

void encriptarFormulario(DatosCompartidos* datos, int semid) 
{
    int esperando_final = 0;
    while (!terminar && !datos->finalizar) 
    {
        P(semid, SEM_ENCRIPTAR);

        if (datos->finalizar)
        {
            printf("encriptarFormulario finalizó.\n");
            exit(EXIT_SUCCESS);
        }

        if (esperando_final) 
            continue;// Ya procesé el último, solo espero la orden del padre
        

        int idx = datos->cantidad - 1;
        Formulario* f = &datos->formularios[idx];

        // Encriptamos campos sensibles
        cifradoCesar(f->nroTelefono, 3);

        // Convertimos el DNI a string para encriptarlo
        char dniTexto[20];
        snprintf(dniTexto, sizeof(dniTexto), "%ld", f->dni);
        cifradoCesar(dniTexto, 3);

        printf("Encriptar: Formulario %d encriptado.\n", f->id);
        
        DEBUG_SLEEP(); // Simulamos procesamiento

        V(semid, SEM_CLASIFICAR);

        if (datos->ultimo && idx + 1 == datos->cantidad) 
            esperando_final = 1;
    }

    printf("encriptarFormulario finalizó.\n");
    exit(EXIT_SUCCESS);
}

void clasificarFormulario(DatosCompartidos* datos, int semid) 
{
    int esperando_final = 0; 
    while (!terminar && !datos->finalizar) 
    {
        P(semid, SEM_CLASIFICAR);

        if (datos->finalizar)
        {
            printf("clasificarFormulario finalizó.\n");
            exit(EXIT_SUCCESS);
        }

        if (esperando_final) 
            continue;// Ya procesé el último, solo espero la orden del padre

        int idx = datos->cantidad - 1;
        Formulario* f = &datos->formularios[idx];
        char* desc = f->descripcion;

        if (strstr(desc, "reclamo") || strstr(desc, "queja") || strstr(desc, "denuncia")) 
            strcpy(f->tipoForm, "Reclamo");
        else if (strstr(desc, "pedido") || strstr(desc, "solicito") || strstr(desc, "requiero") || strstr(desc, "necesito")) 
            strcpy(f->tipoForm, "Pedido");
        else if (strstr(desc, "consulta") || strstr(desc, "duda") || strstr(desc, "pregunta")) 
            strcpy(f->tipoForm, "Consulta");
        else 
            strcpy(f->tipoForm, "Otros"); //Se considerara a esta categoria aquellos que requieran examinacion puntual o no corresponda a ninguna categoria. 
                                          //EJ: si contiene "requiero", podria caer en cualquier categoria segun que se diga en el msg.
                                          //Esto es a efectos de simplificar el ejemplo, en un caso real se podria hacer una clasificacion mas compleja.

        printf("Clasificar: Formulario %d clasificado como '%s'\n\n", f->id, f->tipoForm);
        
        DEBUG_SLEEP(); // Simulamos procesamiento

        V(semid, SEM_CARGAR);  // Habilita al próximo ciclo de carga

        if (datos->ultimo && idx + 1 == datos->cantidad) 
            esperando_final = 1;
    }

    printf("clasificarFormulario finalizó.\n");
    exit(EXIT_SUCCESS);
}

void crear_hijos(int shmid, int semid, pid_t pids[]) 
{
    for (int i = 0; i < NUM_HIJOS; i++) 
    {
        pid_t pid = fork();  // Creamos un proceso hijo

        if (pid == 0) //Si pid == 0 es el proceso hijo, no el padre.
        {
            // Codigo que se ejecuta SOLO en el hijo

            // Nos conectamos a la memoria compartida
            DatosCompartidos* datos = (DatosCompartidos*)shmat(shmid, NULL, 0);
            if (datos == (void*)-1) 
            {
                perror("shmat hijo");
                exit(1);
            }

            // Elegimos que funcion ejecutar segun el indice del hijo
            switch (i) 
            {
                case 0: cargarFormulario(datos, semid); break;
                case 1: validarFormulario(datos, semid); break;
                case 2: encriptarFormulario(datos, semid); break;
                case 3: clasificarFormulario(datos, semid); break;
            }

            exit(0); // Por si la funcion llamada no termina el proceso
        } 
        else if (pid > 0) 
        {
            // Estamos en el padre: guardamos el PID del hijo
            pids[i] = pid;
        } 
        else 
        {
            // Si fork falla, mostramos error y terminamos
            perror("fork");
            exit(1);
        }
    }
}

int main() 
{
    int shmid;
    pid_t pids[NUM_HIJOS];
     
    
    // Inicializar memoria compartida
    datos = inicializar_memoria_compartida(&shmid);

    // Crear e inicializar semáforos
    semid = crear_semaforos();

    // Configurar manejador de SIGINT. Esta forma es como una version mas "segura" que signal, solo que en lugar de ocupar una linea se hace de esta manera
    struct sigaction sa;            //Estructura para configurar cómo el programa responde a señales.
    sa.sa_handler = handler_SIGINT; //"Cuando llegue la señal SIGINT, ejecuta la función handler_SIGINT"
    sigemptyset(&sa.sa_mask);       //Esto inicializa la máscara de señales a bloquear durante la ejecución del handler, en este caso,
                                    //ninguna señal será bloqueada mientras handler_SIGINT se ejecuta.
    sa.sa_flags = 0;        //Sin flags especiales. 
    sigaction(SIGINT, &sa, NULL); //Esta llamada registra la estructura sa para que sea el comportamiento del proceso al recibir la señal SIGINT

    // Crear hijos y guardar sus PIDs
    crear_hijos(shmid, semid, pids);

    printf("\033[1;33mProceso padre: esperando señal SIGINT (Ctrl+C) para terminar...\033[0m\n");

    // Esperar hasta que se reciba SIGINT
    while(!terminar) {
        pause();
    }

    printf("\nSeñal recibida. Indicando a hijos finalizar...\n");

    // Indicar a hijos que terminen (flag en memoria compartida) y desbloquear
    finalizar();

    // Esperar que hijos terminen
    for (int i = 0; i < NUM_HIJOS; i++) 
    {
        waitpid(pids[i], NULL, 0);
    }

    printf("Todos los hijos finalizaron.\n\n");

    // Actualizar contadores finales
    int reclamos=0, pedidos=0, consultas=0, otros=0, total=0;
    for (int i = 0; i < datos->cantidad; i++) 
    {
        if (strcmp(datos->formularios[i].tipoForm, "Reclamo") == 0)
            reclamos++;
        else if (strcmp(datos->formularios[i].tipoForm, "Pedido") == 0)
            pedidos++;
        else if (strcmp(datos->formularios[i].tipoForm, "Consulta") == 0)
            consultas++;
        else
            otros++;
    }
    total = datos->cantidad;

    datos->cantidadReclamos = reclamos;
    datos->cantidadPedidos = pedidos;
    datos->cantidadConsultas = consultas;
    datos->cantidadOtros = otros;
    datos->totalProcesados = total;

    printf("Estadisticas finales:\n");
    printf("Total procesados correctamente: %d\n", datos->totalProcesados);
    printf("Reclamos: %d\n", datos->cantidadReclamos);
    printf("Pedidos: %d\n", datos->cantidadPedidos);
    printf("Consultas: %d\n", datos->cantidadConsultas);
    printf("Otros: %d\n", datos->cantidadOtros);

    FILE* salida = fopen("procesados.txt", "w");
    if (salida) 
    {
        fprintf(salida, "%-3s %-10s %-15s %-15s %-12s %-12s %-10s %s\n", 
                "ID", "DNI", "Nombre", "Apellido", "Fecha Nac", "Nro Tel", "Tipo Form", "Descripcion");
        for (int i = 0; i < datos->cantidad; i++)
        {
            Formulario* f = &datos->formularios[i];
            fprintf(salida, "%-3d %-10ld %-15s %-15s %-12s %-12s %-10s %s\n",
            f->id, f->dni, f->nombre, f->apellido, f->fechaNac, f->nroTelefono, f->tipoForm, f->descripcion);
        }
        fclose(salida);
        printf("Datos procesados guardados en procesados.txt\n");
    }   
    else 
        printf("No se pudo abrir procesados.txt para escritura.\n");


    // Liberar recursos
    liberar_semaforos(semid);
    liberar_memoria(shmid, datos);

    printf("Recursos liberados. Programa finalizado.\n");
    return 0;
}
