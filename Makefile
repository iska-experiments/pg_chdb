# Named, not globbed: the control files of the other modules live here too.
EXTENSION    = chdb
# The default_version a control file names, so a module's versioned script
# always matches its control file.
ctl_version  = $(shell grep -m 1 'default_version' $(1).control | \
               sed -e "s/[[:space:]]*default_version[[:space:]]*=[[:space:]]*'\([^']*\)',\{0,1\}/\1/")
EXTVERSION   = $(call ctl_version,$(EXTENSION))
DISTVERSION  = $(shell grep -m 1 '^[[:space:]]\{2\}"version":' META.json | \
               sed -e 's/[[:space:]]*"version":[[:space:]]*"\([^"]*\)",\{0,1\}/\1/')

MAX_CONCURRENT_TESTS ?=

DATA         = $(sort $(wildcard sql/$(EXTENSION)--*.sql) sql/$(EXTENSION)--$(EXTVERSION).sql)
DOCS         = $(wildcard doc/*.md)
# The stub worker client (CHDB_SEARCH_STUB=1) has no store, so the tests of
# the worker and of searches end to end run only with the worker; the
# search_stub tests, whose rows the stub's GUCs supply, run only with the stub.
TESTS        ?= $(if $(CHDB_SEARCH_STUB),$(filter-out test/sql/search_worker.sql \
                test/sql/search_e2e.sql,$(wildcard test/sql/*.sql)), \
                $(filter-out test/sql/search_stub%.sql,$(wildcard test/sql/*.sql)))
REGRESS      = --schedule test/schedule$(MAX_CONCURRENT_TESTS)
# UTF8: the search predicates lowercase by Unicode, and the tests say so in
# their literals, whatever the cluster's locale (CI also runs them with
# NO_LOCALE=1, in a database with the C locale).
REGRESS_OPTS = --inputdir=test --load-extension=$(EXTENSION) --encoding=UTF8 $(if $(MAX_CONCURRENT_TESTS),--max-concurrent-tests $(MAX_CONCURRENT_TESTS))
MODULE_big   = $(EXTENSION)
PG_CONFIG   ?= pg_config
TAP_TESTS   ?= 1
OBJS         = $(subst .c,.o, $(wildcard src/*.c))

# The programs linking libchdb, each built by a sub-make beside its sources.
HELPER       = src/helper/chdb_helper
ENGINE       = src/search/engine/chdb_search_engine

# One jobserver sized to the machine's processors reaches every sub-make, so a
# bare make builds with every core; a -j on the command line still wins.
NPROC       ?= $(shell nproc --all 2>/dev/null || sysctl -n hw.ncpu)
MAKEFLAGS   += -j$(NPROC)

# Determine the OS and architecture.
OS         ?= $(shell uname -s | tr A-Z a-z)
ARCH        = $(shell uname -m)
ifeq ($(ARCH),aarch64)
  ARCH       := arm64
else ifeq ($(ARCH),x86_64)
  ARCH       := amd64
endif

CLANG_FORMAT ?= clang-format

# Binary dependency on specific version (for now) of libchdb. Optionally
# download locally by setting BUNDLE_LIBCHDB and compile statically with
# LIBCHDB_BUILD=static.
LIBCHDB_VERSION ?= v26.9.0
LIBCHDB_BUILD   ?= dynamic

# Header-only dependencies, vendored as submodules. clickhouse-c comes from
# pg-clickhouse-c's own pin, its signatures naming clickhouse-c types, so a
# second checkout on the include path would silently win.
PGCH_DIR     = $(CURDIR)/vendor/pg-clickhouse-c
CH_C_DIR     = $(PGCH_DIR)/clickhouse-c

# Suppress annoying pre-c99 warning, error on 	/other warnings.
PG_CFLAGS    = -Wno-declaration-after-statement -Wall -Werror

# -isystem keeps the vendored headers' warnings out of the -Werror build.
# PGCH_MSG_PREFIX prefixes messages pg-clickhouse-c raises like our own.
# clickhouse-c copies what it raises through chc_err.msg, 256 bytes by default,
# which clips the longer type names out of a decoding error.
PG_CPPFLAGS  = -isystem $(CH_C_DIR) -isystem $(PGCH_DIR) -DPGCH_MSG_PREFIX='"chdb: "' \
               -DCHC_ERR_MSG_LEN=4096

# Clean up generated files.
EXTRA_CLEAN  = src/version.h sql/$(EXTENSION)--$(EXTVERSION).sql src/hook/chdb_hook$(DLSUFFIX) src/hook/*.o src/hook/*.bc src/search/client.mode test/schedule*

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Set default prove flags.
ifeq ($(PROVE_FLAGS),)
PROVE_FLAGS = -fwvj $(if $(MAX_CONCURRENT_TESTS),$(MAX_CONCURRENT_TESTS),$(NPROC))
endif

# Build against, install, uninstall a local copy of libchdb.
ifneq ($(BUNDLE_LIBCHDB),)
LIBCHDB_DIR = vendor/libchdb-$(LIBCHDB_VERSION)-$(OS)-$(ARCH)
$(HELPER) $(ENGINE): $(LIBCHDB_DIR)/lib/libchdb.$(if $(filter $(LIBCHDB_BUILD),static),a,so)
ifneq ($(LIBCHDB_BUILD),static)
install: install-libchdb
uninstall: uninstall-libchdb
endif
endif

SEARCH_VERSION := $(shell sed -n "s/^default_version *= *'\(.*\)'/\1/p" chdb_search.control)
SEARCH_MODULE  := src/search/chdb_search$(DLSUFFIX)

# Require the versioned SQL script.
all: sql/$(EXTENSION)--$(EXTVERSION).sql src/hook/chdb_hook$(DLSUFFIX)

# PGXS tracks no header dependencies, and the vendored libraries are all header.
# *.bc compiles same sources, so needs same headers.
$(OBJS) $(OBJS:.o=.bc): $(CH_C_DIR)/clickhouse.h src/version.h \
                        $(wildcard src/*.h $(PGCH_DIR)/*.h $(CH_C_DIR)/*.h)

# Versioned SQL script.
sql/$(EXTENSION)--$(EXTVERSION).sql: sql/$(EXTENSION).sql
	cp $< $@

# Versioned source file.
src/version.h: META.json
	@printf '#define PGCHCB_VERSION "%s"\n' "$(DISTVERSION)" > $@

# Hook module.
HOOK_MODULE := src/hook/chdb_hook$(DLSUFFIX)
$(HOOK_MODULE): $(wildcard src/hook/*.c src/hook/*.h) $(OBJS)
	@$(MAKE) -C $(dir $@) all CH_C_DIR=$(CH_C_DIR) PGCH_DIR=$(PGCH_DIR) NO_FILE_SCHEME=$(NO_FILE_SCHEME)

# Install and uninstall the chdb_hook module.
install-hook: $(HOOK_MODULE)
	$(INSTALL_SHLIB) $< '$(DESTDIR)$(pkglibdir)/'
uninstall-hook:
	rm -f $(DESTDIR)$(pkglibdir)/$(HOOK_MODULE)
install: install-hook
uninstall: uninstall-hook

# An extension module of its own: built by a sub-make under src/<name>/ and
# installed here like chdb_hook, with its control file and versioned script.
# Eval it below this point, once PGXS has set DLSUFFIX:
#   $(eval $(call ext_module,chdb_<name>,<extra prerequisites>,<sub-make arguments>))
define ext_module
$(1)_VERSION := $$(call ctl_version,$(1))
$(1)_SO := src/$(patsubst chdb_%,%,$(1))/$(1)$$(DLSUFFIX)
$$($(1)_SO): $$(wildcard $$(dir $$($(1)_SO))*.c $$(dir $$($(1)_SO))*.h) $(2)
	@$$(MAKE) -C $$(dir $$@) all $(3)
sql/$(1)--$$($(1)_VERSION).sql: sql/$(1).sql
	cp $$< $$@
install-$(patsubst chdb_%,%,$(1)): $$($(1)_SO) sql/$(1)--$$($(1)_VERSION).sql
	$$(INSTALL_SHLIB) $$< '$$(DESTDIR)$$(pkglibdir)/'
	$$(MKDIR_P) '$$(DESTDIR)$$(datadir)/extension'
	$$(INSTALL_DATA) $(1).control sql/$(1)--$$($(1)_VERSION).sql '$$(DESTDIR)$$(datadir)/extension/'
uninstall-$(patsubst chdb_%,%,$(1)):
	rm -f $$(DESTDIR)$$(pkglibdir)/$(1)$$(DLSUFFIX)
	rm -f $$(DESTDIR)$$(datadir)/extension/$(1).control $$(DESTDIR)$$(datadir)/extension/$(1)--$$($(1)_VERSION).sql
all: $$($(1)_SO) sql/$(1)--$$($(1)_VERSION).sql
install: install-$(patsubst chdb_%,%,$(1))
uninstall: uninstall-$(patsubst chdb_%,%,$(1))
EXTRA_CLEAN += sql/$(1)--$$($(1)_VERSION).sql $$($(1)_SO) $$(dir $$($(1)_SO))*.o $$(dir $$($(1)_SO))*.bc
endef

# Fail with something more useful than a missing include.
$(CH_C_DIR)/clickhouse.h: .gitmodules
	git submodule update --init --recursive

# A program linking libchdb: built by a sub-make beside its sources, installed
# into pkglibdir beside the library that starts it. Write beside the live copy
# and rename over it: install unlinks its target first, so a COPY starting in
# that moment finds no helper. rename leaves no such gap.
#   $(eval $(call libchdb_program,<short name>,<path>,<extra prerequisites>))
define libchdb_program
$(2): $$(wildcard $$(dir $(2))*.c $$(dir $(2))*.h) $(3)
	@$$(MAKE) -C $$(dir $$@) all LIBCHDB_DIR=$$(LIBCHDB_DIR) LIBCHDB_BUILD=$$(LIBCHDB_BUILD)
install-$(1): $(2)
	@to=$$(DESTDIR)$$(pkglibdir)/$$(notdir $(2)); \
	  $$(INSTALL_PROGRAM) $$< $$$$to.new && mv -f $$$$to.new $$$$to
uninstall-$(1):
	rm -f $$(DESTDIR)$$(pkglibdir)/$$(notdir $(2))
all: $(2)
install: install-$(1)
uninstall: uninstall-$(1)
EXTRA_CLEAN += $(2) $$(dir $(2))*.o
endef

# chdb_helper answers one COPY.
$(eval $(call libchdb_program,helper,$(HELPER),src/setup.h))

# The chdb_search extension: the search worker, its clients and the chdb index
# access method, with the chdb_search_engine the worker forks to run libchdb,
# so that a libchdb crash never takes the worker, and with it the cluster,
# down. Pass CHDB_SEARCH_STUB=1 to link the per-backend fake in
# src/search/client_stub.c instead of the worker client, for building and
# testing the access method without a worker. Both need PostgreSQL 17 or
# later, as the control file says; an older server builds and tests the chdb
# extension alone, and its TAP tests skip the search ones.
ifeq ($(shell test $(VERSION_NUM) -ge 170000 && echo yes),yes)
$(eval $(call ext_module,chdb_search,$(OBJS) src/search/client.mode,CH_C_DIR=$(CH_C_DIR) PGCH_DIR=$(PGCH_DIR) CHDB_SEARCH_STUB=$(CHDB_SEARCH_STUB)))
$(eval $(call libchdb_program,engine,$(ENGINE),src/search/protocol.h src/setup.h))
else
TESTS := $(filter-out test/sql/search_%,$(TESTS))
PROVE_TESTS := $(filter-out t/search_%,$(wildcard t/*.pl))
endif

# Which client the module was linked with, rewritten only when that changes,
# so switching CHDB_SEARCH_STUB relinks it.
src/search/client.mode: FORCE
	@echo '$(CHDB_SEARCH_STUB)' | cmp -s - $@ || echo '$(CHDB_SEARCH_STUB)' > $@
.PHONY: FORCE
FORCE:

# The search tests share the database's worker, extension and table names, so
# they run one at a time after the others.
.PHONY: test/schedule$(MAX_CONCURRENT_TESTS)
test/schedule$(MAX_CONCURRENT_TESTS): schedule = $(patsubst test/sql/%.sql,%,$(filter-out test/sql/search_%,$(TESTS)))
test/schedule$(MAX_CONCURRENT_TESTS): serial = $(patsubst test/sql/%.sql,%,$(filter test/sql/search_%,$(TESTS)))
test/schedule$(MAX_CONCURRENT_TESTS):
ifneq ($(MAX_CONCURRENT_TESTS),)
	@perl -E 'say "test: ", join " ", splice @ARGV, 0, $(MAX_CONCURRENT_TESTS) while @ARGV' $(schedule) > $@
else
	@echo $(if $(schedule),test: $(schedule),) > $@
endif
	@$(if $(serial),printf 'test: %s\n' $(serial) >> $@,:)

installcheck: test/schedule$(MAX_CONCURRENT_TESTS)

# libchdb
$(LIBCHDB_DIR)/lib/libchdb.so:
	@env INSTALL_VERSION="$(LIBCHDB_VERSION)" DESTDIR=$(LIBCHDB_DIR) bash vendor/get-libchdb.sh

$(LIBCHDB_DIR)/lib/libchdb.a:
	env INSTALL_VERSION="$(LIBCHDB_VERSION)" DESTDIR=$(LIBCHDB_DIR) STATIC=1 bash vendor/get-libchdb.sh

# GitHub stuff.
libchdb-version:
	@echo $(LIBCHDB_VERSION)
libchdb-variables:
	@echo VERSION=$(LIBCHDB_VERSION)
	@echo DIRECTORY=$(LIBCHDB_DIR)

# Install and uninstall libchdb, which is configured to live in /usr/local/lib.
install-libchdb: $(LIBCHDB_DIR)/lib/libchdb.so
	$(MKDIR_P) $(DESTDIR)/usr/local/lib
	$(INSTALL_SHLIB) $< $(DESTDIR)/usr/local/lib
	if [ "$$(uname -s)" = "Linux" ]; then ldconfig; fi

uninstall-libchdb:
	rm -f $(DESTDIR)/usr/local/lib/libchdb.so

.PHONY: format # Format .c and .h files to project standard in .clang-format.
format: $(wildcard src/*.c src/*.h src/helper/*.c src/search/*.c src/search/*.h src/search/engine/*.c src/search/engine/*.h)
	@$(CLANG_FORMAT) --style=file:.clang-format -i $^

.PHONY: type-table # Regenerate the data type tables of doc/chdb_hook.md.
type-table:
	@(cd $(PGCH_DIR) && ./gen_type_table.awk) | dev/type_table.awk doc/chdb_hook.md
	@(cd $(PGCH_DIR) && ./gen_type_table.awk -v section=ENCODE) | \
		dev/type_table.awk -v section=ENCODE doc/chdb_hook.md

.PHONY: clang-tidy # Run clang-tidy static analysis (requires compile_commands.json)
clang-tidy: compile_commands.json
	run-clang-tidy -p . $(wildcard src/*.c src/*.h src/helper/*.c)

.PHONY: lint # Lint the project
lint: .pre-commit-config.yaml
	@pre-commit run --show-diff-on-failure --color=always --all-files

## .git/hooks/pre-commit: Install the pre-commit hook
.git/hooks/pre-commit:
	@printf "#!/bin/sh\nmake lint\n" > $@
	@chmod +x $@

# Requires https://github.com/rizsotto/Bear.
compile_commands.json:
	$(MAKE) clean -j $$(nproc)
	bear --config "dev/bear.$$(if [ "$$(bear --version | awk -F'[^0-9]+' '{ print $$2 }')" -eq 3 ]; then echo 'json'; else echo 'yml'; fi)" -- $(MAKE) all -j $$(nproc)

debian-install-lint:
	@curl -SsLo /tmp/pre-commit.pyz https://github.com/pre-commit/pre-commit/releases/download/v4.6.0/pre-commit-4.6.0.pyz
	@printf "#!/bin/sh\npython3 /tmp/pre-commit.pyz \"\$$@\"\n" > /usr/local/bin/pre-commit
	@chmod +x /usr/local/bin/pre-commit

# Test the PGXN distribution.
dist-test: $(EXTENSION)-$(DISTVERSION).zip
	unzip $(EXTENSION)-$(DISTVERSION).zip
	cd $(EXTENSION)-$(DISTVERSION)
	$(MAKE) && $(DIST_TEST_SUDO) $(MAKE) install && $(MAKE) installcheck

.PHONY: release-notes # Show release notes for current version (must have `mknotes` in PATH).
release-notes: CHANGELOG.md
	mknotes -v v$(DISTVERSION) -f $< -r https://github.com/$(or $(GITHUB_REPOSITORY),ClickHouse/pg_chdb)

$(EXTENSION)-$(DISTVERSION).zip:
	git archive-all -v --prefix "$(EXTENSION)-$(DISTVERSION)/" --force-submodules $(EXTENSION)-$(DISTVERSION).zip

zip: $(EXTENSION)-$(DISTVERSION).zip

kv-rest:
	curl -Ls https://github.com/theory/kv-rest/releases/download/v0.1.1/kv-rest-v0.1.1-$(OS)-$(ARCH).tar.gz | tar zxf - --strip-components=1 $(if $(filter $(OS),linux),--wildcards) '*/kv-rest'

start-kv-rest: kv-rest
	KVREST_PORT="$${KVREST_PORT:-9182}" ./kv-rest &

# Run make print-VARIABLE_NAME to print VARIABLE_NAME's flavor and value.
print-%	: ; $(info $* is $(flavor $*) variable set to "$($*)") @true
