/* ShadowMountPlus shortcut launcher: the eboot.bin of every shortcut app.
 *
 * A shortcut is a small app whose folder holds smp-launch.json, naming another
 * installed app and the arguments to start it with. An app cannot start
 * another app itself (the launch services answer SCE_LNC_UTIL_ERROR_NOT_ALLOWED),
 * so this asks ShadowMountPlus's local API to do it. The request carries
 * nothing: ShadowMountPlus finds the running shortcut and reads its file. It
 * then closes this app and starts the target. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define SMP_PORT 10101
/* ShadowMountPlus closes this app once the target can start; leave if that never happens. */
#define WAIT_FOR_CLOSE_US (20u * 1000u * 1000u)

typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
int sceKernelUsleep(unsigned int);
int sceSystemServiceLoadExec(const char *path, const char *argv[]);

static void notify(const char *text)
{
    notify_request_t req;
    memset(&req, 0, sizeof req);
    snprintf(req.message, sizeof req.message, "%s", text);
    sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

/* Sends the launch request. Returns the HTTP status, or -1 when ShadowMountPlus did not answer.
 * The response's "error" text, if any, goes to error_out. */
static int request_launch(char *error_out, size_t error_size)
{
    static const char request[] = "POST /api/v1/shortcuts/launch HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                                  "Content-Type: application/json\r\nContent-Length: 2\r\n"
                                  "Connection: close\r\n\r\n{}";
    char reply[1024];
    error_out[0] = 0;

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_len = sizeof sa;
    sa.sin_family = AF_INET;
    sa.sin_port = htons(SMP_PORT);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0 ||
        send(fd, request, sizeof request - 1, 0) != (ssize_t)(sizeof request - 1)) {
        close(fd);
        return -1;
    }
    size_t got = 0;
    ssize_t n;
    while (got < sizeof reply - 1 && (n = recv(fd, reply + got, sizeof reply - 1 - got, 0)) > 0)
        got += (size_t)n;
    close(fd);
    reply[got] = 0;

    int status = -1;
    if (sscanf(reply, "HTTP/1.%*d %d", &status) != 1)
        return -1;
    const char *error = strstr(reply, "\"error\":\"");
    if (error) {
        error += strlen("\"error\":\"");
        size_t len = strcspn(error, "\"");
        if (len >= error_size)
            len = error_size - 1;
        memcpy(error_out, error, len);
        error_out[len] = 0;
    }
    return status;
}

int main(void)
{
    char error[256], text[320];
    const int status = request_launch(error, sizeof error);
    if (status == 200) {
        sceKernelUsleep(WAIT_FOR_CLOSE_US);
        notify("Shortcut: ShadowMountPlus did not start the app");
    } else if (status < 0) {
        notify("Shortcut: ShadowMountPlus is not running, or its build has no shortcut support");
    } else {
        snprintf(text, sizeof text, "Shortcut: %s (%d)", error[0] ? error : "launch refused", status);
        notify(text);
    }
    sceKernelUsleep(3u * 1000u * 1000u);
    sceSystemServiceLoadExec("exit", NULL);
    for (;;)
        sceKernelUsleep(1000000);
}
