/* xomxplayer
 * An x window control for omxplayer
 * Dr. R. Padgett <rod_padgett@hotmail.com>
 * April 2017
 * Window is moved / resized, but we only require the final location / size.
 * This listens for xevents on a file descriptor, with a timeout of 500ms (see http://www.linuxquestions.org/questions/showthread.php?p=2431345#post2431345).
 * It is assumed that if no events are received in 500ms, that is the end of the move / resize.
 * If it is the first timeout since the events were received (evc>0) move omxplayer overlay to the current xwindow position
 *
 * gcc -Wall xomxplayer.c -o xomxplayer -lX11
 */

#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <X11/keysym.h>

#define LENGTH(X) (sizeof X / sizeof X[0])
#define DEBUG

typedef struct {
   KeySym keysym;
   const char **v;
} Key;

static Display *dis;
static Window win;
/* Command variables for config */
static char pid[16];
static char dbusParam[128];    /* Store for dbus name parameter */
static char destParam[256];    /* dest parameter for dbus constol */
static char winParam[32];      /* Requested OMX window size */
static char resizeParam[64];   /* Resize parameter for dbus control */
static char videoFile[4096];   /* File to play with path */
Atom wmDeleteMessage;

/* Config */
static char className[] = "xomxplayer";
static const char omxplayerFont[]="/usr/share/fonts/TTF/Vera.ttf";
static const char omxplayerItFont[]="/usr/share/fonts/TTF/VeraIt.ttf";

/* Commands to run omxplayer, quit omxplayer and resize / reposition the window. These must be present */
static const char *omxplayer[]={ "omxplayer.bin", "--font", omxplayerFont, "--italic-font", omxplayerItFont, "--no-keys", "--dbus_name", dbusParam, "--layer", pid, "--win", winParam, "--aspect-mode", "Letterbox", videoFile, NULL };
static const char *quit_player[]={ "dbus-send", "--type=method_call", "--session", destParam, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Quit", NULL };
static const char *resize_player[]={ "dbus-send", "--type=method_call", "--session", destParam, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player.VideoPos", "objpath:/not/used", resizeParam, NULL };
static const char *hide_video[]={ "dbus-send", "--type=method_call", "--session", destParam, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player.Action", "int32:28", NULL };
static const char *unhide_video[]={ "dbus-send", "--type=method_call", "--session", destParam, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player.Action", "int32:29", NULL };

/* Other dbus commands 
 * See https://github.com/popcornmix/omxplayer/blob/master/README.md
 */
static const char *pause_player[]={ "dbus-send", "--type=method_call", "--session", destParam, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player.Action", "int32:16", NULL };
static const char *stop_player[]={ "dbus-send", "--type=method_call", "--session", destParam, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player.Action", "int32:15", NULL };
static const char *seek_back_small[]={ "dbus-send", "--type=method_call", "--session", destParam, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player.Action", "int32:19", NULL };
static const char *seek_forward_small[]={ "dbus-send", "--type=method_call", "--session", destParam, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player.Action", "int32:20", NULL };
static const char *seek_back_large[]={ "dbus-send", "--type=method_call", "--session", destParam, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player.Action", "int32:21", NULL };
static const char *seek_forward_large[]={ "dbus-send", "--type=method_call", "--session", destParam, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player.Action", "int32:22", NULL };
static const char *toggle_subtitle[]={ "dbus-send", "--type=method_call", "--session", destParam, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player.Action", "int32:12", NULL };

/* See /usr/include/X11/keysymdef.h for keycodes */
static KeySym quitKey=XK_q;
static KeySym fullScreenKey=XK_f;
static Key keys[]= {
   { XK_p,         pause_player },
   { XK_s,         stop_player },
   { XK_Left,      seek_back_small },
   { XK_Right,     seek_forward_small },
   { XK_Page_Up,   seek_forward_large },
   { XK_Page_Down, seek_back_large },
   { XK_v,         toggle_subtitle },
};

/* Adapted from dwm spawn() (http://suckless.org/) */
static pid_t spawn(const char **arg) {
   pid_t chld_pid;
#ifdef DEBUG
   int i=0;
   fprintf(stderr, "exec ");
   while (arg[i]!=NULL) {
      fprintf(stderr, "%s ",arg[i]);
      i++;
   }
   fprintf(stderr, "\n");
#endif
   chld_pid=fork();
   if (chld_pid==0) {
      if (dis)
         close(ConnectionNumber(dis));
      setsid();
      execvp(((char **)arg)[0], (char **)arg);
      fprintf(stderr, "xomxplayer: execvp %s", ((char **)arg)[0]);
      perror(" failed");
      exit(EXIT_SUCCESS);
   }
   return chld_pid;
}

static void xhints(void) {
   XClassHint class = {className, className};
   XWMHints wm = {.flags = InputHint, .input = 1};
   XSizeHints *sizeh = NULL;

   sizeh = XAllocSizeHints();
   sizeh->flags = PSize | PMinSize | PMaxSize ;
   sizeh->height = 576;
   sizeh->width = 1024;
   sizeh->min_width=320;
   sizeh->min_height=240;
   sizeh->max_width=1920;
   sizeh->max_height=1080;

   XSetWMProperties(dis, win, NULL, NULL, NULL, 0, sizeh, &wm, &class);
   XFree(sizeh);
}

static void toggleFS() {
   XEvent fsToggle;

   memset(&fsToggle, 0, sizeof(fsToggle));
   fsToggle.type = ClientMessage;
   fsToggle.xclient.window = win;
   fsToggle.xclient.message_type = XInternAtom(dis, "_NET_WM_STATE", False);
   fsToggle.xclient.format = 32;
   fsToggle.xclient.data.l[0] = 2; /* Toggle full screen mode */
   fsToggle.xclient.data.l[1] = XInternAtom(dis, "_NET_WM_STATE_FULLSCREEN", False);
   fsToggle.xclient.data.l[2] = 0;
   XSendEvent (dis, DefaultRootWindow(dis), False,
   SubstructureRedirectMask | SubstructureNotifyMask, &fsToggle);
}

/* Adapted from dwm keypress() (http://suckless.org/)
 * Returns updated omxplayerRunning
 */
static int keypress(XEvent *e) {
   unsigned int i;
   KeySym keysym;
   XKeyEvent *ev;

   ev = &e->xkey;
   keysym = XKeycodeToKeysym(dis, (KeyCode)ev->keycode, 0);
   if (keysym==quitKey) {
      spawn(quit_player);
      return 0; /* Quit player */
   }
   if (keysym==fullScreenKey) {
      toggleFS();
      return 1; /* Don't quit player */
   }
   for (i = 0; i < LENGTH(keys); i++) {
      if (keysym==keys[i].keysym) {
         spawn(keys[i].v);
         break;
      }
   }
   return 1;   /* Don't quit player */
}

static int init(char *file) {
   strncpy(videoFile, file, 4095);
   snprintf(pid,15,"%i",getpid());
   strncpy(dbusParam, "org.mpris.MediaPlayer2.omxplayer", 96);
   strncat(dbusParam, pid, 8);
   strncpy(destParam, "--dest=", 8);
   strncat(destParam, dbusParam, 128);

   dis=XOpenDisplay(NULL);
   win=XCreateSimpleWindow(dis, DefaultRootWindow(dis), 1, 1, 1024, 576, 0, BlackPixel (dis, 0), BlackPixel(dis, 0));
   XSetStandardProperties(dis, win, videoFile, videoFile, None, NULL, 0, NULL);
   XSelectInput(dis, win, KeyPressMask | StructureNotifyMask | VisibilityChangeMask);
   xhints();
   wmDeleteMessage = XInternAtom(dis, "WM_DELETE_WINDOW", False);
   XSetWMProtocols(dis, win, &wmDeleteMessage, 1); 
   XMapWindow(dis, win);
   XFlush(dis);

   return ConnectionNumber(dis);
}

int main(int argc, char *argv[]) {
   int x11_fd;
   fd_set in_fds;
   struct timeval tv;
   XEvent ev;
   int wx,wy;
   unsigned int ww, wh;
   long unsigned int evc=0;
   pid_t omxplayer_pid;
   pid_t chld_pid;
   int chld_status;
   int omxplayerRunning=2;

   if (argc!=2) {
      printf("Usage: %s <video file>\n", argv[0]);
      return 1;
   }

   x11_fd=init(argv[1]);

   while(omxplayerRunning > 0) {
      FD_ZERO(&in_fds);
      FD_SET(x11_fd, &in_fds);

      tv.tv_usec = 500000;
      tv.tv_sec = 0;

      
      switch (select(x11_fd+1, &in_fds, 0, 0, &tv)) {
      case 0: /* Timed out waiting for xevents */
         if (evc>0) {
            snprintf(winParam,30,"%i %i %i %i", wx, wy, wx+ww, wy+wh);
            if (omxplayerRunning==2) { /* omxplayer has not been started yet */
               omxplayer_pid=spawn(omxplayer);
               if (omxplayer_pid < 1)
                  omxplayerRunning=0; /* Failed */
               else
                  omxplayerRunning=1;
            }
            else {   /* omxplayer is running */
               strncpy(resizeParam, "string:",10);
               strncat(resizeParam, winParam, 30);
               spawn(resize_player);
            }
         }
         evc=0;   /* Reset resize event counter */
         chld_pid=waitpid(-1, &chld_status, WNOHANG);
         if (chld_pid>0) {
            if (WIFEXITED(chld_status)) {
            #ifdef DEBUG
               fprintf(stderr, "Child with pid %i finished with exit code %i.\n",chld_pid, WEXITSTATUS(chld_status));
            #endif
               if (omxplayerRunning==1 && chld_pid==omxplayer_pid)
                  omxplayerRunning=0;  /* omxplayer finished */
            }
         }
      break;
      case -1: /* Error occured or signal received */
         if (omxplayerRunning==1) {
            spawn(quit_player);
            omxplayerRunning=0;
         }
      break;
      }

      /* Handle XEvents and flush the input */
      while(XPending(dis)) {
         XNextEvent(dis, &ev);
         switch (ev.type) {
         case KeyPress:
            omxplayerRunning=keypress(&ev);
         break;
         case ClientMessage:
            if (ev.xclient.data.l[0] == wmDeleteMessage) {
               spawn(quit_player);
               omxplayerRunning=0;
            }
         break;
         case ConfigureNotify:
            if (ev.xconfigure.x>=0 && ev.xconfigure.y>=0) {
               ww=ev.xconfigure.width;
               wh=ev.xconfigure.height;
               wx=ev.xconfigure.x;
               wy=ev.xconfigure.y;
               evc++;
            }
         break;
         case VisibilityNotify:
            if (ev.xvisibility.state==VisibilityFullyObscured)
               spawn(hide_video);
            else if (ev.xvisibility.state==VisibilityUnobscured)
               spawn(unhide_video);
         break;
         }
      }
   }

   XDestroyWindow(dis,win);
   XCloseDisplay(dis);
   return 0;
}
