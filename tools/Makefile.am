include $(top_srcdir)/build/autotools/Makefile.am.silent

AM_CPPFLAGS = \
	-I$(top_srcdir) \
	-I$(top_srcdir)/rut \
	-I$(top_builddir)/rut
	-DG_DISABLE_SINGLE_INCLUDES \
	-DCOGL_DISABLE_DEPRECATED

AM_CFLAGS = \
	$(RIG_DEP_CFLAGS) \
	$(RIG_EXTRA_CFLAGS) \
	-DRIG_EXAMPLES_DATA=\""$(abs_top_srcdir)/examples/"\" \
	-DRIG_SHARE_DIR=\"$(datadir)/\"

common_ldadd = \
	$(RIG_DEP_LIBS)

bin_PROGRAMS = rig-bump-map-gen

rig_bump_map_gen_SOURCES = bump-map-gen.c
rig_bump_map_gen_LDADD = $(common_ldadd)

noinst_PROGRAMS =

if HAVE_LIBCRYPTO
noinst_PROGRAMS += rig-check-signature
endif

rig_check_signature_SOURCES = \
	check-signature.c
rig_check_signature_LDADD = \
	$(GLIB_LIBS) \
	$(LIBCRYPTO_LIBS)
rig_check_signatures_CFLAGS = \
	$(GLIB_CFLAGS) \
	$(LIBCRYPTO_CFLAGS)
