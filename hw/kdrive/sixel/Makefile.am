AM_CPPFLAGS = \
	@KDRIVE_PURE_INCS@ \
	@KDRIVE_CFLAGS@ \
	@XSIXEL_INCS@

AM_CFLAGS = -DHAVE_DIX_CONFIG_H

bin_PROGRAMS = Xkitty

# Provide a local main() wrapper like Xephyr to avoid --as-needed issues
Xkitty_SOURCES = kitty.c kittyinit.c

# Use the canonical KDRIVE_LIBS ordering from configure to satisfy
# static link dependencies on Debian (with --as-needed).
Xkitty_LDADD = \
	@KDRIVE_LIBS@ \
	@XSIXEL_LIBS@ \
	@KDRIVE_MAIN_LIB@ \
	$(XSERVER_SYS_LIBS)

Xkitty_DEPENDENCIES = @KDRIVE_LOCAL_LIBS@

Xkitty_LDFLAGS = $(LD_EXPORT_SYMBOLS_FLAG)

relink:
	rm -f $(bin_PROGRAMS) && make $(bin_PROGRAMS)
