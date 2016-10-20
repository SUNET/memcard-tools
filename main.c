#include <stdio.h>
#include <stdlib.h>
#include <winscard.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
 
#define CHECK(f, rv) \
    if (SCARD_S_SUCCESS != rv) \
    { \
        fprintf(stderr, f ": %s\n", pcsc_stringify_error(rv)); \
        return -1; \
    }
#define CHECK_RESPONSE(buffer, bufferLength) \
    if(buffer[bufferLength-2] != 0x90 || buffer[bufferLength-1] != 0x00) { \
        fprintf(stderr, "Invalid response!\n"); \
        return -2; \
    }

typedef enum mode_enum { READ, WRITE, PROTECT } rwp_t;

extern char *optarg;
extern int optind, opterr, optopt;

int main(int argc, char *argv[]) {
    LONG rv;
    SCARDCONTEXT hContext;
    SCARDHANDLE hCard;
    DWORD dwReaders, dwActiveProtocol, dwRecvLength;
    LPTSTR mszReaders;
    SCARD_IO_REQUEST pioSendPci;
    BYTE pbRecvBuffer[266];
    BYTE pbSendBuffer[266];
    unsigned int i;
    unsigned int length = 128;
    BYTE offset = 0;
    rwp_t mode = READ;
    unsigned int msglen = 0;
    char *file_name = "-";
    char *ptr, **readers = NULL;
    int fd,nbReaders;
    int rid = -1;
 
    BYTE cmdDefineCardType[] = { 0xFF, 0xA4, 0x00, 0x00, 0x01, 0x06 };
    BYTE cmdWriteCard[] = { 0xFF, 0xD0, 0x00 };

    memset(pbRecvBuffer,0,265);
    memset(pbSendBuffer,0,265);
    memset(&pioSendPci,0,sizeof(SCARD_IO_REQUEST));

    while ((i = getopt(argc, argv, "rwo:l:R:")) != EOF) {
       switch (i) {
       case 'r':
          mode = READ;
          break;
       case 'w':
          mode = WRITE;
          break;
       case 'f':
          file_name = strdup(optarg);
          break;
       case 'R':
          rid = strtol(optarg, NULL, 10);
          break;
       case 'o':
          offset = strtol(optarg, NULL, 16);
          if (offset > 256) {
             fprintf(stderr, "Invalid argument to -o. Must be < 256.\n");
             exit(3); 
          }
          break;
       case 'l':
          length = strtol(argv[2], NULL, 16);
          if (length > 256) {
             fprintf(stderr, "Invalid argument to -l. Must be < 256.\n");
             exit(3);
          }
          break;
       }
    }

    rv = SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &hContext);
    CHECK("SCardEstablishContext", rv);
 
    dwReaders = SCARD_AUTOALLOCATE;
    rv = SCardListReaders(hContext, NULL, (LPTSTR)&mszReaders, &dwReaders);
    CHECK("SCardListReaders", rv)

    /* Extract readers from the null separated string and get the total
     * number of readers */
    nbReaders = 0;
    ptr = mszReaders;
    while (*ptr != '\0') {
        ptr += strlen(ptr)+1;
        nbReaders++;
    }

    if (nbReaders == 0) {
       printf("No reader found\n");
       exit(-1);
    }

    /* allocate the readers table */
    readers = calloc(nbReaders, sizeof(char *));
    if (NULL == readers) {
       printf("out of memory... bye!\n");
       exit(1);
    }

    /* fill the readers table */
    nbReaders = 0;
    ptr = mszReaders;
    while (*ptr != '\0') {
       readers[nbReaders] = ptr;
       ptr += strlen(ptr)+1;
       nbReaders++;
    }

    if (nbReaders == 1) {
       rid = 0;
    } else if (rid == -1) {
       printf("Available readers (use -R to select)\n");
       for (int i = 0; i < nbReaders; i++) {
          printf("%d: %s\n", i, readers[i]);
       }
       printf("\n");
       exit(0);
    }

    assert(rid > -1);
    
    rv = SCardConnect(hContext, 
                      readers[rid],
                      SCARD_SHARE_DIRECT,
                      SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1,
                      &hCard,
                      &dwActiveProtocol);
    CHECK("SCardConnect", rv)
 
    switch(dwActiveProtocol)
    {
        case SCARD_PROTOCOL_T0:
            pioSendPci = *SCARD_PCI_T0;
            break;
 
        case SCARD_PROTOCOL_T1:
            pioSendPci = *SCARD_PCI_T1;
            break;

        case SCARD_PROTOCOL_RAW:
            pioSendPci = *SCARD_PCI_RAW;
            break;

        default:
            break;
    }
    dwRecvLength = sizeof(pbRecvBuffer);
 
    rv = SCardTransmit(hCard, &pioSendPci, cmdDefineCardType, sizeof(cmdDefineCardType), NULL, pbRecvBuffer, &dwRecvLength);
    CHECK("SCardTransmit (define card type)", rv)
    CHECK_RESPONSE(pbRecvBuffer, dwRecvLength);

    if (mode == READ) {
       pbSendBuffer[0] = 0xFF;
       pbSendBuffer[1] = 0xB0;
       pbSendBuffer[2] = 0x00;
       pbSendBuffer[3] = offset;
       pbSendBuffer[4] = length;
       msglen = 5;

       if (strncmp(file_name,"-",1) == 0) {
          fd = STDOUT_FILENO;
       } else {
          fd = open(file_name,O_CREAT|O_WRONLY);
          if (fd == -1) {
             fprintf(stderr, "error opening %s for writing: %s\n", file_name, strerror(errno));
             exit(2);
          }
       }
    } else if (mode == WRITE) {
       pbSendBuffer[0] = 0xFF;
       pbSendBuffer[1] = 0xD0;
       pbSendBuffer[2] = 0x00;
       pbSendBuffer[3] = offset;
       pbSendBuffer[4] = length;
       msglen = 5+length;
    }

    if (mode == WRITE) {
       read(fd, &pbSendBuffer[5], length);
    }
 
    dwRecvLength = sizeof(pbRecvBuffer);
    rv = SCardTransmit(hCard, &pioSendPci, pbSendBuffer, msglen, NULL, pbRecvBuffer, &dwRecvLength);
    CHECK("SCardTransmit", rv)
    CHECK_RESPONSE(pbRecvBuffer, dwRecvLength);

    if (mode == READ) { 
       write(fd, pbRecvBuffer, dwRecvLength-2);
    }
 
    rv = SCardDisconnect(hCard, SCARD_LEAVE_CARD);
    CHECK("SCardDisconnect", rv);
 
    rv = SCardFreeMemory(hContext, mszReaders);
    CHECK("SCardFreeMemory", rv);
 
    rv = SCardReleaseContext(hContext);
    CHECK("SCardReleaseContext", rv);
}
