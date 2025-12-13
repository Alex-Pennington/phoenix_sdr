/**
 * @file serial_dump.c
 * @brief Simple serial port dumper - shows raw data from COM port
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

int main(int argc, char *argv[]) {
    const char *port = argc > 1 ? argv[1] : "COM6";
    int duration = argc > 2 ? atoi(argv[2]) : 5;
    
    char full_port[32];
    snprintf(full_port, sizeof(full_port), "\\\\.\\%s", port);
    
    HANDLE hSerial = CreateFileA(full_port,
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING, 0, NULL);
    
    if (hSerial == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Failed to open %s (error %lu)\n", port, GetLastError());
        return 1;
    }
    
    /* Configure serial */
    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    GetCommState(hSerial, &dcb);
    dcb.BaudRate = 115200;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    SetCommState(hSerial, &dcb);
    
    COMMTIMEOUTS timeouts = {0};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 100;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    SetCommTimeouts(hSerial, &timeouts);
    
    PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);
    
    printf("Reading from %s for %d seconds...\n\n", port, duration);
    
    char buffer[256];
    DWORD bytes_read;
    DWORD start = GetTickCount();
    int total_bytes = 0;
    
    while ((GetTickCount() - start) / 1000 < (DWORD)duration) {
        if (ReadFile(hSerial, buffer, sizeof(buffer) - 1, &bytes_read, NULL) && bytes_read > 0) {
            buffer[bytes_read] = '\0';
            printf("%s", buffer);
            fflush(stdout);
            total_bytes += bytes_read;
        }
    }
    
    CloseHandle(hSerial);
    printf("\n\nTotal bytes: %d\n", total_bytes);
    return 0;
}
