CC	= gcc
CFLAGS	= -g -W -Wall -Werror -Wno-unused
V	= @
LIBS	= -lpthread -lcrypto -lssl

# Uncomment the following line to run on Solaris machines.
#LIBS	+= -lsocket -lnsl -lresolv

all: osppeer

%.o: %.c
	@echo + cc $<
	$(V)$(CC) $(CPPFLAGS) $(CFLAGS) -c $<

run: osppeer
	@-/bin/rm -rf test
	@echo + mkdir test
	@mkdir test
	@echo + ./osppeer -dtest -t131.179.80.139:11111 cat1.jpg cat2.jpg cat3.jpg
	@./osppeer -dtest -t131.179.80.139:11111 cat1.jpg cat2.jpg cat3.jpg

run-good: osppeer
	@-/bin/rm -rf test
	@echo + mkdir test
	@mkdir test
	@echo + ./osppeer -dtest -t164.67.100.231:12998 cat1.jpg cat2.jpg cat3.jpg
	@./osppeer -dtest -t164.67.100.231:12998 cat1.jpg cat2.jpg cat3.jpg

run-slow: osppeer
	@-/bin/rm -rf test
	@echo + mkdir test
	@mkdir test
	@echo + ./osppeer -dtest -t164.67.100.231:12999 cat1.jpg cat2.jpg cat3.jpg
	@./osppeer -dtest -t164.67.100.231:12999 cat1.jpg cat2.jpg cat3.jpg

run-bad: osppeer
	@-/bin/rm -rf test
	@echo + mkdir test
	@mkdir test
	@echo + ./osppeer -b -dtest -t164.67.100.231:12995 cat1.jpg cat2.jpg cat3.jpg
	@./osppeer -b -dtest -t164.67.100.231:12995 cat1.jpg cat2.jpg cat3.jpg

run-popular: osppeer
	@-/bin/rm -rf test
	@echo + mkdir test
	@mkdir test
	@echo + ./osppeer -dtest -t164.67.100.231:13000 cat1.jpg cat2.jpg cat3.jpg
	@./osppeer -dtest -t164.67.100.231:13000 cat1.jpg cat2.jpg cat3.jpg

run-parallel: osppeer
	@echo cleaning up...
	@./kill-seeders.sh
	@-/bin/rm hostdir/osppeer
	@-/bin/rm *.idx
	@-/bin/rm rc-*
	@-/bin/rm *.part_*
	@cp osppeer hostdir/
	@echo + Starting seeders
	@./pd-test.sh
	@echo + Wait for seeder init...
	@sleep 2
	@echo + ./osppeer -p testfile.txt
	@./osppeer -p testfile.txt

clean:
	@-rm -f *.o *~ osptracker osptracker.cc osppeer hostdir/osppeer *.idx rc-* *.part_*
	@./kill-seeders.sh

distclean: clean

DISTDIR := lab4-$(USER)

tarballdir-nocheck: clean always
	@echo + mk $(DISTDIR)
	$(V)/bin/rm -rf $(DISTDIR)
	$(V)mkdir $(DISTDIR)
	$(V)tar cf - `ls | grep -v '^$(DISTDIR)\|^test\|^lab4\|\.tar\.gz$$\|\.tgz$$\|\.qcow2$$\|~$$'` | (cd $(DISTDIR) && tar xf -)
	$(V)/bin/rm -rf `find $(DISTDIR) -name CVS -o -name .svn -print`
	$(V)date > $(DISTDIR)/tarballstamp

tarballdir: tarballdir-nocheck
	$(V)/bin/bash ./check-lab.sh $(DISTDIR) || (rm -rf $(DISTDIR); false)

tarball: tarballdir
	@echo + mk $(DISTDIR).tar.gz
	$(V)tar cf $(DISTDIR).tar $(DISTDIR)
	$(V)gzip $(DISTDIR).tar
	$(V)/bin/rm -rf $(DISTDIR)

osppeer: osppeer.o md5.o writescan.o reconstruct.o
	@echo + ld osppeer
	$(V)$(CC) $(CFLAGS) -o $@ osppeer.o md5.o writescan.o reconstruct.o $(LIBS)


.PHONY: all always clean distclean tarball tarballdir-nocheck tarballdir \
	dist distdir install run run-good run-slow run-bad
