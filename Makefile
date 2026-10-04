# Named, not globbed: chdb_search.control belongs to the search module below.
EXTENSION    = chdb
EXTVERSION   = $(shell grep -m 1 'default_version' chdb.control | \
               sed -e "s/[[:space:]]*default_version[[:space:]]*=[[:space:]]*'\([^']*\)',\{0,1\}/\1/")
DISTVERSION  = $(shell grep -m 1 '^[[:space:]]\{2\}"version":' META.json | \
               sed -e 's/[[:space:]]*"version":[[:space:]]*"\([^"]*\)",\{0,1\}/\1/')

MAX_CONCURRENT_TESTS ?=

DATA         = $(sort $(wildcard sql/$(EXTENSION)--*.sql) sql/$(EXTENSION)--$(EXTVERSION).sql)
DOCS         = $(wildcard doc/*.md)
# The stub worker client (CHDB_SEARCH_STUB=1) answers nothing, so of the search
# tests only search_am, which checks the generated statements, runs against it.
TESTS        ?= $(if $(CHDB_SEARCH_STUB),$(filter-out test/sql/search_worker.sql \
                test/sql/search_e2e.sql,$(wildcard test/sql/*.sql)),$(wildcard test/sql/*.sql))
REGRESS      = --schedule test/schedule$(MAX_CONCURRENT_TESTS)
REGRESS_OPTS = --inputdir=test --load-extension=$(EXTENSION) $(if $(MAX_CONCURRENT_TESTS),--max-concurrent-tests $(MAX_CONCURRENT_TESTS))
MODULE_big   = $(EXTENSION)
PG_CONFIG   ?= pg_config
TAP_TESTS   ?= 1
OBJS         = $(subst .c,.o, $(wildcard src/*.c))

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
EXTRA_CLEAN  = src/version.h sql/$(EXTENSION)--$(EXTVERSION).sql src/hook/chdb_hook$(DLSUFFIX) src/hook/*.o src/hook/*.bc src/helper/chdb_helper src/helper/*.o src/search/chdb_search$(DLSUFFIX) src/search/*.o src/search/*.bc src/search/client.mode sql/chdb_search--*.sql test/schedule*

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Set default prove flags.
ifeq ($(PROVE_FLAGS),)
PROVE_FLAGS = -fwvj $(if $(MAX_CONCURRENT_TESTS),$(MAX_CONCURRENT_TESTS),$(shell nproc))
endif

# Build against, install, uninstall a local copy of libchdb.
ifneq ($(BUNDLE_LIBCHDB),)
LIBCHDB_DIR = vendor/libchdb-$(LIBCHDB_VERSION)-$(OS)-$(ARCH)
src/helper/chdb_helper: $(LIBCHDB_DIR)/lib/libchdb.$(if $(filter $(LIBCHDB_BUILD),static),a,so)
ifneq ($(LIBCHDB_BUILD),static)
install: install-libchdb
uninstall: uninstall-libchdb
endif
endif

SEARCH_VERSION := $(shell sed -n "s/^default_version *= *'\(.*\)'/\1/p" chdb_search.control)
SEARCH_MODULE  := src/search/chdb_search$(DLSUFFIX)

# Require the versioned SQL script.
all: sql/$(EXTENSION)--$(EXTVERSION).sql src/helper/chdb_helper src/hook/chdb_hook$(DLSUFFIX) $(SEARCH_MODULE) sql/chdb_search--$(SEARCH_VERSION).sql

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
	@$(MAKE) -C $(dir $@) all -j $$(nproc) CH_C_DIR=$(CH_C_DIR) PGCH_DIR=$(PGCH_DIR) NO_FILE_SCHEME=$(NO_FILE_SCHEME)

# Install and uninstall the chdb_hook module.
install-hook: $(HOOK_MODULE)
	$(INSTALL_SHLIB) $< '$(DESTDIR)$(pkglibdir)/'
uninstall-hook:
	rm -f $(DESTDIR)$(pkglibdir)/$(HOOK_MODULE)
install: install-hook
uninstall: uninstall-hook

# Search module: the chdb_search extension, its worker and the chdb index
# access method. It has its own control file and script, so it installs
# through a sub-make of its own. Pass CHDB_SEARCH_STUB=1 to link the
# per-backend fake in src/search/client_stub.c instead of the worker client,
# for building and testing the access method without a worker.
$(SEARCH_MODULE): $(wildcard src/search/*.c src/search/*.h) $(OBJS) src/version.h src/search/client.mode
	@$(MAKE) -C $(dir $@) all CH_C_DIR=$(CH_C_DIR) PGCH_DIR=$(PGCH_DIR) LIBCHDB_DIR=$(LIBCHDB_DIR) CHDB_SEARCH_STUB=$(CHDB_SEARCH_STUB)

# Which client the module was linked with, rewritten only when that changes,
# so switching CHDB_SEARCH_STUB relinks it.
src/search/client.mode: FORCE
	@echo '$(CHDB_SEARCH_STUB)' | cmp -s - $@ || echo '$(CHDB_SEARCH_STUB)' > $@
.PHONY: FORCE
FORCE:

sql/chdb_search--$(SEARCH_VERSION).sql: sql/chdb_search.sql chdb_search.control
	cp $< $@

install-search: $(SEARCH_MODULE) sql/chdb_search--$(SEARCH_VERSION).sql
	$(INSTALL_SHLIB) $< '$(DESTDIR)$(pkglibdir)/'
	$(MKDIR_P) '$(DESTDIR)$(datadir)/extension'
	$(INSTALL_DATA) chdb_search.control sql/chdb_search--$(SEARCH_VERSION).sql '$(DESTDIR)$(datadir)/extension/'
uninstall-search:
	rm -f $(DESTDIR)$(pkglibdir)/chdb_search$(DLSUFFIX)
	rm -f $(DESTDIR)$(datadir)/extension/chdb_search.control $(DESTDIR)$(datadir)/extension/chdb_search--$(SEARCH_VERSION).sql
install: install-search
uninstall: uninstall-search

# Fail with something more useful than a missing include.
$(CH_C_DIR)/clickhouse.h: .gitmodules
	git submodule update --init --recursive

# The only program linking libchdb, kept beside the library that starts it.
src/helper/chdb_helper: $(wildcard src/helper/*.c) src/setup.h
	@$(MAKE) -C $(dir $@) all LIBCHDB_DIR=$(LIBCHDB_DIR) LIBCHDB_BUILD=$(LIBCHDB_BUILD)

# Install the helper. Write beside the live copy and rename over it: install
# unlinks its target first, so a COPY starting in that moment finds no helper.
# rename leaves no such gap.
install-helper: src/helper/chdb_helper
	@to=$(DESTDIR)$(pkglibdir)/chdb_helper; \
	  $(INSTALL_PROGRAM) $< $$to.new && mv -f $$to.new $$to
uninstall-helper:
	rm -f $(DESTDIR)$(pkglibdir)/chdb_helper
install: install-helper
uninstall: uninstall-helper

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
format: $(wildcard src/*.c src/*.h src/helper/*.c src/search/*.c src/search/*.h)
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
