/* slimlock
 * Copyright (c) 2010-2012 Joel Burget <joelburget@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/vt.h>
#include <X11/keysym.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/dpms.h>
#include <security/pam_appl.h>
#include <err.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <errno.h>
#include <sys/file.h>
#include <fcntl.h>

#include "cfg.h"
#include "util.h"
#include "panel.h"

#undef APPNAME
#define APPNAME "slimlock"
#define SLIMLOCKCFG SYSCONFDIR"/slimlock.conf"

using namespace std;

void setBackground(const string& themedir);
void InitWindow();
void CloseWindow();
void HideCursor();
bool AuthenticateUser();
static int ConvCallback(int num_msgs, const struct pam_message **msg,
						struct pam_response **resp, void *appdata_ptr);
string findValidRandomTheme(const string& set);
void HandleSignal(int sig);

// I really didn't wanna put these globals here, but it's the only way...
Display* dpy;
int scr;
Window win;
Window root;
Cfg* cfg;
Panel* loginPanel;
string themeName = "";
string themedir = "";
const char *display = getenv("DISPLAY");

pam_handle_t *pam_handle;
struct pam_conv conv = {ConvCallback, NULL};

CARD16 dpms_standby, dpms_suspend, dpms_off, dpms_level;
BOOL dpms_state, using_dpms;
int term;

static void
die(const char *errstr, ...) {
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(EXIT_FAILURE);
}

int main(int argc, char **argv) {
        int error;
  
	if((argc == 2) && !strcmp("-v", argv[1]))
		die(APPNAME"-"VERSION", © 2010-2012 Joel Burget\n");
	else if(argc != 1)
		die("usage: "APPNAME" [-v]\n");

	void (*prev_fn)(int);

	// restore DPMS settings should slimlock be killed in the line of duty
	prev_fn = signal(SIGTERM, HandleSignal);
	if (prev_fn == SIG_IGN) signal(SIGTERM, SIG_IGN);

	// create a lock file to solve mutliple instances problem
	// /var/lock used to be the place to put this, now it's /run/lock
	// ...i think
	struct stat statbuf;
	int lock_file;

	// try /run/lock first, since i believe it's preferred
	if (!stat("/run/lock", &statbuf))
		lock_file = open("/run/lock/"APPNAME".lock", O_CREAT | O_RDWR, 0666);
	else
		lock_file = open("/var/lock/"APPNAME".lock", O_CREAT | O_RDWR, 0666);

	int rc = flock(lock_file, LOCK_EX | LOCK_NB);

	if(rc) {
		if(EWOULDBLOCK == errno)
			die(APPNAME" already running\n");
	}

	unsigned int cfg_passwd_timeout;
	// Read user's current theme
	cfg = new Cfg;
	cfg->readConf(CFGFILE);
	cfg->readConf(SLIMLOCKCFG);
	string themebase = "";
	string themefile = "";
	themeName = "";
	themebase = string(THEMESDIR) + "/";
	themeName = cfg->getOption("current_theme");
	string::size_type pos;
	if ((pos = themeName.find(",")) != string::npos) {
		themeName = findValidRandomTheme(themeName);
	}

	bool loaded = false;
	while (!loaded) {
		themedir =  themebase + themeName;
		themefile = themedir + THEMESFILE;
		if (!cfg->readConf(themefile)) {
			if (themeName == "default") {
				cerr << APPNAME << ": Failed to open default theme file "
					 << themefile << endl;
				exit(ERR_EXIT);
			} else {
				cerr << APPNAME << ": Invalid theme in config: "
					 << themeName << endl;
				themeName = "default";
			}
		} else {
			loaded = true;
		}
	}

	display = getenv("DISPLAY");
	if (!display)
		display = DISPLAY;

  	if(!(dpy = XOpenDisplay(display)))
		die(APPNAME": cannot open display\n");

	root = RootWindow(dpy, scr);
	InitWindow();
	
	// disable tty switching
	if(cfg->getOption("tty_lock") == "1") {
		if ((term = open("/dev/console", O_RDWR)) == -1)
			perror("error opening console");

		if ((ioctl(term, VT_LOCKSWITCH)) == -1)
			perror("error locking console");
	}

	// Get password timeout
	cfg_passwd_timeout = Cfg::string2int(cfg->getOption("wrong_passwd_timeout").c_str());
	// Let's just make sure it has a sane value
	cfg_passwd_timeout = cfg_passwd_timeout > 60 ? 60 : cfg_passwd_timeout;

	// Main loop
	while (true)
	{
	        if (loginPanel->MonitorsChanged()) {
   		        loginPanel->ClosePanel();
			delete loginPanel;
			CloseWindow();
			InitWindow();
		}

		loginPanel->ResetPasswd();

		// AuthenticateUser returns true if authenticated
		if (AuthenticateUser())
			break;

		if (!loginPanel->MonitorsChanged())
		        loginPanel->WrongPassword(cfg_passwd_timeout);
	}

	loginPanel->ClosePanel();
	delete loginPanel;

	
	CloseWindow();
	// Get DPMS stuff back to normal
	if (using_dpms) {
	  DPMSSetTimeouts(dpy, dpms_standby, dpms_suspend, dpms_off);
	  // turn off DPMS if it was off when we entered
		if (!dpms_state)
		  DPMSDisable(dpy);
	}

	XUnmapWindow(dpy, root);
	XDestroyWindow(dpy, root);
	XCloseDisplay(dpy);
	close(lock_file);

	if(cfg->getOption("tty_lock") == "1") {
		if ((ioctl(term, VT_UNLOCKSWITCH)) == -1) {
			perror("error unlocking console");
		}
	}
	close(term);

	delete cfg;
	return 0;
}

void InitWindow()
{
  	scr = DefaultScreen(dpy);

	XSetWindowAttributes wa;
	wa.override_redirect = 1;
	wa.background_pixel = BlackPixel(dpy, scr);

	XFlush(dpy);
	// Create a full screen window
	win = XCreateWindow(dpy,
	  root,
	  0,
	  0,
	  DisplayWidth(dpy, scr),
	  DisplayHeight(dpy, scr),
	  0,
	  DefaultDepth(dpy, scr),
	  CopyFromParent,
	  DefaultVisual(dpy, scr),
	  CWOverrideRedirect | CWBackPixel,
	  &wa);
	XMapWindow(dpy, win);

	for (int len = 1000; len; len--) {
		if(XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime)
			== GrabSuccess)
			break;
		usleep(1000);
	}
	XSelectInput(dpy, win, ExposureMask | KeyPressMask);

	// This hides the cursor if the user has that option enabled in their
	// configuration
	HideCursor();

	loginPanel = new Panel(dpy, scr, win, cfg, themedir, Panel::Mode_Lock);
	int ret = pam_start(APPNAME, loginPanel->GetName().c_str(), &conv, &pam_handle);
	// If we can't start PAM, just exit because slimlock won't work right
	if (ret != PAM_SUCCESS)
		die("PAM: %s\n", pam_strerror(pam_handle, ret));

	// Set up DPMS
	unsigned int cfg_dpms_standby, cfg_dpms_off;
	cfg_dpms_standby = Cfg::string2int(cfg->getOption("dpms_standby_timeout").c_str());
	cfg_dpms_off = Cfg::string2int(cfg->getOption("dpms_off_timeout").c_str());
	using_dpms = DPMSCapable(dpy) && (cfg_dpms_standby > 0);
	if (using_dpms) {
		DPMSGetTimeouts(dpy, &dpms_standby, &dpms_suspend, &dpms_off);

		DPMSSetTimeouts(dpy, cfg_dpms_standby,
						cfg_dpms_standby, cfg_dpms_off);

		DPMSInfo(dpy, &dpms_level, &dpms_state);
		if (!dpms_state)
			DPMSEnable(dpy);
	}

	return;
}

void CloseWindow()
{        
        pam_end(pam_handle, 0);
	XFlush(dpy);	
}

void HideCursor()
{
	if (cfg->getOption("hidecursor") == "true") {
		XColor black;
		char cursordata[1];
		Pixmap cursorpixmap;
		Cursor cursor;
		cursordata[0] = 0;
		cursorpixmap = XCreateBitmapFromData(dpy, win, cursordata, 1, 1);
		black.red = 0;
		black.green = 0;
		black.blue = 0;
		cursor = XCreatePixmapCursor(dpy, cursorpixmap, cursorpixmap,
									 &black, &black, 0, 0);
		XFreePixmap(dpy, cursorpixmap);
		XDefineCursor(dpy, win, cursor);
	}
}

static int ConvCallback(int num_msgs, const struct pam_message **msg,
						struct pam_response **resp, void *appdata_ptr)
{
	loginPanel->EventHandler(Panel::Get_Passwd);
	if (loginPanel->MonitorsChanged()) {
	        return PAM_ABORT;
	}

	// PAM expects an array of responses, one for each message
	if (num_msgs == 0 ||
		(*resp = (pam_response*) calloc(num_msgs, sizeof(struct pam_message))) == NULL)
		return PAM_BUF_ERR;

	for (int i = 0; i < num_msgs; i++) {
		if (msg[i]->msg_style != PAM_PROMPT_ECHO_OFF &&
			msg[i]->msg_style != PAM_PROMPT_ECHO_ON)
			continue;

		// return code is currently not used but should be set to zero
		resp[i]->resp_retcode = 0;
		if ((resp[i]->resp = strdup(loginPanel->GetPasswd().c_str())) == NULL) {
			free(*resp);
			return PAM_BUF_ERR;
		}
	}

	return PAM_SUCCESS;
}

bool AuthenticateUser()
{
	return(pam_authenticate(pam_handle, 0) == PAM_SUCCESS);
}

string findValidRandomTheme(const string& set)
{
	// extract random theme from theme set; return empty string on error
	string name = set;
	struct stat buf;

	if (name[name.length() - 1] == ',') {
		name.erase(name.length() - 1);
	}

	Util::srandom(Util::makeseed());

	vector<string> themes;
	string themefile;
	Cfg::split(themes, name, ',');
	do {
		int sel = Util::random() % themes.size();

		name = Cfg::Trim(themes[sel]);
		themefile = string(THEMESDIR) +"/" + name + THEMESFILE;
		if (stat(themefile.c_str(), &buf) != 0) {
			themes.erase(find(themes.begin(), themes.end(), name));
			cerr << APPNAME << ": Invalid theme in config: "
				 << name << endl;
			name = "";
		}
	} while (name == "" && themes.size());
	return name;
}

void HandleSignal(int sig)
{
	// Get DPMS stuff back to normal
	if (using_dpms) {
		DPMSSetTimeouts(dpy, dpms_standby, dpms_suspend, dpms_off);
		// turn off DPMS if it was off when we entered
		if (!dpms_state)
			DPMSDisable(dpy);
	}

	if ((ioctl(term, VT_UNLOCKSWITCH)) == -1) {
		perror("error unlocking console");
	}
	close(term);

	loginPanel->ClosePanel();
	delete loginPanel;

	die(APPNAME": Caught signal; dying\n");
}
