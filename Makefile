XSS_DIR = xscreensaver-5.35/
CFLAGS = -pedantic -Wall -std=c11 -U__STRICT_ANSI__ -I../../utils -I../ -I../../ -DSTANDALONE -DUSE_GL -DHAVE_CONFIG_H -g -I$(XSS_DIR)hacks/glx -I$(XSS_DIR) -I$(XSS_DIR)utils -I$(XSS_DIR)hacks `curl-config --cflags` `freetype-config --cflags`
LDFLAGS = -lGL -lGLU -lpthread -lXft -lXt -lX11 -lXmu -lm `curl-config --libs`

src = src/wip24.c src/json.c src/stb_image.c $(XSS_DIR)hacks/fps.c \
	  $(XSS_DIR)hacks/glx/fps-gl.c $(XSS_DIR)utils/resources.c \
	  $(XSS_DIR)utils/visual.c $(XSS_DIR)utils/visual-gl.c \
	  $(XSS_DIR)utils/usleep.c $(XSS_DIR)utils/yarandom.c \
	  $(XSS_DIR)utils/hsv.c $(XSS_DIR)utils/colors.c \
	  $(XSS_DIR)utils/async_netdb.c $(XSS_DIR)utils/aligned_malloc.c \
	  $(XSS_DIR)utils/thread_util.c $(XSS_DIR)utils/utf8wc.c \
	  $(XSS_DIR)hacks/screenhack.c $(XSS_DIR)hacks/xlockmore.c \
	  $(XSS_DIR)hacks/glx/xlock-gl-utils.c $(XSS_DIR)hacks/glx/texfont.c
base_obj = $(src:.c=.o)
obj = $(join $(dir $(base_obj)), $(addprefix ., $(notdir $(base_obj))))
dep = $(obj:.o=.d)

.PHONY: build
build:
	make $(XSS_DIR)README
	make wip24

wip24: $(obj)
	$(CC) $(obj) $(LDFLAGS) -o wip24

-include $(dep)

.%.d: %.c $(XSS_DIR)README $(XSS_DIR)config.h 
	@$(CPP) $(CFLAGS) $< -MM -MT $(@:.d=.o) >$@

.%.o: %.c $(XSS_DIR)README $(XSS_DIR)config.h 
	$(CC) -c $(CFLAGS) $< -o $@

$(XSS_DIR)config.h:
	cd $(XSS_DIR); ./configure $(XXS_CONFIG)

$(XSS_DIR)README:
	curl https://www.jwz.org/xscreensaver/xscreensaver-5.35.tar.gz > .xscreensaver.tar.gz
	tar -xzf .xscreensaver.tar.gz
	rm .xscreensaver.tar.gz

.PHONY: clean
clean:
	rm -f $(dep) $(obj) wip24
	rm -f -r xscreensaver-5.34

.PHONY: install
install:
	cp wip24 /usr/libexec/xscreensaver/wip24
	cp src/wip24.xml /usr/share/xscreensaver/config/wip24.xml
	cp src/wip24.conf /usr/share/xscreensaver/hacks.conf.d/wip24.conf
	update-xscreensaver-hacks

.PHONY: uninstall
uninstall:
	rm -f /usr/libexec/xscreensaver/wip24
	rm -f /usr/share/xscreensaver/config/wip24.xml
	rm -f /usr/share/xscreensaver/hacks.conf.d/wip24.conf
	update-xscreensaver-hacks

.PHONY: full-uninstall
full-uninstall: uninstall
	rm -r ~/.wip24
