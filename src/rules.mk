# The Makefile templates of pg_chdb's modules and programs, installed with
# its headers for the extensions that build against it. Include this file
# before PGXS; eval the templates after it, once PGXS has set DLSUFFIX.

# The default_version a control file names, so a module's versioned script
# always matches its control file.
ctl_version  = $(shell grep -m 1 'default_version' $(1).control | \
               sed -e "s/[[:space:]]*default_version[[:space:]]*=[[:space:]]*'\([^']*\)',\{0,1\}/\1/")

# An extension module of its own: built by a sub-make under src/<name>/ and
# installed like chdb_hook, with its control file and versioned script, which
# is sql/<name>.sql with any further parts appended in the order given.
#   $(eval $(call ext_module,chdb_<name>,<extra prerequisites>,<sub-make arguments>,<further script parts>))
define ext_module
$(1)_VERSION := $$(call ctl_version,$(1))
$(1)_SO := src/$(patsubst chdb_%,%,$(1))/$(1)$$(DLSUFFIX)
$$($(1)_SO): $$(wildcard $$(dir $$($(1)_SO))*.c $$(dir $$($(1)_SO))*.h $$(dir $$($(1)_SO))*/*.c $$(dir $$($(1)_SO))*/*.h) $(2)
	@$$(MAKE) -C $$(dir $$@) all $(3)
sql/$(1)--$$($(1)_VERSION).sql: sql/$(1).sql $(4)
	cat $$^ > $$@
install-$(patsubst chdb_%,%,$(1)): $$($(1)_SO) sql/$(1)--$$($(1)_VERSION).sql
	$$(MKDIR_P) '$$(DESTDIR)$$(pkglibdir)'
	$$(INSTALL_SHLIB) $$< '$$(DESTDIR)$$(pkglibdir)/'
	$$(MKDIR_P) '$$(DESTDIR)$$(datadir)/extension'
	$$(INSTALL_DATA) $(1).control sql/$(1)--$$($(1)_VERSION).sql '$$(DESTDIR)$$(datadir)/extension/'
uninstall-$(patsubst chdb_%,%,$(1)):
	rm -f $$(DESTDIR)$$(pkglibdir)/$(1)$$(DLSUFFIX)
	rm -f $$(DESTDIR)$$(datadir)/extension/$(1).control $$(DESTDIR)$$(datadir)/extension/$(1)--$$($(1)_VERSION).sql
all: $$($(1)_SO) sql/$(1)--$$($(1)_VERSION).sql
install: install-$(patsubst chdb_%,%,$(1))
uninstall: uninstall-$(patsubst chdb_%,%,$(1))
EXTRA_CLEAN += sql/$(1)--$$($(1)_VERSION).sql $$($(1)_SO) $$(dir $$($(1)_SO))*.o $$(dir $$($(1)_SO))*.bc $$(dir $$($(1)_SO))*/*.o $$(dir $$($(1)_SO))*/*.bc
endef

# A program linking libchdb: built by a sub-make beside its sources against
# libchdb.mk and the libchdb tree named, LIBCHDB_DIR unless another is given,
# installed into pkglibdir beside the library that starts it. Write beside
# the live copy and rename over it: install unlinks its target first, so a
# COPY starting in that moment finds no helper. rename leaves no such gap.
#   $(eval $(call libchdb_program,<short name>,<path>,<extra prerequisites>[,<libchdb dir>]))
define libchdb_program
$(2): $$(wildcard $$(dir $(2))*.c $$(dir $(2))*.h) $(3)
	@$$(MAKE) -C $$(dir $$@) all LIBCHDB_DIR=$(or $(4),$$(LIBCHDB_DIR)) LIBCHDB_BUILD=$$(LIBCHDB_BUILD)
install-$(1): $(2)
	$$(MKDIR_P) '$$(DESTDIR)$$(pkglibdir)'
	@to=$$(DESTDIR)$$(pkglibdir)/$$(notdir $(2)); \
	  $$(INSTALL_PROGRAM) $$< $$$$to.new && mv -f $$$$to.new $$$$to
uninstall-$(1):
	rm -f $$(DESTDIR)$$(pkglibdir)/$$(notdir $(2))
all: $(2)
install: install-$(1)
uninstall: uninstall-$(1)
EXTRA_CLEAN += $(2) $$(dir $(2))*.o
endef
