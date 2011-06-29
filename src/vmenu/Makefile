# vmenu 
# See LICENSE file for copyright and license details.

include config.mk

SRC = vmenu.c
OBJ = ${SRC:.c=.o}

all: options vmenu

options:
	@echo vmenu build options:
	@echo "CFLAGS   = ${CFLAGS}"
	@echo "LDFLAGS  = ${LDFLAGS}"
	@echo "CC       = ${CC}"

.c.o:
	@echo CC $<
	@${CC} -c ${CFLAGS} $<

${OBJ}: config.mk

vmenu: ${OBJ}
	@echo CC -o $@
	@${CC} -o $@ ${OBJ} ${LDFLAGS}

clean:
	@echo cleaning
	@rm -f vmenu ${OBJ} vmenu-${VERSION}.tar.gz

dist: clean
	@echo creating dist tarball
	@mkdir -p vmenu-${VERSION}
	@cp -R LICENSE Makefile README config.mk vmenu.1 vmenu_path vmenu_run ${SRC} vmenu-${VERSION}
	@tar -cf vmenu-${VERSION}.tar vmenu-${VERSION}
	@gzip vmenu-${VERSION}.tar
	@rm -rf vmenu-${VERSION}

install: all
	@echo installing executable file to ${DESTDIR}${PREFIX}/bin
	@mkdir -p ${DESTDIR}${PREFIX}/bin
	@cp -f vmenu ${DESTDIR}${PREFIX}/bin
	@chmod 755 ${DESTDIR}${PREFIX}/bin/vmenu
	@echo installing manual page to ${DESTDIR}${MANPREFIX}/man1
	@mkdir -p ${DESTDIR}${MANPREFIX}/man1
	@sed "s/VERSION/${VERSION}/g" < vmenu.1 > ${DESTDIR}${MANPREFIX}/man1/vmenu.1
	@chmod 644 ${DESTDIR}${MANPREFIX}/man1/vmenu.1

uninstall:
	@echo removing executable file from ${DESTDIR}${PREFIX}/bin
	@echo removing manual page from ${DESTDIR}${MANPREFIX}/man1
	@rm -f ${DESTDIR}${MANPREFIX}/man1/vmenu.1

.PHONY: all options clean dist install uninstall
