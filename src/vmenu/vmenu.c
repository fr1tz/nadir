/* See LICENSE file for copyright and license details. */
#include <ctype.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <X11/keysym.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif

/* macros */
#define CLEANMASK(mask)         (mask & ~(numlockmask | LockMask))
#define INRECT(X,Y,RX,RY,RW,RH) ((X) >= (RX) && (X) < (RX) + (RW) && (Y) >= (RY) && (Y) < (RY) + (RH))
#define MIN(a, b)               ((a) < (b) ? (a) : (b))
#define GETENV(var, name, def)  { char* v = getenv(name); var = strdup(v ? v : def); }

/* enums */
enum { ColFG, ColBG, ColLast };

/* typedefs */
typedef struct {
	int x, y, w, h;
	unsigned long norm[ColLast];
	unsigned long sel[ColLast];
	Drawable drawable;
	GC gc;
	struct {
		XFontStruct *xfont;
		XFontSet set;
		int ascent;
		int descent;
		int height;
	} font;
} DC; /* draw context */

typedef struct Item Item;
struct Item {
	char *text;
	Item *next;		/* traverses all items */
	Item *left, *right;	/* traverses items matching current search pattern */
};

typedef struct Shortcut Shortcut;
struct Shortcut {
	int       ctrl;
	KeySym    key;
	Item*     item;
	Shortcut* next;
};

/* forward declarations */
static void appenditem(Item *i, Item **list, Item **last);
static void calcoffsets(void);
static char *cistrstr(const char *s, const char *sub);
static void cleanup(void);
static void drawmenu(void);
static void drawtext(const char *text, int selected);
static void eprint(const char *errstr, ...);
static char *find_shortcut_string(char *str, int strlen);
static unsigned long getcolor(const char *p9colstr);
static Bool grabkeyboard(void);
static void initfont(const char *fontstr);
static void kpress(XKeyEvent * e);
static void resizewindow(void);
static void match(char *pattern);
static void readstdin(void);
static void run(void);
static void selected(const char *s);
static void setup(void);
static int textnw(const char *text, unsigned int len);
static int textw(const char *text);

/* variables */
static char *maxname = NULL;
static char *prompt = NULL;
static char **tokens = NULL;
static char text[4096];
static char hitstxt[16];
static int cursorpos = 0;
static int cmdw = 0;
static int promptw = 0;
static int ret = 0;
static int screen;
static int x, y;
static unsigned int mw, mh;
static unsigned int numlockmask = 0;
static unsigned int hits = 0;
static unsigned int lines = 0;
static unsigned int xoffset = 0;
static unsigned int yoffset = 0;
static unsigned int width = 0;
static unsigned int height = 0;
static Bool running = True;
static Bool topbar = False;
static Bool hitcounter = False;
static Bool alignright = False;
static Bool resize = False;
static Bool indicators = True;
static Bool xmms = False;
static Display *dpy;
static DC dc;
static Item *allitems = NULL;	/* first of all items */
static Item *item = NULL;	/* first of pattern matching items */
static Item *sel = NULL;
static Item *next = NULL;
static Item *prev = NULL;
static Item *curr = NULL;
static Window root, win;
static int (*fstrncmp)(const char *, const char *, size_t n) = strncmp;
static char *(*fstrstr)(const char *, const char *) = strstr;
static Shortcut* shortcuts = NULL;

/* appearance */
static const char *font; 
static const char *normbgcolor;
static const char *normfgcolor;
static const char *selbgcolor;
static const char *selfgcolor;

static unsigned int maxtokens  = 16; /* max. tokens for pattern matching */

void
appenditem(Item *i, Item **list, Item **last) {
	if(!(*last))
		*list = i;
	else
		(*last)->right = i;
	i->left = *last;
	i->right = NULL;
	*last = i;
	++hits;
}

void
calcoffsets(void) {
	static unsigned int w;

	if(!curr)
		return;
	w = (dc.font.height + 2) * (lines + 1);
	for(next = curr; next; next=next->right) {
		w -= dc.font.height + 2;
		if(w <= 0)
			break;
	}
	w = (dc.font.height + 2) * (lines + 1);
	for(prev = curr; prev && prev->left; prev=prev->left) {
		w -= dc.font.height + 2;
		if(w <= 0)
			break;
	}
}

char *
cistrstr(const char *s, const char *sub) {
	int c, csub;
	unsigned int len;

	if(!sub)
		return (char *)s;
	if((c = *sub++) != 0) {
		c = tolower(c);
		len = strlen(sub);
		do {
			do {
				if((csub = *s++) == 0)
					return NULL;
			}
			while(tolower(csub) != c);
		}
		while(strncasecmp(s, sub, len) != 0);
		s--;
	}
	return (char *)s;
}

void
cleanup(void) {
	Item *itm;

	while(allitems) {
		itm = allitems->next;
		free(allitems->text);
		free(allitems);
		allitems = itm;
	}
	if(dc.font.set)
		XFreeFontSet(dpy, dc.font.set);
	else
		XFreeFont(dpy, dc.font.xfont);
	XFreePixmap(dpy, dc.drawable);
	XFreeGC(dpy, dc.gc);
	XDestroyWindow(dpy, win);
	XUngrabKeyboard(dpy, CurrentTime);
	free(tokens);
}

void
drawmenu(void) {
	static Item *i;

	dc.x = 0;
	dc.y = 0;
	dc.h = mh;
	drawtext(NULL, 0);
	/* print prompt? */
	if(promptw) {
		dc.w = promptw;
		drawtext(prompt, 1);
	}
	dc.x += promptw;
	dc.w = mw - promptw - (hitcounter ? textnw(hitstxt, strlen(hitstxt)) : 0);

	drawtext(text, 0);
	if(curr) {
		if (hitcounter) {
			dc.w = textw(hitstxt);
			dc.x = mw - textw(hitstxt);
			drawtext(hitstxt, 0);
		}
		dc.x = 0;
		dc.w = mw;
		if (indicators) {	
			dc.y += dc.font.height + 2;
			drawtext((curr && curr->left) ? "^" : NULL, 0);
		}
		dc.y += dc.font.height + 2;
		/* determine maximum items */
		for(i = curr; i != next; i=i->right) {
			drawtext(i->text, (sel == i) ? 1 : 0);
			dc.y += dc.font.height + 2;
		}
		drawtext(indicators && next ? "v" : NULL, 0);
	} else {
		if (hitcounter) {
			dc.w = textw(hitstxt);
			dc.x = mw - textw(hitstxt);
			dc.y = 0;
			drawtext(hitstxt, 0);
		}
		dc.x = 0;
		dc.w = mw;
		dc.h = mh;
		dc.y += dc.font.height + 2;
		drawtext(NULL, 0);
	} 
	XCopyArea(dpy, dc.drawable, win, dc.gc, 0, 0, mw, mh, 0, 0);
	XFlush(dpy);
}

void
updatemenu(Bool updown) {
	static Item *i;
	
	if(curr) {
		dc.x = 0;
		dc.y = (dc.font.height + 2) * (indicators?2:1);
		dc.w = mw;
		dc.h = mh;
		for(i = curr; i != next; i = i->right) {
			if(((i == sel->left) && !updown) || (i == sel)
			||((i == sel->right) && updown)) {
				drawtext(i->text, sel == i);
				XCopyArea(dpy, dc.drawable, win, dc.gc, dc.x, dc.y,
					dc.w, dc.font.height + 2, dc.x, dc.y);
			}
			dc.y += dc.font.height + 2;
		}
	}			
	XFlush(dpy);
}

void
drawtext(const char *txt, int selected) {
	char buf[256];
	int i, x, y, h, len, olen, cx;
	XRectangle r = { dc.x, dc.y, dc.w, dc.h };

	XSetForeground(dpy, dc.gc, selected ? dc.sel[ColBG] : dc.norm[ColBG]);
	XFillRectangles(dpy, dc.drawable, dc.gc, &r, 1);
	if(!txt)
		return;
	olen = strlen(txt);
	h = dc.font.height;
	y = dc.y + ((h + 2) / 2) - (h / 2) + dc.font.ascent;
	x = dc.x + (h / 2);
	/* shorten txt if necessary */
	for(len = MIN(olen, sizeof buf); len && textnw(txt, len) > dc.w - h; len--);
	/* choose colors */
	if(txt[0] == ' ' || (txt == text && txt[len-1] == ' '))
		XSetForeground(dpy, dc.gc, 0xFF888888);		
	else
		XSetForeground(dpy, dc.gc, selected ? dc.sel[ColFG] : dc.norm[ColFG]);
	/* draw cursor? */
	if(txt == text) {
		cx = textnw(text, cursorpos);
		XDrawLine(dpy, dc.drawable, dc.gc, x+cx, dc.y, x+cx, dc.y+h);
	}
	if(!len)
		return;
	memcpy(buf, txt, len);
	if(len < olen)
		for(i = len; i && i > len - 3; buf[--i] = '.');
	if(dc.font.set)
		XmbDrawString(dpy, dc.drawable, dc.font.set, dc.gc, x, y, buf, len);
	else
		XDrawString(dpy, dc.drawable, dc.gc, x, y, buf, len);
}

void
eprint(const char *errstr, ...) {
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(EXIT_FAILURE);
}

char *
find_shortcut_string(char *str, int strlen)
{
	char* p;

	if(strlen < 3)
		return NULL;
	p = str + strlen - 1;
	if(*p != '\\')
		return NULL;	
	*p = '\0';
	while(--p >= str && *p != '\\');
	if(p == str)
		return NULL;
	return p;
}

unsigned long
getcolor(const char *p9colstr) {
	Colormap cmap = DefaultColormap(dpy, screen);
	XColor color;

	char* x11color = strdup(p9colstr);
	x11color[1] = '#';
	if(!XAllocNamedColor(dpy, cmap, x11color+1, &color, &color))
		eprint("error, cannot allocate color '%s'\n", p9colstr);
	free(x11color);
	return color.pixel;
}

Bool
grabkeyboard(void) {
	unsigned int len;

	for(len = 1000; len; len--) {
		if(XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime)
		== GrabSuccess)
			break;
		usleep(1000);
	}
	return len > 0;
}

void
initfont(const char *fontstr) {
	char *def, **missing;
	int i, n;

	if(!fontstr || fontstr[0] == '\0')
		eprint("error, cannot load font: '%s'\n", fontstr);
	missing = NULL;
	dc.font.set = XCreateFontSet(dpy, fontstr, &missing, &n, &def);
	if(missing)
		XFreeStringList(missing);
	if(dc.font.set) {
		XFontSetExtents *font_extents;
		XFontStruct **xfonts;
		char **font_names;
		dc.font.ascent = dc.font.descent = 0;
		font_extents = XExtentsOfFontSet(dc.font.set);
		n = XFontsOfFontSet(dc.font.set, &xfonts, &font_names);
		for(i = 0, dc.font.ascent = 0, dc.font.descent = 0; i < n; i++) {
			if(dc.font.ascent < (*xfonts)->ascent)
				dc.font.ascent = (*xfonts)->ascent;
			if(dc.font.descent < (*xfonts)->descent)
				dc.font.descent = (*xfonts)->descent;
			xfonts++;
		}
	}
	else {
		if(!(dc.font.xfont = XLoadQueryFont(dpy, fontstr))
		&& !(dc.font.xfont = XLoadQueryFont(dpy, "fixed")))
			eprint("error, cannot load font: '%s'\n", fontstr);
		dc.font.ascent = dc.font.xfont->ascent;
		dc.font.descent = dc.font.xfont->descent;
	}
	dc.font.height = dc.font.ascent + dc.font.descent;
}

void
kpress(XKeyEvent * e) {
	char buf[32], stor[sizeof text];
	int i, num;
	unsigned int len;
	KeySym ksym;
	Shortcut* sc;

	len = strlen(text);
	buf[0] = 0;
	num = XLookupString(e, buf, sizeof buf, &ksym, NULL);
	if(IsKeypadKey(ksym)) {
		if(ksym == XK_KP_Enter)
			ksym = XK_Return;
		else if(ksym >= XK_KP_0 && ksym <= XK_KP_9)
			ksym = (ksym - XK_KP_0) + XK_0;
	}
	if(IsFunctionKey(ksym) || IsKeypadKey(ksym)
	   || IsMiscFunctionKey(ksym) || IsPFKey(ksym)
	   || IsPrivateKeypadKey(ksym))
		return;
	/* dynamic shortcuts first */
	sc = shortcuts;
	while(sc) {
		if(sc->key == ksym 
		&& sc->ctrl == ((e->state & ControlMask) > 0)) {
			selected(sc->item->text);
			return;
		}
		sc = sc->next;
	}
	/* built-in control keys */
	if(e->state & ControlMask) {
		switch (ksym) {
		default:	/* ignore other control sequences */
			return;
		case XK_bracketleft:
			ksym = XK_Escape;
			break;
		case XK_h:
		case XK_H:
			ksym = XK_BackSpace;
			break;
		case XK_j:
		case XK_J:
			ksym = XK_Return;
			break;
		case XK_a:
		case XK_A:
			cursorpos = 0;
			drawmenu();
			return;
		case XK_e:
		case XK_E:
			cursorpos = len;
			drawmenu();
			return;
		case XK_u:
		case XK_U:
			strcpy(stor, text + cursorpos);
			cursorpos = 0;
			strcpy(text + cursorpos, stor);
			match(text);
			drawmenu();
			return;
		case XK_w:
		case XK_W:
			if(len) {
				strcpy(stor, text + cursorpos);
				i = cursorpos - 1;
				while(i >= 0 && text[i] == ' ') i--;
				while(i >= 0 && text[i] != ' ') i--;
				cursorpos = i + 1;
				strcpy(text + cursorpos, stor);
				match(text);
				drawmenu();
			}
			return;
		case XK_d:
		case XK_D:
			selected(text);
			return;
		}
	}
	switch(ksym) {
	default:
		if(num && !iscntrl((int) buf[0])) {
			buf[num] = 0;
			strcpy(stor, text + cursorpos);
			strncpy(text + cursorpos, buf, sizeof(text)-cursorpos);
			if(cursorpos < sizeof(text)-1)
				cursorpos++;
			strncpy(text + cursorpos, stor, sizeof(text)-cursorpos);
			match(text);
		}
		break;
	case XK_BackSpace:
		if(len) {
			strcpy(stor, text + cursorpos);
			cursorpos--;
			strcpy(text + cursorpos, stor);
			match(text);
		}
		break;
	case XK_End:
		if(!item)
			return;
		while(next) {
			sel = curr = next;
			calcoffsets();
		}
		while(sel && sel->right)
			sel = sel->right;
		break;
	case XK_Escape:
		ret = 1;
		running = False;
		break;
	case XK_Home:
		if(!item)
			return;
		sel = curr = item;
		calcoffsets();
		break;
	case XK_Left:
		if(cursorpos == 0)
			return;
		cursorpos--;
		drawmenu();
		return;
	case XK_Right:
		if(text[cursorpos] == '\0')
			return;
		cursorpos++;
		drawmenu();
		return;
	case XK_Up:
		if(!(sel && sel->left))
			return;
		sel=sel->left;
		if(sel->right == curr) {
			curr = curr->left;
			calcoffsets();
		} else {
			updatemenu(True);
			return;
		}
		break;
	case XK_Next:
		if(!next)
			return;
		sel = curr = next;
		calcoffsets();
		break;
	case XK_Prior:
		if(!prev)
			return;
		sel = curr = prev;
		calcoffsets();
		break;
	case XK_Return:
		if(sel)
			selected(sel->text);
		else if(*text)
			selected(text);
		break;
	case XK_Down:
		if(!(sel && sel->right))
			return;
		sel=sel->right;
		if(sel == next) {
			curr = curr->right;
			calcoffsets();
		} else {
			updatemenu(False);
			return;
		}
		break;
	}
	drawmenu();
}

void
resizewindow(void)
{
	if (resize) {
		static int rlines, ry, rmh;

		rlines = (hits > lines ? lines : hits) + (indicators?3:1);
		rmh = (dc.font.height + 2) * rlines;
		ry = topbar ? y + yoffset : y - rmh + (dc.font.height + 2) - yoffset;
		XMoveResizeWindow(dpy, win, x, ry, mw, rmh);
	}
}

unsigned int
tokenize(char *pat, char **tok)
{
	unsigned int i = 0;
	char tmp[4096] = {0};

	strncpy(tmp, pat, strlen(pat));
	tok[0] = strtok(tmp, " ");

	while(tok[i] && i < maxtokens)
		tok[++i] = strtok(NULL, " ");
	return i;
}

void
match(char *pattern) {
	unsigned int plen, tokencnt = 0;
	char append = 0;
	Item *i, *itemend, *lexact, *lprefix, *lsubstr, *exactend, *prefixend, *substrend;

	if(!pattern)
		return;

	if(!xmms)
		tokens[(tokencnt = 1)-1] = pattern;
	else
		if(!(tokencnt = tokenize(pattern, tokens)))
			tokens[(tokencnt = 1)-1] = "";

	item = lexact = lprefix = lsubstr = itemend = exactend = prefixend = substrend = NULL;
	for(i = allitems; i; i = i->next) {
		for(int j = 0; j < tokencnt; ++j) {
			plen = strlen(tokens[j]);
			if(!fstrncmp(tokens[j], i->text, plen + 1))
				append = !append || append > 1 ? 1 : append;
			else if(!fstrncmp(tokens[j], i->text, plen ))
				append = !append || append > 2 ? 2 : append;
			else if(fstrstr(i->text, tokens[j]))
				append = append > 0 && append < 3 ? append : 3;
			else {
				append = 0;
				break;
			}
		}
		if(append == 1)
			appenditem(i, &lexact, &exactend);
		else if(append == 2)
			appenditem(i, &lprefix, &prefixend);
		else if(append == 3)
			appenditem(i, &lsubstr, &substrend);
	}
	if(lexact) {
		item = lexact;
		itemend = exactend;
	}
	if(lprefix) {
		if(itemend) {
			itemend->right = lprefix;
			lprefix->left = itemend;
		}
		else
			item = lprefix;
		itemend = prefixend;
	}
	if(lsubstr) {
		if(itemend) {
			itemend->right = lsubstr;
			lsubstr->left = itemend;
		}
		else
			item = lsubstr;
	}
	curr = prev = next = sel = item;
	calcoffsets();
	resizewindow();
	snprintf(hitstxt, sizeof(hitstxt), "(%d)", hits);
	hits = 0;
}

void
readstdin(void) {
	char *p, buf[1024];
	unsigned int len = 0, max = 0;
	Item *i, *new;
	Shortcut *sc;
	KeySym ksym;

	i = 0;
	while(fgets(buf, sizeof buf, stdin)) {
		len = strlen(buf);
		if(buf[len - 1] == '\n')
		{
			buf[len - 1] = 0;
			len--;
		}
		if(!(p = strdup(buf)))
			eprint("fatal: could not strdup() %u bytes\n", strlen(buf));
		if(max < len) {
			maxname = p;
			max = len;
		}
		if(!(new = (Item *)malloc(sizeof(Item))))
			eprint("fatal: could not malloc() %u bytes\n", sizeof(Item));
		new->next = new->left = new->right = NULL;
		new->text = p;
		if(!i)
			allitems = new;
		else 
			i->next = new;
		i = new;
		/* shortcut? */
		p = find_shortcut_string(buf, len);
		if(!p)
			continue;
		p++;
		ksym = XStringToKeysym(*p == '^' ? p+1 : p);
		if(ksym == NoSymbol) {
			fprintf(stderr, "Invalid shortcut string: %s\n", p);
			continue;
		}
		if(!(sc = malloc(sizeof(Shortcut))))
			eprint("fatal: could not malloc() %u bytes\n", sizeof(Shortcut));
		sc->ctrl = (*p == '^');
		sc->key = ksym;
		sc->item = new;
		sc->next = NULL;
		if(shortcuts) 
			sc->next = shortcuts;
		shortcuts = sc;	
	}
}

void
run(void) {
	XEvent ev;

	/* main event loop */
	while(running && !XNextEvent(dpy, &ev))
		switch (ev.type) {
		default:	/* ignore all crap */
			break;
		case KeyPress:
			kpress(&ev.xkey);
			break;
		case Expose:
			if(ev.xexpose.count == 0)
				drawmenu();
			break;
		}
}

void
selected(const char *s) {
	char *buf, *cmd, *arg, *p;
	char stor[sizeof text];
	int i, len;

	if(s[0] == '#') {
		buf = strdup(s);
		/* ignore shortcut string */
		if((p = find_shortcut_string(buf, strlen(buf))))
			*p = '\0';
		len = strlen(buf);
		cmd = buf;
		arg = NULL; 
		/* find possible arguments */
		for(i = 1; i < len; i++) {
			if(buf[i] == ' ') {
				buf[i] = '\0';
				p = &buf[i];
				while(*++p != '\0')
					if(!isspace(*p)) {
						arg = p;
						break;		
					}
				break;				
			}
		}
		/* process command */
		if(strcmp(cmd, "#insert") == 0) {
			if(!arg && !sel) return;
			if(!arg) arg = sel->text;
			strcpy(stor, text + cursorpos);
			strncpy(text + cursorpos, arg, sizeof(text)-cursorpos);
			cursorpos += strlen(arg);
			if(cursorpos > sizeof(text)-1)
				cursorpos = sizeof(text)-1;
			strncpy(text + cursorpos, stor, sizeof(text)-cursorpos);
			match(text);
			drawmenu();
		}
		else if(strcmp(cmd, "#replace") == 0) {
			if(!arg && !sel) return;
			if(!arg) arg = sel->text;
			strncpy(text, arg, sizeof(text));
			cursorpos = strlen(text);
			match(text);
			drawmenu();
		}
	}
	else {
		fprintf(stdout, "%s", s);
		fflush(stdout);
		running = False;
	}
}

void
setup(void) {
	int i, j, sy, slines;
#if XINERAMA
	int n;
	XineramaScreenInfo *info = NULL;
#endif
	XModifierKeymap *modmap;
	XSetWindowAttributes wa;

	/* init modifier map */
	modmap = XGetModifierMapping(dpy);
	for(i = 0; i < 8; i++)
		for(j = 0; j < modmap->max_keypermod; j++) {
			if(modmap->modifiermap[i * modmap->max_keypermod + j]
			== XKeysymToKeycode(dpy, XK_Num_Lock))
				numlockmask = (1 << i);
		}
	XFreeModifiermap(modmap);

	/* style */
	dc.norm[ColBG] = getcolor(normbgcolor);
	dc.norm[ColFG] = getcolor(normfgcolor);
	dc.sel[ColBG] = getcolor(selbgcolor);
	dc.sel[ColFG] = getcolor(selfgcolor);
	initfont(font);

	/* menu window */
	wa.override_redirect = True;
	wa.background_pixmap = ParentRelative;
	wa.event_mask = ExposureMask | ButtonPressMask | KeyPressMask;

	/* menu window geometry */
	mh = dc.font.height + 2;
#if XINERAMA
	if(XineramaIsActive(dpy) && (info = XineramaQueryScreens(dpy, &n))) {
		i = 0;
		if(n > 1) {
			int di;
			unsigned int dui;
			Window dummy;
			if(XQueryPointer(dpy, root, &dummy, &dummy, &x, &y, &di, &di, &dui))
				for(i = 0; i < n; i++)
					if(INRECT(x, y, info[i].x_org, info[i].y_org, info[i].width, info[i].height))
						break;
		}
		x = info[i].x_org;
		y = topbar ? info[i].y_org : info[i].y_org + info[i].height - mh;
		mw = info[i].width;
		XFree(info);
	}
	else
#endif
	{
		x = 0;
		y = topbar ? 0 : DisplayHeight(dpy, screen) - mh;
		mw = DisplayWidth(dpy, screen);
	}

	/* update menu window geometry */
	if(lines == 0 && height == 0)
		height = DisplayHeight(dpy, screen) / 2;
	slines = (lines ? lines : (lines = height / (dc.font.height + 2))) + (indicators?3:1);
	mh = (dc.font.height + 2) * slines;
	sy = topbar ? y + yoffset : y - mh + (dc.font.height + 2) - yoffset;
	x = alignright ? mw - (width ? width : mw) - xoffset : xoffset;
	mw = width ? width : mw;

	win = XCreateWindow(dpy, root, x, sy, mw, mh, 0,
			DefaultDepth(dpy, screen), CopyFromParent,
			DefaultVisual(dpy, screen),
			CWOverrideRedirect | CWBackPixmap | CWEventMask, &wa);

	/* pixmap */
	dc.drawable = XCreatePixmap(dpy, root, mw, mh, DefaultDepth(dpy, screen));
	dc.gc = XCreateGC(dpy, root, 0, NULL);
	XSetLineAttributes(dpy, dc.gc, 1, LineSolid, CapButt, JoinMiter);
	if(!dc.font.set)
		XSetFont(dpy, dc.gc, dc.font.xfont->fid);
	if(maxname)
		cmdw = textw(maxname);
	if(cmdw > mw / 3)
		cmdw = mw / 3;
	if(prompt)
		promptw = textw(prompt);
	if(promptw > mw / 5)
		promptw = mw / 5;
	text[0] = 0;
	tokens = malloc((xmms?maxtokens:1)*sizeof(char*));
	match(text);
	XMapRaised(dpy, win);

	/* set WM_CLASS */
	XClassHint *ch = XAllocClassHint();
	ch->res_name = "vmenu";
	ch->res_class = "vmenu";
	XSetClassHint(dpy, win, ch);
	XFree(ch);
}

int
textnw(const char *text, unsigned int len) {
	XRectangle r;

	if(dc.font.set) {
		XmbTextExtents(dc.font.set, text, len, NULL, &r);
		return r.width;
	}
	return XTextWidth(dc.font.xfont, text, len);
}

int
textw(const char *text) {
	return textnw(text, strlen(text)) + dc.font.height;
}

int
main(int argc, char *argv[]) {
	unsigned int i;

	GETENV(font,        "°°fixfontx",    "-*-terminus-medium-r-normal-*-14-*-*-*-*-*-*-*");
	GETENV(normbgcolor, "°°bgcolor",     "0xFFFFFF");
	GETENV(normfgcolor, "°°fgcolor",     "0x000000");
	GETENV(selbgcolor,  "°°execbgcolor", "0xAA0000");
	GETENV(selfgcolor,  "°°execfgcolor", "0xFFFFFF");

	/* command line args */
	for(i = 1; i < argc; i++)
	{
		if(!strcmp(argv[i], "-i")) {
			fstrncmp = strncasecmp;
			fstrstr = cistrstr;
		}
		else if(!strcmp(argv[i], "-c"))
			hitcounter = True;		
		else if(!strcmp(argv[i], "-z"))
			xmms = True;
		else if(!strcmp(argv[i], "-r"))
			resize = True;
		else if(!strcmp(argv[i], "-w")) {
			if(++i < argc) width = atoi(argv[i]);
		}
		else if(!strcmp(argv[i], "-h")) {
			if(++i < argc) height = atoi(argv[i]);
		}
		else if(!strcmp(argv[i], "-x")) {
			if(++i < argc) {
				if(argv[i][0] == '-') {
					xoffset = -atoi(argv[i]);
					alignright = True;
				} else
					xoffset = atoi(argv[i]);
			}
		}
		else if(!strcmp(argv[i], "-y")) {
			if(++i < argc) {
				if(argv[i][0] == '-') {
					yoffset = -atoi(argv[i]);
					topbar = False;
				} else
					yoffset = atoi(argv[i]);
			}
		}
		else if(!strcmp(argv[i], "-p")) {
			if(++i < argc) prompt = argv[i];
		}
		else
			eprint("usage: vmenu [-iczr] [-x xpos] [-y ypos] [-w width] [-h height] [-p prompt]\n");

	}
	if(!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		fprintf(stderr, "warning: no locale support\n");
	if(!(dpy = XOpenDisplay(NULL)))
		eprint("vmenu: cannot open display\n");
	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);

	if(isatty(STDIN_FILENO)) {
		readstdin();
		running = grabkeyboard();
	}
	else { /* prevent keypress loss */
		running = grabkeyboard();
		readstdin();
	}
	
	setup();
	drawmenu();
	XSync(dpy, False);
	run();
	cleanup();
	XCloseDisplay(dpy);
	return ret;
}
