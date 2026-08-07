#include <X11/X.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/Xutil.h>

#include <X11/extensions/Xrender.h>
#include <X11/extensions/shape.h>

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define WIDTH 512
#define HEIGHT 384

#define OPAQUE 0xffffffff

#define NAME "xwinwrap"

#define ATOM(a) XInternAtom(display, #a, False)

Display *display = NULL;
int screen;

typedef enum {
  SHAPE_RECT = 0,
  SHAPE_CIRCLE,
  SHAPE_TRIG,
} win_shape;

struct window {
  Window root, window;
  Drawable drawable;
  Visual *visual;
  Colormap colourmap;

  unsigned int width;
  unsigned int height;
  int x;
  int y;
} window;

bool debug = false;

static pid_t pid = 0;

static bool daemon_stop = false;

static char **childArgv = 0;
static int nChildArgv = 0;

static int addArguments(char **argv, int n) {
  char **newArgv;
  int i;

  newArgv = realloc(childArgv, sizeof(char *) * (nChildArgv + n));
  if (!newArgv)
    return 0;

  for (i = 0; i < n; i++)
    newArgv[nChildArgv + i] = argv[i];

  childArgv = newArgv;
  nChildArgv += n;

  return n;
}

static void setWindowOpacity(unsigned int opacity) {
  CARD32 o;
  o = opacity;
  XChangeProperty(display, window.window, ATOM(_NET_WM_WINDOW_OPACITY), XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&o, 1);
}

static void init_x11() {
  display = XOpenDisplay(NULL);
  if (!display) {
    fprintf(stderr, NAME ": Error: couldn't open display\n");
    return;
  }
  screen = DefaultScreen(display);
}

static int get_argb_visual(Visual **visual, int *depth) {
  XVisualInfo visual_template;
  XVisualInfo *visual_list;
  int nxvisuals = 0, i;

  visual_template.screen = screen;
  visual_list = XGetVisualInfo(display, VisualScreenMask, &visual_template, &nxvisuals);
  for (i = 0; i < nxvisuals; i++) {
    if (visual_list[i].depth == 32 && (visual_list[i].red_mask == 0xff0000 && visual_list[i].green_mask == 0x00ff00 && visual_list[i].blue_mask == 0x0000ff)) {
      *visual = visual_list[i].visual;
      *depth = visual_list[i].depth;
      if (debug)
        fprintf(stderr, "Found ARGB Visual\n");
      XFree(visual_list);
      return 1;
    }
  }
  if (debug)
    fprintf(stderr, "No ARGB Visual found");
  XFree(visual_list);
  return 0;
}

static void sigHandler(int sig) {
  kill(pid, sig);
  daemon_stop = true;
}

static void usage(void) {
  fprintf(stderr, "%s \n", NAME);
  fprintf(stderr,
          "\nUsage: %s [-g {w}x{h}+{x}+{y}] [-ni] [-argb] [-fdt] [-fs] [-s] "
          "[-st] [-sp] [-a] [-d] "
          "[-b] [-nf] [-o OPACITY] [-sh SHAPE] [-ov]-- COMMAND ARG1...\n",
          NAME);
  fprintf(stderr, "Options:\n \
            -g      - Specify Geometry (w=width, h=height, x=x-coord, y=y-coord. ex: -g 640x480+100+100)\n \
            -ni     - Ignore Input\n \
            -argb   - RGB\n \
            -fdt    - force WID window a desktop type window\n \
            -fs     - Full Screen\n \
            -un     - Undecorated\n \
            -s      - Sticky\n \
            -st     - Skip Taskbar\n \
            -sp     - Skip Pager\n \
            -a      - Above\n \
            -b      - Below\n \
            -nf     - No Focus\n \
            -o      - Opacity value between 0 to 1 (ex: -o 0.20)\n \
            -sh     - Shape of window (choose between rectangle, circle or triangle. Default is rectangle)\n \
            -ov     - Set override_redirect flag (For seamless desktop background integration in non-fullscreenmode)\n \
            -d      - Daemonize\n \
            -debug  - Enable debug messages\n");
}

static void update_root_for_vroot() {
  Atom type;
  int format;
  unsigned long nitems, bytes;
  unsigned int n;
  Window root = RootWindow(display, screen);
  Window troot, parent, *children;
  unsigned char *buf = NULL;

  XQueryTree(display, root, &troot, &parent, &children, &n);
  for (unsigned int i = 0; i < n; i++) {
    if (XGetWindowProperty(display, children[i], ATOM(__SWM_VROOT),
                           0, 1, False, XA_WINDOW,
                           &type, &format, &nitems, &bytes, &buf) == Success
        && type == XA_WINDOW) {
      window.root = *(Window *)buf;
      XFree(buf);
      XFree(children);
      return;
    }
    if (buf) { XFree(buf); buf = NULL; }
  }
  XFree(children);
}

static Window find_desktop_layer(Window root) {
  Window troot, parent, *children;
  unsigned int n;
  Atom type;
  int format;
  unsigned long nitems, bytes;
  unsigned char *buf = NULL;

  XQueryTree(display, root, &troot, &parent, &children, &n);
  for (unsigned int i = 0; i < n; i++) {
    if (XGetWindowProperty(display, children[i], ATOM(_NET_WM_WINDOW_TYPE),
                           0, 1, False, XA_ATOM,
                           &type, &format, &nitems, &bytes, &buf) == Success
        && type == XA_ATOM && nitems > 0) {
      if (*(Atom *)buf == ATOM(_NET_WM_WINDOW_TYPE_DESKTOP)) {
        XFree(buf);
        Window desktop = children[i];
        XFree(children);
        return desktop;
      }
    }
    if (buf) { XFree(buf); buf = NULL; }
  }
  XFree(children);
  return 0;
}

static void forward_motion(int x_root, int y_root, unsigned int state, Time time) {
  XEvent fake = {
    .xmotion = {
      .type = MotionNotify,
      .window = window.root,
      .root = window.root,
      .subwindow = None,
      .time = time,
      .x_root = x_root,
      .y_root = y_root,
      .state = state,
      .is_hint = NotifyNormal,
      .same_screen = True,
    }
  };
  XSendEvent(display, window.root, True, PointerMotionMask, &fake);
}

int main(int argc, char **argv) {
  char widArg[256];
  char *widArgv[] = {widArg};
  char *endArg = NULL;
  int status = 0;
  unsigned int opacity = OPAQUE;

  int i;
  bool have_argb_visual = false;
  bool noInput = false;
  bool argb = false;
  bool set_desktop_type = false;
  bool fullscreen = false;
  bool noFocus = false;
  bool override = false;
  bool undecorated = false;
  bool sticky = false;
  bool below = false;
  bool above = false;
  bool skip_taskbar = false;
  bool skip_pager = false;
  bool daemonize = false;

  win_shape shape = SHAPE_RECT;

  window.width = WIDTH;
  window.height = HEIGHT;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-g") == 0) {
      if (++i < argc)
        XParseGeometry(argv[i], &window.x, &window.y, &window.width, &window.height);
    } else if (strcmp(argv[i], "-ni") == 0) {
      noInput = 1;
    } else if (strcmp(argv[i], "-argb") == 0) {
      argb = true;
    } else if (strcmp(argv[i], "-fdt") == 0) {
      set_desktop_type = true;
    } else if (strcmp(argv[i], "-fs") == 0) {
      fullscreen = 1;
    } else if (strcmp(argv[i], "-un") == 0) {
      undecorated = true;
    } else if (strcmp(argv[i], "-s") == 0) {
      sticky = true;
    } else if (strcmp(argv[i], "-st") == 0) {
      skip_taskbar = true;
    } else if (strcmp(argv[i], "-sp") == 0) {
      skip_pager = true;
    } else if (strcmp(argv[i], "-a") == 0) {
      above = true;
    } else if (strcmp(argv[i], "-b") == 0) {
      below = true;
    } else if (strcmp(argv[i], "-nf") == 0) {
      noFocus = 1;
    } else if (strcmp(argv[i], "-o") == 0) {
      if (++i < argc)
        opacity = (unsigned int)(atof(argv[i]) * OPAQUE);
    } else if (strcmp(argv[i], "-sh") == 0) {
      if (++i < argc) {
        if (strcasecmp(argv[i], "circle") == 0) {
          shape = SHAPE_CIRCLE;
        } else if (strcasecmp(argv[i], "triangle") == 0) {
          shape = SHAPE_TRIG;
        }
      }
    } else if (strcmp(argv[i], "-ov") == 0) {
      override = true;
    } else if (strcmp(argv[i], "-debug") == 0) {
      debug = true;
    } else if (strcmp(argv[i], "-d") == 0) {
      daemonize = true;
    } else if (strcmp(argv[i], "--") == 0) {
      break;
    } else {
      usage();
      return 1;
    }
  }

  if (daemonize) {
    pid_t process_id = 0;
    pid_t sid = 0;
    process_id = fork();
    if (process_id < 0) {
      fprintf(stderr, "fork failed!\n");
      exit(1);
    }

    if (process_id > 0) {
      fprintf(stderr, "pid of child process %d \n", process_id);
      exit(0);
    }
    umask(0);
    sid = setsid();
    if (sid < 0) {
      exit(1);
    }

    chdir("/");
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
  }

  for (i = i + 1; i < argc; i++) {
    if (strcmp(argv[i], "WID") == 0)
      addArguments(widArgv, 1);
    else
      addArguments(&argv[i], 1);
  }

  if (!nChildArgv) {
    fprintf(stderr, "%s: Error: couldn't create command line\n", argv[0]);
    usage();

    return 1;
  }

  addArguments(&endArg, 1);

  init_x11();
  if (!display)
    return 1;

  window.root = RootWindow(display, screen);
  update_root_for_vroot();

  if (fullscreen) {
    window.x = 0;
    window.y = 0;
    window.width = DisplayWidth(display, screen);
    window.height = DisplayHeight(display, screen);
  }
  int depth = 0, flags = CWOverrideRedirect | CWBackingStore;
  Visual *visual = NULL;

  if (argb && get_argb_visual(&visual, &depth)) {
    have_argb_visual = true;
    window.visual = visual;
    window.colourmap = XCreateColormap(display, DefaultRootWindow(display), window.visual, AllocNone);
  } else {
    window.visual = DefaultVisual(display, screen);
    window.colourmap = DefaultColormap(display, screen);
    depth = CopyFromParent;
    visual = CopyFromParent;
  }

  if (override) {
    /* An override_redirect True window.
     * No WM hints or button processing needed. */
    XSetWindowAttributes attrs = {
      ParentRelative,
      0L,
      0,
      0L,
      0,
      0,
      Always,
      0L,
      0L,
      False,
      StructureNotifyMask | ExposureMask,
      0L,
      True,
      0,
      0
    };

    if (have_argb_visual) {
      attrs.colormap = window.colourmap;
      flags |= CWBorderPixel | CWColormap;
    } else {
      flags |= CWBackPixel;
    }

    Window parent = find_desktop_layer(window.root);
    window.window = XCreateWindow(display, parent ? parent : window.root, window.x, window.y, window.width, window.height, 0, depth, InputOutput, visual, flags, &attrs);
    XLowerWindow(display, window.window);

    fprintf(stderr, NAME ": window type - override\n");
    fflush(stderr);
  } else {
    XSetWindowAttributes attrs = {
      ParentRelative,
      0L,
      0,
      0L,
      0,
      0,
      Always,
      0L,
      0L,
      False,
      StructureNotifyMask | ExposureMask | ButtonPressMask | ButtonReleaseMask,
      0L,
      False,
      0,
      0
    };

    XWMHints wmHint;
    Atom xa;

    if (have_argb_visual) {
      attrs.colormap = window.colourmap;
      flags |= CWBorderPixel | CWColormap;
    } else {
      flags |= CWBackPixel;
    }

    window.window = XCreateWindow(display, window.root, window.x, window.y, window.width, window.height, 0, depth,InputOutput, visual, flags, &attrs);

    wmHint.flags = InputHint | StateHint;
    wmHint.input = !noFocus;
    wmHint.initial_state = NormalState;

    XSetWMProperties(display, window.window, NULL, NULL, argv, argc, NULL, &wmHint, NULL);

    xa = ATOM(_NET_WM_WINDOW_TYPE);

    Atom prop;
    if (set_desktop_type) {
      prop = ATOM(_NET_WM_WINDOW_TYPE_DESKTOP);
    } else {
      prop = ATOM(_NET_WM_WINDOW_TYPE_NORMAL);
    }

    XChangeProperty(display, window.window, xa, XA_ATOM, 32, PropModeReplace, (unsigned char *)&prop, 1);

    if (undecorated) {
      xa = ATOM(_MOTIF_WM_HINTS);
      if (xa != None) {
        long prop[5] = {2, 0, 0, 0, 0};
        XChangeProperty(display, window.window, xa, xa, 32, PropModeReplace, (unsigned char *)prop, 5);
      }
    }

    /* Below other windows */
    if (below) {

      xa = ATOM(_WIN_LAYER);
      if (xa != None) {
        long prop = 0;
        XChangeProperty(display, window.window, xa, XA_CARDINAL, 32, PropModeAppend, (unsigned char *)&prop, 1);
      }

      xa = ATOM(_NET_WM_STATE);
      if (xa != None) {
        Atom xa_prop = ATOM(_NET_WM_STATE_BELOW);
        XChangeProperty(display, window.window, xa, XA_ATOM, 32, PropModeAppend, (unsigned char *)&xa_prop, 1);
      }
    }

    /* Above other windows */
    if (above) {

      xa = ATOM(_WIN_LAYER);
      if (xa != None) {
        long prop = 6;
        XChangeProperty(display, window.window, xa, XA_CARDINAL, 32, PropModeAppend, (unsigned char *)&prop, 1);
      }

      xa = ATOM(_NET_WM_STATE);
      if (xa != None) {
        Atom xa_prop = ATOM(_NET_WM_STATE_ABOVE);
        XChangeProperty(display, window.window, xa, XA_ATOM, 32, PropModeAppend, (unsigned char *)&xa_prop, 1);
      }
    }

    /* Sticky */
    if (sticky) {

      xa = ATOM(_NET_WM_DESKTOP);
      if (xa != None) {
        CARD32 xa_prop = 0xFFFFFFFF;
        XChangeProperty(display, window.window, xa, XA_CARDINAL, 32, PropModeAppend, (unsigned char *)&xa_prop, 1);
      }

      xa = ATOM(_NET_WM_STATE);
      if (xa != None) {
        Atom xa_prop = ATOM(_NET_WM_STATE_STICKY);
        XChangeProperty(display, window.window, xa, XA_ATOM, 32, PropModeAppend, (unsigned char *)&xa_prop, 1);
      }
    }

    /* Skip taskbar */
    if (skip_taskbar) {

      xa = ATOM(_NET_WM_STATE);
      if (xa != None) {
        Atom xa_prop = ATOM(_NET_WM_STATE_SKIP_TASKBAR);
        XChangeProperty(display, window.window, xa, XA_ATOM, 32, PropModeAppend, (unsigned char *)&xa_prop, 1);
      }
    }

    /* Skip pager */
    if (skip_pager) {

      xa = ATOM(_NET_WM_STATE);
      if (xa != None) {
        Atom xa_prop = ATOM(_NET_WM_STATE_SKIP_PAGER);
        XChangeProperty(display, window.window, xa, XA_ATOM, 32, PropModeAppend, (unsigned char *)&xa_prop, 1);
      }
    }
  }

  XClassHint ch = {"xwinwrap", "xwinwrap"};
  XSetClassHint(display, window.window, &ch);

  if (opacity != OPAQUE)
    setWindowOpacity(opacity);

  if (noInput) {
    Region region;

    region = XCreateRegion();
    if (region) {
      XShapeCombineRegion(display, window.window, ShapeInput, 0, 0, region, ShapeSet);
      XDestroyRegion(region);
    }
  }

  if (shape) {
    Pixmap mask = XCreatePixmap(display, window.window, window.width, window.height, 1);
    GC mask_gc = XCreateGC(display, mask, 0, NULL);

    switch (shape) {
    // Nothing special to be done if it's a rectangle
    case SHAPE_CIRCLE:
      /* fill mask */
      XSetForeground(display, mask_gc, 0);
      XFillRectangle(display, mask, mask_gc, 0, 0, window.width, window.height);

      XSetForeground(display, mask_gc, 1);
      XFillArc(display, mask, mask_gc, 0, 0, window.width, window.height, 0, 23040);
      break;

    case SHAPE_TRIG: {
      XPoint points[3] = {
        {0, window.height},
        {window.width / 2, 0},
        {window.width, window.height}
      };

      XSetForeground(display, mask_gc, 0);
      XFillRectangle(display, mask, mask_gc, 0, 0, window.width, window.height);

      XSetForeground(display, mask_gc, 1);
      XFillPolygon(display, mask, mask_gc, points, 3, Complex, CoordModeOrigin);
    }

    break;

    default:
      break;
    }
    /* combine */
    XShapeCombineMask(display, window.window, ShapeBounding, 0, 0, mask, ShapeSet);
  }

  XSelectInput(display, window.window, SubstructureNotifyMask | EnterWindowMask | LeaveWindowMask | PointerMotionMask);
  XMapWindow(display, window.window);

  XFlush(display);

  sprintf(widArg, "0x%x", (int)window.window);

  pid = fork();

  switch (pid) {
  case -1:
    perror("fork");
    return 1;
  case 0:
    execvp(childArgv[0], childArgv);
    perror(childArgv[0]);
    exit(2);
    break;
  default:
    break;
  }

  signal(SIGTERM, sigHandler);
  signal(SIGINT, sigHandler);


  int fd = ConnectionNumber(display);

  for (;;) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    XEvent ev;
    struct timeval tv = {0, 200000}; // 200ms

    if (select(fd + 1, &fds, NULL, NULL, &tv) < 0) {
      if (errno == EINTR)
        continue;
      break;
    }

    if (FD_ISSET(fd, &fds)) {
      do {
        XNextEvent(display, &ev);
        if (ev.type == MapNotify) {
          XMapEvent *map = &ev.xmap;
          if (map->window != window.window) {
            XSelectInput(display, map->window, PointerMotionMask);
          }
        } else if (ev.type == EnterNotify || ev.type == LeaveNotify) {
          XCrossingEvent *cross = &ev.xcrossing;
          forward_motion(cross->x_root, cross->y_root, cross->state, cross->time);
        } else if (ev.type == MotionNotify) {
          XMotionEvent *motion = &ev.xmotion;
          forward_motion(motion->x_root, motion->y_root, motion->state, motion->time);
        }
      } while (QLength(display));
    }

    if (waitpid(pid, &status, WNOHANG) > 0) {
      if (WIFEXITED(status))
        fprintf(stderr, "%s died, exit status %d\n", childArgv[0], WEXITSTATUS(status));
      break;
    }
  }

  XDestroyWindow(display, window.window);
  XCloseDisplay(display);

  return 0;
}
