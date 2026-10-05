/* PS5SX2 home screen shortcut: one small app per PS2 game. It reads the disc image's path from
 * /app0/ps5sx2-boot.txt and asks ShadowMountPlus (its local API) to start PS5SX2 straight into
 * that game; ShadowMountPlus closes this app and starts PS5SX2. An app cannot start another one
 * itself (the launch services answer SCE_LNC_UTIL_ERROR_NOT_ALLOWED). */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define BOOT_FILE "/app0/ps5sx2-boot.txt"
#define SMP_PORT 10101
#define ROUTE "/api/v1/ps5sx2/launch"
/* ShadowMountPlus closes this app once PS5SX2 can start; leave if that never happens. */
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

static int read_image_path(char *out, size_t size)
{
    FILE *f = fopen(BOOT_FILE, "r");
    if (!f)
        return -1;
    int ok = fgets(out, (int)size, f) != NULL;
    fclose(f);
    if (!ok)
        return -1;
    out[strcspn(out, "\r\n")] = 0;
    return out[0] == '/' ? 0 : -1;
}

/* JSON string escaping for the characters a file name can hold that JSON reserves. */
static int json_escape(const char *in, char *out, size_t size)
{
    size_t n = 0;
    for (; *in; in++) {
        const char c = *in;
        if ((unsigned char)c < 0x20)
            return -1;
        if (c == '"' || c == '\\') {
            if (n + 2 >= size)
                return -1;
            out[n++] = '\\';
        } else if (n + 1 >= size) {
            return -1;
        }
        out[n++] = c;
    }
    out[n] = 0;
    return 0;
}

/* Sends the launch request; returns the HTTP status, or -1 when ShadowMountPlus did not answer.
 * The response's "error" text, if any, goes to error_out. */
static int request_launch(const char *image, char *error_out, size_t error_size)
{
    char escaped[1024], body[1200], request[1600], reply[1024];
    error_out[0] = 0;
    if (json_escape(image, escaped, sizeof escaped) != 0)
        return -1;
    const int body_len = snprintf(body, sizeof body, "{\"image\":\"%s\"}", escaped);
    const int req_len = snprintf(request, sizeof request,
                                 "POST " ROUTE " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                                 "Content-Type: application/json\r\nContent-Length: %d\r\n"
                                 "Connection: close\r\n\r\n%s",
                                 body_len, body);

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
        send(fd, request, (size_t)req_len, 0) != req_len) {
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
    char image[900], error[256], text[1200];
    if (read_image_path(image, sizeof image) != 0) {
        notify("PS5SX2 shortcut: " BOOT_FILE " does not name a disc image");
    } else {
        const int status = request_launch(image, error, sizeof error);
        if (status == 200) {
            sceKernelUsleep(WAIT_FOR_CLOSE_US);
            notify("PS5SX2 shortcut: ShadowMountPlus did not start PS5SX2");
        } else if (status < 0) {
            notify("PS5SX2 shortcut: ShadowMountPlus is not running, or its build cannot start PS5SX2 games");
        } else {
            snprintf(text, sizeof text, "PS5SX2 shortcut: %s (%d)", error[0] ? error : "launch refused", status);
            notify(text);
        }
    }
    sceKernelUsleep(3u * 1000u * 1000u);
    sceSystemServiceLoadExec("exit", NULL);
    for (;;)
        sceKernelUsleep(1000000);
}
