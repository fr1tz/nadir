#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <cursor.h>
#include <mouse.h>
#include <keyboard.h>
#include <frame.h>
#include <fcall.h>
#include <plumb.h>
#include "dat.h"
#include "fns.h"

Text* latestselectiontext = nil;
int   latestselectionid = 0;

static char* selectionprocarg_context = 0;
static char* selectionprocarg_text = 0;
static int   selectionprocarg_ntext = 0;

void
getxselarg(Text* t)
{
	char* av[] = { "xsel2arg", 0 };
	int pfd[2], sfd[3], pid, n, nb, nr, nulls;
	char buf[1024];
	Rune *r;

	textdelete(t, 0, t->file->b.nc, TRUE);

	if(pipe(pfd) < 0)
	{
		warning(nil, "getxselarg: can't create pipe");
		return;
	}
	fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
	fcntl(pfd[1], F_SETFD, FD_CLOEXEC);

	sfd[0] = open("/dev/null", OREAD);
	sfd[1] = pfd[0];
	sfd[2] = dup(erroutfd, -1);

	pid = threadspawn(sfd, av[0], av);
	if(pid == -1)
	{
		warning(nil, "can't spawn thread %s: %r\n", av[0]);
		return;
	}	

	while(0 < (n = read(pfd[1],  buf, 1024)))
	{
		r = runemalloc(n+1);
		cvttorunes(buf, n, r, &nb, &nr, &nulls);
		textinsert(t, 0, r, nr, TRUE);
		free(r);
	}

	textsetselect(t, 0, t->file->b.nc);
}

void
putxsel(Text* t)
{
	char* av[] = { "setguisel", 0, 0 };
	int pfd[2], sfd[3], pid, n, m, q, q0, q1;
	Rune *r;
	char *s;

	q0 = t->q0;

	q1 = t->q1;
	if(q0 == q1)
		return;

	if(pipe(pfd) < 0)
	{
		warning(nil, "putxsel: can't create pipe");
		return;
	}
	fcntl(pfd[0], F_SETFD, FD_CLOEXEC);
	fcntl(pfd[1], F_SETFD, FD_CLOEXEC);

	sfd[0] = pfd[0];
	sfd[1] = open("/dev/null", OWRITE);
	sfd[2] = dup(erroutfd, -1);

	if(t->file->name) 
		av[1] = runetobyte(t->file->name, t->file->nname);

	pid = threadspawn(sfd, av[0], av);

	free(av[1]);

	if(pid == -1)
	{
		warning(nil, "can't spawn thread %s: %r\n", av[0]);
		return;
	}	

	r = fbufalloc();
	s = fbufalloc();
	for(q=q0; q<q1; q+=n)
	{
		n = q1 - q;
		if(n > BUFSIZE/UTFmax)
			n = BUFSIZE/UTFmax;
		bufread(&t->file->b, q, r, n);
		m = snprint(s, BUFSIZE+1, "%.*S", n, r);
		if(write(pfd[1], s, m) != m)
		{
			warning(nil, "error writing to setguisel: %r\n");
			break;
		}
	}
	fbuffree(r);
	fbuffree(s);

	close(pfd[1]);

	newsel(t);
}

void
selectionproc(void* v)
{
	SelectionChange sc;
	sc.selectionID = latestselectionid;

	char* gui = getenv("gui");
	char* fn = 0;
	int newtextfd = -1;
	int fd = -1;

	fn = smprint("%s/sel/new", gui);
	fd = open(fn, OWRITE);
	if(fd < 0)
	{
		warning(nil, "error opening %s: %r\n", fn);
		goto cleanup;
	}
	char* context = selectionprocarg_context;
	int ncontext = strlen(context);
	if(write(fd, context, ncontext) != ncontext)
	{
		warning(nil, "error writing to %s: %r\n", fn);
		goto cleanup;
	}
	close(fd); fd = -1;

	char textid[5];
	newtextfd = open(fn, OREAD);
	if(newtextfd < 0)
	{
		warning(nil, "error opening %s: %r\n", fn);
		goto cleanup;
	}
	if(read(newtextfd, textid, 4) != 4)
	{
		warning(nil, "error reading id from %s: %r\n", fn);
		goto cleanup;
	}
	textid[4] = '\0';
	free(fn);

	fn = smprint("%s/sel/%s-text", gui, textid);
	fd = open(fn, OREAD);
	if(fd < 0)
	{
		warning(nil, "error opening %s for reading: %r\n", fn);
		goto cleanup;
	}	
	char* text;
	int ntext;
	text = malloc(ncontext);
	if(readn(fd, text, ncontext) != ncontext)
	{
		warning(nil, "error reading from %s: %r\n", fn);
		goto cleanup;
	}
	if(strncmp(context, text, ncontext) != 0)
	{
		/* another text was loaded into the selection 
		   server between our write to sel/new and
		   our opening of sel/$textid-text */
		goto cleanup;
	}
	free(text);
	close(fd); fd = -1;

	fd = open(fn, OWRITE);
	if(fd < 0)
	{
		warning(nil, "error opening %s for writing: %r\n", fn);
		goto cleanup;
	}	
	text = selectionprocarg_text;
	ntext = selectionprocarg_ntext;
	if(write(fd, text, ntext) != ntext)
	{
		warning(nil, "error writing to %s: %r\n", fn);
		goto cleanup;
	}
	close(fd); fd = -1;

	seek(newtextfd, 0, 0);
	while(read(newtextfd, textid, 4) == 4)
	{
		free(fn);
		fn = smprint("%s/sel/%s-text", gui, textid);
		fd = open(fn, OREAD);
		if(fd < 0)
			continue;	

		sc.ndata = 0;
		send(cselchange, &sc);	

		while((sc.ndata = read(fd, sc.data, sizeof(sc.data))) > 0)
			send(cselchange, &sc);		

		seek(newtextfd, 0, 0);
	}

 cleanup:
	close(fd);
	close(newtextfd);
	free(gui);
	free(fn);
}



void
newsel(Text* t)
{
	int n, m, q, q0, q1;
	Rune *r;
	char *s;

	q0 = t->q0;

	q1 = t->q1;
	if(q0 == q1)
		return;

	latestselectionid++;

	free(selectionprocarg_context);
	if(t->file->name) 
		selectionprocarg_context = runetobyte(t->file->name, t->file->nname);
	else
		selectionprocarg_context = strdup(getsrvname());

	char* text = 0; 
	int ntext = 0;

	r = fbufalloc();
	s = fbufalloc();
	for(q=q0; q<q1; q+=n)
	{
		n = q1 - q;
		if(n > BUFSIZE/UTFmax)
			n = BUFSIZE/UTFmax;
		bufread(&t->file->b, q, r, n);
		m = snprint(s, BUFSIZE+1, "%.*S", n, r);
		text = realloc(text, ntext + m);
		memcpy(text + ntext, s, m);
		ntext += m;
	}
	fbuffree(r);
	fbuffree(s);

	free(selectionprocarg_text);
	selectionprocarg_text = text;
	selectionprocarg_ntext = ntext;

	latestselectiontext = t;

	proccreate(selectionproc, nil, STACK);
}
