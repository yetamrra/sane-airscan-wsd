/* AirScan (a.k.a. eSCL) backend for SANE
 *
 * Copyright (C) 2019 and up by Alexander Pevzner (pzz@apevzner.com)
 * See LICENSE for license terms and conditions
 *
 * Device management
 */

#include "airscan.h"

/******************** Constants *********************/
/* Max time to wait until device table is ready, in seconds
 */
#define DEVICE_TABLE_READY_TIMEOUT              5

/* Default resolution, DPI
 */
#define DEVICE_DEFAULT_RESOLUTION               300

/******************** Device management ********************/
/* Device flags
 */
enum {
    DEVICE_LISTED           = (1 << 0), /* Device listed in device_table */
    DEVICE_READY            = (1 << 2), /* Device is ready */
    DEVICE_HALTED           = (1 << 3), /* Device is halted */
    DEVICE_INIT_WAIT        = (1 << 4), /* Device was found during initial
                                           scan and not ready yet */
    DEVICE_ALL_FLAGS        = 0xffffffff
};

/* Device descriptor
 */
struct device {
    /* Common part */
    volatile gint        refcnt;        /* Reference counter */
    const char           *name;         /* Device name */
    unsigned int         flags;         /* Device flags */
    devcaps              caps;          /* Device capabilities */

    /* I/O handling (AVAHI and HTTP) */
    zeroconf_addrinfo    *addresses;    /* Device addresses, NULL if
                                           device was statically added */
    zeroconf_addrinfo    *addr_current; /* Current address to probe */
    SoupURI              *base_url;     /* eSCL base URI */
    GPtrArray            *http_pending; /* Pending HTTP requests */

    /* Options */
    SANE_Option_Descriptor opt_desc[NUM_OPTIONS]; /* Option descriptors */
    OPT_SOURCE             opt_src;               /* Current source */
    OPT_COLORMODE          opt_colormode;         /* Color mode */
    SANE_Word              opt_resolution;        /* Current resolution */
    SANE_Word              opt_tl_x, opt_tl_y;    /* Top-left x/y */
    SANE_Word              opt_br_x, opt_br_y;    /* Bottom-right x/y */
};

/* Static variables
 */
static GTree *device_table;
static GCond device_table_cond;

static SoupSession *device_http_session;

/* Forward declarations
 */
static void
device_add_static (const char *name, SoupURI *uri);

static void
device_scanner_capabilities_callback (device *dev, SoupMessage *msg);

static void
device_http_get (device *dev, const char *path,
        void (*callback)(device*, SoupMessage*));

static void
device_table_purge (void);

/* Compare device names, for device_table
 */
static int
device_name_compare (gconstpointer a, gconstpointer b, gpointer userdata)
{
    (void) userdata;
    return strcmp((const char *) a, (const char*) b);
}

/* Initialize device management
 */
SANE_Status
device_management_init (void)
{
    g_cond_init(&device_table_cond);
    device_table = g_tree_new_full(device_name_compare, NULL, NULL, NULL);

    return SANE_STATUS_GOOD;
}

/* Cleanup device management
 */
void
device_management_cleanup (void)
{
    if (device_table != NULL) {
        g_assert(g_tree_nnodes(device_table) == 0);
        g_cond_clear(&device_table_cond);
        g_tree_unref(device_table);
        device_table = NULL;
    }
}

/* Start/stop devices management. Called from the airscan thread
 */
static void
device_management_start (void)
{
    conf_device *dev_conf;

    device_http_session = soup_session_new();
    for (dev_conf = conf.devices; dev_conf != NULL; dev_conf = dev_conf->next) {
        device_add_static(dev_conf->name, dev_conf->uri);
    }
}

/* Stop device management. Called from the airscan thread
 */
static void
device_management_stop (void)
{
    soup_session_abort(device_http_session);
    device_table_purge();
    g_object_unref(device_http_session);
    device_http_session = NULL;
}

/* Start/stop device management
 */
void
device_management_start_stop (gboolean start)
{
    if (start) {
        device_management_start();
    } else {
        device_management_stop();
    }
}

/* Add device to the table
 */
static device*
device_add (const char *name)
{
    /* Create device */
    device      *dev = g_new0(device, 1);

    dev->refcnt = 1;
    dev->name = g_strdup(name);
    dev->flags = DEVICE_LISTED;
    devcaps_init(&dev->caps);

    dev->http_pending = g_ptr_array_new();
    dev->opt_src = OPT_SOURCE_UNKNOWN;
    dev->opt_colormode = OPT_COLORMODE_UNKNOWN;

    DBG_DEVICE(dev->name, "created");

    /* Add to the table */
    g_tree_insert(device_table, (gpointer) dev->name, dev);

    return dev;
}

/* Ref the device
 */
static inline device*
device_ref (device *dev)
{
    g_atomic_int_inc(&dev->refcnt);
    return dev;
}

/* Unref the device
 */
static inline void
device_unref (device *dev)
{
    if (g_atomic_int_dec_and_test(&dev->refcnt)) {
        DBG_DEVICE(dev->name, "destroyed");
        g_assert((dev->flags & DEVICE_LISTED) == 0);
        g_assert((dev->flags & DEVICE_HALTED) != 0);

        /* Release all memory */
        g_free((void*) dev->name);

        devcaps_cleanup(&dev->caps);

        zeroconf_addrinfo_list_free(dev->addresses);

        if (dev->base_url != NULL) {
            soup_uri_free(dev->base_url);
        }
        g_ptr_array_unref(dev->http_pending);

        g_free(dev);
    }
}

/* Del device from the table. It implicitly halts all
 * pending I/O activity
 *
 * Note, reference to the device may still exist (device
 * may be opened), so memory can be freed later, when
 * device is not used anymore
 */
static void
device_del (device *dev)
{
    /* Remove device from table */
    DBG_DEVICE(dev->name, "removed from device table");
    g_assert((dev->flags & DEVICE_LISTED) != 0);

    dev->flags &= ~DEVICE_LISTED;
    g_tree_remove(device_table, dev->name);

    /* Stop all pending I/O activity */
    guint i;
    for (i = 0; i < dev->http_pending->len; i ++) {
        soup_session_cancel_message(device_http_session,
                g_ptr_array_index(dev->http_pending, i), SOUP_STATUS_CANCELLED);
    }

    dev->flags |= DEVICE_HALTED;
    dev->flags &= ~DEVICE_READY;

    /* Unref the device */
    device_unref(dev);
}

/* Find device in a table
 */
static device*
device_find (const char *name)
{
    return g_tree_lookup(device_table, name);
}

/* Add statically configured device
 */
static void
device_add_static (const char *name, SoupURI *uri)
{
    /* Don't allow duplicate devices */
    device *dev = device_find(name);
    if (dev != NULL) {
        DBG_DEVICE(name, "device already exist");
        return;
    }

    /* Add a device */
    dev = device_add(name);
    dev->flags |= DEVICE_INIT_WAIT;
    dev->base_url = soup_uri_copy(uri);

    /* Make sure URI's path ends with '/' character */
    const char *path = soup_uri_get_path(dev->base_url);
    if (!g_str_has_suffix(path, "/")) {
        size_t len = strlen(path);
        char *path2 = g_alloca(len + 2);
        memcpy(path2, path, len);
        path2[len] = '/';
        path2[len+1] = '\0';
        soup_uri_set_path(dev->base_url, path2);
    }

    /* Fetch device capabilities */
    device_http_get(dev, "ScannerCapabilities",
            device_scanner_capabilities_callback);
}

/* Probe next device address
 */
static void
device_probe_address (device *dev, zeroconf_addrinfo *addrinfo)
{
    /* Cleanup after previous probe */
    dev->addr_current = addrinfo;
    if (dev->base_url != NULL) {
        soup_uri_free(dev->base_url);
    }

    /* Build device API URL */
    char str_addr[128], *url;

    if (addrinfo->addr.proto == AVAHI_PROTO_INET) {
        avahi_address_snprint(str_addr, sizeof(str_addr), &addrinfo->addr);
    } else {
        str_addr[0] = '[';
        avahi_address_snprint(str_addr + 1, sizeof(str_addr) - 2,
            &addrinfo->addr);
        size_t l = strlen(str_addr);

        /* Connect to link-local address requires explicit scope */
        if (addrinfo->linklocal) {
            /* Percent character in the IPv6 address literal
             * needs to be properly escaped, so it becomes %25
             * See RFC6874 for details
             */
            l += sprintf(str_addr + l, "%%25%d", addrinfo->interface);
        }

        str_addr[l++] = ']';
        str_addr[l] = '\0';
    }

    if (addrinfo->rs != NULL) {
        url = g_strdup_printf("http://%s:%d/%s/", str_addr, addrinfo->port,
                addrinfo->rs);
    } else {
        url = g_strdup_printf("http://%s:%d/", str_addr, addrinfo->port);
    }

    dev->base_url = soup_uri_new(url);
    g_assert(dev->base_url != NULL);
    DBG_DEVICE(dev->name, "url=\"%s\"", url);

    /* Fetch device capabilities */
    device_http_get(dev, "ScannerCapabilities",
            device_scanner_capabilities_callback);
}

/* Device found notification -- called by ZeroConf
 */
void
device_found (const char *name, gboolean init_scan,
        zeroconf_addrinfo *addresses)
{
    /* Don't allow duplicate devices */
    device *dev = device_find(name);
    if (dev != NULL) {
        DBG_DEVICE(name, "device already exist");
        return;
    }

    /* Add a device */
    dev = device_add(name);
    if (init_scan) {
        dev->flags |= DEVICE_INIT_WAIT;
    }
    dev->addresses = zeroconf_addrinfo_list_copy(addresses);
    device_probe_address(dev, dev->addresses);
}

/* Device removed notification -- called by ZeroConf
 */
void
device_removed (const char *name)
{
    device *dev = device_find(name);
    if (dev) {
        device_del(dev);
    }
}

/* Device initial scan finished notification -- called by ZeroConf
 */
void
device_init_scan_finished (void)
{
    g_cond_broadcast(&device_table_cond);
}

/* Rebuild option descriptors
 */
static void
device_rebuild_opt_desc (device *dev)
{
    SANE_Option_Descriptor *desc;
    devcaps_source         *src = dev->caps.src[dev->opt_src];

    memset(dev->opt_desc, 0, sizeof(dev->opt_desc));

    /* OPT_NUM_OPTIONS */
    desc = &dev->opt_desc[OPT_NUM_OPTIONS];
    desc->name = SANE_NAME_NUM_OPTIONS;
    desc->title = SANE_TITLE_NUM_OPTIONS;
    desc->desc = SANE_DESC_NUM_OPTIONS;
    desc->type = SANE_TYPE_INT;
    desc->cap = SANE_CAP_SOFT_DETECT;

    /* OPT_GROUP_STANDARD */
    desc = &dev->opt_desc[OPT_GROUP_STANDARD];
    desc->name = SANE_NAME_STANDARD;
    desc->title = SANE_TITLE_STANDARD;
    desc->desc = SANE_DESC_STANDARD;
    desc->type = SANE_TYPE_GROUP;
    desc->cap = 0;

    /* OPT_SCAN_RESOLUTION */
    desc = &dev->opt_desc[OPT_SCAN_RESOLUTION];
    desc->name = SANE_NAME_SCAN_RESOLUTION;
    desc->title = SANE_TITLE_SCAN_RESOLUTION;
    desc->desc = SANE_DESC_SCAN_RESOLUTION;
    desc->type = SANE_TYPE_INT;
    desc->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    desc->unit = SANE_UNIT_DPI;
    if ((src->flags & DEVCAPS_SOURCE_RES_DISCRETE) != 0) {
        desc->constraint_type = SANE_CONSTRAINT_WORD_LIST;
        desc->constraint.word_list = src->resolutions;
    } else {
        desc->constraint_type = SANE_CONSTRAINT_RANGE;
        desc->constraint.range = &src->res_range;
    }

    /* OPT_SCAN_MODE */
    desc = &dev->opt_desc[OPT_SCAN_COLORMODE];
    desc->name = SANE_NAME_SCAN_MODE;
    desc->title = SANE_TITLE_SCAN_MODE;
    desc->type = SANE_TYPE_STRING;
    desc->size = array_of_string_max_strlen(&src->sane_colormodes) + 1;
    desc->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    desc->constraint_type = SANE_CONSTRAINT_STRING_LIST;
    desc->constraint.string_list = (SANE_String_Const*) src->sane_colormodes;

    /* OPT_SCAN_SOURCE */
    desc = &dev->opt_desc[OPT_SCAN_SOURCE];
    desc->name = SANE_NAME_SCAN_SOURCE;
    desc->title = SANE_TITLE_SCAN_SOURCE;
    desc->type = SANE_TYPE_STRING;
    desc->size = array_of_string_max_strlen(&dev->caps.sane_sources) + 1;
    desc->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    desc->constraint_type = SANE_CONSTRAINT_STRING_LIST;
    desc->constraint.string_list = (SANE_String_Const*) dev->caps.sane_sources;

    /* OPT_GROUP_GEOMETRY */
    desc = &dev->opt_desc[OPT_GROUP_GEOMETRY];
    desc->name = SANE_NAME_GEOMETRY;
    desc->title = SANE_TITLE_GEOMETRY;
    desc->desc = SANE_DESC_GEOMETRY;
    desc->type = SANE_TYPE_GROUP;
    desc->cap = 0;

    /* OPT_SCAN_TL_X */
    desc = &dev->opt_desc[OPT_SCAN_TL_X];
    desc->name = SANE_NAME_SCAN_TL_X;
    desc->title = SANE_TITLE_SCAN_TL_X;
    desc->desc = SANE_DESC_SCAN_TL_X;
    desc->type = SANE_TYPE_FIXED;
    desc->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    desc->unit = SANE_UNIT_MM;
    desc->constraint_type = SANE_CONSTRAINT_RANGE;
    desc->constraint.range = &src->tl_x_range;

    /* OPT_SCAN_TL_Y */
    desc = &dev->opt_desc[OPT_SCAN_TL_Y];
    desc->name = SANE_NAME_SCAN_TL_Y;
    desc->title = SANE_TITLE_SCAN_TL_Y;
    desc->desc = SANE_DESC_SCAN_TL_Y;
    desc->type = SANE_TYPE_FIXED;
    desc->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    desc->unit = SANE_UNIT_MM;
    desc->constraint_type = SANE_CONSTRAINT_RANGE;
    desc->constraint.range = &src->tl_y_range;


    /* OPT_SCAN_BR_X */
    desc = &dev->opt_desc[OPT_SCAN_BR_X];
    desc->name = SANE_NAME_SCAN_BR_X;
    desc->title = SANE_TITLE_SCAN_BR_X;
    desc->desc = SANE_DESC_SCAN_BR_X;
    desc->type = SANE_TYPE_FIXED;
    desc->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    desc->unit = SANE_UNIT_MM;
    desc->constraint_type = SANE_CONSTRAINT_RANGE;
    desc->constraint.range = &src->br_x_range;

    /* OPT_SCAN_BR_Y */
    desc = &dev->opt_desc[OPT_SCAN_BR_Y];
    desc->name = SANE_NAME_SCAN_BR_Y;
    desc->title = SANE_TITLE_SCAN_BR_Y;
    desc->desc = SANE_DESC_SCAN_BR_Y;
    desc->type = SANE_TYPE_FIXED;
    desc->cap = SANE_CAP_SOFT_SELECT | SANE_CAP_SOFT_DETECT;
    desc->unit = SANE_UNIT_MM;
    desc->constraint_type = SANE_CONSTRAINT_RANGE;
    desc->constraint.range = &src->br_y_range;
}

/* Set current source. Affects many other options
 */
static void
device_set_source (device *dev, OPT_SOURCE opt_src)
{
    dev->opt_src = opt_src;

    /* Choose appropriate color mode */
    devcaps_source *src = dev->caps.src[dev->opt_src];
    dev->opt_colormode = devcaps_source_choose_colormode(src,
            OPT_COLORMODE_UNKNOWN);

    /* Adjust resolution */
    dev->opt_resolution = devcaps_source_choose_resolution(src,
            DEVICE_DEFAULT_RESOLUTION);

    /* Adjust window */
    dev->opt_tl_x = 0;
    dev->opt_tl_y = 0;

    dev->opt_br_x = src->br_x_range.max;
    dev->opt_br_y = src->br_y_range.max;

    device_rebuild_opt_desc(dev);
}

/* Get device option
 */
SANE_Status
device_get_option (device *dev, SANE_Int option, void *value)
{
    SANE_Status status = SANE_STATUS_GOOD;

    switch (option) {
    case OPT_NUM_OPTIONS:
        *(SANE_Word*) value = NUM_OPTIONS;
        break;

    case OPT_SCAN_RESOLUTION:
        *(SANE_Word*) value = dev->opt_resolution;
        break;

    case OPT_SCAN_COLORMODE:
        strcpy(value, opt_colormode_to_sane(dev->opt_colormode));
        break;

    case OPT_SCAN_SOURCE:
        strcpy(value, opt_source_to_sane(dev->opt_src));
        break;

    case OPT_SCAN_TL_X:
        *(SANE_Word*) value = dev->opt_tl_x;
        break;

    case OPT_SCAN_TL_Y:
        *(SANE_Word*) value = dev->opt_tl_y;
        break;

    case OPT_SCAN_BR_X:
        *(SANE_Word*) value = dev->opt_tl_x;
        break;

    case OPT_SCAN_BR_Y:
        *(SANE_Word*) value = dev->opt_tl_y;
        break;

    default:
        status = SANE_STATUS_INVAL;
    }

    return status;
}

/* Userdata passed to device_table_foreach_callback
 */
typedef struct {
    unsigned int flags;     /* Device flags */
    unsigned int count;     /* Count of devices used so far */
    device       **devlist; /* List of devices collected so far. May be NULL */
} device_table_foreach_userdata;

/* g_tree_foreach callback for traversing device table
 */
static gboolean
device_table_foreach_callback (gpointer key, gpointer value, gpointer userdata)
{
    device *dev = value;
    device_table_foreach_userdata *data = userdata;

    (void) key;

    if (!(data->flags & dev->flags)) {
        return FALSE;
    }

    if (data->devlist != NULL) {
        data->devlist[data->count] = dev;
    }

    data->count ++;

    return FALSE;
}

/* Collect devices matching the flags. Return count of
 * collected devices. If caller is only interested in
 * the count, it is safe to call with out == NULL
 *
 * It's a caller responsibility to provide big enough
 * output buffer (use device_table_size() to make a guess)
 *
 * Caller must own glib_main_loop lock
 */
static unsigned int
device_table_collect (unsigned int flags, device *out[])
{
    device_table_foreach_userdata       data = {flags, 0, out};
    g_tree_foreach(device_table, device_table_foreach_callback, &data);
    return data.count;
}

/* Get current device_table size
 */
static unsigned
device_table_size (void)
{
    g_assert(device_table);
    return g_tree_nnodes(device_table);
}

/* Purge device_table
 */
static void
device_table_purge (void)
{
    size_t  sz = device_table_size(), i;
    device  **devices = g_new0(device*, sz);

    sz = device_table_collect(DEVICE_ALL_FLAGS, devices);
    for (i = 0; i < sz; i ++) {
        device_del(devices[i]);
    }
}

/* Check if device table is ready, i.e., there is no DEVICE_INIT_WAIT
 * devices
 */
static SANE_Bool
device_table_ready (void)
{
    return device_table_collect(DEVICE_INIT_WAIT, NULL) == 0;
}

/* ScannerCapabilities fetch callback
 */
static void
device_scanner_capabilities_callback (device *dev, SoupMessage *msg)
{
    DBG_DEVICE(dev->name, "ScannerCapabilities: status=%d", msg->status_code);

    xmlDoc      *doc = NULL;
    const char *err = NULL;

    /* Check request status */
    if (!SOUP_STATUS_IS_SUCCESSFUL(msg->status_code)) {
        err = "failed to load ScannerCapabilities";
        goto DONE;
    }

    /* Parse XML response */
    SoupBuffer *buf = soup_message_body_flatten(msg->response_body);
    doc = xmlParseMemory(buf->data, buf->length);
    soup_buffer_free(buf);

    if (doc == NULL) {
        err = "failed to parse ScannerCapabilities response XML";
        goto DONE;
    }

    err = devcaps_parse(&dev->caps, doc);
    if (err == NULL) {
        devcaps_dump(dev->name, &dev->caps);
    }

    /* Cleanup and exit */
DONE:
    if (doc != NULL) {
        xmlFreeDoc(doc);
    }

    if (err != NULL) {
        if (dev->addr_current != NULL && dev->addr_current->next != NULL) {
            device_probe_address(dev, dev->addr_current->next);
        } else {
            device_del(dev);
        }
    } else {
        /* Choose initial source */
        OPT_SOURCE opt_src = (OPT_SOURCE) 0;
        while (opt_src < NUM_OPT_SOURCE &&
                (dev->caps.src[opt_src]) == NULL) {
            opt_src ++;
        }

        g_assert(opt_src != NUM_OPT_SOURCE);
        device_set_source(dev, opt_src);

        dev->flags |= DEVICE_READY;
        dev->flags &= ~DEVICE_INIT_WAIT;
    }

    g_cond_broadcast(&device_table_cond);
}

/* User data, associated with each HTTP message
 */
typedef struct {
    device *dev;
    void   (*callback)(device *dev, SoupMessage *msg);
} device_http_userdata;

/* HTTP request completion callback
 */
static void
device_http_callback(SoupSession *session, SoupMessage *msg, gpointer userdata)
{
    (void) session;

    if (DBG_ENABLED(DBG_FLG_HTTP)) {
        SoupURI *uri = soup_message_get_uri(msg);
        char *uri_str = soup_uri_to_string(uri, FALSE);

        DBG_HTTP("%s %s: %s", msg->method, uri_str,
                soup_status_get_phrase(msg->status_code));

        g_free(uri_str);
    }

    if (msg->status_code != SOUP_STATUS_CANCELLED) {
        device_http_userdata *data = userdata;
        g_ptr_array_remove(data->dev->http_pending, msg);
        data->callback(data->dev, msg);
    }

    g_free(userdata);
}

/* Initiate HTTP request
 */
static void
device_http_get (device *dev, const char *path,
        void (*callback)(device*, SoupMessage*))
{
    SoupURI *url = soup_uri_new_with_base(dev->base_url, path);
    g_assert(url);
    SoupMessage *msg = soup_message_new_from_uri("GET", url);
    soup_uri_free(url);

    device_http_userdata *data = g_new0(device_http_userdata, 1);
    data->dev = dev;
    data->callback = callback;

    soup_session_queue_message(device_http_session, msg,
            device_http_callback, data);
    g_ptr_array_add(dev->http_pending, msg);
}

/* Get list of devices, in SANE format
 */
const SANE_Device**
device_list_get (void)
{
    /* Wait until device table is ready */
    gint64 timeout = g_get_monotonic_time() +
            DEVICE_TABLE_READY_TIMEOUT * G_TIME_SPAN_SECOND;

    while ((!device_table_ready() || zeroconf_init_scan()) &&
            g_get_monotonic_time() < timeout) {
        eloop_cond_wait(&device_table_cond, timeout);
    }

    /* Build a list */
    device            **devices = g_newa(device*, device_table_size());
    unsigned int      count = device_table_collect(DEVICE_READY, devices);
    unsigned int      i;
    const SANE_Device **dev_list = g_new0(const SANE_Device*, count + 1);

    for (i = 0; i < count; i ++) {
        SANE_Device *info = g_new0(SANE_Device, 1);
        dev_list[i] = info;

        info->name = g_strdup(devices[i]->name);
        info->vendor = g_strdup(devices[i]->caps.vendor);
        info->model = g_strdup(devices[i]->caps.model);
        info->type = "eSCL network scanner";
    }

    return dev_list;
}

/* Free list of devices, returned by device_list_get()
 */
void
device_list_free (const SANE_Device **dev_list)
{
    if (dev_list != NULL) {
        unsigned int       i;
        const SANE_Device *info;

        for (i = 0; (info = dev_list[i]) != NULL; i ++) {
            g_free((void*) info->name);
            g_free((void*) info->vendor);
            g_free((void*) info->model);
            g_free((void*) info);
        }

        g_free(dev_list);
    }
}

/* Open a device
 */
device*
device_open (const char *name)
{
    device *dev = device_find(name);
    if (dev != NULL && (dev->flags & DEVICE_READY) != 0) {
        return device_ref(dev);
    }
    return NULL;
}

/* Close the device
 */
void
device_close (device *dev)
{
    device_unref(dev);
}

/* Get option descriptor
 */
const SANE_Option_Descriptor*
dev_get_option_descriptor (device *dev, SANE_Int option)
{
    if (0 <= option && option < NUM_OPTIONS) {
        return &dev->opt_desc[option];
    }

    return NULL;
}

/* vim:ts=8:sw=4:et
 */
