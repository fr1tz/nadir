#define Point OSXPoint
#define Rect OSXRect
#define Cursor OSXCursor
#include <Carbon/Carbon.h>
#undef Rect
#undef Point
#undef Cursor
#undef offsetof
#undef nil

#include "u.h"
#include "libc.h"
#include <thread.h>
#include <draw.h>
#include <memdraw.h>
#include <keyboard.h>
#include "mouse.h"
#include <cursor.h>
#include "osx-screen.h"
#include "osx-keycodes.h"
#include "devdraw.h"
#include "glendapng.h"

AUTOFRAMEWORK(Carbon)

#define panic sysfatal

extern Rectangle mouserect;

struct {
	char *label;
	char *winsize;
	QLock labellock;

	Rectangle fullscreenr;
	Rectangle screenr;
	Memimage *screenimage;
	int isfullscreen;
	ulong fullscreentime;
	
	Point xy;
	int buttons;
	int kbuttons;

	CGDataProviderRef provider;
	MenuRef wmenu;
	MenuRef vmenu;
	WindowRef window;
	CGImageRef image;
	CGContextRef windowctx;
	PasteboardRef snarf;
	int needflush;
	QLock flushlock;
	int active;
	int infullscreen;
	int kalting;		// last keystroke was Kalt
} osx;

enum
{
	WindowAttrs =
		kWindowCloseBoxAttribute |
		kWindowCollapseBoxAttribute |
		kWindowResizableAttribute |
		kWindowStandardHandlerAttribute |
		kWindowFullZoomAttribute
};

enum
{
	P9PEventLabelUpdate = 1
};

static void screenproc(void*);
static void eresized(int);
static void fullscreen(int);
static void seticon(void);
static void activated(int);

static OSStatus quithandler(EventHandlerCallRef, EventRef, void*);
static OSStatus eventhandler(EventHandlerCallRef, EventRef, void*);
static OSStatus cmdhandler(EventHandlerCallRef, EventRef, void*);

enum
{
	CmdFullScreen = 1,
};

void screeninit(void);
void _flushmemscreen(Rectangle r);

Memimage*
attachscreen(char *label, char *winsize)
{
	if(label == nil)
		label = "gnot a label";
	osx.label = strdup(label);
	osx.winsize = winsize;
	if(osx.screenimage == nil){
		screeninit();
		if(osx.screenimage == nil)
			panic("cannot create OS X screen");
	}
	return osx.screenimage;
}

void
_screeninit(void)
{
	CGRect cgr;
	OSXRect or;
	Rectangle r;
	int havemin;

	memimageinit();

	ProcessSerialNumber psn = { 0, kCurrentProcess };
	TransformProcessType(&psn, kProcessTransformToForegroundApplication);
	SetFrontProcess(&psn);

	cgr = CGDisplayBounds(CGMainDisplayID());
	osx.fullscreenr = Rect(0, 0, cgr.size.width, cgr.size.height);
	
	InitCursor();
	
	// Create minimal menu with full-screen option.
	ClearMenuBar();
	CreateStandardWindowMenu(0, &osx.wmenu);
	InsertMenu(osx.wmenu, 0);
	MenuItemIndex ix;
	CreateNewMenu(1004, 0, &osx.vmenu);	// XXX 1004?
	SetMenuTitleWithCFString(osx.vmenu, CFSTR("View"));
	AppendMenuItemTextWithCFString(osx.vmenu,
		CFSTR("Full Screen"), 0, CmdFullScreen, &ix);
	SetMenuItemCommandKey(osx.vmenu, ix, 0, 'F');
	AppendMenuItemTextWithCFString(osx.vmenu,
		CFSTR("Cmd-F exits full screen"),
		kMenuItemAttrDisabled, CmdFullScreen, &ix);
	InsertMenu(osx.vmenu, GetMenuID(osx.wmenu));
	DrawMenuBar();

	// Create the window.
	r = Rect(0, 0, Dx(osx.fullscreenr)*2/3, Dy(osx.fullscreenr)*2/3);
	havemin = 0;
	if(osx.winsize && osx.winsize[0]){
		if(parsewinsize(osx.winsize, &r, &havemin) < 0)
			sysfatal("%r");
	}
	if(!havemin)
		r = rectaddpt(r, Pt((Dx(osx.fullscreenr)-Dx(r))/2, (Dy(osx.fullscreenr)-Dy(r))/2));
	or.left = r.min.x;
	or.top = r.min.y;
	or.right = r.max.x;
	or.bottom = r.max.y;
	CreateNewWindow(kDocumentWindowClass, WindowAttrs, &or, &osx.window);
	setlabel(osx.label);
	seticon();
	
	// Set up the clip board.
	if(PasteboardCreate(kPasteboardClipboard, &osx.snarf) != noErr)
		panic("pasteboard create");

	// Explain in great detail which events we want to handle.
	// Why can't we just have one handler?
	const EventTypeSpec quits[] = {
		{ kEventClassApplication, kEventAppQuit }
	};
	const EventTypeSpec cmds[] = {
		{ kEventClassWindow, kEventWindowClosed },
		{ kEventClassWindow, kEventWindowBoundsChanged },
		{ kEventClassCommand, kEventCommandProcess },
		{ kEventClassWindow, kEventWindowActivated },
		{ kEventClassWindow, kEventWindowDeactivated },
	};
	const EventTypeSpec events[] = {
		{ kEventClassApplication, kEventAppShown },
		{ kEventClassKeyboard, kEventRawKeyDown },
		{ kEventClassKeyboard, kEventRawKeyModifiersChanged },
		{ kEventClassKeyboard, kEventRawKeyRepeat },
		{ kEventClassMouse, kEventMouseDown },
		{ kEventClassMouse, kEventMouseUp },
		{ kEventClassMouse, kEventMouseMoved },
		{ kEventClassMouse, kEventMouseDragged },
		{ kEventClassMouse, kEventMouseWheelMoved },
		{ 'P9PE', P9PEventLabelUpdate}
	};

	InstallApplicationEventHandler(
		NewEventHandlerUPP(quithandler),
		nelem(quits), quits, nil, nil);

 	InstallApplicationEventHandler(
 		NewEventHandlerUPP(eventhandler),
		nelem(events), events, nil, nil);

	InstallWindowEventHandler(osx.window,
		NewEventHandlerUPP(cmdhandler),
		nelem(cmds), cmds, osx.window, nil);

	// Finally, put the window on the screen.
	ShowWindow(osx.window);
	ShowMenuBar();
	eresized(0);
	SelectWindow(osx.window);
	
	InitCursor();
}

static Rendez scr;
static QLock slock;

void
screeninit(void)
{
	scr.l = &slock;
	qlock(scr.l);
	proccreate(screenproc, nil, 256*1024);
	while(osx.window == nil)
		rsleep(&scr);
	qunlock(scr.l);
}

static void
screenproc(void *v)
{
	qlock(scr.l);
	_screeninit();
	rwakeup(&scr);
	qunlock(scr.l);
	RunApplicationEventLoop();
}

static OSStatus kbdevent(EventRef);
static OSStatus mouseevent(EventRef);

static OSStatus
cmdhandler(EventHandlerCallRef next, EventRef event, void *arg)
{
	return eventhandler(next, event, arg);
}

static OSStatus
quithandler(EventHandlerCallRef next, EventRef event, void *arg)
{
	exit(0);
	return 0;
}

static OSStatus
eventhandler(EventHandlerCallRef next, EventRef event, void *arg)
{
	OSStatus result;

	result = CallNextEventHandler(next, event);

	switch(GetEventClass(event)){

	case 'P9PE':
		if (GetEventKind(event) == P9PEventLabelUpdate) {
			qlock(&osx.labellock);
			setlabel(osx.label);
			qunlock(&osx.labellock);
			return noErr;
		} else
			return eventNotHandledErr;

	case kEventClassApplication:;
		Rectangle r = Rect(0, 0, Dx(osx.screenr), Dy(osx.screenr));
		_flushmemscreen(r);
		return eventNotHandledErr;

	case kEventClassKeyboard:
		return kbdevent(event);
	
	case kEventClassMouse:
		return mouseevent(event);
	
	case kEventClassCommand:;
		HICommand cmd;
		GetEventParameter(event, kEventParamDirectObject,
			typeHICommand, nil, sizeof cmd, nil, &cmd);
		switch(cmd.commandID){
		case kHICommandQuit:
			exit(0);
		
		case CmdFullScreen:
			fullscreen(1);
			break;
		
		default:
			return eventNotHandledErr;
		}
		break;
	
	case kEventClassWindow:
		switch(GetEventKind(event)){
		case kEventWindowClosed:
			exit(0);
		
		case kEventWindowBoundsChanged:
			eresized(1);
			break;
		
		case kEventWindowActivated:
			activated(1);
			return eventNotHandledErr;
					
		case kEventWindowDeactivated:
			activated(0);
			return eventNotHandledErr;

		default:
			return eventNotHandledErr;
		}
		break;
	}
	
	return result;
}

static ulong
msec(void)
{
	return nsec()/1000000;
}

static OSStatus
mouseevent(EventRef event)
{
	int wheel;
	OSXPoint op;
	
	GetEventParameter(event, kEventParamMouseLocation,
		typeQDPoint, 0, sizeof op, 0, &op);

	osx.xy = subpt(Pt(op.h, op.v), osx.screenr.min);
	wheel = 0;

	switch(GetEventKind(event)){
	case kEventMouseWheelMoved:;
		SInt32 delta;
		GetEventParameter(event, kEventParamMouseWheelDelta,
			typeSInt32, 0, sizeof delta, 0, &delta);
		if(delta > 0)
			wheel = 8;
		else
			wheel = 16;
		break;
	
	case kEventMouseDown:
	case kEventMouseUp:;
		UInt32 but, mod;
		GetEventParameter(event, kEventParamMouseChord,
			typeUInt32, 0, sizeof but, 0, &but);
		GetEventParameter(event, kEventParamKeyModifiers,
			typeUInt32, 0, sizeof mod, 0, &mod);
		
		// OS X swaps button 2 and 3
		but = (but & ~6) | ((but & 4)>>1) | ((but&2)<<1);

		but = mouseswap(but);
		
		// Apply keyboard modifiers and pretend it was a real mouse button.
		// (Modifiers typed while holding the button go into kbuttons,
		// but this one does not.)
		if(but == 1){
			if(mod & optionKey) {
				// Take the ALT away from the keyboard handler.
				if(osx.kalting) {
					osx.kalting = 0;
					keystroke(Kalt);
				}
				but = 2;
			}
			else if(mod & cmdKey)
				but = 4;
		}
		osx.buttons = but;
		break;

	case kEventMouseMoved:
	case kEventMouseDragged:
		break;
	
	default:
		return eventNotHandledErr;
	}

	mousetrack(osx.xy.x, osx.xy.y, osx.buttons|osx.kbuttons|wheel, msec());
	return noErr;	
}

static int keycvt[] =
{
	[QZ_IBOOK_ENTER] '\n',
	[QZ_RETURN] '\n',
	[QZ_ESCAPE] 27,
	[QZ_BACKSPACE] '\b',
	[QZ_LALT] Kalt,
	[QZ_LCTRL] Kctl,
	[QZ_LSHIFT] Kshift,
	[QZ_F1] KF+1,
	[QZ_F2] KF+2,
	[QZ_F3] KF+3,
	[QZ_F4] KF+4,
	[QZ_F5] KF+5,
	[QZ_F6] KF+6,
	[QZ_F7] KF+7,
	[QZ_F8] KF+8,
	[QZ_F9] KF+9,
	[QZ_F10] KF+10,
	[QZ_F11] KF+11,
	[QZ_F12] KF+12,
	[QZ_INSERT] Kins,
	[QZ_DELETE] 0x7F,
	[QZ_HOME] Khome,
	[QZ_END] Kend,
	[QZ_KP_PLUS] '+',
	[QZ_KP_MINUS] '-',
	[QZ_TAB] '\t',
	[QZ_PAGEUP] Kpgup,
	[QZ_PAGEDOWN] Kpgdown,
	[QZ_UP] Kup,
	[QZ_DOWN] Kdown,
	[QZ_LEFT] Kleft,
	[QZ_RIGHT] Kright,
	[QZ_KP_MULTIPLY] '*',
	[QZ_KP_DIVIDE] '/',
	[QZ_KP_ENTER] '\n',
	[QZ_KP_PERIOD] '.',
	[QZ_KP0] '0',
	[QZ_KP1] '1',
	[QZ_KP2] '2',
	[QZ_KP3] '3',
	[QZ_KP4] '4',
	[QZ_KP5] '5',
	[QZ_KP6] '6',
	[QZ_KP7] '7',
	[QZ_KP8] '8',
	[QZ_KP9] '9',
};

static OSStatus
kbdevent(EventRef event)
{
	char ch;
	UInt32 code;
	UInt32 mod;
	int k;

	GetEventParameter(event, kEventParamKeyMacCharCodes,
		typeChar, nil, sizeof ch, nil, &ch);
	GetEventParameter(event, kEventParamKeyCode,
		typeUInt32, nil, sizeof code, nil, &code);
	GetEventParameter(event, kEventParamKeyModifiers,
		typeUInt32, nil, sizeof mod, nil, &mod);

	switch(GetEventKind(event)){
	case kEventRawKeyDown:
	case kEventRawKeyRepeat:
		osx.kalting = 0;
		if(mod == cmdKey){
			if(ch == 'F' || ch == 'f'){
				if(osx.isfullscreen && msec() - osx.fullscreentime > 500)
					fullscreen(0);
				return noErr;
			}
			
			// Pass most Cmd keys through as Kcmd + ch.
			// OS X interprets a few no matter what we do,
			// so it is useless to pass them through as keystrokes too.
			switch(ch) {
			case 'm':	// minimize window
			case 'h':	// hide window
			case 'H':	// hide others
			case 'q':	// quit
				return eventNotHandledErr;
			}
			if(' ' <= ch && ch <= '~') {
				keystroke(Kcmd + ch);
				return noErr;
			}
			return eventNotHandledErr;
		}
		k = ch;
		if(code < nelem(keycvt) && keycvt[code])
			k = keycvt[code];
		if(k >= 0)
			keystroke(k);
		else{
			UniChar uc;
			OSStatus s;

			s = GetEventParameter(event, kEventParamKeyUnicodes,
				typeUnicodeText, nil, sizeof uc, nil, &uc);
			if(s == noErr)
				keystroke(uc);
		}
		break;

	case kEventRawKeyModifiersChanged:
		if(!osx.buttons && !osx.kbuttons){
			if(mod == optionKey) {
				osx.kalting = 1;
				keystroke(Kalt);
			}
			break;
		}
		
		// If the mouse button is being held down, treat 
		// changes in the keyboard modifiers as changes
		// in the mouse buttons.
		osx.kbuttons = 0;
		if(mod & optionKey)
			osx.kbuttons |= 2;
		if(mod & cmdKey)
			osx.kbuttons |= 4;
		mousetrack(osx.xy.x, osx.xy.y, osx.buttons|osx.kbuttons, msec());
		break;
	}
	return noErr;
}

static void
eresized(int new)
{
	Memimage *m;
	OSXRect or;
	ulong chan;
	Rectangle r;
	int bpl;
	CGDataProviderRef provider;
	CGImageRef image;
	CGColorSpaceRef cspace;

	GetWindowBounds(osx.window, kWindowContentRgn, &or);
	r = Rect(or.left, or.top, or.right, or.bottom);
	if(Dx(r) == Dx(osx.screenr) && Dy(r) == Dy(osx.screenr)){
		// No need to make new image.
		osx.screenr = r;
		return;
	}

	chan = XBGR32;
	m = allocmemimage(Rect(0, 0, Dx(r), Dy(r)), chan);
	if(m == nil)
		panic("allocmemimage: %r");
	if(m->data == nil)
		panic("m->data == nil");
	bpl = bytesperline(r, 32);
	provider = CGDataProviderCreateWithData(0,
		m->data->bdata, Dy(r)*bpl, 0);
	//cspace = CGColorSpaceCreateWithName(kCGColorSpaceGenericRGB);
	cspace = CGColorSpaceCreateDeviceRGB();
	image = CGImageCreate(Dx(r), Dy(r), 8, 32, bpl,
		cspace,
		kCGImageAlphaNoneSkipLast,
		provider, 0, 0, kCGRenderingIntentDefault);
	CGColorSpaceRelease(cspace);
	CGDataProviderRelease(provider);	// CGImageCreate did incref
	
	mouserect = m->r;
	if(new){
		mouseresized = 1;
		mousetrack(osx.xy.x, osx.xy.y, osx.buttons|osx.kbuttons, msec());
	}
//	termreplacescreenimage(m);
	_drawreplacescreenimage(m);	// frees old osx.screenimage if any
	if(osx.image)
		CGImageRelease(osx.image);
	osx.image = image;
	osx.screenimage = m;
	osx.screenr = r;
	
	// I'm not 100% sure why this is necessary
	// but otherwise some resizes (esp. vertical ones)
	// stop updating the screen.
	qlock(&osx.flushlock);
	QDEndCGContext(GetWindowPort(osx.window), &osx.windowctx);
	osx.windowctx = nil;
	qunlock(&osx.flushlock);
}

void
flushproc(void *v)
{
	for(;;){
		if(osx.needflush && osx.windowctx && canqlock(&osx.flushlock)){
			if(osx.windowctx){
				CGContextFlush(osx.windowctx);
				osx.needflush = 0;
			}
			qunlock(&osx.flushlock);
		}
		usleep(33333);
	}
}

void
_flushmemscreen(Rectangle r)
{
	CGRect cgr;
	CGImageRef subimg;

	qlock(&osx.flushlock);
	if(osx.windowctx == nil){
		QDBeginCGContext(GetWindowPort(osx.window), &osx.windowctx);
		proccreate(flushproc, nil, 256*1024);
	}
	
	cgr.origin.x = r.min.x;
	cgr.origin.y = r.min.y;
	cgr.size.width = Dx(r);
	cgr.size.height = Dy(r);
	subimg = CGImageCreateWithImageInRect(osx.image, cgr);
	cgr.origin.y = Dy(osx.screenr) - r.max.y; // XXX how does this make any sense?
	CGContextDrawImage(osx.windowctx, cgr, subimg);
	osx.needflush = 1;
	qunlock(&osx.flushlock);
	CGImageRelease(subimg);
}

void
activated(int active)
{
	osx.active = active;
}

void
fullscreen(int wascmd)
{
	static OSXRect oldrect;
	GDHandle device;
	OSXRect dr;

	if(!wascmd)
		return;
	
	if(!osx.isfullscreen){
		GetWindowGreatestAreaDevice(osx.window,
			kWindowTitleBarRgn, &device, nil);
		dr = (*device)->gdRect;
		if(dr.top == 0 && dr.left == 0)
			HideMenuBar();
		GetWindowBounds(osx.window, kWindowContentRgn, &oldrect);
		ChangeWindowAttributes(osx.window,
			kWindowNoTitleBarAttribute,
			kWindowResizableAttribute);
		MoveWindow(osx.window, 0, 0, 1);
		MoveWindow(osx.window, dr.left, dr.top, 0);
		SizeWindow(osx.window,
			dr.right - dr.left,
			dr.bottom - dr.top, 0);
		osx.isfullscreen = 1;
	}else{
		ShowMenuBar();
		ChangeWindowAttributes(osx.window,
			kWindowResizableAttribute,
			kWindowNoTitleBarAttribute);
		SizeWindow(osx.window,
			oldrect.right - oldrect.left,
			oldrect.bottom - oldrect.top, 0);
		MoveWindow(osx.window, oldrect.left, oldrect.top, 0);
		osx.isfullscreen = 0;
	}
	eresized(1);
}
		
void
setmouse(Point p)
{
	CGPoint cgp;
	
	cgp.x = p.x + osx.screenr.min.x;
	cgp.y = p.y + osx.screenr.min.y;
	CGWarpMouseCursorPosition(cgp);
}

void
setcursor(Cursor *c)
{
	OSXCursor oc;
	int i;

	if(c == nil){
		InitCursor();
		return;
	}
	
	// SetCursor is deprecated, but what replaces it?
	for(i=0; i<16; i++){
		oc.data[i] = ((ushort*)c->set)[i];
		oc.mask[i] = oc.data[i] | ((ushort*)c->clr)[i];
	}
	oc.hotSpot.h = - c->offset.x;
	oc.hotSpot.v = - c->offset.y;
	SetCursor(&oc);
}

void
getcolor(ulong i, ulong *r, ulong *g, ulong *b)
{
	ulong v;
	
	v = 0;
	*r = (v>>16)&0xFF;
	*g = (v>>8)&0xFF;
	*b = v&0xFF;
}

int
setcolor(ulong i, ulong r, ulong g, ulong b)
{
	/* no-op */
	return 0;
}


int
hwdraw(Memdrawparam *p)
{
	return 0;
}

struct {
	QLock lk;
	char buf[SnarfSize];
	Rune rbuf[SnarfSize];
	PasteboardRef apple;
} clip;

char*
getsnarf(void)
{
	char *s;
	CFArrayRef flavors;
	CFDataRef data;
	CFIndex nflavor, ndata, j;
	CFStringRef type;
	ItemCount nitem;
	PasteboardItemID id;
	PasteboardSyncFlags flags;
	UInt32 i;
	u16int *u;
	Fmt fmt;
	Rune r;

/*	fprint(2, "applegetsnarf\n"); */
	qlock(&clip.lk);
	clip.apple = osx.snarf;
	if(clip.apple == nil){
		if(PasteboardCreate(kPasteboardClipboard, &clip.apple) != noErr){
			fprint(2, "apple pasteboard create failed\n");
			qunlock(&clip.lk);
			return nil;
		}
	}
	flags = PasteboardSynchronize(clip.apple);
	if(flags&kPasteboardClientIsOwner){
		s = strdup(clip.buf);
		qunlock(&clip.lk);
		return s;
	}
	if(PasteboardGetItemCount(clip.apple, &nitem) != noErr){
		fprint(2, "apple pasteboard get item count failed\n");
		qunlock(&clip.lk);
		return nil;
	}
	for(i=1; i<=nitem; i++){
		if(PasteboardGetItemIdentifier(clip.apple, i, &id) != noErr)
			continue;
		if(PasteboardCopyItemFlavors(clip.apple, id, &flavors) != noErr)
			continue;
		nflavor = CFArrayGetCount(flavors);
		for(j=0; j<nflavor; j++){
			type = (CFStringRef)CFArrayGetValueAtIndex(flavors, j);
			if(!UTTypeConformsTo(type, CFSTR("public.utf16-plain-text")))
				continue;
			if(PasteboardCopyItemFlavorData(clip.apple, id, type, &data) != noErr)
				continue;
			qunlock(&clip.lk);
			ndata = CFDataGetLength(data)/2;
			u = (u16int*)CFDataGetBytePtr(data);
			fmtstrinit(&fmt);
			// decode utf-16.  what was apple thinking?
			for(i=0; i<ndata; i++) {
				r = u[i];
				if(0xd800 <= r && r < 0xdc00 && i+1 < ndata && 0xdc00 <= u[i+1] && u[i+1] < 0xe000) {
					r = (((r - 0xd800)<<10) |  (u[i+1] - 0xdc00)) + 0x10000;
					i++;
				}
				else if(0xd800 <= r && r < 0xe000)
					r = Runeerror;
				if(r == '\r')
					r = '\n';
				fmtrune(&fmt, r);
			}
			CFRelease(flavors);
			CFRelease(data);
			return fmtstrflush(&fmt);
		}
		CFRelease(flavors);
	}
	qunlock(&clip.lk);
	return nil;		
}

void
putsnarf(char *s)
{
	CFDataRef cfdata;
	PasteboardSyncFlags flags;
	u16int *u, *p;
	Rune r;
	int i;

/*	fprint(2, "appleputsnarf\n"); */

	if(strlen(s) >= SnarfSize)
		return;
	qlock(&clip.lk);
	strcpy(clip.buf, s);
	runesnprint(clip.rbuf, nelem(clip.rbuf), "%s", s);
	clip.apple = osx.snarf;
	if(PasteboardClear(clip.apple) != noErr){
		fprint(2, "apple pasteboard clear failed\n");
		qunlock(&clip.lk);
		return;
	}
	flags = PasteboardSynchronize(clip.apple);
	if((flags&kPasteboardModified) || !(flags&kPasteboardClientIsOwner)){
		fprint(2, "apple pasteboard cannot assert ownership\n");
		qunlock(&clip.lk);
		return;
	}
	u = malloc(runestrlen(clip.rbuf)*4);
	p = u;
	for(i=0; clip.rbuf[i]; i++) {
		r = clip.rbuf[i];
		// convert to utf-16
		if(0xd800 <= r && r < 0xe000)
			r = Runeerror;
		if(r >= 0x10000) {
			r -= 0x10000;
			*p++ = 0xd800 + (r>>10);
			*p++ = 0xdc00 + (r & ((1<<10)-1));
		} else
			*p++ = r;
	}
	cfdata = CFDataCreate(kCFAllocatorDefault, 
		(uchar*)u, (p-u)*2);
	free(u);
	if(cfdata == nil){
		fprint(2, "apple pasteboard cfdatacreate failed\n");
		qunlock(&clip.lk);
		return;
	}
	if(PasteboardPutItemFlavor(clip.apple, (PasteboardItemID)1,
		CFSTR("public.utf16-plain-text"), cfdata, 0) != noErr){
		fprint(2, "apple pasteboard putitem failed\n");
		CFRelease(cfdata);
		qunlock(&clip.lk);
		return;
	}
	CFRelease(cfdata);
	qunlock(&clip.lk);
}

void
setlabel(char *label)
{
	CFStringRef cs;

	cs = CFStringCreateWithBytes(nil, (uchar*)label, strlen(label), kCFStringEncodingUTF8, false);
	SetWindowTitleWithCFString(osx.window, cs);
	CFRelease(cs);
}

void
kicklabel(char *label)
{
	char *p;
	EventRef e;

	p = strdup(label);
	if(p == nil)
		return;
	qlock(&osx.labellock);
	free(osx.label);
	osx.label = p;
	qunlock(&osx.labellock);
	
	CreateEvent(nil, 'P9PE', P9PEventLabelUpdate, 0, kEventAttributeUserEvent, &e);
	PostEventToQueue(GetMainEventQueue(), e, kEventPriorityStandard);
	
}

static void
seticon(void)
{
	CGImageRef im;
	CGDataProviderRef d;

	d = CGDataProviderCreateWithData(nil, glenda_png, sizeof glenda_png, nil);
	im = CGImageCreateWithPNGDataProvider(d, nil, true, kCGRenderingIntentDefault);
	if(im)
		SetApplicationDockTileImage(im);
	CGImageRelease(im);
	CGDataProviderRelease(d);
}

