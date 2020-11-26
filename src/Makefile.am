bin_PROGRAMS = jio
jio_SOURCES = \
	bootid.c \
	bootid.h \
	humane.c \
	humane.h \
	jio.c \
	journals.c \
	journals.h \
	machid.c \
	machid.h \
	op.h \
	readfile.c \
	readfile.h \
	reclaim-tail-waste.c \
	reclaim-tail-waste.h \
	report-tail-waste.c \
	report-tail-waste.h \
	report-usage.c \
	report-usage.h

jio_CPPFLAGS = -I@top_srcdir@/thunk_h -I@top_srcdir@/libiou/src
jio_LDADD = @top_builddir@/libiou/src/libiou.a
