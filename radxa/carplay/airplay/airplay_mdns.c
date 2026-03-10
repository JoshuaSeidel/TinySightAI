/*
 * airplay_mdns.c — mDNS/Bonjour Advertisement via Avahi
 *
 * Uses the Avahi client library to register _airplay._tcp on port 7000
 * so that iPhones discover our AirPlay receiver automatically.
 *
 * The Avahi event loop runs in a dedicated background thread.
 *
 * Avahi API overview:
 *   AvahiSimplePoll  — embeds a select()-based event loop
 *   AvahiClient      — connection to the avahi-daemon
 *   AvahiEntryGroup  — holds our registered service record(s)
 *
 * Build dependency: -lavahi-client -lavahi-common
 */

#include "airplay_mdns.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/malloc.h>
#include <avahi-common/error.h>
#include <avahi-common/alternative.h>

/* -----------------------------------------------------------------------
 * Context structure (opaque to callers)
 * ----------------------------------------------------------------------- */

struct airplay_mdns_ctx {
    AvahiSimplePoll  *poll;
    AvahiClient      *client;
    AvahiEntryGroup  *group;

    char     service_name[128];
    char     mac[18];
    uint8_t  ed25519_pub[32];
    uint16_t port;

    pthread_t thread;
    volatile int running;
};

/* -----------------------------------------------------------------------
 * Forward declarations
 * ----------------------------------------------------------------------- */
static void create_services(airplay_mdns_ctx_t *ctx);

/* -----------------------------------------------------------------------
 * Build the TXT record string list
 * ----------------------------------------------------------------------- */

static AvahiStringList *build_txt_records(const airplay_mdns_ctx_t *ctx)
{
    /* Convert Ed25519 public key to hex */
    char pk_hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(pk_hex + i * 2, 3, "%02x", ctx->ed25519_pub[i]);
    }

    AvahiStringList *txt = NULL;

    txt = avahi_string_list_add_pair(txt, "deviceid", ctx->mac);
    txt = avahi_string_list_add_pair(txt, "features", "0x5A7FFFF7,0x1E");
    txt = avahi_string_list_add_pair(txt, "model",    "AppleTV3,2");
    txt = avahi_string_list_add_pair(txt, "srcvers",  "220.68");
    txt = avahi_string_list_add_pair(txt, "vv",       "2");
    txt = avahi_string_list_add_pair(txt, "pk",       pk_hex);
    txt = avahi_string_list_add_pair(txt, "pi",
                                      "b08f5a79-db29-4384-b456-a4784d9e6055");

    return txt;
}

/* -----------------------------------------------------------------------
 * Entry group state callback
 * ----------------------------------------------------------------------- */

static void entry_group_callback(AvahiEntryGroup *g,
                                  AvahiEntryGroupState state,
                                  void *userdata)
{
    airplay_mdns_ctx_t *ctx = (airplay_mdns_ctx_t *)userdata;
    (void)g;

    switch (state) {
    case AVAHI_ENTRY_GROUP_ESTABLISHED:
        printf("mdns: service '%s' registered on _airplay._tcp port %u\n",
               ctx->service_name, ctx->port);
        break;

    case AVAHI_ENTRY_GROUP_COLLISION: {
        /* Name collision — generate an alternative name and re-register */
        char *alt = avahi_alternative_service_name(ctx->service_name);
        printf("mdns: name collision, trying '%s'\n", alt);
        strncpy(ctx->service_name, alt, sizeof(ctx->service_name) - 1);
        avahi_free(alt);
        create_services(ctx);
        break;
    }

    case AVAHI_ENTRY_GROUP_FAILURE:
        fprintf(stderr, "mdns: entry group failure: %s\n",
                avahi_strerror(avahi_client_errno(ctx->client)));
        avahi_simple_poll_quit(ctx->poll);
        break;

    default:
        break;
    }
}

/* -----------------------------------------------------------------------
 * Service registration
 * ----------------------------------------------------------------------- */

static void create_services(airplay_mdns_ctx_t *ctx)
{
    int ret;

    if (!ctx->group) {
        ctx->group = avahi_entry_group_new(ctx->client,
                                            entry_group_callback,
                                            ctx);
        if (!ctx->group) {
            fprintf(stderr, "mdns: avahi_entry_group_new failed: %s\n",
                    avahi_strerror(avahi_client_errno(ctx->client)));
            return;
        }
    } else {
        avahi_entry_group_reset(ctx->group);
    }

    AvahiStringList *txt = build_txt_records(ctx);

    ret = avahi_entry_group_add_service_strlst(
        ctx->group,
        AVAHI_IF_UNSPEC,       /* all interfaces */
        AVAHI_PROTO_UNSPEC,    /* IPv4 + IPv6 */
        0,                     /* flags */
        ctx->service_name,
        "_airplay._tcp",
        NULL,                  /* domain (NULL = local) */
        NULL,                  /* host (NULL = local hostname) */
        ctx->port,
        txt);

    avahi_string_list_free(txt);

    if (ret < 0) {
        fprintf(stderr, "mdns: failed to add _airplay._tcp service: %s\n",
                avahi_strerror(ret));
        return;
    }

    /* Also advertise _raop._tcp for legacy AirPlay audio compatibility */
    /* Format: "AABBCCDDEEFF@<name>" per the RAOP service naming convention */
    char raop_name[160];
    char mac_no_colons[13];
    {
        const char *m = ctx->mac;
        int j = 0;
        for (int i = 0; m[i]; i++) {
            if (m[i] != ':') mac_no_colons[j++] = m[i];
        }
        mac_no_colons[j] = '\0';
    }
    snprintf(raop_name, sizeof(raop_name), "%s@%s", mac_no_colons, ctx->service_name);

    AvahiStringList *raop_txt = NULL;
    raop_txt = avahi_string_list_add_pair(raop_txt, "cn", "0,1,2,3");
    raop_txt = avahi_string_list_add_pair(raop_txt, "da", "true");
    raop_txt = avahi_string_list_add_pair(raop_txt, "et", "0,3,5");
    raop_txt = avahi_string_list_add_pair(raop_txt, "md", "0,1,2");
    raop_txt = avahi_string_list_add_pair(raop_txt, "pw", "false");
    raop_txt = avahi_string_list_add_pair(raop_txt, "sv", "false");
    raop_txt = avahi_string_list_add_pair(raop_txt, "sr", "44100");
    raop_txt = avahi_string_list_add_pair(raop_txt, "ss", "16");
    raop_txt = avahi_string_list_add_pair(raop_txt, "tp", "TCP,UDP");
    raop_txt = avahi_string_list_add_pair(raop_txt, "vs", "220.68");
    raop_txt = avahi_string_list_add_pair(raop_txt, "vn", "65537");

    avahi_entry_group_add_service_strlst(
        ctx->group,
        AVAHI_IF_UNSPEC,
        AVAHI_PROTO_UNSPEC,
        0,
        raop_name,
        "_raop._tcp",
        NULL, NULL,
        ctx->port,
        raop_txt);

    avahi_string_list_free(raop_txt);

    ret = avahi_entry_group_commit(ctx->group);
    if (ret < 0) {
        fprintf(stderr, "mdns: avahi_entry_group_commit failed: %s\n",
                avahi_strerror(ret));
    }
}

/* -----------------------------------------------------------------------
 * Avahi client state callback
 * ----------------------------------------------------------------------- */

static void client_callback(AvahiClient *client,
                              AvahiClientState state,
                              void *userdata)
{
    airplay_mdns_ctx_t *ctx = (airplay_mdns_ctx_t *)userdata;
    ctx->client = client;

    switch (state) {
    case AVAHI_CLIENT_S_RUNNING:
        /* The avahi daemon is up — register our service */
        create_services(ctx);
        break;

    case AVAHI_CLIENT_FAILURE:
        fprintf(stderr, "mdns: client failure: %s\n",
                avahi_strerror(avahi_client_errno(client)));
        avahi_simple_poll_quit(ctx->poll);
        break;

    case AVAHI_CLIENT_S_COLLISION:
    case AVAHI_CLIENT_S_REGISTERING:
        /* Reset group on name collision or re-register */
        if (ctx->group)
            avahi_entry_group_reset(ctx->group);
        break;

    case AVAHI_CLIENT_CONNECTING:
        break;
    }
}

/* -----------------------------------------------------------------------
 * Background thread
 * ----------------------------------------------------------------------- */

static void *mdns_thread(void *arg)
{
    airplay_mdns_ctx_t *ctx = (airplay_mdns_ctx_t *)arg;

    /* Run the Avahi event loop until stopped */
    while (ctx->running) {
        avahi_simple_poll_loop(ctx->poll);
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

airplay_mdns_ctx_t *airplay_mdns_register(const char *mac,
                                           const char *name,
                                           const uint8_t ed25519_pub[32],
                                           uint16_t port)
{
    airplay_mdns_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    snprintf(ctx->mac, sizeof(ctx->mac), "%s", mac ? mac : "AA:BB:CC:DD:EE:FF");
    snprintf(ctx->service_name, sizeof(ctx->service_name), "%s", name ? name : "CarPlay");
    memcpy(ctx->ed25519_pub, ed25519_pub, 32);
    ctx->port    = port;
    ctx->running = 1;

    /* Create Avahi simple poll */
    ctx->poll = avahi_simple_poll_new();
    if (!ctx->poll) {
        fprintf(stderr, "mdns: avahi_simple_poll_new failed\n");
        free(ctx);
        return NULL;
    }

    /* Create Avahi client */
    int error = 0;
    ctx->client = avahi_client_new(avahi_simple_poll_get(ctx->poll),
                                    0,
                                    client_callback,
                                    ctx,
                                    &error);
    if (!ctx->client) {
        fprintf(stderr, "mdns: avahi_client_new failed: %s\n",
                avahi_strerror(error));
        avahi_simple_poll_free(ctx->poll);
        free(ctx);
        return NULL;
    }

    /* Start the background thread */
    if (pthread_create(&ctx->thread, NULL, mdns_thread, ctx) != 0) {
        perror("mdns: pthread_create");
        avahi_client_free(ctx->client);
        avahi_simple_poll_free(ctx->poll);
        free(ctx);
        return NULL;
    }

    printf("mdns: advertising '%s' on _airplay._tcp port %u\n",
           ctx->service_name, ctx->port);
    return ctx;
}

void airplay_mdns_unregister(airplay_mdns_ctx_t *ctx)
{
    if (!ctx) return;

    ctx->running = 0;
    avahi_simple_poll_quit(ctx->poll);
    pthread_join(ctx->thread, NULL);

    if (ctx->group)  avahi_entry_group_free(ctx->group);
    if (ctx->client) avahi_client_free(ctx->client);
    if (ctx->poll)   avahi_simple_poll_free(ctx->poll);

    free(ctx);
    printf("mdns: service unregistered\n");
}
