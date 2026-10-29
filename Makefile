PREFIX ?= /usr/local
CFLAGS ?= -O2 -Wall -Wno-deprecated-declarations

moji: moji.c ucdata.h
	$(CC) $(CFLAGS) -o $@ $< $(shell pkg-config --cflags --libs gtk4) -lm

install: moji
	install -Dm755 moji $(DESTDIR)$(PREFIX)/bin/moji
	install -Dm644 moji.desktop $(DESTDIR)$(PREFIX)/share/applications/moji.desktop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/moji $(DESTDIR)$(PREFIX)/share/applications/moji.desktop

clean:
	rm -f moji

.PHONY: install uninstall clean
