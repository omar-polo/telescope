RSYNC =		openrsync --rsync-path=openrsync

MANPAGES =	../telescope.1

PAGES =		index.gmi \
		gallery.gmi
#		changelog.gmi

TITLE_index.gmi =	Home
TITLE_changelog.gmi =	ChangeLog
TITLE_gallery.gmi =	Gallery

REPOLOGY_BANNER =	https://repology.org/badge/vertical-allrepos/telescope.svg
REPOLOGY_URL =		https://repology.org/project/telescope/versions

SUBST =	./subst CURRV=0.9 CURRKEY=0.9 NEXTKEY=0.10 \
	CURRSIGN=RWSmshjFM8Q0IDYUd4I/8G2q9Y0DUaZLac4enOtQh5rzsIcVF+ePXCy7 \
	REPOLOGY_BANNER=${REPOLOGY_BANNER} \
	REPOLOGY_URL=${REPOLOGY_URL}

SUBST_GEM =	${SUBST} MANEXT=txt  EXT=gmi
SUBST_WWW =	${SUBST} MANEXT=html EXT=html

.PHONY: all dirs manpages serve-www serve-gemini upload clean titles

all: dirs manpages pages
	cp logo.png gemini/
	cp mandoc.css style.css www/
	cp logo.png www/
	cp -R gallery gemini
	cp -R gallery www
	convert logo.png -resize 200x200 www/logo.small.png

dirs:
	mkdir -p gemini www

manpages:
.for m in ${MANPAGES}
	man -O style=mandoc.css -Thtml -l $m > www/${m:T}.html
	man -O width=64 -Tutf8 -l $m | col -b > gemini/${m:T}.txt
.endfor

pages:
.for p in ${PAGES}
	grep -v '^<' $p | ${SUBST_GEM} > gemini/$p

	${SUBST_WWW} TITLE=${TITLE_${p}:Q} header.html > www/$p
	${MAKE} titles | ./menu.pl "${p:.gmi=.html}" >> www/$p
	${SUBST_WWW} $p | ./gem2html >> www/$p
	cat footer.html >> www/$p
	cd www && mv $p ${p:.gmi=.html}
.endfor

serve-www:
	python3 -m http.server --directory www 8888

serve-gemini:
	ge -p 1966 gemini

upload:
	${RSYNC} --del -a gemini/ antartica:/var/gemini/telescope.omarpolo.com
	${RSYNC} --del -a www/ antartica:/var/www/telescope.omarpolo.com

clean:
	rm -rf gemini www

titles:
.for p in ${PAGES}
	@printf "%s %s\n" "${p:.gmi=.html}" ${TITLE_${p}:Q}
.endfor
