#include "../pd/src/m_pd.h"
#include "../pd/src/s_stuff.h"
#include "../pd/src/s_net.h"
#include "../pd/src/m_imp.h"
#include "../pd/src/g_canvas.h"
#include "../pd/src/g_undo.h"
#include "espd.h"
#include "espd_runtime_config.h"
#include "espd_usb.h"
#include "espd_midi.h"
#include "esp_attr.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include "lwip/sockets.h"
#include "lwip/errno.h"

extern int pd_setloadingabstraction(t_symbol *sym);

int sys_trytoopenit(const char *dir, const char *name, const char *ext,
    char *dirresult, char **nameresult, unsigned int size, int bin,
    int okgui);

void pd_init(void);
void glob_open(t_pd *ignore, t_symbol *name, t_symbol *dir, t_floatarg f);

void pdmain_print( const char *s);

void trymem(int foo)
{
#if 1
    int i;
    char msg[80];
    for (i = 1; i < 500; i++)
    {
        char *foo = malloc(i*1024);
        if (foo)
            free(foo);
        else break;
    }
    sprintf(msg, "%d max mem %dk\n", foo, i-1);
    pdmain_print(msg);
#endif
}

static int espd_path_list_contains(t_namelist *nl, const char *dir)
{
    for (; nl; nl = nl->nl_next)
        if (!strcmp(nl->nl_string, dir))
            return 1;
    return 0;
}

static void espd_add_patch_dir_to_searchpath(const char *dir)
{
    if (!dir || !*dir)
        return;
    if (!espd_path_list_contains(STUFF->st_searchpath, dir))
        STUFF->st_searchpath = namelist_append(STUFF->st_searchpath, dir, 0);
    if (!espd_path_list_contains(STUFF->st_temppath, dir))
        STUFF->st_temppath = namelist_append(STUFF->st_temppath, dir, 0);
}

static void espd_print_pd_paths(const char *label)
{
    char msg[256];
    size_t used = 0;
    t_namelist *nl;

    used += (size_t)snprintf(msg + used, sizeof(msg) - used, "%s searchpath:", label);
    for (nl = STUFF->st_searchpath; nl && used + 4 < sizeof(msg); nl = nl->nl_next)
        used += (size_t)snprintf(msg + used, sizeof(msg) - used, " %s", nl->nl_string);
    used += (size_t)snprintf(msg + used, sizeof(msg) - used, "\n");
    pdmain_print(msg);

    used = 0;
    used += (size_t)snprintf(msg + used, sizeof(msg) - used, "%s temppath:", label);
    for (nl = STUFF->st_temppath; nl && used + 4 < sizeof(msg); nl = nl->nl_next)
        used += (size_t)snprintf(msg + used, sizeof(msg) - used, " %s", nl->nl_string);
    used += (size_t)snprintf(msg + used, sizeof(msg) - used, "\n");
    pdmain_print(msg);
}

void canvas_popabstraction(t_canvas *x);

static t_gobj *espd_abstraction_classes;

static t_pd *espd_create_abstraction(t_symbol *s, int argc, t_atom *argv)
{
    const char *objectname;
    char dirbuf[MAXPDSTRING], classslashclass[MAXPDSTRING], *nameptr;
    t_glist *glist;
    t_canvas *canvas;
    t_pd *was;
    int fd;

    if (!s || !s->s_name || !*s->s_name)
        return 0;
    if (pd_setloadingabstraction(s))
    {
        pd_error(0, "%s: can't load abstraction within itself\n", s->s_name);
        pd_this->pd_newest = 0;
        return 0;
    }
    objectname = s->s_name;
    glist = (t_glist *)canvas_getcurrent();
    if (!glist)
    {
        pd_this->pd_newest = 0;
        return 0;
    }
    canvas = (t_canvas *)glist_getcanvas(glist);
    was = s__X.s_thing;
    pd_snprintf(classslashclass, MAXPDSTRING, "%s/%s", objectname, objectname);
    fd = canvas_open(canvas, objectname, ".pd", dirbuf, &nameptr, MAXPDSTRING, 0);
    if (fd < 0)
        fd = canvas_open(canvas, objectname, ".pat", dirbuf, &nameptr, MAXPDSTRING, 0);
    if (fd < 0)
        fd = canvas_open(canvas, classslashclass, ".pd", dirbuf, &nameptr, MAXPDSTRING, 0);
    if (fd < 0)
    {
        pd_this->pd_newest = 0;
        return 0;
    }
    close(fd);
    espd_add_patch_dir_to_searchpath(dirbuf);
    canvas_setargs(argc, argv);
    binbuf_evalfile(gensym(nameptr), gensym(dirbuf));
    if (s__X.s_thing && was != s__X.s_thing)
        canvas_popabstraction((t_canvas *)(s__X.s_thing));
    else
        s__X.s_thing = was;
    canvas_setargs(0, 0);
    return pd_this->pd_newest;
}

static int espd_try_register_abstraction(const char *objectname, const char *path)
{
    t_class *c = 0;
    char dirbuf[MAXPDSTRING], classslashclass[MAXPDSTRING], *nameptr;
    int fd;

    if (!objectname || !*objectname)
        return 0;
    if (zgetfn(&pd_objectmaker, gensym(objectname)))
        return 1;
    if (!path || !*path)
        return 0;

    pd_snprintf(classslashclass, MAXPDSTRING, "%s/%s", objectname, objectname);
    if ((fd = sys_trytoopenit(path, objectname, ".pd",
            dirbuf, &nameptr, MAXPDSTRING, 0, 0)) < 0 &&
        (fd = sys_trytoopenit(path, objectname, ".pat",
            dirbuf, &nameptr, MAXPDSTRING, 0, 0)) < 0 &&
        (fd = sys_trytoopenit(path, classslashclass, ".pd",
            dirbuf, &nameptr, MAXPDSTRING, 0, 0)) < 0)
        return 0;
    close(fd);
    espd_add_patch_dir_to_searchpath(dirbuf);

    class_set_extern_dir(gensym(dirbuf));
    c = class_new(gensym(objectname), (t_newmethod)espd_create_abstraction,
        0, 0, 0, A_GIMME, 0);
    class_set_extern_dir(&s_);
    if (!c)
        return 0;

    {
        t_gobj *absclass = (t_gobj *)t_getbytes(sizeof(*absclass));
        absclass->g_pd = c;
        absclass->g_next = espd_abstraction_classes;
        espd_abstraction_classes = absclass;
    }
    return 1;
}

typedef struct _espd_loadlib_data
{
    t_canvas *canvas;
    const char *classname;
    int ok;
} t_espd_loadlib_data;

static int espd_loadlib_iter(const char *path, t_espd_loadlib_data *data)
{
    if (data->ok)
        return 0;
    data->ok = espd_try_register_abstraction(data->classname, path);
    return (data->ok == 0);
}

static void espd_preload_from_searchpaths(const char *classname, int *ok)
{
    t_namelist *nl;

    if (*ok)
        return;
    for (nl = STUFF->st_temppath; nl && !*ok; nl = nl->nl_next)
        *ok = espd_try_register_abstraction(classname, nl->nl_string);
    for (nl = STUFF->st_searchpath; nl && !*ok; nl = nl->nl_next)
        *ok = espd_try_register_abstraction(classname, nl->nl_string);
}

void pd_sendmsg(char *buf, int bufsize)
{
    static t_binbuf *b;
    if (!b)
        b = binbuf_new();
    binbuf_text(b, buf, bufsize);
    binbuf_eval(b, 0, 0, 0);
}

extern float soundin[], soundout[];
void  canvas_start_dsp( void);

void pdmain_reload_patch_from(const char *dir)
{
    t_canvas *c;

    if (!dir || !dir[0]) {
        pdmain_print("RELOAD failed: no target directory\n");
        return;
    }

    while ((c = pd_getcanvaslist()) != NULL)
        pd_free((t_pd *)c);

    espd_add_patch_dir_to_searchpath(dir);
    {
        t_pd *loaded = glob_evalfile(0, gensym("main.pd"), gensym(dir));
        espd_main_pd_loaded_from_store = 1;
        espd_main_pd_loaded_dir = dir;
        if (loaded && *loaded == canvas_class) {
            espd_pdcontrol_sync_cwd();
            canvas_update_dsp();
        }
    }
    pdmain_print("RELOAD done: main.pd\n");
}

void pdmain_reload_patch(void)
{
    pdmain_reload_patch_from(ESPD_SDCARD_MOUNT);
}

void pdmain_init( void)
{
    sys_printhook = pdmain_print;
    pd_init();
    STUFF->st_dacsr = sys_getsr();
    STUFF->st_soundout = soundout;
    STUFF->st_soundin = soundin;
    espd_midi_init();   /* open USB MIDI in/out; native Pd MIDI objects active */

    espd_main_pd_loaded_from_store = 0;
    espd_main_pd_loaded_dir = NULL;

    {
        const char *dir = espd_storage_main_pd_mount_dir();
        if (dir) {
            t_pd *loaded;
            espd_add_patch_dir_to_searchpath(dir);
            espd_print_pd_paths("pd");
            loaded = glob_evalfile(0, gensym("main.pd"), gensym(dir));
            if (loaded && *loaded == canvas_class) {
                espd_main_pd_loaded_from_store = 1;
                espd_main_pd_loaded_dir = dir;
                espd_pdcontrol_sync_cwd();
                canvas_update_dsp();
            }
        }
    }
    if (!espd_main_pd_loaded_from_store) {
#ifdef ESPD_USE_WIFI
#if ESPD_ENABLE_LEGACY_WIFI_TRANSPORT
            if (espd_wifi_ssid[0] == '\0') {
                snprintf(espd_wifi_ssid, sizeof(espd_wifi_ssid), "%s", CONFIG_ESP_WIFI_SSID);
                snprintf(espd_wifi_password, sizeof(espd_wifi_password), "%s", CONFIG_ESP_WIFI_PASSWORD);
            }
            espd_wifi_net_enabled = 1;
            pdmain_print("No main.pd found on SD or internal storage. Waiting for Wi-Fi connection...\n");
            wifi_start_sta();
            (void)wifi_wait_sta(pdMS_TO_TICKS(1500));
            net_init();
            net_hello();
#else
            pdmain_print("No main.pd found on SD or internal storage. System is idle.\n");
#endif
#else
        pdmain_print("No main.pd found and Wi-Fi is not compiled into firmware. System is idle.\n");
#endif
    }
}


/* IRAM_ATTR: per-block scheduler entry; pin so we can't be evicted between
 * blocks by unrelated flash activity (USB MSC, file I/O, etc.). memset is
 * already in IRAM by IDF default; sched_tick is also IRAM_ATTR below;
 * sys_pollgui stays in flash but is cheap (returns fast when no FDs). */
IRAM_ATTR void pdmain_tick( void)
{
    //memset(soundout, 0, (size_t)sys_get_outchannels() * DEFDACBLKSIZE * sizeof(t_sample));
    sched_tick();
    sys_pollgui();
    espd_midi_poll();   /* dispatch inbound USB MIDI + flush outbound MIDI queue */
}

/* ----------------- stuff to keep Pd happy -------------------- */

t_class *glob_pdobject;

void glob_mem(void *dummy);
static void glob_canvas_editmode(t_glist *x, t_floatarg f)
{
    (void)x;
    (void)f;
    /* Headless target: ignore GUI-only canvas edit mode updates. */
}

static void glob_beginnew(void *dummy, t_symbol *pname, t_symbol *pdir)
{
    glob_setfilename(0, pname, pdir);
    pd_bind(&pd_canvasmaker, &s__N);
}

static void glob_endnew(void *dummy)
{
    pd_unbind(&pd_canvasmaker, &s__N);
    if ((t_canvas *)s__X.s_thing)
    {
        canvas_loadbang((t_canvas *)s__X.s_thing);
        vmess(s__X.s_thing, gensym("pop"), "i", 0);
        /* loadbang may send dsp 1; rebuild graph once canvas is complete */
        canvas_update_dsp();
    }
    glob_setfilename(0, &s_, &s_);
}

static void glob_close(void *dummy, t_symbol *pname)
{
    t_pd *c = pd_findbyclass(pname, canvas_class);
    if (c)
        pd_free(c);
 }


void glob_dsp(void *dummy, t_symbol *s, int argc, t_atom *argv);

void glob_init( void)
{
    glob_pdobject = class_new(gensym("pd"), 0, 0, sizeof(t_pd),
        CLASS_DEFAULT, A_NULL);
    class_addmethod(glob_pdobject, (t_method)glob_dsp, gensym("dsp"),
        A_GIMME, 0);
    class_addmethod(glob_pdobject, (t_method)glob_mem, gensym("espd/check/mem"), 0);
    class_addmethod(glob_pdobject, (t_method)glob_beginnew, gensym("begin-new"),
        A_SYMBOL, A_SYMBOL, 0);
    class_addmethod(glob_pdobject, (t_method)glob_close, gensym("close"),
        A_SYMBOL, 0);
    class_addmethod(glob_pdobject, (t_method)glob_endnew, gensym("end-new"),
        0);
    class_addmethod(canvas_class, (t_method)glob_canvas_editmode,
        gensym("editmode"), A_FLOAT, 0);
    pd_bind(&glob_pdobject, gensym("pd"));
}

void g_array_setup(void);
void g_canvas_setup(void);
void g_guiconnect_setup(void);
/* iemlib */
void g_bang_setup(void);
void g_mycanvas_setup(void);
void g_numbox_setup(void);
void g_radio_setup(void);
void g_slider_setup(void);
void g_toggle_setup(void);
void g_vumeter_setup(void);
/* iemlib */
void g_io_setup(void);
void g_scalar_setup(void);
void g_template_setup(void);
void g_text_setup(void);
void g_traversal_setup(void);
void clone_setup(void);
void m_pd_setup(void);
void x_acoustics_setup(void);
void x_interface_setup(void);
void x_connective_setup(void);
void x_time_setup(void);
void x_arithmetic_setup(void);
void x_array_setup(void);
void x_midi_setup(void);
void x_misc_setup(void);
void x_net_setup(void);
void x_qlist_setup(void);
void x_gui_setup(void);
void x_list_setup(void);
void x_scalar_setup(void);
void expr_setup(void);
void d_arithmetic_setup(void);
void d_array_setup(void);
void d_ctl_setup(void);
void d_dac_setup(void);
void d_delay_setup(void);
void d_fft_setup(void);
void d_filter_setup(void);
void d_global_setup(void);
void d_math_setup(void);
void d_misc_setup(void);
void d_osc_setup(void);
void d_soundfile_setup(void);
void d_ugen_setup(void);
void espdsp_osc_override_setup(void);
void espd_pdcontrol_setup(void);
void x_file_setup(void);
void espd_cputime_setup(void);

void conf_init(void)
{
    trymem(10);
    g_array_setup();
    g_bang_setup();
    g_canvas_setup();
    g_mycanvas_setup();
    g_numbox_setup();
    g_radio_setup();
    g_slider_setup();
    g_text_setup();
    g_toggle_setup();
    g_vumeter_setup();
    x_time_setup();
    x_interface_setup();
    x_misc_setup();
    g_guiconnect_setup();
    g_scalar_setup();
    g_template_setup();
    m_pd_setup();
    x_acoustics_setup();
    x_interface_setup();
    x_connective_setup();
    x_time_setup();
    x_arithmetic_setup();
    x_array_setup();
    x_midi_setup();
    x_net_setup();
    x_misc_setup();
    x_qlist_setup();
    x_gui_setup();
    x_list_setup();
    x_scalar_setup();
    g_io_setup();
    d_global_setup();
    d_soundfile_setup();
    d_ugen_setup();
    d_dac_setup();
    d_ctl_setup();
    d_osc_setup();
    d_arithmetic_setup();
    d_array_setup();
    espdsp_osc_override_setup();
    clone_setup();
    d_delay_setup();
    d_filter_setup();
    d_math_setup();
    d_fft_setup();
    d_misc_setup();
    expr_setup();
    x_file_setup();
    espd_pdcontrol_setup();
    espd_cputime_setup();
    trymem(11);
}

/*
    g_traversal_setup();
*/

/* ------- STUBS that do nothing ------------- */
int sys_get_outchannels(void) {return(IOCHANS); }
int sys_get_inchannels(void) {return(IOCHANS); }
float sys_getsr( void) { return ((float)espd_audio_sample_rate_hz()); }
int sys_getblksize(void) { return (DEFDACBLKSIZE); }

int pd_compatibilitylevel = 100;
int sys_verbose = 0;
int sys_noloadbang = 0;

int audio_shouldkeepopen(void) { return (0);}
int audio_isopen( void) { return (1); }
void sys_reopen_audio ( void) { }
void sys_close_audio ( void) { }

t_symbol *sys_libdir = &s_;

void sys_vgui(const char *format, ...) {}
void sys_gui(const char *s) { }

typedef struct _fdpoll
{
    int fd;
    t_fdpollfn fn;
    void *ptr;
} t_fdpoll;

#define ESPD_MAX_FDPOLL 32
static t_fdpoll s_fdpolls[ESPD_MAX_FDPOLL];
static int s_nfdpoll;
static unsigned char s_recvbuf[NET_MAXPACKETSIZE];
static t_binbuf *s_net_binbuf;

struct _socketreceiver
{
    char *sr_inbuf;
    int sr_inhead;
    int sr_intail;
    void *sr_owner;
    int sr_udp;
    struct sockaddr_storage *sr_fromaddr;
    t_socketnotifier sr_notifier;
    t_socketreceivefn sr_socketreceivefn;
    t_socketfromaddrfn sr_fromaddrfn;
};

#define INBUFSIZE 4096

unsigned char *sys_getrecvbuf(unsigned int *size)
{
    if (size)
        *size = NET_MAXPACKETSIZE;
    return s_recvbuf;
}

void sys_sockerror(const char *s)
{
    pd_error(0, "%s: %s (%d)", s, strerror(errno), errno);
}

void sys_closesocket(int fd)
{
    close(fd);
}

void sys_addpollfn(int fd, t_fdpollfn fn, void *ptr)
{
    int i;
    for (i = 0; i < s_nfdpoll; i++)
        if (s_fdpolls[i].fd == fd)
            return;
    if (s_nfdpoll >= ESPD_MAX_FDPOLL)
    {
        post("warning: fdpoll list full");
        return;
    }
    s_fdpolls[s_nfdpoll].fd = fd;
    s_fdpolls[s_nfdpoll].fn = fn;
    s_fdpolls[s_nfdpoll].ptr = ptr;
    s_nfdpoll++;
}

void sys_rmpollfn(int fd)
{
    int i;
    for (i = 0; i < s_nfdpoll; i++)
    {
        if (s_fdpolls[i].fd == fd)
        {
            for (; i + 1 < s_nfdpoll; i++)
                s_fdpolls[i] = s_fdpolls[i + 1];
            s_nfdpoll--;
            return;
        }
    }
}

static int socketreceiver_doread(t_socketreceiver *x)
{
    char messbuf[INBUFSIZE], *bp = messbuf;
    int indx, first = 1;
    int inhead = x->sr_inhead;
    int intail = x->sr_intail;
    char *inbuf = x->sr_inbuf;
    for (indx = intail; first || (indx != inhead);
        first = 0, (indx = (indx + 1) & (INBUFSIZE - 1)))
    {
        char c = *bp++ = inbuf[indx];
        if (c == ';' && (!indx || inbuf[indx - 1] != '\\'))
        {
            x->sr_intail = (indx + 1) & (INBUFSIZE - 1);
            binbuf_text(s_net_binbuf, messbuf, bp - messbuf);
            return 1;
        }
    }
    return 0;
}

t_socketreceiver *socketreceiver_new(void *owner, t_socketnotifier notifier,
    t_socketreceivefn socketreceivefn, int udp)
{
    t_socketreceiver *x = (t_socketreceiver *)getbytes(sizeof(*x));
    x->sr_inhead = x->sr_intail = 0;
    x->sr_owner = owner;
    x->sr_notifier = notifier;
    x->sr_socketreceivefn = socketreceivefn;
    x->sr_udp = udp;
    x->sr_fromaddr = NULL;
    x->sr_fromaddrfn = NULL;
    x->sr_inbuf = udp ? NULL : (char *)getbytes(INBUFSIZE);
    if (!s_net_binbuf)
        s_net_binbuf = binbuf_new();
    return x;
}

void socketreceiver_set_fromaddrfn(t_socketreceiver *x,
    t_socketfromaddrfn fromaddrfn)
{
    x->sr_fromaddrfn = fromaddrfn;
    if (fromaddrfn && !x->sr_fromaddr)
        x->sr_fromaddr = (struct sockaddr_storage *)getbytes(sizeof(struct sockaddr_storage));
    else if (!fromaddrfn && x->sr_fromaddr)
    {
        freebytes(x->sr_fromaddr, sizeof(struct sockaddr_storage));
        x->sr_fromaddr = NULL;
    }
}

void socketreceiver_free(t_socketreceiver *x)
{
    if (x->sr_inbuf)
        freebytes(x->sr_inbuf, INBUFSIZE);
    if (x->sr_fromaddr)
        freebytes(x->sr_fromaddr, sizeof(struct sockaddr_storage));
    freebytes(x, sizeof(*x));
}

void socketreceiver_read(t_socketreceiver *x, int fd)
{
    if (x->sr_udp)
    {
        char *buf = (char *)sys_getrecvbuf(0);
        socklen_t fromaddrlen = sizeof(struct sockaddr_storage);
        int ret = (int)recvfrom(fd, buf, NET_MAXPACKETSIZE - 1, 0,
            (struct sockaddr *)x->sr_fromaddr, (x->sr_fromaddr ? &fromaddrlen : 0));
        if (ret <= 0)
        {
            if (ret < 0 && x->sr_notifier)
                (*x->sr_notifier)(x->sr_owner, fd);
            return;
        }
        buf[ret] = 0;
        if (x->sr_fromaddrfn)
            (*x->sr_fromaddrfn)(x->sr_owner, (const void *)x->sr_fromaddr);
        binbuf_text(s_net_binbuf, buf, strlen(buf));
        outlet_setstacklim();
        if (x->sr_socketreceivefn)
            (*x->sr_socketreceivefn)(x->sr_owner, s_net_binbuf);
        return;
    }
    else
    {
        int readto = (x->sr_inhead >= x->sr_intail ? INBUFSIZE : x->sr_intail - 1);
        int ret;
        if (readto == x->sr_inhead)
            x->sr_inhead = x->sr_intail = 0, readto = INBUFSIZE;
        ret = (int)recv(fd, x->sr_inbuf + x->sr_inhead, readto - x->sr_inhead, 0);
        if (ret <= 0)
        {
            if (x->sr_notifier)
                (*x->sr_notifier)(x->sr_owner, fd);
            sys_rmpollfn(fd);
            sys_closesocket(fd);
            return;
        }
        x->sr_inhead += ret;
        if (x->sr_inhead >= INBUFSIZE)
            x->sr_inhead = 0;
        while (socketreceiver_doread(x))
        {
            if (x->sr_fromaddrfn && x->sr_fromaddr)
            {
                socklen_t fromaddrlen = sizeof(struct sockaddr_storage);
                if (!getpeername(fd, (struct sockaddr *)x->sr_fromaddr, &fromaddrlen))
                    (*x->sr_fromaddrfn)(x->sr_owner, (const void *)x->sr_fromaddr);
            }
            outlet_setstacklim();
            if (x->sr_socketreceivefn)
                (*x->sr_socketreceivefn)(x->sr_owner, s_net_binbuf);
            else
                binbuf_eval(s_net_binbuf, 0, 0, 0);
            if (x->sr_inhead == x->sr_intail)
                break;
        }
    }
}

int sys_havegui(void) {return (0);}
int sys_havetkproc(void) {return (0);}
int sys_pollgui(void)
{
    fd_set readset;
    struct timeval timeout = {0, 0};
    int i, maxfd = -1, did = 0;
    FD_ZERO(&readset);
    for (i = 0; i < s_nfdpoll; i++)
    {
        FD_SET(s_fdpolls[i].fd, &readset);
        if (s_fdpolls[i].fd > maxfd)
            maxfd = s_fdpolls[i].fd;
    }
    if (maxfd < 0)
        return 0;
#ifdef ESP_PLATFORM
    /* Use lwIP select directly on ESP-IDF to avoid VFS select lock contention. */
    if (lwip_select(maxfd + 1, &readset, NULL, NULL, &timeout) <= 0)
        return 0;
#else
    if (select(maxfd + 1, &readset, NULL, NULL, &timeout) <= 0)
        return 0;
#endif
    for (i = 0; i < s_nfdpoll; i++)
    {
        if (FD_ISSET(s_fdpolls[i].fd, &readset))
        {
            s_fdpolls[i].fn(s_fdpolls[i].ptr, s_fdpolls[i].fd);
            did = 1;
        }
    }
    return did;
}

void sys_lock(void) {}
void sys_unlock(void) {}
void pd_globallock(void) {}
void pd_globalunlock(void) {}
int sys_defaultfont = 1;
int sys_nearestfontsize(int fontsize) {return (1);}
int sys_noautopatch = 1;

void canvas_undo_cleardirty(t_canvas *x) {}

t_undo_action *canvas_undo_init(t_canvas *x) {return (0);}
t_undo_action *canvas_undo_add(t_canvas *x,
 t_undo_type type, const char *name, void *data) {return (0);}
void canvas_undo_undo(t_canvas *x) {}
void canvas_undo_redo(t_canvas *x) {}
void canvas_undo_rebranch(t_canvas *x) {}
void canvas_undo_check_canvas_pointers(t_canvas *x) {}
void canvas_undo_purge_abstraction_actions(t_canvas *x) {}
void canvas_undo_free(t_canvas *x) {}
void *canvas_undo_set_create(t_canvas *x) { return (0); }
void *canvas_undo_set_recreate(t_canvas *x,
    t_gobj *y, int old_pos) { return (0); }
void canvas_noundo(t_canvas *x) {}
void canvas_properties(t_canvas *x) {}
void pd_undo_set_objectstate(t_canvas *canvas, t_pd *x, t_symbol *s,
                                    int undo_argc, t_atom *undo_argv,
                                    int redo_argc, t_atom *redo_argv) {}

void glist_select(t_glist *x, t_gobj *y) {}
void glist_deselect(t_glist *x, t_gobj *y) {}
void glist_noselect(t_glist *x) {}
int glist_isselected(t_glist *x, t_gobj *g) {return (0);}
void glist_getnextxy(t_glist *gl, int *xpix, int *ypix) {*xpix = *ypix = 40;}
int glist_getindex(t_glist *x, t_gobj *y) {return (0);}

void canvas_create_editor(t_canvas *x) {}
void canvas_destroy_editor(t_canvas *x) {}
void canvas_editor_for_class(t_class *c) {}
void g_editor_setup( void) {}
void canvas_finderror(const void *error_object) {}
void canvas_setcursor(t_canvas *x, unsigned int cursornum) {}
void canvas_setgraph(t_glist *x, int flag, int nogoprect) {}
int canvas_hitbox(t_canvas *x, t_gobj *y, int xpos, int ypos,
    int *x1p, int *y1p, int *x2p, int *y2p, int extrapix)
{
    (void)x;
    (void)y;
    (void)xpos;
    (void)ypos;
    (void)x1p;
    (void)y1p;
    (void)x2p;
    (void)y2p;
    (void)extrapix;
    return (0);
}
void canvas_restoreconnections(t_canvas *x) {}
void canvas_reload(t_symbol *name, t_symbol *dir, t_glist *except) {}

void sys_queuegui(void *client, t_glist *glist, t_guicallbackfn f) {}
void sys_unqueuegui(void *client) {}
char sys_fontweight[10] = "no";
char sys_font[10] = "no";
int sys_hostfontsize(int fontsize, int zoom) { return (1);}
int sys_zoom_open = 1;
int sys_zoomfontwidth(int fontsize, int zoom, int worstcase) { return (1);}
int sys_zoomfontheight(int fontsize, int zoom, int worstcase) { return (1);}

int sys_load_lib(t_canvas *canvas, const char *classname)
{
    t_espd_loadlib_data data;
    int dspstate;

    if (!classname || !*classname)
        return 0;
    if (zgetfn(&pd_objectmaker, gensym(classname)))
        return 1;

    data.canvas = canvas;
    data.classname = classname;
    data.ok = 0;

    dspstate = canvas_suspend_dsp();
    if (canvas)
        canvas_path_iterate(canvas, (t_canvas_path_iterator)espd_loadlib_iter, &data);
    if (!data.ok)
        espd_preload_from_searchpaths(classname, &data.ok);
    canvas_resume_dsp(dspstate);
    return data.ok;
}

t_rtext *glist_textedfor(t_glist *gl)
{
    (void)gl;
    return 0;
}

void glist_settexted(t_glist *gl, t_rtext *x)
{
    (void)gl;
    (void)x;
}

int sys_batch;

void s_inter_newpdinstance( void) {}
/* x_midi_newpdinstance() / x_midi_freepdinstance() are provided by the now-compiled
 * pd/src/x_midi.c (allocates pd_this->pd_midi). */

/* --------------- m_sched.c -------------------- */
#define TIMEUNITPERMSEC (32. * 441.)
#define TIMEUNITPERSECOND (TIMEUNITPERMSEC * 1000.)
void dsp_tick(void);
int sys_quit = 0;
void sched_init(void) {}


typedef void (*t_clockmethod)(void *client);

struct _clock
{
    double c_settime;       /* in TIMEUNITS; <0 if unset */
    void *c_owner;
    t_clockmethod c_fn;
    struct _clock *c_next;
    t_float c_unit;         /* >0 if in TIMEUNITS; <0 if in samples */
};

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

t_clock *clock_new(void *owner, t_method fn)
{
    t_clock *x = (t_clock *)getbytes(sizeof *x);
    x->c_settime = -1;
    x->c_owner = owner;
    x->c_fn = (t_clockmethod)fn;
    x->c_next = 0;
    x->c_unit = TIMEUNITPERMSEC;
    return (x);
}

void clock_unset(t_clock *x)
{
    if (x->c_settime >= 0)
    {
        if (x == pd_this->pd_clock_setlist)
            pd_this->pd_clock_setlist = x->c_next;
        else
        {
            t_clock *x2 = pd_this->pd_clock_setlist;
            while (x2->c_next != x) x2 = x2->c_next;
            x2->c_next = x->c_next;
        }
        x->c_settime = -1;
    }
}

    /* set the clock to call back at an absolute system time */
void clock_set(t_clock *x, double setticks)
{
    if (setticks < pd_this->pd_systime) setticks = pd_this->pd_systime;
    clock_unset(x);
    x->c_settime = setticks;
    if (pd_this->pd_clock_setlist &&
        pd_this->pd_clock_setlist->c_settime <= setticks)
    {
        t_clock *cbefore, *cafter;
        for (cbefore = pd_this->pd_clock_setlist,
            cafter = pd_this->pd_clock_setlist->c_next;
                cbefore; cbefore = cafter, cafter = cbefore->c_next)
        {
            if (!cafter || cafter->c_settime > setticks)
            {
                cbefore->c_next = x;
                x->c_next = cafter;
                return;
            }
        }
    }
    else x->c_next = pd_this->pd_clock_setlist, pd_this->pd_clock_setlist = x;
}

    /* set the clock to call back after a delay in msec */
void clock_delay(t_clock *x, double delaytime)
{
    clock_set(x, (x->c_unit > 0 ?
        pd_this->pd_systime + x->c_unit * delaytime :
            pd_this->pd_systime -
                (x->c_unit*(TIMEUNITPERSECOND/STUFF->st_dacsr)) * delaytime));
}

    /* set the time unit in msec or (if 'samps' is set) in samples.  This
    is flagged by setting c_unit negative.  If the clock is currently set,
    recalculate the delay based on the new unit and reschedule */
void clock_setunit(t_clock *x, double timeunit, int sampflag)
{
    double timeleft;
    if (timeunit <= 0)
        timeunit = 1;
    /* if no change, return to avoid truncation errors recalculating delay */
    if ((sampflag && (timeunit == -x->c_unit)) ||
        (!sampflag && (timeunit == x->c_unit * TIMEUNITPERMSEC)))
            return;

        /* figure out time left in the units we were in */
    timeleft = (x->c_settime < 0 ? -1 :
        (x->c_settime - pd_this->pd_systime)/((x->c_unit > 0)? x->c_unit :
            (x->c_unit*(TIMEUNITPERSECOND/STUFF->st_dacsr))));
    if (sampflag)
        x->c_unit = -timeunit;  /* negate to flag sample-based */
    else x->c_unit = timeunit * TIMEUNITPERMSEC;
    if (timeleft >= 0)  /* reschedule if already set */
        clock_delay(x, timeleft);
}

    /* get current logical time.  We don't specify what units this is in;
    use clock_gettimesince() to measure intervals from time of this call. */
double clock_getlogicaltime(void)
{
    return (pd_this->pd_systime);
}

    /* OBSOLETE (misleading) function name kept for compatibility */
double clock_getsystime(void) { return (pd_this->pd_systime); }

    /* elapsed time in milliseconds since the given system time */
double clock_gettimesince(double prevsystime)
{
    return ((pd_this->pd_systime - prevsystime)/TIMEUNITPERMSEC);
}

    /* elapsed time in units, ala clock_setunit(), since given system time */
double clock_gettimesincewithunits(double prevsystime,
    double units, int sampflag)
{
            /* If in samples, divide TIMEUNITPERSECOND/sys_dacsr first (at
            cost of an extra division) since it's probably an integer and if
            units == 1 and (sys_time - prevsystime) is an integer number of
            DSP ticks, the result will be exact. */
    if (sampflag)
        return ((pd_this->pd_systime - prevsystime)/
            ((TIMEUNITPERSECOND/STUFF->st_dacsr)*units));
    else return ((pd_this->pd_systime - prevsystime)/(TIMEUNITPERMSEC*units));
}

    /* what value the system clock will have after a delay */
double clock_getsystimeafter(double delaytime)
{
    return (pd_this->pd_systime + TIMEUNITPERMSEC * delaytime);
}

void clock_free(t_clock *x)
{
    clock_unset(x);
    freebytes(x, sizeof *x);
}
    /* take the scheduler forward one DSP tick, also handling clock timeouts */
    /* IRAM_ATTR: called every block; the callees (dsp_tick, sys_pollgui)
     * remain in flash, but pinning sched_tick itself keeps the block-rate
     * scheduler shell out of cache contention. */
IRAM_ATTR void sched_tick(void)
{
    /* st_schedblocksize and st_dacsr are constants for the lifetime of an
     * audio session; cache the per-block systime increment so the per-block
     * float→double conversion + double mul + double add (all emulated on
     * Xtensa LX7, no hardware double FPU) only run when SR or block size
     * actually change. */
    static double cached_increment;
    static t_float cached_dacsr;
    static t_float cached_block;
    t_float dacsr = STUFF->st_dacsr;
    t_float block = STUFF->st_schedblocksize;
    if (dacsr != cached_dacsr || block != cached_block)
    {
        cached_dacsr = dacsr;
        cached_block = block;
        cached_increment =
            (double)block / (double)dacsr * (double)TIMEUNITPERSECOND;
    }
    double next_sys_time = pd_this->pd_systime + cached_increment;
    int countdown = 5000;
    while (pd_this->pd_clock_setlist &&
        pd_this->pd_clock_setlist->c_settime < next_sys_time)
    {
        t_clock *c = pd_this->pd_clock_setlist;
        pd_this->pd_systime = c->c_settime;
        clock_unset(pd_this->pd_clock_setlist);
        outlet_setstacklim();
        (*c->c_fn)(c->c_owner);
        if (!countdown--)
        {
            countdown = 5000;
            sys_pollgui();
        }
        if (sys_quit)
            return;
    }
    pd_this->pd_systime = next_sys_time;
    dsp_tick();
}

/* ------------------------ g_editor.c ---------------------- */

struct _instanceeditor
{
    t_binbuf *copy_binbuf;
    char *canvas_textcopybuf;
    int canvas_textcopybufsize;
    t_undofn canvas_undo_fn;         /* current undo function if any */
    int canvas_undo_whatnext;        /* whether we can now UNDO or REDO */
    void *canvas_undo_buf;           /* data private to the undo function */
    t_canvas *canvas_undo_canvas;    /* which canvas we can undo on */
    const char *canvas_undo_name;
    int canvas_undo_already_set_move;
    double canvas_upclicktime;
    int canvas_upx, canvas_upy;
    int canvas_find_index, canvas_find_wholeword;
    t_binbuf *canvas_findbuf;
    int paste_onset;
    t_canvas *paste_canvas;
    t_glist *canvas_last_glist;
    int canvas_last_glist_x, canvas_last_glist_y;
    t_canvas *canvas_cursorcanvaswas;
    unsigned int canvas_cursorwas;
};

extern t_class *text_class;

#define THISED (pd_this->pd_gui->i_editor)

void canvas_startmotion(t_canvas *x)
{
    int xval, yval;
    if (!x->gl_editor) return;
    glist_getnextxy(x, &xval, &yval);
    if (xval == 0 && yval == 0) return;
    x->gl_editor->e_onmotion = MA_MOVE;
    x->gl_editor->e_xwas = xval;
    x->gl_editor->e_ywas = yval;
}

void g_editor_newpdinstance(void)
{
    THISED = getbytes(sizeof(*THISED));
        /* other stuff is null-checked but this needs to exist: */
    THISED->copy_binbuf = binbuf_new();
}

void g_editor_freepdinstance(void)
{
    if (THISED->copy_binbuf)
        binbuf_free(THISED->copy_binbuf);
    if (THISED->canvas_undo_buf)
    {
        if (!THISED->canvas_undo_fn)
            bug("g_editor_freepdinstance");
        else (*THISED->canvas_undo_fn)
            (THISED->canvas_undo_canvas, THISED->canvas_undo_buf, UNDO_FREE);
    }
    if (THISED->canvas_findbuf)
        binbuf_free(THISED->canvas_findbuf);
    freebytes(THISED, sizeof(*THISED));
}

void canvas_connect(t_canvas *x, t_floatarg fwhoout, t_floatarg foutno,
    t_floatarg fwhoin, t_floatarg finno)
{
    int whoout = fwhoout, outno = foutno, whoin = fwhoin, inno = finno;
    t_gobj *src = 0, *sink = 0;
    t_object *objsrc, *objsink;
    t_outconnect *oc;
    int nin = whoin, nout = whoout;
    if (THISED->paste_canvas == x)
        whoout += THISED->paste_onset,
        whoin += THISED->paste_onset;
    for (src = x->gl_list; whoout; src = src->g_next, whoout--)
        if (!src->g_next) {
            src = NULL;
            logpost(sink, PD_DEBUG, "cannot connect non-existing object");
            goto bad; /* bug fix thanks to Hannes */
        }
    for (sink = x->gl_list; whoin; sink = sink->g_next, whoin--)
        if (!sink->g_next) {
            sink = NULL;
            logpost(src, PD_DEBUG, "cannot connect to non-existing object");
            goto bad;
        }

        /* check they're both patchable objects */
    if (!(objsrc = pd_checkobject(&src->g_pd)) ||
        !(objsink = pd_checkobject(&sink->g_pd))) {
        logpost(src?src:sink, PD_DEBUG, "cannot connect unpatchable object");
        goto bad;
    }

        /* if object creation failed, make dummy inlets or outlets
           as needed */
    if (pd_class(&src->g_pd) == text_class && objsrc->te_type == T_OBJECT)
        while (outno >= obj_noutlets(objsrc))
            outlet_new(objsrc, 0);
    if (pd_class(&sink->g_pd) == text_class && objsink->te_type == T_OBJECT)
        while (inno >= obj_ninlets(objsink))
            inlet_new(objsink, &objsink->ob_pd, 0, 0);

    if (!(oc = obj_connect(objsrc, outno, objsink, inno))) goto bad;
    if (glist_isvisible(x) && x->gl_havewindow)
    {
        char tag[128];
        char*tags[] = {tag, "cord"};
        sprintf(tag, "l%p", oc);
        pdgui_vmess(0, "crr iiii ri rS",
            glist_getcanvas(x), "create", "line",
            0, 0, 0, 0,
            "-width", (obj_issignaloutlet(objsrc, outno) ? 2 : 1) * x->gl_zoom,
            "-tags", 2, tags);
        canvas_fixlinesfor(x, objsrc);
    }
    return;

bad:
    post("%s %d %d %d %d (%s->%s) connection failed",
        x->gl_name->s_name, nout, outno, nin, inno,
            (src? class_getname(pd_class(&src->g_pd)) : "???"),
            (sink? class_getname(pd_class(&sink->g_pd)) : "???"));
}


void canvas_vis(t_canvas *x, t_floatarg f) {}

/* ------------------- s_file.c ------------------------ */
#define DEBUG(x)

/* add a single item to a namelist.  If "allowdup" is true, duplicates
may be added; otherwise they're dropped.  */

t_namelist *namelist_append(t_namelist *listwas, const char *s, int allowdup)
{
    t_namelist *nl, *nl2;
    nl2 = (t_namelist *)(getbytes(sizeof(*nl)));
    nl2->nl_next = 0;
    nl2->nl_string = (char *)getbytes(strlen(s) + 1);
    strcpy(nl2->nl_string, s);
    sys_unbashfilename(nl2->nl_string, nl2->nl_string);
    if (!listwas)
        return (nl2);
    else
    {
        for (nl = listwas; ;)
        {
            if (!allowdup && !strcmp(nl->nl_string, s))
            {
                freebytes(nl2->nl_string, strlen(nl2->nl_string) + 1);
                return (listwas);
            }
            if (!nl->nl_next)
                break;
            nl = nl->nl_next;
        }
        nl->nl_next = nl2;
    }
    return (listwas);
}

void namelist_free(t_namelist *listwas)
{
    t_namelist *nl, *nl2;
    for (nl = listwas; nl; nl = nl2)
    {
        nl2 = nl->nl_next;
        t_freebytes(nl->nl_string, strlen(nl->nl_string) + 1);
        t_freebytes(nl, sizeof(*nl));
    }
}

    /* change '/' characters to the system's native file separator */
void sys_bashfilename(const char *from, char *to)
{
    char c;
    while ((c = *from++))
    {
#ifdef _WIN32
        if (c == '/') c = '\\';
#endif
        *to++ = c;
    }
    *to = 0;
}

    /* change the system's native file separator to '/' characters  */
void sys_unbashfilename(const char *from, char *to)
{
    char c;
    while ((c = *from++))
    {
#ifdef _WIN32
        if (c == '\\') c = '/';
#endif
        *to++ = c;
    }
    *to = 0;
}

/* test if path is absolute or relative, based on leading /, env vars, ~, etc */
int sys_isabsolutepath(const char *dir)
{
    if (dir[0] == '/' || dir[0] == '~'
#ifdef _WIN32
        || dir[0] == '%' || (dir[1] == ':' && dir[2] == '/')
#endif
        )
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

/* expand env vars and ~ at the beginning of a path and make a copy to return */
void sys_expandpath(const char *from, char *to, int bufsize)
{
    if ((strlen(from) == 1 && from[0] == '~') || (strncmp(from,"~/", 2) == 0))
    {
#ifdef _WIN32
        const char *home = getenv("USERPROFILE");
#else
        const char *home = getenv("HOME");
#endif
        if (home)
        {
            strncpy(to, home, bufsize);
            to[bufsize-1] = 0;
            strncpy(to + strlen(to), from + 1, bufsize - strlen(to));
            to[bufsize-1] = 0;
        }
        else *to = 0;
    }
    else
    {
        strncpy(to, from, bufsize);
        to[bufsize-1] = 0;
    }
#ifdef _WIN32
    {
        char *buf = alloca(bufsize);
        ExpandEnvironmentStrings(to, buf, bufsize-1);
        buf[bufsize-1] = 0;
        strncpy(to, buf, bufsize);
        to[bufsize-1] = 0;
    }
#endif
}


int sys_open(const char *path, int oflag, ...)
{
    int fd;
    char pathbuf[MAXPDSTRING];
    sys_bashfilename(path, pathbuf);
    if (oflag & O_CREAT)
    {
        mode_t mode;
        int imode;
        va_list ap;
        va_start(ap, oflag);

        /* Mac compiler complains if we just set mode = va_arg ... so, even
        though we all know it's just an int, we explicitly va_arg to an int
        and then convert.
           -> http://www.mail-archive.com/bug-gnulib@gnu.org/msg14212.html
           -> http://bugs.debian.org/647345
        */

        imode = va_arg (ap, int);
        mode = (mode_t)imode;
        va_end(ap);
        fd = open(pathbuf, oflag, mode);
    }
    else
        fd = open(pathbuf, oflag);
    return fd;
}

FILE *sys_fopen(const char *filename, const char *mode)
{
  char namebuf[MAXPDSTRING];
  sys_bashfilename(filename, namebuf);
  return fopen(namebuf, mode);
}

   /* close a previously opened file
   this is needed on platforms where you cannot open/close resources
   across dll-boundaries, but we provide it for other platforms as well */
int sys_close(int fd)
{
    return close(fd);
}

int sys_fclose(FILE *stream)
{
    return fclose(stream);
}

int sys_usestdpath = 0;



    /* try to open a file in the directory "dir", named "name""ext",
    for reading.  "Name" may have slashes.  The directory is copied to
    "dirresult" which must be at least "size" bytes.  "nameresult" is set
    to point to the filename (copied elsewhere into the same buffer).
    The "bin" flag requests opening for binary (which only makes a difference
    on Windows). */

int sys_trytoopenit(const char *dir, const char *name, const char* ext,
    char *dirresult, char **nameresult, unsigned int size, int bin,
    int okgui)
{
    int fd;
    char buf[MAXPDSTRING];
    if (strlen(dir) + strlen(name) + strlen(ext) + 4 > size)
        return (-1);
    sys_expandpath(dir, buf, MAXPDSTRING);
    strcpy(dirresult, buf);
    if (*dirresult && dirresult[strlen(dirresult)-1] != '/')
        strcat(dirresult, "/");
    strcat(dirresult, name);
    strcat(dirresult, ext);

    DEBUG(post("looking for %s",dirresult));
        /* see if we can open the file for reading */
    if ((fd=sys_open(dirresult, O_RDONLY)) >= 0)
    {
            /* in unix, further check that it's not a directory */
#ifdef HAVE_UNISTD_H
        struct stat statbuf;
        int ok =  ((fstat(fd, &statbuf) >= 0) &&
            !S_ISDIR(statbuf.st_mode));
        if (!ok)
        {
            if (okgui)
                logpost(NULL, PD_VERBOSE, "tried %s; stat failed or directory",
                    dirresult);
            close (fd);
            fd = -1;
        }
        else
#endif
        {
            char *slash;
            if (okgui)
                logpost(NULL, PD_VERBOSE, "tried %s and succeeded", dirresult);
            sys_unbashfilename(dirresult, dirresult);
            slash = strrchr(dirresult, '/');
            if (slash)
            {
                *slash = 0;
                *nameresult = slash + 1;
            }
            else *nameresult = dirresult;

            return (fd);
        }
    }
    else
    {
        if (okgui)
            logpost(NULL, PD_VERBOSE, "tried %s and failed", dirresult);
    }
    return (-1);
}

    /* check if we were given an absolute pathname, if so try to open it
    and return 1 to signal the caller to cancel any path searches */
int sys_open_absolute(const char *name, const char* ext,
    char *dirresult, char **nameresult, unsigned int size, int bin, int *fdp,
    int okgui)
{
    if (sys_isabsolutepath(name))
    {
        char dirbuf[MAXPDSTRING], *z = strrchr(name, '/');
        int dirlen;
        if (!z)
            return (0);
        dirlen = (int)(z - name);
        if (dirlen > MAXPDSTRING-1)
            dirlen = MAXPDSTRING-1;
        strncpy(dirbuf, name, dirlen);
        dirbuf[dirlen] = 0;
        *fdp = sys_trytoopenit(dirbuf, name+(dirlen+1), ext,
            dirresult, nameresult, size, bin, 1);
        return (1);
    }
    else return (0);
}

int do_open_via_path(const char *dir, const char *name,
    const char *ext, char *dirresult, char **nameresult, unsigned int size,
    int bin, t_namelist *searchpath, int okgui)
{
    t_namelist *nl;
    int fd = -1;

        /* first check if "name" is absolute (and if so, try to open) */
    if (sys_open_absolute(name, ext, dirresult, nameresult, size, bin, &fd, 1))
        return (fd);

        /* otherwise "name" is relative; try the directory "dir" first. */
    if ((fd = sys_trytoopenit(dir, name, ext,
        dirresult, nameresult, size, bin, 1)) >= 0)
            return (fd);

        /* next go through the temp paths from the commandline */
    for (nl = STUFF->st_temppath; nl; nl = nl->nl_next)
        if ((fd = sys_trytoopenit(nl->nl_string, name, ext,
            dirresult, nameresult, size, bin, 1)) >= 0)
                return (fd);
        /* next look in built-in paths like "extra" */
    for (nl = searchpath; nl; nl = nl->nl_next)
        if ((fd = sys_trytoopenit(nl->nl_string, name, ext,
            dirresult, nameresult, size, bin, 1)) >= 0)
                return (fd);
        /* next look in built-in paths like "extra" */
    if (sys_usestdpath)
        for (nl = STUFF->st_staticpath; nl; nl = nl->nl_next)
            if ((fd = sys_trytoopenit(nl->nl_string, name, ext,
                dirresult, nameresult, size, bin, 1)) >= 0)
                    return (fd);

    *dirresult = 0;
    *nameresult = dirresult;
    return (-1);
}

    /* open via path, using the global search path. */
int open_via_path(const char *dir, const char *name, const char *ext,
    char *dirresult, char **nameresult, unsigned int size, int bin)
{
    return (do_open_via_path(dir, name, ext, dirresult, nameresult,
        size, bin, STUFF->st_searchpath, 1));
}

void open_via_helppath(const char *name, const char *dir) {}

/* --------------------- s_inter.c --------------- */

    /* get "real time" in seconds; take the
    first time we get called as a reference time of zero. */
double sys_getrealtime(void)
{
    static struct timeval then;
    struct timeval now;
    gettimeofday(&now, 0);
    if (then.tv_sec == 0 && then.tv_usec == 0) then = now;
    return ((now.tv_sec - then.tv_sec) +
        (1./1000000.) * (now.tv_usec - then.tv_usec));
}

