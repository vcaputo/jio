SUBDIRS = upstream
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
	report-entry-arrays.c \
	report-entry-arrays.h \
	report-layout.c \
	report-layout.h \
	report-tail-waste.c \
	report-tail-waste.h \
	report-usage.c \
	report-usage.h \
	verify-hashed-objects.c \
	verify-hashed-objects.h

jio_CPPFLAGS = -I@top_srcdir@/thunk_h -I@top_srcdir@/libiou/src
jio_LDADD = @top_builddir@/libiou/src/libiou.a @top_builddir@/src/upstream/libupstream.a
