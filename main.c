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
#include <stdarg.h>
#include <getopt.h>
 
#define CHECK(f, rv) \
    if (SCARD_S_SUCCESS != rv) \
    { \
        fprintf(stderr, f ": %s\n", pcsc_stringify_error(rv)); \
        return -1; \
    }
#define CHECK_RESPONSE(buffer, bufferLength, okCode) \
    if (verbose) { fprintf(stderr,"Response: %x:%x\n", buffer[bufferLength-2], buffer[bufferLength-1]); } \
    if (buffer[bufferLength-2] != 0x90 || buffer[bufferLength-1] != okCode) { \
        fprintf(stderr, "invalid response from card\n"); \
        return -2; \
    }

void err(const int code, const char *msg, ...) {
   va_list ap;

   va_start(ap, msg);
   vfprintf(stderr, msg, ap);
   va_end(ap);
   exit(code);
}

typedef enum mode_enum { READ, WRITE, PROTECT } rwp_t;

extern char *optarg;
extern int optind, opterr, optopt;

void usage(int ec) {
   printf("usage: mcrw \n");
   printf("     [-R <n>]            : use PCSC reader <n>\n");
   printf("     [-r]                : read <length> bytes from card at <offset>\n");
   printf("     [-w]                : write <length> bytes (or until EOF) to card at <offset>\n");
   printf("     [-o <offset (0)> ]  : offset of read/write operation - must be < 255\n");
   printf("     [-l <length> ]      : number of bytes to read/write\n");
   printf("     [-B]                : when wreading output raw bytes to <file>\n");
   printf("     [-f <file (-)>]     : read data from / write data to this file.\n");
   printf("     [-C <code (ffffff)>]: use this hex code as secret code for writing\n");
   printf("     [-v]                : a bit more output\n");
   printf("     [-h]                : print this message\n");
   printf("\n");
   printf("If multiple card readers are present and -R is not provided a list of the\n");
   printf("available readers will be printed on stdout and the program will exit with\n");
   printf("a non-zero exit code. Re-run the program with the apropriate argument to -R\n");
   printf("\n");
   printf("If '-' is given as the filename, stdin/stdout will be used.\n");
   printf("\n");
   exit(ec);
}

int main(int argc, char *argv[]) {
    LONG rv;
    SCARDCONTEXT hContext;
    SCARDHANDLE hCard;
    DWORD dwReaders, dwActiveProtocol, dwRecvLength;
    LPTSTR mszReaders;
    SCARD_IO_REQUEST pioSendPci;
    BYTE pbRecvBuffer[266];
    BYTE pbSendBuffer[266];
    unsigned int i, sz, remaining;
    unsigned int length = 256;
    unsigned int nbytes = 0;
    unsigned int writeCode = 0xffffff;
    BYTE offset = 0;
    rwp_t mode = READ;
    unsigned int msglen = 0;
    char *file_name = "-";
    char *ptr, **readers = NULL;
    int fd,nbReaders;
    int rid = -1;
    int binary_output = 0;
    int verbose = 0;
 
    BYTE cmdDefineCardType[] = { 0xFF, 0xA4, 0x00, 0x00, 0x01, 0x06 };
    BYTE cmdWriteCard[] = { 0xFF, 0xD0, 0x00 };

    memset(pbRecvBuffer,0,265);
    memset(pbSendBuffer,0,265);
    memset(&pioSendPci,0,sizeof(SCARD_IO_REQUEST));

    while ((i = getopt(argc, argv, "vhrwo:l:R:BC:")) != EOF) {
       switch (i) {
       case 'v':
          verbose = 1;
          break;
       case 'h':
          usage(0);
          break;
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
       case 'B':
          binary_output = 1;
          break;
       case 'o':
          offset = strtol(optarg, NULL, 16);
          if (offset > 256) {
             err(3, "Invalid argument to -o. Must be < 256.\n");
          }
          break;
       case 'l':
          length = strtol(optarg, NULL, 16);
          if (length > 256) {
             err(3, "Invalid argument to -l. Must be < 256.\n");
          }
          break;
       case 'C':
          writeCode = strtol(optarg, NULL, 16);
          if (writeCode > 0xffffff) {
             err(3, "Invalid argument to -C. Must be < 0xffffff.\n");
          }
       default:
          usage(3);
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
       err(1, "no reader found\n");
    }

    /* allocate the readers table */
    readers = calloc(nbReaders, sizeof(char *));
    if (NULL == readers) {
       err(1, "out of memory... bye!\n");
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
       fprintf(stdout, "Available readers (use -R to select)\n");
       for (int i = 0; i < nbReaders; i++) {
          fprintf(stdout, "%d: %s\n", i, readers[i]);
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
    CHECK_RESPONSE(pbRecvBuffer, dwRecvLength, 0x00);

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
             err(1, "error opening %s for writing: %s\n", file_name, strerror(errno));
          }
       }
    } else if (mode == WRITE) {
       /* unlock card for writing */
       pbSendBuffer[0] = 0xFF;
       pbSendBuffer[1] = 0x20;
       pbSendBuffer[2] = 0x00;
       pbSendBuffer[3] = 0x00;
       pbSendBuffer[4] = 0x03;
       msglen = 8;
       memcpy(&pbSendBuffer[5],&writeCode,3);
       dwRecvLength = sizeof(pbRecvBuffer);
       rv = SCardTransmit(hCard, &pioSendPci, pbSendBuffer, msglen, NULL, pbRecvBuffer, &dwRecvLength);
       CHECK("SCardTransmit (unlock)", rv);
       CHECK_RESPONSE(pbRecvBuffer, dwRecvLength, 0x07);

       memset(pbSendBuffer,0,265);
       memset(pbRecvBuffer,0,265);
       pbSendBuffer[0] = 0xFF;
       pbSendBuffer[1] = 0xD0;
       pbSendBuffer[2] = 0x00;
       pbSendBuffer[3] = offset;
       msglen = 5;
  
       if (strncmp(file_name,"-",1) == 0) {
          fd = STDIN_FILENO;
       } else {
          fd = open(file_name,O_RDONLY);
          if (fd == -1) {
             err(1, "error opening %s for writing: %s\n", file_name, strerror(errno));
          }
       }

       ptr = &pbSendBuffer[5];
       sz = 1;
       while (length > 0 && sz > 0) {
          sz = read(fd, ptr, length);
          if (verbose)
             fprintf(stderr, "read %d/%d bytes\n", sz, length);
          length -= sz;
          nbytes += sz;
          ptr += sz;
       }
       if (sz == -1) {
          err(2, "error reading data from %s: %s\n", file_name, strerror(errno));
       }
       pbSendBuffer[4] = nbytes;
       msglen += nbytes;
       if (verbose)
          fprintf(stderr, "%x:%x:%x:%x:%x:'%s'\n",pbSendBuffer[0],pbSendBuffer[1],pbSendBuffer[2],pbSendBuffer[3],pbSendBuffer[4],&pbSendBuffer[5]);
    }

    dwRecvLength = sizeof(pbRecvBuffer);
    rv = SCardTransmit(hCard, &pioSendPci, pbSendBuffer, msglen, NULL, pbRecvBuffer, &dwRecvLength);
    CHECK("SCardTransmit", rv)
    CHECK_RESPONSE(pbRecvBuffer, dwRecvLength, 0x00);

    if (verbose)
       fprintf(stderr, "received %lu bytes\n", dwRecvLength);

    if (mode == READ) {
       if (binary_output) {
          write(fd, pbRecvBuffer, dwRecvLength-2);
       } else {
          FILE *out = fdopen(fd, "w");
          pbRecvBuffer[dwRecvLength-2] = '\0';
          fprintf(out, "%s\n", &pbRecvBuffer[0]);
       }
    }
 
    rv = SCardDisconnect(hCard, SCARD_LEAVE_CARD);
    CHECK("SCardDisconnect", rv);
 
    rv = SCardFreeMemory(hContext, mszReaders);
    CHECK("SCardFreeMemory", rv);
 
    rv = SCardReleaseContext(hContext);
    CHECK("SCardReleaseContext", rv);
}
