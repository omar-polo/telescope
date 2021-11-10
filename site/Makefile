.PHONY: all serve-www serve-gemini upload

all: telescope.1.html telescope.1.txt

telescope.1.html: ../telescope.1
	./mdoc2html.sh $? $@

telescope.1.txt: ../telescope.1
	MANWIDTH=72 man -Tutf8 -l $? | col -b > $@

serve-www:
	python3 -m http.server 8888

serve-gemini:
	gmid -p 1966 .

upload:
	rsync --delete -a . op:sites/telescope.omarpolo.com
	rsync --delete -a . op:gemini/telescope.omarpolo.com