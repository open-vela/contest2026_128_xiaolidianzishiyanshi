/****************************************************************************
 * contest2026_128_xiaolidianzishiyanshi/app/hello_app/wss_test_main.c
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <libwebsockets.h>
#include <net/if.h>
#include <nuttx/net/ioctl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct wss_test_state_s
{
  bool connected;
  bool failed;
  char error[160];
  char device_id[18];
  const char *token;
};

struct wss_test_url_s
{
  char host[128];
  char path[256];
  int port;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t wss_test_milliseconds(void)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static bool wss_test_parse_url(const char *url,
                               struct wss_test_url_s *parsed)
{
  static const char scheme[] = "wss://";
  const char *authority;
  const char *slash;
  const char *colon;
  size_t authority_len;
  size_t host_len;
  char *end;
  long port;

  if (strncmp(url, scheme, sizeof(scheme) - 1) != 0)
    {
      return false;
    }

  authority = url + sizeof(scheme) - 1;
  slash = strchr(authority, '/');
  authority_len = slash == NULL ? strlen(authority) :
                                  (size_t)(slash - authority);
  if (authority_len == 0 || authority_len >= sizeof(parsed->host))
    {
      return false;
    }

  colon = memchr(authority, ':', authority_len);
  host_len = colon == NULL ? authority_len : (size_t)(colon - authority);
  if (host_len == 0 || host_len >= sizeof(parsed->host))
    {
      return false;
    }

  memcpy(parsed->host, authority, host_len);
  parsed->host[host_len] = '\0';
  parsed->port = 443;
  if (colon != NULL)
    {
      errno = 0;
      port = strtol(colon + 1, &end, 10);
      if (errno != 0 || end != authority + authority_len || port < 1 ||
          port > 65535)
        {
          return false;
        }

      parsed->port = (int)port;
    }

  if (slash == NULL)
    {
      strcpy(parsed->path, "/");
    }
  else if (strlen(slash) >= sizeof(parsed->path))
    {
      return false;
    }
  else
    {
      strcpy(parsed->path, slash);
    }

  return true;
}

static void wss_test_get_device_id(char device_id[18])
{
  static const char *const interfaces[] =
  {
    "wlan0", "eth0", "wlan1"
  };

  struct ifreq request;
  int sock;
  unsigned int index;

  strcpy(device_id, "00:00:00:00:00:00");
  sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0)
    {
      return;
    }

  for (index = 0;
       index < sizeof(interfaces) / sizeof(interfaces[0]); index++)
    {
      memset(&request, 0, sizeof(request));
      strncpy(request.ifr_name, interfaces[index], IFNAMSIZ - 1);
      if (ioctl(sock, SIOCGIFHWADDR,
                (unsigned long)(uintptr_t)&request) == 0)
        {
          const uint8_t *mac = (const uint8_t *)request.ifr_hwaddr.sa_data;
          snprintf(device_id, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
          break;
        }
    }

  close(sock);
}

static int wss_test_add_header(struct lws *wsi, unsigned char **cursor,
                               unsigned char *end, const char *name,
                               const char *value)
{
  if (value == NULL || value[0] == '\0')
    {
      return 0;
    }

  return lws_add_http_header_by_name(wsi, (const unsigned char *)name,
                                     (const unsigned char *)value,
                                     strlen(value), cursor, end);
}

static int wss_test_callback(struct lws *wsi,
                             enum lws_callback_reasons reason,
                             void *user, void *input, size_t length)
{
  struct wss_test_state_s *state =
      (struct wss_test_state_s *)lws_context_user(lws_get_context(wsi));
  unsigned char **cursor;
  unsigned char *end;
  char authorization[192];

  (void)user;

  switch (reason)
    {
      case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
        cursor = (unsigned char **)input;
        end = *cursor + length;
        authorization[0] = '\0';
        if (state->token != NULL && state->token[0] != '\0')
          {
            snprintf(authorization, sizeof(authorization), "Bearer %s",
                     state->token);
          }

        if (wss_test_add_header(wsi, cursor, end, "authorization:",
                                authorization) < 0 ||
            wss_test_add_header(wsi, cursor, end, "protocol-version:",
                                "1") < 0 ||
            wss_test_add_header(wsi, cursor, end, "device-id:",
                                state->device_id) < 0 ||
            wss_test_add_header(wsi, cursor, end, "client-id:",
                                "00000000-0000-4000-8000-000000000128") < 0)
          {
            snprintf(state->error, sizeof(state->error),
                     "cannot append HTTP upgrade headers");
            state->failed = true;
            return -1;
          }
        break;

      case LWS_CALLBACK_CLIENT_ESTABLISHED:
        state->connected = true;
        printf("wss_test: connected "
               "(TLS and WebSocket handshake complete)\n");
        break;

      case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        snprintf(state->error, sizeof(state->error), "%s",
                 input == NULL ? "connection failed" : (const char *)input);
        state->failed = true;
        break;

      case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
        if (!state->connected)
          {
            snprintf(state->error, sizeof(state->error),
                     "peer closed during handshake");
            state->failed = true;
          }
        break;

      default:
        break;
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  static const struct lws_protocols protocols[] =
  {
    {"xiaozhi", wss_test_callback, 0, 4096, 0, NULL, 0},
    {NULL, NULL, 0, 0, 0, NULL, 0}
  };

  const char *url = argc > 1 ? argv[1] : CONFIG_CONTEST2026_128_WSS_TEST_URL;
  const char *token = argc > 2 ? argv[2] : "";
  struct wss_test_url_s parsed;
  struct wss_test_state_s state;
  struct lws_context_creation_info context_info;
  struct lws_client_connect_info connect_info;
  struct lws_context *context;
  struct lws *wsi;
  uint64_t deadline;
  int service_result;

  memset(&parsed, 0, sizeof(parsed));
  if (!wss_test_parse_url(url, &parsed))
    {
      fprintf(stderr, "usage: wss_test [wss://host[:port]/path] [token]\n");
      return 1;
    }

  memset(&state, 0, sizeof(state));
  state.token = token;
  wss_test_get_device_id(state.device_id);

  memset(&context_info, 0, sizeof(context_info));
  context_info.port = CONTEXT_PORT_NO_LISTEN;
  context_info.protocols = protocols;
  context_info.gid = -1;
  context_info.uid = -1;
  context_info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
  context_info.user = &state;

  printf("wss_test: url=%s device-id=%s timeout=%d seconds\n", url,
         state.device_id, CONFIG_CONTEST2026_128_WSS_TEST_TIMEOUT);
  context = lws_create_context(&context_info);
  if (context == NULL)
    {
      fprintf(stderr, "wss_test: lws_create_context failed\n");
      return 1;
    }

  memset(&connect_info, 0, sizeof(connect_info));
  connect_info.context = context;
  connect_info.address = parsed.host;
  connect_info.port = parsed.port;
  connect_info.path = parsed.path;
  connect_info.host = parsed.host;
  connect_info.origin = parsed.host;
  connect_info.protocol = protocols[0].name;
  connect_info.ssl_connection = LCCSCF_USE_SSL;
#ifdef CONFIG_CONTEST2026_128_WSS_TEST_TLS_ALLOW_INSECURE
  fprintf(stderr, "wss_test: WARNING: TLS verification disabled\n");
  connect_info.ssl_connection |= LCCSCF_ALLOW_INSECURE |
                                 LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
#endif

  printf("wss_test: connecting to %s:%d%s\n", parsed.host, parsed.port,
         parsed.path);
  wsi = lws_client_connect_via_info(&connect_info);
  if (wsi == NULL)
    {
      fprintf(stderr, "wss_test: lws_client_connect_via_info failed\n");
      lws_context_destroy(context);
      return 1;
    }

  deadline = wss_test_milliseconds() +
             CONFIG_CONTEST2026_128_WSS_TEST_TIMEOUT * 1000ULL;
  while (!state.connected && !state.failed &&
         wss_test_milliseconds() < deadline)
    {
      service_result = lws_service(context, 100);
      if (service_result < 0)
        {
          snprintf(state.error, sizeof(state.error),
                   "libwebsockets service loop returned %d", service_result);
          state.failed = true;
        }
    }

  if (!state.connected && !state.failed)
    {
      snprintf(state.error, sizeof(state.error), "connection timed out");
      state.failed = true;
    }

  lws_context_destroy(context);
  if (state.failed)
    {
      fprintf(stderr, "wss_test: failed: %s\n", state.error);
      return 1;
    }

  printf("wss_test: PASS\n");
  return 0;
}
