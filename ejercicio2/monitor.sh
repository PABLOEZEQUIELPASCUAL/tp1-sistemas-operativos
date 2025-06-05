#!/bin/bash

# Función para mostrar el encabezado de cada sección
print_header() {
    echo -e "\n============== $1 ==============\n"
}

# Función para contar threads
count_threads() {
    local pid=$1
    ps -T -p $pid | wc -l
}

# Función para contar conexiones
count_connections() {
    netstat -tn | grep ":8080" | wc -l
}

# Función para encontrar el PID del servidor
find_server_pid() {
    pgrep servidor
}

# Función para monitorear el proceso del servidor
monitor_server() {
    local pid=$1
    local prev_threads=0
    local prev_connections=0
    
    while true; do
        clear
        echo "Timestamp: $(date '+%H:%M:%S')"
        
        # Buscar el PID del servidor si no lo tenemos
        if [ -z "$pid" ] || ! kill -0 $pid 2>/dev/null; then
            pid=$(find_server_pid)
            if [ -z "$pid" ]; then
                echo "Esperando al servidor..."
                sleep 1
                continue
            fi
            echo "Servidor detectado con PID: $pid"
        fi

        # Contar threads y conexiones actuales
        local current_threads=$(($(count_threads $pid) - 1))  # -1 por el header
        local current_connections=$(count_connections)

        # Mostrar información de threads con cambios
        print_header "THREADS DEL SERVIDOR"
        if [ $current_threads -gt $prev_threads ]; then
            echo -e "\e[32m¡NUEVO THREAD CREADO! Total threads: $current_threads\e[0m"
        elif [ $current_threads -lt $prev_threads ]; then
            echo -e "\e[31mThread terminado. Total threads: $current_threads\e[0m"
        else
            echo "Total threads: $current_threads"
        fi
        echo -e "\nDetalle de threads:"
        ps -T -p $pid -o spid,tid,pcpu,state,time,command

        # Mostrar conexiones de red con cambios
        print_header "CONEXIONES DE RED"
        if [ $current_connections -gt $prev_connections ]; then
            echo -e "\e[32m¡NUEVA CONEXIÓN ESTABLECIDA! Total conexiones: $current_connections\e[0m"
        elif [ $current_connections -lt $prev_connections ]; then
            echo -e "\e[31mConexión cerrada. Total conexiones: $current_connections\e[0m"
        else
            echo "Total conexiones: $current_connections"
        fi
        echo -e "\nDetalle de conexiones:"
        netstat -tn | grep ":8080"

        # Mostrar sockets y archivos abiertos
        print_header "SOCKETS Y ARCHIVOS ABIERTOS"
        echo "Descriptores de archivo del servidor:"
        ls -l /proc/$pid/fd 2>/dev/null | grep "socket"

        # Mostrar uso de recursos
        print_header "USO DE RECURSOS POR THREAD"
        ps -p $pid -L -o pid,tid,pcpu,state,time,command --sort=-pcpu | head -n 6

        print_header "RESUMEN DE MONITOREO"
        echo "- Threads activos: $current_threads (1 principal + 1 refresco + ${current_connections} clientes)"
        echo "- Conexiones activas: $current_connections"
        echo "- Estado del servidor: ACTIVO"
        echo -e "\nPresiona Ctrl+C para detener el monitoreo"

        prev_threads=$current_threads
        prev_connections=$current_connections
        sleep 1
    done
}

# Verificar que tenemos las herramientas necesarias
command -v netstat >/dev/null 2>&1 || { echo "Error: netstat no está instalado. Ejecuta: sudo apt-get install net-tools"; exit 1; }
command -v ps >/dev/null 2>&1 || { echo "Error: ps no está instalado. Ejecuta: sudo apt-get install procps"; exit 1; }

echo "Monitor iniciado. Esperando al servidor..."
monitor_server "" 