include Makefile.sources
include $(top_srcdir)/src/gallium/Automake.inc

AM_CFLAGS = \
	-Wno-packed-bitfield-compat \
	-I$(top_srcdir)/src/gallium/drivers/freedreno/ir3 \
	-I$(top_builddir)/src/compiler/nir \
	-I$(top_srcdir)/src/compiler/nir \
	$(GALLIUM_DRIVER_CFLAGS) \
	$(FREEDRENO_CFLAGS)

ir3/ir3_nir_trig.c: ir3/ir3_nir_trig.py $(top_srcdir)/src/compiler/nir/nir_algebraic.py
	$(MKDIR_GEN)
	$(AM_V_GEN) PYTHONPATH=$(top_srcdir)/src/compiler/nir $(PYTHON2) $(PYTHON_FLAGS) $(srcdir)/ir3/ir3_nir_trig.py > $@ || ($(RM) $@; false)

noinst_LTLIBRARIES = libfreedreno.la

libfreedreno_la_SOURCES = \
	$(C_SOURCES) \
	$(a2xx_SOURCES) \
	$(a3xx_SOURCES) \
	$(a4xx_SOURCES) \
	$(ir3_SOURCES) \
	$(ir3_GENERATED_FILES)

BUILT_SOURCES := $(ir3_GENERATED_FILES)
CLEANFILES := $(BUILT_SOURCES)

noinst_PROGRAMS = ir3_compiler

# XXX: Required due to the C++ sources in libnir
nodist_EXTRA_ir3_compiler_SOURCES = dummy.cpp
ir3_compiler_SOURCES = \
	ir3/ir3_cmdline.c

ir3_compiler_LDADD = \
	libfreedreno.la \
	$(top_builddir)/src/gallium/auxiliary/libgallium.la \
	$(top_builddir)/src/compiler/nir/libnir.la \
	$(top_builddir)/src/util/libmesautil.la \
	$(GALLIUM_COMMON_LIB_DEPS) \
	$(FREEDRENO_LIBS)
